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

#include "vk_postprocess.h"
#include "flatvertices.h"
#include "hw_cvars.h"
#include "hw_vrmodes.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "r_videoscale.h"
#include "vulkan/renderer/vk_pprenderstate.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/shaders/vk_ppshader.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_hwbuffer.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/textures/vk_imagetransition.h"
#include "vulkan/textures/vk_pptexture.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_texture.h"
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkanswapchain.h>

EXTERN_CVAR(Int, gl_dither_bpc)

VkPostprocess::VkPostprocess(VulkanRenderDevice *fb) : fb(fb) {}

VkPostprocess::~VkPostprocess() {}

void VkPostprocess::SetActiveRenderTarget() {
  auto buffers = fb->GetBuffers();

  VkImageTransition()
      .AddImage(&buffers->PipelineImage[mCurrentPipelineImage],
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
      .AddImage(&buffers->PipelineDepthStencil,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false)
      .Execute(fb->GetCommands()->GetDrawCommands());

  fb->GetRenderState()->SetRenderTarget(
      &buffers->PipelineImage[mCurrentPipelineImage],
      buffers->PipelineDepthStencil.View.get(), buffers->GetWidth(),
      buffers->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_SAMPLE_COUNT_1_BIT);
}

void VkPostprocess::PostProcessScene(
    int fixedcm, float flash,
    const std::function<void()> &afterBloomDrawEndScene2D) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  VkPPRenderState renderstate(fb);

  hw_postprocess.Pass1(&renderstate, fixedcm, sceneWidth, sceneHeight);
  SetActiveRenderTarget();
  afterBloomDrawEndScene2D();
  hw_postprocess.Pass2(&renderstate, fixedcm, flash, sceneWidth, sceneHeight);
}

void VkPostprocess::BlitSceneToPostprocess() {
  fb->GetRenderState()->EndRenderPass();

  auto buffers = fb->GetBuffers();
  auto cmdbuffer = fb->GetCommands()->GetDrawCommands();

  mCurrentPipelineImage = 0;

  VkImageTransition()
      .AddImage(&buffers->SceneColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                false)
      .AddImage(&buffers->PipelineImage[mCurrentPipelineImage],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
      .Execute(fb->GetCommands()->GetDrawCommands());

  if (buffers->GetSceneSamples() != VK_SAMPLE_COUNT_1_BIT) {
    auto sceneColor = buffers->SceneColor.Image.get();
    VkImageResolve resolve = {};
    resolve.srcOffset = {0, 0, 0};
    resolve.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolve.srcSubresource.mipLevel = 0;
    resolve.srcSubresource.baseArrayLayer = 0;
    resolve.srcSubresource.layerCount = 1;
    resolve.dstOffset = {0, 0, 0};
    resolve.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolve.dstSubresource.mipLevel = 0;
    resolve.dstSubresource.baseArrayLayer = 0;
    resolve.dstSubresource.layerCount = 1;
    resolve.extent = {(uint32_t)sceneColor->width, (uint32_t)sceneColor->height,
                      1};
    cmdbuffer->resolveImage(
        sceneColor->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        buffers->PipelineImage[mCurrentPipelineImage].Image->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);
  } else {
    auto sceneColor = buffers->SceneColor.Image.get();
    VkImageBlit blit = {};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {sceneColor->width, sceneColor->height, 1};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = 0;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {sceneColor->width, sceneColor->height, 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel = 0;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;
    cmdbuffer->blitImage(
        sceneColor->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        buffers->PipelineImage[mCurrentPipelineImage].Image->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
  }
}

void VkPostprocess::ImageTransitionScene(bool undefinedSrcLayout) {
  auto buffers = fb->GetBuffers();

  VkImageTransition()
      .AddImage(&buffers->SceneColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                undefinedSrcLayout)
      .AddImage(&buffers->SceneFog, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                undefinedSrcLayout)
      .AddImage(&buffers->SceneNormal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                undefinedSrcLayout)
      .AddImage(&buffers->SceneDepthStencil,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                undefinedSrcLayout)
      .Execute(fb->GetCommands()->GetDrawCommands());
}

void VkPostprocess::BlitCurrentToImage(VkTextureImage *dstimage,
                                       VkImageLayout finallayout) {
  fb->GetRenderState()->EndRenderPass();

  auto srcimage = &fb->GetBuffers()->PipelineImage[mCurrentPipelineImage];
  auto cmdbuffer = fb->GetCommands()->GetDrawCommands();

  VkImageTransition()
      .AddImage(srcimage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
      .AddImage(dstimage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true)
      .Execute(cmdbuffer);

  VkImageBlit blit = {};
  blit.srcOffsets[0] = {0, 0, 0};
  blit.srcOffsets[1] = {srcimage->Image->width, srcimage->Image->height, 1};
  blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  blit.srcSubresource.mipLevel = 0;
  blit.srcSubresource.baseArrayLayer = 0;
  blit.srcSubresource.layerCount = 1;
  blit.dstOffsets[0] = {0, 0, 0};
  blit.dstOffsets[1] = {dstimage->Image->width, dstimage->Image->height, 1};
  blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  blit.dstSubresource.mipLevel = 0;
  blit.dstSubresource.baseArrayLayer = 0;
  blit.dstSubresource.layerCount = 1;

  cmdbuffer->blitImage(
      srcimage->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      dstimage->Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
      VK_FILTER_NEAREST);

  VkImageTransition().AddImage(dstimage, finallayout, false).Execute(cmdbuffer);
}

void VkPostprocess::DrawPresentTexture(const IntRect &box, bool applyGamma,
                                       bool screenshot, PPTexture *output) {
  VkPPRenderState renderstate(fb);

  // Run custom "screen" shaders here for both the live present and the
  // screenshot capture (GetScreenshotBuffer composes a fresh frame), matching
  // the GL backend where captures include the fully presented image.
  hw_postprocess.customShaders.Run(&renderstate, "screen");

  PresentUniforms uniforms{};
  if (!applyGamma) {
    uniforms.InvGamma = 1.0f;
    uniforms.Contrast = 1.0f;
    uniforms.Brightness = 0.0f;
    uniforms.Saturation = 1.0f;
  } else {
    uniforms.InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
    uniforms.Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
    uniforms.Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
    uniforms.Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
    uniforms.GrayFormula = static_cast<int>(gl_satformula);
  }
  uniforms.AtmosphereMode = gl_atmosphere;
  uniforms.AtmosphereIntensity = gl_atmosphere_intensity;
  uniforms.AtmosphereContrast = gl_atmosphere_contrast;
  const int postfxQuality = bd_postfx_quality;
  const bool postfxEnabled = (bd_postfx_enable && postfxQuality > 0);
  uniforms.CrtMode = (postfxEnabled ? gl_crt_mode : 0);
  uniforms.CrtDistortion = gl_crt_distortion;
  uniforms.CrtZoom = gl_crt_zoom;
  uniforms.CrtScanline = gl_crt_scanline;
  uniforms.CrtScanlineDensity = gl_crt_scanline_density;
  uniforms.CrtScanlineSharpness = gl_crt_scanline_sharpness;
  uniforms.CrtMaskIntensity = gl_crt_mask_intensity;
  uniforms.NtscMode = (postfxEnabled ? gl_ntsc_mode : 0);
  uniforms.VignetteStrength =
      (postfxEnabled && bd_vignette_enable) ? bd_vignette_strength : 0.0f;
  uniforms.ChromaticStrength =
      (postfxEnabled && bd_chromatic_enable) ? bd_chromatic_strength : 0.0f;
  uniforms.FilmgrainStrength =
      (postfxEnabled && bd_filmgrain_enable) ? bd_filmgrain_strength : 0.0f;
  uniforms.FilmgrainScale = bd_filmgrain_scale;
  uniforms.SharpenStrength =
      (postfxEnabled && bd_sharpen_enable) ? bd_sharpen_strength : 0.0f;
  uniforms.RetroPixelScale =
      (postfxEnabled && bd_retro_pixel_enable) ? bd_retro_pixel_scale : 1.0f;
  uniforms.ColorgradeMode =
      (postfxEnabled && bd_colorgrade_strength > 0.0f) ? bd_colorgrade_mode : 0;
  uniforms.ColorgradeStrength =
      (postfxEnabled) ? bd_colorgrade_strength : 0.0f;
  uniforms.ColorgradeLut =
      (postfxEnabled && bd_colorgrade_strength > 0.0f) ? bd_colorgrade_lut : 0;
  uniforms.VhsStrength =
      (postfxEnabled && bd_vhs_enable) ? bd_vhs_strength : 0.0f;
  uniforms.VhsScanline =
      (postfxEnabled && bd_vhs_enable) ? bd_vhs_scanline : 0.0f;
  uniforms.VhsJitter =
      (postfxEnabled && bd_vhs_enable) ? bd_vhs_jitter : 0.0f;
  uniforms.VhsTime = (postfxEnabled && bd_vhs_enable) ? (screen->FrameTime * 0.001f) : 0.0f;
  uniforms.VhsTracking =
      (postfxEnabled && bd_vhs_enable) ? bd_vhs_tracking : 0.0f;
  uniforms.VhsGhosting =
      (postfxEnabled && bd_vhs_enable) ? bd_vhs_ghosting : 0.0f;
  uniforms.VhsNoise =
      (postfxEnabled && bd_vhs_enable) ? bd_vhs_noise : 0.0f;
  uniforms.VhsEvil =
      (postfxEnabled && bd_vhs_enable) ? bd_vhs_evil : 0.0f;
  uniforms.VhsPanicEnable =
      (postfxEnabled && bd_vhs_enable && bd_vhs_panic_enable) ? 1 : 0;
  uniforms.GradeEnable = (postfxEnabled && bd_grade_enable) ? 1 : 0;
  uniforms.VibranceStrength =
      (postfxEnabled && bd_vibrance_enable) ? bd_vibrance_strength : 0.0f;
  uniforms.WhiteBalanceTemperature =
      (postfxEnabled && bd_whitebalance_enable) ? bd_whitebalance_temperature
                                                : 0.0f;
  uniforms.WhiteBalanceTint =
      (postfxEnabled && bd_whitebalance_enable) ? bd_whitebalance_tint : 0.0f;
  uniforms.HueShiftDegrees =
      (postfxEnabled && bd_hueshift_enable) ? bd_hueshift_degrees : 0.0f;
  uniforms.PosterizeLevels =
      (postfxEnabled && bd_posterize_enable) ? bd_posterize_levels : 0.0f;
  uniforms.EdgeGlowStrength =
      (postfxEnabled && bd_edgeglow_enable) ? bd_edgeglow_strength : 0.0f;
  uniforms.EdgeGlowThreshold =
      (postfxEnabled && bd_edgeglow_enable) ? bd_edgeglow_threshold : 0.0f;
  uniforms.GodRaysStrength =
      (postfxEnabled && bd_godrays_enable) ? bd_godrays_strength : 0.0f;
  uniforms.GodRaysLength = bd_godrays_length;
  uniforms.GodRaysThreshold = bd_godrays_threshold;
  uniforms.LensFlareStrength =
      (postfxEnabled && bd_lensflare_enable) ? bd_lensflare_strength : 0.0f;
  uniforms.ClarityStrength =
      (postfxEnabled && bd_clarity_enable) ? bd_clarity_strength : 0.0f;
  if (postfxEnabled && bd_grade_enable) {
    uniforms.GradeLiftR = bd_grade_lift_r;
    uniforms.GradeLiftG = bd_grade_lift_g;
    uniforms.GradeLiftB = bd_grade_lift_b;
    uniforms.GradeGammaR = bd_grade_gamma_r;
    uniforms.GradeGammaG = bd_grade_gamma_g;
    uniforms.GradeGammaB = bd_grade_gamma_b;
    uniforms.GradeGainR = bd_grade_gain_r;
    uniforms.GradeGainG = bd_grade_gain_g;
    uniforms.GradeGainB = bd_grade_gain_b;
  } else {
    uniforms.GradeLiftR = 0.0f;
    uniforms.GradeLiftG = 0.0f;
    uniforms.GradeLiftB = 0.0f;
    uniforms.GradeGammaR = 1.0f;
    uniforms.GradeGammaG = 1.0f;
    uniforms.GradeGammaB = 1.0f;
    uniforms.GradeGainR = 1.0f;
    uniforms.GradeGainG = 1.0f;
    uniforms.GradeGainB = 1.0f;
  }
  uniforms.ColorScale =
      (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);

  if (screenshot) {
    uniforms.Scale = {
        screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(),
        screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight()};
    uniforms.Offset = {0.0f, 0.0f};
  } else {
    uniforms.Scale = {
        screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(),
        -screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight()};
    uniforms.Offset = {0.0f, 1.0f};
  }

  if (applyGamma &&
      fb->GetFramebufferManager()->SwapChain->Format().colorSpace ==
          VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT &&
      !screenshot) {
    uniforms.HdrMode = 1;
  } else {
    uniforms.HdrMode = 0;
  }

  renderstate.Clear();
  renderstate.Shader = &hw_postprocess.present.Present;
  renderstate.Uniforms.Set(uniforms);
  renderstate.Viewport = box;
  renderstate.SetInputCurrent(0, ViewportLinearScale() ? PPFilterMode::Linear
                                                       : PPFilterMode::Nearest);
  renderstate.SetInputTexture(1, &hw_postprocess.present.Dither,
                              PPFilterMode::Nearest, PPWrapMode::Repeat);
  if (output)
    renderstate.SetOutputTexture(output);
  else if (screenshot)
    renderstate.SetOutputNext();
  else
    renderstate.SetOutputSwapChain();
  renderstate.SetNoBlend();
  renderstate.Draw();
}

void VkPostprocess::AmbientOccludeScene(float m5) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  VkPPRenderState renderstate(fb);
  hw_postprocess.ssao.Render(&renderstate, m5, sceneWidth, sceneHeight);

  ImageTransitionScene(false);
}

void VkPostprocess::BlurScene(float gameinfobluramount) {
  int sceneWidth = fb->GetBuffers()->GetSceneWidth();
  int sceneHeight = fb->GetBuffers()->GetSceneHeight();

  VkPPRenderState renderstate(fb);

  auto vrmode = VRMode::GetVRMode(true);
  int eyeCount = vrmode->mEyeCount;
  for (int i = 0; i < eyeCount; ++i) {
    hw_postprocess.bloom.RenderBlur(&renderstate, sceneWidth, sceneHeight,
                                    gameinfobluramount);
    if (eyeCount - i > 1)
      NextEye(eyeCount);
  }
}

void VkPostprocess::ClearTonemapPalette() {
  hw_postprocess.tonemap.ClearTonemapPalette();
}

void VkPostprocess::UpdateShadowMap() {
  if (screen->mShadowMap.PerformUpdate()) {
    VkPPRenderState renderstate(fb);
    hw_postprocess.shadowmap.Update(&renderstate);

    VkImageTransition()
        .AddImage(&fb->GetTextureManager()->Shadowmap,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
        .Execute(fb->GetCommands()->GetDrawCommands());

    screen->mShadowMap.FinishUpdate();
  }
}

void VkPostprocess::NextEye(int eyeCount) {}
