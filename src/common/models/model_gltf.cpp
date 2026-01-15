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
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/util.hpp>
#include <filesystem>
#include <optional>

namespace {
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

static TRS TRSFromMatrix(const fastgltf::Node::TransformMatrix &matrix) {
  TRS transform = MakeIdentityTRS();
  if (matrix.size() >= 16) {
    transform.translation = FVector3(matrix[12], matrix[13], matrix[14]);
  }
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
  Printf("FGLTFModel::Load called! Path: %s, Length: %d\n",
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
  // from PK3
  auto options = fastgltf::Options::LoadGLBBuffers |
                 fastgltf::Options::DecomposeNodeMatrices;

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
  auto options = fastgltf::Options::LoadGLBBuffers |
                 fastgltf::Options::DecomposeNodeMatrices;

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

  if (hasSkinning) {
    BuildBoneHierarchy();
  }

  return true;
}

bool FGLTFModel::ProcessBuffers() {
  Printf("ProcessBuffers: asset->buffers.size()=%zu\n", asset->buffers.size());
  buffers.Resize(asset->buffers.size());
  Printf("ProcessBuffers: buffers resized to %d\n", buffers.Size());

  for (size_t i = 0; i < asset->buffers.size(); ++i) {
    const auto &buffer = asset->buffers[i];
    Printf("ProcessBuffers: Processing buffer %zu, byteLength=%zu\n", i,
           buffer.byteLength);

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

    Printf("*** glTF LoadMaterial [%d]: baseColorFactor = (%.3f, %.3f, %.3f, "
           "%.3f)\n",
           materialIndex, material.baseColorFactor.X,
           material.baseColorFactor.Y, material.baseColorFactor.Z,
           material.baseColorFactor.W);

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

  if (stride != sizeof(T)) {
    Printf("Warning: Accessor stride mismatch. Expected %zu, got %d\n",
           sizeof(T), stride);
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

  const auto &bufferView = asset->bufferViews[accessor.bufferViewIndex.value()];

  outCount = accessor.count;
  outStride =
      fastgltf::getElementByteSize(accessor.type, accessor.componentType);

  size_t totalSize = outCount * outStride;
  outData.Resize(totalSize);

  const uint8_t *srcData = buffers[bufferView.bufferIndex].Data() +
                           bufferView.byteOffset + accessor.byteOffset;

  if (bufferView.byteStride.has_value() &&
      bufferView.byteStride.value() != outStride) {
    // Interleaved data - need to deinterleave
    int stride = bufferView.byteStride.value();
    for (int i = 0; i < outCount; ++i) {
      memcpy(outData.Data() + i * outStride, srcData + i * stride, outStride);
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
                            node.transform = TRSFromMatrix(matrix);
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

    // Copy joint indices
    skin.jointIndices.Resize(gltfSkin.joints.size());
    for (size_t j = 0; j < gltfSkin.joints.size(); ++j) {
      const int jointIndex = static_cast<int>(gltfSkin.joints[j]);
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

    if (gltfSkin.skeleton.has_value()) {
      skin.skeletonRootIndex = gltfSkin.skeleton.value();
    }
  }

  return true;
}

bool FGLTFModel::ProcessAnimations() {
  scene.animations.Resize(asset->animations.size());
  modelAnimations.Resize(asset->animations.size());

  bool success = true;

  for (size_t i = 0; i < asset->animations.size(); ++i) {
    if (!ConvertGLTFAnimation(asset->animations[i], scene.animations[i],
                              lastError)) {
      PrintErrorDetails(lastError);
      success = false;
      continue;
    }

    // Create corresponding ModelAnim
    ModelAnim &modelAnim = modelAnimations[i];
    modelAnim.firstFrame = 0;
    modelAnim.lastFrame =
        static_cast<int>(scene.animations[i].duration * 30.0f); // Assume 30 FPS
    modelAnim.loopFrame = 0;
    modelAnim.framerate = 30.0f;
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

  outAnim.duration = maxTime;
  return success;
}

void FGLTFModel::ComputeNodeTransforms() {
  // Compute local matrices
  for (auto &node : scene.nodes) {
    node.localMatrix = BuildMatrixFromTRS(node.transform);
  }

  // Compute global matrices via depth-first traversal
  std::function<void(int, const VSMatrix &)> computeGlobal =
      [&](int nodeIndex, const VSMatrix &parentMatrix) {
        if (nodeIndex < 0 || nodeIndex >= scene.nodes.Size())
          return;

        GLTFNode &node = scene.nodes[nodeIndex];
        VSMatrix combined = parentMatrix;
        combined.multMatrix(node.localMatrix);
        node.globalMatrix = combined;

        for (int childIndex : node.childIndices) {
          computeGlobal(childIndex, node.globalMatrix);
        }
      };

  VSMatrix identity;
  identity.loadIdentity();

  for (int rootIndex : scene.rootNodeIndices) {
    computeGlobal(rootIndex, identity);
  }
}

void FGLTFModel::BuildBoneHierarchy() {
  if (scene.skins.Size() == 0)
    return;

  // For now, use the first skin
  const GLTFSkin &skin = scene.skins[0];

  basePose.Resize(skin.jointIndices.Size());
  boneMatrices.Resize(skin.jointIndices.Size());

  for (int i = 0; i < skin.jointIndices.Size(); ++i) {
    int nodeIndex = skin.jointIndices[i];
    basePose[i] = scene.nodes[nodeIndex].transform;
    boneMatrices[i] = scene.nodes[nodeIndex].globalMatrix;

    if (i < skin.inverseBindMatrices.Size()) {
      VSMatrix combined = boneMatrices[i];
      combined.multMatrix(skin.inverseBindMatrices[i]);
      boneMatrices[i] = combined;
    }
  }
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
      // Not yet implemented: create texture from memory buffer
      result.SetError(GLTFError::TextureLoadFailure,
                      "Vector textures not yet implemented");
      return nullptr;
    } else if (std::holds_alternative<fastgltf::sources::ByteView>(
                   image.data)) {
      const auto &bv = std::get<fastgltf::sources::ByteView>(image.data);
      // Not yet implemented: create texture from memory view
      result.SetError(GLTFError::TextureLoadFailure,
                      "ByteView textures not yet implemented");
      return nullptr;
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

  // Handle data URIs
  if (strncmp(uri, "data:", 5) == 0) {
    result.SetError(GLTFError::TextureLoadFailure,
                    "Data URIs not yet supported");
    return nullptr;
  }

  // Handle relative paths - need to resolve relative to model path
  if (uri[0] != '/' && strstr(uri, "://") == nullptr) {
    // This is a relative path
    texturePath.Format("models/%s", uri); // Assume models directory
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

  if (bufferView.byteOffset + bufferView.byteLength > buffer.Size()) {
    result.SetError(GLTFError::TextureLoadFailure,
                    "Buffer view exceeds buffer bounds");
    return nullptr;
  }

  // Create a temporary file or use memory texture loading
  // For now, this is a placeholder - would need GZDoom memory texture support
  result.SetError(GLTFError::TextureLoadFailure,
                  "Buffer view textures not yet implemented");
  return nullptr;
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

    // Find keyframe indices
    int keyframe = 0;
    for (int i = 0; i < times.Size() - 1; ++i) {
      if (time >= times[i] && time < times[i + 1]) {
        keyframe = i;
        break;
      }
    }

    float t = 0.0f;
    if (keyframe < times.Size() - 1) {
      float duration = times[keyframe + 1] - times[keyframe];
      if (duration > 0.0f) {
        t = (time - times[keyframe]) / duration;
      }
    }

    // Apply the animated value based on target path
    if (channel.targetPath.CompareNoCase("translation") == 0) {
      TArray<FVector3> values;
      if (ReadAccessorTyped(sampler.outputAccessorIndex, values) &&
          keyframe < values.Size() - 1) {
        outBoneTransforms[boneIndex].translation =
            values[keyframe] * (1.0f - t) + values[keyframe + 1] * t;
      }
    } else if (channel.targetPath.CompareNoCase("rotation") == 0) {
      TArray<FQuaternion> values;
      if (ReadAccessorTyped(sampler.outputAccessorIndex, values) &&
          keyframe < values.Size() - 1) {
        outBoneTransforms[boneIndex].rotation = InterpolateQuat(
            values[keyframe], values[keyframe + 1], t, 1.0f - t);
      }
    } else if (channel.targetPath.CompareNoCase("scale") == 0) {
      TArray<FVector3> values;
      if (ReadAccessorTyped(sampler.outputAccessorIndex, values) &&
          keyframe < values.Size() - 1) {
        outBoneTransforms[boneIndex].scaling =
            values[keyframe] * (1.0f - t) + values[keyframe + 1] * t;
      }
    }
  }

  return true;
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

  for (int i = 0; i < scene.animations.Size(); ++i) {
    if (scene.animations[i].name.CompareNoCase(name) == 0) {
      return i;
    }
  }

  return -1;
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

  Printf("ProcessMeshes: asset has %zu glTF meshes\n", asset->meshes.size());

  for (size_t meshIndex = 0; meshIndex < asset->meshes.size(); ++meshIndex) {
    const auto &gltfMesh = asset->meshes[meshIndex];
    Printf("  glTF mesh %zu has %zu primitives\n", meshIndex,
           gltfMesh.primitives.size());

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

      Printf("  Loaded primitive %zu: %d vertices, %d indices\n", primIndex,
             mesh.vertices.Size(), mesh.indices.Size());

      if (primitive.materialIndex.has_value()) {
        LoadMaterial(primitive.materialIndex.value(), mesh.material, lastError);
        mesh.materialIndex = primitive.materialIndex.value();
      }

      scene.meshes.Push(mesh);
    }
  }

  Printf("ProcessMeshes: Created %d GLTFMesh objects\n", scene.meshes.Size());

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

    TArray<uint32_t> joints;
    TArray<FVector4> weights;

    if (ReadAccessorTyped(static_cast<int>(jointsIt->second), joints) &&
        ReadAccessorTyped(static_cast<int>(weightsIt->second), weights) &&
        joints.Size() == vertexCount && weights.Size() == vertexCount) {

      for (int i = 0; i < vertexCount; ++i) {
        mesh.vertices[i].boneIndices[0] =
            static_cast<uint8_t>(joints[i] & 0xFF);
        mesh.vertices[i].boneIndices[1] =
            static_cast<uint8_t>((joints[i] >> 8) & 0xFF);
        mesh.vertices[i].boneIndices[2] =
            static_cast<uint8_t>((joints[i] >> 16) & 0xFF);
        mesh.vertices[i].boneIndices[3] =
            static_cast<uint8_t>((joints[i] >> 24) & 0xFF);

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
