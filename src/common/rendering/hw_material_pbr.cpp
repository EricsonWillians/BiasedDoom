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
** hw_material_pbr.cpp
**
** PBR (Physically Based Rendering) material system extensions
**
**/

#include "hw_material_pbr.h"
#include "gametexture.h"
#include "hw_renderstate.h"
#include "model_gltf.h"
#include "printf.h"
#include <algorithm>

//===========================================================================
//
// FPBRMaterial Implementation
//
//===========================================================================

FPBRMaterial::FPBRMaterial(FGameTexture *tex, int scaleflags,
                           PBRShaderMode mode)
    : FMaterial(tex, scaleflags), pbrMode(mode) {
  // Initialize PBR textures to null
  for (int i = 0; i < PBRTextureSlots::MaxPBRTextures; ++i) {
    pbrTextures[i] = nullptr;
  }

  // Set default PBR values
  pbrUniforms.baseColorFactor = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
  pbrUniforms.emissiveFactor = FVector4(0.0f, 0.0f, 0.0f, 1.0f);
  pbrUniforms.pbrFactors = FVector4(
      1.0f, 1.0f, 1.0f, 0.5f); // metallic, roughness, normalScale, alphaCutoff
  pbrUniforms.flags = 0;

  // Initialize texture transforms to identity
  for (int i = 0; i < PBRTextureSlots::MaxPBRTextures; ++i) {
    pbrUniforms.textureTransforms[i] =
        FVector4(1.0f, 1.0f, 0.0f, 0.0f); // scale.xy, offset.xy
  }

  isDirty = true;
}

void FPBRMaterial::SetPBRMode(PBRShaderMode mode) {
  if (pbrMode != mode) {
    pbrMode = mode;
    isDirty = true;
  }
}

void FPBRMaterial::SetBaseColorFactor(const FVector4 &color) {
  pbrUniforms.baseColorFactor = color;
  isDirty = true;
}

void FPBRMaterial::SetMetallicFactor(float metallic) {
  pbrUniforms.pbrFactors.X = std::clamp(metallic, 0.0f, 1.0f);
  isDirty = true;
}

void FPBRMaterial::SetRoughnessFactor(float roughness) {
  pbrUniforms.pbrFactors.Y = std::clamp(roughness, 0.0f, 1.0f);
  isDirty = true;
}

void FPBRMaterial::SetNormalScale(float scale) {
  pbrUniforms.pbrFactors.Z = scale;
  isDirty = true;
}

void FPBRMaterial::SetEmissiveFactor(const FVector3 &emissive) {
  pbrUniforms.emissiveFactor =
      FVector4(emissive.X, emissive.Y, emissive.Z, 1.0f);
  isDirty = true;
}

void FPBRMaterial::SetAlphaCutoff(float cutoff) {
  pbrUniforms.pbrFactors.W = std::clamp(cutoff, 0.0f, 1.0f);
  if (cutoff > 0.0f && cutoff < 1.0f) {
    pbrUniforms.flags |= PBRMaterialUniforms::AlphaTest;
  } else {
    pbrUniforms.flags &= ~PBRMaterialUniforms::AlphaTest;
  }
  isDirty = true;
}

void FPBRMaterial::SetDoubleSided(bool enabled) {
  if (enabled) {
    pbrUniforms.flags |= PBRMaterialUniforms::DoubleSided;
  } else {
    pbrUniforms.flags &= ~PBRMaterialUniforms::DoubleSided;
  }
  isDirty = true;
}

void FPBRMaterial::SetPBRTexture(int slot, FGameTexture *texture,
                                 const FVector4 &transform) {
  if (slot < 0 || slot >= PBRTextureSlots::MaxPBRTextures) {
    return;
  }

  pbrTextures[slot] = texture;
  pbrUniforms.textureTransforms[slot] = transform;

  // Update texture flags
  uint32_t flag = 1 << slot;
  if (texture != nullptr) {
    pbrUniforms.flags |= flag;
  } else {
    pbrUniforms.flags &= ~flag;
  }

  isDirty = true;
}

FGameTexture *FPBRMaterial::GetPBRTexture(int slot) const {
  if (slot < 0 || slot >= PBRTextureSlots::MaxPBRTextures) {
    return nullptr;
  }
  return pbrTextures[slot];
}

const PBRMaterialUniforms &FPBRMaterial::GetPBRUniforms() {
  if (isDirty) {
    UpdateUniforms();
    isDirty = false;
  }
  return pbrUniforms;
}

bool FPBRMaterial::HasPBRTextures() const {
  for (int i = 0; i < PBRTextureSlots::MaxPBRTextures; ++i) {
    if (pbrTextures[i] != nullptr) {
      return true;
    }
  }
  return false;
}

void FPBRMaterial::UpdateUniforms() {
  ValidateTextures();

  // Additional uniform validation could go here
  // For example, ensuring metallic/roughness values are in valid ranges
}

void FPBRMaterial::ValidateTextures() {
  // Validate that texture references are still valid
  for (int i = 0; i < PBRTextureSlots::MaxPBRTextures; ++i) {
    if (pbrTextures[i] != nullptr) {
      // Could add texture validation logic here
      // For now, assume textures remain valid
    }
  }
}

void FPBRMaterial::SetMaterialTextures(FRenderState &state) {
  // Call base class implementation for standard textures
  FMaterial::SetMaterialTextures(state);

  // Bind PBR-specific textures if this is a PBR material
  if (IsPBRMaterial()) {
    // Implementation would bind PBR textures to appropriate texture units
    // This is hardware renderer specific and would need to integrate
    // with the existing texture binding system

    Printf("TODO: Implement PBR texture binding for hardware renderer\n");
  }
}

void FPBRMaterial::SetMaterialShader(FRenderState &state) {
  if (IsPBRMaterial()) {
    // Set PBR shader and uniforms
    Printf("TODO: Implement PBR shader selection and uniform binding\n");

    // The implementation would:
    // 1. Select appropriate PBR shader based on available textures and features
    // 2. Bind PBR uniform buffer with material properties
    // 3. Set up any additional render state for PBR rendering
  } else {
    // Use standard material shader
    FMaterial::SetMaterialShader(state);
  }
}

//===========================================================================
//
// Global PBR Functions
//
//===========================================================================

FPBRMaterial *CreatePBRMaterial(FGameTexture *baseTexture,
                                const PBRMaterialProperties &props,
                                const TArray<FGameTexture *> &textures) {
  if (!baseTexture) {
    Printf("Warning: CreatePBRMaterial called with null base texture\n");
    return nullptr;
  }

  // Create PBR material with metallic-roughness workflow
  auto *material =
      new FPBRMaterial(baseTexture, 0, PBRShaderMode::MetallicRoughness);

  // Set PBR factors
  material->SetBaseColorFactor(props.baseColorFactor);
  material->SetMetallicFactor(props.metallicFactor);
  material->SetRoughnessFactor(props.roughnessFactor);
  material->SetNormalScale(props.normalScale);
  material->SetEmissiveFactor(props.emissiveFactor);
  material->SetAlphaCutoff(props.alphaCutoff);
  material->SetDoubleSided(props.doubleSided);

  // Bind textures
  if (props.baseColorTextureIndex >= 0 &&
      props.baseColorTextureIndex < textures.Size()) {
    FVector4 transform(1.0f, 1.0f, 0.0f, 0.0f); // Default UV transform
    material->SetPBRTexture(PBRTextureSlots::BaseColor,
                            textures[props.baseColorTextureIndex], transform);
  }

  if (props.metallicRoughnessTextureIndex >= 0 &&
      props.metallicRoughnessTextureIndex < textures.Size()) {
    FVector4 transform(1.0f, 1.0f, 0.0f, 0.0f);
    material->SetPBRTexture(PBRTextureSlots::MetallicRoughness,
                            textures[props.metallicRoughnessTextureIndex],
                            transform);
  }

  if (props.normalTextureIndex >= 0 &&
      props.normalTextureIndex < textures.Size()) {
    FVector4 transform(1.0f, 1.0f, 0.0f, 0.0f);
    material->SetPBRTexture(PBRTextureSlots::Normal,
                            textures[props.normalTextureIndex], transform);
  }

  if (props.occlusionTextureIndex >= 0 &&
      props.occlusionTextureIndex < textures.Size()) {
    FVector4 transform(1.0f, 1.0f, 0.0f, 0.0f);
    material->SetPBRTexture(PBRTextureSlots::Occlusion,
                            textures[props.occlusionTextureIndex], transform);
  }

  if (props.emissiveTextureIndex >= 0 &&
      props.emissiveTextureIndex < textures.Size()) {
    FVector4 transform(1.0f, 1.0f, 0.0f, 0.0f);
    material->SetPBRTexture(PBRTextureSlots::Emissive,
                            textures[props.emissiveTextureIndex], transform);
  }

  return material;
}

void RegisterPBRShaders() {
  // This function would register PBR shaders with the hardware renderer
  // Implementation depends on the specific renderer (OpenGL/Vulkan)

  Printf("TODO: Implement PBR shader registration\n");

  // The implementation would:
  // 1. Load PBR vertex and fragment shaders
  // 2. Register shader variants for different feature combinations
  // 3. Set up uniform buffer layouts for PBR parameters
  // 4. Initialize default PBR rendering state
}

bool IsPBRRenderingSupported() {
  // Check if the current hardware renderer supports PBR features
  // This would check for required shader features, texture units, etc.

  // For now, assume PBR is supported on hardware renderers
  // but not on software renderer

  Printf("TODO: Implement PBR capability detection\n");
  return true; // Placeholder
}