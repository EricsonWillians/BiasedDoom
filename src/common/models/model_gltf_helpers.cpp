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
** model_gltf_helpers.cpp
**
** Helper functions and validation for glTF model support
**
**/

#include "model_gltf.h"

#ifdef NEODOOM_GLTF_SUPPORT

#include "i_time.h"
#include "printf.h"
#include <algorithm>

//===========================================================================
//
// Memory Management and Validation Helpers
//
//===========================================================================

void FGLTFModel::CleanupResources() {
  scene.meshes.Clear();
  scene.nodes.Clear();
  scene.skins.Clear();
  scene.animations.Clear();
  scene.rootNodeIndices.Clear();

  for (auto &buffer : buffers) {
    buffer.Clear();
  }
  buffers.Clear();

  textures.Clear();
  modelAnimations.Clear();
  basePose.Clear();
  boneMatrices.Clear();

  asset.reset();

  memoryUsage = 0;
  isValid = false;
}

bool FGLTFModel::CheckMemoryLimits(GLTFLoadResult &result) const {
  // Get current memory usage (simplified - would integrate with GZDoom's memory
  // tracking)
  size_t currentMemory = sizeof(*this);

  // Check if we have reasonable limits
  const size_t maxModelMemory = 256 * 1024 * 1024; // 256MB per model
  if (currentMemory > maxModelMemory) {
    result.SetError(GLTFError::OutOfMemory,
                    "Model memory usage exceeds limits");
    return false;
  }

  return true;
}

void FGLTFModel::UpdateMemoryUsage() {
  memoryUsage = sizeof(*this);

  // Add mesh data
  for (const auto &mesh : scene.meshes) {
    memoryUsage += mesh.vertices.Size() * sizeof(FGLTFVertex);
    memoryUsage += mesh.indices.Size() * sizeof(unsigned int);
  }

  // Add buffer data
  for (const auto &buffer : buffers) {
    memoryUsage += buffer.Size();
  }

  // Add animation data
  for (const auto &anim : scene.animations) {
    memoryUsage += anim.samplers.Size() * sizeof(GLTFAnimationSampler);
    memoryUsage += anim.channels.Size() * sizeof(GLTFAnimationChannel);
  }

  // Add bone data
  memoryUsage += basePose.Size() * sizeof(TRS);
  memoryUsage += boneMatrices.Size() * sizeof(VSMatrix);
}

void FGLTFModel::GetPerformanceStats(size_t &memory, double &loadTime,
                                     int &frames) const {
  memory = memoryUsage;
  loadTime = totalLoadTime;
  frames = framesSinceLoad;
}

//===========================================================================
//
// Validation Functions
//
//===========================================================================

bool FGLTFModel::ValidateModel(GLTFLoadResult &result) const {
  result.Clear();

  if (!asset) {
    result.SetError(GLTFError::ValidationFailure, "No asset loaded");
    return false;
  }

  // Validate basic structure
  if (!ValidateBuffers(result))
    return false;
  if (!ValidateAccessors(result))
    return false;
  if (!ValidateNodes(result))
    return false;

  // Validate specific subsystems if present
  if (hasSkinning && !ValidateAnimations(result))
    return false;
  if (!ValidateMaterials(result))
    return false;

  return true;
}

bool FGLTFModel::ValidateBuffers(GLTFLoadResult &result) const {
  Printf("ValidateBuffers: buffers.Size()=%d, asset->buffers.size()=%zu\n",
         buffers.Size(), asset->buffers.size());

  if (buffers.Size() != asset->buffers.size()) {
    result.SetError(GLTFError::ValidationFailure, "Buffer count mismatch");
    Printf(
        "  ERROR: Buffer count mismatch! Loaded %d buffers but asset has %zu\n",
        buffers.Size(), asset->buffers.size());
    return false;
  }

  for (size_t i = 0; i < buffers.Size(); ++i) {
    if (buffers[i].Size() != asset->buffers[i].byteLength) {
      result.SetError(GLTFError::ValidationFailure, "Buffer size mismatch");
      return false;
    }

    if (buffers[i].Size() > loadOptions.maxVertexCount * sizeof(FGLTFVertex)) {
      result.SetError(GLTFError::ValidationFailure, "Buffer too large");
      return false;
    }
  }

  return true;
}

bool FGLTFModel::ValidateAccessors(GLTFLoadResult &result) const {
  for (size_t i = 0; i < asset->accessors.size(); ++i) {
    const auto &accessor = asset->accessors[i];

    if (!accessor.bufferViewIndex.has_value()) {
      continue; // Sparse accessors not fully supported yet
    }

    size_t bufferViewIndex = accessor.bufferViewIndex.value();
    if (bufferViewIndex >= asset->bufferViews.size()) {
      result.SetError(GLTFError::ValidationFailure,
                      "Accessor references invalid buffer view");
      return false;
    }

    const auto &bufferView = asset->bufferViews[bufferViewIndex];
    if (bufferView.bufferIndex >= buffers.Size()) {
      result.SetError(GLTFError::ValidationFailure,
                      "Buffer view references invalid buffer");
      return false;
    }

    // Check bounds
    size_t totalOffset = bufferView.byteOffset + accessor.byteOffset;
    size_t accessorSize =
        accessor.count *
        fastgltf::getElementByteSize(accessor.type, accessor.componentType);

    if (totalOffset + accessorSize > buffers[bufferView.bufferIndex].Size()) {
      result.SetError(GLTFError::ValidationFailure,
                      "Accessor exceeds buffer bounds");
      return false;
    }
  }

  return true;
}

bool FGLTFModel::ValidateNodes(GLTFLoadResult &result) const {
  if (scene.nodes.Size() != asset->nodes.size()) {
    result.SetError(GLTFError::ValidationFailure, "Node count mismatch");
    return false;
  }

  // Check for cycles in node hierarchy
  TArray<bool> visited;
  TArray<bool> inPath;
  visited.Resize(scene.nodes.Size());
  inPath.Resize(scene.nodes.Size());

  // Initialize all to false
  for (unsigned int i = 0; i < scene.nodes.Size(); ++i) {
    visited[i] = false;
    inPath[i] = false;
  }

  std::function<bool(int)> detectCycle = [&](int nodeIndex) -> bool {
    if (nodeIndex < 0 || nodeIndex >= scene.nodes.Size()) {
      return false;
    }

    if (inPath[nodeIndex]) {
      return true; // Cycle detected
    }

    if (visited[nodeIndex]) {
      return false; // Already processed
    }

    visited[nodeIndex] = true;
    inPath[nodeIndex] = true;

    for (int childIndex : scene.nodes[nodeIndex].childIndices) {
      if (detectCycle(childIndex)) {
        return true;
      }
    }

    inPath[nodeIndex] = false;
    return false;
  };

  for (int i = 0; i < scene.nodes.Size(); ++i) {
    if (!visited[i] && detectCycle(i)) {
      result.SetError(GLTFError::ValidationFailure,
                      "Cycle detected in node hierarchy");
      return false;
    }
  }

  return true;
}

bool FGLTFModel::ValidateAnimations(GLTFLoadResult &result) const {
  for (const auto &anim : scene.animations) {
    if (!ValidateAnimationData(anim, const_cast<GLTFLoadResult &>(result))) {
      return false;
    }
  }
  return true;
}

bool FGLTFModel::ValidateMaterials(GLTFLoadResult &result) const {
  for (const auto &mesh : scene.meshes) {
    // Validate material texture indices
    const auto &material = mesh.material;

    auto validateTextureIndex = [&](int index, const char *name) -> bool {
      if (index >= 0 && index >= textures.Size()) {
        FString msg;
        msg.Format("Material %s texture index out of bounds", name);
        result.SetError(GLTFError::ValidationFailure, msg.GetChars());
        return false;
      }
      return true;
    };

    if (!validateTextureIndex(material.baseColorTextureIndex, "base color"))
      return false;
    if (!validateTextureIndex(material.metallicRoughnessTextureIndex,
                              "metallic-roughness"))
      return false;
    if (!validateTextureIndex(material.normalTextureIndex, "normal"))
      return false;
    if (!validateTextureIndex(material.occlusionTextureIndex, "occlusion"))
      return false;
    if (!validateTextureIndex(material.emissiveTextureIndex, "emissive"))
      return false;
  }

  return true;
}

bool FGLTFModel::ValidateAnimationData(const GLTFAnimation &anim,
                                       GLTFLoadResult &result) const {
  if (anim.samplers.Size() == 0 && anim.channels.Size() != 0) {
    result.SetError(GLTFError::AnimationError,
                    "Animation has channels but no samplers");
    return false;
  }

  for (const auto &channel : anim.channels) {
    if (channel.samplerIndex < 0 ||
        channel.samplerIndex >= anim.samplers.Size()) {
      result.SetError(GLTFError::AnimationError,
                      "Animation channel references invalid sampler");
      return false;
    }

    if (channel.targetNodeIndex < 0 ||
        channel.targetNodeIndex >= (int)scene.nodes.Size()) {
      result.SetError(GLTFError::AnimationError,
                      "Animation channel targets invalid node");
      return false;
    }

    // Validate target path
    if (channel.targetPath.Compare("translation") != 0 &&
        channel.targetPath.Compare("rotation") != 0 &&
        channel.targetPath.Compare("scale") != 0 &&
        channel.targetPath.Compare("weights") != 0) {
      result.SetError(GLTFError::AnimationError,
                      "Invalid animation target path");
      return false;
    }
  }

  return true;
}

//===========================================================================
//
// Utility Functions
//
//===========================================================================

bool FGLTFModel::IsBufferValid(int bufferIndex) const {
  return bufferIndex >= 0 && bufferIndex < (int)buffers.Size() &&
         buffers[bufferIndex].Size() != 0;
}

bool FGLTFModel::IsAccessorValid(int accessorIndex) const {
  return asset && accessorIndex >= 0 &&
         accessorIndex < static_cast<int>(asset->accessors.size());
}

bool FGLTFModel::IsNodeValid(int nodeIndex) const {
  return nodeIndex >= 0 && nodeIndex < scene.nodes.Size();
}

const char *FGLTFModel::GetErrorString(GLTFError error) const {
  switch (error) {
  case GLTFError::None:
    return "No error";
  case GLTFError::InvalidFormat:
    return "Invalid file format";
  case GLTFError::UnsupportedVersion:
    return "Unsupported glTF version";
  case GLTFError::MissingRequiredData:
    return "Missing required data";
  case GLTFError::CorruptedBuffer:
    return "Corrupted buffer data";
  case GLTFError::OutOfMemory:
    return "Out of memory";
  case GLTFError::LibraryError:
    return "Library error";
  case GLTFError::TextureLoadFailure:
    return "Texture load failure";
  case GLTFError::AnimationError:
    return "Animation error";
  case GLTFError::ValidationFailure:
    return "Validation failure";
  default:
    return "Unknown error";
  }
}

void FGLTFModel::PrintErrorDetails(const GLTFLoadResult &result) const {
  if (result.error == GLTFError::None) {
    return;
  }

  Printf("glTF Error [%s]: %s", GetErrorString(result.error),
         result.errorMessage.GetChars());
  if (result.errorLine >= 0) {
    Printf(" (line %d)", result.errorLine);
  }
  Printf("\n");
}

void FGLTFModel::PrintLoadInfo() const {
  Printf("glTF Model loaded successfully:\n");
  Printf("  Meshes: %d\n", scene.meshes.Size());
  Printf("  Nodes: %d\n", scene.nodes.Size());
  Printf("  Animations: %d\n", scene.animations.Size());
  Printf("  Textures: %d\n", textures.Size());
  Printf("  Has skinning: %s\n", hasSkinning ? "Yes" : "No");
  Printf("  Memory usage: %.2f KB\n", memoryUsage / 1024.0);
  Printf("  Load time: %.3f seconds\n", totalLoadTime);

  if (hasSkinning && scene.skins.Size() != 0) {
    Printf("  Bones: %d\n", scene.skins[0].jointIndices.Size());
  }
}

#endif // NEODOOM_GLTF_SUPPORT
