/*
**  Vulkan backend
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include <zvulkan/vulkanobjects.h>

#include <cstring>
#include <inttypes.h>

#include "v_video.h"
#include "i_video.h"
#include "m_png.h"
#include "m_misc.h"

#include "r_videoscale.h"
#include "i_time.h"
#include "v_text.h"
#include "version.h"
#include "v_draw.h"

#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_cvars.h"
#include "hw_skydome.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"

#include "vk_renderdevice.h"
#include "vk_hwbuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/renderer/vk_streambuffer.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/renderer/vk_raytrace.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/textures/vk_framebuffer.h"
#include <zvulkan/vulkanswapchain.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkansurface.h>
#include <zvulkan/vulkancompatibledevice.h>
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_buffer.h"
#include "engineerrors.h"
#include "c_dispatch.h"

#include <new>

FString JitCaptureStackTrace(int framesToSkip, bool includeNativeFrames, int maxFrames = -1);

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)

CVAR(Bool, vk_raytrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Keep continuous capture bounded even on unusually large output modes. The
// recorder itself has a 128 MiB final-RGB frame ceiling; a Vulkan readback is
// RGBA, so a 96 MiB staging cap leaves room for the reusable conversion image
// and avoids a capture feature becoming a device-memory leak.
static constexpr size_t MAX_VIDEO_READBACK_STAGING_BYTES = 96ull * 1024ull * 1024ull;

struct VulkanRenderDevice::FVideoReadbackSlot
{
	FVideoReadbackSlot(int width, int height) : Presentation(width, height, PixelFormat::Rgba8)
	{
		Presentation.TransferSource = true;
	}

	// The present shader writes straight to this RGBA8 target.  It replaces the
	// old screenshot-style R16F pipeline target plus a second full-image blit.
	PPTexture Presentation;
	std::unique_ptr<VulkanBuffer> Staging;
	int Width = 0;
	int Height = 0;
	uint64_t CaptureTimeNS = 0;
	bool Pending = false;
};

// Physical device info
static std::vector<VulkanCompatibleDevice> SupportedDevices;
int vkversion;

CUSTOM_CVAR(Bool, vk_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CVAR(Bool, vk_debug_callstack, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, vk_device, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vk_listdevices)
{
	for (size_t i = 0; i < SupportedDevices.size(); i++)
	{
		Printf("#%d - %s\n", (int)i, SupportedDevices[i].Device->Properties.Properties.deviceName);
	}
}

void VulkanError(const char* text)
{
	throw CVulkanError(text);
}

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	bool showcallstack = strstr(typestr, "error") != nullptr;

	if (showcallstack)
		Printf("\n");

	Printf(TEXTCOLOR_RED "[%s] ", typestr);
	Printf(TEXTCOLOR_WHITE "%s\n", msg.c_str());

	if (vk_debug_callstack && showcallstack)
	{
		FString callstack = JitCaptureStackTrace(0, true, 5);
		if (!callstack.IsEmpty())
			Printf("%s\n", callstack.GetChars());
	}
}

VulkanRenderDevice::VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanSurface> surface) :
	Super(hMonitor, fullscreen) 
{
	VulkanDeviceBuilder builder;
	builder.OptionalRayQuery();
	builder.Surface(surface);
	builder.SelectDevice(vk_device);
	SupportedDevices = builder.FindDevices(surface->Instance);
	device = builder.Create(surface->Instance);
}

VulkanRenderDevice::~VulkanRenderDevice()
{
	vkDeviceWaitIdle(device->device); // make sure the GPU is no longer using any objects before RAII tears them down
	ResetVideoCapture();

	// PPShader backends in the global postprocess chain point back at this
	// device; reset them while it is still valid.
	PPResource::ResetAll();

	delete mVertexData;
	delete mSkyData;
	delete mViewpoints;
	delete mLights;
	delete mBones;
	mShadowMap.Reset();

	if (mDescriptorSetManager)
		mDescriptorSetManager->Deinit();
	if (mTextureManager)
		mTextureManager->Deinit();
	if (mBufferManager)
		mBufferManager->Deinit();
	if (mShaderManager)
		mShaderManager->Deinit();

	mCommands->DeleteFrameObjects();
}

void VulkanRenderDevice::InitializeState()
{
	static bool first = true;
	if (first)
	{
		PrintStartupLog();
		first = false;
	}

	// Use the same names here as OpenGL returns.
	switch (device->PhysicalDevice.Properties.Properties.vendorID)
	{
	case 0x1002: vendorstring = "ATI Technologies Inc.";     break;
	case 0x10DE: vendorstring = "NVIDIA Corporation";  break;
	case 0x8086: vendorstring = "Intel";   break;
	default:     vendorstring = "Unknown"; break;
	}

	hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
	glslversion = 4.50f;
	uniformblockalignment = (unsigned int)device->PhysicalDevice.Properties.Properties.limits.minUniformBufferOffsetAlignment;
	maxuniformblock = device->PhysicalDevice.Properties.Properties.limits.maxUniformBufferRange;

	mCommands.reset(new VkCommandBufferManager(this));

	mSamplerManager.reset(new VkSamplerManager(this));
	mTextureManager.reset(new VkTextureManager(this));
	mFramebufferManager.reset(new VkFramebufferManager(this));
	mBufferManager.reset(new VkBufferManager(this));
	mBufferManager->Init();

	mScreenBuffers.reset(new VkRenderBuffers(this));
	mSaveBuffers.reset(new VkRenderBuffers(this));
	mActiveRenderBuffers = mScreenBuffers.get();

	// The present postprocess shader receives PresentUniforms as push
	// constants; a device offering less than that makes every present pipeline
	// layout invalid. Fail here, before any postprocess pipeline is created.
	if (device->PhysicalDevice.Properties.Properties.limits.maxPushConstantsSize < (uint32_t)sizeof(PresentUniforms))
		I_FatalError("Vulkan cannot support the present postprocess uniforms on this device:\n"
			"maxPushConstantsSize is %u bytes but the present shader requires %u.\n"
			"Please use the OpenGL backend or a different GPU.",
			(unsigned int)device->PhysicalDevice.Properties.Properties.limits.maxPushConstantsSize,
			(unsigned int)sizeof(PresentUniforms));

	mPostprocess.reset(new VkPostprocess(this));
	mDescriptorSetManager.reset(new VkDescriptorSetManager(this));
	mRenderPassManager.reset(new VkRenderPassManager(this));
	mRaytrace.reset(new VkRaytrace(this));

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer;
	mLights = new FLightBuffer();
	mBones = new BoneBuffer();

	mShaderManager.reset(new VkShaderManager(this));
	mDescriptorSetManager->Init();
#ifdef __APPLE__
	mRenderState.reset(new VkRenderStateMolten(this));
#else
	mRenderState.reset(new VkRenderState(this));
#endif
}

void VulkanRenderDevice::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();

	GetPostprocess()->SetActiveRenderTarget();

	Draw2D();
	twod->Clear();

	// The screenshot command is queued by the input responder. Process it only
	// after this frame's 2D pass (automap, HUD, console) has been composed, while
	// the render targets and command stream still belong to this frame.
	M_ProcessPendingScreenShot();

	mRenderState->EndRenderPass();
	mRenderState->EndFrame();

	Flush3D.Unclock();

	WaitForCommands(true);
	mCommands->UpdateGpuStats();

	Super::Update();
}

bool VulkanRenderDevice::CompileNextShader()
{
	return mShaderManager->CompileNextShader();
}

void VulkanRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	auto BaseLayer = static_cast<VkHardwareTexture*>(tex->GetHardwareTexture(0, 0));

	VkTextureImage *image = BaseLayer->GetImage(tex, 0, 0);
	VkTextureImage *depthStencil = BaseLayer->GetDepthStencil(tex);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(image, depthStencil->View.get(), image->Image->width, image->Image->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = min(tex->GetWidth(), image->Image->width);
	bounds.height = min(tex->GetHeight(), image->Image->height);

	renderFunc(bounds);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples());

	tex->SetUpdated(true);
}

void VulkanRenderDevice::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) mPostprocess->BlitSceneToPostprocess(); // Copy the resulting scene to the current post process texture
	mPostprocess->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

const char* VulkanRenderDevice::DeviceName() const
{
	return device->PhysicalDevice.Properties.Properties.deviceName;
}

void VulkanRenderDevice::SetVSync(bool vsync)
{
	mVSync = vsync;
}

void VulkanRenderDevice::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	MaterialLayerInfo* layer;

	auto systex = static_cast<VkHardwareTexture*>(mat->GetLayer(0, translation, &layer));
	systex->GetImage(layer->layerTexture, translation, layer->scaleFlags);

	int numLayers = mat->NumLayers();
	for (int i = 1; i < numLayers; i++)
	{
		auto syslayer = static_cast<VkHardwareTexture*>(mat->GetLayer(i, 0, &layer));
		syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
	}
}

IHardwareTexture *VulkanRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new VkHardwareTexture(this, numchannels);
}

FMaterial* VulkanRenderDevice::CreateMaterial(FGameTexture* tex, int scaleflags)
{
	return new VkMaterial(this, tex, scaleflags);
}

IVertexBuffer *VulkanRenderDevice::CreateVertexBuffer()
{
	return GetBufferManager()->CreateVertexBuffer();
}

IIndexBuffer *VulkanRenderDevice::CreateIndexBuffer()
{
	return GetBufferManager()->CreateIndexBuffer();
}

IDataBuffer *VulkanRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return GetBufferManager()->CreateDataBuffer(bindingpoint, ssbo, needsresize);
}

void VulkanRenderDevice::SetTextureFilterMode()
{
	if (mSamplerManager)
	{
		mDescriptorSetManager->ResetHWTextureSets();
		mSamplerManager->ResetHWSamplers();
	}
}

void VulkanRenderDevice::StartPrecaching()
{
	// Destroy the texture descriptors to avoid problems with potentially stale textures.
	mDescriptorSetManager->ResetHWTextureSets();
}

void VulkanRenderDevice::BlurScene(float amount)
{
	if (mPostprocess)
		mPostprocess->BlurScene(amount);
}

void VulkanRenderDevice::UpdatePalette()
{
	if (mPostprocess)
		mPostprocess->ClearTonemapPalette();
}

FTexture *VulkanRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");

	return tex;
}

FTexture *VulkanRenderDevice::WipeEndScreen()
{
	GetPostprocess()->SetActiveRenderTarget();
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");

	return tex;
}

void VulkanRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data)
{
	VkTextureImage image;

	// Convert from rgba16f to rgba8 using the GPU:
	image.Image = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.Size(w, h)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	GetPostprocess()->BlitCurrentToImage(&image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// Staging buffer for download
	auto staging = BufferBuilder()
		.Size(w * h * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	// Copy from image to buffer
	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	mCommands->GetDrawCommands()->copyImageToBuffer(image.Image->image, image.Layout, staging->buffer, 1, &region);

	// Submit command buffers and wait for device to finish the work
	WaitForCommands(false);

	// Map and convert from rgba8 to rgb8
	uint8_t *dest = (uint8_t*)data;
	uint8_t *pixels = (uint8_t*)staging->Map(0, w * h * 4);
	if (pixels == nullptr)
	{
		return;
	}
	// GPU_TO_CPU allocations are not necessarily HOST_COHERENT. The command
	// wait above completes the copy, but non-coherent mappings still need an
	// explicit invalidate before their bytes are consumed by the CPU.
	staging->Invalidate(0, w * h * 4);
	int dindex = 0;
	for (int y = 0; y < h; y++)
	{
		int sindex = (h - y - 1) * w * 4;
		for (int x = 0; x < w; x++)
		{
			dest[dindex] = pixels[sindex];
			dest[dindex + 1] = pixels[sindex + 1];
			dest[dindex + 2] = pixels[sindex + 2];
			dindex += 3;
			sindex += 4;
		}
	}
	staging->Unmap();
}

void VulkanRenderDevice::SetActiveRenderTarget()
{
	mPostprocess->SetActiveRenderTarget();
}

TArray<uint8_t> VulkanRenderDevice::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = w;
	box.height = h;

	// The screenshot present pass renders into the next pipeline image and
	// advances its index. Save/restore the index so this frame's real present
	// still reads the composed frame instead of presenting the screenshot's
	// output image a second time (double gamma/atmosphere for one frame).
	int savedPipelineImage = mPostprocess->GetCurrentPipelineImage();
	mPostprocess->DrawPresentTexture(box, true, true);

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());
	mPostprocess->SetCurrentPipelineImage(savedPipelineImage);

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return ScreenshotBuffer;
}

//===========================================================================
//
// Bounded frame-delayed video readback
//
// A normal screenshot is intentionally synchronous: the caller needs pixels
// during this Update(). Continuous recording does not. Vulkan's frame manager
// completes its submitted work at the end of an Update(), so retain one
// staging image/buffer pair and consume it on the following capture request.
// This removes the old per-frame VMA allocations and the extra, mid-frame
// WaitForCommands(false) that made the renderer submit and stall twice for a
// single recorded frame. It deliberately does not alter the backend's global
// fence/deferred-destruction model.
//
//===========================================================================

void VulkanRenderDevice::ReleaseVideoReadback()
{
	if (!mVideoReadback)
	{
		return;
	}

	if (mCommands)
	{
		mVideoReadback->Presentation.ResetBackend();
		mCommands->DrawDeleteList->Add(std::move(mVideoReadback->Staging));
		// DrawDeleteList owns the old image/staging pair until the next command
		// retirement. Hold capture for that one boundary rather than allocating a
		// second large pair while the old one remains resident.
		mVideoReadbackRetirementPending = true;
	}
	else
	{
		mVideoReadback->Presentation.ResetBackend();
		mVideoReadback->Staging.reset();
	}
	mVideoReadback.reset();
}

void VulkanRenderDevice::ResetVideoCapture()
{
	mVideoReadbackIssueFailed = false;
	ReleaseVideoReadback();
}

void VulkanRenderDevice::AbandonPendingVideoCaptureReadbacks()
{
	// GetVideoCaptureBuffer() is serviced only after the previous Update()'s
	// WaitForCommands(true), so a pending slot here has completed GPU work. The
	// CPU writer cannot accept its pixels, however; discard that sample while
	// preserving the reusable presentation image and staging allocation.
	if (mVideoReadback && mVideoReadback->Pending)
	{
		mVideoReadback->CaptureTimeNS = 0;
		mVideoReadback->Pending = false;
	}
}

bool VulkanRenderDevice::IssueVideoReadback(uint64_t requestTimeNS)
{
	if (mVideoReadbackRetirementPending)
	{
		return false;
	}
	const int width = SCREENWIDTH;
	const int height = SCREENHEIGHT;
	if (width <= 0 || height <= 0)
	{
		return false;
	}
	const uint64_t pixels = (uint64_t)width * (uint64_t)height;
	if (pixels > std::numeric_limits<uint64_t>::max() / 4ull)
	{
		return false;
	}
	const uint64_t rgbaBytes = pixels * 4ull;
	if (rgbaBytes == 0 || rgbaBytes > MAX_VIDEO_READBACK_STAGING_BYTES)
	{
		return false;
	}

	if (mVideoReadback && (mVideoReadback->Pending || mVideoReadback->Width != width ||
		mVideoReadback->Height != height || mVideoReadback->Presentation.Backend == nullptr ||
		mVideoReadback->Staging == nullptr))
	{
		ReleaseVideoReadback();
		return false;
	}
	if (!mVideoReadback)
	{
		try
		{
			mVideoReadback = std::make_unique<FVideoReadbackSlot>(width, height);
			// GetTexture materializes the backend before DrawPresentTexture selects
			// it as an output target.  The PP texture remains persistent across the
			// take instead of allocating a capture image every sampled frame.
			mTextureManager->GetTexture(PPTextureType::PPTexture, &mVideoReadback->Presentation);
			mVideoReadback->Staging = BufferBuilder()
				.Size((size_t)rgbaBytes)
				.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
				.DebugName("VideoCaptureReadbackStaging")
				.Create(device.get());
			mVideoReadback->Width = width;
			mVideoReadback->Height = height;
		}
		catch (const CVulkanError &)
		{
			// Capture is optional.  A device-memory allocation failure must end the
			// take through the normal empty-frame path, not unwind through the render
			// loop or retain a partially materialized PP texture/staging buffer.
			mVideoReadbackIssueFailed = true;
			ReleaseVideoReadback();
			return false;
		}
		catch (const std::bad_alloc &)
		{
			// Keep host-allocation pressure symmetric with device allocation pressure:
			// discard the partially constructed optional capture slot and let the
			// recorder stop cleanly rather than turning it into a process failure.
			mVideoReadbackIssueFailed = true;
			ReleaseVideoReadback();
			return false;
		}
	}

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = width;
	box.height = height;

	// Draw the same fully postprocessed presentation texture a normal screenshot
	// uses, but directly into the persistent RGBA8 readback target.  The former
	// path rendered to the next R16F pipeline image and then blitted that whole
	// image again before the download.  Keeping this pass out of the pipeline
	// also avoids perturbing the live present's current-image index.
	VkTextureImage *captureImage = mTextureManager->GetTexture(PPTextureType::PPTexture,
		&mVideoReadback->Presentation);
	mPostprocess->DrawPresentTexture(box, true, true, &mVideoReadback->Presentation);
	VkImageTransition()
		.AddImage(captureImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	VkBufferImageCopy region = {};
	region.imageExtent.width = (uint32_t)width;
	region.imageExtent.height = (uint32_t)height;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	mCommands->GetDrawCommands()->copyImageToBuffer(captureImage->Image->image,
		captureImage->Layout, mVideoReadback->Staging->buffer, 1, &region);
		mVideoReadback->CaptureTimeNS = requestTimeNS;
		mVideoReadback->Pending = true;
		mVideoReadbackIssueFailed = false;
		return true;
}

TArray<uint8_t> VulkanRenderDevice::ConsumeVideoReadback(int &width, int &height, int &pitch, ESSType &color_type,
	float &gamma, uint64_t &captureTimeNS, bool &bottomUp)
{
	if (!mVideoReadback || !mVideoReadback->Pending || mVideoReadback->Staging == nullptr)
	{
		return TArray<uint8_t>();
	}

	const int captureWidth = mVideoReadback->Width;
	const int captureHeight = mVideoReadback->Height;
	const uint64_t rgbaBytes = (uint64_t)captureWidth * (uint64_t)captureHeight * 4ull;
	if (captureWidth <= 0 || captureHeight <= 0 || rgbaBytes > mVideoReadback->Staging->size ||
		rgbaBytes > 0xffffffffull)
	{
		ReleaseVideoReadback();
		return TArray<uint8_t>();
	}

	uint8_t *pixels = static_cast<uint8_t *>(mVideoReadback->Staging->Map(0, (size_t)rgbaBytes));
	if (pixels == nullptr)
	{
		ReleaseVideoReadback();
		return TArray<uint8_t>();
	}
	// The prior frame fence makes the transfer complete, but completion alone
	// does not invalidate a non-coherent GPU_TO_CPU mapping. Refresh its CPU
	// cache before copying native RGBA rows into the recorder-owned buffer.
	mVideoReadback->Staging->Invalidate(0, (size_t)rgbaBytes);

	// Keep the native rows in a single bulk copy. PNG and AVI already own a
	// background writer, so they perform the RGB packing/orientation there
	// instead of making every recorded frame run a per-pixel loop on the
	// renderer's critical path.
	TArray<uint8_t> result((unsigned int)rgbaBytes, true);
	memcpy(result.Data(), pixels, (size_t)rgbaBytes);
	mVideoReadback->Staging->Unmap();

	captureTimeNS = mVideoReadback->CaptureTimeNS;
	mVideoReadback->CaptureTimeNS = 0;
	mVideoReadback->Pending = false;
	width = captureWidth;
	height = captureHeight;
	pitch = captureWidth * 4;
	color_type = SS_RGBA;
	bottomUp = true;
	gamma = 1.0f;
	return result;
}

TArray<uint8_t> VulkanRenderDevice::GetVideoCaptureBuffer(int &width, int &height, int &pitch, ESSType &color_type,
	float &gamma, uint64_t requestTimeNS, uint64_t &captureTimeNS, bool &pending, bool &bottomUp, bool issueNext)
{
	width = SCREENWIDTH;
	height = SCREENHEIGHT;
	pending = false;
	bottomUp = false;
	captureTimeNS = requestTimeNS;
	if (mVideoReadbackRetirementPending)
	{
		// A discarded allocation still has to retire through DrawDeleteList, but
		// an allocation failure is terminal for this take.  Returning pending here
		// would otherwise turn a persistent device/host OOM into an endless stream
		// of deferred samples instead of letting the recorder begin its bounded
		// stop/drain path.
		pending = !mVideoReadbackIssueFailed;
		return TArray<uint8_t>();
	}

	// A delayed slot belongs to the output geometry that created it. During a
	// live take, never hand an old-resolution frame to the recorder as if it
	// described the current SCREENWIDTH/SCREENHEIGHT. During finalization,
	// however, its own stored dimensions remain valid: poll it once without
	// issuing a replacement so a resize cannot discard a ready tail frame.
	if (issueNext && mVideoReadback && mVideoReadback->Pending &&
		(mVideoReadback->Width != SCREENWIDTH || mVideoReadback->Height != SCREENHEIGHT))
	{
		ReleaseVideoReadback();
	}

	if (mVideoReadback && mVideoReadback->Pending)
	{
		TArray<uint8_t> result = ConsumeVideoReadback(width, height, pitch, color_type, gamma, captureTimeNS, bottomUp);
		if (result.Size() != 0)
		{
			// Queue the next transfer only after mapping the prior one. The normal
			// end-of-frame fence makes that prior slot safe to reuse here.
			if (issueNext)
			{
				IssueVideoReadback(requestTimeNS);
			}
			return result;
		}
	}

	if (issueNext && IssueVideoReadback(requestTimeNS))
	{
		pending = true;
		return TArray<uint8_t>();
	}
	if (mVideoReadbackRetirementPending)
	{
		pending = !mVideoReadbackIssueFailed;
		return TArray<uint8_t>();
	}

	// Do not fall back to the old synchronous screenshot path here: a very
	// large mode must fail the take cleanly instead of reintroducing the
	// mid-frame GPU wait and temporary allocations that this bounded path
	// exists to prevent. CaptureFrame reports the unavailable final frame and
	// stops the take without retaining a growing resource family.
	return TArray<uint8_t>();
}

bool VulkanRenderDevice::HasPendingVideoCapture() const
{
	return mVideoReadbackRetirementPending || (mVideoReadback != nullptr && mVideoReadback->Pending);
}

void VulkanRenderDevice::BeginFrame()
{
	SetViewportRects(nullptr);
	mViewpoints->Clear();
	mCommands->BeginFrame();
	mTextureManager->BeginFrame();
	mScreenBuffers->BeginFrame(screen->mScreenViewport.width, screen->mScreenViewport.height, screen->mSceneViewport.width, screen->mSceneViewport.height);
	mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT);
	mRenderState->BeginFrame();
	mDescriptorSetManager->BeginFrame();
}

void VulkanRenderDevice::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GetTextureManager()->SetLightmap(LMTextureSize, LMTextureCount, LMTextureData);

		// Keep the source pixels on SDL so a live backend switch can upload
		// them to the replacement device. Other platforms require a restart.
		if (!I_SupportsLiveBackendSwitch()) LMTextureData.Reset();
	}
}

void VulkanRenderDevice::InitSectorBleed(int width, int height, const TArray<uint8_t>& data)
{
	if (width > 0 && height > 0 && data.Size() >= unsigned(width * height * 4))
		GetTextureManager()->SetSectorBleed(width, height, data);
}

void VulkanRenderDevice::Draw2D()
{
	::Draw2D(twod, *mRenderState);
}

void VulkanRenderDevice::WaitForCommands(bool finish)
{
	mCommands->WaitForCommands(finish);
	// WaitForCommands() retires the current DrawDeleteList before returning, so
	// a later sampled capture can allocate after a discarded readback without
	// transiently keeping two large image/staging pairs alive.
	mVideoReadbackRetirementPending = false;
}

unsigned int VulkanRenderDevice::GetLightBufferBlockSize() const
{
	return mLights->GetBlockSize();
}

void VulkanRenderDevice::PrintStartupLog()
{
	const auto &props = device->PhysicalDevice.Properties.Properties;

	FString deviceType;
	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_OTHER: deviceType = "other"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "discrete gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "virtual gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "cpu"; break;
	default: deviceType.Format("%d", (int)props.deviceType); break;
	}

	FString apiVersion, driverVersion;
	apiVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	driverVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
	vkversion = VK_API_VERSION_MAJOR(props.apiVersion) * 100 + VK_API_VERSION_MINOR(props.apiVersion);

	Printf("Vulkan device: " TEXTCOLOR_ORANGE "%s\n", props.deviceName);
	Printf("Vulkan device type: %s\n", deviceType.GetChars());
	Printf("Vulkan version: %s (api) %s (driver)\n", apiVersion.GetChars(), driverVersion.GetChars());

	Printf(PRINT_LOG, "Vulkan extensions:");
	for (const VkExtensionProperties &p : device->PhysicalDevice.Extensions)
	{
		Printf(PRINT_LOG, " %s", p.extensionName);
	}
	Printf(PRINT_LOG, "\n");

	const auto &limits = props.limits;
	Printf("Max. texture size: %d\n", limits.maxImageDimension2D);
	Printf("Max. uniform buffer range: %d\n", limits.maxUniformBufferRange);
	Printf("Min. uniform buffer offset alignment: %" PRIu64 "\n", limits.minUniformBufferOffsetAlignment);
}

void VulkanRenderDevice::SetLevelMesh(hwrenderer::LevelMesh* mesh)
{
	mRaytrace->SetLevelMesh(mesh);
}

void VulkanRenderDevice::UpdateShadowMap()
{
	mPostprocess->UpdateShadowMap();
}

void VulkanRenderDevice::SetSaveBuffers(bool yes)
{
	if (yes) mActiveRenderBuffers = mSaveBuffers.get();
	else mActiveRenderBuffers = mScreenBuffers.get();
}

void VulkanRenderDevice::ImageTransitionScene(bool unknown)
{
	mPostprocess->ImageTransitionScene(unknown);
}

FRenderState* VulkanRenderDevice::RenderState()
{
	return mRenderState.get();
}

void VulkanRenderDevice::AmbientOccludeScene(float m5)
{
	mPostprocess->AmbientOccludeScene(m5);
}

void VulkanRenderDevice::SetSceneRenderTarget(bool useSSAO)
{
	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples());
}

bool VulkanRenderDevice::RaytracingEnabled()
{
	return vk_raytrace && device->SupportsExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
}
