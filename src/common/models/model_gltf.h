#pragma once

// Conditional compilation for glTF support
#ifdef NEODOOM_GLTF_SUPPORT

#include <stdint.h>
#include <memory>
#include <fastgltf/types.hpp>
#include "model.h"
#include "vectors.h"
#include "matrix.h"
#include "tarray.h"
#include "name.h"
#include "bonecomponents.h"
#include "xs_Float.h"
#include "m_alloc.h"

// Forward declarations
class FModelRenderer;
class FGameTexture;
struct FLevelLocals;

namespace fastgltf {
    class Asset;
    struct Scene;
    struct Node;
    struct Mesh;
    struct Material;
    struct Animation;
    struct Accessor;
    struct BufferView;
    struct Buffer;
}

//===========================================================================
//
// Error Handling and Validation (moved up for complete types)
//
//===========================================================================

enum class GLTFError
{
    None = 0,
    InvalidFormat,
    UnsupportedVersion,
    MissingRequiredData,
    CorruptedBuffer,
    OutOfMemory,
    LibraryError,
    TextureLoadFailure,
    AnimationError,
    ValidationFailure
};

struct GLTFLoadResult
{
    GLTFError error = GLTFError::None;
    FString errorMessage;
    int errorLine = -1;

    bool IsSuccess() const { return error == GLTFError::None; }
    void SetError(GLTFError err, const char* msg, int line = -1);
    void Clear();
};

//===========================================================================
//
// Performance and Memory Management (moved up so class can embed by value)
//
//===========================================================================

struct GLTFLoadOptions
{
    bool validateOnLoad = true;
    bool generateMissingNormals = true;
    bool generateMissingTangents = true;
    bool optimizeMeshes = true;
    bool preloadTextures = false;
    int maxBoneInfluences = 4;
    float animationTolerance = 0.001f;

    // Memory limits
    size_t maxVertexCount = 1000000;
    size_t maxTriangleCount = 2000000;
    size_t maxTextureSize = 4096;
};

//===========================================================================
//
// PBR Material Properties
//
//===========================================================================

struct PBRMaterialProperties
{
    FVector4 baseColorFactor = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    FVector3 emissiveFactor = FVector3(0.0f, 0.0f, 0.0f);
    double alphaCutoff = 0.5;
    bool doubleSided = false;

    // Texture indices (into GLTFModel texture array)
    int baseColorTextureIndex = -1;
    int metallicRoughnessTextureIndex = -1;
    int normalTextureIndex = -1;
    int occlusionTextureIndex = -1;
    int emissiveTextureIndex = -1;

    // Texture coordinate indices
    int baseColorTexCoord = 0;
    int metallicRoughnessTexCoord = 0;
    int normalTexCoord = 0;
    int occlusionTexCoord = 0;
    int emissiveTexCoord = 0;
};

//===========================================================================
//
// glTF Extended Vertex Format
//
//===========================================================================

struct FGLTFVertex : public FModelVertex
{
    FVector4 tangent = FVector4(0.0f, 0.0f, 0.0f, 1.0f);    // w component for handedness
    FVector4 color0 = FVector4(1.0f, 1.0f, 1.0f, 1.0f);     // Vertex colors
    FVector2 texCoord1 = FVector2(0.0f, 0.0f);               // Secondary UV set
    uint8_t boneIndices[4] = {0, 0, 0, 0};                   // Bone indices for skinning
    float boneWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};        // Bone weights for skinning
};

//===========================================================================
//
// glTF Scene Components
//
//===========================================================================

struct GLTFMesh
{
    FString name;
    TArray<FGLTFVertex> vertices;
    TArray<unsigned int> indices;
    PBRMaterialProperties material;
    int materialIndex = -1;
    FTextureID skin = FNullTextureID();
};

struct GLTFNode
{
    FString name;
    int parentIndex = -1;
    TArray<int> childIndices;

    // Transformation
    TRS transform;
    VSMatrix localMatrix;
    VSMatrix globalMatrix;

    // Content
    int meshIndex = -1;
    int skinIndex = -1;
    bool isBone = false;
    int boneIndex = -1;
};

struct GLTFSkin
{
    FString name;
    TArray<int> jointIndices;          // Node indices that are joints
    TArray<VSMatrix> inverseBindMatrices;
    int skeletonRootIndex = -1;
};

struct GLTFAnimationSampler
{
    int inputAccessorIndex = -1;       // Time values
    int outputAccessorIndex = -1;      // Animation values
    FString interpolation = "LINEAR";   // LINEAR, STEP, CUBICSPLINE
};

struct GLTFAnimationChannel
{
    int samplerIndex = -1;
    int targetNodeIndex = -1;
    FString targetPath;                // "translation", "rotation", "scale", "weights"
};

struct GLTFAnimation
{
    FString name;
    TArray<GLTFAnimationSampler> samplers;
    TArray<GLTFAnimationChannel> channels;
    float duration = 0.0f;
};

struct GLTFScene
{
    TArray<GLTFMesh> meshes;
    TArray<GLTFNode> nodes;
    TArray<GLTFSkin> skins;
    TArray<GLTFAnimation> animations;
    TArray<int> rootNodeIndices;
};

//===========================================================================
//
// FGLTFModel - Main glTF Model Class
//
//===========================================================================

class FGLTFModel : public FModel
{
private:
    GLTFScene scene;
    TArray<FGameTexture*> textures;
    TArray<TArray<uint8_t>> buffers;           // Raw buffer data
    std::unique_ptr<fastgltf::Asset> asset;    // fastgltf parsed data
    int mLumpNum = -1;
    FString basePath;                          // Base path for loading external resources

    // Error tracking and validation
    mutable GLTFLoadResult lastError;
    GLTFLoadOptions loadOptions;
    bool isValid = false;

    // Animation state
    TArray<ModelAnim> modelAnimations;
    int currentAnimationIndex = -1;
    double lastAnimationTime = 0.0;

    // Bone management
    TArray<TRS> basePose;                      // Rest pose bone transforms
    TArray<VSMatrix> boneMatrices;             // Current bone matrices for GPU
    bool hasSkinning = false;
    int maxBonesPerVertex = 4;

    // Performance tracking
    mutable int framesSinceLoad = 0;
    mutable double totalLoadTime = 0.0;
    mutable size_t memoryUsage = 0;

public:
    FGLTFModel();
    virtual ~FGLTFModel();

    // Core FModel interface
    virtual bool Load(const char* path, int lumpnum, const char* buffer, int length) override;
    virtual void BuildVertexBuffer(FModelRenderer* renderer) override;
    virtual int FindFrame(const char* name, bool nodefault = false) override;
    virtual void RenderFrame(FModelRenderer* renderer, FGameTexture* skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition) override;
    virtual void AddSkins(uint8_t* hitlist, const FTextureID* surfaceskinids) override;

    // Enhanced interface with options and error handling
    bool LoadWithOptions(const char* path, int lumpnum, const char* buffer, int length, const GLTFLoadOptions& options);

    // Error and validation interface
    const GLTFLoadResult& GetLastError() const { return lastError; }
    bool IsValid() const { return isValid; }
    void SetLoadOptions(const GLTFLoadOptions& options) { loadOptions = options; }
    const GLTFLoadOptions& GetLoadOptions() const { return loadOptions; }

    // Performance and memory interface
    size_t GetMemoryUsage() const { return memoryUsage; }
    double GetLoadTime() const { return totalLoadTime; }
    void GetPerformanceStats(size_t& memory, double& loadTime, int& frames) const;

    // Enhanced validation
    bool ValidateModel(GLTFLoadResult& result) const;
    bool ValidateAnimations(GLTFLoadResult& result) const;
    bool ValidateMaterials(GLTFLoadResult& result) const;

    // glTF specific functionality
    const GLTFScene& GetScene() const { return scene; }
    const TArray<FGameTexture*>& GetTextures() const { return textures; }
    bool HasPBRMaterials() const;
    bool HasSkinning() const { return hasSkinning; }

    // Animation interface
    int GetAnimationCount() const { return modelAnimations.Size(); }
    const char* GetAnimationName(int index) const;
    float GetAnimationDuration(int index) const;
    void SetCurrentAnimation(int index);
    void UpdateAnimation(double currentTime, TArray<VSMatrix>& outBoneMatrices);
    int FindAnimation(const char* name) const;  // Find animation by name

    // Bone interface for skeletal animation
    int GetBoneCount() const;
    const char* GetBoneName(int index) const;
    int FindBone(const char* name) const;
    bool GetBoneTransform(int boneIndex, TRS& outTransform) const;
    bool GetBoneWorldTransform(int boneIndex, VSMatrix& outMatrix) const;
    const TArray<TRS>& GetBasePose() const { return basePose; }
    const TArray<VSMatrix>& GetBoneMatrices() const { return boneMatrices; }

private:
    // Loading implementation
    bool LoadGLTF(const char* buffer, int length);
    bool LoadGLB(const char* buffer, int length);
    bool ProcessAsset();

    // Scene processing
    bool ProcessBuffers();
    bool LoadExternalBuffer(const char* uri, TArray<uint8_t>& outData);
    bool ProcessTextures();
    bool ProcessMaterials();
    bool ProcessMeshes();
    bool ProcessNodes();
    bool ProcessSkins();
    bool ProcessAnimations();

    // Helper functions with error handling
    bool ReadAccessor(int accessorIndex, TArray<uint8_t>& outData, int& outCount, int& outStride);
    template<typename T>
    bool ReadAccessorTyped(int accessorIndex, TArray<T>& outData);
    bool ReadAccessorSafe(int accessorIndex, TArray<uint8_t>& outData, int& outCount, int& outStride, GLTFLoadResult& result);

    void ComputeNodeTransforms();
    void BuildBoneHierarchy();
    FGameTexture* LoadTextureFromGLTF(int textureIndex, GLTFLoadResult& result);
    FGameTexture* LoadTextureFromURI(const char* uri, GLTFLoadResult& result);
    FGameTexture* LoadTextureFromBufferView(size_t bufferViewIndex, GLTFLoadResult& result);

    // Animation helpers with validation
    bool ConvertGLTFAnimation(const fastgltf::Animation& gltfAnim, GLTFAnimation& outAnim, GLTFLoadResult& result);
    bool SampleAnimation(const GLTFAnimation& anim, float time, TArray<TRS>& outBoneTransforms);
    bool ValidateAnimationData(const GLTFAnimation& anim, GLTFLoadResult& result) const;

    // Enhanced validation and error handling
    bool ValidateAsset(GLTFLoadResult& result) const;
    bool ValidateBuffers(GLTFLoadResult& result) const;
    bool ValidateAccessors(GLTFLoadResult& result) const;
    bool ValidateNodes(GLTFLoadResult& result) const;
    void PrintLoadInfo() const;
    void PrintErrorDetails(const GLTFLoadResult& result) const;

    // Memory management helpers
    void CleanupResources();
    bool CheckMemoryLimits(GLTFLoadResult& result) const;
    void UpdateMemoryUsage();

    // Safe mesh loading with bounds checking
    bool LoadMeshPrimitive(const fastgltf::Primitive& primitive, GLTFMesh& mesh, GLTFLoadResult& result);
    bool LoadMaterial(int materialIndex, PBRMaterialProperties& material, GLTFLoadResult& result);

    // Utility functions
    bool IsBufferValid(int bufferIndex) const;
    bool IsAccessorValid(int accessorIndex) const;
    bool IsNodeValid(int nodeIndex) const;
    const char* GetErrorString(GLTFError error) const;

    // Rendering helpers (implemented in model_gltf_render.cpp)
    void BuildVertexData(FModelRenderer* renderer, ModelRendererType rendererType);
    void UploadVertexData(IModelVertexBuffer* buffer, const TArray<FModelVertex>& vertices, const TArray<unsigned int>& indices);
    void UploadBoneData(FModelRenderer* renderer);
    void RenderMeshWithPBR(FModelRenderer* renderer, const GLTFMesh& mesh, FGameTexture* skin, FTranslationID translation, size_t vertexOffset, int boneStartPosition);
    void RenderMeshStandard(FModelRenderer* renderer, const GLTFMesh& mesh, FGameTexture* skin, FTranslationID translation, size_t vertexOffset, int boneStartPosition);
    void UpdateAnimationState(double currentTime);
};

//===========================================================================
//
// Global Functions
//
//===========================================================================


//===========================================================================
//
// Global Functions
//
//===========================================================================

// Check if buffer contains glTF data with detailed error reporting
bool IsGLTFFile(const char* buffer, int length, GLTFLoadResult* result = nullptr);
bool IsGLBFile(const char* buffer, int length, GLTFLoadResult* result = nullptr);

// Validation functions
bool ValidateGLTFBuffer(const char* buffer, int length, GLTFLoadResult& result);
bool ValidateGLBHeader(const char* buffer, int length, GLTFLoadResult& result);

#else // !NEODOOM_GLTF_SUPPORT

// Stub functions when glTF support is disabled
inline bool IsGLTFFile(const char*, int, void* = nullptr) { return false; }
inline bool IsGLBFile(const char*, int, void* = nullptr) { return false; }

#endif // NEODOOM_GLTF_SUPPORT
