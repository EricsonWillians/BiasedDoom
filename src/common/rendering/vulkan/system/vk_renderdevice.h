#pragma once

#include "gl_sysfb.h"
#include "engineerrors.h"
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanobjects.h>

struct FRenderViewpoint;
class VkSamplerManager;
class VkBufferManager;
class VkTextureManager;
class VkShaderManager;
class VkCommandBufferManager;
class VkDescriptorSetManager;
class VkRenderPassManager;
class VkFramebufferManager;
class VkRaytrace;
class VkRenderState;
class VkStreamBuffer;
class VkHardwareDataBuffer;
class VkHardwareTexture;
class VkRenderBuffers;
class VkPostprocess;
class SWSceneDrawer;

class VulkanRenderDevice : public SystemBaseFrameBuffer
{
	typedef SystemBaseFrameBuffer Super;


public:
	std::shared_ptr<VulkanDevice> device;

	VkCommandBufferManager* GetCommands() { return mCommands.get(); }
	VkShaderManager *GetShaderManager() { return mShaderManager.get(); }
	VkSamplerManager *GetSamplerManager() { return mSamplerManager.get(); }
	VkBufferManager* GetBufferManager() { return mBufferManager.get(); }
	VkTextureManager* GetTextureManager() { return mTextureManager.get(); }
	VkFramebufferManager* GetFramebufferManager() { return mFramebufferManager.get(); }
	VkDescriptorSetManager* GetDescriptorSetManager() { return mDescriptorSetManager.get(); }
	VkRenderPassManager *GetRenderPassManager() { return mRenderPassManager.get(); }
	VkRaytrace* GetRaytrace() { return mRaytrace.get(); }
	VkRenderState *GetRenderState() { return mRenderState.get(); }
	VkPostprocess *GetPostprocess() { return mPostprocess.get(); }
	VkRenderBuffers *GetBuffers() { return mActiveRenderBuffers; }
	FRenderState* RenderState() override;

	unsigned int GetLightBufferBlockSize() const;

	VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanSurface> surface);
	~VulkanRenderDevice();
	bool IsVulkan() override { return true; }

	void Update() override;

	void InitializeState() override;
	bool CompileNextShader() override;
	void PrecacheMaterial(FMaterial *mat, int translation) override;
	void UpdatePalette() override;
	const char* DeviceName() const override;
	int Backend() override { return 1; }
	void SetTextureFilterMode() override;
	void StartPrecaching() override;
	void BeginFrame() override;
	void InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData) override;
	bool SupportsSectorBleed() const override { return true; }
	void InitSectorBleed(int width, int height, const TArray<uint8_t>& data) override;
	void BlurScene(float amount) override;
	void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) override;
	void AmbientOccludeScene(float m5) override;
	void SetSceneRenderTarget(bool useSSAO) override;
	void SetLevelMesh(hwrenderer::LevelMesh* mesh) override;
	void UpdateShadowMap() override;
	void SetSaveBuffers(bool yes) override;
	void ImageTransitionScene(bool unknown) override;
	void SetActiveRenderTarget() override;

	IHardwareTexture *CreateHardwareTexture(int numchannels) override;
	FMaterial* CreateMaterial(FGameTexture* tex, int scaleflags) override;
	IVertexBuffer *CreateVertexBuffer() override;
	IIndexBuffer *CreateIndexBuffer() override;
	IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;

	FTexture *WipeStartScreen() override;
	FTexture *WipeEndScreen() override;

	TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) override;
	TArray<uint8_t> GetVideoCaptureBuffer(int &width, int &height, int &pitch, ESSType &color_type, float &gamma,
		uint64_t requestTimeNS, uint64_t &captureTimeNS, bool &pending, bool &bottomUp, bool issueNext) override;
	bool HasPendingVideoCapture() const override;
	void ResetVideoCapture() override;
	void AbandonPendingVideoCaptureReadbacks() override;

	bool GetVSync() { return mVSync; }
	void SetVSync(bool vsync) override;

	void Draw2D() override;

	void WaitForCommands(bool finish) override;

	bool RaytracingEnabled();

private:
	struct FVideoReadbackSlot;

	void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc) override;
	void PrintStartupLog();
	void CopyScreenToBuffer(int w, int h, uint8_t *data) override;
	bool IssueVideoReadback(uint64_t requestTimeNS);
	TArray<uint8_t> ConsumeVideoReadback(int &width, int &height, int &pitch, ESSType &color_type,
		float &gamma, uint64_t &captureTimeNS, bool &bottomUp);
	void ReleaseVideoReadback();

	std::unique_ptr<VkCommandBufferManager> mCommands;
	std::unique_ptr<VkBufferManager> mBufferManager;
	std::unique_ptr<VkSamplerManager> mSamplerManager;
	std::unique_ptr<VkTextureManager> mTextureManager;
	std::unique_ptr<VkFramebufferManager> mFramebufferManager;
	std::unique_ptr<VkShaderManager> mShaderManager;
	std::unique_ptr<VkRenderBuffers> mScreenBuffers;
	std::unique_ptr<VkRenderBuffers> mSaveBuffers;
	std::unique_ptr<VkPostprocess> mPostprocess;
	std::unique_ptr<VkDescriptorSetManager> mDescriptorSetManager;
	std::unique_ptr<VkRenderPassManager> mRenderPassManager;
	std::unique_ptr<VkRaytrace> mRaytrace;
	std::unique_ptr<VkRenderState> mRenderState;
	// The normal Vulkan frame fence is completed before the next Update(), so
	// one persistent, frame-delayed slot removes capture's old allocation and
	// mid-frame WaitForCommands() without changing the renderer's lifetime
	// model or keeping unbounded GPU/CPU memory alive.
	std::unique_ptr<FVideoReadbackSlot> mVideoReadback;
	// A discarded slot's image and staging buffer live in DrawDeleteList until
	// the next command-manager retirement boundary. Do not allocate a replacement
	// in that interval: one skipped sample keeps the capture footprint bounded
	// across stop/restart and output-size changes.
	bool mVideoReadbackRetirementPending = false;
	// An optional capture allocation failure must reach the recorder as a real
	// unavailable frame.  The old resources may still need one deferred-delete
	// boundary, but treating that boundary as ordinary readback progress would
	// otherwise make the recorder retry a permanent OOM forever.
	bool mVideoReadbackIssueFailed = false;

	VkRenderBuffers *mActiveRenderBuffers = nullptr;

	bool mVSync = false;
};

class CVulkanError : public CEngineError
{
public:
	CVulkanError() : CEngineError() {}
	CVulkanError(const char* message) : CEngineError(message) {}
};
