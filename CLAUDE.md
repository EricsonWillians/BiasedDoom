# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

BiasedDoom is a modern fork of GZDoom that extends the classic DOOM engine with native glTF 2.0 support, enabling skeletal animations, PBR materials, and seamless Blender workflows while maintaining backward compatibility with traditional DOOM assets.

## Build System

This project uses **CMake** as its primary build system with vcpkg for dependency management.

### Common Commands

**Build the project:**
```bash
# Configure with CMake
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release

# For development builds
cmake --build build --config Debug
```

**Clean build:**
```bash
rm -rf build/
cmake -B build -S .
cmake --build build
```

### vcpkg Dependencies

The project uses vcpkg for cross-platform dependency management. Key dependencies are defined in `vcpkg.json`:
- bzip2, libvpx (conditional)
- GTK3 and glib (Linux only)
- OpenAL (optional via OPENAL_SOFT_VCPKG flag)

## Architecture Overview

### Core Engine Structure

- **src/**: Main source directory containing engine core
  - **common/**: Shared utilities and models system
    - **models/**: Model loading and rendering system (MD2, MD3, IQM, OBJ, UE1 formats)
  - **gamedata/**: Game logic, actors, weapons, map info, and scripting integration
  - **rendering/**: Rendering subsystem with hardware and software renderers
    - **hwrenderer/**: OpenGL/Vulkan hardware acceleration
    - **swrenderer/**: Software fallback renderer
  - **scripting/**: ZScript/DECORATE scripting engine and VM
  - **playsim/**: Game simulation, physics, and actor management
  - **sound/**: Audio system integration

### Model System Architecture

The engine supports multiple model formats through a unified interface:
- Traditional: MD2, MD3, KVX (voxels), OBJ
- Modern: IQM (Inter-Quake Model)
- **Future**: glTF 2.0 support for skeletal animation and PBR materials

Model rendering is handled through `FModelRenderer` with bone animation support via `DBoneComponents`.

### Scripting System

- **ZScript**: Primary scripting language for game logic
- **DECORATE**: Legacy actor definition format (still supported)
- **ACS**: Classic scripting for level scripting
- VM-based execution with native function bindings

### Asset Pipeline

The project follows DOOM's traditional WAD-based asset system but extends it for modern workflows:
- **wadsrc/**: Source assets that get compiled into WAD files
- **fm_banks/**: MIDI FM synthesis banks
- **soundfont/**: Audio sample banks

## Development Workflow

### Testing

The project includes continuous integration via GitHub Actions (`.github/workflows/continuous_integration.yml`) that builds on:
- Windows (Visual Studio 2022)
- macOS (with Homebrew dependencies)
- Linux platforms

### Platform-Specific Notes

**Windows**: Uses static linking with MSVC runtime
**macOS**: Requires MoltenVK and Vulkan-Volk for Vulkan support
**Linux**: GTK3 and glib dependencies for GUI elements

### Code Organization

- Engine core follows traditional DOOM architecture with modern C++17 features
- Header files use `#pragma once` for include guards
- Platform abstraction in `src/posix/` and `src/win32/`
- OpenGL/Vulkan rendering abstracted through hardware renderer interface

## glTF 2.0 Integration Implementation Plan

### Technical Architecture Analysis

Based on comprehensive analysis of the GZDoom codebase, the following implementation plan provides a roadmap for integrating native glTF 2.0 support while maintaining full backward compatibility with existing model formats.

#### 1. Core Model System Extension

**Base Architecture**: The existing model system utilizes a polymorphic hierarchy rooted in `FModel` with format-specific implementations:
- `FDMDModel`/`FMD2Model`: Legacy Quake-style formats
- `FMD3Model`: Quake 3 format with multi-mesh support
- `IQMModel`: Advanced format with skeletal animation capabilities
- `FVoxelModel`: Volumetric representation for KVX format

**Integration Point**: Create `FGLTFModel` inheriting from `FModel`, following the established pattern in `src/common/models/model_*.h`.

```cpp
// Proposed: src/common/models/model_gltf.h
class FGLTFModel : public FModel
{
    struct GLTFMesh {
        TArray<FModelVertex> vertices;
        TArray<unsigned int> indices;
        FString materialName;
        PBRMaterialProperties pbrProperties;
    };

    struct GLTFScene {
        TArray<GLTFMesh> meshes;
        TArray<GLTFNode> nodes;
        TArray<GLTFAnimation> animations;
    };

private:
    GLTFScene scene;
    TArray<FGameTexture*> textures;
    std::unique_ptr<GLTFLoader> loader;

public:
    bool Load(const char* path, int lumpnum, const char* buffer, int length) override;
    void BuildVertexBuffer(FModelRenderer* renderer) override;
    int FindFrame(const char* name, bool nodefault) override;
    // ... Additional overrides
};
```

#### 2. Skeletal Animation Integration

**Current System**: The engine implements a sophisticated bone animation system via `DBoneComponents` (src/common/models/bonecomponents.h) with:
- `ModelAnim` structures for animation state management
- `BoneOverride` system for runtime bone manipulation
- Template-based interpolation framework (`BoneOverrideComponent<T>`)

**glTF Integration Strategy**:
1. **Bone Mapping**: Map glTF joint hierarchy to GZDoom's bone index system
2. **Animation Conversion**: Transform glTF animation channels to GZDoom's `ModelAnimFrameInterp` format
3. **Interpolation Bridge**: Utilize existing `InterpolateQuat()` and vector interpolation functions

```cpp
// Animation integration approach
struct GLTFAnimationChannel {
    int targetNodeIndex;
    AnimationPath path; // Translation, Rotation, Scale
    TArray<float> timestamps;
    TArray<TRS> values;
};

class GLTFAnimationConverter {
    static void ConvertToGZDoomFormat(
        const GLTFAnimation& gltfAnim,
        ModelAnim& gzAnim,
        TArray<ModelAnimFramePrecalculatedIQM>& frames
    );
};
```

#### 3. PBR Material System Implementation

**Current Material Pipeline**: GZDoom's material system (`src/common/textures/hw_material.h`) supports:
- `FMaterial` base class with multi-layer texture support
- `MaterialLayerInfo` for texture composition
- Hardware texture abstraction via `IHardwareTexture`

**PBR Extension Requirements**:
1. **Material Property Extension**: Extend `MaterialLayers` struct to include PBR parameters
2. **Texture Binding**: Map glTF material textures to GZDoom texture slots
3. **Shader Integration**: Modify hardware renderers to support metallic-roughness workflow

```cpp
// Proposed PBR material extension
struct PBRMaterialProperties {
    FVector4 baseColorFactor = FVector4(1,1,1,1);
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    FVector3 emissiveFactor = FVector3(0,0,0);

    FGameTexture* baseColorTexture = nullptr;
    FGameTexture* metallicRoughnessTexture = nullptr;
    FGameTexture* normalTexture = nullptr;
    FGameTexture* occlusionTexture = nullptr;
    FGameTexture* emissiveTexture = nullptr;
};

// Integration with existing material system
class FGLTFMaterial : public FMaterial {
    PBRMaterialProperties pbrProps;
public:
    FGLTFMaterial(FGameTexture* tex, const PBRMaterialProperties& pbr);
    void SetupPBRUniforms(FRenderState& state);
};
```

#### 4. Renderer Integration Architecture

**Hardware Renderer Integration**: The `FHWModelRenderer` class (src/rendering/hwrenderer/hw_models.h) provides the interface between models and OpenGL/Vulkan renderers.

**Implementation Strategy**:
1. **Vertex Buffer Extensions**: Modify `FModelVertexBuffer` to support additional vertex attributes (tangents, vertex colors)
2. **Shader Pipeline**: Extend hardware shaders to handle PBR lighting calculations
3. **Uniform Management**: Integrate PBR material properties into the existing uniform buffer system

```cpp
// Vertex format extension for glTF
struct FGLTFVertex : public FModelVertex {
    FVector4 tangent;    // w component for handedness
    FVector4 color0;     // Vertex colors (optional)
    FVector2 texCoord1;  // Secondary UV set (optional)
};

// Renderer integration
class FGLTFModelRenderer : public FHWModelRenderer {
    void SetupPBRMaterial(const PBRMaterialProperties& props);
    void BindPBRTextures(const FGLTFMaterial& material);
};
```

#### 5. File System Integration

**Loader Architecture**: Follow the established pattern in `src/common/models/model.cpp`:
1. **Format Detection**: Add glTF/GLB magic number detection to model loading pipeline
2. **Library Integration**: Integrate a header-only glTF library (recommended: fastgltf or tinygltf)
3. **Memory Management**: Utilize GZDoom's memory allocation patterns and TArray containers

```cpp
// Integration point in model loading
FModel* LoadModel(const char* path) {
    // Existing format checks...

    // Add glTF detection
    if (IsGLTFFile(buffer, length)) {
        auto model = new FGLTFModel();
        if (model->Load(path, lumpnum, buffer, length)) {
            return model;
        }
        delete model;
    }

    return nullptr;
}
```

#### 6. Blender Workflow Integration

**Export Pipeline Optimization**:
1. **Validation System**: Implement glTF validation to ensure Blender-exported files meet BiasedDoom requirements
2. **Asset Preprocessing**: Optional build-time optimization for glTF assets (mesh optimization, texture compression)
3. **Error Reporting**: Comprehensive error messages for common Blender export issues

**ZScript Integration**:
```cpp
// Actor definition with glTF model
class MyCyberDemon : Actor {
    default {
        Model.Path "models/cyberdemon.glb";
        Model.Animation "idle";
        Model.PBREnabled true;
        Model.Scale 1.0;
    }
}
```

#### 7. Memory and Performance Considerations

**Optimization Strategy**:
1. **Lazy Loading**: Load glTF animations and textures on-demand
2. **GPU Skinning**: Leverage existing bone buffer system for hardware-accelerated animation
3. **LOD Support**: Implement glTF level-of-detail extensions for performance scaling
4. **Caching**: Utilize GZDoom's existing texture and model caching infrastructure

#### 8. Implementation Phase Plan

**Phase 1: Foundation** (Core Integration)
- Add FGLTFModel class with basic mesh loading
- Integrate glTF library dependency via vcpkg
- Implement basic material property loading

**Phase 2: Animation System** (Skeletal Animation)
- Map glTF joint system to GZDoom bone components
- Implement animation conversion and playback
- Add bone override compatibility

**Phase 3: PBR Pipeline** (Material System)
- Extend material system for PBR properties
- Modify hardware shaders for metallic-roughness workflow
- Implement texture binding pipeline

**Phase 4: Optimization** (Performance & Polish)
- GPU skinning optimization
- Memory usage optimization
- Comprehensive error handling and validation

This implementation plan leverages GZDoom's robust architecture while introducing modern 3D asset capabilities, ensuring seamless integration with existing game logic and maximum compatibility with Blender's glTF 2.0 export functionality.

## Important Files

- `CMakeLists.txt`: Main build configuration
- `vcpkg.json`: Dependency specifications
- `src/version.h`: Version and build information
- `src/doomdef.h`: Core engine definitions and constants
- `src/common/models/`: Model loading and animation system