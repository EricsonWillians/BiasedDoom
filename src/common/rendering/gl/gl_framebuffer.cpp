/*
** gl_framebuffer.cpp
** Implementation of the non-hardware specific parts of the
** OpenGL frame buffer
**
**---------------------------------------------------------------------------
** Copyright 2010-2020 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/ 

#include "gl_system.h"
#include "v_video.h"
#include "i_video.h"
#include "m_png.h"
#include "m_misc.h"

#include "i_time.h"

#include "gl_interface.h"
#include "gl_framebuffer.h"
#include "gl_renderer.h"
#include "gl_renderbuffers.h"
#include "gl_samplers.h"
#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_skydome.h"
#include "hw_viewpointbuffer.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "gl_shaderprogram.h"
#include "gl_debug.h"
#include "r_videoscale.h"
#include "gl_buffers.h"
#include "gl_postprocessstate.h"
#include "v_draw.h"
#include "printf.h"
#include "gl_hwtexture.h"

#include "flatvertices.h"
#include "hw_cvars.h"

EXTERN_CVAR (Bool, vid_vsync)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Int, gl_pipeline_depth);

void gl_LoadExtensions();
void gl_PrintStartupLog();

extern bool vid_hdr_active;

namespace OpenGLRenderer
{
	FGLRenderer *GLRenderer;

	// A fence normally retires through a zero-time poll. A few software and
	// remote GL stacks require a finite client wait before a PBO readback can
	// make forward progress, however. Use this only after several deferred
	// polls, and cap it tightly enough that a stalled GPU cannot reproduce the
	// old recording freeze.
	static constexpr GLuint64 VideoReadbackRecoveryWaitNS = 1000000ull;
	static constexpr uint8_t VideoReadbackRecoveryPollCount = 3;
	// Once the three end-of-frame fences are full, no additional rendering work
	// may be admitted until one retires. Each admission poll has a tiny finite
	// wait so a driver that needs a flush can recover, but a genuinely wedged GPU
	// never turns recording into an unbounded render-thread wait. About 120 ms of
	// bounded polling is long enough to smooth an ordinary transient stall while
	// still failing the take before it can make the desktop feel wedged.
	static constexpr GLuint64 VideoFramePacingRecoveryWaitNS = 1000000ull;
	static constexpr uint8_t VideoFramePacingMaxSaturationPolls = 120;

	// Keep the asynchronous capture path all-or-nothing.  In particular, a
	// context transition can leave synchronization entry points intact while
	// removing the map/unmap calls needed to consume an already-issued PBO.
	// Callers must tear down that old bookkeeping before falling back to the
	// synchronous screenshot path.
	static bool HasAsyncVideoReadbackSupport()
	{
		return glFenceSync != nullptr && glClientWaitSync != nullptr &&
			glMapBufferRange != nullptr && glDeleteSync != nullptr &&
			glUnmapBuffer != nullptr;
	}

	// The GL error flag is sticky. Capture is injected after arbitrary renderer
	// work, so a prior, unrelated error must not make a valid PBO allocation
	// look like an out-of-memory failure. Conversely, each allocation/readback
	// operation needs to consume its own errors before the slot is reused. Keep
	// the drain bounded in case a lost or non-conforming context reports an
	// error repeatedly.
	static bool VideoReadbackHasError()
	{
		if (glGetError == nullptr)
		{
			return false;
		}

		bool hadError = false;
		for (unsigned int count = 0; count < 16; ++count)
		{
			if (glGetError() == GL_NO_ERROR)
			{
				break;
			}
			hadError = true;
		}
		return hadError;
	}

	static void ClearVideoReadbackErrors()
	{
		(void)VideoReadbackHasError();
	}

//==========================================================================
//
//
//
//==========================================================================

OpenGLFrameBuffer::OpenGLFrameBuffer(void *hMonitor, bool fullscreen) : 
	Super(hMonitor, fullscreen) 
{
	// SetVSync needs to be at the very top to workaround a bug in Nvidia's OpenGL driver.
	// If wglSwapIntervalEXT is called after glBindFramebuffer in a frame the setting is not changed!
	Super::SetVSync(vid_vsync);
	FHardwareTexture::InitGlobalState();

	// Make sure all global variables tracking OpenGL context state are reset..
	gl_RenderState.Reset();

	GLRenderer = nullptr;
}

OpenGLFrameBuffer::~OpenGLFrameBuffer()
{
	// The recorder owns no GL resources outside this framebuffer. At renderer
	// teardown there is no later frame on which to poll retired work, so release
	// the bounded PBO/fence ring while the context is still valid.
	for (auto &slot : mVideoReadbacks)
	{
		ReleaseVideoReadback(slot);
	}
	ReleaseVideoFramePacingFences();
	PPResource::ResetAll();

	if (mVertexData != nullptr) delete mVertexData;
	if (mSkyData != nullptr) delete mSkyData;
	if (mViewpoints != nullptr) delete mViewpoints;
	if (mLights != nullptr) delete mLights;
	if (mBones != nullptr) delete mBones;
	mShadowMap.Reset();

	if (GLRenderer)
	{
		delete GLRenderer;
		GLRenderer = nullptr;
	}
}

//==========================================================================
//
// Initializes the GL renderer
//
//==========================================================================

void OpenGLFrameBuffer::InitializeState()
{
	static bool first=true;

	if (first)
	{
		if (ogl_LoadFunctions() == ogl_LOAD_FAILED)
		{
			I_FatalError("Failed to load OpenGL functions.");
		}
	}

	gl_LoadExtensions();

	mPipelineNbr = clamp(*gl_pipeline_depth, 1, HW_MAX_PIPELINE_BUFFERS);
	mPipelineType = gl_pipeline_depth > 0;

	// Move some state to the framebuffer object for easier access.
	hwcaps = gl.flags;
	glslversion = gl.glslversion;
	uniformblockalignment = gl.uniformblockalignment;
	maxuniformblock = gl.maxuniformblock;
	vendorstring = gl.vendorstring;

	if (first)
	{
		first=false;
		gl_PrintStartupLog();
	}

	glDepthFunc(GL_LESS);

	glEnable(GL_DITHER);
	glDisable(GL_CULL_FACE);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glEnable(GL_POLYGON_OFFSET_LINE);
	glEnable(GL_BLEND);
	glEnable(GL_DEPTH_CLAMP);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LINE_SMOOTH);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	SetViewportRects(nullptr);

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), screen->mPipelineNbr);
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer(screen->mPipelineNbr);
	mLights = new FLightBuffer(screen->mPipelineNbr);
	mBones = new BoneBuffer(screen->mPipelineNbr);
	GLRenderer = new FGLRenderer(this);
	GLRenderer->Initialize(GetWidth(), GetHeight());
	static_cast<GLDataBuffer*>(mLights->GetBuffer())->BindBase();
	static_cast<GLDataBuffer*>(mBones->GetBuffer())->BindBase();

	mDebug = std::make_unique<FGLDebug>();
	mDebug->Update();
}

//==========================================================================
//
// Updates the screen
//
//==========================================================================

void OpenGLFrameBuffer::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();
	GLRenderer->Flush();
	Flush3D.Unclock();

	// Capture after the complete 3D + 2D composition has reached the back
	// buffer, but before swapping invalidates that buffer's contents.
	M_ProcessPendingScreenShot();
	Swap();
	Super::Update();
}

void OpenGLFrameBuffer::CopyScreenToBuffer(int width, int height, uint8_t* scr)
{
	IntRect bounds;
	bounds.left = 0;
	bounds.top = 0;
	bounds.width = width;
	bounds.height = height;
	GLRenderer->CopyToBackbuffer(&bounds, false);

	// strictly speaking not needed as the glReadPixels should block until the scene is rendered, but this is to safeguard against shitty drivers
	glFinish();
	glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, scr);
}

//===========================================================================
//
// Camera texture rendering
//
//===========================================================================

void OpenGLFrameBuffer::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	GLRenderer->StartOffscreen();
	GLRenderer->BindToFrameBuffer(tex);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = FHardwareTexture::GetTexDimension(tex->GetWidth());
	bounds.height = FHardwareTexture::GetTexDimension(tex->GetHeight());

	renderFunc(bounds);
	GLRenderer->EndOffscreen();

	tex->SetUpdated(true);
	static_cast<OpenGLFrameBuffer*>(screen)->camtexcount++;
}

//===========================================================================
//
// 
//
//===========================================================================

const char* OpenGLFrameBuffer::DeviceName() const 
{
	return gl.modelstring;
}

//==========================================================================
//
// Swap the buffers
//
//==========================================================================

CVAR(Bool, gl_finishbeforeswap, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);

void OpenGLFrameBuffer::Swap()
{
	bool swapbefore = gl_finishbeforeswap && camtexcount == 0;
	// The traditional completion point forces a PBO readback to finish
	// immediately. Do not restore it merely because the capture ring is full:
	// that makes the nominally asynchronous path drain the GPU every few
	// presentations. The ring itself is the back-pressure boundary; when no
	// slot is available CaptureFrame simply skips the sample. Retired requests
	// are reclaimed with zero-time fence polls rather than a stop/resize finish.
	RetireDiscardedVideoReadbacks();
	RetireVideoFramePacingFences();
	const bool pendingVideoReadback = HasPendingVideoReadback();
	bool useVideoFramePacing = pendingVideoReadback && gl_pipeline_depth < 1 &&
		glFenceSync != nullptr && glClientWaitSync != nullptr && glDeleteSync != nullptr;
	// A PBO ring bounds capture storage, but not all of the unrelated rendering
	// commands a driver can queue while the legacy glFinish() is suppressed.
	// Hold at most three completed frame command streams instead. This waits for
	// one oldest frame only when the GPU is genuinely behind; it never drains the
	// whole queue just because a capture PBO is still transferring.
	if (useVideoFramePacing)
	{
		ThrottleVideoFramePacing();
		// A discard-only recovery may abandon PBOs after its bounded fence wait.
		// Re-evaluate before deciding whether the legacy finish path is safe.
		useVideoFramePacing = HasPendingVideoReadback() && gl_pipeline_depth < 1 &&
			glFenceSync != nullptr && glClientWaitSync != nullptr && glDeleteSync != nullptr;
	}
	// A discarded PBO still represents already submitted GPU work. Keep the
	// same bounded admission/fence policy until it retires or its bounded
	// discard-only recovery abandons it; otherwise suppressing glFinish() here
	// would let ordinary rendering accumulate without a completion bound.
	const bool waitForGpuAtSwap = !HasPendingVideoReadback() || !useVideoFramePacing;
	Finish.Reset();
	Finish.Clock();
	if (gl_pipeline_depth < 1)
	{
		if (swapbefore && waitForGpuAtSwap) glFinish();
		FPSLimit();
		SwapBuffers();
		if (useVideoFramePacing)
		{
			QueueVideoFramePacingFence();
		}
		if (!swapbefore && waitForGpuAtSwap) glFinish();
	}
	else
	{
		mVertexData->DropSync();

		FPSLimit();
		SwapBuffers();
		mVertexData->NextPipelineBuffer();
		mVertexData->WaitSync();

		RenderState()->SetVertexBuffer(screen->mVertexData); // Needed for Raze because it does not reset it
	}
	Finish.Unclock();
	camtexcount = 0;
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
	mDebug->Update();
}

//==========================================================================
//
// Enable/disable vertical sync
//
//==========================================================================

void OpenGLFrameBuffer::SetVSync(bool vsync)
{
	// Switch to the default frame buffer because some drivers associate the vsync state with the bound FB object.
	GLint oldDrawFramebufferBinding = 0, oldReadFramebufferBinding = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebufferBinding);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebufferBinding);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	Super::SetVSync(vsync);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldDrawFramebufferBinding);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, oldReadFramebufferBinding);
}

//===========================================================================
//
//
//===========================================================================

void OpenGLFrameBuffer::SetTextureFilterMode()
{
	if (GLRenderer != nullptr && GLRenderer->mSamplerManager != nullptr) GLRenderer->mSamplerManager->SetTextureFilterMode();
}

IHardwareTexture *OpenGLFrameBuffer::CreateHardwareTexture(int numchannels) 
{ 
	return new FHardwareTexture(numchannels);
}

void OpenGLFrameBuffer::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	int numLayers = mat->NumLayers();
	MaterialLayerInfo* layer;
	auto base = static_cast<FHardwareTexture*>(mat->GetLayer(0, translation, &layer));

	if (base->BindOrCreate(layer->layerTexture, 0, CLAMP_NONE, translation, layer->scaleFlags))
	{
		for (int i = 1; i < numLayers; i++)
		{
			auto systex = static_cast<FHardwareTexture*>(mat->GetLayer(i, 0, &layer));
			systex->BindOrCreate(layer->layerTexture, i, CLAMP_NONE, 0, layer->scaleFlags);
		}
	}
	// unbind everything. 
	FHardwareTexture::UnbindAll();
	gl_RenderState.ClearLastMaterial();
}

IVertexBuffer *OpenGLFrameBuffer::CreateVertexBuffer()
{ 
	return new GLVertexBuffer; 
}

IIndexBuffer *OpenGLFrameBuffer::CreateIndexBuffer()
{ 
	return new GLIndexBuffer; 
}

IDataBuffer *OpenGLFrameBuffer::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return new GLDataBuffer(bindingpoint, ssbo);
}

void OpenGLFrameBuffer::BlurScene(float amount)
{
	GLRenderer->BlurScene(amount);
}

void OpenGLFrameBuffer::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GLint activeTex = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
		glActiveTexture(GL_TEXTURE0 + 17);

		if (GLRenderer->mLightMapID == 0)
			glGenTextures(1, (GLuint*)&GLRenderer->mLightMapID);

		glBindTexture(GL_TEXTURE_2D_ARRAY, GLRenderer->mLightMapID);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB16F, LMTextureSize, LMTextureSize, LMTextureCount, 0, GL_RGB, GL_HALF_FLOAT, &LMTextureData[0]);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

		glActiveTexture(activeTex);

		// Keep the source pixels on SDL so a live backend switch can upload
		// them to the replacement device. Other platforms require a restart.
		if (!I_SupportsLiveBackendSwitch()) LMTextureData.Reset();
	}
}

bool OpenGLFrameBuffer::SupportsSectorBleed() const
{
	GLint textureUnits = 0;
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &textureUnits);
	return textureUnits > 18;
}

void OpenGLFrameBuffer::InitSectorBleed(int width, int height, const TArray<uint8_t>& data)
{
	if (!SupportsSectorBleed() || width <= 0 || height <= 0 || data.Size() < unsigned(width * height * 4))
		return;

	GLint activeTex = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
	glActiveTexture(GL_TEXTURE0 + 18);

	if (GLRenderer->mSectorBleedID == 0)
		glGenTextures(1, (GLuint*)&GLRenderer->mSectorBleedID);

	glBindTexture(GL_TEXTURE_2D, GLRenderer->mSectorBleedID);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.Data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glActiveTexture(activeTex);
}

void OpenGLFrameBuffer::SetViewportRects(IntRect *bounds)
{
	Super::SetViewportRects(bounds);
	if (!bounds)
	{
		auto vrmode = VRMode::GetVRMode(true);
		vrmode->AdjustViewport(this);
	}
}

void OpenGLFrameBuffer::UpdatePalette()
{
	if (GLRenderer)
		GLRenderer->ClearTonemapPalette();
}

FRenderState* OpenGLFrameBuffer::RenderState()
{
	return &gl_RenderState;
}

void OpenGLFrameBuffer::AmbientOccludeScene(float m5)
{
	gl_RenderState.EnableDrawBuffers(1);
	GLRenderer->AmbientOccludeScene(m5);
	glViewport(screen->mSceneViewport.left, mSceneViewport.top, mSceneViewport.width, mSceneViewport.height);
	GLRenderer->mBuffers->BindSceneFB(true);
	gl_RenderState.EnableDrawBuffers(gl_RenderState.GetPassDrawBufferCount());
	gl_RenderState.Apply();
}

void OpenGLFrameBuffer::FirstEye()
{
	GLRenderer->mBuffers->CurrentEye() = 0;  // always begin at zero, in case eye count changed
}

void OpenGLFrameBuffer::NextEye(int eyecount)
{
	GLRenderer->mBuffers->NextEye(eyecount);
}

void OpenGLFrameBuffer::SetSceneRenderTarget(bool useSSAO)
{
	GLRenderer->mBuffers->BindSceneFB(useSSAO);
}

void OpenGLFrameBuffer::UpdateShadowMap()
{
	if (mShadowMap.PerformUpdate())
	{
		FGLDebug::PushGroup("ShadowMap");

		FGLPostProcessState savedState;

		static_cast<GLDataBuffer*>(screen->mShadowMap.mLightList)->BindBase();
		static_cast<GLDataBuffer*>(screen->mShadowMap.mNodesBuffer)->BindBase();
		static_cast<GLDataBuffer*>(screen->mShadowMap.mLinesBuffer)->BindBase();

		GLRenderer->mBuffers->BindShadowMapFB();

		GLRenderer->mShadowMapShader->Bind();
		GLRenderer->mShadowMapShader->Uniforms->ShadowmapQuality = gl_shadowmap_quality;
		GLRenderer->mShadowMapShader->Uniforms->NodesCount = screen->mShadowMap.NodesCount();
		GLRenderer->mShadowMapShader->Uniforms.SetData();
		static_cast<GLDataBuffer*>(GLRenderer->mShadowMapShader->Uniforms.GetBuffer())->BindBase();

		glViewport(0, 0, gl_shadowmap_quality, 1024);
		GLRenderer->RenderScreenQuad();

		const auto& viewport = screen->mScreenViewport;
		glViewport(viewport.left, viewport.top, viewport.width, viewport.height);

		GLRenderer->mBuffers->BindShadowMapTexture(16);
		FGLDebug::PopGroup();
		screen->mShadowMap.FinishUpdate();
	}
}

void OpenGLFrameBuffer::WaitForCommands(bool finish)
{
	glFinish();
}

void OpenGLFrameBuffer::SetSaveBuffers(bool yes)
{
	if (!GLRenderer) return;
	if (yes) GLRenderer->mBuffers = GLRenderer->mSaveBuffers;
	else GLRenderer->mBuffers = GLRenderer->mScreenBuffers;
}

//===========================================================================
//
// 
//
//===========================================================================

void OpenGLFrameBuffer::BeginFrame()
{
	SetViewportRects(nullptr);
	mViewpoints->Clear();
	if (GLRenderer != nullptr)
		GLRenderer->BeginFrame();
}

//===========================================================================
// 
//	Takes a screenshot
//
//===========================================================================

TArray<uint8_t> OpenGLFrameBuffer::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	const auto &viewport = mOutputLetterbox;

	// Update() processes screenshot requests after the final 2D composition and
	// before swap, so this back buffer is the exact frame about to be displayed.
	TArray<uint8_t> pixels;
	pixels.Resize(viewport.width * viewport.height * 3);
	GLint previousReadFramebuffer = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(viewport.left, viewport.top, viewport.width, viewport.height,
		GL_RGB, GL_UNSIGNED_BYTE, &pixels[0]);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);

	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;
	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	float rcpWidth = 1.0f / w;
	float rcpHeight = 1.0f / h;
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			float u = (x + 0.5f) * rcpWidth;
			float v = (y + 0.5f) * rcpHeight;
			int sx = u * viewport.width;
			int sy = v * viewport.height;
			int sindex = (sx + sy * viewport.width) * 3;
			int dindex = (x + (h - y - 1) * w) * 3;
			ScreenshotBuffer[dindex] = pixels[sindex];
			ScreenshotBuffer[dindex + 1] = pixels[sindex + 1];
			ScreenshotBuffer[dindex + 2] = pixels[sindex + 2];
		}
	}

	pitch = w * 3;
	color_type = SS_RGB;

	// Screenshot should not use gamma correction if it was already applied to rendered image
	gamma = 1;
	if (vid_hdr_active && vid_fullscreen)
		gamma *= 2.2f;
	return ScreenshotBuffer;
}

//===========================================================================
//
//  Bounded asynchronous final-frame readback for the video recorder.
//
//  A regular screenshot is deliberately still synchronous: it is an explicit
//  one-shot request and callers need its pixels immediately. Video capture is
//  different. A PBO plus a zero-time fence poll lets the game present frames
//  while the GPU/DMA engine copies an older final composition. We never wait
//  for an unsignalled fence and keep only three requests in flight, so a slow
//  GPU cannot turn capture into an unbounded stall or allocation source.
//
//===========================================================================

OpenGLFrameBuffer::FVideoReadbackSlot *OpenGLFrameBuffer::OldestVideoReadback()
{
	// Do not let an old, discarded capture block the active take from retiring
	// ready frames. Discarded slots are still polled and released below, but
	// active capture data always wins when the ring is shared during a quick
	// stop/start or a presentation resize.
	for (int discardPass = 0; discardPass < 2; ++discardPass)
	{
		FVideoReadbackSlot *oldest = nullptr;
		for (auto &slot : mVideoReadbacks)
		{
			if (slot.Pending && slot.Discard == (discardPass != 0) &&
				(oldest == nullptr || slot.Sequence < oldest->Sequence))
			{
				oldest = &slot;
			}
		}
		if (oldest != nullptr)
		{
			return oldest;
		}
	}
	return nullptr;
}

OpenGLFrameBuffer::FVideoReadbackSlot *OpenGLFrameBuffer::FreeVideoReadback()
{
	for (auto &slot : mVideoReadbacks)
	{
		if (!slot.Pending && slot.Buffer != 0)
		{
			return &slot;
		}
	}
	for (auto &slot : mVideoReadbacks)
	{
		if (!slot.Pending)
		{
			return &slot;
		}
	}
	return nullptr;
}

OpenGLFrameBuffer::FVideoFramePacingFence *OpenGLFrameBuffer::OldestVideoFramePacingFence()
{
	FVideoFramePacingFence *oldest = nullptr;
	for (auto &entry : mVideoFramePacingFences)
	{
		if (entry.Fence != nullptr && (oldest == nullptr || entry.Sequence < oldest->Sequence))
		{
			oldest = &entry;
		}
	}
	return oldest;
}

void OpenGLFrameBuffer::ReleaseVideoReadback(FVideoReadbackSlot &slot)
{
	if (slot.Fence != nullptr)
	{
		// Context recovery can clear the sync entry points before a stale PBO
		// reaches this bookkeeping path. Forget our handle either way; when the
		// entry point still exists, GL defers destruction until queued commands
		// stop referencing it.
		if (glDeleteSync != nullptr)
		{
			glDeleteSync(slot.Fence);
		}
		slot.Fence = nullptr;
	}
	if (slot.Buffer != 0)
	{
		if (glDeleteBuffers != nullptr)
		{
			glDeleteBuffers(1, &slot.Buffer);
		}
		slot.Buffer = 0;
	}
	slot = FVideoReadbackSlot();
}

void OpenGLFrameBuffer::RecycleVideoReadback(FVideoReadbackSlot &slot)
{
	// A discarded request reached a signaled fence. Keep the PBO allocation for
	// a later sample, but clear every per-request field so stale pixels can
	// never re-enter a take after CPU-writer back-pressure subsides.
	if (slot.Fence != nullptr)
	{
		if (glDeleteSync != nullptr)
		{
			glDeleteSync(slot.Fence);
		}
		slot.Fence = nullptr;
	}
	slot.SourceLeft = 0;
	slot.SourceTop = 0;
	slot.SourceWidth = 0;
	slot.SourceHeight = 0;
	slot.TargetWidth = 0;
	slot.TargetHeight = 0;
	slot.CaptureTimeNS = 0;
	slot.Sequence = 0;
	slot.DeferredPolls = 0;
	slot.Pending = false;
	slot.Discard = false;
}

void OpenGLFrameBuffer::ResetVideoCapture()
{
	for (auto &slot : mVideoReadbacks)
	{
		if (slot.Pending)
		{
			// The GPU may still own this PBO. Preserve the fence and collect it
			// non-blockingly in Swap() rather than making a recording stop or a
			// resize wait for every prior rendering command.
			slot.Discard = true;
		}
		else
		{
			ReleaseVideoReadback(slot);
		}
	}
}

void OpenGLFrameBuffer::AbandonPendingVideoCaptureReadbacks()
{
	// The CPU writer is already full, so an in-flight capture cannot become a
	// useful frame. Mark only those submissions discarded. Idle PBO storage is
	// intentionally retained, and Swap() will zero-poll the discarded fences
	// before the buffers are reused or released.
	for (auto &slot : mVideoReadbacks)
	{
		if (slot.Pending && !slot.Discard)
		{
			slot.Discard = true;
		}
	}
}

void OpenGLFrameBuffer::RetireDiscardedVideoReadbacks()
{
	if (glClientWaitSync == nullptr)
	{
		return;
	}
	for (auto &slot : mVideoReadbacks)
	{
		if (!slot.Pending || !slot.Discard)
		{
			continue;
		}
		if (slot.Fence == nullptr)
		{
			ReleaseVideoReadback(slot);
			continue;
		}
		// A zero-time client wait is still allowed to flush the command stream.
		// Some software/remote GL implementations do not advance a PBO fence from
		// a bare poll even after a prior glFlush(), which used to leave an entire
		// recording with no completed frames. The flush bit submits work but does
		// not wait for it, so this keeps the no-stall contract intact.
		const GLenum waitResult = glClientWaitSync(slot.Fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
		if (waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED)
		{
			RecycleVideoReadback(slot);
		}
		else if (waitResult == GL_WAIT_FAILED)
		{
			ReleaseVideoReadback(slot);
		}
	}
}

void OpenGLFrameBuffer::AbandonDiscardedVideoReadbacks()
{
	// No current recording frame remains in these slots. GL object deletion is
	// ordered after any command that still references these PBOs/syncs, so this
	// releases our bounded handles without waiting for a potentially wedged GPU.
	for (auto &slot : mVideoReadbacks)
	{
		if (slot.Discard)
		{
			ReleaseVideoReadback(slot);
		}
	}
}

void OpenGLFrameBuffer::AbandonVideoReadbacks()
{
	// Context/API recovery cannot poll an old PBO safely. Release the engine's
	// logical ownership of every slot; guarded GL deletion below lets the driver
	// defer any object still referenced by work that preceded the context change.
	for (auto &slot : mVideoReadbacks)
	{
		ReleaseVideoReadback(slot);
	}
}

void OpenGLFrameBuffer::ReleaseVideoFramePacingFences()
{
	for (auto &entry : mVideoFramePacingFences)
	{
		if (entry.Fence != nullptr)
		{
			if (glDeleteSync != nullptr)
			{
				glDeleteSync(entry.Fence);
			}
			entry.Fence = nullptr;
		}
		entry.Sequence = 0;
	}
	mVideoFramePacingSaturationPolls = 0;
	mVideoFramePacingSaturated = false;
	mVideoFramePacingFailurePending = false;
	mVideoFramePacingFailureReported = false;
	mVideoFramePacingFenceCreationFailed = false;
}

void OpenGLFrameBuffer::RetireVideoFramePacingFences()
{
	if (glClientWaitSync == nullptr || glDeleteSync == nullptr)
	{
		return;
	}
	for (auto &entry : mVideoFramePacingFences)
	{
		if (entry.Fence == nullptr)
		{
			continue;
		}
		const GLenum waitResult = glClientWaitSync(entry.Fence, 0, 0);
		if (waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED ||
			waitResult == GL_WAIT_FAILED)
		{
			glDeleteSync(entry.Fence);
			entry.Fence = nullptr;
			entry.Sequence = 0;
			if (waitResult == GL_WAIT_FAILED)
			{
				// A failed fence is not proof that the command stream retired. Do
				// not continue an unbounded asynchronous capture after losing that
				// proof. Active footage follows the normal recorder-failure path;
				// an already-discarded take has no footage to protect, so release
				// its bounded handles and restore normal presentation instead.
				if (HasPendingVideoCapture())
				{
					mVideoFramePacingSaturated = true;
					NoteVideoFramePacingFailure();
					// NoteVideoFramePacingFailure() marks active slots discarded. Do
					// not continue this loop and mistake that transition for an
					// independently discard-only failure before D_Display consumes
					// the active-take failure.
					return;
				}
				else
				{
					AbandonDiscardedVideoReadbacks();
					ReleaseVideoFramePacingFences();
					return;
				}
			}
		}
	}
}

void OpenGLFrameBuffer::NoteVideoFramePacingFailure()
{
	if (!mVideoFramePacingFailureReported)
	{
		mVideoFramePacingFailureReported = true;
		mVideoFramePacingFailurePending = true;
		// Existing PBOs remain allocated only until their zero-wait fence polls
		// prove them safe to release. Dropping their frames now lets Stop() avoid
		// waiting for a stale final sample while the pacing fence is recovering.
		ResetVideoCapture();
	}
}

bool OpenGLFrameBuffer::ConsumeVideoCaptureBackpressureFailure()
{
	const bool failure = mVideoFramePacingFailurePending;
	mVideoFramePacingFailurePending = false;
	return failure;
}

bool OpenGLFrameBuffer::CanRenderNextFrame()
{
	// Keep the bounded admission gate alive for *all* pending PBOs, including
	// abandoned ones. Suppressing glFinish while discard-only work remains is
	// safe only while this three-fence gate still limits ordinary GL submission.
	if (!HasPendingVideoReadback())
	{
		ReleaseVideoFramePacingFences();
		return true;
	}
	const auto abandonDiscardOnlyReadbacks = [this]()
	{
		AbandonDiscardedVideoReadbacks();
		ReleaseVideoFramePacingFences();
	};
	if (!HasAsyncVideoReadbackSupport())
	{
		// A context/API transition can occur before the three-fence ring reaches
		// saturation. Do not leave either active or discarded PBO bookkeeping for
		// a fallback screenshot path or final-readback drain to misinterpret.
		ResetVideoCapture();
		AbandonVideoReadbacks();
		ReleaseVideoFramePacingFences();
		return true;
	}

	// This one-shot is raised only while active footage exists and must reach
	// D_Display so it can stop that take. Discard-only recovery paths below do
	// not set it, preventing an abandoned PBO from poisoning a later recording.
	if (mVideoFramePacingFailurePending)
	{
		return false;
	}

	// The recorder may deliberately abandon every submitted PBO when its
	// bounded CPU writer is full. Those slots no longer contain footage that a
	// take must preserve, so do not spend the frame-pacing recovery budget
	// holding presentation behind their fences. GL object deletion is ordered
	// after prior commands by the driver; releasing our handles here lets the
	// next Swap use the normal renderer synchronization instead of a capture
	// specific 120-poll freeze.
	if (!HasPendingVideoCapture())
	{
		AbandonDiscardedVideoReadbacks();
		ReleaseVideoFramePacingFences();
		return true;
	}

	// Inspect the bounded fence ring before admitting a new render, rather than
	// waiting for Swap() to discover that it was already full.  Otherwise the
	// fourth frame can be rendered and submitted without a fence after the
	// three tracked frames, which weakens the very back-pressure guarantee this
	// path exists to enforce.  There is no cross-thread race here: rendering
	// and presentation both run on the game thread.
	if (!mVideoFramePacingSaturated && glClientWaitSync != nullptr && glDeleteSync != nullptr)
	{
		RetireVideoFramePacingFences();
		if (mVideoFramePacingFailurePending)
		{
			return false;
		}
		if (!HasPendingVideoReadback())
		{
			ReleaseVideoFramePacingFences();
			return true;
		}

		bool full = true;
		for (const auto &entry : mVideoFramePacingFences)
		{
			if (entry.Fence == nullptr)
			{
				full = false;
				break;
			}
		}
		if (full)
		{
			mVideoFramePacingSaturated = true;
			mVideoFramePacingSaturationPolls = 0;
		}
	}

	if (!mVideoFramePacingSaturated)
	{
		return true;
	}
	if (mVideoFramePacingFenceCreationFailed)
	{
		if (!HasPendingVideoCapture())
		{
			abandonDiscardOnlyReadbacks();
			return true;
		}
		// The current frame was submitted but could not be fenced, so there is
		// no GL completion point that can prove it safe to admit another one.
		// Keep event/tick processing alive but leave presentation quarantined
		// until a renderer/context recreation constructs a fresh framebuffer.
		// Resuming blindly here would recreate exactly the unbounded driver
		// backlog that this capture path is designed to prevent.
		return false;
	}
	RetireVideoFramePacingFences();
	if (mVideoFramePacingFailurePending)
	{
		return false;
	}
	if (!HasPendingVideoReadback())
	{
		ReleaseVideoFramePacingFences();
		return true;
	}

	bool full = true;
	for (const auto &entry : mVideoFramePacingFences)
	{
		if (entry.Fence == nullptr)
		{
			full = false;
			break;
		}
	}
	if (!full)
	{
		mVideoFramePacingSaturated = false;
		mVideoFramePacingSaturationPolls = 0;
		mVideoFramePacingFailureReported = false;
		mVideoFramePacingFenceCreationFailed = false;
		return true;
	}
	if (mVideoFramePacingFailureReported)
	{
		if (!HasPendingVideoCapture())
		{
			abandonDiscardOnlyReadbacks();
			return true;
		}
		// The recorder has already detached after a bounded recovery window. Keep
		// zero-polling above so a recovered GPU can resume presentation, but do
		// not spend another millisecond per game-loop iteration waiting on a
		// failure that was already reported. That would turn an otherwise
		// responsive event/tick loop into needless CPU/GPU pressure while the
		// recorder's independent final-readback drain is winding down.
		return false;
	}

	FVideoFramePacingFence *oldest = OldestVideoFramePacingFence();
	if (oldest == nullptr)
	{
		mVideoFramePacingSaturated = false;
		mVideoFramePacingSaturationPolls = 0;
		mVideoFramePacingFailureReported = false;
		mVideoFramePacingFenceCreationFailed = false;
		return true;
	}

	// No drawing, readback, or SwapBuffers call has happened on this path. A
	// single millisecond is a hard upper bound for this poll; timeout leaves the
	// presentation frozen instead of queuing another frame behind a wedged GPU.
	const GLenum waitResult = glClientWaitSync(oldest->Fence, GL_SYNC_FLUSH_COMMANDS_BIT,
		VideoFramePacingRecoveryWaitNS);
	if (waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED)
	{
		glDeleteSync(oldest->Fence);
		oldest->Fence = nullptr;
		oldest->Sequence = 0;
		mVideoFramePacingSaturated = false;
		mVideoFramePacingSaturationPolls = 0;
		mVideoFramePacingFailureReported = false;
		mVideoFramePacingFenceCreationFailed = false;
		return true;
	}
	if (waitResult == GL_WAIT_FAILED)
	{
		glDeleteSync(oldest->Fence);
		oldest->Fence = nullptr;
		oldest->Sequence = 0;
		if (!HasPendingVideoCapture())
		{
			abandonDiscardOnlyReadbacks();
			return true;
		}
		NoteVideoFramePacingFailure();
		return false;
	}

	if (mVideoFramePacingSaturationPolls < VideoFramePacingMaxSaturationPolls)
	{
		++mVideoFramePacingSaturationPolls;
	}
	if (mVideoFramePacingSaturationPolls >= VideoFramePacingMaxSaturationPolls)
	{
		if (!HasPendingVideoCapture())
		{
			abandonDiscardOnlyReadbacks();
			return true;
		}
		NoteVideoFramePacingFailure();
	}
	return false;
}

void OpenGLFrameBuffer::ThrottleVideoFramePacing()
{
	if (glClientWaitSync == nullptr || glDeleteSync == nullptr)
	{
		return;
	}
	RetireVideoFramePacingFences();
	if (mVideoFramePacingFailurePending || !HasPendingVideoReadback())
	{
		return;
	}
	FVideoFramePacingFence *oldest = nullptr;
	for (auto &entry : mVideoFramePacingFences)
	{
		if (entry.Fence == nullptr)
		{
			return;
		}
		if (oldest == nullptr || entry.Sequence < oldest->Sequence)
		{
			oldest = &entry;
		}
	}
	if (oldest == nullptr)
	{
		return;
	}

	// This is the last admission point after a frame has already been rendered.
	// Give the oldest command stream one bounded chance to complete, then latch
	// the frame-admission gate. Continuing to swap while all three fences remain
	// unsignalled would make this three-entry tracker cosmetic: every later
	// rendered frame could still accumulate inside the driver.
	const GLenum waitResult = glClientWaitSync(oldest->Fence, GL_SYNC_FLUSH_COMMANDS_BIT,
		VideoFramePacingRecoveryWaitNS);
	if (waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED)
	{
		glDeleteSync(oldest->Fence);
		oldest->Fence = nullptr;
		oldest->Sequence = 0;
		mVideoFramePacingSaturationPolls = 0;
		mVideoFramePacingFailureReported = false;
		return;
	}
	if (waitResult == GL_WAIT_FAILED)
	{
		glDeleteSync(oldest->Fence);
		oldest->Fence = nullptr;
		oldest->Sequence = 0;
		if (!HasPendingVideoCapture())
		{
			AbandonDiscardedVideoReadbacks();
			ReleaseVideoFramePacingFences();
			return;
		}
		mVideoFramePacingSaturated = true;
		NoteVideoFramePacingFailure();
		return;
	}

	mVideoFramePacingSaturated = true;
	mVideoFramePacingSaturationPolls = 0;
}

void OpenGLFrameBuffer::QueueVideoFramePacingFence()
{
	if (glFenceSync == nullptr)
	{
		return;
	}
	for (auto &entry : mVideoFramePacingFences)
	{
		if (entry.Fence != nullptr)
		{
			continue;
		}
		entry.Fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		if (entry.Fence != nullptr)
		{
			entry.Sequence = mNextVideoFramePacingSequence++;
			if (mNextVideoFramePacingSequence == 0)
			{
				mNextVideoFramePacingSequence = 1;
			}
		}
		else
		{
			// Swap() has already submitted this frame. If it cannot be fenced,
			// suppressing glFinish() would leave no bounded completion proof for
			// it, so detach capture and quarantine presentation until the backend
			// is recreated rather than allowing an untracked command backlog.
			if (!HasPendingVideoCapture())
			{
				AbandonDiscardedVideoReadbacks();
				ReleaseVideoFramePacingFences();
			}
			else
			{
				mVideoFramePacingSaturated = true;
				mVideoFramePacingFenceCreationFailed = true;
				NoteVideoFramePacingFailure();
			}
		}
		return;
	}
}

bool OpenGLFrameBuffer::IssueVideoReadback(uint64_t requestTimeNS)
{
	const auto &viewport = mOutputLetterbox;
	const int targetWidth = SCREENWIDTH;
	const int targetHeight = SCREENHEIGHT;
	const uint64_t sourceBytes = (uint64_t)viewport.width * (uint64_t)viewport.height * 4ull;
	// PBOs are explicitly bounded as a group, not merely one at a time. Three
	// normal 4K RGBA requests fit easily (~100 MiB); at extreme resolutions the ring
	// automatically shrinks to one/two slots instead of quietly retaining
	// hundreds of MiB of graphics memory.
	constexpr uint64_t MaxVideoReadbackBytes = 128ull * 1024ull * 1024ull;
	if (viewport.width <= 0 || viewport.height <= 0 || targetWidth <= 0 || targetHeight <= 0 ||
		sourceBytes == 0 || sourceBytes > MaxVideoReadbackBytes)
	{
		return false;
	}
	// The allocation budget belongs to the ring as a whole. Completed PBOs do
	// not keep their source dimensions, so a resize could otherwise retain a
	// full old ring and grow another full ring one slot at a time. Release every
	// old backing allocation before choosing a slot for a new byte geometry.
	// In-flight old storage is marked discarded and remains inside this same
	// three-slot budget until its zero-wait fence poll releases it; a resize
	// therefore skips captures temporarily instead of synchronizing the GPU.
	bool resetStorage = false;
	for (const auto &candidate : mVideoReadbacks)
	{
		if (candidate.Buffer != 0 && !candidate.Discard && candidate.AllocatedBytes != sourceBytes)
		{
			resetStorage = true;
			break;
		}
	}
	if (resetStorage)
	{
		ResetVideoCapture();
	}

	FVideoReadbackSlot *slot = FreeVideoReadback();
	if (slot == nullptr)
	{
		return false;
	}
	const unsigned int slotsByBudget = (unsigned int)(MaxVideoReadbackBytes / sourceBytes);
	const unsigned int allowedSlots = slotsByBudget == 0 ? 1 :
		(slotsByBudget < VideoReadbackSlotCount ? slotsByBudget : VideoReadbackSlotCount);
	unsigned int allocatedSlots = 0;
	uint64_t residentBytes = 0;
	for (const auto &candidate : mVideoReadbacks)
	{
		if (candidate.Buffer != 0)
		{
			// Discarded PBOs remain resident until their fence retires. Count
			// their old backing allocation as well as live slots: a resize must
			// skip a sample rather than briefly retaining an old 128 MiB PBO and
			// allocating a second ring for the new presentation size.
			if (candidate.AllocatedBytes > MaxVideoReadbackBytes - residentBytes)
			{
				return false;
			}
			residentBytes += candidate.AllocatedBytes;
			++allocatedSlots;
		}
	}
	if (slot->Buffer == 0 && (allocatedSlots >= allowedSlots ||
		sourceBytes > MaxVideoReadbackBytes - residentBytes))
	{
		return false;
	}

	if (slot->Buffer == 0)
	{
		ClearVideoReadbackErrors();
		glGenBuffers(1, &slot->Buffer);
		if (slot->Buffer == 0 || VideoReadbackHasError())
		{
			ReleaseVideoReadback(*slot);
			return false;
		}
	}

	GLint previousReadFramebuffer = 0;
	GLint previousPackBuffer = 0;
	GLint previousPackAlignment = 4;
	ClearVideoReadbackErrors();
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, slot->Buffer);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	const auto restoreReadbackState = [&]()
	{
		glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPackBuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
	};
	if (VideoReadbackHasError())
	{
		restoreReadbackState();
		ReleaseVideoReadback(*slot);
		return false;
	}
	// Reuse a completed PBO's backing storage. Re-orphaning it every frame can
	// make drivers repeatedly allocate/device-page the same large transfer.
	if (slot->AllocatedBytes != sourceBytes)
	{
		ClearVideoReadbackErrors();
		glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)sourceBytes, nullptr, GL_STREAM_READ);
		if (VideoReadbackHasError())
		{
			// Do not remember a failed allocation. Releasing the slot resets its
			// byte count so a later capture can either allocate a real PBO or
			// report a clean skipped/fatal sample to the recorder.
			restoreReadbackState();
			ReleaseVideoReadback(*slot);
			return false;
		}
		slot->AllocatedBytes = sourceBytes;
	}
	ClearVideoReadbackErrors();
	glReadPixels(viewport.left, viewport.top, viewport.width, viewport.height,
		GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	if (VideoReadbackHasError())
	{
		restoreReadbackState();
		ReleaseVideoReadback(*slot);
		return false;
	}
	restoreReadbackState();

	ClearVideoReadbackErrors();
	slot->Fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (slot->Fence == nullptr || VideoReadbackHasError())
	{
		// A readback without a completion fence must never be recycled: the
		// queued glReadPixels could still own its backing storage. Deleting the
		// GL object is safely deferred by the driver until that command retires.
		ReleaseVideoReadback(*slot);
		return false;
	}
	// Update() issues capture before FPSLimit() and the platform swap. Submit
	// this batch now so DMA can overlap that limiter; later fence polls pass
	// zero flags and must not depend on a backend-specific SwapBuffers flush.
	// glFlush submits commands but never waits for their completion.
	ClearVideoReadbackErrors();
	glFlush();
	if (VideoReadbackHasError())
	{
		ReleaseVideoReadback(*slot);
		return false;
	}
	slot->SourceLeft = viewport.left;
	slot->SourceTop = viewport.top;
	slot->SourceWidth = viewport.width;
	slot->SourceHeight = viewport.height;
	slot->TargetWidth = targetWidth;
	slot->TargetHeight = targetHeight;
	slot->CaptureTimeNS = requestTimeNS;
	slot->Sequence = mNextVideoReadbackSequence++;
	if (mNextVideoReadbackSequence == 0)
	{
		mNextVideoReadbackSequence = 1;
	}
	slot->Pending = true;
	return true;
}

TArray<uint8_t> OpenGLFrameBuffer::ConsumeVideoReadback(FVideoReadbackSlot &slot, int &width, int &height, int &pitch,
	ESSType &color_type, float &gamma, uint64_t &captureTimeNS, bool &bottomUp)
{
	const uint64_t sourceBytes = (uint64_t)slot.SourceWidth * (uint64_t)slot.SourceHeight * 4ull;
	TArray<uint8_t> result;
	if (sourceBytes == 0 || sourceBytes > 0xffffffffull)
	{
		ReleaseVideoReadback(slot);
		return result;
	}

	GLint previousPackBuffer = 0;
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.Buffer);
	// The fence was already observed as signaled before this method is called,
	// so this read cannot wait for a pending GPU transfer. Do not add
	// GL_MAP_UNSYNCHRONIZED_BIT here: OpenGL only permits that flag for maps
	// that include GL_MAP_WRITE_BIT, and Mesa correctly rejects it on a PBO
	// read map. That error previously made every otherwise-ready frame vanish.
	const uint8_t *pixels = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
		(GLsizeiptr)sourceBytes, GL_MAP_READ_BIT);
	if (pixels != nullptr)
	{
		// Keep native viewport RGBA rows as one contiguous bulk copy. Resizing
		// and RGB packing here made letterboxed/high-DPI recording run a full
		// per-pixel loop on the render thread. The recorder writes a separately
		// described part whenever this presentation size changes.
		result.Resize((unsigned int)sourceBytes);
		memcpy(result.Data(), pixels, (size_t)sourceBytes);
		if (glUnmapBuffer(GL_PIXEL_PACK_BUFFER) == GL_FALSE)
		{
			// The mapped PBO contents became invalid while reading. Do not hand a
			// partially corrupt image to the encoder; the caller will request a
			// bounded replacement on a later frame.
			result.Reset();
		}
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, previousPackBuffer);
	captureTimeNS = slot.CaptureTimeNS;
	width = slot.SourceWidth;
	height = slot.SourceHeight;
	pitch = slot.SourceWidth * 4;
	color_type = SS_RGBA;
	bottomUp = true;
	gamma = 1.0f;
	if (vid_hdr_active && vid_fullscreen)
	{
		gamma *= 2.2f;
	}
	// Reuse the PBO storage next time, but fence ownership ended once the map
	// was copied. Keep its dimensions/timestamp from leaking into a future slot.
	if (slot.Fence != nullptr)
	{
		glDeleteSync(slot.Fence);
		slot.Fence = nullptr;
	}
	slot.Pending = false;
	slot.SourceWidth = slot.SourceHeight = 0;
	slot.TargetWidth = slot.TargetHeight = 0;
	slot.CaptureTimeNS = 0;
	slot.Sequence = 0;
	return result;
}

void OpenGLFrameBuffer::GetVideoCaptureDimensions(int &width, int &height) const
{
	// The asynchronous PBO path reads the native presentation viewport rather
	// than the logical framebuffer. On a HiDPI window that may be larger than
	// GetWidth()/GetHeight(), so reporting it up front keeps the recorder's
	// bounded-queue reservation honest and avoids an expensive readback that it
	// would have to reject afterward.
	width = mOutputLetterbox.width > 0 ? mOutputLetterbox.width : SCREENWIDTH;
	height = mOutputLetterbox.height > 0 ? mOutputLetterbox.height : SCREENHEIGHT;
}

TArray<uint8_t> OpenGLFrameBuffer::GetVideoCaptureBuffer(int &width, int &height, int &pitch, ESSType &color_type,
	float &gamma, uint64_t requestTimeNS, uint64_t &captureTimeNS, bool &pending, bool &bottomUp, bool issueNext)
{
	GetVideoCaptureDimensions(width, height);
	pending = false;
	bottomUp = false;
	captureTimeNS = requestTimeNS;

	// Desktop OpenGL 3.2+ supplies the synchronization primitives. Keep a
	// correct one-shot fallback for unusually old drivers instead of emitting
	// incomplete frames.
	if (!HasAsyncVideoReadbackSupport())
	{
		return issueNext ? GetScreenshotBuffer(pitch, color_type, gamma) : TArray<uint8_t>();
	}

	const int targetWidth = SCREENWIDTH;
	const int targetHeight = SCREENHEIGHT;
	const auto &viewport = mOutputLetterbox;
	// A stop/restart or a resize leaves in-flight PBOs in a discard state until
	// their fences signal. Reclaim any that have completed without waiting so a
	// subsequent take can reuse the bounded ring promptly.
	RetireDiscardedVideoReadbacks();
	// Resolution/output-viewport changes invalidate old readbacks while a take
	// is active. A final zero-poll deliberately keeps the slot's own recorded
	// dimensions: stopping a take must be able to retain a ready tail even if a
	// window resize lands between the last request and its fence signal.
	if (issueNext)
	{
		for (auto &slot : mVideoReadbacks)
		{
			if (slot.Pending && !slot.Discard && (slot.TargetWidth != targetWidth || slot.TargetHeight != targetHeight ||
				slot.SourceWidth != viewport.width || slot.SourceHeight != viewport.height ||
				slot.SourceLeft != viewport.left || slot.SourceTop != viewport.top))
			{
				ResetVideoCapture();
				break;
			}
		}
	}

	FVideoReadbackSlot *oldest = OldestVideoReadback();
	if (oldest != nullptr)
	{
		// Preserve the zero timeout, but make progress explicit for drivers whose
		// asynchronous transfer queue is not advanced by a bare poll (notably
		// llvmpipe/Xvfb). This never waits for the GPU.
		GLenum waitResult = oldest->Fence != nullptr ?
			glClientWaitSync(oldest->Fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0) : GL_WAIT_FAILED;
		if (waitResult == GL_TIMEOUT_EXPIRED &&
			oldest->DeferredPolls >= VideoReadbackRecoveryPollCount)
		{
			// This is a recovery path for a repeatedly deferred *oldest* request,
			// not a per-frame synchronization point. It gives the driver one
			// millisecond to execute already-submitted work, then immediately
			// returns control to rendering even when the GPU is wedged.
			waitResult = glClientWaitSync(oldest->Fence, GL_SYNC_FLUSH_COMMANDS_BIT,
				VideoReadbackRecoveryWaitNS);
			oldest->DeferredPolls = 0;
		}
		if (waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED)
		{
			if (oldest->Discard)
			{
				// This belongs to the take or geometry that ResetVideoCapture()
				// intentionally abandoned. Its fence proves the backing PBO is
				// safe to reuse; never let it leak a stale frame into a new take.
				RecycleVideoReadback(*oldest);
				pending = (issueNext && IssueVideoReadback(requestTimeNS)) || HasPendingVideoReadback();
				return TArray<uint8_t>();
			}
			TArray<uint8_t> result = ConsumeVideoReadback(*oldest, width, height, pitch, color_type, gamma, captureTimeNS, bottomUp);
			// Keep the small ring filled after consuming one ready frame. This is
			// only a command enqueue; no GPU wait is introduced on the render path.
			if (result.Size() == 0)
			{
				// A transient map failure should discard only this one GPU result.
				// If a replacement was queued, keep the recorder alive rather than
				// treating the empty result as a renderer failure.
					pending = (issueNext && IssueVideoReadback(requestTimeNS)) || HasPendingVideoReadback();
					return result;
				}
				if (issueNext)
				{
					IssueVideoReadback(requestTimeNS);
				}
				return result;
		}
		if (waitResult == GL_WAIT_FAILED)
		{
			ReleaseVideoReadback(*oldest);
			// A bad fence loses at most this one sample. Try to refill the ring
			// and keep recording responsive instead of treating it as a fatal
			// final-frame failure on the main thread.
			pending = (issueNext && IssueVideoReadback(requestTimeNS)) || HasPendingVideoReadback();
			return TArray<uint8_t>();
		}

		// No wait, no stall: an unsignalled fence simply means this sampling slot
		// is not ready yet. Fill one additional free slot if possible and let the
		// recorder return to gameplay.
		if (oldest->DeferredPolls < VideoReadbackRecoveryPollCount)
		{
			++oldest->DeferredPolls;
		}
		if (issueNext)
		{
			IssueVideoReadback(requestTimeNS);
		}
		pending = HasPendingVideoReadback();
		return TArray<uint8_t>();
	}

	if (issueNext && IssueVideoReadback(requestTimeNS))
	{
		pending = true;
		return TArray<uint8_t>();
	}
	return TArray<uint8_t>();
}

bool OpenGLFrameBuffer::HasPendingVideoReadback() const
{
	for (const auto &slot : mVideoReadbacks)
	{
		if (slot.Pending)
		{
			return true;
		}
	}
	return false;
}

bool OpenGLFrameBuffer::HasPendingVideoCapture() const
{
	// A normal stop must drain only frames belonging to its active take. Slots
	// marked Discard were deliberately abandoned by an earlier resize or restart
	// and are retired opportunistically from Swap(); making finalization wait for
	// one of those fences can leave a completed take stuck in FINALIZING when a
	// driver is slow to retire stale work. They still count toward the bounded PBO
	// budget, but they are not footage that the current writer must preserve.
	for (const auto &slot : mVideoReadbacks)
	{
		if (slot.Pending && !slot.Discard)
		{
			return true;
		}
	}
	return false;
}

//===========================================================================
// 
// 2D drawing
//
//===========================================================================

void OpenGLFrameBuffer::Draw2D()
{
	if (GLRenderer != nullptr)
	{
		GLRenderer->mBuffers->BindCurrentFB();
		::Draw2D(twod, gl_RenderState);
	}
}

void OpenGLFrameBuffer::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) GLRenderer->mBuffers->BlitSceneToTexture(); // Copy the resulting scene to the current post process texture
	GLRenderer->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

bool OpenGLFrameBuffer::CompileNextShader()
{
	return GLRenderer->mShaderManager->CompileNextShader();
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeStartScreen
//
// Called before the current screen has started rendering. This needs to
// save what was drawn the previous frame so that it can be animated into
// what gets drawn this frame.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeStartScreen()
{
	const auto &viewport = screen->mScreenViewport;

	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(nullptr, viewport.width, viewport.height, 0, false, "WipeStartScreen");
	glFinish();
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);

	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

//==========================================================================
//
// OpenGLFrameBuffer :: WipeEndScreen
//
// The screen we want to animate to has just been drawn.
//
//==========================================================================

FTexture *OpenGLFrameBuffer::WipeEndScreen()
{
	GLRenderer->Flush();
	const auto &viewport = screen->mScreenViewport;
	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	tex->GetSystemTexture()->CreateTexture(NULL, viewport.width, viewport.height, 0, false, "WipeEndScreen");
	glFinish();
	static_cast<FHardwareTexture*>(tex->GetSystemTexture())->Bind(0, false);
	GLRenderer->mBuffers->BindCurrentFB();
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport.left, viewport.top, viewport.width, viewport.height);
	return tex;
}

}
