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
** model_gltf_debug.cpp
**
** Debug and logging support for glTF models
**
**/

#include "model_gltf_debug.h"

#ifdef NEODOOM_GLTF_SUPPORT

#include "c_cvars.h"
#include "model_gltf.h"
#include <chrono>
#include <cmath>

//===========================================================================
//
// Global Debug Settings
//
//===========================================================================

GLTFDebugLevel gltf_debug_level = GLTFDebugLevel::Warning;
bool gltf_debug_performance = false;
bool gltf_debug_validation = false;
bool gltf_debug_memory = false;

GLTFProfiler gltf_profiler;
GLTFMemoryTracker gltf_memory_tracker;

// Console variables for runtime control
CVAR(Int, gltf_debug_level, 2, CVAR_ARCHIVE)
CVAR(Bool, gltf_debug_perf, false, CVAR_ARCHIVE)
CVAR(Bool, gltf_debug_validate, false, CVAR_ARCHIVE)
CVAR(Bool, gltf_debug_mem, false, CVAR_ARCHIVE)

//===========================================================================
//
// Debug Helper Functions
//
//===========================================================================

const char *GLTFDebugLevelToString(GLTFDebugLevel level) {
  switch (level) {
  case GLTFDebugLevel::Error:
    return "ERROR";
  case GLTFDebugLevel::Warning:
    return "WARN";
  case GLTFDebugLevel::Info:
    return "INFO";
  case GLTFDebugLevel::Verbose:
    return "VERBOSE";
  case GLTFDebugLevel::Debug:
    return "DEBUG";
  default:
    return "UNKNOWN";
  }
}

void GLTFSetDebugLevel(GLTFDebugLevel level) {
  gltf_debug_level = level;
  gltf_debug_level = static_cast<int>(level);
}

void GLTFSetDebugFlags(bool performance, bool validation, bool memory) {
  gltf_debug_performance = performance;
  gltf_debug_validation = validation;
  gltf_debug_memory = memory;

  gltf_debug_perf = performance;
  gltf_debug_validate = validation;
  gltf_debug_mem = memory;
}

void GLTFDumpModelInfo(const FGLTFModel *model) {
  if (!model || !model->IsValid()) {
    Printf("[glTF DUMP] Invalid model\n");
    return;
  }

  Printf("[glTF DUMP] Model Information:\n");

  const auto &scene = model->GetScene();
  Printf("  Meshes: %d\n", scene.meshes.Size());
  Printf("  Nodes: %d\n", scene.nodes.Size());
  Printf("  Animations: %d\n", scene.animations.Size());
  Printf("  Textures: %d\n", model->GetTextures().Size());
  Printf("  Has Skinning: %s\n", model->HasSkinning() ? "Yes" : "No");
  Printf("  Has PBR: %s\n", model->HasPBRMaterials() ? "Yes" : "No");

  size_t memory;
  double loadTime;
  int frames;
  model->GetPerformanceStats(memory, loadTime, frames);

  Printf("  Memory Usage: %.2f KB\n", memory / 1024.0);
  Printf("  Load Time: %.3f seconds\n", loadTime);
  Printf("  Frames Since Load: %d\n", frames);

  // Dump mesh details
  for (int i = 0; i < scene.meshes.Size(); ++i) {
    const auto &mesh = scene.meshes[i];
    Printf("  Mesh %d '%s': %d vertices, %d indices\n", i, mesh.name.GetChars(),
           mesh.vertices.Size(), mesh.indices.Size());
  }
}

void GLTFDumpSceneHierarchy(const GLTFScene &scene) {
  Printf("[glTF DUMP] Scene Hierarchy:\n");

  std::function<void(int, int)> dumpNode = [&](int nodeIndex, int depth) {
    if (nodeIndex < 0 || nodeIndex >= scene.nodes.Size()) {
      return;
    }

    const auto &node = scene.nodes[nodeIndex];

    // Print indentation
    for (int i = 0; i < depth; ++i) {
      Printf("  ");
    }

    Printf("Node %d '%s'", nodeIndex, node.name.GetChars());

    if (node.meshIndex >= 0) {
      Printf(" [Mesh %d]", node.meshIndex);
    }

    if (node.isBone) {
      Printf(" [Bone %d]", node.boneIndex);
    }

    Printf("\n");

    // Print transform
    if (depth == 0 || node.transform.translation.Length() > 0.001f ||
        node.transform.scaling.Length() - std::sqrt(3.0f) > 0.001f) {
      for (int i = 0; i <= depth; ++i) {
        Printf("  ");
      }
      Printf("T: (%.3f, %.3f, %.3f) S: (%.3f, %.3f, %.3f)\n",
             node.transform.translation.X, node.transform.translation.Y,
             node.transform.translation.Z, node.transform.scaling.X,
             node.transform.scaling.Y, node.transform.scaling.Z);
    }

    // Recursively dump children
    for (int childIndex : node.childIndices) {
      dumpNode(childIndex, depth + 1);
    }
  };

  for (int rootIndex : scene.rootNodeIndices) {
    dumpNode(rootIndex, 0);
  }
}

void GLTFDumpAnimationInfo(const GLTFAnimation &animation) {
  Printf("[glTF DUMP] Animation '%s':\n", animation.name.GetChars());
  Printf("  Duration: %.3f seconds\n", animation.duration);
  Printf("  Samplers: %d\n", animation.samplers.Size());
  Printf("  Channels: %d\n", animation.channels.Size());

  for (int i = 0; i < animation.channels.Size(); ++i) {
    const auto &channel = animation.channels[i];
    Printf("  Channel %d: Node %d, Path '%s', Sampler %d\n", i,
           channel.targetNodeIndex, channel.targetPath.GetChars(),
           channel.samplerIndex);
  }
}

void GLTFDumpMaterialInfo(const PBRMaterialProperties &material) {
  Printf("[glTF DUMP] PBR Material:\n");
  Printf("  Base Color: (%.3f, %.3f, %.3f, %.3f)\n", material.baseColorFactor.X,
         material.baseColorFactor.Y, material.baseColorFactor.Z,
         material.baseColorFactor.W);
  Printf("  Metallic: %.3f, Roughness: %.3f\n", material.metallicFactor,
         material.roughnessFactor);
  Printf("  Normal Scale: %.3f\n", material.normalScale);
  Printf("  Emissive: (%.3f, %.3f, %.3f)\n", material.emissiveFactor.X,
         material.emissiveFactor.Y, material.emissiveFactor.Z);
  Printf("  Alpha Cutoff: %.3f\n", material.alphaCutoff);
  Printf("  Double Sided: %s\n", material.doubleSided ? "Yes" : "No");

  if (material.baseColorTextureIndex >= 0) {
    Printf("  Base Color Texture: %d (UV set %d)\n",
           material.baseColorTextureIndex, material.baseColorTexCoord);
  }
  if (material.metallicRoughnessTextureIndex >= 0) {
    Printf("  Metallic-Roughness Texture: %d (UV set %d)\n",
           material.metallicRoughnessTextureIndex,
           material.metallicRoughnessTexCoord);
  }
  if (material.normalTextureIndex >= 0) {
    Printf("  Normal Texture: %d (UV set %d)\n", material.normalTextureIndex,
           material.normalTexCoord);
  }
}

//===========================================================================
//
// GLTFProfiler Implementation
//
//===========================================================================

void GLTFProfiler::Begin(const char *name) {
  int index = FindEntry(name);
  if (index < 0) {
    ProfileEntry entry;
    entry.name = name;
    entry.totalTime = 0.0;
    entry.callCount = 0;
    entry.active = false;
    entries.Push(entry);
    index = entries.Size() - 1;
  }

  auto &entry = entries[index];
  if (entry.active) {
    GLTF_WARNING("Profiler entry '%s' already active", name);
    return;
  }

  entry.startTime = std::chrono::high_resolution_clock::now();
  entry.active = true;
  currentEntryIndex = index;
}

void GLTFProfiler::End() {
  if (currentEntryIndex < 0 || currentEntryIndex >= entries.Size()) {
    GLTF_WARNING("No active profiler entry to end");
    return;
  }

  auto &entry = entries[currentEntryIndex];
  if (!entry.active) {
    GLTF_WARNING("Profiler entry '%s' not active", entry.name);
    return;
  }

  auto endTime = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration<double>(endTime - entry.startTime).count();

  entry.totalTime += duration;
  entry.callCount++;
  entry.active = false;
  currentEntryIndex = -1;
}

void GLTFProfiler::Reset() {
  for (auto &entry : entries) {
    entry.totalTime = 0.0;
    entry.callCount = 0;
    entry.active = false;
  }
  currentEntryIndex = -1;
}

void GLTFProfiler::PrintReport() const {
  Printf("[glTF PROFILER] Performance Report:\n");

  double totalTime = 0.0;
  for (const auto &entry : entries) {
    totalTime += entry.totalTime;
  }

  for (const auto &entry : entries) {
    if (entry.callCount > 0) {
      double avgTime = entry.totalTime / entry.callCount;
      double percentage =
          totalTime > 0.0 ? (entry.totalTime / totalTime) * 100.0 : 0.0;

      Printf("  %s: %.3f ms total, %.3f ms avg, %d calls (%.1f%%)\n",
             entry.name, entry.totalTime * 1000.0, avgTime * 1000.0,
             entry.callCount, percentage);
    }
  }

  Printf("  Total Time: %.3f ms\n", totalTime * 1000.0);
}

double GLTFProfiler::GetTotalTime(const char *name) const {
  int index = FindEntry(name);
  return index >= 0 ? entries[index].totalTime : 0.0;
}

int GLTFProfiler::GetCallCount(const char *name) const {
  int index = FindEntry(name);
  return index >= 0 ? entries[index].callCount : 0;
}

int GLTFProfiler::FindEntry(const char *name) {
  for (int i = 0; i < entries.Size(); ++i) {
    if (strcmp(entries[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

//===========================================================================
//
// GLTFMemoryTracker Implementation
//
//===========================================================================

void GLTFMemoryTracker::RecordAllocation(void *ptr, size_t size,
                                         const char *desc) {
  if (!ptr) {
    return;
  }

  AllocationInfo info;
  info.size = size;
  info.description = desc;
  info.timestamp = std::chrono::high_resolution_clock::now();

  allocations[ptr] = info;
  totalAllocated += size;
  allocationCount++;

  if (totalAllocated > peakUsage) {
    peakUsage = totalAllocated;
  }

  GLTF_MEM_ALLOC(size, desc);
}

void GLTFMemoryTracker::RecordDeallocation(void *ptr) {
  if (!ptr) {
    return;
  }

  auto it = allocations.CheckKey(ptr);
  if (it) {
    const auto &info = *it;
    totalAllocated -= info.size;
    GLTF_MEM_FREE(info.size, info.description);
    allocations.Remove(ptr);
  }
}

void GLTFMemoryTracker::Reset() {
  allocations.Clear();
  totalAllocated = 0;
  peakUsage = 0;
  allocationCount = 0;
}

void GLTFMemoryTracker::PrintReport() const {
  Printf("[glTF MEMORY] Memory Usage Report:\n");
  Printf("  Current Allocated: %.2f KB\n", totalAllocated / 1024.0);
  Printf("  Peak Usage: %.2f KB\n", peakUsage / 1024.0);
  Printf("  Total Allocations: %d\n", allocationCount);
  Printf("  Outstanding Allocations: %d\n", allocations.CountUsed());

  if (allocations.CountUsed() > 0) {
    Printf("  Outstanding allocations:\n");

    TMap<const char *, size_t> summaryByType;

    for (auto &pair : allocations) {
      const auto &info = pair.Value;
      auto existing = summaryByType.CheckKey(info.description);
      if (existing) {
        *existing += info.size;
      } else {
        summaryByType[info.description] = info.size;
      }
    }

    for (auto &pair : summaryByType) {
      Printf("    %s: %.2f KB\n", pair.Key, pair.Value / 1024.0);
    }
  }
}

//===========================================================================
//
// GLTFValidator Implementation
//
//===========================================================================

bool GLTFValidator::ValidateVector3(const FVector3 &vec, const char *name) {
  bool valid =
      std::isfinite(vec.X) && std::isfinite(vec.Y) && std::isfinite(vec.Z);
  GLTF_VALIDATE(valid, va("Invalid Vector3 '%s': non-finite values", name));
  return valid;
}

bool GLTFValidator::ValidateVector4(const FVector4 &vec, const char *name) {
  bool valid = std::isfinite(vec.X) && std::isfinite(vec.Y) &&
               std::isfinite(vec.Z) && std::isfinite(vec.W);
  GLTF_VALIDATE(valid, va("Invalid Vector4 '%s': non-finite values", name));
  return valid;
}

bool GLTFValidator::ValidateQuaternion(const FQuaternion &quat,
                                       const char *name) {
  bool finite = std::isfinite(quat.X) && std::isfinite(quat.Y) &&
                std::isfinite(quat.Z) && std::isfinite(quat.W);
  GLTF_VALIDATE(finite, va("Invalid Quaternion '%s': non-finite values", name));

  if (finite) {
    float length = sqrt(quat.X * quat.X + quat.Y * quat.Y + quat.Z * quat.Z +
                        quat.W * quat.W);
    bool normalized = fabs(length - 1.0f) < 0.001f;
    GLTF_VALIDATE(
        normalized,
        va("Quaternion '%s' not normalized (length: %.6f)", name, length));
    return normalized;
  }

  return false;
}

bool GLTFValidator::ValidateMatrix(const VSMatrix &matrix, const char *name) {
  const float *m = matrix.get()[0];
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(m[i])) {
      GLTF_VALIDATE(
          false,
          va("Invalid Matrix '%s': non-finite value at index %d", name, i));
      return false;
    }
  }
  return true;
}

bool GLTFValidator::ValidateUVCoordinates(float u, float v, const char *name) {
  bool valid = std::isfinite(u) && std::isfinite(v);
  GLTF_VALIDATE(valid,
                va("Invalid UV coordinates '%s': non-finite values", name));

  if (valid && (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)) {
    GLTF_VALIDATE(false,
                  va("UV coordinates '%s' outside [0,1] range: (%.3f, %.3f)",
                     name, u, v));
  }

  return valid;
}

bool GLTFValidator::ValidateColorValue(float value, const char *name) {
  bool valid = std::isfinite(value) && value >= 0.0f && value <= 1.0f;
  GLTF_VALIDATE(valid, va("Invalid color value '%s': %.3f (should be in [0,1])",
                          name, value));
  return valid;
}

bool GLTFValidator::ValidateArrayBounds(int index, int size, const char *name) {
  bool valid = index >= 0 && index < size;
  GLTF_VALIDATE(valid,
                va("Array bounds check failed for '%s': index %d, size %d",
                   name, index, size));
  return valid;
}

bool GLTFValidator::ValidateFileOffset(size_t offset, size_t fileSize,
                                       size_t dataSize, const char *name) {
  bool valid = offset + dataSize <= fileSize;
  GLTF_VALIDATE(valid, va("File offset validation failed for '%s': offset %zu "
                          "+ size %zu > file size %zu",
                          name, offset, dataSize, fileSize));
  return valid;
}

#endif // NEODOOM_GLTF_SUPPORT