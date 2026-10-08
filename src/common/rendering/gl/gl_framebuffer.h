#ifndef __GL_FRAMEBUFFER
#define __GL_FRAMEBUFFER

#include "gl_sysfb.h"
#include "gl_load.h"
#include "m_png.h"

#include <memory>

namespace OpenGLRenderer
{

class FHardwareTexture;
class FGLDebug;

class OpenGLFrameBuffer : public SystemGLFrameBuffer
{
	typedef SystemGLFrameBuffer Super;

	void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc) override;

public:

	OpenGLFrameBuffer(void *hMonitor, bool fullscreen) ;
	~OpenGLFrameBuffer();
	int Backend() override { return 2; }
	bool CompileNextShader() override;
	void InitializeState() override;
	void Update() override;

	void AmbientOccludeScene(float m5) override;
	void FirstEye() override;
	void NextEye(int eyecount) override;
	void SetSceneRenderTarget(bool useSSAO) override;
	void UpdateShadowMap() override;
	void WaitForCommands(bool finish) override;
	void SetSaveBuffers(bool yes) override;
	void CopyScreenToBuffer(int width, int height, uint8_t* buffer) override;
	bool FlipSavePic() const override { return true; }

	FRenderState* RenderState() override;
	void UpdatePalette() override;
	const char* DeviceName() const override;
	void SetTextureFilterMode() override;
	IHardwareTexture *CreateHardwareTexture(int numchannels) override;
	void PrecacheMaterial(FMaterial *mat, int translation) override;
	void BeginFrame() override;
	void SetViewportRects(IntRect *bounds) override;
	void BlurScene(float amount) override;
	IVertexBuffer *CreateVertexBuffer() override;
	IIndexBuffer *CreateIndexBuffer() override;
	IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;

	void InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData) override;
	bool SupportsSectorBleed() const override;
	void InitSectorBleed(int width, int height, const TArray<uint8_t>& data) override;

	// Retrieves a buffer containing image data for a screenshot.
	// Hint: Pitch can be negative for upside-down images, in which case buffer
	// points to the last row in the buffer, which will be the first row output.
	virtual TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) override;
	void GetVideoCaptureDimensions(int &width, int &height) const override;
	TArray<uint8_t> GetVideoCaptureBuffer(int &width, int &height, int &pitch, ESSType &color_type, float &gamma,
		uint64_t requestTimeNS, uint64_t &captureTimeNS, bool &pending, bool &bottomUp, bool issueNext) override;
	bool HasPendingVideoCapture() const override;
	void ResetVideoCapture() override;
	void AbandonPendingVideoCaptureReadbacks() override;
	bool CanRenderNextFrame() override;
	bool ConsumeVideoCaptureBackpressureFailure() override;
	// A default-depth OpenGL renderer normally glFinish()s every swap. That
	// would immediately wait on a capture PBO and turn the asynchronous path
	// back into a per-frame stall, so Swap keeps it deferred while any capture
	// transfer is outstanding. A full ring is capture back-pressure, not a
	// reason to drain the whole graphics queue on the render thread.
	bool HasPendingVideoReadback() const;

	void Swap();
	bool IsHWGammaActive() const { return HWGammaActive; }

	void SetVSync(bool vsync) override;

	void Draw2D() override;
	void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) override;

	bool HWGammaActive = false;			// Are we using hardware or software gamma?
	std::unique_ptr<FGLDebug> mDebug;	// Debug API

    FTexture *WipeStartScreen() override;
    FTexture *WipeEndScreen() override;

	int camtexcount = 0;

private:
	struct FVideoReadbackSlot
	{
		GLuint Buffer = 0;
		GLsync Fence = nullptr;
		int SourceLeft = 0;
		int SourceTop = 0;
		int SourceWidth = 0;
		int SourceHeight = 0;
		int TargetWidth = 0;
		int TargetHeight = 0;
		uint64_t AllocatedBytes = 0;
		uint64_t CaptureTimeNS = 0;
		uint64_t Sequence = 0;
		// A healthy asynchronous transfer is normally ready by its next
		// zero-time poll. Keep a tiny count so a driver that does not retire
		// PBO fences without being given a bounded chance to run can recover
		// without turning every presentation into an unbounded glFinish().
		uint8_t DeferredPolls = 0;
		bool Pending = false;
		// A reset drops an in-flight image from the recording, but keeps its PBO
		// and fence alive until a zero-wait poll proves they are safe to delete.
		// That avoids turning stop, resize, or an immediate restart into glFinish.
		bool Discard = false;
	};

	// This is deliberately separate from the PBO ring. PBO fences bound capture
	// storage, whereas these end-of-frame fences bound ordinary GL command
	// submission while recording has suppressed the legacy glFinish() path.
	// Without this small queue, a vsync-off driver could accumulate unrelated
	// rendering work until it eventually stalls much more severely.
	struct FVideoFramePacingFence
	{
		GLsync Fence = nullptr;
		uint64_t Sequence = 0;
	};

	static constexpr unsigned int VideoReadbackSlotCount = 3;
	static constexpr unsigned int VideoFramePacingFenceCount = 3;
	FVideoReadbackSlot mVideoReadbacks[VideoReadbackSlotCount];
	FVideoFramePacingFence mVideoFramePacingFences[VideoFramePacingFenceCount];
	uint64_t mNextVideoReadbackSequence = 1;
	uint64_t mNextVideoFramePacingSequence = 1;
	uint8_t mVideoFramePacingSaturationPolls = 0;
	bool mVideoFramePacingSaturated = false;
	bool mVideoFramePacingFailurePending = false;
	bool mVideoFramePacingFailureReported = false;
	bool mVideoFramePacingFenceCreationFailed = false;
	FVideoReadbackSlot *OldestVideoReadback();
	FVideoReadbackSlot *FreeVideoReadback();
	FVideoFramePacingFence *OldestVideoFramePacingFence();
	void ReleaseVideoReadback(FVideoReadbackSlot &slot);
	void RecycleVideoReadback(FVideoReadbackSlot &slot);
	void RetireDiscardedVideoReadbacks();
	void AbandonVideoReadbacks();
	void AbandonDiscardedVideoReadbacks();
	void ReleaseVideoFramePacingFences();
	void RetireVideoFramePacingFences();
	void NoteVideoFramePacingFailure();
	void ThrottleVideoFramePacing();
	void QueueVideoFramePacingFence();
	bool IssueVideoReadback(uint64_t requestTimeNS);
	TArray<uint8_t> ConsumeVideoReadback(FVideoReadbackSlot &slot, int &width, int &height, int &pitch,
		ESSType &color_type, float &gamma, uint64_t &captureTimeNS, bool &bottomUp);
};

}

#endif //__GL_FRAMEBUFFER
