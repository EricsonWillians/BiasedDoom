//
//---------------------------------------------------------------------------
//
// Copyright(C) 2025 BiasedDoom Team
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** model_gltf.cpp
**
** glTF 2.0 model loading and processing
**
**/

#include "model_gltf.h"

// Only compile glTF support if enabled
#ifdef NEODOOM_GLTF_SUPPORT

#include "cmdlib.h"
#include "image.h"
#include "base64.h"
#include "stb_image.h"

// Image source for glTF images embedded in the file itself (GLB buffer
// views, data URIs): decoded eagerly into an RGBA buffer, since the
// lump-backed image classes would try to re-read a nonexistent lump
// at upload time.
class FGLTFMemoryImage : public FImageSource {
  TArray<uint8_t> pixels; // BGRA, as FBitmap expects

public:
  FGLTFMemoryImage(const uint8_t *rgba, int w, int h)
      : FImageSource(-1) // -1 = no lump, procedurally generated
  {
    Width = w;
    Height = h;
    pixels.Resize(w * h * 4);

    bool translucent = false;
    for (int i = 0; i < w * h; ++i) {
      pixels[i * 4 + 0] = rgba[i * 4 + 2]; // B
      pixels[i * 4 + 1] = rgba[i * 4 + 1]; // G
      pixels[i * 4 + 2] = rgba[i * 4 + 0]; // R
      pixels[i * 4 + 3] = rgba[i * 4 + 3]; // A
      if (rgba[i * 4 + 3] < 255)
        translucent = true;
    }

    bUseGamePalette = false;
    bMasked = translucent;
    bTranslucent = 0;
  }

  PalettedPixels CreatePalettedPixels(int conversion,
                                      int frame = 0) override {
    // Software fallback path only; not used by the hardware renderer.
    PalettedPixels out(Width * Height);
    memset(out.Data(), 0, Width * Height);
    return out;
  }

  int CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override {
    bmp->Create(Width, Height);
    memcpy(bmp->GetPixels(), pixels.Data(), Width * Height * 4);
    return 0;
  }
};
#include "dobject.h"
#include "engineerrors.h"
#include "filesystem.h"
#include "hw_bonebuffer.h"
#include "i_time.h"
#include "m_alloc.h"
#include "m_swap.h"
#include "modelrenderer.h"
#include "printf.h"
#include "texturemanager.h"
#include "v_video.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/util.hpp>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>

namespace {
constexpr float GLTFAnimationSampleRate = 30.0f;
// Upper bound on sampled frames per clip: guards against absurd or
// non-finite durations from corrupt keyframe data blowing up the
// sampled-frame allocation.
constexpr int GLTFMaxSampledFrames = 30 * 60 * 60; // one hour at 30 fps

static TRS MakeIdentityTRS() {
  TRS transform;
  transform.translation = FVector3(0.0f, 0.0f, 0.0f);
  transform.rotation = FQuaternion(0.0f, 0.0f, 0.0f, 1.0f);
  transform.scaling = FVector3(1.0f, 1.0f, 1.0f);
  return transform;
}

static TRS TRSFromFastgltfTRS(const fastgltf::Node::TRS &src) {
  TRS transform = MakeIdentityTRS();
  transform.translation =
      FVector3(src.translation[0], src.translation[1], src.translation[2]);
  transform.rotation = FQuaternion(src.rotation[0], src.rotation[1],
                                   src.rotation[2], src.rotation[3]);
  transform.scaling = FVector3(src.scale[0], src.scale[1], src.scale[2]);
  return transform;
}

static VSMatrix BuildMatrixFromTRS(const TRS &transform) {
  VSMatrix mat;
  mat.loadIdentity();
  mat.translate(transform.translation.X, transform.translation.Y,
                transform.translation.Z);
  mat.multQuaternion(transform.rotation);
  const float sx = transform.scaling.X != 0.0f ? transform.scaling.X : 1.0f;
  const float sy = transform.scaling.Y != 0.0f ? transform.scaling.Y : 1.0f;
  const float sz = transform.scaling.Z != 0.0f ? transform.scaling.Z : 1.0f;
  mat.scale(sx, sy, sz);
  return mat;
}

static int GetSampledFrameCount(float duration) {
  if (!std::isfinite(duration) || duration <= 0.0f) {
    return 1;
  }
  const double frames =
      std::ceil(static_cast<double>(duration) * GLTFAnimationSampleRate) +
      1.0;
  if (frames >= GLTFMaxSampledFrames) {
    return GLTFMaxSampledFrames;
  }
  return std::max(1, static_cast<int>(frames));
}

static TRS InterpolateGLTFBone(const TRS &from, const TRS &to, float t) {
  const float invt = 1.0f - t;
  TRS bone;
  bone.translation = from.translation * invt + to.translation * t;
  bone.rotation = InterpolateQuat(from.rotation, to.rotation, t, invt);
  bone.scaling = from.scaling * invt + to.scaling * t;
  return bone;
}

static const char *
ToInterpolationString(fastgltf::AnimationInterpolation interpolation) {
  switch (interpolation) {
  case fastgltf::AnimationInterpolation::Linear:
    return "LINEAR";
  case fastgltf::AnimationInterpolation::Step:
    return "STEP";
  case fastgltf::AnimationInterpolation::CubicSpline:
    return "CUBICSPLINE";
  default:
    return "LINEAR";
  }
}

static const char *ToAnimationPathString(fastgltf::AnimationPath path) {
  switch (path) {
  case fastgltf::AnimationPath::Translation:
    return "translation";
  case fastgltf::AnimationPath::Rotation:
    return "rotation";
  case fastgltf::AnimationPath::Scale:
    return "scale";
  case fastgltf::AnimationPath::Weights:
    return "weights";
  default:
    return "translation";
  }
}
} // namespace

//===========================================================================
//
// Global glTF Detection Functions
//
//===========================================================================

//===========================================================================
//
// GLTFLoadResult Implementation
//
//===========================================================================

void GLTFLoadResult::SetError(GLTFError err, const char *msg, int line) {
  error = err;
  errorMessage = msg;
  errorLine = line;
}

void GLTFLoadResult::Clear() {
  error = GLTFError::None;
  errorMessage.Truncate(0);
  errorLine = -1;
}

//===========================================================================
//
// Enhanced Detection Functions with Error Reporting
//
//===========================================================================

bool IsGLTFFile(const char *buffer, int length, GLTFLoadResult *result) {
  if (!buffer || length < 4) {
    if (result) {
      result->SetError(GLTFError::InvalidFormat, "Buffer too small or null");
    }
    return false;
  }

  // Check for JSON glTF magic
  if (length >= 20) {
    const char *str = buffer;

    // Look for opening brace
    if (str[0] != '{') {
      if (result) {
        result->SetError(GLTFError::InvalidFormat, "Not a JSON file");
      }
      return false;
    }

    // Look for required glTF fields
    const char *asset_field = strstr(str, "\"asset\"");
    const char *version_field = strstr(str, "\"version\"");

    if (asset_field && version_field) {
      return true;
    } else {
      if (result) {
        result->SetError(GLTFError::MissingRequiredData,
                         "Missing asset or version field");
      }
    }
  } else {
    if (result) {
      result->SetError(GLTFError::InvalidFormat,
                       "File too small for glTF JSON");
    }
  }

  return false;
}

bool IsGLBFile(const char *buffer, int length, GLTFLoadResult *result) {
  if (!buffer || length < 12) {
    if (result) {
      result->SetError(GLTFError::InvalidFormat,
                       "Buffer too small for GLB header");
    }
    return false;
  }

  // Check GLB magic number: "glTF" + version + length
  const uint32_t *header = reinterpret_cast<const uint32_t *>(buffer);

  if (header[0] != 0x46546C67) { // "glTF" in little-endian
    if (result) {
      result->SetError(GLTFError::InvalidFormat, "Invalid GLB magic number");
    }
    return false;
  }

  // Check version
  uint32_t version = LittleLong(header[1]);
  if (version != 2) {
    if (result) {
      result->SetError(GLTFError::UnsupportedVersion,
                       "Unsupported GLB version");
    }
    return false;
  }

  // Validate file length
  uint32_t fileLength = LittleLong(header[2]);
  if (fileLength > static_cast<uint32_t>(length)) {
    if (result) {
      result->SetError(GLTFError::CorruptedBuffer, "GLB file length mismatch");
    }
    return false;
  }

  return true;
}

bool ValidateGLTFBuffer(const char *buffer, int length,
                        GLTFLoadResult &result) {
  result.Clear();

  if (!buffer) {
    result.SetError(GLTFError::InvalidFormat, "Null buffer");
    return false;
  }

  if (length <= 0) {
    result.SetError(GLTFError::InvalidFormat, "Invalid buffer length");
    return false;
  }

  // Basic JSON structure validation
  int braceCount = 0;
  bool inString = false;
  bool escaped = false;

  for (int i = 0; i < length; ++i) {
    char c = buffer[i];

    if (escaped) {
      escaped = false;
      continue;
    }

    if (c == '\\') {
      escaped = true;
      continue;
    }

    if (c == '"') {
      inString = !inString;
      continue;
    }

    if (!inString) {
      if (c == '{') {
        braceCount++;
      } else if (c == '}') {
        braceCount--;
        if (braceCount < 0) {
          result.SetError(GLTFError::InvalidFormat, "Unmatched closing brace",
                          i);
          return false;
        }
      }
    }
  }

  if (braceCount != 0) {
    result.SetError(GLTFError::InvalidFormat, "Unmatched braces in JSON");
    return false;
  }

  return true;
}

bool ValidateGLBHeader(const char *buffer, int length, GLTFLoadResult &result) {
  result.Clear();

  if (!IsGLBFile(buffer, length, &result)) {
    return false;
  }

  if (length < 20) { // Need at least header + first chunk header
    result.SetError(GLTFError::InvalidFormat, "GLB file too small");
    return false;
  }

  const uint32_t *header = reinterpret_cast<const uint32_t *>(buffer);
  uint32_t fileLength = LittleLong(header[2]);

  // Validate first chunk (JSON)
  uint32_t chunkLength = LittleLong(header[3]);
  uint32_t chunkType = LittleLong(header[4]);

  if (chunkType != 0x4E4F534A) { // "JSON" in ASCII
    result.SetError(GLTFError::InvalidFormat, "First chunk must be JSON");
    return false;
  }

  if (12 + 8 + chunkLength > fileLength) {
    result.SetError(GLTFError::CorruptedBuffer,
                    "JSON chunk size exceeds file length");
    return false;
  }

  return true;
}

//===========================================================================
//
// FGLTFModel Implementation
//
//===========================================================================

FGLTFModel::FGLTFModel()
    : currentAnimationIndex(-1), lastAnimationTime(0.0), hasSkinning(false),
      maxBonesPerVertex(4), isValid(false), framesSinceLoad(0),
      totalLoadTime(0.0), memoryUsage(0) {
  // Set default load options
  loadOptions.validateOnLoad = true;
  loadOptions.generateMissingNormals = true;
  loadOptions.generateMissingTangents = true;
  loadOptions.optimizeMeshes = true;
  loadOptions.preloadTextures = false;
}

FGLTFModel::~FGLTFModel() { CleanupResources(); }

bool FGLTFModel::Load(const char *path, int lumpnum, const char *buffer,
                      int length) {
  DPrintf(DMSG_NOTIFY, "FGLTFModel::Load path=%s length=%d\n",
          path ? path : "(null)", length);
  return LoadWithOptions(path, lumpnum, buffer, length, loadOptions);
}

bool FGLTFModel::LoadWithOptions(const char *path, int lumpnum,
                                 const char *buffer, int length,
                                 const GLTFLoadOptions &options) {
  auto startTime = std::chrono::high_resolution_clock::now();

  mLumpNum = lumpnum;
  loadOptions = options;
  lastError.Clear();
  isValid = false;

  // Store base path for loading external resources (e.g., .bin files)
  if (path) {
    basePath = path;
    // Extract directory path (everything before the last /)
    auto lastSlash = basePath.LastIndexOf('/');
    if (lastSlash >= 0) {
      basePath.Truncate(lastSlash + 1); // Keep the trailing slash
    } else {
      basePath = "";
    }
  }

  try {
    DPrintf(DMSG_NOTIFY, "Loading glTF model: %s (size: %d bytes)\n", path,
            length);

    // Validate input parameters
    if (!path || !buffer || length <= 0) {
      lastError.SetError(GLTFError::InvalidFormat, "Invalid input parameters");
      PrintErrorDetails(lastError);
      return false;
    }

    // Check memory limits before loading
    if (!CheckMemoryLimits(lastError)) {
      PrintErrorDetails(lastError);
      return false;
    }

    // Pre-validation if requested
    if (loadOptions.validateOnLoad) {
      GLTFLoadResult validationResult;
      if (IsGLBFile(buffer, length, &validationResult)) {
        if (!ValidateGLBHeader(buffer, length, validationResult)) {
          lastError = validationResult;
          PrintErrorDetails(lastError);
          return false;
        }
      } else if (IsGLTFFile(buffer, length, &validationResult)) {
        if (!ValidateGLTFBuffer(buffer, length, validationResult)) {
          lastError = validationResult;
          PrintErrorDetails(lastError);
          return false;
        }
      } else {
        lastError = validationResult;
        PrintErrorDetails(lastError);
        return false;
      }
    }

    // Determine file format and load
    bool success = false;
    if (IsGLBFile(buffer, length, &lastError)) {
      success = LoadGLB(buffer, length);
    } else if (IsGLTFFile(buffer, length, &lastError)) {
      success = LoadGLTF(buffer, length);
    } else {
      if (lastError.error == GLTFError::None) {
        lastError.SetError(GLTFError::InvalidFormat,
                           "Unrecognized glTF/GLB format");
      }
      PrintErrorDetails(lastError);
      return false;
    }

    if (!success) {
      if (lastError.error == GLTFError::None) {
        lastError.SetError(GLTFError::LibraryError,
                           "Failed to parse glTF data");
      }
      PrintErrorDetails(lastError);
      return false;
    }

    // Process the loaded asset
    if (!ProcessAsset()) {
      if (lastError.error == GLTFError::None) {
        lastError.SetError(GLTFError::ValidationFailure,
                           "Failed to process glTF asset");
      }
      PrintErrorDetails(lastError);
      return false;
    }

    // Final validation
    if (loadOptions.validateOnLoad && !ValidateModel(lastError)) {
      PrintErrorDetails(lastError);
      return false;
    }

    // Calculate load time and update performance stats
    auto endTime = std::chrono::high_resolution_clock::now();
    totalLoadTime = std::chrono::duration<double>(endTime - startTime).count();
    UpdateMemoryUsage();

    isValid = true;
    framesSinceLoad = 0;

    DPrintf(DMSG_NOTIFY, "glTF model loaded successfully in %.3f seconds\n",
            totalLoadTime);
    if (loadOptions.validateOnLoad) {
      PrintLoadInfo();
    }

    return true;

  } catch (const std::exception &e) {
    lastError.SetError(GLTFError::LibraryError, e.what());
    Printf("Exception loading glTF model: %s\n", e.what());
    PrintErrorDetails(lastError);
    return false;
  } catch (...) {
    lastError.SetError(GLTFError::LibraryError,
                       "Unknown exception during loading");
    Printf("Unknown exception loading glTF model\n");
    PrintErrorDetails(lastError);
    return false;
  }
}

bool FGLTFModel::LoadGLTF(const char *buffer, int length) {
  fastgltf::Parser parser;

  // Configure parser options
  // NOTE: We DON'T use LoadExternalBuffers - we'll load .bin files manually
  // from PK3. We also DON'T use DecomposeNodeMatrices: nodes authored
  // with a `matrix` property are kept as raw matrices (GLTFNode::rawMatrix)
  // because decomposing rotation * non-uniform scale into TRS is lossy.
  auto options = fastgltf::Options::LoadGLBBuffers;

  fastgltf::GltfDataBuffer dataBuffer;
  if (!dataBuffer.copyBytes(reinterpret_cast<const uint8_t *>(buffer),
                            static_cast<size_t>(length))) {
    lastError.SetError(GLTFError::LibraryError,
                       "Failed to copy glTF data into parser buffer");
    return false;
  }

  // Convert basePath to std::filesystem::path for fastgltf
  std::filesystem::path dirPath;
  if (!basePath.IsEmpty()) {
    dirPath = std::filesystem::path(basePath.GetChars());
  } else {
    dirPath = std::filesystem::path(".");
  }

  auto gltf = parser.loadGLTF(&dataBuffer, dirPath, options);
  if (!gltf) {
    auto error = parser.getError();
    FString errorMsg;
    errorMsg.Format("fastgltf error: %d", static_cast<int>(error));
    Printf("%s\n", errorMsg.GetChars());
    lastError.SetError(GLTFError::LibraryError, errorMsg.GetChars());
    return false;
  }

  auto parseError = gltf->parse(fastgltf::Category::All);
  if (parseError != fastgltf::Error::None) {
    FString errorMsg;
    errorMsg.Format("fastgltf parse error: %d", static_cast<int>(parseError));
    Printf("%s\n", errorMsg.GetChars());
    lastError.SetError(GLTFError::LibraryError, errorMsg.GetChars());
    return false;
  }

  auto parsedAsset = gltf->getParsedAsset();
  if (!parsedAsset) {
    lastError.SetError(GLTFError::LibraryError, "fastgltf returned null asset");
    return false;
  }

  asset = std::move(parsedAsset);
  // Don't validate here - validation happens in LoadWithOptions after
  // ProcessAsset
  return true;
}

bool FGLTFModel::LoadGLB(const char *buffer, int length) {
  fastgltf::Parser parser;

  // For GLB files, we only need LoadGLBBuffers since all data is embedded
  // We explicitly DON'T use LoadExternalBuffers for PK3-loaded files
  // (and not DecomposeNodeMatrices either - see LoadGLTF)
  auto options = fastgltf::Options::LoadGLBBuffers;

  fastgltf::GltfDataBuffer dataBuffer;
  if (!dataBuffer.copyBytes(reinterpret_cast<const uint8_t *>(buffer),
                            static_cast<size_t>(length))) {
    lastError.SetError(GLTFError::LibraryError,
                       "Failed to copy GLB data into parser buffer");
    return false;
  }

  // For GLB files loaded from PK3, the basePath is a virtual path, not a real
  // filesystem path. Since GLB files are self-contained, we just need to
  // provide a valid path to satisfy fastgltf. We use current directory "." as a
  // dummy path since all data is embedded in the GLB.
  std::filesystem::path dirPath(".");

  auto gltf = parser.loadBinaryGLTF(&dataBuffer, dirPath, options);
  if (!gltf) {
    auto error = parser.getError();
    FString errorMsg;
    errorMsg.Format("fastgltf GLB error: %d", static_cast<int>(error));
    Printf("%s\n", errorMsg.GetChars());
    lastError.SetError(GLTFError::LibraryError, errorMsg.GetChars());
    return false;
  }

  auto parseError = gltf->parse(fastgltf::Category::All);
  if (parseError != fastgltf::Error::None) {
    FString errorMsg;
    errorMsg.Format("fastgltf GLB parse error: %d",
                    static_cast<int>(parseError));
    Printf("%s\n", errorMsg.GetChars());
    lastError.SetError(GLTFError::LibraryError, errorMsg.GetChars());
    return false;
  }

  auto parsedAsset = gltf->getParsedAsset();
  if (!parsedAsset) {
    lastError.SetError(GLTFError::LibraryError, "fastgltf returned null asset");
    return false;
  }

  asset = std::move(parsedAsset);
  // Don't validate here - validation happens in LoadWithOptions after
  // ProcessAsset
  return true;
}

bool FGLTFModel::ProcessAsset() {
  if (!asset)
    return false;

  // Process in dependency order
  if (!ProcessBuffers())
    return false;
  if (!ProcessTextures())
    return false;
  if (!ProcessMaterials())
    return false;
  if (!ProcessMeshes())
    return false;
  if (!ProcessNodes())
    return false;
  if (!ProcessSkins())
    return false;
  if (!ProcessAnimations())
    return false;

  // Compute final transformations
  ComputeNodeTransforms();

  // the renderer applies node transforms only through skinning, so
  // bake them into the vertices of unskinned meshes
  BakeNodeTransforms();

  if (hasSkinning) {
    BuildBoneHierarchy();
  }

  if (!BuildSampledAnimationFrames())
    return false;

  return true;
}

bool FGLTFModel::ProcessBuffers() {
  buffers.Resize(asset->buffers.size());

  for (size_t i = 0; i < asset->buffers.size(); ++i) {
    const auto &buffer = asset->buffers[i];

    std::visit(
        fastgltf::visitor{
            [&](const fastgltf::sources::Vector &vector) {
              buffers[i].Resize(vector.bytes.size());
              memcpy(buffers[i].Data(), vector.bytes.data(),
                     vector.bytes.size());
              DPrintf(DMSG_NOTIFY,
                      "glTF: Loaded embedded buffer %zu (%zu bytes)\n", i,
                      vector.bytes.size());
            },
            [&](const fastgltf::sources::ByteView &byteview) {
              buffers[i].Resize(byteview.bytes.size());
              memcpy(buffers[i].Data(), byteview.bytes.data(),
                     byteview.bytes.size());
              DPrintf(DMSG_NOTIFY,
                      "glTF: Loaded byte view buffer %zu (%zu bytes)\n", i,
                      byteview.bytes.size());
            },
            [&](const fastgltf::sources::URI &uri) {
              // Handle external .bin files from PK3
              // In fastgltf 0.8+, uri.uri is a fastgltf::URI type with raw()
              // method returning string_view
              auto uriView = uri.uri.raw();
              std::string uriString(uriView);
              const char *uriStr = uriString.c_str();
              DPrintf(DMSG_NOTIFY,
                      "glTF: Loading external buffer %zu from URI: %s\n", i,
                      uriStr);
              if (!LoadExternalBuffer(uriStr, buffers[i])) {
                Printf("glTF Error: Failed to load external buffer %zu: %s\n",
                       i, uriStr);
              }
            },
            [&](auto &&) {
              Printf("glTF Warning: Unsupported buffer source type for buffer "
                     "%zu\n",
                     i);
            }},
        buffer.data);
  }

  return true;
}

bool FGLTFModel::ProcessTextures() {
  textures.Resize(asset->textures.size());

  for (size_t i = 0; i < asset->textures.size(); ++i) {
    textures[i] = LoadTextureFromGLTF(i, lastError);
  }

  return true;
}

bool FGLTFModel::ProcessMaterials() {
  // Pre-load material textures into the textures array for mesh rendering
  // This ensures materials with textures can be properly bound during rendering
  // and allows MODELDEF Skin directives to override them if needed

  // Iterate through each material and load its baseColorTexture if present
  for (size_t i = 0; i < asset->materials.size(); ++i) {
    const auto &gltfMaterial = asset->materials[i];

    // Check if this material has a base color texture
    if (gltfMaterial.pbrData.has_value()) {
      const auto &pbr = gltfMaterial.pbrData.value();
      if (pbr.baseColorTexture.has_value()) {
        int texIndex = static_cast<int>(pbr.baseColorTexture->textureIndex);

        // Load the texture (this may have already been loaded in
        // ProcessTextures) The textures array is resized in ProcessTextures, so
        // this should be valid
        if (texIndex >= 0 && texIndex < textures.Size() && textures[texIndex]) {
          DPrintf(DMSG_NOTIFY, "glTF: Material %zu uses texture %d\n", i,
                  texIndex);
        }
      }
    }
  }

  return true;
}

bool FGLTFModel::LoadMaterial(int materialIndex,
                              PBRMaterialProperties &material,
                              GLTFLoadResult &result) {
  result.Clear();

  if (materialIndex < 0 ||
      materialIndex >= static_cast<int>(asset->materials.size())) {
    result.SetError(GLTFError::MissingRequiredData,
                    "Material index out of range");
    return false;
  }

  const auto &gltfMaterial = asset->materials[materialIndex];

  // Load PBR metallic-roughness properties
  if (gltfMaterial.pbrData.has_value()) {
    const auto &pbr = gltfMaterial.pbrData.value();

    material.baseColorFactor =
        FVector4(pbr.baseColorFactor[0], pbr.baseColorFactor[1],
                 pbr.baseColorFactor[2], pbr.baseColorFactor[3]);

    material.metallicFactor = pbr.metallicFactor;
    material.roughnessFactor = pbr.roughnessFactor;

    // Base color texture
    if (pbr.baseColorTexture.has_value()) {
      material.baseColorTextureIndex =
          static_cast<int>(pbr.baseColorTexture->textureIndex);
      material.baseColorTexCoord =
          static_cast<int>(pbr.baseColorTexture->texCoordIndex);
    }

    // Metallic-roughness texture
    if (pbr.metallicRoughnessTexture.has_value()) {
      material.metallicRoughnessTextureIndex =
          static_cast<int>(pbr.metallicRoughnessTexture->textureIndex);
      material.metallicRoughnessTexCoord =
          static_cast<int>(pbr.metallicRoughnessTexture->texCoordIndex);
    }
  }

  // Normal texture
  if (gltfMaterial.normalTexture.has_value()) {
    material.normalTextureIndex =
        static_cast<int>(gltfMaterial.normalTexture->textureIndex);
    material.normalTexCoord =
        static_cast<int>(gltfMaterial.normalTexture->texCoordIndex);
    material.normalScale = gltfMaterial.normalTexture->scale;
  }

  // Occlusion texture
  if (gltfMaterial.occlusionTexture.has_value()) {
    material.occlusionTextureIndex =
        static_cast<int>(gltfMaterial.occlusionTexture->textureIndex);
    material.occlusionTexCoord =
        static_cast<int>(gltfMaterial.occlusionTexture->texCoordIndex);
    material.occlusionStrength = gltfMaterial.occlusionTexture->scale;
  }

  // Emissive properties
  material.emissiveFactor =
      FVector3(gltfMaterial.emissiveFactor[0], gltfMaterial.emissiveFactor[1],
               gltfMaterial.emissiveFactor[2]);

  if (gltfMaterial.emissiveTexture.has_value()) {
    material.emissiveTextureIndex =
        static_cast<int>(gltfMaterial.emissiveTexture->textureIndex);
    material.emissiveTexCoord =
        static_cast<int>(gltfMaterial.emissiveTexture->texCoordIndex);
  }

  // Alpha properties
  material.alphaCutoff = gltfMaterial.alphaCutoff;
  material.alphaMode = static_cast<int>(gltfMaterial.alphaMode);
  material.doubleSided = gltfMaterial.doubleSided;

  return true;
}

template <typename T>
bool FGLTFModel::ReadAccessorTyped(int accessorIndex, TArray<T> &outData) {
  TArray<uint8_t> rawData;
  int count, stride;

  if (!ReadAccessor(accessorIndex, rawData, count, stride)) {
    return false;
  }

  // The deinterleaved output matches the element size exactly; anything
  // else means the accessor type does not match T, and a blind memcpy
  // could read past the source data.
  if (stride != static_cast<int>(sizeof(T))) {
    Printf("Warning: glTF accessor %d stride mismatch. Expected %zu, got "
           "%d; rejecting\n",
           accessorIndex, sizeof(T), stride);
    return false;
  }

  outData.Resize(count);
  memcpy(outData.Data(), rawData.Data(), count * sizeof(T));

  return true;
}

bool FGLTFModel::ReadAccessor(int accessorIndex, TArray<uint8_t> &outData,
                              int &outCount, int &outStride) {
  if (accessorIndex < 0 ||
      accessorIndex >= static_cast<int>(asset->accessors.size())) {
    return false;
  }

  const auto &accessor = asset->accessors[accessorIndex];
  if (!accessor.bufferViewIndex.has_value()) {
    return false;
  }

  // Reject references into parts of the file we do not have. All range
  // checks below use subtraction/division comparisons instead of
  // addition/multiplication so they cannot overflow.
  const size_t bufferViewIndex = accessor.bufferViewIndex.value();
  if (bufferViewIndex >= asset->bufferViews.size()) {
    return false;
  }

  const auto &bufferView = asset->bufferViews[bufferViewIndex];
  if (bufferView.bufferIndex >= static_cast<size_t>(buffers.Size())) {
    return false;
  }

  const TArray<uint8_t> &buffer = buffers[bufferView.bufferIndex];
  const size_t bufferSize = buffer.Size();

  // The buffer view itself must lie entirely inside its buffer.
  if (bufferView.byteOffset > bufferSize ||
      bufferView.byteLength > bufferSize - bufferView.byteOffset) {
    return false;
  }
  if (accessor.byteOffset > bufferView.byteLength) {
    return false;
  }
  const size_t available = bufferView.byteLength - accessor.byteOffset;

  const size_t elementSize =
      fastgltf::getElementByteSize(accessor.type, accessor.componentType);
  if (elementSize == 0) {
    return false;
  }

  // Source element stride: an explicit byteStride for interleaved views,
  // otherwise tightly packed. A stride smaller than the element size
  // would mean overlapping elements, which cannot be read safely.
  const size_t srcStride = bufferView.byteStride.has_value()
                               ? bufferView.byteStride.value()
                               : elementSize;
  if (srcStride < elementSize) {
    return false;
  }

  const size_t count = accessor.count;
  if (count > 0) {
    // The last element occupies bytes [(count-1)*srcStride,
    // (count-1)*srcStride + elementSize); it must fit in the available
    // range.
    if (elementSize > available ||
        (count - 1) > (available - elementSize) / srcStride) {
      return false;
    }
  }

  if (count > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  outCount = static_cast<int>(count);
  outStride = static_cast<int>(elementSize);
  const size_t totalSize = count * elementSize;
  outData.Resize(totalSize);

  const uint8_t *srcData =
      buffer.Data() + bufferView.byteOffset + accessor.byteOffset;

  if (srcStride != elementSize) {
    // Interleaved data - need to deinterleave
    for (size_t i = 0; i < count; ++i) {
      memcpy(outData.Data() + i * elementSize, srcData + i * srcStride,
             elementSize);
    }
  } else {
    // Packed data - direct copy
    memcpy(outData.Data(), srcData, totalSize);
  }

  return true;
}

template <>
bool FGLTFModel::ReadAccessorTyped<FVector3>(int accessorIndex,
                                             TArray<FVector3> &outData) {
  TArray<uint8_t> rawData;
  int count, stride;

  if (!ReadAccessor(accessorIndex, rawData, count, stride)) {
    return false;
  }

  if (!IsAccessorValid(accessorIndex)) {
    return false;
  }

  const auto &accessor = asset->accessors[accessorIndex];

  if (accessor.type == fastgltf::AccessorType::Vec3) {
    outData.Resize(count);

    if (accessor.componentType == fastgltf::ComponentType::Float) {
      const float *src = reinterpret_cast<const float *>(rawData.Data());
      for (int i = 0; i < count; ++i) {
        outData[i] = FVector3(src[i * 3], src[i * 3 + 1], src[i * 3 + 2]);
      }
      return true;
    }
  }

  return false;
}

template <>
bool FGLTFModel::ReadAccessorTyped<FVector4>(int accessorIndex,
                                             TArray<FVector4> &outData) {
  TArray<uint8_t> rawData;
  int count, stride;

  if (!ReadAccessor(accessorIndex, rawData, count, stride)) {
    return false;
  }

  if (!IsAccessorValid(accessorIndex)) {
    return false;
  }

  const auto &accessor = asset->accessors[accessorIndex];

  if (accessor.type == fastgltf::AccessorType::Vec4 &&
      accessor.componentType == fastgltf::ComponentType::Float) {
    outData.Resize(count);

    const float *src = reinterpret_cast<const float *>(rawData.Data());
    for (int i = 0; i < count; ++i) {
      outData[i] =
          FVector4(src[i * 4], src[i * 4 + 1], src[i * 4 + 2], src[i * 4 + 3]);
    }
    return true;
  }

  if (accessor.type == fastgltf::AccessorType::Vec4 &&
      accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
    outData.Resize(count);

    const uint8_t *src = rawData.Data();
    const float scale = accessor.normalized ? 1.0f / 255.0f : 1.0f;
    for (int i = 0; i < count; ++i) {
      outData[i] = FVector4(src[i * 4] * scale, src[i * 4 + 1] * scale,
                            src[i * 4 + 2] * scale,
                            src[i * 4 + 3] * scale);
    }
    return true;
  }

  if (accessor.type == fastgltf::AccessorType::Vec4 &&
      accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
    outData.Resize(count);

    const uint16_t *src = reinterpret_cast<const uint16_t *>(rawData.Data());
    const float scale = accessor.normalized ? 1.0f / 65535.0f : 1.0f;
    for (int i = 0; i < count; ++i) {
      outData[i] = FVector4(src[i * 4] * scale, src[i * 4 + 1] * scale,
                            src[i * 4 + 2] * scale,
                            src[i * 4 + 3] * scale);
    }
    return true;
  }

  return false;
}

bool FGLTFModel::ReadAccessorVec4UInt(
    int accessorIndex, TArray<std::array<uint32_t, 4>> &outData) {
  TArray<uint8_t> rawData;
  int count, stride;

  if (!ReadAccessor(accessorIndex, rawData, count, stride)) {
    return false;
  }

  if (!IsAccessorValid(accessorIndex)) {
    return false;
  }

  const auto &accessor = asset->accessors[accessorIndex];
  if (accessor.type != fastgltf::AccessorType::Vec4) {
    return false;
  }

  outData.Resize(count);

  if (accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
    const uint8_t *src = rawData.Data();
    for (int i = 0; i < count; ++i) {
      outData[i] = {src[i * 4], src[i * 4 + 1], src[i * 4 + 2],
                    src[i * 4 + 3]};
    }
    return true;
  }

  if (accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
    const uint16_t *src = reinterpret_cast<const uint16_t *>(rawData.Data());
    for (int i = 0; i < count; ++i) {
      outData[i] = {src[i * 4], src[i * 4 + 1], src[i * 4 + 2],
                    src[i * 4 + 3]};
    }
    return true;
  }

  if (accessor.componentType == fastgltf::ComponentType::UnsignedInt) {
    const uint32_t *src = reinterpret_cast<const uint32_t *>(rawData.Data());
    for (int i = 0; i < count; ++i) {
      outData[i] = {src[i * 4], src[i * 4 + 1], src[i * 4 + 2],
                    src[i * 4 + 3]};
    }
    return true;
  }

  return false;
}

template <>
bool FGLTFModel::ReadAccessorTyped<uint32_t>(int accessorIndex,
                                             TArray<uint32_t> &outData) {
  TArray<uint8_t> rawData;
  int count, stride;

  if (!ReadAccessor(accessorIndex, rawData, count, stride)) {
    return false;
  }

  if (!IsAccessorValid(accessorIndex)) {
    return false;
  }

  const auto &accessor = asset->accessors[accessorIndex];
  outData.Resize(count);

  if (accessor.componentType == fastgltf::ComponentType::UnsignedInt) {
    memcpy(outData.Data(), rawData.Data(), count * sizeof(uint32_t));
  } else if (accessor.componentType == fastgltf::ComponentType::UnsignedShort) {
    const uint16_t *src = reinterpret_cast<const uint16_t *>(rawData.Data());
    for (int i = 0; i < count; ++i) {
      outData[i] = static_cast<uint32_t>(src[i]);
    }
  } else if (accessor.componentType == fastgltf::ComponentType::UnsignedByte) {
    const uint8_t *src = rawData.Data();
    for (int i = 0; i < count; ++i) {
      outData[i] = static_cast<uint32_t>(src[i]);
    }
  } else {
    return false;
  }

  return true;
}

template <>
bool FGLTFModel::ReadAccessorTyped<FQuaternion>(int accessorIndex,
                                                TArray<FQuaternion> &outData) {
  TArray<uint8_t> rawData;
  int count, stride;

  if (!ReadAccessor(accessorIndex, rawData, count, stride)) {
    return false;
  }

  if (!IsAccessorValid(accessorIndex)) {
    return false;
  }

  const auto &accessor = asset->accessors[accessorIndex];

  if (accessor.type == fastgltf::AccessorType::Vec4 &&
      accessor.componentType == fastgltf::ComponentType::Float) {
    outData.Resize(count);

    const float *src = reinterpret_cast<const float *>(rawData.Data());
    for (int i = 0; i < count; ++i) {
      outData[i] = FQuaternion(src[i * 4], src[i * 4 + 1], src[i * 4 + 2],
                               src[i * 4 + 3]);
    }
    return true;
  }

  return false;
}
bool FGLTFModel::ProcessNodes() {
  scene.nodes.Resize(asset->nodes.size());

  // First pass: load basic node data
  for (size_t i = 0; i < asset->nodes.size(); ++i) {
    const auto &gltfNode = asset->nodes[i];
    GLTFNode &node = scene.nodes[i];

    node.name = gltfNode.name.c_str();
    node.transform = MakeIdentityTRS();

    std::visit(
        fastgltf::visitor{[&](const fastgltf::Node::TRS &trs) {
                            node.transform = TRSFromFastgltfTRS(trs);
                          },
                          [&](const fastgltf::Node::TransformMatrix &matrix) {
                            // keep the raw matrix (column-major per the
                            // glTF spec); see GLTFNode::hasRawMatrix
                            node.hasRawMatrix = true;
                            for (int m = 0; m < 16; ++m)
                              node.rawMatrix[m] = matrix[m];
                          }},
        gltfNode.transform);

    node.meshIndex = gltfNode.meshIndex.has_value()
                         ? static_cast<int>(gltfNode.meshIndex.value())
                         : -1;
    node.skinIndex = gltfNode.skinIndex.has_value()
                         ? static_cast<int>(gltfNode.skinIndex.value())
                         : -1;
  }

  // Second pass: build hierarchy
  for (size_t i = 0; i < asset->nodes.size(); ++i) {
    const auto &gltfNode = asset->nodes[i];
    GLTFNode &node = scene.nodes[i];

    for (size_t childIndex : gltfNode.children) {
      if (childIndex >= asset->nodes.size()) {
        // Out-of-range child reference: ignore it rather than corrupt
        // scene.nodes with an invalid parentIndex.
        continue;
      }
      const int child = static_cast<int>(childIndex);
      node.childIndices.Push(child);
      scene.nodes[child].parentIndex = static_cast<int>(i);
    }
  }

  // Find root nodes (nodes with no parent)
  for (size_t i = 0; i < scene.nodes.Size(); ++i) {
    if (scene.nodes[i].parentIndex == -1) {
      scene.rootNodeIndices.Push(static_cast<int>(i));
    }
  }

  return true;
}

bool FGLTFModel::ProcessSkins() {
  scene.skins.Resize(asset->skins.size());

  for (size_t i = 0; i < asset->skins.size(); ++i) {
    const auto &gltfSkin = asset->skins[i];
    GLTFSkin &skin = scene.skins[i];

    skin.name = gltfSkin.name.c_str();

    // Copy joint indices, ignoring joints that reference nodes outside
    // the scene. Invalid slots stay -1 (all consumers range-check) so
    // the joint array stays aligned with the inverse bind matrices.
    skin.jointIndices.Resize(gltfSkin.joints.size());
    for (size_t j = 0; j < gltfSkin.joints.size(); ++j) {
      const size_t jointNode = gltfSkin.joints[j];
      if (jointNode >= static_cast<size_t>(scene.nodes.Size())) {
        skin.jointIndices[j] = -1;
        continue;
      }
      const int jointIndex = static_cast<int>(jointNode);
      skin.jointIndices[j] = jointIndex;
      scene.nodes[jointIndex].isBone = true;
      scene.nodes[jointIndex].boneIndex = static_cast<int>(j);
    }

    // Load inverse bind matrices
    if (gltfSkin.inverseBindMatrices.has_value()) {
      TArray<std::array<float, 16>> matrices;
      if (ReadAccessorTyped(
              static_cast<int>(gltfSkin.inverseBindMatrices.value()),
              matrices)) {
        skin.inverseBindMatrices.Resize(matrices.Size());
        for (int j = 0; j < matrices.Size(); ++j) {
          auto &dstMat = skin.inverseBindMatrices[j];
          dstMat.loadMatrix(matrices[j].data());
        }
      }
    }

    if (gltfSkin.skeleton.has_value() &&
        gltfSkin.skeleton.value() < static_cast<size_t>(scene.nodes.Size())) {
      skin.skeletonRootIndex = static_cast<int>(gltfSkin.skeleton.value());
    }
  }

  return true;
}

bool FGLTFModel::ProcessAnimations() {
  scene.animations.Resize(asset->animations.size());
  modelAnimations.Resize(asset->animations.size());
  namedAnimations.Clear();

  bool success = true;

  for (size_t i = 0; i < asset->animations.size(); ++i) {
    if (!ConvertGLTFAnimation(asset->animations[i], scene.animations[i],
                              lastError)) {
      PrintErrorDetails(lastError);
      success = false;
      continue;
    }

    if (scene.animations[i].name.IsEmpty()) {
      scene.animations[i].name.Format("Animation%zu", i);
    }
    namedAnimations.Insert(FName(scene.animations[i].name), static_cast<int>(i));

    // Frame ranges are assigned after skins are processed and the clip is
    // sampled into the shared FModel animation frame cache.
    ModelAnim &modelAnim = modelAnimations[i];
    modelAnim.firstFrame = 0;
    modelAnim.lastFrame = 0;
    modelAnim.loopFrame = 0;
    modelAnim.framerate = GLTFAnimationSampleRate;
    modelAnim.startFrame = 0;
    modelAnim.flags = MODELANIM_LOOP;
    modelAnim.startTic = 0;
    modelAnim.switchOffset = 0;
  }

  return success;
}

bool FGLTFModel::ConvertGLTFAnimation(const fastgltf::Animation &gltfAnim,
                                      GLTFAnimation &outAnim,
                                      GLTFLoadResult &result) {
  result.Clear();
  bool success = true;

  outAnim.name = gltfAnim.name.c_str();
  outAnim.samplers.Resize(gltfAnim.samplers.size());
  outAnim.channels.Resize(gltfAnim.channels.size());

  float maxTime = 0.0f;

  // Convert samplers
  for (size_t i = 0; i < gltfAnim.samplers.size(); ++i) {
    const auto &gltfSampler = gltfAnim.samplers[i];
    GLTFAnimationSampler &sampler = outAnim.samplers[i];

    sampler.inputAccessorIndex = static_cast<int>(gltfSampler.inputAccessor);
    sampler.outputAccessorIndex = static_cast<int>(gltfSampler.outputAccessor);
    sampler.interpolation = ToInterpolationString(gltfSampler.interpolation);

    // Find max time for duration calculation
    TArray<float> times;
    if (ReadAccessorTyped(sampler.inputAccessorIndex, times) &&
        times.Size() > 0) {
      maxTime = std::max(maxTime, times.Last());
    } else if (times.Size() == 0) {
      result.SetError(GLTFError::AnimationError,
                      "Missing keyframe times for animation sampler");
      success = false;
    }
  }

  // Convert channels
  for (size_t i = 0; i < gltfAnim.channels.size(); ++i) {
    const auto &gltfChannel = gltfAnim.channels[i];
    GLTFAnimationChannel &channel = outAnim.channels[i];

    channel.samplerIndex = static_cast<int>(gltfChannel.samplerIndex);
    channel.targetNodeIndex = (gltfChannel.nodeIndex < scene.nodes.Size())
                                  ? static_cast<int>(gltfChannel.nodeIndex)
                                  : -1;
    channel.targetPath = ToAnimationPathString(gltfChannel.path);
  }

  // Clamp the duration: non-finite or absurd keyframe times would
  // otherwise produce huge frame counts when the clip is sampled.
  const float maxDuration =
      static_cast<float>(GLTFMaxSampledFrames) / GLTFAnimationSampleRate;
  outAnim.duration = std::isfinite(maxTime)
                         ? std::clamp(maxTime, 0.0f, maxDuration)
                         : 0.0f;
  return success;
}

// Coordinate convention conversion applied at the scene roots. Doom model
// space is Y-up (render Y = world Z) with the actor's facing along +X, and
// MD3 content reaches it through the (x,z,y) axis swap — a mirror — so the
// pipeline's model space is left-handed relative to glTF. Passing glTF data
// through raw therefore rendered every glTF model mirrored (decal text read
// backwards). This matrix maps glTF +Z (asset front) to +X (actor facing)
// and glTF +X to render +Z while keeping +Y up: a swap of X and Z, which
// has the same handedness as the MD3 path, so text and asymmetric details
// match the asset. Face winding needs no fixup (model drawing is two-sided,
// Cull_None) and normals stay consistent through the inverse-transpose
// bake. Applied at the roots it covers unskinned meshes (via
// BakeNodeTransforms) and skinned ones (bone matrices derive from the same
// node globals; see also buildMatrices' root fallback).
static VSMatrix GLTFSceneConversion() {
  // column-major: columns are the images of +X, +Y, +Z
  static const float swap[16] = {0, 0, 1, 0,  // +X -> +Z
                                 0, 1, 0, 0,  // +Y -> +Y
                                 1, 0, 0, 0,  // +Z -> +X
                                 0, 0, 0, 1};
  VSMatrix m;
  m.loadMatrix(swap);
  return m;
}

void FGLTFModel::ComputeNodeTransforms() {
  // Compute local matrices
  for (auto &node : scene.nodes) {
    if (node.hasRawMatrix)
      node.localMatrix.loadMatrix(node.rawMatrix);
    else
      node.localMatrix = BuildMatrixFromTRS(node.transform);
  }

  // Compute global matrices via an iterative traversal. The per-node
  // "done" flag both keeps the traversal bounded regardless of graph
  // depth and breaks cycles: a malformed asset with a node reachable
  // from its own descendants must not recurse or loop forever.
  const int nodeCount = scene.nodes.Size();
  if (nodeCount <= 0) {
    return;
  }

  TArray<uint8_t> done;
  done.Resize(nodeCount);
  memset(done.Data(), 0, done.Size());

  struct PendingNode {
    int nodeIndex;
    VSMatrix parentMatrix;
  };
  TArray<PendingNode> stack;

  for (int rootIndex : scene.rootNodeIndices) {
    if (rootIndex < 0 || rootIndex >= nodeCount || done[rootIndex]) {
      continue;
    }

    stack.Clear();
    stack.Push({rootIndex, GLTFSceneConversion()});

    while (stack.Size() > 0) {
      const PendingNode current = stack.Last();
      stack.Pop();

      const int nodeIndex = current.nodeIndex;
      if (nodeIndex < 0 || nodeIndex >= nodeCount || done[nodeIndex]) {
        continue;
      }
      done[nodeIndex] = 1;

      GLTFNode &node = scene.nodes[nodeIndex];
      VSMatrix combined = current.parentMatrix;
      combined.multMatrix(node.localMatrix);
      node.globalMatrix = combined;

      for (int childIndex : node.childIndices) {
        if (childIndex < 0 || childIndex >= nodeCount || done[childIndex]) {
          continue;
        }
        stack.Push({childIndex, combined});
      }
    }
  }
}

// The renderer uploads raw vertex positions and only ever applies node
// transforms through the skinning matrices, so node TRS/matrices on
// unskinned models would otherwise be silently ignored (wrong size and
// part placement on most real-world assets, e.g. Sketchfab exports).
// Bake them into the vertex data instead.
void FGLTFModel::BakeNodeTransforms() {
  if (asset == nullptr)
    return;

  // map each glTF mesh to its run of primitives inside scene.meshes,
  // as recorded by ProcessMeshes (skipped primitives would otherwise
  // shift every run after them)
  TArray<int> firstPrim;
  TArray<int> primCount;
  if (meshPrimStart.Size() == asset->meshes.size()) {
    firstPrim = meshPrimStart;
    primCount = meshPrimCount;
  } else {
    // fallback for models processed before the mapping existed
    firstPrim.Resize(asset->meshes.size());
    primCount.Resize(asset->meshes.size());
    int runStart = 0;
    for (size_t i = 0; i < asset->meshes.size(); ++i) {
      firstPrim[i] = runStart;
      primCount[i] = (int)asset->meshes[i].primitives.size();
      runStart += primCount[i];
    }
  }

  // meshes instanced by several nodes bake only once (proper support
  // would require duplicating the mesh per instance)
  TArray<uint8_t> baked;
  baked.Resize(asset->meshes.size());
  memset(baked.Data(), 0, baked.Size());

  for (auto &node : scene.nodes) {
    // skinned meshes are posed through the bone matrices
    if (node.meshIndex < 0 || node.skinIndex >= 0)
      continue;
    if (node.meshIndex >= (int)asset->meshes.size())
      continue;
    if (baked[node.meshIndex])
      continue;
    baked[node.meshIndex] = 1;

    const auto &m = node.globalMatrix.get();

    int begin = firstPrim[node.meshIndex];
    int end = begin + primCount[node.meshIndex];

    // normals transform with the inverse-transpose of the upper 3x3,
    // otherwise non-uniform scale skews them (column-major storage)
    float nrm[9];
    {
      float a = m[0], b = m[4], c = m[8];
      float d = m[1], e = m[5], f = m[9];
      float g = m[2], h = m[6], k = m[10];
      float det = a * (e * k - f * h) - b * (d * k - f * g) +
                  c * (d * h - e * g);
      if (fabsf(det) > 1e-12f) {
        float inv = 1.0f / det;
        // adjugate^T / det = inverse^T; store row-major inverse-transpose
        nrm[0] = (e * k - f * h) * inv;
        nrm[1] = (f * g - d * k) * inv;
        nrm[2] = (d * h - e * g) * inv;
        nrm[3] = (c * h - b * k) * inv;
        nrm[4] = (a * k - c * g) * inv;
        nrm[5] = (b * g - a * h) * inv;
        nrm[6] = (b * f - c * e) * inv;
        nrm[7] = (c * d - a * f) * inv;
        nrm[8] = (a * e - b * d) * inv;
      } else {
        // degenerate: fall back to the rotation-ish upper 3x3
        nrm[0] = a; nrm[1] = d; nrm[2] = g;
        nrm[3] = b; nrm[4] = e; nrm[5] = h;
        nrm[6] = c; nrm[7] = f; nrm[8] = k;
      }
    }

    // TEMP DEBUG: dump baked AABB per mesh node
    if (getenv("GLTF_BAKE_DEBUG") != nullptr) {
      float mn[3] = {1e30f, 1e30f, 1e30f};
      float mx[3] = {-1e30f, -1e30f, -1e30f};
      for (int mi = begin; mi < end; ++mi) {
        for (auto &v : scene.meshes[mi].vertices) {
          float px = m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12];
          float py = m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13];
          float pz = m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14];
          mn[0] = std::min(mn[0], px); mn[1] = std::min(mn[1], py); mn[2] = std::min(mn[2], pz);
          mx[0] = std::max(mx[0], px); mx[1] = std::max(mx[1], py); mx[2] = std::max(mx[2], pz);
        }
      }
      Printf("BAKEDBG node '%s' mesh %d min(%.2f %.2f %.2f) max(%.2f %.2f %.2f)\n",
             node.name.GetChars(), node.meshIndex, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
    }

    for (int mi = begin; mi < end; ++mi) {
      for (auto &v : scene.meshes[mi].vertices) {
        float px = v.x, py = v.y, pz = v.z;
        v.x = m[0] * px + m[4] * py + m[8] * pz + m[12];
        v.y = m[1] * px + m[5] * py + m[9] * pz + m[13];
        v.z = m[2] * px + m[6] * py + m[10] * pz + m[14];

        // normals transform with the inverse-transpose (renormalized)
        int inx = ((int)v.packedNormal << 22) >> 22;
        int iny = ((int)v.packedNormal << 12) >> 22;
        int inz = ((int)v.packedNormal << 2) >> 22;

        if (inx || iny || inz) {
          float nx = inx / 512.0f, ny = iny / 512.0f, nz = inz / 512.0f;
          float tx = nrm[0] * nx + nrm[1] * ny + nrm[2] * nz;
          float ty = nrm[3] * nx + nrm[4] * ny + nrm[5] * nz;
          float tz = nrm[6] * nx + nrm[7] * ny + nrm[8] * nz;
          float len = sqrtf(tx * tx + ty * ty + tz * tz);

          if (len > 1e-6f) {
            tx /= len;
            ty /= len;
            tz /= len;
          }

          v.SetNormal(tx, ty, tz);
        }
      }
    }
  }
}

void FGLTFModel::BuildBoneHierarchy() {
  if (scene.skins.Size() == 0)
    return;

  // For now, use the first skin
  const GLTFSkin &skin = scene.skins[0];

  namedBones.Clear();
  rootBones.Clear();
  basePose.Resize(skin.jointIndices.Size());
  boneMatrices.Resize(skin.jointIndices.Size());

  for (int i = 0; i < skin.jointIndices.Size(); ++i) {
    int nodeIndex = skin.jointIndices[i];
    if (nodeIndex < 0 || nodeIndex >= scene.nodes.Size()) {
      basePose[i] = MakeIdentityTRS();
      boneMatrices[i].loadIdentity();
      continue;
    }

    basePose[i] = scene.nodes[nodeIndex].transform;
    boneMatrices[i] = scene.nodes[nodeIndex].globalMatrix;
    scene.nodes[nodeIndex].boneIndex = i;

    if (!scene.nodes[nodeIndex].name.IsEmpty()) {
      namedBones.Insert(FName(scene.nodes[nodeIndex].name), i);
    }

    const int parentNode = scene.nodes[nodeIndex].parentIndex;
    if (parentNode < 0 || parentNode >= scene.nodes.Size() ||
        scene.nodes[parentNode].boneIndex < 0) {
      rootBones.Push(i);
    }

    if (i < skin.inverseBindMatrices.Size()) {
      VSMatrix combined = boneMatrices[i];
      combined.multMatrix(skin.inverseBindMatrices[i]);
      boneMatrices[i] = combined;
    }
  }
}

bool FGLTFModel::BuildSampledAnimationFrames() {
  sampledAnimationFrames.Clear();

  int frameBase = 0;
  const int boneCount = GetBoneCount();

  for (int animIndex = 0; animIndex < scene.animations.Size(); ++animIndex) {
    const GLTFAnimation &anim = scene.animations[animIndex];
    int frameCount = GetSampledFrameCount(anim.duration);

    ModelAnim &modelAnim = modelAnimations[animIndex];
    modelAnim.firstFrame = frameBase;
    modelAnim.lastFrame = frameBase + frameCount;
    modelAnim.loopFrame = frameBase;
    modelAnim.framerate = GLTFAnimationSampleRate;
    modelAnim.startFrame = frameBase;
    modelAnim.flags = MODELANIM_LOOP;
    modelAnim.startTic = 0;
    modelAnim.switchOffset = 0;

    if (boneCount > 0) {
      // Overflow-safe accumulation check before resizing: both factors
      // are bounded, but keep the total within int range.
      if (frameCount > 0 &&
          boneCount > (std::numeric_limits<int>::max() -
                       sampledAnimationFrames.Size()) /
                          frameCount) {
        Printf("glTF Warning: sampled frames for animation '%s' exceed "
               "engine limits; skipping clip samples\n",
               anim.name.GetChars());
        // Keep the clip's exported frame range empty so consumers never
        // index the shared sampled-frame array beyond its allocated size.
        frameCount = 0;
      } else {
        const int oldSize = sampledAnimationFrames.Size();
        sampledAnimationFrames.Resize(oldSize + frameCount * boneCount);

        for (int frame = 0; frame < frameCount; ++frame) {
          const float sampleTime =
              anim.duration > 0.0f
                  ? std::min(anim.duration, frame / GLTFAnimationSampleRate)
                  : 0.0f;

          TArray<TRS> sampledPose;
          if (!SampleAnimation(anim, sampleTime, sampledPose) ||
              sampledPose.Size() != boneCount) {
            sampledPose = basePose;
          }

          for (int bone = 0; bone < boneCount; ++bone) {
            sampledAnimationFrames[oldSize + frame * boneCount + bone] =
                sampledPose[bone];
          }
        }
      }
    }

    frameBase += frameCount;
  }

  return true;
}

FGameTexture *FGLTFModel::LoadTextureFromGLTF(int textureIndex,
                                              GLTFLoadResult &result) {
  if (textureIndex < 0 ||
      textureIndex >= static_cast<int>(asset->textures.size())) {
    result.SetError(GLTFError::TextureLoadFailure, "Invalid texture index");
    return nullptr;
  }

  const auto &gltfTexture = asset->textures[textureIndex];

  if (!gltfTexture.imageIndex.has_value()) {
    result.SetError(GLTFError::TextureLoadFailure,
                    "Texture has no image reference");
    return nullptr;
  }

  size_t imageIndex = gltfTexture.imageIndex.value();
  if (imageIndex >= asset->images.size()) {
    result.SetError(GLTFError::TextureLoadFailure,
                    "Texture references invalid image");
    return nullptr;
  }

  const auto &image = asset->images[imageIndex];

  try {
    // Handle different image sources
    if (std::holds_alternative<fastgltf::sources::URI>(image.data)) {
      const auto &uri = std::get<fastgltf::sources::URI>(image.data);
      auto rawUri = uri.uri.raw();
      return LoadTextureFromURI(rawUri.data(), result);
    } else if (std::holds_alternative<fastgltf::sources::BufferView>(
                   image.data)) {
      const auto &bufferView =
          std::get<fastgltf::sources::BufferView>(image.data);
      return LoadTextureFromBufferView(bufferView.bufferViewIndex, result);
    } else if (std::holds_alternative<fastgltf::sources::Vector>(image.data)) {
      const auto &vec = std::get<fastgltf::sources::Vector>(image.data);
      return LoadTextureFromMemory(vec.bytes.data(), vec.bytes.size(),
                                   result);
    } else if (std::holds_alternative<fastgltf::sources::ByteView>(
                   image.data)) {
      const auto &bv = std::get<fastgltf::sources::ByteView>(image.data);
      return LoadTextureFromMemory(
          reinterpret_cast<const uint8_t *>(bv.bytes.data()),
          bv.bytes.size(), result);
    } else {
      result.SetError(GLTFError::TextureLoadFailure,
                      "Unsupported image data source");
      return nullptr;
    }
  } catch (const std::exception &e) {
    result.SetError(GLTFError::TextureLoadFailure, e.what());
    return nullptr;
  }
}

FGameTexture *FGLTFModel::LoadTextureFromURI(const char *uri,
                                             GLTFLoadResult &result) {
  if (!uri || !*uri) {
    result.SetError(GLTFError::TextureLoadFailure, "Empty URI");
    return nullptr;
  }

  // Convert URI to filesystem path
  FString texturePath;

  // Handle data URIs (base64-embedded images)
  if (strncmp(uri, "data:", 5) == 0) {
    const char *comma = strchr(uri, ',');
    if (comma && strstr(uri, ";base64") && strstr(uri, ";base64") < comma) {
      const char *b64 = comma + 1;

      // count actual base64 characters (whitespace/padding excluded)
      size_t digits = 0;
      for (const char *p = b64; *p && *p != '='; ++p)
        if (isalnum((uint8_t)*p) || *p == '+' || *p == '/')
          digits++;

      TArray<uint8_t> decoded;
      decoded.Resize((digits * 3) / 4 + 3);
      base64_decode(decoded.Data(), decoded.Size(), b64);

      return LoadTextureFromMemory(decoded.Data(), (digits * 3) / 4,
                                   result);
    }
    result.SetError(GLTFError::TextureLoadFailure,
                    "Unsupported data URI (need base64)");
    return nullptr;
  }

  // Handle relative paths - need to resolve relative to model path
  if (uri[0] != '/' && strstr(uri, "://") == nullptr) {
    // This is a relative path - resolve it against the model file's
    // own directory (glTF URIs are relative to the .gltf), falling
    // back to the historical "models/" prefix.
    if (basePath.IsNotEmpty()) {
      texturePath.Format("%s%s", basePath.GetChars(), uri);
      if (fileSystem.CheckNumForFullName(texturePath.GetChars()) < 0)
        texturePath.Format("models/%s", uri);
    } else {
      texturePath.Format("models/%s", uri);
    }
  } else {
    texturePath = uri;
  }

  // Try to load the texture through GZDoom's texture manager
  int lump = fileSystem.CheckNumForFullName(texturePath.GetChars());
  if (lump >= 0) {
    FString texName = fileSystem.GetFileFullName(lump);
    FTextureID texID = TexMan.CheckForTexture(
        texName.GetChars(), ETextureType::Any,
        FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ForceLookup);

    if (texID.isValid()) {
      return TexMan.GetGameTexture(texID);
    }
  }

  // Try different extensions if the exact path doesn't work
  const char *extensions[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", nullptr};
  FString basePath = texturePath;

  // Remove existing extension
  auto lastDot = basePath.LastIndexOf('.');
  if (lastDot >= 0) {
    basePath.Truncate(lastDot);
  }

  for (const char **ext = extensions; *ext; ++ext) {
    FString tryPath = basePath + *ext;
    int tryLump = fileSystem.CheckNumForFullName(tryPath.GetChars());

    if (tryLump >= 0) {
      FString texName = fileSystem.GetFileFullName(tryLump);
      FTextureID texID = TexMan.CheckForTexture(
          texName.GetChars(), ETextureType::Any,
          FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ForceLookup);

      if (texID.isValid()) {
        DPrintf(DMSG_NOTIFY, "Loaded glTF texture: %s\n", tryPath.GetChars());
        return TexMan.GetGameTexture(texID);
      }
    }
  }

  FString errorMsg;
  errorMsg.Format("Could not load texture: %s", uri);
  result.SetError(GLTFError::TextureLoadFailure, errorMsg.GetChars());

  DPrintf(DMSG_WARNING, "Failed to load glTF texture: %s\n", uri);
  return nullptr;
}

FGameTexture *FGLTFModel::LoadTextureFromBufferView(size_t bufferViewIndex,
                                                    GLTFLoadResult &result) {
  if (bufferViewIndex >= asset->bufferViews.size()) {
    result.SetError(GLTFError::TextureLoadFailure,
                    "Invalid buffer view index for texture");
    return nullptr;
  }

  const auto &bufferView = asset->bufferViews[bufferViewIndex];

  if (bufferView.bufferIndex >= buffers.Size()) {
    result.SetError(GLTFError::TextureLoadFailure,
                    "Buffer view references invalid buffer");
    return nullptr;
  }

  const auto &buffer = buffers[bufferView.bufferIndex];

  // Subtraction comparison keeps the check overflow-safe
  if (bufferView.byteOffset > buffer.Size() ||
      bufferView.byteLength > buffer.Size() - bufferView.byteOffset) {
    result.SetError(GLTFError::TextureLoadFailure,
                    "Buffer view exceeds buffer bounds");
    return nullptr;
  }

  return LoadTextureFromMemory(buffer.Data() + bufferView.byteOffset,
                               bufferView.byteLength, result);
}

FGameTexture *FGLTFModel::LoadTextureFromMemory(const uint8_t *data,
                                                size_t length,
                                                GLTFLoadResult &result) {
  if (!data || length == 0) {
    result.SetError(GLTFError::TextureLoadFailure, "Empty embedded image");
    return nullptr;
  }

  int w = 0, h = 0, channels = 0;
  stbi_uc *rgba = stbi_load_from_memory(data, (int)length, &w, &h,
                                        &channels, STBI_rgb_alpha);
  if (!rgba || w <= 0 || h <= 0) {
    result.SetError(GLTFError::TextureLoadFailure,
                    "Unsupported embedded image format");
    return nullptr;
  }

  FImageSource *imgSrc = new FGLTFMemoryImage(rgba, w, h);
  stbi_image_free(rgba);

  static int embeddedCounter = 0;
  FString texName;
  texName.Format("GLTFEmbedded_%d", embeddedCounter++);

  FImageTexture *tex = new FImageTexture(imgSrc, 0);
  FGameTexture *gameTex = new FGameTexture(tex, texName.GetChars());
  TexMan.AddGameTexture(gameTex);
  gameTex->GetTexture(); // generate pixels now, like the URI path does

  return gameTex;
}

bool FGLTFModel::SampleAnimation(const GLTFAnimation &anim, float time,
                                 TArray<TRS> &outBoneTransforms) {
  if (scene.skins.Size() == 0)
    return false;

  const GLTFSkin &skin = scene.skins[0];
  outBoneTransforms.Resize(skin.jointIndices.Size());

  // Initialize with base pose
  for (int i = 0; i < skin.jointIndices.Size(); ++i) {
    outBoneTransforms[i] = basePose[i];
  }

  // Apply animation channels
  for (const auto &channel : anim.channels) {
    if (channel.targetNodeIndex < 0 || channel.samplerIndex < 0)
      continue;

    if (channel.samplerIndex >= anim.samplers.Size())
      continue;

    const auto &sampler = anim.samplers[channel.samplerIndex];

    // Find the bone index for this node
    int boneIndex = -1;
    for (int i = 0; i < skin.jointIndices.Size(); ++i) {
      if (skin.jointIndices[i] == channel.targetNodeIndex) {
        boneIndex = i;
        break;
      }
    }

    if (boneIndex < 0)
      continue;

    // Sample the animation data (simplified linear interpolation)
    TArray<float> times;
    if (!ReadAccessorTyped(sampler.inputAccessorIndex, times))
      continue;
    if (times.Size() == 0)
      continue;

    // Find keyframe indices
    int keyframe = 0;
    float t = 0.0f;
    if (times.Size() > 1) {
      if (time <= times[0]) {
        keyframe = 0;
      } else if (time >= times.Last()) {
        keyframe = times.Size() - 2;
        t = 1.0f;
      } else {
        for (int i = 0; i < times.Size() - 1; ++i) {
          if (time >= times[i] && time < times[i + 1]) {
            keyframe = i;
            float duration = times[keyframe + 1] - times[keyframe];
            if (duration > 0.0f) {
              t = (time - times[keyframe]) / duration;
            }
            break;
          }
        }
      }
    }

    const bool stepInterpolation =
        sampler.interpolation.CompareNoCase("STEP") == 0;
    const bool cubicInterpolation =
        sampler.interpolation.CompareNoCase("CUBICSPLINE") == 0;
    if (stepInterpolation) {
      t = 0.0f;
    }
    const int valueIndex = cubicInterpolation ? keyframe * 3 + 1 : keyframe;
    const int nextValueIndex =
        cubicInterpolation ? (keyframe + 1) * 3 + 1 : keyframe + 1;

    // Apply the animated value based on target path
    if (channel.targetPath.CompareNoCase("translation") == 0) {
      TArray<FVector3> values;
      if (ReadAccessorTyped(sampler.outputAccessorIndex, values) &&
          valueIndex < values.Size()) {
        const int nextIndex =
            nextValueIndex < values.Size() ? nextValueIndex : valueIndex;
        outBoneTransforms[boneIndex].translation =
            values[valueIndex] * (1.0f - t) + values[nextIndex] * t;
      }
    } else if (channel.targetPath.CompareNoCase("rotation") == 0) {
      TArray<FQuaternion> values;
      if (ReadAccessorTyped(sampler.outputAccessorIndex, values) &&
          valueIndex < values.Size()) {
        const int nextIndex =
            nextValueIndex < values.Size() ? nextValueIndex : valueIndex;
        outBoneTransforms[boneIndex].rotation = InterpolateQuat(
            values[valueIndex], values[nextIndex], t, 1.0f - t);
      }
    } else if (channel.targetPath.CompareNoCase("scale") == 0) {
      TArray<FVector3> values;
      if (ReadAccessorTyped(sampler.outputAccessorIndex, values) &&
          valueIndex < values.Size()) {
        const int nextIndex =
            nextValueIndex < values.Size() ? nextValueIndex : valueIndex;
        outBoneTransforms[boneIndex].scaling =
            values[valueIndex] * (1.0f - t) + values[nextIndex] * t;
      }
    }
  }

  return true;
}

const TArray<TRS> *FGLTFModel::AttachAnimationData() {
  return sampledAnimationFrames.Size() > 0 ? &sampledAnimationFrames : nullptr;
}

static TRS GetSampledGLTFBone(const TArray<TRS> &frames, int boneCount,
                              int frame1, int frame2, float inter, int bone,
                              const TRS &fallback) {
  if (boneCount <= 0 || frames.Size() < boneCount || bone < 0 ||
      bone >= boneCount) {
    return fallback;
  }

  const int frameCount = frames.Size() / boneCount;
  if (frameCount <= 0) {
    return fallback;
  }

  frame1 = clamp(frame1, 0, frameCount - 1);
  if (inter < 0.0f || frame2 < 0) {
    return frames[frame1 * boneCount + bone];
  }

  frame2 = clamp(frame2, 0, frameCount - 1);
  return InterpolateGLTFBone(frames[frame1 * boneCount + bone],
                             frames[frame2 * boneCount + bone], inter);
}

ModelAnimFrame FGLTFModel::PrecalculateFrame(
    const ModelAnimFrame &from, const ModelAnimFrameInterp &to, float inter,
    const TArray<TRS> *animationData) {
  ModelAnimFramePrecalculatedIQM out;
  const int boneCount = GetBoneCount();
  out.precalcBones.Resize(boneCount);

  const TArray<TRS> &frames =
      animationData != nullptr ? *animationData : sampledAnimationFrames;
  if (boneCount <= 0 || frames.Size() < boneCount) {
    return out;
  }

  for (int bone = 0; bone < boneCount; ++bone) {
    const TRS fallback =
        bone < basePose.Size() ? basePose[bone] : MakeIdentityTRS();

    TRS prev = fallback;
    if (std::holds_alternative<ModelAnimFramePrecalculatedIQM>(from)) {
      const auto &precalc = std::get<ModelAnimFramePrecalculatedIQM>(from);
      if (bone < precalc.precalcBones.Size()) {
        prev = precalc.precalcBones[bone];
      }
    } else if (std::holds_alternative<ModelAnimFrameInterp>(from)) {
      const auto &fromInterp = std::get<ModelAnimFrameInterp>(from);
      prev = GetSampledGLTFBone(frames, boneCount, fromInterp.frame1,
                                fromInterp.frame2, fromInterp.inter, bone,
                                fallback);
    } else {
      prev = GetSampledGLTFBone(frames, boneCount, to.frame1, to.frame1,
                                -1.0f, bone, fallback);
    }

    const TRS next =
        GetSampledGLTFBone(frames, boneCount, to.frame1, to.frame2, to.inter,
                           bone, fallback);

    out.precalcBones[bone] =
        inter < 0.0f ? next : InterpolateGLTFBone(prev, next, inter);
  }

  return out;
}

const TArray<VSMatrix> *FGLTFModel::CalculateBones(
    const ModelAnimFrame &from, const ModelAnimFrameInterp &to, float inter,
    const TArray<TRS> *animationData, TArray<BoneOverride> *in, BoneInfo *out,
    double time) {
  if (!hasSkinning || scene.skins.Size() == 0) {
    return nullptr;
  }

  const int boneCount = GetBoneCount();
  if (boneCount <= 0) {
    return nullptr;
  }

  ModelAnimFrame frame = PrecalculateFrame(from, to, inter, animationData);
  if (!std::holds_alternative<ModelAnimFramePrecalculatedIQM>(frame)) {
    return nullptr;
  }

  const auto &precalc = std::get<ModelAnimFramePrecalculatedIQM>(frame);
  if (precalc.precalcBones.Size() != boneCount) {
    return nullptr;
  }

  TArray<TRS> bones = precalc.precalcBones;
  TArray<TRS> bonesWithOverride = bones;

  if (in && in->Size() != static_cast<unsigned int>(boneCount)) {
    in = nullptr;
  }

  if (in) {
    for (int bone = 0; bone < boneCount; ++bone) {
      (*in)[bone].Modify(bonesWithOverride[bone], time);
    }
  }

  if (out) {
    out->bones = bones;
    out->bones_with_override = bonesWithOverride;
  }

  const GLTFSkin &skin = scene.skins[0];

  auto buildMatrices = [&](const TArray<TRS> &localBones,
                           TArray<VSMatrix> &finalMatrices) {
    TArray<VSMatrix> globalMatrices;
    TArray<uint8_t> visited;
    globalMatrices.Resize(boneCount);
    visited.Resize(boneCount);
    std::fill(visited.begin(), visited.end(), 0);

    std::function<void(int)> buildBone = [&](int bone) {
      if (bone < 0 || bone >= boneCount || visited[bone]) {
        return;
      }

      visited[bone] = 1;
      const int nodeIndex = skin.jointIndices[bone];
      VSMatrix local = BuildMatrixFromTRS(localBones[bone]);
      int parentBone = -1;

      if (nodeIndex >= 0 && nodeIndex < scene.nodes.Size()) {
        const int parentNode = scene.nodes[nodeIndex].parentIndex;
        if (parentNode >= 0 && parentNode < scene.nodes.Size()) {
          parentBone = scene.nodes[parentNode].boneIndex;
          if (parentBone >= 0 && parentBone < boneCount) {
            buildBone(parentBone);
            globalMatrices[bone] = globalMatrices[parentBone];
          } else {
            globalMatrices[bone] = scene.nodes[parentNode].globalMatrix;
          }
        } else {
          // Armature root bone: keep the scene-axis conversion
          globalMatrices[bone] = GLTFSceneConversion();
        }
      } else {
        globalMatrices[bone] = GLTFSceneConversion();
      }

      globalMatrices[bone].multMatrix(local);
    };

    finalMatrices.Resize(boneCount);
    for (int bone = 0; bone < boneCount; ++bone) {
      buildBone(bone);
      finalMatrices[bone] = globalMatrices[bone];
      if (bone < skin.inverseBindMatrices.Size()) {
        finalMatrices[bone].multMatrix(skin.inverseBindMatrices[bone]);
      }
    }
  };

  if (out) {
    buildMatrices(bones, out->positions);
    buildMatrices(bonesWithOverride, out->positions_with_override);
    boneMatrices = out->positions_with_override;
  } else {
    buildMatrices(bonesWithOverride, boneMatrices);
  }

  return &boneMatrices;
}

//===========================================================================
//
// Template Specializations
//
//===========================================================================

//===========================================================================
//
// Enhanced Animation and Bone Interface Implementation
//
//===========================================================================

int FGLTFModel::FindAnimation(const char *name) const {
  if (!name || !*name)
    return -1;

  const int *animation = namedAnimations.CheckKey(FName(name, true));
  if (animation) {
    return *animation;
  }

  return -1;
}

int FGLTFModel::FindFirstFrame(FName name) {
  int *animation = namedAnimations.CheckKey(name);
  return animation ? modelAnimations[*animation].firstFrame : FErr_NotFound;
}

int FGLTFModel::FindLastFrame(FName name) {
  int *animation = namedAnimations.CheckKey(name);
  return animation ? modelAnimations[*animation].lastFrame : FErr_NotFound;
}

double FGLTFModel::FindFramerate(FName name) {
  int *animation = namedAnimations.CheckKey(name);
  return animation ? modelAnimations[*animation].framerate : FErr_NotFound;
}

int FGLTFModel::GetBoneCount() const {
  if (!hasSkinning || scene.skins.Size() == 0) {
    return 0;
  }
  return scene.skins[0].jointIndices.Size();
}

const char *FGLTFModel::GetBoneName(int index) const {
  if (!hasSkinning || scene.skins.Size() == 0) {
    return "";
  }

  const GLTFSkin &skin = scene.skins[0];
  if (index < 0 || index >= skin.jointIndices.Size()) {
    return "";
  }

  int nodeIndex = skin.jointIndices[index];
  if (nodeIndex < 0 || nodeIndex >= scene.nodes.Size()) {
    return "";
  }

  return scene.nodes[nodeIndex].name.GetChars();
}

int FGLTFModel::FindBone(const char *name) const {
  if (!name || !*name || !hasSkinning || scene.skins.Size() == 0) {
    return -1;
  }

  const GLTFSkin &skin = scene.skins[0];
  for (int i = 0; i < skin.jointIndices.Size(); ++i) {
    int nodeIndex = skin.jointIndices[i];
    if (nodeIndex >= 0 && nodeIndex < scene.nodes.Size()) {
      if (scene.nodes[nodeIndex].name.CompareNoCase(name) == 0) {
        return i;
      }
    }
  }

  return -1;
}

int FGLTFModel::FindJoint(FName name) {
  int *bone = namedBones.CheckKey(name);
  return bone ? *bone : -1;
}

int FGLTFModel::GetJointParent(int joint) {
  if (!hasSkinning || scene.skins.Size() == 0 || joint < 0 ||
      joint >= scene.skins[0].jointIndices.Size()) {
    return -1;
  }

  const int nodeIndex = scene.skins[0].jointIndices[joint];
  if (nodeIndex < 0 || nodeIndex >= scene.nodes.Size()) {
    return -1;
  }

  const int parentNode = scene.nodes[nodeIndex].parentIndex;
  if (parentNode < 0 || parentNode >= scene.nodes.Size()) {
    return -1;
  }

  return scene.nodes[parentNode].boneIndex;
}

FName FGLTFModel::GetJointName(int joint) {
  if (!hasSkinning || scene.skins.Size() == 0 || joint < 0 ||
      joint >= scene.skins[0].jointIndices.Size()) {
    return NAME_None;
  }

  const int nodeIndex = scene.skins[0].jointIndices[joint];
  if (nodeIndex < 0 || nodeIndex >= scene.nodes.Size() ||
      scene.nodes[nodeIndex].name.IsEmpty()) {
    return NAME_None;
  }

  return FName(scene.nodes[nodeIndex].name);
}

FQuaternion FGLTFModel::GetJointRotation(int joint) {
  return (joint >= 0 && joint < basePose.Size())
             ? basePose[joint].rotation
             : FQuaternion(0.0f, 0.0f, 0.0f, 1.0f);
}

FVector3 FGLTFModel::GetJointPosition(int joint) {
  return (joint >= 0 && joint < basePose.Size()) ? basePose[joint].translation
                                                 : FVector3(0.0f, 0.0f, 0.0f);
}

TRS FGLTFModel::GetJointBaseTRS(int joint) {
  return (joint >= 0 && joint < basePose.Size()) ? basePose[joint] : TRS{};
}

TRS FGLTFModel::GetJointPose(int joint, int frame) {
  const int boneCount = GetBoneCount();
  if (joint < 0 || joint >= boneCount || boneCount <= 0 ||
      sampledAnimationFrames.Size() < boneCount) {
    return TRS{};
  }

  const int frameCount = sampledAnimationFrames.Size() / boneCount;
  if (frame < 0 || frame >= frameCount) {
    return TRS{};
  }

  return sampledAnimationFrames[frame * boneCount + joint];
}

int FGLTFModel::NumFrames() {
  const int boneCount = GetBoneCount();
  return boneCount > 0 ? sampledAnimationFrames.Size() / boneCount : 0;
}

void FGLTFModel::GetRootJoints(TArray<int> &out) { out = rootBones; }

void FGLTFModel::GetJointChildren(int joint, TArray<int> &out) {
  out.Clear();

  if (!hasSkinning || scene.skins.Size() == 0 || joint < 0 ||
      joint >= scene.skins[0].jointIndices.Size()) {
    return;
  }

  const int nodeIndex = scene.skins[0].jointIndices[joint];
  if (nodeIndex < 0 || nodeIndex >= scene.nodes.Size()) {
    return;
  }

  for (int childNode : scene.nodes[nodeIndex].childIndices) {
    if (childNode >= 0 && childNode < scene.nodes.Size() &&
        scene.nodes[childNode].boneIndex >= 0) {
      out.Push(scene.nodes[childNode].boneIndex);
    }
  }
}

bool FGLTFModel::GetBoneTransform(int boneIndex, TRS &outTransform) const {
  if (!hasSkinning || scene.skins.Size() == 0) {
    return false;
  }

  const GLTFSkin &skin = scene.skins[0];
  if (boneIndex < 0 || boneIndex >= skin.jointIndices.Size()) {
    return false;
  }

  if (boneIndex < basePose.Size()) {
    outTransform = basePose[boneIndex];
    return true;
  }

  return false;
}

bool FGLTFModel::ProcessMeshes() {
  // Don't use Resize() here - we'll Push() each loaded primitive
  // (A glTF mesh can have multiple primitives, each becomes a separate
  // GLTFMesh)

  meshPrimStart.Resize(asset->meshes.size());
  meshPrimCount.Resize(asset->meshes.size());

  for (size_t meshIndex = 0; meshIndex < asset->meshes.size(); ++meshIndex) {
    const auto &gltfMesh = asset->meshes[meshIndex];

    meshPrimStart[meshIndex] = (int)scene.meshes.Size();
    meshPrimCount[meshIndex] = 0;

    for (size_t primIndex = 0; primIndex < gltfMesh.primitives.size();
         ++primIndex) {
      GLTFMesh mesh;
      mesh.name = gltfMesh.name.c_str();

      const auto &primitive = gltfMesh.primitives[primIndex];

      if (!LoadMeshPrimitive(primitive, mesh, lastError)) {
        Printf("Failed to load mesh primitive %zu of mesh %zu\n", primIndex,
               meshIndex);
        continue;
      }

      meshPrimCount[meshIndex]++;

      if (primitive.materialIndex.has_value()) {
        LoadMaterial(primitive.materialIndex.value(), mesh.material, lastError);
        mesh.materialIndex = primitive.materialIndex.value();
      }

      scene.meshes.Push(mesh);
    }
  }

  // Mark model as having surfaces if there are multiple meshes
  // This enables MODELDEF SurfaceSkin directive support
  hasSurfaces = scene.meshes.Size() > 1;

  return true;
}

bool FGLTFModel::LoadMeshPrimitive(const fastgltf::Primitive &primitive,
                                   GLTFMesh &mesh, GLTFLoadResult &result) {
  result.Clear();

  auto positionIt = primitive.attributes.find("POSITION");
  if (positionIt == primitive.attributes.end()) {
    Printf("Error: Mesh primitive missing POSITION attribute\n");
    result.SetError(GLTFError::MissingRequiredData,
                    "Mesh primitive missing POSITION attribute");
    return false;
  }

  TArray<FVector3> positions;
  if (!ReadAccessorTyped(static_cast<int>(positionIt->second), positions)) {
    result.SetError(GLTFError::ValidationFailure,
                    "Failed to read POSITION accessor");
    return false;
  }

  int vertexCount = positions.Size();
  mesh.vertices.Resize(vertexCount);

  for (int i = 0; i < vertexCount; ++i) {
    mesh.vertices[i].x = positions[i].X;
    mesh.vertices[i].y = positions[i].Y;
    mesh.vertices[i].z = positions[i].Z;
  }

  auto normalIt = primitive.attributes.find("NORMAL");
  if (normalIt != primitive.attributes.end()) {
    TArray<FVector3> normals;
    if (ReadAccessorTyped(static_cast<int>(normalIt->second), normals) &&
        normals.Size() == vertexCount) {
      for (int i = 0; i < vertexCount; ++i) {
        mesh.vertices[i].SetNormal(normals[i].X, normals[i].Y, normals[i].Z);
      }
    }
  }

  auto texCoordIt = primitive.attributes.find("TEXCOORD_0");
  if (texCoordIt != primitive.attributes.end()) {
    TArray<FVector2> texCoords;
    if (ReadAccessorTyped(static_cast<int>(texCoordIt->second), texCoords) &&
        texCoords.Size() == vertexCount) {
      for (int i = 0; i < vertexCount; ++i) {
        mesh.vertices[i].u = texCoords[i].X;
        mesh.vertices[i].v = texCoords[i].Y;
      }
    }
  }

  auto tangentIt = primitive.attributes.find("TANGENT");
  if (tangentIt != primitive.attributes.end()) {
    TArray<FVector4> tangents;
    if (ReadAccessorTyped(static_cast<int>(tangentIt->second), tangents) &&
        tangents.Size() == vertexCount) {
      for (int i = 0; i < vertexCount; ++i) {
        mesh.vertices[i].tangent = tangents[i];
      }
    }
  }

  auto colorIt = primitive.attributes.find("COLOR_0");
  if (colorIt != primitive.attributes.end()) {
    TArray<FVector4> colors;
    if (ReadAccessorTyped(static_cast<int>(colorIt->second), colors) &&
        colors.Size() == vertexCount) {
      for (int i = 0; i < vertexCount; ++i) {
        mesh.vertices[i].color0 = colors[i];
      }
    }
  }

  auto jointsIt = primitive.attributes.find("JOINTS_0");
  auto weightsIt = primitive.attributes.find("WEIGHTS_0");

  if (jointsIt != primitive.attributes.end() &&
      weightsIt != primitive.attributes.end()) {
    hasSkinning = true;

    TArray<std::array<uint32_t, 4>> joints;
    TArray<FVector4> weights;

    if (ReadAccessorVec4UInt(static_cast<int>(jointsIt->second), joints) &&
        ReadAccessorTyped(static_cast<int>(weightsIt->second), weights) &&
        joints.Size() == vertexCount && weights.Size() == vertexCount) {

      for (int i = 0; i < vertexCount; ++i) {
        mesh.vertices[i].boneIndices[0] =
            static_cast<uint8_t>(std::min<uint32_t>(joints[i][0], 255));
        mesh.vertices[i].boneIndices[1] =
            static_cast<uint8_t>(std::min<uint32_t>(joints[i][1], 255));
        mesh.vertices[i].boneIndices[2] =
            static_cast<uint8_t>(std::min<uint32_t>(joints[i][2], 255));
        mesh.vertices[i].boneIndices[3] =
            static_cast<uint8_t>(std::min<uint32_t>(joints[i][3], 255));

        mesh.vertices[i].boneWeights[0] = weights[i].X;
        mesh.vertices[i].boneWeights[1] = weights[i].Y;
        mesh.vertices[i].boneWeights[2] = weights[i].Z;
        mesh.vertices[i].boneWeights[3] = weights[i].W;
      }
    }
  }

  if (primitive.indicesAccessor.has_value()) {
    TArray<uint32_t> indices;
    if (ReadAccessorTyped(static_cast<int>(primitive.indicesAccessor.value()),
                          indices)) {
      mesh.indices.Resize(indices.Size());
      for (int i = 0; i < indices.Size(); ++i) {
        mesh.indices[i] = indices[i];
      }
    }
  } else {
    mesh.indices.Resize(vertexCount);
    for (int i = 0; i < vertexCount; ++i) {
      mesh.indices[i] = i;
    }
  }

  return true;
}
bool FGLTFModel::GetBoneWorldTransform(int boneIndex,
                                       VSMatrix &outMatrix) const {
  if (!hasSkinning || scene.skins.Size() == 0) {
    return false;
  }

  const GLTFSkin &skin = scene.skins[0];
  if (boneIndex < 0 || boneIndex >= skin.jointIndices.Size()) {
    return false;
  }

  if (boneIndex < boneMatrices.Size()) {
    outMatrix = boneMatrices[boneIndex];
    return true;
  }

  return false;
}

bool FGLTFModel::ValidateAsset(GLTFLoadResult &result) const {
  result.Clear();

  if (!asset) {
    result.SetError(GLTFError::ValidationFailure, "Asset is null");
    return false;
  }

  if (asset->scenes.empty()) {
    result.SetError(GLTFError::MissingRequiredData, "No scenes in glTF file");
    return false;
  }

  if (asset->nodes.empty()) {
    result.SetError(GLTFError::MissingRequiredData, "No nodes in glTF file");
    return false;
  }

  return ValidateBuffers(result) && ValidateAccessors(result) &&
         ValidateNodes(result);
}

//===========================================================================
//
// LoadExternalBuffer
//
// Load external .bin files from PK3 archives using GZDoom's file system
//
//===========================================================================

bool FGLTFModel::LoadExternalBuffer(const char *uri, TArray<uint8_t> &outData) {
  if (!uri || !*uri) {
    Printf("glTF Error: LoadExternalBuffer called with null/empty URI\n");
    return false;
  }

  // Construct full path: basePath + uri
  FString fullPath;
  fullPath.Format("%s%s", basePath.GetChars(), uri);

  DPrintf(DMSG_NOTIFY, "glTF: Attempting to load external buffer from: %s\n",
          fullPath.GetChars());

  // Load from GZDoom's file system (handles PK3/ZIP archives)
  int lump = fileSystem.CheckNumForFullName(fullPath.GetChars());
  if (lump < 0) {
    Printf("glTF Error: External buffer not found: %s\n", fullPath.GetChars());
    return false;
  }

  auto length = fileSystem.FileLength(lump);
  if (length <= 0) {
    Printf("glTF Error: External buffer has invalid length: %s (%d bytes)\n",
           fullPath.GetChars(), (int)length);
    return false;
  }

  auto data = fileSystem.ReadFile(lump);
  if (!data.data()) {
    Printf("glTF Error: Failed to read external buffer: %s\n",
           fullPath.GetChars());
    return false;
  }

  // Copy to output buffer
  outData.Resize(length);
  memcpy(outData.Data(), data.data(), length);

  DPrintf(DMSG_NOTIFY,
          "glTF: Successfully loaded external buffer: %s (%d bytes)\n",
          fullPath.GetChars(), (int)length);
  return true;
}

#endif // NEODOOM_GLTF_SUPPORT
