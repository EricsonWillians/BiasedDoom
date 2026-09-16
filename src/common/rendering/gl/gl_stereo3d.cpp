/*
** gl_stereo.cpp
** Stereoscopic 3D API
**
**---------------------------------------------------------------------------
** Copyright 2015 Christopher Bruns
** Copyright 2016-2021 Christoph Oelckers
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
** 
**
*/

#include "gl_system.h"
#include "gl_renderer.h"
#include "gl_renderbuffers.h"
#include "hw_vrmodes.h"
#include "gl_framebuffer.h"
#include "gl_postprocessstate.h"
#include "gl_framebuffer.h"
#include "gl_shaderprogram.h"
#include "gl_buffers.h"
#include "menu.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"


EXTERN_CVAR(Int, vr_mode)
EXTERN_CVAR(Float, vid_saturation)
EXTERN_CVAR(Float, vid_brightness)
EXTERN_CVAR(Float, vid_contrast)
EXTERN_CVAR(Int, gl_satformula)
EXTERN_CVAR(Int, gl_dither_bpc)

#ifdef _WIN32
EXTERN_CVAR(Bool, vr_enable_quadbuffered)
#endif

void UpdateVRModes(bool considerQuadBuffered)
{
	FOptionValues** pVRModes = OptionValues.CheckKey("VRMode");
	if (pVRModes == nullptr) return;

	TArray<FOptionValues::Pair>& vals = (*pVRModes)->mValues;
	TArray<FOptionValues::Pair> filteredValues;
	int cnt = vals.Size();
	for (int i = 0; i < cnt; ++i) {
		auto const& mode = vals[i];
		if (mode.Value == 7) {  // Quad-buffered stereo
#ifdef _WIN32
			if (!vr_enable_quadbuffered) continue;
#else
			continue;  // Remove quad-buffered option on Mac and Linux
#endif
			if (!considerQuadBuffered) continue;  // Probably no compatible screen mode was found
		}
		filteredValues.Push(mode);
	}
	vals = filteredValues;
}

namespace OpenGLRenderer
{

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentAnaglyph(bool r, bool g, bool b)
{
	mBuffers->BindOutputFB();
	ClearBorders();

	glColorMask(r, g, b, 1);
	mBuffers->BindEyeTexture(0, 0);
 	DrawPresentTexture(screen->mOutputLetterbox, true);

	glColorMask(!r, !g, !b, 1);
	mBuffers->BindEyeTexture(1, 0);
	DrawPresentTexture(screen->mOutputLetterbox, true);

	glColorMask(1, 1, 1, 1);
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentSideBySide(int vrmode)
{
	if (vrmode == VR_SIDEBYSIDEFULL || vrmode == VR_SIDEBYSIDESQUISHED)
	{
		mBuffers->BindOutputFB();
		ClearBorders();

		// Compute screen regions to use for left and right eye views
		int leftWidth = screen->mOutputLetterbox.width / 2;
		int rightWidth = screen->mOutputLetterbox.width - leftWidth;
		IntRect leftHalfScreen = screen->mOutputLetterbox;
		leftHalfScreen.width = leftWidth;
		IntRect rightHalfScreen = screen->mOutputLetterbox;
		rightHalfScreen.width = rightWidth;
		rightHalfScreen.left += leftWidth;

		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(leftHalfScreen, true);

		mBuffers->BindEyeTexture(1, 0);
		DrawPresentTexture(rightHalfScreen, true);
	}
	else if (vrmode == VR_SIDEBYSIDELETTERBOX)
	{
		mBuffers->BindOutputFB();
		screen->mOutputLetterbox.top = screen->mOutputLetterbox.height;

		ClearBorders();
		screen->mOutputLetterbox.top = 0;  //reset so screenshots can be taken

		// Compute screen regions to use for left and right eye views
		int leftWidth = screen->mOutputLetterbox.width / 2;
		int rightWidth = screen->mOutputLetterbox.width - leftWidth;
		//cut letterbox height in half
		int height = screen->mOutputLetterbox.height / 2;
		int top = height * .5;
		IntRect leftHalfScreen = screen->mOutputLetterbox;
		leftHalfScreen.width = leftWidth;
		leftHalfScreen.height = height;
		leftHalfScreen.top = top;
		IntRect rightHalfScreen = screen->mOutputLetterbox;
		rightHalfScreen.width = rightWidth;
		rightHalfScreen.left += leftWidth;
		//give it those cinematic black bars on top and bottom
		rightHalfScreen.height = height;
		rightHalfScreen.top = top;

		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(leftHalfScreen, true);

		mBuffers->BindEyeTexture(1, 0);
		DrawPresentTexture(rightHalfScreen, true);
	}
}


//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentTopBottom()
{
	mBuffers->BindOutputFB();
	ClearBorders();

	// Compute screen regions to use for left and right eye views
	int topHeight = screen->mOutputLetterbox.height / 2;
	int bottomHeight = screen->mOutputLetterbox.height - topHeight;
	IntRect topHalfScreen = screen->mOutputLetterbox;
	topHalfScreen.height = topHeight;
	topHalfScreen.top = topHeight;
	IntRect bottomHalfScreen = screen->mOutputLetterbox;
	bottomHalfScreen.height = bottomHeight;
	bottomHalfScreen.top = 0;

	mBuffers->BindEyeTexture(0, 0);
	DrawPresentTexture(topHalfScreen, true);

	mBuffers->BindEyeTexture(1, 0);
	DrawPresentTexture(bottomHalfScreen, true);
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::prepareInterleavedPresent(FPresentShaderBase& shader)
{
	mBuffers->BindOutputFB();
	ClearBorders();


	// Bind each eye texture, for composition in the shader
	mBuffers->BindEyeTexture(0, 0);
	mBuffers->BindEyeTexture(1, 1);
	mBuffers->BindDitherTexture(2);

	glActiveTexture(GL_TEXTURE0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glActiveTexture(GL_TEXTURE1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glActiveTexture(GL_TEXTURE2);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	const IntRect& box = screen->mOutputLetterbox;
	glViewport(box.left, box.top, box.width, box.height);

	shader.Bind();

	if (framebuffer->IsHWGammaActive())
	{
		shader.Uniforms->InvGamma = 1.0f;
		shader.Uniforms->Contrast = 1.0f;
		shader.Uniforms->Brightness = 0.0f;
		shader.Uniforms->Saturation = 1.0f;
	}
	else
	{
		shader.Uniforms->InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
		shader.Uniforms->Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		shader.Uniforms->Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
		shader.Uniforms->Saturation = clamp<float>(vid_saturation, -15.0f, 15.0f);
		shader.Uniforms->GrayFormula = static_cast<int>(gl_satformula);
	}
	shader.Uniforms->AtmosphereMode = gl_atmosphere;
	shader.Uniforms->AtmosphereIntensity = gl_atmosphere_intensity;
	shader.Uniforms->AtmosphereContrast = gl_atmosphere_contrast;
	const int postfxQuality = bd_postfx_quality;
	const bool postfxEnabled = (bd_postfx_enable && postfxQuality > 0);
	shader.Uniforms->CrtMode = (postfxEnabled ? gl_crt_mode : 0);
	shader.Uniforms->CrtDistortion = gl_crt_distortion;
	shader.Uniforms->CrtZoom = gl_crt_zoom;
	shader.Uniforms->CrtScanline = gl_crt_scanline;
	shader.Uniforms->CrtScanlineDensity = gl_crt_scanline_density;
	shader.Uniforms->CrtScanlineSharpness = gl_crt_scanline_sharpness;
	shader.Uniforms->CrtMaskIntensity = gl_crt_mask_intensity;
	shader.Uniforms->NtscMode = (postfxEnabled ? gl_ntsc_mode : 0);
	shader.Uniforms->VignetteStrength =
		(postfxEnabled && bd_vignette_enable) ? bd_vignette_strength : 0.0f;
	shader.Uniforms->ChromaticStrength =
		(postfxEnabled && bd_chromatic_enable) ? bd_chromatic_strength : 0.0f;
	shader.Uniforms->FilmgrainStrength =
		(postfxEnabled && bd_filmgrain_enable) ? bd_filmgrain_strength : 0.0f;
	shader.Uniforms->FilmgrainScale = bd_filmgrain_scale;
	shader.Uniforms->SharpenStrength =
		(postfxEnabled && bd_sharpen_enable) ? bd_sharpen_strength : 0.0f;
	shader.Uniforms->RetroPixelScale =
		(postfxEnabled && bd_retro_pixel_enable) ? bd_retro_pixel_scale : 1.0f;
	shader.Uniforms->ColorgradeMode =
		(postfxEnabled && bd_colorgrade_strength > 0.0f) ? bd_colorgrade_mode : 0;
	shader.Uniforms->ColorgradeStrength =
		(postfxEnabled) ? bd_colorgrade_strength : 0.0f;
	shader.Uniforms->ColorgradeLut =
		(postfxEnabled && bd_colorgrade_strength > 0.0f) ? bd_colorgrade_lut : 0;
	shader.Uniforms->VhsStrength =
		(postfxEnabled && bd_vhs_enable) ? bd_vhs_strength : 0.0f;
	shader.Uniforms->VhsScanline =
		(postfxEnabled && bd_vhs_enable) ? bd_vhs_scanline : 0.0f;
	shader.Uniforms->VhsJitter =
		(postfxEnabled && bd_vhs_enable) ? bd_vhs_jitter : 0.0f;
	shader.Uniforms->VhsTime =
		(postfxEnabled && bd_vhs_enable) ? screen->FrameTime * 0.001f : 0.0f;
	shader.Uniforms->VhsTracking =
		(postfxEnabled && bd_vhs_enable) ? bd_vhs_tracking : 0.0f;
	shader.Uniforms->VhsGhosting =
		(postfxEnabled && bd_vhs_enable) ? bd_vhs_ghosting : 0.0f;
	shader.Uniforms->VhsNoise =
		(postfxEnabled && bd_vhs_enable) ? bd_vhs_noise : 0.0f;
	shader.Uniforms->VhsEvil =
		(postfxEnabled && bd_vhs_enable) ? bd_vhs_evil : 0.0f;
	shader.Uniforms->VhsPanicEnable =
		(postfxEnabled && bd_vhs_enable && bd_vhs_panic_enable) ? 1 : 0;
	shader.Uniforms->GradeEnable =
		(postfxEnabled && bd_grade_enable) ? 1 : 0;
	shader.Uniforms->VibranceStrength =
		(postfxEnabled && bd_vibrance_enable) ? bd_vibrance_strength : 0.0f;
	shader.Uniforms->WhiteBalanceTemperature =
		(postfxEnabled && bd_whitebalance_enable) ? bd_whitebalance_temperature : 0.0f;
	shader.Uniforms->WhiteBalanceTint =
		(postfxEnabled && bd_whitebalance_enable) ? bd_whitebalance_tint : 0.0f;
	shader.Uniforms->HueShiftDegrees =
		(postfxEnabled && bd_hueshift_enable) ? bd_hueshift_degrees : 0.0f;
	shader.Uniforms->PosterizeLevels =
		(postfxEnabled && bd_posterize_enable) ? bd_posterize_levels : 0.0f;
	shader.Uniforms->EdgeGlowStrength =
		(postfxEnabled && bd_edgeglow_enable) ? bd_edgeglow_strength : 0.0f;
	shader.Uniforms->EdgeGlowThreshold =
		(postfxEnabled && bd_edgeglow_enable) ? bd_edgeglow_threshold : 0.0f;
	shader.Uniforms->GodRaysStrength =
		(postfxEnabled && bd_godrays_enable) ? bd_godrays_strength : 0.0f;
	shader.Uniforms->GodRaysLength = bd_godrays_length;
	shader.Uniforms->GodRaysThreshold = bd_godrays_threshold;
	shader.Uniforms->LensFlareStrength =
		(postfxEnabled && bd_lensflare_enable) ? bd_lensflare_strength : 0.0f;
	shader.Uniforms->ClarityStrength =
		(postfxEnabled && bd_clarity_enable) ? bd_clarity_strength : 0.0f;
	if (postfxEnabled && bd_grade_enable)
	{
		shader.Uniforms->GradeLiftR = bd_grade_lift_r;
		shader.Uniforms->GradeLiftG = bd_grade_lift_g;
		shader.Uniforms->GradeLiftB = bd_grade_lift_b;
		shader.Uniforms->GradeGammaR = bd_grade_gamma_r;
		shader.Uniforms->GradeGammaG = bd_grade_gamma_g;
		shader.Uniforms->GradeGammaB = bd_grade_gamma_b;
		shader.Uniforms->GradeGainR = bd_grade_gain_r;
		shader.Uniforms->GradeGainG = bd_grade_gain_g;
		shader.Uniforms->GradeGainB = bd_grade_gain_b;
	}
	else
	{
		shader.Uniforms->GradeLiftR = 0.0f;
		shader.Uniforms->GradeLiftG = 0.0f;
		shader.Uniforms->GradeLiftB = 0.0f;
		shader.Uniforms->GradeGammaR = 1.0f;
		shader.Uniforms->GradeGammaG = 1.0f;
		shader.Uniforms->GradeGammaB = 1.0f;
		shader.Uniforms->GradeGainR = 1.0f;
		shader.Uniforms->GradeGainG = 1.0f;
		shader.Uniforms->GradeGainB = 1.0f;
	}

	shader.Uniforms->HdrMode = 0;
	shader.Uniforms->ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);
	shader.Uniforms->Scale = {
		screen->mScreenViewport.width / (float)mBuffers->GetWidth(),
		screen->mScreenViewport.height / (float)mBuffers->GetHeight()
	};
	shader.Uniforms->Offset = { 0.0f, 0.0f };
	shader.Uniforms.SetData();
	static_cast<GLDataBuffer*>(shader.Uniforms.GetBuffer())->BindBase();
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentColumnInterleaved()
{
	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);
	prepareInterleavedPresent(*mPresent3dColumnShader);

	// Compute absolute offset from top of screen to top of current display window
	// because we need screen-relative, not window-relative, scan line parity

	// Todo:
	//auto clientoffset = screen->GetClientOffset();
	//auto windowHOffset = clientoffset.X % 2;
	int windowHOffset = 0;

	mPresent3dColumnShader->Uniforms->WindowPositionParity = windowHOffset;
	mPresent3dColumnShader->Uniforms.SetData();
	static_cast<GLDataBuffer*>(mPresent3dColumnShader->Uniforms.GetBuffer())->BindBase();

	RenderScreenQuad();
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentRowInterleaved()
{
	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);
	prepareInterleavedPresent(*mPresent3dRowShader);

	// Todo:
	//auto clientoffset = screen->GetClientOffset();
	//auto windowVOffset = clientoffset.Y % 2;
	int windowVOffset = 0;

	mPresent3dRowShader->Uniforms->WindowPositionParity =
		(windowVOffset
			+ screen->mOutputLetterbox.height + 1 // +1 because of origin at bottom
			) % 2;

	mPresent3dRowShader->Uniforms.SetData();
	static_cast<GLDataBuffer*>(mPresent3dRowShader->Uniforms.GetBuffer())->BindBase();
	RenderScreenQuad();
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentCheckerInterleaved()
{
	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);
	prepareInterleavedPresent(*mPresent3dCheckerShader);

	// Compute absolute offset from top of screen to top of current display window
	// because we need screen-relative, not window-relative, scan line parity

	//auto clientoffset = screen->GetClientOffset();
	//auto windowHOffset = clientoffset.X % 2;
	//auto windowVOffset = clientoffset.Y % 2;
	int windowHOffset = 0;
	int windowVOffset = 0;

	mPresent3dCheckerShader->Uniforms->WindowPositionParity =
		(windowVOffset
			+ windowHOffset
			+ screen->mOutputLetterbox.height + 1 // +1 because of origin at bottom
			) % 2; // because we want the top pixel offset, but gl_FragCoord.y is the bottom pixel offset

	mPresent3dCheckerShader->Uniforms.SetData();
	static_cast<GLDataBuffer*>(mPresent3dCheckerShader->Uniforms.GetBuffer())->BindBase();
	RenderScreenQuad();
}

//==========================================================================
//
// Sometimes the stereo render context is not ready immediately at start up
//
//==========================================================================

bool FGLRenderer::QuadStereoCheckInitialRenderContextState()
{
	// Keep trying until we see at least one good OpenGL context to render to
	bool bQuadStereoSupported = false;
	bool bDecentContextWasFound = false;
	int contextCheckCount = 0;
	if ((!bDecentContextWasFound) && (contextCheckCount < 200))
	{
		contextCheckCount += 1;
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);  // This question is about the main screen display context
		GLboolean supportsStereo, supportsBuffered;
		glGetBooleanv(GL_DOUBLEBUFFER, &supportsBuffered);
		if (supportsBuffered) // Finally, a useful OpenGL context
		{
			// This block will be executed exactly ONCE during a game run
			bDecentContextWasFound = true; // now we can stop checking every frame...
										   // Now check whether this context supports hardware stereo
			glGetBooleanv(GL_STEREO, &supportsStereo);
			bQuadStereoSupported = supportsStereo && supportsBuffered;
			if (! bQuadStereoSupported)
				UpdateVRModes(false);
		}
	}
	return bQuadStereoSupported;
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentQuadStereo()
{
	if (QuadStereoCheckInitialRenderContextState())
	{
		mBuffers->BindOutputFB();

		glDrawBuffer(GL_BACK_LEFT);
		ClearBorders();
		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(screen->mOutputLetterbox, true);

		glDrawBuffer(GL_BACK_RIGHT);
		ClearBorders();
		mBuffers->BindEyeTexture(1, 0);
		DrawPresentTexture(screen->mOutputLetterbox, true);

		glDrawBuffer(GL_BACK);
	}
	else
	{
		mBuffers->BindOutputFB();
		ClearBorders();
		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(screen->mOutputLetterbox, true);
	}
}


void FGLRenderer::PresentStereo()
{
	auto vrmode = VRMode::GetVRMode(true);
	const int eyeCount = vrmode->mEyeCount;
	// Don't invalidate the bound framebuffer (..., false)
	if (eyeCount > 1)
		mBuffers->BlitToEyeTexture(mBuffers->CurrentEye(), false);

	switch (vr_mode)
	{
	default:
		return;

	case VR_GREENMAGENTA:
		PresentAnaglyph(false, true, false);
		break;

	case VR_REDCYAN:
		PresentAnaglyph(true, false, false);
		break;

	case VR_AMBERBLUE:
		PresentAnaglyph(true, true, false);
		break;

	case VR_SIDEBYSIDEFULL:
	case VR_SIDEBYSIDESQUISHED:
	case VR_SIDEBYSIDELETTERBOX:
		PresentSideBySide(vr_mode);
		break;

	case VR_TOPBOTTOM:
		PresentTopBottom();
		break;

	case VR_ROWINTERLEAVED:
		PresentRowInterleaved();
		break;

	case VR_COLUMNINTERLEAVED:
		PresentColumnInterleaved();
		break;

	case VR_CHECKERINTERLEAVED:
		PresentCheckerInterleaved();
		break;

	case VR_QUADSTEREO:
		PresentQuadStereo();
		break;
	}
}

}
