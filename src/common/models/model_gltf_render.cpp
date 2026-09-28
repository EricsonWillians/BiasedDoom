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
** model_gltf_render.cpp
**
** Vertex buffer and rendering implementation for glTF models
**
**/

#include "model_gltf.h"

#ifdef NEODOOM_GLTF_SUPPORT

#include "bitmap.h"
#include "doomdef.h"
#include "gametexture.h"
#include "hw_bonebuffer.h"
#include "hw_material_pbr.h"
#include "hw_renderstate.h"
#include "i_time.h"
#include "image.h"
#include "modelrenderer.h"
#include "printf.h"
#include "texturemanager.h"
#include "textures.h"
#include "v_video.h"

//===========================================================================
//
// Colored Image Source for Materials Without Textures
//
//===========================================================================

class FGLTFColoredImage : public FImageSource {
  PalEntry color;

public:
  FGLTFColoredImage(int r, int g, int b, int a = 255)
      : FImageSource(-1) // -1 = no lump, procedurally generated
  {
    Width = 8;
    Height = 8;
    color = PalEntry(a, r, g, b);

    // CRITICAL: Mark as true-color image, not using game palette
    bUseGamePalette = false;
    // Preserve the alpha channel when the color is translucent
    // (glTF BLEND materials without a texture, e.g. car glass at 25%
    // alpha) — otherwise the upload path treats the texture as opaque
    // and the material renders as a solid sheet.
    bMasked = a < 255;
    bTranslucent = 0;
  }

  PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override {
    // Create paletted pixels - not used for hardware rendering
    // but required for software fallback paths
    PalettedPixels pixels(Width * Height);

    // For software rendering, use a simple palette index approximation
    // This is not perfect but functional for fallback rendering
    uint8_t paletteIndex = 255; // White as fallback

    // Simple color mapping to DOOM palette ranges
    if (color.r > 192 && color.g < 64 && color.b < 64) {
      paletteIndex = 176; // Red-ish range
    } else if (color.r < 64 && color.g > 192 && color.b < 64) {
      paletteIndex = 112; // Green-ish range
    } else if (color.r < 64 && color.g < 64 && color.b > 192) {
      paletteIndex = 200; // Blue-ish range
    }

    memset(pixels.Data(), paletteIndex, Width * Height);
    return pixels;
  }

  int CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override {
    // Create an 8x8 bitmap with the solid color
    // This is used for hardware rendering via texture upload
    bmp->Create(Width, Height);

    // Get direct access to pixel data (BGRA format, 4 bytes per pixel)
    uint8_t *pixels = bmp->GetPixels();

    // Fill entire bitmap with the solid color
    for (int i = 0; i < Width * Height; ++i) {
      int offset = i * 4;
      pixels[offset + 0] = color.r; // Red
      pixels[offset + 1] = color.g; // Green
      pixels[offset + 2] = color.b; // Blue
      pixels[offset + 3] = color.a; // Alpha
    }

    // Return 0 to indicate success (non-transparent texture)
    return 0;
  }
};

namespace {
// Helper function to create a colored texture for materials without textures
static FGameTexture *CreateColoredTexture(const FVector4 &color) {
  // Convert 0-1 range to 0-255
  int r = clamp(int(color.X * 255.0f), 0, 255);
  int g = clamp(int(color.Y * 255.0f), 0, 255);
  int b = clamp(int(color.Z * 255.0f), 0, 255);
  int a = clamp(int(color.W * 255.0f), 0, 255);

  // Create a unique name for this color
  FString texName;
  texName.Format("GLTFColor_%02X%02X%02X%02X", r, g, b, a);

  // Don't bother checking cache - just create a static texture once at startup
  // (The cache check was failing anyway, causing texture recreation every
  // frame)
  static TMap<uint32_t, FGameTexture *> colorTextureCache;
  uint32_t colorKey = (r << 24) | (g << 16) | (b << 8) | a;

  auto cached = colorTextureCache.CheckKey(colorKey);
  if (cached) {
    DPrintf(DMSG_NOTIFY, "Using cached color texture\n");
    return *cached;
  }

  DPrintf(DMSG_NOTIFY, "Creating NEW color texture\n");

  // Create new colored image source
  FImageSource *imgSrc = new FGLTFColoredImage(r, g, b, a);

  // Create texture from image source
  FImageTexture *tex = new FImageTexture(imgSrc, 0);

  // Wrap in FGameTexture
  FGameTexture *gameTex = new FGameTexture(tex, texName.GetChars());

  // Add to texture manager
  FTextureID texID = TexMan.AddGameTexture(gameTex);

  // FORCE the texture to generate its pixels NOW by calling GetTexture
  // This ensures CopyPixels is called and the texture has actual data
  auto hwTexture = gameTex->GetTexture();
  DPrintf(DMSG_NOTIFY, "Forced texture creation\n");

  // Cache it
  colorTextureCache[colorKey] = gameTex;

  DPrintf(DMSG_NOTIFY, "Added to TexMan and cached\n");
  DPrintf(DMSG_NOTIFY,
          "Created glTF colored texture '%s' (RGBA: %d,%d,%d,%d)\n",
          texName.GetChars(), r, g, b, a);

  return gameTex;
}

//===========================================================================
//
// Tinted Image Source for Materials With Texture AND baseColorFactor
//
// glTF multiplies the base color texture by baseColorFactor. Doom textures
// are used as-is, so bake the factor into a tinted copy of the texture.
//
//===========================================================================

class FGLTFTintedImage : public FImageSource {
  TArray<uint8_t> pixels; // BGRA

public:
  FGLTFTintedImage(FTexture *src, const FVector4 &factor)
      : FImageSource(-1) // -1 = no lump, procedurally generated
  {
    FBitmap bmp = src->GetBgraBitmap(nullptr, nullptr);
    Width = bmp.GetWidth();
    Height = bmp.GetHeight();

    const int fb = clamp(int(factor.Z * 255.0f), 0, 255);
    const int fg = clamp(int(factor.Y * 255.0f), 0, 255);
    const int fr = clamp(int(factor.X * 255.0f), 0, 255);
    const int fa = clamp(int(factor.W * 255.0f), 0, 255);

    pixels.Resize(Width * Height * 4);
    const uint8_t *srcPixels = bmp.GetPixels();

    for (int i = 0; i < Width * Height; ++i) {
      int offset = i * 4;
      pixels[offset + 0] = (srcPixels[offset + 0] * fb) / 255; // Blue
      pixels[offset + 1] = (srcPixels[offset + 1] * fg) / 255; // Green
      pixels[offset + 2] = (srcPixels[offset + 2] * fr) / 255; // Red
      pixels[offset + 3] = (srcPixels[offset + 3] * fa) / 255; // Alpha
    }

    bUseGamePalette = false;
    bMasked = fa < 255;
    bTranslucent = 0;
  }

  PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override {
    // Software fallback path only; approximate with white.
    PalettedPixels out(Width * Height);
    memset(out.Data(), 255, Width * Height);
    return out;
  }

  int CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override {
    bmp->Create(Width, Height);
    memcpy(bmp->GetPixels(), pixels.Data(), Width * Height * 4);
    return 0;
  }
};

// Helper: cached tinted variant of a base color texture
static FGameTexture *GetTintedTexture(FGameTexture *src,
                                      const FVector4 &factor) {
  if (!src || !src->GetTexture()) {
    return src;
  }

  const uint32_t factorKey =
      (uint32_t(clamp(int(factor.X * 255.0f), 0, 255)) << 24) |
      (uint32_t(clamp(int(factor.Y * 255.0f), 0, 255)) << 16) |
      (uint32_t(clamp(int(factor.Z * 255.0f), 0, 255)) << 8) |
      uint32_t(clamp(int(factor.W * 255.0f), 0, 255));

  struct TintedEntry {
    const FGameTexture *src;
    uint32_t factorKey;
    FGameTexture *out;
  };
  static TArray<TintedEntry> cache;

  for (const auto &entry : cache) {
    if (entry.src == src && entry.factorKey == factorKey) {
      return entry.out;
    }
  }

  static int tintedCounter = 0;
  FString texName;
  texName.Format("GLTFTinted_%d", tintedCounter++);

  FImageSource *imgSrc = new FGLTFTintedImage(src->GetTexture(), factor);
  FImageTexture *tex = new FImageTexture(imgSrc, 0);
  FGameTexture *gameTex = new FGameTexture(tex, texName.GetChars());
  TexMan.AddGameTexture(gameTex);
  gameTex->GetTexture(); // force pixel generation now

  cache.Push({src, factorKey, gameTex});
  return gameTex;
}

//===========================================================================
//
// PBR material layer support
//
// glTF materials carry metallic/roughness (combined: roughness in G,
// metallic in B), normal, ambient occlusion and emissive maps. The hardware
// renderer's PBR shader (SHADER_PBR) reads these from material layers, so
// extract the channels into standalone grayscale textures and attach them
// to a per-material clone of the base game texture. FMaterial then selects
// the PBR shader automatically (see hw_material.cpp).
//
//===========================================================================

// Grayscale image holding one extracted (and factor-scaled) texture channel.
class FGLTFChannelImage : public FImageSource {
  TArray<uint8_t> pixels; // BGRA, R=G=B=value, A=255

public:
  // mode: 0 = metallic (B channel * factor), 1 = roughness (G * factor),
  //       2 = ambient occlusion (R channel mixed with white by strength)
  FGLTFChannelImage(FTexture *src, int mode, float factor)
      : FImageSource(-1) // -1 = no lump, procedurally generated
  {
    FBitmap bmp = src->GetBgraBitmap(nullptr, nullptr);
    Width = bmp.GetWidth();
    Height = bmp.GetHeight();

    const int chan = (mode == 0) ? 0 : (mode == 1) ? 1 : 2; // BGRA offsets
    const int f = clamp(int(factor * 255.0f), 0, 255);

    pixels.Resize(Width * Height * 4);
    const uint8_t *srcPixels = bmp.GetPixels();

    for (int i = 0; i < Width * Height; ++i) {
      int offset = i * 4;
      int v = srcPixels[offset + chan];
      if (mode == 2) {
        // glTF occlusion: mix(white, texel, strength)
        v = 255 - ((255 - v) * f) / 255;
      } else {
        v = (v * f) / 255;
      }
      pixels[offset + 0] = pixels[offset + 1] = pixels[offset + 2] =
          uint8_t(v);
      pixels[offset + 3] = 255;
    }

    bUseGamePalette = false;
    bMasked = false;
    bTranslucent = 0;
  }

  PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override {
    // Software fallback path only; approximate with white.
    PalettedPixels out(Width * Height);
    memset(out.Data(), 255, Width * Height);
    return out;
  }

  int CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override {
    bmp->Create(Width, Height);
    memcpy(bmp->GetPixels(), pixels.Data(), Width * Height * 4);
    return 0;
  }
};

// Cached single-channel texture extracted from a glTF material texture.
static FGameTexture *GetPBRChannelTexture(FGameTexture *src, int mode,
                                          float factor) {
  const int f = clamp(int(factor * 255.0f), 0, 255);

  struct ChannelEntry {
    const FGameTexture *src;
    int mode;
    int factor;
    FGameTexture *out;
  };
  static TArray<ChannelEntry> cache;

  for (const auto &entry : cache) {
    if (entry.src == src && entry.mode == mode && entry.factor == f) {
      return entry.out;
    }
  }

  static int channelCounter = 0;
  FString texName;
  texName.Format("GLTFChannel_%d", channelCounter++);

  FImageSource *imgSrc = new FGLTFChannelImage(src->GetTexture(), mode, factor);
  FImageTexture *tex = new FImageTexture(imgSrc, 0);
  FGameTexture *gameTex = new FGameTexture(tex, texName.GetChars());
  TexMan.AddGameTexture(gameTex);
  gameTex->GetTexture(); // force pixel generation now

  cache.Push({src, mode, f, gameTex});
  return gameTex;
}

// Does this material carry explicit PBR content? (Fully default glTF
// materials keep the standard Doom shading; metallicFactor defaults to 1.0
// per spec, which would wrongly turn untextured surfaces into metal.)
static bool MaterialHasExplicitPBR(const PBRMaterialProperties &m) {
  // Debug/compat switch: GLTF_NO_PBR=1 forces standard shading everywhere.
  static const bool pbrDisabled = getenv("GLTF_NO_PBR") != nullptr;
  if (pbrDisabled) {
    return false;
  }
  return m.metallicRoughnessTextureIndex >= 0 || m.normalTextureIndex >= 0 ||
         m.occlusionTextureIndex >= 0 || m.emissiveTextureIndex >= 0 ||
         fabs(m.metallicFactor - 1.0f) > 0.001f ||
         fabs(m.roughnessFactor - 1.0f) > 0.001f;
}

} // namespace

// Attach PBR layers to the mesh skin. The base texture may be shared by
// several materials (or be a cached tinted copy), so clone the game texture
// (sharing the underlying FTexture pixel data) and put the layers on the
// clone. Returns the original texture for non-PBR materials.
FGameTexture *FGLTFModel::ApplyPBRMaterialLayers(
    FGameTexture *base, const PBRMaterialProperties &mat) {
  if (!base || !base->GetTexture() || !MaterialHasExplicitPBR(mat)) {
    return base;
  }

  const int mrTex = mat.metallicRoughnessTextureIndex;
  const int normalTex = mat.normalTextureIndex;
  const int aoTex = mat.occlusionTextureIndex;
  const int emisTex = mat.emissiveTextureIndex;
  const int metByte = clamp(int(mat.metallicFactor * 255.0f), 0, 255);
  const int roughByte = clamp(int(mat.roughnessFactor * 255.0f), 0, 255);
  const int occByte = clamp(int(mat.occlusionStrength * 255.0f), 0, 255);

  struct PBRLayerEntry {
    const FGameTexture *base;
    int mrTex, normalTex, aoTex, emisTex;
    int metByte, roughByte, occByte;
    FGameTexture *out;
  };
  static TArray<PBRLayerEntry> cache;

  for (const auto &entry : cache) {
    if (entry.base == base && entry.mrTex == mrTex &&
        entry.normalTex == normalTex && entry.aoTex == aoTex &&
        entry.emisTex == emisTex && entry.metByte == metByte &&
        entry.roughByte == roughByte && entry.occByte == occByte) {
      return entry.out;
    }
  }

  auto textureAt = [this](int index) -> FGameTexture * {
    return (index >= 0 && index < (int)textures.Size()) ? textures[index]
                                                        : nullptr;
  };

  // All four PBR layers are required for FMaterial to select SHADER_PBR;
  // fall back to neutral textures for maps the material does not provide.
  MaterialLayers lay = {};
  lay.Glossiness = -2000.0f;     // keep texture defaults
  lay.SpecularLevel = -2000.0f;

  lay.Normal = textureAt(normalTex);
  if (!lay.Normal) {
    // Flat tangent-space normal (128, 128, 255)
    lay.Normal = CreateColoredTexture(
        FVector4(128.0f / 255.0f, 128.0f / 255.0f, 1.0f, 1.0f));
  }

  FGameTexture *mrTexture = textureAt(mrTex);
  if (mrTexture) {
    lay.Metallic = GetPBRChannelTexture(mrTexture, 0, mat.metallicFactor);
    lay.Roughness = GetPBRChannelTexture(mrTexture, 1, mat.roughnessFactor);
  } else {
    lay.Metallic = CreateColoredTexture(
        FVector4(mat.metallicFactor, mat.metallicFactor, mat.metallicFactor,
                 1.0f));
    lay.Roughness = CreateColoredTexture(
        FVector4(mat.roughnessFactor, mat.roughnessFactor,
                 mat.roughnessFactor, 1.0f));
  }

  FGameTexture *aoTexture = textureAt(aoTex);
  lay.AmbientOcclusion =
      aoTexture ? GetPBRChannelTexture(aoTexture, 2, mat.occlusionStrength)
                : CreateColoredTexture(FVector4(1.0f, 1.0f, 1.0f, 1.0f));

  // Emissive maps render as fullbright brightmaps (emissiveFactor scaling
  // is not baked in; emission is suppressed entirely when the factor is 0).
  FGameTexture *emisTexture = textureAt(emisTex);
  if (emisTexture && (mat.emissiveFactor.X > 0.0f ||
                      mat.emissiveFactor.Y > 0.0f ||
                      mat.emissiveFactor.Z > 0.0f)) {
    lay.Brightmap = emisTexture;
  }

  static int pbrCloneCounter = 0;
  FString texName;
  texName.Format("GLTFPBR_%d", pbrCloneCounter++);

  static const bool pbrDebug = getenv("GLTF_RENDER_DEBUG") != nullptr;
  if (pbrDebug) {
    Printf("PBRDBG clone '%s' base '%s' mrTex %d nrm %d ao %d em %d met %.2f "
           "rgh %.2f\n",
           texName.GetChars(), base->GetName().GetChars(), mrTex, normalTex,
           aoTex, emisTex, mat.metallicFactor, mat.roughnessFactor);
  }

  FGameTexture *clone =
      new FGameTexture(base->GetTexture(), texName.GetChars());
  clone->SetShaderLayers(lay);
  TexMan.AddGameTexture(clone);

  cache.Push({base, mrTex, normalTex, aoTex, emisTex, metByte, roughByte,
              occByte, clone});
  return clone;
}

//===========================================================================
//
// Vertex Buffer Implementation
//
//===========================================================================

void FGLTFModel::BuildVertexBuffer(FModelRenderer *renderer) {
  if (!renderer || !isValid) {
    DPrintf(DMSG_ERROR,
            "Cannot build vertex buffer: invalid renderer or model\n");
    return;
  }

  framesSinceLoad++;

  try {
    // Get the renderer type to select appropriate vertex buffer
    ModelRendererType rendererType = renderer->GetType();

    if (GetVertexBuffer(rendererType) != nullptr) {
      // Vertex buffer already exists
      return;
    }

    // Calculate total vertex and index counts
    size_t totalVertices = 0;
    size_t totalIndices = 0;

    for (const auto &mesh : scene.meshes) {
      totalVertices += mesh.vertices.Size();
      totalIndices += mesh.indices.Size();
    }

    if (totalVertices == 0) {
      DPrintf(DMSG_WARNING, "glTF model has no vertices\n");
      return;
    }

    // Check limits
    if (totalVertices > loadOptions.maxVertexCount) {
      DPrintf(DMSG_ERROR, "glTF model vertex count (%zu) exceeds limit (%zu)\n",
              totalVertices, loadOptions.maxVertexCount);
      return;
    }

    if (totalIndices > loadOptions.maxTriangleCount * 3) {
      DPrintf(DMSG_ERROR, "glTF model triangle count exceeds limit\n");
      return;
    }

    // Create vertex buffer
    bool needIndex = totalIndices > 0;
    bool singleFrame = scene.animations.Size() == 0;

    auto *vb = renderer->CreateVertexBuffer(needIndex, singleFrame);
    SetVertexBuffer(rendererType, vb);

    if (!GetVertexBuffer(rendererType)) {
      DPrintf(DMSG_ERROR, "Failed to create vertex buffer for glTF model\n");
      return;
    }

    // Build vertex data
    BuildVertexData(renderer, rendererType);

    DPrintf(DMSG_NOTIFY,
            "Built glTF vertex buffer: %zu vertices, %zu indices\n",
            totalVertices, totalIndices);

  } catch (const std::exception &e) {
    DPrintf(DMSG_ERROR, "Exception building glTF vertex buffer: %s\n",
            e.what());

    // Clean up on failure
    if (GetVertexBuffer(renderer->GetType())) {
      auto *doomed = GetVertexBuffer(renderer->GetType());
      delete doomed;
      SetVertexBuffer(renderer->GetType(), nullptr);
    }
  }
}

void FGLTFModel::BuildVertexData(FModelRenderer *renderer,
                                 ModelRendererType rendererType) {
  auto *buffer = GetVertexBuffer(rendererType);
  if (!buffer) {
    return;
  }

  // Convert glTF vertices to GZDoom format.
  // NOTE: indices stay LOCAL to each mesh (0-based): at render time
  // SetupFrame shifts the vertex attribute pointers by the mesh's vertex
  // offset (the MD3 convention), so baking the offset into the indices
  // here too would address past each mesh's vertices.
  TArray<FModelVertex> gzVertices;
  TArray<unsigned int> gzIndices;

  for (const auto &mesh : scene.meshes) {
    // Convert vertices
    for (const auto &gltfVertex : mesh.vertices) {
      FModelVertex gzVertex;

      // Position
      gzVertex.x = gltfVertex.x;
      gzVertex.y = gltfVertex.y;
      gzVertex.z = gltfVertex.z;

      // Normal (already packed in gltf vertex if available)
      gzVertex.packedNormal = gltfVertex.packedNormal;

      // Texture coordinates
      gzVertex.u = gltfVertex.u;
      gzVertex.v = gltfVertex.v;

      // Bone indices and weights for skinning
      gzVertex.boneselector[0] = gltfVertex.boneIndices[0];
      gzVertex.boneselector[1] = gltfVertex.boneIndices[1];
      gzVertex.boneselector[2] = gltfVertex.boneIndices[2];
      gzVertex.boneselector[3] = gltfVertex.boneIndices[3];

      // Convert float weights (0.0-1.0) to uint8_t (0-255)
      gzVertex.boneweight[0] =
          static_cast<uint8_t>(gltfVertex.boneWeights[0] * 255.0f);
      gzVertex.boneweight[1] =
          static_cast<uint8_t>(gltfVertex.boneWeights[1] * 255.0f);
      gzVertex.boneweight[2] =
          static_cast<uint8_t>(gltfVertex.boneWeights[2] * 255.0f);
      gzVertex.boneweight[3] =
          static_cast<uint8_t>(gltfVertex.boneWeights[3] * 255.0f);

      // Lightmap (not used for GLTF)
      gzVertex.lu = 0.0f;
      gzVertex.lv = 0.0f;
      gzVertex.lindex = -1.0f;

      gzVertices.Push(gzVertex);
    }

    // Convert indices (kept mesh-local, see above)
    for (unsigned int index : mesh.indices) {
      gzIndices.Push(index);
    }
  }

  // Upload to GPU
  UploadVertexData(buffer, gzVertices, gzIndices);

  // Handle bone data if present
  if (hasSkinning && boneMatrices.Size() != 0) {
    UploadBoneData(renderer);
  }
}

void FGLTFModel::UploadVertexData(IModelVertexBuffer *buffer,
                                  const TArray<FModelVertex> &vertices,
                                  const TArray<unsigned int> &indices) {
  if (!buffer) {
    DPrintf(DMSG_ERROR, "UploadVertexData: null buffer\n");
    return;
  }

  if (vertices.Size() == 0) {
    DPrintf(DMSG_WARNING, "UploadVertexData: no vertices to upload\n");
    return;
  }

  DPrintf(DMSG_NOTIFY, "Uploading glTF vertex data: %d vertices, %d indices\n",
          vertices.Size(), indices.Size());

  // Lock buffers and get pointers (following MD3 pattern)
  FModelVertex *vertptr = buffer->LockVertexBuffer(vertices.Size());
  // Non-indexed primitives are drawn with DrawArrays and the vertex buffer
  // was created without an index buffer (needIndex == totalIndices > 0),
  // so only lock it when there are indices to upload; requiring it here
  // would abort before the vertex data is ever copied.
  unsigned int *indxptr =
      indices.Size() > 0 ? buffer->LockIndexBuffer(indices.Size()) : nullptr;

  if (!vertptr) {
    DPrintf(DMSG_ERROR, "Failed to lock vertex buffer\n");
    return;
  }

  if (indices.Size() > 0 && !indxptr) {
    DPrintf(DMSG_ERROR, "Failed to lock index buffer\n");
    buffer->UnlockVertexBuffer();
    return;
  }

  // Copy vertex data
  memcpy(vertptr, vertices.Data(), vertices.Size() * sizeof(FModelVertex));

  // Copy index data
  if (indxptr) {
    memcpy(indxptr, indices.Data(), indices.Size() * sizeof(unsigned int));
  }

  // Unlock buffers to commit data to GPU
  buffer->UnlockVertexBuffer();
  if (indxptr) {
    buffer->UnlockIndexBuffer();
  }

  DPrintf(DMSG_NOTIFY, "glTF vertex data uploaded successfully\n");
}

void FGLTFModel::UploadBoneData(FModelRenderer *renderer) {
  if (!hasSkinning || boneMatrices.Size() == 0) {
    return;
  }
}

//===========================================================================
//
// Rendering Implementation
//
//===========================================================================

void FGLTFModel::RenderFrame(FModelRenderer *renderer, FGameTexture *skin,
                             int frame, int frame2, double inter,
                             FTranslationID translation,
                             const FTextureID *surfaceskinids,
                             int boneStartPosition) {
  if (!renderer || !isValid)
    return;

  framesSinceLoad++;

  try {
    //------------------------------------------------------------
    // Animation handling
    //------------------------------------------------------------

    // Bone matrices are calculated through ProcessModelFrame/CalculateBones so
    // actor state timing, MODELDEF frame mapping, and decoupled SetAnimation
    // all share the same clock.
    int actualBoneStartPosition = -1;
    if (hasSkinning && scene.skins.Size() > 0) {
      if (boneStartPosition >= 0) {
        actualBoneStartPosition = boneStartPosition;
      } else {
        CalculateBones(nullptr, {static_cast<float>(inter), frame, frame2},
                       -1.0f, AttachAnimationData(), nullptr, nullptr,
                       I_GetTime() * (1.0 / TICRATE));

        actualBoneStartPosition =
            boneMatrices.Size() > 0 ? screen->mBones->UploadBones(boneMatrices)
                                    : -1;
      }
    }

    //------------------------------------------------------------
    // Rendering configuration
    //------------------------------------------------------------

    // TEMP DEBUG: one-shot dump of mesh -> material -> texture bindings
    static bool texDbgDone = false;
    if (!texDbgDone && getenv("GLTF_RENDER_DEBUG") != nullptr) {
      texDbgDone = true;
      for (size_t i = 0; i < scene.meshes.Size(); ++i) {
        const auto &m = scene.meshes[i];
        Printf("MESHDBG mesh %2zu '%s' matIdx %d alphaMode %d texIdx %d ", i,
               m.name.GetChars(), m.materialIndex, m.material.alphaMode,
               m.material.baseColorTextureIndex);
        if (m.material.baseColorTextureIndex >= 0 &&
            m.material.baseColorTextureIndex < textures.Size() &&
            textures[m.material.baseColorTextureIndex]) {
          auto *gt = textures[m.material.baseColorTextureIndex];
          auto *ft = gt->GetTexture();
          int lump = ft ? ft->GetSourceLump() : -1;
          Printf("-> %dx%d srclump '%s'\n", ft ? ft->GetWidth() : -1,
                 ft ? ft->GetHeight() : -1,
                 lump >= 0 ? fileSystem.GetFileFullName(lump) : "(none)");
        } else {
          Printf("-> (no texture)\n");
        }
      }
    }

    //------------------------------------------------------------
    // Iterate over all meshes in the scene
    //------------------------------------------------------------
    // Two passes: opaque/masked meshes first, alpha-blended meshes last,
    // so translucent materials (glass, clearcoat, shadow planes) composite
    // over the finished opaque geometry instead of being overwritten.
    for (int pass = 0; pass < 2; ++pass) {
      size_t vertexOffset = 0;

      for (size_t meshIndex = 0; meshIndex < scene.meshes.Size();
           ++meshIndex) {
        const auto &mesh = scene.meshes[meshIndex];
        const size_t meshVertexOffset = vertexOffset;
        vertexOffset += mesh.vertices.Size();

        const bool isBlend = mesh.material.alphaMode == 2;
        if ((pass == 1) != isBlend)
          continue;

        // TEMP DEBUG: GLTF_HIDE_MAT="3,7,10" skips meshes by material index
        static int hideMats[16] = {-1};
        static bool hideInit = false;
        if (!hideInit) {
          hideInit = true;
          const char *hm = getenv("GLTF_HIDE_MAT");
          if (hm) {
            int n = 0;
            char *copy = strdup(hm);
            for (char *tok = strtok(copy, ","); tok && n < 15;
                 tok = strtok(nullptr, ","))
              hideMats[n++] = atoi(tok);
            hideMats[n] = -1;
            free(copy);
          }
        }
        bool hidden = false;
        for (int h = 0; hideMats[h] >= 0; ++h)
          if (mesh.materialIndex == hideMats[h]) {
            hidden = true;
            break;
          }
        if (hidden)
          continue;

        FGameTexture *meshSkin = nullptr;
        bool skinOverrideUsed = false;

        // 1. MODELDEF SurfaceSkin override
        if (surfaceskinids && meshIndex < MD3_MAX_SURFACES &&
            surfaceskinids[meshIndex].isValid()) {
          meshSkin = TexMan.GetGameTexture(surfaceskinids[meshIndex], true);
          skinOverrideUsed = meshSkin != nullptr;
        }

        // 2. Embedded texture from glTF material, tinted by baseColorFactor
        if (!meshSkin && mesh.material.baseColorTextureIndex >= 0 &&
            mesh.material.baseColorTextureIndex < textures.Size()) {
          meshSkin = textures[mesh.material.baseColorTextureIndex];

          const auto &factor = mesh.material.baseColorFactor;
          if (meshSkin &&
              (factor.X != 1.0f || factor.Y != 1.0f || factor.Z != 1.0f ||
               factor.W != 1.0f)) {
            meshSkin = GetTintedTexture(meshSkin, factor);
          }
        }

        // 3. MODELDEF Skin or baseColorFactor fallback
        if (!meshSkin) {
          const bool hasCustomColor =
              (mesh.material.baseColorFactor.X != 1.0f ||
               mesh.material.baseColorFactor.Y != 1.0f ||
               mesh.material.baseColorFactor.Z != 1.0f ||
               mesh.material.baseColorFactor.W != 1.0f);

          if (!hasCustomColor && skin) {
            meshSkin = skin;
          }
        }

        // 4. Neutralize Doom player/team translations for glTF colors
        const bool usingGeneratedColor =
            mesh.material.baseColorTextureIndex < 0;

        // 4b. Attach PBR layers (metallic/roughness/normal/AO/emissive) so
        // FMaterial selects the PBR shader for materials that carry them.
        // Factor-only PBR materials need a generated base texture first.
        // MODELDEF SurfaceSkin overrides are left untouched.
        if (!skinOverrideUsed) {
          if (!meshSkin && MaterialHasExplicitPBR(mesh.material)) {
            meshSkin = CreateColoredTexture(mesh.material.baseColorFactor);
          }
          meshSkin = ApplyPBRMaterialLayers(meshSkin, mesh.material);
        }

        // 5. Per-mesh alpha mode (blend/alpha-test render state)
        renderer->SetMeshAlphaMode(mesh.material.alphaMode,
                                   (float)mesh.material.alphaCutoff);

        // 6. Render mesh (the PBR shader is selected automatically through
        // the material layers attached above)
        RenderMeshStandard(renderer, mesh, meshSkin,
                           usingGeneratedColor ? FTranslationID()
                                               : translation,
                           meshVertexOffset, actualBoneStartPosition);
      }
    }

    // Restore opaque state for whatever draws after this model
    renderer->SetMeshAlphaMode(0, 0.5f);
  } catch (const std::exception &e) {
    DPrintf(DMSG_ERROR, "Exception rendering glTF frame: %s\n", e.what());
  }
}

void FGLTFModel::RenderMeshStandard(FModelRenderer *renderer,
                                    const GLTFMesh &mesh, FGameTexture *skin,
                                    FTranslationID translation,
                                    size_t vertexOffset,
                                    int boneStartPosition) {
  // TEMP DEBUG: GLTF_MATID=1 paints every mesh with a flat per-material
  // color so artifact pixels can be attributed to a material
  static const bool matIdMode = getenv("GLTF_MATID") != nullptr;
  if (matIdMode) {
    static const float pal[][3] = {
        {1, 0, 0},   {0, 1, 0},   {0, 0, 1},     {1, 1, 0},  {0, 1, 1},
        {1, 0, 1},   {1, 0.5, 0}, {1, 1, 1},     {0.5, 0.5, 0.5},
        {0.5, 0, 1}, {0.5, 1, 0}, {0, 0, 0.5},   {0, 0.5, 0.5},
        {1, 0.5, 0.5}};
    int m = mesh.materialIndex;
    if (m < 0 || m > 13)
      m = 7;
    skin = CreateColoredTexture(
        FVector4(pal[m][0], pal[m][1], pal[m][2], 1.0f));
    renderer->SetMeshAlphaMode(0, 0.5f);
  }

  // Validate material before rendering
  // glTF models may not have textures embedded, so we need to handle NULL skin
  if (!skin) {
    // Create a colored texture from the material's baseColorFactor
    // This properly renders materials that use only vertex colors or
    // baseColorFactor
    skin = CreateColoredTexture(mesh.material.baseColorFactor);

    if (!skin) {
      DPrintf(DMSG_ERROR,
              "Cannot render glTF mesh: failed to create colored texture\n");
      return;
    }
  }

  // If we're using generated color from baseColorFactor, DO NOT apply actor
  // translation. Actor translation means tinting according to team colors,
  // which is not desired for materials that specify their own color.
  const bool usingGeneratedColor = mesh.material.baseColorTextureIndex < 0;

  // Set material
  renderer->SetMaterial(skin, false,
                        usingGeneratedColor ? FTranslationID() : translation);

  // Setup vertex/index buffers - CRITICAL for rendering!
  // This binds the vertex and index buffers to the rendering state
  // Pass bone start position for skeletal animation
  renderer->SetupFrame(this, vertexOffset, vertexOffset, mesh.vertices.Size(),
                       boneStartPosition);

  // Render geometry
  if (mesh.indices.Size() == 0) {
    // Non-indexed rendering (start is mesh-local: the vertex buffer
    // pointers are already shifted by vertexOffset via SetupFrame)
    renderer->DrawArrays(0, mesh.vertices.Size());
  } else {
    // Indexed rendering
    size_t indexOffset = 0;
    // Calculate proper index offset based on previous meshes
    for (size_t i = 0; i < scene.meshes.Size() && &scene.meshes[i] != &mesh;
         ++i) {
      indexOffset += scene.meshes[i].indices.Size();
    }

    renderer->DrawElements(mesh.indices.Size(),
                           indexOffset * sizeof(unsigned int));
  }
}

void FGLTFModel::UpdateAnimationState(double currentTime) {
  if (currentAnimationIndex < 0 ||
      currentAnimationIndex >= scene.animations.Size()) {
    return;
  }

  const auto &anim = scene.animations[currentAnimationIndex];
  if (anim.duration <= 0.0f || currentAnimationIndex >= modelAnimations.Size()) {
    return;
  }

  const float animTime = fmod(currentTime - lastAnimationTime, anim.duration);
  const double frame =
      modelAnimations[currentAnimationIndex].firstFrame +
      animTime * modelAnimations[currentAnimationIndex].framerate;
  ModelAnimFrameInterp to;
  to.frame1 = static_cast<int>(floor(frame));
  to.frame2 = static_cast<int>(ceil(frame));
  to.inter = static_cast<float>(frame - to.frame1);
  CalculateBones(nullptr, to, -1.0f, AttachAnimationData(), nullptr, nullptr,
                 currentTime);
}

//===========================================================================
//
// Animation and Skinning Interface
//
//===========================================================================

int FGLTFModel::FindFrame(const char *name, bool nodefault) {
  if (!name || !*name) {
    return nodefault ? FErr_NotFound : 0;
  }

  const char *colon = strrchr(name, ':');
  FString animName = colon ? FString(name, colon - name) : FString(name);
  int animationIndex = FindAnimation(animName.GetChars());

  if (animationIndex >= 0 && animationIndex < modelAnimations.Size()) {
    int frame = modelAnimations[animationIndex].firstFrame;
    if (colon) {
      const int offset = atoi(colon + 1);
      const int frameCount = modelAnimations[animationIndex].lastFrame -
                             modelAnimations[animationIndex].firstFrame;
      if (offset < 0 || offset >= frameCount) {
        return FErr_NotFound;
      }
      frame += offset;
    }
    return frame;
  }

  return scene.animations.Size() == 0 && !nodefault ? 0 : FErr_NotFound;
}

void FGLTFModel::AddSkins(uint8_t *hitlist, const FTextureID *surfaceskinids) {
  if (!hitlist) {
    return;
  }

  // Add all textures used by this model to the hitlist
  for (const auto &texture : textures) {
    if (texture && texture->GetID().isValid()) {
      int index = texture->GetID().GetIndex();
      if (index >= 0 && index < INT_MAX) {
        hitlist[index] = 1;
      }
    }
  }

  // Add surface skin overrides. The caller's array has only
  // MD3_MAX_SURFACES entries; glTF meshes are primitives and can number
  // in the thousands (each glTF mesh becomes its own GLTFMesh), so the
  // loop must stop at the array's capacity or it reads out of bounds
  // and crashes the level precache on large models.
  if (surfaceskinids) {
    size_t maxSurfaces = std::min<size_t>(scene.meshes.Size(), MD3_MAX_SURFACES);
    for (size_t i = 0; i < maxSurfaces; ++i) {
      if (surfaceskinids[i].isValid()) {
        int index = surfaceskinids[i].GetIndex();
        if (index >= 0 && index < INT_MAX) {
          hitlist[index] = 1;
        }
      }
    }
  }
}

bool FGLTFModel::HasPBRMaterials() const {
  for (const auto &mesh : scene.meshes) {
    const auto &material = mesh.material;

    // Check if this material uses PBR features
    if (material.metallicFactor != 1.0f || material.roughnessFactor != 1.0f ||
        material.baseColorTextureIndex != -1 ||
        material.metallicRoughnessTextureIndex != -1 ||
        material.normalTextureIndex != -1 ||
        material.emissiveTextureIndex != -1) {
      return true;
    }
  }
  return false;
}

const char *FGLTFModel::GetAnimationName(int index) const {
  if (index < 0 || index >= scene.animations.Size()) {
    return "";
  }
  return scene.animations[index].name.GetChars();
}

float FGLTFModel::GetAnimationDuration(int index) const {
  if (index < 0 || index >= scene.animations.Size()) {
    return 0.0f;
  }
  return scene.animations[index].duration;
}

void FGLTFModel::SetCurrentAnimation(int index) {
  if (index >= 0 && index < scene.animations.Size()) {
    currentAnimationIndex = index;
    lastAnimationTime = I_GetTime() * (1.0 / TICRATE);
  } else {
    currentAnimationIndex = -1;
  }
}

void FGLTFModel::UpdateAnimation(double currentTime,
                                 TArray<VSMatrix> &outBoneMatrices) {
  if (currentAnimationIndex < 0 || !hasSkinning || scene.skins.Size() == 0) {
    return;
  }

  UpdateAnimationState(currentTime);

  // Copy current bone matrices
  outBoneMatrices.Resize(boneMatrices.Size());
  for (int i = 0; i < boneMatrices.Size(); ++i) {
    outBoneMatrices[i] = boneMatrices[i];
  }
}

#endif // NEODOOM_GLTF_SUPPORT
