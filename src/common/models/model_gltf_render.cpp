//
//---------------------------------------------------------------------------
//
// Copyright(C) 2025 NeoDoom Team
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

#include "printf.h"
#include "doomdef.h"
#include "modelrenderer.h"
#include "texturemanager.h"
#include "i_time.h"
#include "hw_renderstate.h"
#include "hw_material_pbr.h"
#include "bitmap.h"
#include "image.h"
#include "textures.h"
#include "v_video.h"
#include "hw_bonebuffer.h"

//===========================================================================
//
// Colored Image Source for Materials Without Textures
//
//===========================================================================

class FGLTFColoredImage : public FImageSource
{
    PalEntry color;

public:
    FGLTFColoredImage(int r, int g, int b, int a = 255)
        : FImageSource(-1)  // -1 = no lump, procedurally generated
    {
        Width = 8;
        Height = 8;
        color = PalEntry(a, r, g, b);

        // CRITICAL: Mark as true-color image, not using game palette
        bUseGamePalette = false;
        bMasked = false;        // Solid color, no transparency
        bTranslucent = 0;       // Fully opaque
    }

    PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override
    {
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

    int CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override
    {
        // Create an 8x8 bitmap with the solid color
        // This is used for hardware rendering via texture upload
        bmp->Create(Width, Height);

        // Get direct access to pixel data (BGRA format, 4 bytes per pixel)
        uint8_t* pixels = bmp->GetPixels();

        // Fill entire bitmap with the solid color
        for (int i = 0; i < Width * Height; ++i) {
            int offset = i * 4;
            pixels[offset + 0] = color.r;  // Red
            pixels[offset + 1] = color.g;  // Green
            pixels[offset + 2] = color.b;  // Blue
            pixels[offset + 3] = color.a;  // Alpha

        }

        // Return 0 to indicate success (non-transparent texture)
        return 0;
    }
};

namespace
{
static VSMatrix TRSToMatrix(const TRS& transform)
{
    VSMatrix mat;
    mat.loadIdentity();
    mat.translate(transform.translation.X, transform.translation.Y, transform.translation.Z);
    mat.multQuaternion(transform.rotation);
    mat.scale(transform.scaling.X != 0.0f ? transform.scaling.X : 1.0f,
              transform.scaling.Y != 0.0f ? transform.scaling.Y : 1.0f,
              transform.scaling.Z != 0.0f ? transform.scaling.Z : 1.0f);
    return mat;
}

// Helper function to create a colored texture for materials without textures
static FGameTexture* CreateColoredTexture(const FVector4& color)
{
    // Convert 0-1 range to 0-255
    int r = clamp(int(color.X * 255.0f), 0, 255);
    int g = clamp(int(color.Y * 255.0f), 0, 255);
    int b = clamp(int(color.Z * 255.0f), 0, 255);
    int a = clamp(int(color.W * 255.0f), 0, 255);

    // Create a unique name for this color
    FString texName;
    texName.Format("GLTFColor_%02X%02X%02X%02X", r, g, b, a);

    // Don't bother checking cache - just create a static texture once at startup
    // (The cache check was failing anyway, causing texture recreation every frame)
    static TMap<uint32_t, FGameTexture*> colorTextureCache;
    uint32_t colorKey = (r << 24) | (g << 16) | (b << 8) | a;

    auto cached = colorTextureCache.CheckKey(colorKey);
    if (cached) {
        DPrintf(DMSG_NOTIFY, "Using cached color texture\n");
        return *cached;
    }

    DPrintf(DMSG_NOTIFY, "Creating NEW color texture\n");

    // Create new colored image source
    FImageSource* imgSrc = new FGLTFColoredImage(r, g, b, a);

    // Create texture from image source
    FImageTexture* tex = new FImageTexture(imgSrc, 0);

    // Wrap in FGameTexture
    FGameTexture* gameTex = new FGameTexture(tex, texName.GetChars());

    // Add to texture manager
    FTextureID texID = TexMan.AddGameTexture(gameTex);

    // FORCE the texture to generate its pixels NOW by calling GetTexture
    // This ensures CopyPixels is called and the texture has actual data
    auto hwTexture = gameTex->GetTexture();
    DPrintf(DMSG_NOTIFY, "Forced texture creation\n");

    // Cache it
    colorTextureCache[colorKey] = gameTex;

    DPrintf(DMSG_NOTIFY, "Added to TexMan and cached\n");
    DPrintf(DMSG_NOTIFY, "Created glTF colored texture '%s' (RGBA: %d,%d,%d,%d)\n",
           texName.GetChars(), r, g, b, a);

    return gameTex;
}

} // namespace

//===========================================================================
//
// Vertex Buffer Implementation
//
//===========================================================================

void FGLTFModel::BuildVertexBuffer(FModelRenderer* renderer)
{
    if (!renderer || !isValid) {
        DPrintf(DMSG_ERROR, "Cannot build vertex buffer: invalid renderer or model\n");
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

        for (const auto& mesh : scene.meshes) {
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

        auto* vb = renderer->CreateVertexBuffer(needIndex, singleFrame);
        SetVertexBuffer(rendererType, vb);

        if (!GetVertexBuffer(rendererType)) {
            DPrintf(DMSG_ERROR, "Failed to create vertex buffer for glTF model\n");
            return;
        }

        // Build vertex data
        BuildVertexData(renderer, rendererType);

        DPrintf(DMSG_NOTIFY, "Built glTF vertex buffer: %zu vertices, %zu indices\n",
               totalVertices, totalIndices);

    } catch (const std::exception& e) {
        DPrintf(DMSG_ERROR, "Exception building glTF vertex buffer: %s\n", e.what());

        // Clean up on failure
        if (GetVertexBuffer(renderer->GetType())) {
            auto* doomed = GetVertexBuffer(renderer->GetType());
            delete doomed;
            SetVertexBuffer(renderer->GetType(), nullptr);
        }
    }
}

void FGLTFModel::BuildVertexData(FModelRenderer* renderer, ModelRendererType rendererType)
{
    auto* buffer = GetVertexBuffer(rendererType);
    if (!buffer) {
        return;
    }

    // Convert glTF vertices to GZDoom format
    TArray<FModelVertex> gzVertices;
    TArray<unsigned int> gzIndices;

    size_t vertexOffset = 0;

    for (const auto& mesh : scene.meshes) {
        // Convert vertices
        for (const auto& gltfVertex : mesh.vertices) {
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
            gzVertex.boneweight[0] = static_cast<uint8_t>(gltfVertex.boneWeights[0] * 255.0f);
            gzVertex.boneweight[1] = static_cast<uint8_t>(gltfVertex.boneWeights[1] * 255.0f);
            gzVertex.boneweight[2] = static_cast<uint8_t>(gltfVertex.boneWeights[2] * 255.0f);
            gzVertex.boneweight[3] = static_cast<uint8_t>(gltfVertex.boneWeights[3] * 255.0f);

            // Lightmap (not used for GLTF)
            gzVertex.lu = 0.0f;
            gzVertex.lv = 0.0f;
            gzVertex.lindex = -1.0f;

            gzVertices.Push(gzVertex);
        }

        // Convert indices with offset
        for (unsigned int index : mesh.indices) {
            gzIndices.Push(index + vertexOffset);
        }

        vertexOffset += mesh.vertices.Size();
    }

    // Upload to GPU
    UploadVertexData(buffer, gzVertices, gzIndices);

    // Handle bone data if present
    if (hasSkinning && boneMatrices.Size() != 0) {
        UploadBoneData(renderer);
    }
}

void FGLTFModel::UploadVertexData(IModelVertexBuffer* buffer,
                                  const TArray<FModelVertex>& vertices,
                                  const TArray<unsigned int>& indices)
{
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
    FModelVertex* vertptr = buffer->LockVertexBuffer(vertices.Size());
    unsigned int* indxptr = buffer->LockIndexBuffer(indices.Size());

    if (!vertptr) {
        DPrintf(DMSG_ERROR, "Failed to lock vertex buffer\n");
        return;
    }

    if (!indxptr) {
        DPrintf(DMSG_ERROR, "Failed to lock index buffer\n");
        buffer->UnlockVertexBuffer();
        return;
    }

    // Copy vertex data
    memcpy(vertptr, vertices.Data(), vertices.Size() * sizeof(FModelVertex));

    // Copy index data
    memcpy(indxptr, indices.Data(), indices.Size() * sizeof(unsigned int));

    // Unlock buffers to commit data to GPU
    buffer->UnlockVertexBuffer();
    buffer->UnlockIndexBuffer();

    DPrintf(DMSG_NOTIFY, "glTF vertex data uploaded successfully\n");
}

void FGLTFModel::UploadBoneData(FModelRenderer* renderer)
{
    if (!hasSkinning || boneMatrices.Size() == 0) {
        return;
    }

    DPrintf(DMSG_NOTIFY, "TODO: Implement bone data upload for glTF skinning\n");
    DPrintf(DMSG_NOTIFY, "  Bones: %d\n", boneMatrices.Size());

    // This would upload bone matrices to a uniform buffer or texture
    // for use by vertex shaders during skinning
}

//===========================================================================
//
// Rendering Implementation
//
//===========================================================================

void FGLTFModel::RenderFrame(
    FModelRenderer* renderer,
    FGameTexture* skin,
    int frame,
    int frame2,
    double inter,
    FTranslationID translation,
    const FTextureID* surfaceskinids,
    int boneStartPosition)
{
    if (!renderer || !isValid)
        return;

    framesSinceLoad++;

    try
    {
        //------------------------------------------------------------
        // 🔹 Animation handling
        //------------------------------------------------------------

        // If we have animations but haven't selected one yet, select animation 0
        if (scene.animations.Size() > 0 && currentAnimationIndex < 0)
        {
            SetCurrentAnimation(0);
            Printf("GLTF: Auto-selected animation 0 '%s' (duration=%.2fs)\n",
                   scene.animations[0].name.GetChars(),
                   scene.animations[0].duration);
        }

        // Automatically select animation by frame index (if frame is valid)
        if (frame != currentAnimationIndex &&
            frame >= 0 && frame < static_cast<int>(scene.animations.Size()))
        {
            SetCurrentAnimation(frame);
        }

        // Calculate bone matrices for animation if we have skinning
        int actualBoneStartPosition = -1;
        if (hasSkinning && scene.skins.Size() > 0)
        {
            // Advance animation clock if valid animation is active
            if (currentAnimationIndex >= 0 &&
                currentAnimationIndex < static_cast<int>(scene.animations.Size()))
            {
                double currentTime = I_GetTime() * (1.0 / TICRATE);

                // Debug animation progress every 60 frames
                static int debugCounter = 0;
                if (debugCounter++ % 60 == 0) {
                    double animTime = currentTime - lastAnimationTime;
                    double duration = scene.animations[currentAnimationIndex].duration;
                    Printf("GLTF Anim: time=%.2f/%.2f bones=%d\n",
                           animTime, duration, boneMatrices.Size());
                }

                UpdateAnimationState(currentTime);
            }

            // Upload bone matrices to GPU (following IQM pattern)
            if (boneStartPosition >= 0) {
                actualBoneStartPosition = boneStartPosition;
            } else if (boneMatrices.Size() > 0) {
                actualBoneStartPosition = screen->mBones->UploadBones(boneMatrices);
            }
        }

        //------------------------------------------------------------
        // 🔹 Rendering configuration
        //------------------------------------------------------------

        const bool usesPBR =
            HasPBRMaterials() && renderer->GetType() == GLModelRendererType;

        size_t vertexOffset = 0;

        //------------------------------------------------------------
        // 🔹 Iterate over all meshes in the scene
        //------------------------------------------------------------
        for (size_t meshIndex = 0; meshIndex < scene.meshes.Size(); ++meshIndex)
        {
            const auto& mesh = scene.meshes[meshIndex];
            FGameTexture* meshSkin = nullptr;

            //--------------------------------------------------------
            // 1. MODELDEF SurfaceSkin override
            //--------------------------------------------------------
            if (surfaceskinids && meshIndex < MD3_MAX_SURFACES &&
                surfaceskinids[meshIndex].isValid())
            {
                meshSkin = TexMan.GetGameTexture(surfaceskinids[meshIndex], true);
            }

            //--------------------------------------------------------
            // 2. Embedded texture from glTF material
            //--------------------------------------------------------
            if (!meshSkin &&
                mesh.material.baseColorTextureIndex >= 0 &&
                mesh.material.baseColorTextureIndex < textures.Size())
            {
                meshSkin = textures[mesh.material.baseColorTextureIndex];
            }

            //--------------------------------------------------------
            // 3. MODELDEF Skin or baseColorFactor fallback
            //--------------------------------------------------------
            if (!meshSkin)
            {
                const bool hasCustomColor =
                    (mesh.material.baseColorFactor.X != 1.0f ||
                     mesh.material.baseColorFactor.Y != 1.0f ||
                     mesh.material.baseColorFactor.Z != 1.0f ||
                     mesh.material.baseColorFactor.W != 1.0f);

                if (!hasCustomColor && skin)
                {
                    meshSkin = skin;
                    DPrintf(DMSG_NOTIFY, "Using MODELDEF skin fallback\n");
                }
                else if (hasCustomColor)
                {
                    DPrintf(DMSG_NOTIFY, "Will generate colored texture from baseColorFactor\n");
                }
            }

            //--------------------------------------------------------
            // 4. Neutralize Doom player/team translations for glTF colors
            //--------------------------------------------------------
            const bool usingGeneratedColor =
                (!mesh.material.baseColorTextureIndex ||
                 mesh.material.baseColorTextureIndex < 0);

            //--------------------------------------------------------
            // 5. Render mesh (PBR or Standard)
            //--------------------------------------------------------
            if (usesPBR)
            {
                RenderMeshWithPBR(renderer, mesh, meshSkin,
                                  usingGeneratedColor ? FTranslationID() : translation,
                                  vertexOffset, actualBoneStartPosition);
            }
            else
            {
                RenderMeshStandard(renderer, mesh, meshSkin,
                                   usingGeneratedColor ? FTranslationID() : translation,
                                   vertexOffset, actualBoneStartPosition);
            }

            vertexOffset += mesh.vertices.Size();
        }
    }
    catch (const std::exception& e)
    {
        DPrintf(DMSG_ERROR, "Exception rendering glTF frame: %s\n", e.what());
    }
}

void FGLTFModel::RenderMeshWithPBR(FModelRenderer* renderer, const GLTFMesh& mesh,
                                   FGameTexture* skin, FTranslationID translation,
                                   size_t vertexOffset, int boneStartPosition)
{
    // Set up PBR material
    const auto& pbrProps = mesh.material;

    DPrintf(DMSG_NOTIFY, "TODO: Implement PBR mesh rendering\n");
    DPrintf(DMSG_NOTIFY, "  Metallic: %.2f, Roughness: %.2f\n",
           pbrProps.metallicFactor, pbrProps.roughnessFactor);

    // This would:
    // 1. Create/get FPBRMaterial for this mesh
    // 2. Bind PBR textures and uniforms
    // 3. Select appropriate PBR shader
    // 4. Render with PBR lighting

    // Fall back to standard rendering for now
    RenderMeshStandard(renderer, mesh, skin, translation, vertexOffset, boneStartPosition);
}

void FGLTFModel::RenderMeshStandard(FModelRenderer* renderer, const GLTFMesh& mesh,
                                    FGameTexture* skin, FTranslationID translation,
                                    size_t vertexOffset, int boneStartPosition)
{
    // Validate material before rendering
    // glTF models may not have textures embedded, so we need to handle NULL skin
    if (!skin) {
        // Create a colored texture from the material's baseColorFactor
        // This properly renders materials that use only vertex colors or baseColorFactor
        DPrintf(DMSG_NOTIFY, "RenderMeshStandard: Creating colored texture\n");

        skin = CreateColoredTexture(mesh.material.baseColorFactor);

        if (!skin) {
            Printf("ERROR: Failed to create colored texture!\n");
            DPrintf(DMSG_ERROR, "Cannot render glTF mesh: failed to create colored texture\n");
            return;
        }
        DPrintf(DMSG_NOTIFY, "Created colored texture\n");
    } else {
        DPrintf(DMSG_NOTIFY, "Using provided skin\n");
    }

    // If we're using generated color from baseColorFactor, DO NOT apply actor translation.
    // Actor translation means tinting according to team colors, which is not desired
    // for materials that specify their own color.
    const bool usingGeneratedColor =
        (!mesh.material.baseColorTextureIndex || mesh.material.baseColorTextureIndex < 0);

    // Set material
    renderer->SetMaterial(skin, false, usingGeneratedColor ? FTranslationID() : translation);

    // Setup vertex/index buffers - CRITICAL for rendering!
    // This binds the vertex and index buffers to the rendering state
    // Pass bone start position for skeletal animation
    renderer->SetupFrame(this, vertexOffset, vertexOffset, mesh.vertices.Size(), boneStartPosition);

    // Render geometry
    if (mesh.indices.Size() == 0) {
        // Non-indexed rendering
        renderer->DrawArrays(vertexOffset, mesh.vertices.Size());
    } else {
        // Indexed rendering
        size_t indexOffset = 0;
        // Calculate proper index offset based on previous meshes
        for (size_t i = 0; i < scene.meshes.Size() && &scene.meshes[i] != &mesh; ++i) {
            indexOffset += scene.meshes[i].indices.Size();
        }

        renderer->DrawElements(mesh.indices.Size(), indexOffset * sizeof(unsigned int));
    }
}

void FGLTFModel::UpdateAnimationState(double currentTime)
{
    if (currentAnimationIndex < 0 || currentAnimationIndex >= scene.animations.Size()) {
        return;
    }

    // Simple time-based animation update
    const auto& anim = scene.animations[currentAnimationIndex];

    if (anim.duration > 0.0f) {
        // Calculate elapsed time since animation started and loop it
        float animTime = fmod(currentTime - lastAnimationTime, anim.duration);

        // Update bone matrices if we have skinning
        if (hasSkinning && scene.skins.Size() > 0) {
            TArray<TRS> boneTransforms;
            if (SampleAnimation(anim, animTime, boneTransforms)) {
                // Convert TRS to matrices
                for (int i = 0; i < boneTransforms.Size() && i < boneMatrices.Size(); ++i) {
                    boneMatrices[i] = TRSToMatrix(boneTransforms[i]);
                }
            }
        }
    }
    // Note: We do NOT update lastAnimationTime here - it stays at animation start time
    // This allows animTime to continuously increase and loop via fmod
}

//===========================================================================
//
// Animation and Skinning Interface
//
//===========================================================================

int FGLTFModel::FindFrame(const char* name, bool nodefault)
{
    if (!name || !*name) {
        return nodefault ? -1 : 0;
    }

    // Search animations by name
    for (int i = 0; i < scene.animations.Size(); ++i) {
        if (scene.animations[i].name.CompareNoCase(name) == 0) {
            return i;
        }
    }

    return nodefault ? -1 : 0;
}

void FGLTFModel::AddSkins(uint8_t* hitlist, const FTextureID* surfaceskinids)
{
    if (!hitlist) {
        return;
    }

    // Add all textures used by this model to the hitlist
    for (const auto& texture : textures) {
        if (texture && texture->GetID().isValid()) {
            int index = texture->GetID().GetIndex();
            if (index >= 0 && index < INT_MAX) {
                hitlist[index] = 1;
            }
        }
    }

    // Add surface skin overrides
    if (surfaceskinids) {
        for (size_t i = 0; i < scene.meshes.Size(); ++i) {
            if (surfaceskinids[i].isValid()) {
                int index = surfaceskinids[i].GetIndex();
                if (index >= 0 && index < INT_MAX) {
                    hitlist[index] = 1;
                }
            }
        }
    }
}

bool FGLTFModel::HasPBRMaterials() const
{
    for (const auto& mesh : scene.meshes) {
        const auto& material = mesh.material;

        // Check if this material uses PBR features
        if (material.metallicFactor != 1.0f ||
            material.roughnessFactor != 1.0f ||
            material.baseColorTextureIndex != -1 ||
            material.metallicRoughnessTextureIndex != -1 ||
            material.normalTextureIndex != -1 ||
            material.emissiveTextureIndex != -1) {
            return true;
        }
    }
    return false;
}

const char* FGLTFModel::GetAnimationName(int index) const
{
    if (index < 0 || index >= scene.animations.Size()) {
        return "";
    }
    return scene.animations[index].name.GetChars();
}

float FGLTFModel::GetAnimationDuration(int index) const
{
    if (index < 0 || index >= scene.animations.Size()) {
        return 0.0f;
    }
    return scene.animations[index].duration;
}

void FGLTFModel::SetCurrentAnimation(int index)
{
    if (index >= 0 && index < scene.animations.Size()) {
        currentAnimationIndex = index;
        lastAnimationTime = I_GetTime() * (1.0 / TICRATE);
    } else {
        currentAnimationIndex = -1;
    }
}

void FGLTFModel::UpdateAnimation(double currentTime, TArray<VSMatrix>& outBoneMatrices)
{
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
