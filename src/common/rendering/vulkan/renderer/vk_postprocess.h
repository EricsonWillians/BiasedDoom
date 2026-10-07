
#pragma once

#include <functional>
#include <map>
#include <array>

#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "zvulkan/vulkanobjects.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/textures/vk_imagetransition.h"

class FString;

class VkPPShader;
class VkPPTexture;
class PipelineBarrier;
class VulkanRenderDevice;

class VkPostprocess
{
public:
	VkPostprocess(VulkanRenderDevice* fb);
	~VkPostprocess();

	void SetActiveRenderTarget();
	void PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D);

	void AmbientOccludeScene(float m5);
	void BlurScene(float gameinfobluramount);
	void ClearTonemapPalette();

	void UpdateShadowMap();

	void ImageTransitionScene(bool undefinedSrcLayout);

	void BlitSceneToPostprocess();
	void BlitCurrentToImage(VkTextureImage *image, VkImageLayout finallayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	// A continuous capture can render the final-present pass directly into a
	// caller-owned RGBA8 texture.  That avoids the old R16F pipeline hop and
	// full-image blit before copying the pixels to a staging buffer.  The normal
	// live-present and one-shot screenshot paths leave `output` null.
	void DrawPresentTexture(const IntRect &box, bool applyGamma, bool screenshot, PPTexture *output = nullptr);

	int GetCurrentPipelineImage() const { return mCurrentPipelineImage; }
	void SetCurrentPipelineImage(int image) { mCurrentPipelineImage = image; }

private:
	void NextEye(int eyeCount);

	VulkanRenderDevice* fb = nullptr;

	int mCurrentPipelineImage = 0;

	friend class VkPPRenderState;
};
