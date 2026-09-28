/*
**  Postprocessing framework
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
*/

#include "hw_postprocess_cvars.h"
#include "v_video.h"

static int GApplyingPresetCount = 0;
static constexpr int MaxGraphicsPreset = 64;
static constexpr int MaxLightingPreset = 39;
static constexpr int MaxFogPreset = 17;
static constexpr int MaxSelectableTonemap = 14;

// Each graphics preset pairs with one named lighting preset and one named fog
// preset. Pairing is applied by moving the bd_lighting_preset / bd_fog_preset
// selectors themselves, so the preset browsers always show the actual look.
// A lighting/fog selection the user made explicitly (selector differs from the
// last auto-paired value) is never overridden by graphics presets.
struct FPresetPairing
{
  int lighting;
  int fog;
};

static const FPresetPairing GGraphicsPresetPairing[] = {
  {0, 0},    // 0 Custom
  {1, 1},    // 1 Vanilla+
  {2, 1},    // 2 Modern Crisp
  {18, 1},   // 3 CRT Arcade
  {4, 4},    // 4 VHS Horror
  {11, 6},   // 5 Industrial Hell
  {1, 1},    // 6 Low-End Performance
  {4, 5},    // 7 Silent Hill Fog
  {12, 4},   // 8 Ashen Graveyard
  {30, 8},   // 9 Toxic Reactor
  {24, 11},  // 10 Moonlit Noir
  {11, 14},  // 11 Inferno Bloom
  {37, 15},  // 12 Frozen Wasteland
  {32, 6},   // 13 Sodium Streets
  {35, 11},  // 14 Cyberpunk Rain
  {16, 3},   // 15 Bleach Bunker
  {39, 17},  // 16 Analog Horror
  {26, 7},   // 17 Dream Decay
  {10, 2},   // 18 Low Light Realism
  {7, 1},    // 19 Clean Visibility
  {3, 6},    // 20 Warm Cinematic
  {9, 3},    // 21 Cool Clarity
  {8, 5},    // 22 Dense Playable Fog
  {18, 1},   // 23 Readable CRT
  {4, 4},    // 24 Action Horror
  {21, 4},   // 25 VHS Found Footage
  {4, 4},    // 26 VHS Tape Rot
  {30, 9},   // 27 VHS Night Vision
  {12, 9},   // 28 Possessed VHS
  {28, 12},  // 29 Blood Moon Evil
  {12, 9},   // 30 Void Ritual
  {3, 4},    // 31 Cinematic Ultra
  {5, 1},    // 32 Neon Vibrance
  {18, 1},   // 33 Retro Poster
  {13, 1},   // 34 Cel Comic
  {27, 3},   // 35 Sepia Archive
  {1, 1},    // 36 Ultra Lightweight
  {1, 2},    // 37 Balanced Performance
  {7, 1},    // 38 Competitive Clarity
  {6, 3},    // 39 HDR Showcase
  {2, 4},    // 40 Maxed Out
  {34, 16},  // 41 Divine Radiance
  {3, 3},    // 42 Analog Cinema
  {9, 1},    // 43 Clarity Max
  {38, 7},   // 44 Dreamlike
  {8, 3},    // 45 Soft Bloom
  {5, 1},    // 46 Neon Bloom
  {8, 2},    // 47 Subtle Film
  {8, 7},    // 48 Muted Pastels
  {33, 4},   // 49 Dark Ambient
  {14, 3},   // 50 Bright Ambient
  {18, 1},   // 51 Sharp Retro
  {1, 1},    // 52 Soft Retro
  {35, 11},  // 53 Noir Punch
  {19, 1},   // 54 Cel Shadows
  {38, 7},   // 55 Watercolor Dream
  {29, 1},   // 56 Overexposed
  {2, 1},    // 57 Clean Lens
  {8, 2},    // 58 Low Glow
  {30, 16},  // 59 Spectral
  {15, 6},   // 60 Golden Film
  {37, 3},   // 61 Cold Facility
  {26, 11},  // 62 Violet Dusk
  {8, 7},    // 63 Soft Focus
  {5, 1},    // 64 Arcade Neon
};
static_assert(sizeof(GGraphicsPresetPairing) / sizeof(GGraphicsPresetPairing[0]) == MaxGraphicsPreset + 1,
              "every graphics preset needs a lighting/fog pairing");

// Durable record of the last lighting/fog preset ID installed by graphics
// preset auto-pairing. These are archived global-config CVARs (not in-memory
// state) because config load replays every archived selector independently,
// which would destroy in-memory tracking depending on load order. They are
// reset to 0 whenever the user changes a selector directly, so explicit
// choices are always respected.
EXTERN_CVAR(Int, bd_autopaired_lighting)
EXTERN_CVAR(Int, bd_autopaired_fog)

static bool IsApplyingPreset()
{
  return GApplyingPresetCount > 0 || C_InInitialCallbackReplay();
}

class FPresetApplyScope
{
public:
  FPresetApplyScope() { ++GApplyingPresetCount; }
  ~FPresetApplyScope() { --GApplyingPresetCount; }

  FPresetApplyScope(const FPresetApplyScope &) = delete;
  FPresetApplyScope &operator=(const FPresetApplyScope &) = delete;
};

EXTERN_CVAR(Bool, gl_light_shadowmap)
EXTERN_CVAR(Int, gl_shadowmap_quality)
EXTERN_CVAR(Int, gl_shadowmap_filter)

static void SetPresetDirtyFromFeatureChange() {
  if (!IsApplyingPreset() && !bd_preset_locked && bd_graphics_preset != 0) {
    bd_graphics_preset = 0;
  }
}

static void SetLightingPresetDirtyFromFeatureChange()
{
  if (!IsApplyingPreset() && !bd_preset_locked && bd_lighting_preset != 0)
    bd_lighting_preset = 0;
}

static void EnsurePostFxActive() {
  if (IsApplyingPreset())
    return;

  if (!bd_postfx_enable)
    bd_postfx_enable = true;

  if (bd_postfx_quality <= 0)
    bd_postfx_quality = 3;
}

static void OnPresetFeatureChanged(FIntCVar &)
{
  SetPresetDirtyFromFeatureChange();
}

static void OnPresetFeatureChanged(FFloatCVar &)
{
  SetPresetDirtyFromFeatureChange();
}

static void OnPresetFeatureChanged(FBoolCVar &)
{
  SetPresetDirtyFromFeatureChange();
}

static void OnPresetFeatureChanged(FColorCVar &)
{
  SetPresetDirtyFromFeatureChange();
}

static void OnLightingFeatureChanged(FIntCVar &)
{
  SetLightingPresetDirtyFromFeatureChange();
}

static void OnLightingFeatureChanged(FFloatCVar &)
{
  SetLightingPresetDirtyFromFeatureChange();
}

static void OnLightingFeatureChanged(FBoolCVar &)
{
  SetLightingPresetDirtyFromFeatureChange();
}

static void OnLightingFeatureChanged(FColorCVar &)
{
  SetLightingPresetDirtyFromFeatureChange();
}

template <class TCVar>
static void OnFogFeatureChanged(TCVar &)
{
  if (!IsApplyingPreset() && !bd_preset_locked && bd_fog_preset != 0)
    bd_fog_preset = 0;
}

static void SetFogPresetColor(int color)
{
  bd_fog_color->SetGenericRep(CVarValue<CVAR_Color>(color), CVAR_Color);
}

static void SetLightingValues(int falloffMode, float falloffExponent, float intensity, float saturation,
                              float temperature, float ambientFloor, float specularScale, float emissiveBoost,
                              bool giAmbient, float giAmbientStrength, bool refineSprites,
                              float rangeScale = 1.0f, float falloffSoftness = 0.0f, float wrap = 0.0f,
                              float indirect = 0.0f, float shadowStrength = 1.0f,
                              float lightContrast = 1.0f, float specularPowerScale = 1.0f,
                              float rimStrength = 0.0f, float rimPower = 3.0f,
                              float ambientGradient = 0.0f, uint32_t ambientGradientColor = 0x8899bb,
                              float flicker = 0.0f, float aerial = 0.0f, float aerialDist = 2048.0f,
                              uint32_t specularTint = 0xffffff)
{
  bd_dynlight_falloff_mode = falloffMode;
  bd_dynlight_falloff_exponent = falloffExponent;
  bd_dynlight_intensity = intensity;
  bd_dynlight_saturation = saturation;
  bd_dynlight_range_scale = rangeScale;
  bd_dynlight_falloff_softness = falloffSoftness;
  bd_dynlight_wrap = wrap;
  bd_dynlight_indirect = indirect;
  bd_dynlight_shadow_strength = shadowStrength;
  bd_light_temperature = temperature;
  bd_light_ambient_floor = ambientFloor;
  bd_light_specular_scale = specularScale;
  bd_emissive_boost = emissiveBoost;
  bd_gi_ambient_enable = giAmbient;
  bd_gi_ambient_strength = giAmbientStrength;
  bd_sprite_lighting_refine = refineSprites;
  bd_light_contrast = lightContrast;
  bd_specular_power_scale = specularPowerScale;
  bd_rimlight_strength = rimStrength;
  bd_rimlight_power = rimPower;
  bd_ambient_gradient_strength = ambientGradient;
  bd_ambient_gradient_color->SetGenericRep(CVarValue<CVAR_Color>((int)ambientGradientColor), CVAR_Color);
  bd_dynlight_flicker = flicker;
  bd_aerial_strength = aerial;
  bd_aerial_distance = aerialDist;
  bd_specular_tint->SetGenericRep(CVarValue<CVAR_Color>((int)specularTint), CVAR_Color);
}

static void ApplyLightingPreset(int preset)
{
  switch (preset)
  {
  case 0: // Custom
    return;
  case 1: // Classic Balanced
    SetLightingValues(0, 2.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, false, 0.0f, false,
                      1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    return;
  case 2: // Modern Pretty
    SetLightingValues(1, 1.85f, 1.22f, 1.10f, 0.04f, 0.035f, 1.28f, 0.24f, true, 0.28f, true,
                      1.25f, 0.38f, 0.22f, 0.13f, 0.80f);
    return;
  case 3: // Warm Cinematic
    SetLightingValues(2, 2.15f, 1.28f, 1.08f, 0.34f, 0.055f, 1.32f, 0.34f, true, 0.32f, true,
                      1.28f, 0.42f, 0.24f, 0.16f, 0.76f);
    return;
  case 4: // Horror Contrast
    SetLightingValues(2, 2.85f, 0.92f, 0.74f, -0.12f, 0.018f, 0.88f, 0.08f, true, 0.18f, true,
                      1.10f, 0.18f, 0.04f, 0.03f, 1.0f);
    return;
  case 5: // Neon Glow
    SetLightingValues(1, 1.45f, 1.70f, 1.75f, -0.20f, 0.055f, 1.65f, 0.78f, true, 0.28f, true,
                      1.55f, 0.58f, 0.34f, 0.24f, 0.58f);
    return;
  case 6: // PBR Showcase
    SetLightingValues(1, 1.75f, 1.32f, 1.14f, 0.0f, 0.045f, 2.05f, 0.48f, true, 0.26f, true,
                      1.32f, 0.34f, 0.16f, 0.12f, 0.86f);
    return;
  case 7: // Bright Playable
    SetLightingValues(1, 1.55f, 1.22f, 0.98f, 0.02f, 0.13f, 1.00f, 0.16f, true, 0.44f, true,
                      1.45f, 0.52f, 0.34f, 0.22f, 0.55f);
    return;
  case 8: // Soft Natural
    SetLightingValues(1, 1.75f, 1.08f, 0.96f, 0.06f, 0.06f, 1.10f, 0.10f, true, 0.36f, true,
                      1.30f, 0.55f, 0.30f, 0.20f, 0.62f);
    return;
  case 9: // Crisp Tactical
    SetLightingValues(2, 2.15f, 1.12f, 0.92f, -0.03f, 0.035f, 1.20f, 0.06f, true, 0.24f, true,
                      1.15f, 0.16f, 0.08f, 0.05f, 0.92f);
    return;
  case 10: // Low Light Realism
    SetLightingValues(2, 2.75f, 0.82f, 0.68f, -0.18f, 0.012f, 0.95f, 0.05f, true, 0.12f, true,
                      1.05f, 0.14f, 0.02f, 0.02f, 1.0f);
    return;
  case 11: // Hellfire Glow
    SetLightingValues(2, 2.05f, 1.45f, 1.28f, 0.48f, 0.05f, 1.45f, 0.50f, true, 0.30f, true,
                      1.36f, 0.44f, 0.24f, 0.18f, 0.76f);
    return;
  case 12: // Void Dread
    SetLightingValues(2, 3.10f, 0.70f, 0.50f, -0.45f, 0.015f, 0.72f, 0.16f, true, 0.16f, true,
                      1.22f, 0.26f, 0.06f, 0.05f, 1.0f);
    return;
  case 13: // Studio Soft
    SetLightingValues(1, 1.60f, 1.10f, 1.00f, 0.10f, 0.06f, 1.00f, 0.10f, true, 0.32f, true,
                      1.20f, 0.50f, 0.30f, 0.15f, 0.70f,
                      0.85f, 1.0f, 0.10f, 3.0f);
    return;
  case 14: // Overcast Day
    SetLightingValues(1, 1.70f, 1.00f, 0.95f, -0.15f, 0.15f, 0.90f, 0.05f, true, 0.40f, true,
                      1.25f, 0.55f, 0.35f, 0.18f, 0.60f,
                      0.80f, 0.8f, 0.0f, 3.0f);
    return;
  case 15: // Golden Hour
    SetLightingValues(1, 1.95f, 1.25f, 1.15f, 0.45f, 0.04f, 1.40f, 0.30f, true, 0.30f, true,
                      1.30f, 0.40f, 0.22f, 0.15f, 0.75f,
                      1.15f, 1.5f, 0.15f, 3.0f);
    return;
  case 16: // Cold Industrial
    SetLightingValues(2, 2.60f, 1.10f, 0.90f, -0.50f, 0.03f, 1.40f, 0.15f, true, 0.22f, true,
                      1.15f, 0.10f, 0.05f, 0.05f, 0.90f,
                      1.20f, 2.0f, 0.0f, 4.0f);
    return;
  case 17: // Pitch Black
    SetLightingValues(2, 2.90f, 0.75f, 0.60f, -0.20f, 0.0f, 0.90f, 0.05f, true, 0.10f, true,
                      1.05f, 0.12f, 0.02f, 0.02f, 1.0f,
                      1.60f, 1.0f, 0.20f, 3.5f);
    return;
  case 18: // Arcade Bright
    SetLightingValues(1, 1.60f, 1.50f, 1.20f, 0.0f, 0.08f, 1.10f, 0.35f, true, 0.35f, true,
                      1.35f, 0.35f, 0.25f, 0.15f, 0.55f,
                      0.90f, 1.0f, 0.0f, 3.0f);
    return;
  case 19: // Rim Drama
    SetLightingValues(2, 2.20f, 1.05f, 1.05f, -0.10f, 0.02f, 1.30f, 0.20f, true, 0.20f, true,
                      1.25f, 0.30f, 0.10f, 0.08f, 0.90f,
                      1.30f, 1.3f, 0.50f, 2.5f);
    return;
  case 20: // Gradient Ambience
    SetLightingValues(1, 1.75f, 1.08f, 0.96f, 0.06f, 0.06f, 1.10f, 0.10f, true, 0.36f, true,
                      1.30f, 0.55f, 0.30f, 0.20f, 0.62f,
                      1.0f, 1.0f, 0.0f, 3.0f, 0.5f, 0x7fa8d8);
    return;
  case 21: // Flickering Candlelight
    SetLightingValues(2, 2.40f, 0.90f, 0.95f, 0.50f, 0.010f, 1.00f, 0.20f, true, 0.20f, true,
                      1.10f, 0.20f, 0.10f, 0.05f, 1.00f,
                      1.30f, 1.20f, 0.05f, 3.0f, 0.0f, 0x8899bb,
                      0.55f);
    return;
  case 22: // Aerial Vista
    SetLightingValues(1, 1.75f, 1.08f, 0.96f, -0.05f, 0.06f, 1.10f, 0.10f, true, 0.36f, true,
                      1.30f, 0.40f, 0.20f, 0.20f, 0.62f,
                      1.0f, 1.0f, 0.0f, 3.0f, 0.30f, 0x7fa8d8,
                      0.0f, 0.50f, 3000.0f);
    return;
  case 23: // Candlelit Crypt
    SetLightingValues(2, 2.35f, 0.88f, 1.05f, 0.55f, 0.02f, 0.95f, 0.28f, true, 0.20f, true,
                      1.12f, 0.28f, 0.12f, 0.08f, 0.90f,
                      1.18f, 1.35f, 0.28f, 2.8f, 0.18f, 0x9a6840,
                      0.62f, 0.12f, 900.0f, 0xffc080);
    return;
  case 24: // Moonlit Expanse
    SetLightingValues(1, 1.70f, 1.02f, 0.88f, -0.55f, 0.10f, 1.05f, 0.08f, true, 0.38f, true,
                      1.45f, 0.50f, 0.28f, 0.22f, 0.70f,
                      0.92f, 1.10f, 0.18f, 3.5f, 0.34f, 0x6f8fc8,
                      0.0f, 0.42f, 4200.0f, 0xb8c8ff);
    return;
  case 25: // Emergency Strobe
    SetLightingValues(2, 2.25f, 1.18f, 1.20f, -0.08f, 0.03f, 1.35f, 0.35f, true, 0.22f, true,
                      1.18f, 0.18f, 0.08f, 0.08f, 0.82f,
                      1.38f, 1.60f, 0.32f, 2.6f, 0.10f, 0x506070,
                      0.68f, 0.08f, 1200.0f, 0xff4040);
    return;
  case 26: // Aurora Veil
    SetLightingValues(1, 1.55f, 1.18f, 1.55f, -0.35f, 0.07f, 1.25f, 0.45f, true, 0.36f, true,
                      1.50f, 0.60f, 0.34f, 0.30f, 0.58f,
                      0.96f, 1.20f, 0.25f, 3.2f, 0.55f, 0x58c8a8,
                      0.0f, 0.45f, 3600.0f, 0x9fd8ff);
    return;
  case 27: // Dusty Archive
    SetLightingValues(1, 1.90f, 0.98f, 0.82f, 0.28f, 0.11f, 0.92f, 0.08f, true, 0.34f, true,
                      1.22f, 0.48f, 0.26f, 0.24f, 0.72f,
                      0.90f, 0.95f, 0.08f, 3.8f, 0.38f, 0xa58f68,
                      0.04f, 0.32f, 2400.0f, 0xd8c8a8);
    return;
  case 28: // Ruby Corridor
    SetLightingValues(2, 2.05f, 1.25f, 1.38f, 0.30f, 0.04f, 1.45f, 0.42f, true, 0.25f, true,
                      1.26f, 0.32f, 0.18f, 0.14f, 0.76f,
                      1.28f, 1.45f, 0.40f, 2.4f, 0.22f, 0x8c3040,
                      0.12f, 0.16f, 1500.0f, 0xff7888);
    return;
  case 29: // Surgical White
    SetLightingValues(1, 1.50f, 1.08f, 0.72f, -0.12f, 0.16f, 1.55f, 0.02f, true, 0.42f, true,
                      1.38f, 0.42f, 0.24f, 0.18f, 0.66f,
                      0.84f, 1.80f, 0.04f, 4.2f, 0.12f, 0xb8c8c8,
                      0.0f, 0.10f, 2600.0f, 0xe8ffff);
    return;
  case 30: // Ectoplasm
    SetLightingValues(1, 1.65f, 1.12f, 1.65f, -0.42f, 0.08f, 1.15f, 0.55f, true, 0.32f, true,
                      1.42f, 0.58f, 0.30f, 0.26f, 0.60f,
                      0.95f, 1.15f, 0.34f, 2.9f, 0.48f, 0x5fbf7a,
                      0.10f, 0.36f, 2100.0f, 0x88ffb0);
    return;
  case 31: // Storm Front
    SetLightingValues(2, 2.45f, 1.04f, 0.86f, -0.38f, 0.05f, 1.28f, 0.12f, true, 0.24f, true,
                      1.24f, 0.24f, 0.12f, 0.10f, 0.84f,
                      1.22f, 1.50f, 0.30f, 3.1f, 0.25f, 0x4d6578,
                      0.36f, 0.48f, 2800.0f, 0x90b8d8);
    return;
  case 32: // Amber Ember
    SetLightingValues(2, 2.00f, 1.34f, 1.28f, 0.62f, 0.04f, 1.25f, 0.48f, true, 0.28f, true,
                      1.30f, 0.36f, 0.20f, 0.16f, 0.78f,
                      1.18f, 1.25f, 0.22f, 2.7f, 0.18f, 0x9a5830,
                      0.18f, 0.22f, 1700.0f, 0xffb060);
    return;
  case 33: // Deep Cavern
    SetLightingValues(2, 3.00f, 0.72f, 0.58f, -0.28f, 0.00f, 0.78f, 0.10f, true, 0.14f, true,
                      1.08f, 0.12f, 0.04f, 0.04f, 1.00f,
                      1.55f, 1.10f, 0.26f, 3.6f, 0.10f, 0x2f3f50,
                      0.0f, 0.20f, 1300.0f, 0x7890a8);
    return;
  case 34: // Cathedral Bloom
    SetLightingValues(1, 1.80f, 1.18f, 1.12f, 0.18f, 0.09f, 1.35f, 0.38f, true, 0.40f, true,
                      1.36f, 0.52f, 0.28f, 0.24f, 0.62f,
                      0.98f, 1.35f, 0.42f, 2.5f, 0.50f, 0xb89a68,
                      0.0f, 0.38f, 3400.0f, 0xffd8a0);
    return;
  case 35: // Neon Noir
    SetLightingValues(2, 2.20f, 1.08f, 1.70f, -0.48f, 0.02f, 1.60f, 0.68f, true, 0.22f, true,
                      1.34f, 0.26f, 0.10f, 0.10f, 0.88f,
                      1.34f, 1.70f, 0.52f, 2.2f, 0.28f, 0x3a2f68,
                      0.08f, 0.18f, 1900.0f, 0xd080ff);
    return;
  case 36: // Desert Heat
    SetLightingValues(1, 1.60f, 1.12f, 1.02f, 0.70f, 0.14f, 0.95f, 0.12f, true, 0.36f, true,
                      1.48f, 0.46f, 0.30f, 0.28f, 0.68f,
                      0.88f, 0.90f, 0.10f, 4.0f, 0.26f, 0xc89a58,
                      0.0f, 0.55f, 5000.0f, 0xffd098);
    return;
  case 37: // Arctic Facility
    SetLightingValues(1, 1.70f, 1.00f, 0.78f, -0.72f, 0.12f, 1.45f, 0.05f, true, 0.38f, true,
                      1.40f, 0.40f, 0.22f, 0.18f, 0.70f,
                      0.86f, 1.90f, 0.08f, 4.5f, 0.20f, 0x8fb8d8,
                      0.0f, 0.40f, 4600.0f, 0xc8e8ff);
    return;
  case 38: // Soft Dawn
    SetLightingValues(1, 1.65f, 1.10f, 1.04f, 0.25f, 0.13f, 1.05f, 0.18f, true, 0.42f, true,
                      1.32f, 0.58f, 0.36f, 0.30f, 0.58f,
                      0.90f, 1.05f, 0.06f, 3.2f, 0.42f, 0xd8a898,
                      0.0f, 0.34f, 3800.0f, 0xffc8b0);
    return;
  case 39: // Analog Fluorescent: cold buzzing institutional light for
           // found-footage horror; preserves the old Analog Horror look.
    SetLightingValues(2, 2.65f, 0.92f, 0.78f, -0.10f, 0.030f, 0.88f, 0.10f, true, 0.32f, true,
                      1.12f, 0.24f, 0.08f, 0.05f, 0.98f,
                      1.0f, 1.0f, 0.0f, 3.0f, 0.0f, 0x8899bb,
                      0.40f, 0.25f, 1800.0f);
    return;
  default:
    return;
  }
}

static void SetFogGradientPreset(int mode, int color, float strength, float scale, float yaw, float pitch)
{
  bd_fog_gradient_mode = mode;
  bd_fog_gradient_color->SetGenericRep(CVarValue<CVAR_Color>(color), CVAR_Color);
  bd_fog_gradient_strength = strength;
  bd_fog_gradient_scale = scale;
  bd_fog_direction_yaw = yaw;
  bd_fog_direction_pitch = pitch;
}

static void ApplyFogPreset(int preset)
{
  switch (preset)
  {
  case 1: // Disabled
    bd_fog_mode = 0;
    bd_fog_sky_strength = 0.0f;
    bd_fog_thick_distance = 0.0f;
    bd_fog_quality = 0;
    bd_fog_height_falloff = 0.0f;
    bd_fog_turbulence = 0.0f;
    SetFogGradientPreset(0, 0x6b746b, 0.0f, 1.0f, 0.0f, 0.0f);
    break;
  case 2: // Preserve map-authored fog, only improve its integration.
    bd_fog_mode = 2;
    bd_sector_fog_scale = 1.0f;
    bd_fog_density = 0.0f;
    bd_fog_color_mode = 0;
    bd_fog_color_strength = 0.0f;
    bd_fog_sky_strength = 0.40f;
    bd_fog_thick_distance = 0.0f;
    bd_fog_thick_multiplier = 1.0f;
    bd_fog_quality = 1;
    bd_fog_height_falloff = 0.18f;
    bd_fog_turbulence = 0.06f;
    bd_fog_turbulence_scale = 0.010f;
    bd_fog_sky_horizon = 0.62f;
    SetFogGradientPreset(0, 0x6b746b, 0.0f, 1.0f, 0.0f, 0.0f);
    break;
  case 3: // Light, gameplay-friendly natural haze.
    bd_fog_mode = 2;
    bd_sector_fog_scale = 0.85f;
    bd_fog_density = 52.0f;
    SetFogPresetColor(0xb7c0ba);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.18f;
    bd_fog_sky_strength = 0.42f;
    bd_fog_thick_distance = 1150.0f;
    bd_fog_thick_multiplier = 1.7f;
    bd_fog_quality = 1;
    bd_fog_height_falloff = 0.35f;
    bd_fog_turbulence = 0.10f;
    bd_fog_turbulence_scale = 0.009f;
    bd_fog_sky_horizon = 0.72f;
    SetFogGradientPreset(1, 0x66706a, 0.10f, 0.65f, 0.0f, 0.0f);
    break;
  case 4: // Layered cinematic atmosphere.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 1.0f;
    bd_fog_density = 128.0f;
    SetFogPresetColor(0x9da49c);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.48f;
    bd_fog_sky_strength = 0.62f;
    bd_fog_thick_distance = 590.0f;
    bd_fog_thick_multiplier = 3.8f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 0.72f;
    bd_fog_turbulence = 0.22f;
    bd_fog_turbulence_scale = 0.008f;
    bd_fog_sky_horizon = 0.78f;
    SetFogGradientPreset(1, 0x3d4740, 0.24f, 0.95f, 0.0f, 0.0f);
    break;
  case 5: // Dense readable horror fog.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 1.0f;
    bd_fog_density = 205.0f;
    SetFogPresetColor(0x747c72);
    bd_fog_color_mode = 1;
    bd_fog_color_strength = 0.76f;
    bd_fog_sky_strength = 0.84f;
    bd_fog_thick_distance = 340.0f;
    bd_fog_thick_multiplier = 7.5f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 1.15f;
    bd_fog_turbulence = 0.30f;
    bd_fog_turbulence_scale = 0.011f;
    bd_fog_sky_horizon = 0.88f;
    SetFogGradientPreset(2, 0x202820, 0.42f, 1.30f, 0.0f, -8.0f);
    break;
  case 6: // Warm directional haze for large outdoor scenes.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 0.95f;
    bd_fog_density = 148.0f;
    SetFogPresetColor(0x9b8875);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.55f;
    bd_fog_sky_strength = 0.68f;
    bd_fog_thick_distance = 520.0f;
    bd_fog_thick_multiplier = 4.5f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 0.62f;
    bd_fog_turbulence = 0.18f;
    bd_fog_turbulence_scale = 0.006f;
    bd_fog_sky_horizon = 0.82f;
    SetFogGradientPreset(2, 0x392719, 0.32f, 1.05f, 28.0f, -5.0f);
    break;
  case 7: // Morning Mist: soft cool white-blue haze, gentle gradient.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 0.90f;
    bd_fog_density = 85.0f;
    SetFogPresetColor(0xc9d6de);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.35f;
    bd_fog_sky_strength = 0.50f;
    bd_fog_thick_distance = 800.0f;
    bd_fog_thick_multiplier = 2.5f;
    bd_fog_quality = 1;
    bd_fog_height_falloff = 0.50f;
    bd_fog_turbulence = 0.08f;
    bd_fog_turbulence_scale = 0.008f;
    bd_fog_sky_horizon = 0.70f;
    SetFogGradientPreset(1, 0x8fa5b5, 0.14f, 0.80f, 0.0f, 0.0f);
    break;
  case 8: // Toxic Haze: green-yellow tint, thick mid-distance fog.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 1.05f;
    bd_fog_density = 170.0f;
    SetFogPresetColor(0x8f9c46);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.60f;
    bd_fog_sky_strength = 0.60f;
    bd_fog_thick_distance = 450.0f;
    bd_fog_thick_multiplier = 5.0f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 0.80f;
    bd_fog_turbulence = 0.25f;
    bd_fog_turbulence_scale = 0.010f;
    bd_fog_sky_horizon = 0.80f;
    SetFogGradientPreset(1, 0x4a5226, 0.28f, 1.0f, 0.0f, -3.0f);
    break;
  case 9: // Blackout: near-field visibility with a hard oppressive wall.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 1.25f;
    bd_fog_density = 210.0f;
    SetFogPresetColor(0x050607);
    bd_fog_color_mode = 1;
    bd_fog_color_strength = 0.78f;
    bd_fog_sky_strength = 0.80f;
    bd_fog_thick_distance = 300.0f;
    bd_fog_thick_multiplier = 8.0f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 1.20f;
    bd_fog_turbulence = 0.18f;
    bd_fog_turbulence_scale = 0.008f;
    bd_fog_sky_horizon = 0.90f;
    SetFogGradientPreset(1, 0x000000, 0.45f, 1.25f, 0.0f, 0.0f);
    break;
  case 10: // Green Valley: soft cool vegetation haze.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 0.82f;
    bd_fog_density = 95.0f;
    SetFogPresetColor(0x9fbf9a);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.38f;
    bd_fog_sky_strength = 0.52f;
    bd_fog_thick_distance = 820.0f;
    bd_fog_thick_multiplier = 2.6f;
    bd_fog_quality = 1;
    bd_fog_height_falloff = 0.42f;
    bd_fog_turbulence = 0.12f;
    bd_fog_turbulence_scale = 0.009f;
    bd_fog_sky_horizon = 0.72f;
    SetFogGradientPreset(1, 0x6f9470, 0.18f, 0.85f, 0.0f, -4.0f);
    break;
  case 11: // Blue Hour: deep blue dusk falloff.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 0.95f;
    bd_fog_density = 145.0f;
    SetFogPresetColor(0x50688f);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.58f;
    bd_fog_sky_strength = 0.68f;
    bd_fog_thick_distance = 560.0f;
    bd_fog_thick_multiplier = 4.2f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 0.75f;
    bd_fog_turbulence = 0.16f;
    bd_fog_turbulence_scale = 0.007f;
    bd_fog_sky_horizon = 0.82f;
    SetFogGradientPreset(2, 0x18243c, 0.34f, 1.10f, -20.0f, -8.0f);
    break;
  case 12: // Crimson Eclipse: directional red-brown gloom.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 1.08f;
    bd_fog_density = 175.0f;
    SetFogPresetColor(0x7a3b32);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.66f;
    bd_fog_sky_strength = 0.72f;
    bd_fog_thick_distance = 430.0f;
    bd_fog_thick_multiplier = 5.8f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 0.88f;
    bd_fog_turbulence = 0.20f;
    bd_fog_turbulence_scale = 0.009f;
    bd_fog_sky_horizon = 0.84f;
    SetFogGradientPreset(2, 0x2a0f0c, 0.40f, 1.20f, 36.0f, -6.0f);
    break;
  case 13: // Underwater: dense blue-green depth haze.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 1.15f;
    bd_fog_density = 190.0f;
    SetFogPresetColor(0x2f6f74);
    bd_fog_color_mode = 1;
    bd_fog_color_strength = 0.70f;
    bd_fog_sky_strength = 0.78f;
    bd_fog_thick_distance = 380.0f;
    bd_fog_thick_multiplier = 6.4f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 1.05f;
    bd_fog_turbulence = 0.28f;
    bd_fog_turbulence_scale = 0.012f;
    bd_fog_sky_horizon = 0.88f;
    SetFogGradientPreset(1, 0x143c42, 0.42f, 1.30f, 0.0f, 0.0f);
    break;
  case 14: // Dust Storm: warm dry rolling dust.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 1.00f;
    bd_fog_density = 155.0f;
    SetFogPresetColor(0xb08a5f);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.62f;
    bd_fog_sky_strength = 0.70f;
    bd_fog_thick_distance = 500.0f;
    bd_fog_thick_multiplier = 5.2f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 0.55f;
    bd_fog_turbulence = 0.34f;
    bd_fog_turbulence_scale = 0.014f;
    bd_fog_sky_horizon = 0.80f;
    SetFogGradientPreset(2, 0x5c4028, 0.34f, 1.15f, 18.0f, -4.0f);
    break;
  case 15: // Polar Whiteout: bright, cold, low-contrast distance loss.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 0.92f;
    bd_fog_density = 165.0f;
    SetFogPresetColor(0xdce7ec);
    bd_fog_color_mode = 1;
    bd_fog_color_strength = 0.72f;
    bd_fog_sky_strength = 0.82f;
    bd_fog_thick_distance = 390.0f;
    bd_fog_thick_multiplier = 6.0f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 0.35f;
    bd_fog_turbulence = 0.14f;
    bd_fog_turbulence_scale = 0.006f;
    bd_fog_sky_horizon = 0.86f;
    SetFogGradientPreset(1, 0xb8cbd8, 0.38f, 0.90f, 0.0f, 0.0f);
    break;
  case 16: // Cathedral Haze: luminous vertical shafts and gentle depth.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 0.88f;
    bd_fog_density = 115.0f;
    SetFogPresetColor(0xbcae91);
    bd_fog_color_mode = 2;
    bd_fog_color_strength = 0.48f;
    bd_fog_sky_strength = 0.58f;
    bd_fog_thick_distance = 680.0f;
    bd_fog_thick_multiplier = 3.2f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 1.35f;
    bd_fog_turbulence = 0.10f;
    bd_fog_turbulence_scale = 0.007f;
    bd_fog_sky_horizon = 0.76f;
    SetFogGradientPreset(1, 0x766a54, 0.30f, 1.45f, 0.0f, 8.0f);
    break;
  case 17: // Analog Sepia: murky brown-black haze for found-footage horror;
           // preserves the old Analog Horror graphics preset's fog.
    bd_fog_mode = 1;
    bd_sector_fog_scale = 1.12f;
    bd_fog_density = 170.0f;
    SetFogPresetColor(0x4b443e);
    bd_fog_color_mode = 1;
    bd_fog_color_strength = 0.62f;
    bd_fog_sky_strength = 0.58f;
    bd_fog_thick_distance = 420.0f;
    bd_fog_thick_multiplier = 6.0f;
    bd_fog_quality = 2;
    bd_fog_height_falloff = 0.85f;
    bd_fog_turbulence = 0.22f;
    bd_fog_turbulence_scale = 0.009f;
    bd_fog_sky_horizon = 0.82f;
    SetFogGradientPreset(2, 0x120f0d, 0.30f, 1.15f, 12.0f, -6.0f);
    break;
  default:
    break;
  }
}

static void SetCrtPreset(int mode, float distortion, float zoom, float scanline, float density, float sharpness, float mask)
{
  gl_crt_mode = mode;
  gl_crt_distortion = distortion;
  gl_crt_zoom = zoom;
  gl_crt_scanline = scanline;
  gl_crt_scanline_density = density;
  gl_crt_scanline_sharpness = sharpness;
  gl_crt_mask_intensity = mask;
}

static float ClampPresetFloat(float value, float minValue, float maxValue)
{
  if (value < minValue)
    return minValue;
  if (value > maxValue)
    return maxValue;
  return value;
}

static void KeepPresetPlayable(int preset)
{
  if (preset <= 0)
    return;

  // Postfx-only clamps. Lighting and fog are owned by the paired named
  // presets (applied via the selectors in SetGraphicsPreset), so nothing here
  // may touch bd_dynlight_*/bd_light_*/bd_fog_* — doing so would override an
  // explicit user selection.
  if (bd_bloom_enable)
    bd_bloom_strength = ClampPresetFloat(bd_bloom_strength, 0.45f, 2.0f);

  if (bd_vignette_enable)
    bd_vignette_strength = ClampPresetFloat(bd_vignette_strength, 0.0f, 0.55f);

  if (bd_chromatic_enable)
    bd_chromatic_strength = ClampPresetFloat(bd_chromatic_strength, 0.0f, 0.22f);

  if (bd_filmgrain_enable)
  {
    bd_filmgrain_strength = ClampPresetFloat(bd_filmgrain_strength, 0.0f, 0.35f);
    bd_filmgrain_scale = ClampPresetFloat(bd_filmgrain_scale, 1.0f, 3.0f);
  }

  if (bd_vhs_enable)
  {
    bd_vhs_strength = ClampPresetFloat(bd_vhs_strength, 0.0f, 0.30f);
    bd_vhs_scanline = ClampPresetFloat(bd_vhs_scanline, 0.0f, 0.28f);
    bd_vhs_jitter = ClampPresetFloat(bd_vhs_jitter, 0.0f, 0.14f);
    bd_vhs_tracking = ClampPresetFloat(bd_vhs_tracking, 0.0f, 0.22f);
    bd_vhs_ghosting = ClampPresetFloat(bd_vhs_ghosting, 0.0f, 0.24f);
    bd_vhs_noise = ClampPresetFloat(bd_vhs_noise, 0.0f, 0.18f);
    bd_vhs_evil = ClampPresetFloat(bd_vhs_evil, 0.0f, 0.12f);
    bd_vhs_panic_enable = false;
  }

  if (bd_vibrance_enable)
    bd_vibrance_strength = ClampPresetFloat(bd_vibrance_strength, 0.0f, 0.80f);

  if (bd_whitebalance_enable)
  {
    bd_whitebalance_temperature = ClampPresetFloat(bd_whitebalance_temperature, -0.60f, 0.60f);
    bd_whitebalance_tint = ClampPresetFloat(bd_whitebalance_tint, -0.60f, 0.60f);
  }

  if (bd_grade_enable)
  {
    bd_grade_lift_r = ClampPresetFloat(bd_grade_lift_r, -0.20f, 0.20f);
    bd_grade_lift_g = ClampPresetFloat(bd_grade_lift_g, -0.20f, 0.20f);
    bd_grade_lift_b = ClampPresetFloat(bd_grade_lift_b, -0.20f, 0.20f);
    bd_grade_gamma_r = ClampPresetFloat(bd_grade_gamma_r, 0.70f, 1.40f);
    bd_grade_gamma_g = ClampPresetFloat(bd_grade_gamma_g, 0.70f, 1.40f);
    bd_grade_gamma_b = ClampPresetFloat(bd_grade_gamma_b, 0.70f, 1.40f);
    bd_grade_gain_r = ClampPresetFloat(bd_grade_gain_r, 0.70f, 1.40f);
    bd_grade_gain_g = ClampPresetFloat(bd_grade_gain_g, 0.70f, 1.40f);
    bd_grade_gain_b = ClampPresetFloat(bd_grade_gain_b, 0.70f, 1.40f);
  }

  if (bd_hueshift_enable)
    bd_hueshift_degrees = ClampPresetFloat(bd_hueshift_degrees, -45.0f, 45.0f);

  if (bd_posterize_enable)
    bd_posterize_levels = ClampPresetFloat(bd_posterize_levels, 4.0f, 16.0f);

  if (bd_edgeglow_enable)
    bd_edgeglow_strength = ClampPresetFloat(bd_edgeglow_strength, 0.0f, 0.80f);

  if (bd_godrays_enable)
    bd_godrays_strength = ClampPresetFloat(bd_godrays_strength, 0.0f, 0.60f);

  if (bd_lensflare_enable)
    bd_lensflare_strength = ClampPresetFloat(bd_lensflare_strength, 0.0f, 0.50f);

  if (bd_clarity_enable)
    bd_clarity_strength = ClampPresetFloat(bd_clarity_strength, 0.0f, 0.60f);

  bd_bloom_radius = ClampPresetFloat(bd_bloom_radius, 0.5f, 2.0f);
  bd_bloom_threshold = ClampPresetFloat(bd_bloom_threshold, 0.50f, 1.20f);
  bd_bloom_knee = ClampPresetFloat(bd_bloom_knee, 0.05f, 1.0f);
  bd_bloom_intensity = ClampPresetFloat(bd_bloom_intensity, 0.25f, 2.0f);

  bd_colorgrade_strength = ClampPresetFloat(bd_colorgrade_strength, 0.0f, 0.55f);
  gl_atmosphere_intensity = ClampPresetFloat(gl_atmosphere_intensity, 0.0f, 0.68f);
  gl_atmosphere_contrast = ClampPresetFloat(gl_atmosphere_contrast, 0.90f, 1.18f);

  if (gl_crt_mode != 0)
  {
    gl_crt_distortion = ClampPresetFloat(gl_crt_distortion, 0.0f, 0.08f);
    gl_crt_zoom = ClampPresetFloat(gl_crt_zoom, 1.0f, 1.04f);
    gl_crt_scanline = ClampPresetFloat(gl_crt_scanline, 0.0f, 0.28f);
    gl_crt_scanline_density = ClampPresetFloat(gl_crt_scanline_density, 0.8f, 1.4f);
    gl_crt_scanline_sharpness = ClampPresetFloat(gl_crt_scanline_sharpness, 0.5f, 1.6f);
    gl_crt_mask_intensity = ClampPresetFloat(gl_crt_mask_intensity, 0.0f, 0.18f);
  }
}

// Resets the filter and performance additions that the original preset
// cases predate, so that switching between any two presets is deterministic.
// Runs under FPresetApplyScope, so these assignments never dirty any preset
// selector; the enable flags go first so the neutral parameter writes cannot
// re-arm a filter's auto-enable coupling.
static void ResetAdvancedPresetFeatures()
{
  gl_ssao = 0;
  gl_fxaa = 0;
  gl_light_shadowmap = false;
  gl_shadowmap_quality = 512;
  gl_shadowmap_filter = 1;
  bd_vibrance_enable = false;
  bd_vibrance_strength = 0.0f;
  bd_whitebalance_enable = false;
  bd_whitebalance_temperature = 0.0f;
  bd_whitebalance_tint = 0.0f;
  bd_grade_enable = false;
  bd_grade_lift_r = 0.0f;
  bd_grade_lift_g = 0.0f;
  bd_grade_lift_b = 0.0f;
  bd_grade_gamma_r = 1.0f;
  bd_grade_gamma_g = 1.0f;
  bd_grade_gamma_b = 1.0f;
  bd_grade_gain_r = 1.0f;
  bd_grade_gain_g = 1.0f;
  bd_grade_gain_b = 1.0f;
  bd_hueshift_enable = false;
  bd_hueshift_degrees = 0.0f;
  bd_posterize_enable = false;
  bd_posterize_levels = 6.0f;
  bd_edgeglow_enable = false;
  bd_edgeglow_strength = 0.0f;
  bd_edgeglow_threshold = 0.1f;
  bd_godrays_enable = false;
  bd_godrays_strength = 0.0f;
  bd_godrays_length = 1.0f;
  bd_godrays_threshold = 0.75f;
  bd_lensflare_enable = false;
  bd_lensflare_strength = 0.0f;
  bd_clarity_enable = false;
  bd_clarity_strength = 0.0f;
  bd_bloom_radius = 1.0f;
  bd_bloom_threshold = 0.92f;
  bd_bloom_knee = 0.55f;
  bd_bloom_intensity = 1.0f;
  bd_dynlight_max_per_surface = 0;
  bd_dynlight_cull_distance = 0.0f;
  bd_shadowmap_max_lights = 1024;
  gl_exposure_scale = 1.3f;
  gl_exposure_min = 0.35f;
  gl_exposure_base = 0.35f;
  gl_exposure_speed = 0.05f;
}

static void ApplyGraphicsPreset(int preset) {
  if (preset <= 0) // Custom
    return;

  ResetAdvancedPresetFeatures();

  switch (preset) {
  case 1: // Vanilla+
    bd_postfx_enable = true;
    bd_postfx_quality = 1;
    bd_bloom_enable = false;
    bd_bloom_strength = 1.4f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = false;
    bd_sharpen_strength = 0.0f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 0;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    return;
  case 2: // Modern Crisp
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.5f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.25f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.35f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 14;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    gl_exposure_scale = 1.20f;
    gl_exposure_min = 0.30f;
    gl_exposure_base = 0.32f;
    gl_exposure_speed = 0.05f;
    return;
  case 3: // CRT Arcade
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.15f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.12f;
    SetCrtPreset(1, 0.04f, 1.0f, 0.20f, 1.10f, 1.05f, 0.10f);
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.04f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.03f;
    bd_filmgrain_scale = 1.2f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.16f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 7;
    bd_colorgrade_strength = 0.18f;
    bd_colorgrade_lut = 5;
    gl_tonemap = 12;
    gl_atmosphere = 6;
    gl_atmosphere_intensity = 0.12f;
    gl_atmosphere_contrast = 1.08f;
    bd_clarity_enable = true;
    bd_clarity_strength = 0.15f;
    return;
  case 4: // VHS Horror
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.95f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.24f;
    SetCrtPreset(0, 0.02f, 1.0f, 0.10f, 1.0f, 1.0f, 0.0f);
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.07f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.16f;
    bd_filmgrain_scale = 2.2f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.06f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.26f;
    bd_vhs_scanline = 0.12f;
    bd_vhs_jitter = 0.08f;
    bd_vhs_tracking = 0.20f;
    bd_vhs_ghosting = 0.16f;
    bd_vhs_noise = 0.18f;
    bd_vhs_evil = 0.04f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 5;
    bd_colorgrade_strength = 0.30f;
    bd_colorgrade_lut = 4;
    gl_tonemap = 11;
    gl_atmosphere = 7;
    gl_atmosphere_intensity = 0.30f;
    gl_atmosphere_contrast = 1.00f;
    gl_exposure_scale = 1.00f;
    gl_exposure_min = 0.28f;
    gl_exposure_base = 0.30f;
    gl_exposure_speed = 0.05f;
    return;
  case 5: // Industrial Hell
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.3f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.28f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.08f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.08f;
    bd_filmgrain_scale = 1.8f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.22f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 8;
    bd_colorgrade_strength = 0.42f;
    bd_colorgrade_lut = 6;
    gl_tonemap = 13;
    gl_atmosphere = 10;
    gl_atmosphere_intensity = 0.42f;
    gl_atmosphere_contrast = 1.12f;
    gl_exposure_scale = 1.15f;
    gl_exposure_min = 0.32f;
    gl_exposure_base = 0.34f;
    gl_exposure_speed = 0.05f;
    return;
  case 6: // Low-End Performance
    bd_postfx_enable = false;
    bd_postfx_quality = 1;
    bd_bloom_enable = false;
    bd_bloom_strength = 1.4f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = false;
    bd_sharpen_strength = 0.0f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 0;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    return;
  case 7: // Silent Hill Fog
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.15f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.38f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.06f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.13f;
    bd_filmgrain_scale = 2.2f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.08f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 5;
    bd_colorgrade_strength = 0.42f;
    bd_colorgrade_lut = 4;
    gl_tonemap = 11;
    gl_atmosphere = 7;
    gl_atmosphere_intensity = 0.62f;
    gl_atmosphere_contrast = 0.96f;
    gl_exposure_scale = 0.95f;
    gl_exposure_min = 0.30f;
    gl_exposure_base = 0.32f;
    gl_exposure_speed = 0.04f;
    return;
  case 8: // Ashen Graveyard
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.25f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.30f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.10f;
    bd_filmgrain_scale = 1.7f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.08f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 4;
    bd_colorgrade_strength = 0.44f;
    bd_colorgrade_lut = 3;
    gl_tonemap = 10;
    gl_atmosphere = 6;
    gl_atmosphere_intensity = 0.40f;
    gl_atmosphere_contrast = 0.96f;
    gl_exposure_scale = 0.95f;
    gl_exposure_min = 0.28f;
    gl_exposure_base = 0.30f;
    gl_exposure_speed = 0.04f;
    return;
  case 9: // Toxic Reactor
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.45f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.22f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.08f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.12f;
    bd_filmgrain_scale = 1.5f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.18f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 6;
    bd_colorgrade_strength = 0.42f;
    bd_colorgrade_lut = 7;
    gl_tonemap = 13;
    gl_atmosphere = 5;
    gl_atmosphere_intensity = 0.38f;
    gl_atmosphere_contrast = 1.10f;
    gl_exposure_scale = 1.15f;
    gl_exposure_min = 0.32f;
    gl_exposure_base = 0.34f;
    gl_exposure_speed = 0.05f;
    return;
  case 10: // Moonlit Noir
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.15f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.36f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.10f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.10f;
    bd_filmgrain_scale = 1.8f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.12f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 2;
    bd_colorgrade_strength = 0.46f;
    bd_colorgrade_lut = 2;
    gl_tonemap = 8;
    gl_atmosphere = 4;
    gl_atmosphere_intensity = 0.42f;
    gl_atmosphere_contrast = 1.12f;
    gl_exposure_scale = 0.90f;
    gl_exposure_min = 0.25f;
    gl_exposure_base = 0.28f;
    gl_exposure_speed = 0.04f;
    return;
  case 11: // Inferno Bloom
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.55f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.38f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.10f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.12f;
    bd_filmgrain_scale = 1.7f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.20f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 8;
    bd_colorgrade_strength = 0.46f;
    bd_colorgrade_lut = 6;
    gl_tonemap = 13;
    gl_atmosphere = 10;
    gl_atmosphere_intensity = 0.45f;
    gl_atmosphere_contrast = 1.12f;
    gl_exposure_scale = 1.20f;
    gl_exposure_min = 0.35f;
    gl_exposure_base = 0.36f;
    gl_exposure_speed = 0.05f;
    return;
  case 12: // Frozen Wasteland
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.45f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.22f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.04f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.05f;
    bd_filmgrain_scale = 1.3f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.30f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 2;
    bd_colorgrade_strength = 0.32f;
    bd_colorgrade_lut = 2;
    gl_tonemap = 12;
    gl_atmosphere = 3;
    gl_atmosphere_intensity = 0.35f;
    gl_atmosphere_contrast = 1.08f;
    gl_exposure_scale = 1.10f;
    gl_exposure_min = 0.40f;
    gl_exposure_base = 0.40f;
    gl_exposure_speed = 0.05f;
    return;
  case 13: // Sodium Streets
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.40f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.25f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.06f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.08f;
    bd_filmgrain_scale = 1.6f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.10f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 8;
    bd_colorgrade_strength = 0.36f;
    bd_colorgrade_lut = 6;
    gl_tonemap = 13;
    gl_atmosphere = 8;
    gl_atmosphere_intensity = 0.40f;
    gl_atmosphere_contrast = 1.05f;
    gl_exposure_scale = 1.05f;
    gl_exposure_min = 0.30f;
    gl_exposure_base = 0.32f;
    gl_exposure_speed = 0.05f;
    return;
  case 14: // Cyberpunk Rain
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.70f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.24f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.12f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.10f;
    bd_filmgrain_scale = 1.6f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.24f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.12f;
    bd_vhs_scanline = 0.10f;
    bd_vhs_jitter = 0.06f;
    bd_vhs_tracking = 0.08f;
    bd_vhs_ghosting = 0.14f;
    bd_vhs_noise = 0.08f;
    bd_vhs_evil = 0.10f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 7;
    bd_colorgrade_strength = 0.50f;
    bd_colorgrade_lut = 8;
    gl_tonemap = 14;
    gl_atmosphere = 9;
    gl_atmosphere_intensity = 0.46f;
    gl_atmosphere_contrast = 1.15f;
    gl_exposure_scale = 1.10f;
    gl_exposure_min = 0.30f;
    gl_exposure_base = 0.32f;
    gl_exposure_speed = 0.06f;
    bd_lensflare_enable = true;
    bd_lensflare_strength = 0.30f;
    bd_bloom_radius = 1.3f;
    return;
  case 15: // Bleach Bunker
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.10f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.12f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.26f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 3;
    bd_colorgrade_strength = 0.26f;
    bd_colorgrade_lut = 1;
    gl_tonemap = 12;
    gl_atmosphere = 1;
    gl_atmosphere_intensity = 0.24f;
    gl_atmosphere_contrast = 1.18f;
    gl_exposure_scale = 1.35f;
    gl_exposure_min = 0.45f;
    gl_exposure_base = 0.45f;
    gl_exposure_speed = 0.06f;
    return;
  case 16: // Analog Horror
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.10f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.34f;
    SetCrtPreset(2, 0.04f, 1.0f, 0.20f, 1.15f, 1.05f, 0.08f);
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.12f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.16f;
    bd_filmgrain_scale = 2.4f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.08f;
    bd_retro_pixel_enable = true;
    bd_retro_pixel_scale = 1.10f;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.28f;
    bd_vhs_scanline = 0.26f;
    bd_vhs_jitter = 0.12f;
    bd_vhs_tracking = 0.18f;
    bd_vhs_ghosting = 0.20f;
    bd_vhs_noise = 0.16f;
    bd_vhs_evil = 0.12f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 5;
    bd_colorgrade_strength = 0.42f;
    bd_colorgrade_lut = 5;
    gl_tonemap = 11;
    gl_atmosphere = 7;
    gl_atmosphere_intensity = 0.46f;
    gl_atmosphere_contrast = 0.98f;
    gl_exposure_scale = 0.90f;
    gl_exposure_min = 0.26f;
    gl_exposure_base = 0.30f;
    gl_exposure_speed = 0.04f;
    return;
  case 17: // Dream Decay
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.25f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.28f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.10f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.08f;
    bd_filmgrain_scale = 1.8f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.08f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.08f;
    bd_vhs_scanline = 0.06f;
    bd_vhs_jitter = 0.05f;
    bd_vhs_tracking = 0.06f;
    bd_vhs_ghosting = 0.10f;
    bd_vhs_noise = 0.06f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 7;
    bd_colorgrade_strength = 0.34f;
    bd_colorgrade_lut = 8;
    gl_tonemap = 8;
    gl_atmosphere = 6;
    gl_atmosphere_intensity = 0.45f;
    gl_atmosphere_contrast = 0.95f;
    gl_exposure_scale = 1.00f;
    gl_exposure_min = 0.30f;
    gl_exposure_base = 0.32f;
    gl_exposure_speed = 0.04f;
    bd_godrays_enable = true;
    bd_godrays_strength = 0.20f;
    bd_godrays_length = 1.2f;
    bd_godrays_threshold = 0.70f;
    bd_bloom_radius = 1.8f;
    return;
  case 18: // Low Light Realism
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.95f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.24f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.04f;
    bd_filmgrain_scale = 1.2f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.16f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 1;
    bd_colorgrade_strength = 0.18f;
    bd_colorgrade_lut = 1;
    gl_tonemap = 3;
    gl_atmosphere = 2;
    gl_atmosphere_intensity = 0.24f;
    gl_atmosphere_contrast = 1.05f;
    gl_exposure_scale = 0.85f;
    gl_exposure_min = 0.20f;
    gl_exposure_base = 0.25f;
    gl_exposure_speed = 0.08f;
    return;
  case 19: // Clean Visibility
    ApplyGraphicsPreset(2);
    bd_bloom_enable = true;
    bd_bloom_strength = 0.65f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    bd_chromatic_enable = false;
    bd_filmgrain_enable = false;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.28f;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    gl_tonemap = 1;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    gl_exposure_scale = 1.25f;
    gl_exposure_min = 0.45f;
    gl_exposure_base = 0.45f;
    gl_exposure_speed = 0.06f;
    bd_clarity_enable = true;
    bd_clarity_strength = 0.30f;
    return;
  case 20: // Warm Cinematic
    ApplyGraphicsPreset(13);
    bd_bloom_strength = 1.25f;
    bd_vignette_strength = 0.28f;
    bd_chromatic_strength = 0.08f;
    bd_filmgrain_strength = 0.08f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.12f;
    bd_colorgrade_strength = 0.42f;
    gl_atmosphere_intensity = 0.42f;
    gl_atmosphere_contrast = 1.08f;
    return;
  case 21: // Cool Clarity
    ApplyGraphicsPreset(12);
    gl_tonemap = 14;
    bd_bloom_strength = 1.10f;
    bd_vignette_strength = 0.20f;
    bd_chromatic_enable = false;
    bd_filmgrain_strength = 0.05f;
    bd_sharpen_strength = 0.28f;
    bd_colorgrade_strength = 0.34f;
    gl_atmosphere_intensity = 0.36f;
    gl_atmosphere_contrast = 1.10f;
    return;
  case 22: // Dense Playable Fog
    ApplyGraphicsPreset(7);
    gl_tonemap = 3;
    bd_bloom_strength = 1.15f;
    bd_vignette_strength = 0.34f;
    bd_chromatic_strength = 0.08f;
    bd_filmgrain_strength = 0.14f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.12f;
    bd_vhs_scanline = 0.10f;
    bd_vhs_jitter = 0.08f;
    bd_vhs_tracking = 0.12f;
    bd_vhs_ghosting = 0.10f;
    bd_vhs_noise = 0.12f;
    bd_vhs_evil = 0.05f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_strength = 0.36f;
    gl_atmosphere_intensity = 0.62f;
    gl_atmosphere_contrast = 0.96f;
    return;
  case 23: // Readable CRT
    ApplyGraphicsPreset(3);
    bd_bloom_strength = 1.05f;
    bd_vignette_strength = 0.08f;
    SetCrtPreset(1, 0.03f, 1.0f, 0.16f, 1.05f, 1.0f, 0.06f);
    bd_chromatic_strength = 0.03f;
    bd_filmgrain_strength = 0.02f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.18f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_strength = 0.14f;
    gl_atmosphere_intensity = 0.08f;
    gl_atmosphere_contrast = 1.08f;
    return;
  case 24: // Action Horror
    ApplyGraphicsPreset(4);
    gl_tonemap = 14;
    bd_bloom_strength = 1.20f;
    bd_vignette_strength = 0.36f;
    bd_chromatic_strength = 0.12f;
    bd_filmgrain_strength = 0.14f;
    bd_filmgrain_scale = 2.2f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.10f;
    bd_retro_pixel_enable = false;
    bd_vhs_strength = 0.24f;
    bd_vhs_scanline = 0.22f;
    bd_vhs_jitter = 0.10f;
    bd_vhs_tracking = 0.16f;
    bd_vhs_ghosting = 0.18f;
    bd_vhs_noise = 0.14f;
    bd_vhs_evil = 0.08f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_strength = 0.48f;
    gl_atmosphere_intensity = 0.52f;
    gl_atmosphere_contrast = 1.00f;
    gl_exposure_scale = 1.05f;
    gl_exposure_min = 0.32f;
    gl_exposure_base = 0.34f;
    gl_exposure_speed = 0.05f;
    return;
  case 25: // VHS Found Footage
    ApplyGraphicsPreset(4);
    bd_bloom_strength = 0.85f;
    bd_vignette_strength = 0.22f;
    SetCrtPreset(0, 0.015f, 1.0f, 0.06f, 1.0f, 1.0f, 0.0f);
    bd_chromatic_strength = 0.05f;
    bd_filmgrain_strength = 0.18f;
    bd_filmgrain_scale = 2.6f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.04f;
    bd_retro_pixel_enable = false;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.28f;
    bd_vhs_scanline = 0.10f;
    bd_vhs_jitter = 0.07f;
    bd_vhs_tracking = 0.18f;
    bd_vhs_ghosting = 0.14f;
    bd_vhs_noise = 0.18f;
    bd_vhs_evil = 0.02f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 5;
    bd_colorgrade_strength = 0.28f;
    bd_colorgrade_lut = 4;
    gl_tonemap = 11;
    gl_atmosphere = 7;
    gl_atmosphere_intensity = 0.28f;
    gl_atmosphere_contrast = 1.00f;
    return;
  case 26: // VHS Tape Rot
    ApplyGraphicsPreset(4);
    bd_bloom_strength = 0.95f;
    bd_vignette_strength = 0.30f;
    SetCrtPreset(0, 0.02f, 1.0f, 0.08f, 1.0f, 1.0f, 0.0f);
    bd_chromatic_strength = 0.11f;
    bd_filmgrain_strength = 0.20f;
    bd_filmgrain_scale = 2.8f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.06f;
    bd_retro_pixel_enable = false;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.30f;
    bd_vhs_scanline = 0.18f;
    bd_vhs_jitter = 0.12f;
    bd_vhs_tracking = 0.22f;
    bd_vhs_ghosting = 0.24f;
    bd_vhs_noise = 0.18f;
    bd_vhs_evil = 0.08f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 8;
    bd_colorgrade_strength = 0.44f;
    bd_colorgrade_lut = 6;
    gl_tonemap = 11;
    gl_atmosphere = 9;
    gl_atmosphere_intensity = 0.40f;
    gl_atmosphere_contrast = 0.96f;
    return;
  case 27: // VHS Night Vision
    ApplyGraphicsPreset(4);
    bd_bloom_strength = 1.00f;
    bd_vignette_strength = 0.26f;
    SetCrtPreset(0, 0.015f, 1.0f, 0.08f, 1.0f, 1.0f, 0.0f);
    bd_chromatic_strength = 0.06f;
    bd_filmgrain_strength = 0.16f;
    bd_filmgrain_scale = 2.4f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.08f;
    bd_retro_pixel_enable = false;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.24f;
    bd_vhs_scanline = 0.14f;
    bd_vhs_jitter = 0.08f;
    bd_vhs_tracking = 0.16f;
    bd_vhs_ghosting = 0.16f;
    bd_vhs_noise = 0.17f;
    bd_vhs_evil = 0.04f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 4;
    bd_colorgrade_strength = 0.42f;
    bd_colorgrade_lut = 4;
    gl_tonemap = 6;
    gl_atmosphere = 4;
    gl_atmosphere_intensity = 0.38f;
    gl_atmosphere_contrast = 1.05f;
    gl_exposure_scale = 1.30f;
    gl_exposure_min = 0.40f;
    gl_exposure_base = 0.42f;
    gl_exposure_speed = 0.06f;
    return;
  case 28: // Possessed VHS
    ApplyGraphicsPreset(4);
    bd_bloom_strength = 1.05f;
    bd_vignette_strength = 0.38f;
    SetCrtPreset(0, 0.025f, 1.0f, 0.12f, 1.0f, 1.0f, 0.0f);
    bd_chromatic_strength = 0.14f;
    bd_filmgrain_strength = 0.20f;
    bd_filmgrain_scale = 2.8f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.06f;
    bd_retro_pixel_enable = false;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.30f;
    bd_vhs_scanline = 0.22f;
    bd_vhs_jitter = 0.14f;
    bd_vhs_tracking = 0.22f;
    bd_vhs_ghosting = 0.22f;
    bd_vhs_noise = 0.18f;
    bd_vhs_evil = 0.12f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 5;
    bd_colorgrade_strength = 0.50f;
    bd_colorgrade_lut = 8;
    gl_tonemap = 7;
    gl_atmosphere = 9;
    gl_atmosphere_intensity = 0.52f;
    gl_atmosphere_contrast = 0.95f;
    return;
  case 29: // Blood Moon Evil
    ApplyGraphicsPreset(11);
    bd_bloom_strength = 1.45f;
    bd_vignette_strength = 0.36f;
    bd_chromatic_strength = 0.10f;
    bd_filmgrain_strength = 0.12f;
    bd_filmgrain_scale = 2.0f;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.16f;
    bd_vhs_scanline = 0.08f;
    bd_vhs_jitter = 0.06f;
    bd_vhs_tracking = 0.12f;
    bd_vhs_ghosting = 0.12f;
    bd_vhs_noise = 0.10f;
    bd_vhs_evil = 0.10f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 8;
    bd_colorgrade_strength = 0.48f;
    bd_colorgrade_lut = 6;
    gl_tonemap = 10;
    gl_atmosphere = 5;
    gl_atmosphere_intensity = 0.54f;
    gl_atmosphere_contrast = 1.08f;
    return;
  case 30: // Void Ritual
    ApplyGraphicsPreset(10);
    bd_bloom_strength = 0.90f;
    bd_vignette_strength = 0.40f;
    bd_chromatic_strength = 0.08f;
    bd_filmgrain_strength = 0.14f;
    bd_filmgrain_scale = 2.2f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.08f;
    bd_vhs_enable = true;
    bd_vhs_strength = 0.18f;
    bd_vhs_scanline = 0.10f;
    bd_vhs_jitter = 0.08f;
    bd_vhs_tracking = 0.14f;
    bd_vhs_ghosting = 0.14f;
    bd_vhs_noise = 0.12f;
    bd_vhs_evil = 0.12f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 5;
    bd_colorgrade_strength = 0.52f;
    bd_colorgrade_lut = 5;
    gl_tonemap = 7;
    gl_atmosphere = 9;
    gl_atmosphere_intensity = 0.58f;
    gl_atmosphere_contrast = 0.96f;
    gl_exposure_scale = 0.85f;
    gl_exposure_min = 0.22f;
    gl_exposure_base = 0.26f;
    gl_exposure_speed = 0.04f;
    return;
  case 31: // Cinematic Ultra
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.3f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.35f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.15f;
    bd_filmgrain_scale = 1.6f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.15f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 14;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    gl_exposure_scale = 1.25f;
    gl_exposure_min = 0.32f;
    gl_exposure_base = 0.34f;
    gl_exposure_speed = 0.06f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.20f;
    bd_whitebalance_enable = true;
    bd_whitebalance_temperature = 0.15f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = true;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.02f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.05f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 0.95f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    gl_ssao = 2;
    gl_fxaa = 0;
    gl_light_shadowmap = true;
    gl_shadowmap_quality = 1024;
    gl_shadowmap_filter = 2;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    bd_godrays_enable = true;
    bd_godrays_strength = 0.25f;
    bd_godrays_length = 1.2f;
    bd_godrays_threshold = 0.70f;
    bd_lensflare_enable = true;
    bd_lensflare_strength = 0.20f;
    bd_bloom_radius = 1.4f;
    return;
  case 32: // Neon Vibrance
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.35f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.15f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.20f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 14;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.70f;
    bd_whitebalance_enable = true;
    bd_whitebalance_temperature = -0.20f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = false;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.0f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.0f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 1.0f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    gl_ssao = 0;
    gl_fxaa = 0;
    gl_light_shadowmap = false;
    gl_shadowmap_quality = 512;
    gl_shadowmap_filter = 1;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    bd_lensflare_enable = true;
    bd_lensflare_strength = 0.20f;
    bd_bloom_radius = 1.2f;
    return;
  case 33: // Retro Poster
    bd_postfx_enable = true;
    bd_postfx_quality = 1;
    bd_bloom_enable = false;
    bd_bloom_strength = 1.4f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.40f;
    bd_retro_pixel_enable = true;
    bd_retro_pixel_scale = 2.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 5;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    bd_vibrance_enable = false;
    bd_vibrance_strength = 0.0f;
    bd_whitebalance_enable = false;
    bd_whitebalance_temperature = 0.0f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = true;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.0f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.08f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 0.92f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = true;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    gl_ssao = 0;
    gl_fxaa = 0;
    gl_light_shadowmap = false;
    gl_shadowmap_quality = 512;
    gl_shadowmap_filter = 1;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    return;
  case 34: // Cel Comic
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = false;
    bd_bloom_strength = 1.4f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.20f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 1;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    bd_vibrance_enable = false;
    bd_vibrance_strength = 0.0f;
    bd_whitebalance_enable = false;
    bd_whitebalance_temperature = 0.0f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = false;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.0f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.0f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 1.0f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = true;
    bd_posterize_levels = 8.0f;
    bd_edgeglow_enable = true;
    bd_edgeglow_strength = 0.70f;
    bd_edgeglow_threshold = 0.08f;
    gl_ssao = 0;
    gl_fxaa = 0;
    gl_light_shadowmap = false;
    gl_shadowmap_quality = 512;
    gl_shadowmap_filter = 1;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    return;
  case 35: // Sepia Archive
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.80f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.50f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.30f;
    bd_filmgrain_scale = 2.4f;
    bd_sharpen_enable = false;
    bd_sharpen_strength = 0.0f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 3;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    bd_vibrance_enable = false;
    bd_vibrance_strength = 0.0f;
    bd_whitebalance_enable = true;
    bd_whitebalance_temperature = 0.50f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = false;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.0f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.0f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 1.0f;
    bd_hueshift_enable = true;
    bd_hueshift_degrees = -10.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    gl_ssao = 0;
    gl_fxaa = 0;
    gl_light_shadowmap = false;
    gl_shadowmap_quality = 512;
    gl_shadowmap_filter = 1;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    return;
  case 36: // Ultra Lightweight
    bd_postfx_enable = true;
    bd_postfx_quality = 1;
    bd_bloom_enable = false;
    bd_bloom_strength = 1.4f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = false;
    bd_sharpen_strength = 0.0f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 0;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    bd_vibrance_enable = false;
    bd_vibrance_strength = 0.0f;
    bd_whitebalance_enable = false;
    bd_whitebalance_temperature = 0.0f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = false;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.0f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.0f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 1.0f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    gl_ssao = 0;
    gl_fxaa = 0;
    gl_light_shadowmap = false;
    gl_shadowmap_quality = 512;
    gl_shadowmap_filter = 1;
    bd_dynlight_max_per_surface = 8;
    bd_dynlight_cull_distance = 1500.0f;
    bd_shadowmap_max_lights = 256;
    return;
  case 37: // Balanced Performance
    bd_postfx_enable = true;
    bd_postfx_quality = 2;
    bd_bloom_enable = false;
    bd_bloom_strength = 1.4f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.30f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 3;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.25f;
    bd_whitebalance_enable = false;
    bd_whitebalance_temperature = 0.0f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = false;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.0f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.0f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 1.0f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    gl_ssao = 0;
    gl_fxaa = 3;
    gl_light_shadowmap = false;
    gl_shadowmap_quality = 512;
    gl_shadowmap_filter = 1;
    bd_dynlight_max_per_surface = 16;
    bd_dynlight_cull_distance = 2500.0f;
    bd_shadowmap_max_lights = 512;
    return;
  case 38: // Competitive Clarity
    ApplyGraphicsPreset(2);
    bd_bloom_enable = false;
    bd_bloom_strength = 1.4f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.35f;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 1;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    gl_ssao = 0;
    gl_fxaa = 3;
    gl_light_shadowmap = false;
    gl_shadowmap_quality = 512;
    gl_shadowmap_filter = 1;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 2000.0f;
    bd_shadowmap_max_lights = 256;
    bd_clarity_enable = true;
    bd_clarity_strength = 0.30f;
    return;
  case 39: // HDR Showcase
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.6f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.20f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.15f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 14;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    gl_exposure_scale = 1.35f;
    gl_exposure_min = 0.30f;
    gl_exposure_base = 0.35f;
    gl_exposure_speed = 0.08f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.30f;
    bd_whitebalance_enable = false;
    bd_whitebalance_temperature = 0.0f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = true;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.015f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.04f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 0.96f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    gl_ssao = 2;
    gl_fxaa = 0;
    gl_light_shadowmap = true;
    gl_shadowmap_quality = 1024;
    gl_shadowmap_filter = 2;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    bd_bloom_radius = 1.4f;
    return;
  case 40: // Maxed Out
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.5f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.25f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.05f;
    bd_filmgrain_scale = 1.2f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.25f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 14;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    gl_exposure_scale = 1.25f;
    gl_exposure_min = 0.32f;
    gl_exposure_base = 0.34f;
    gl_exposure_speed = 0.06f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.20f;
    bd_whitebalance_enable = false;
    bd_whitebalance_temperature = 0.0f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = false;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.0f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.0f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 1.0f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    gl_ssao = 3;
    gl_fxaa = 3;
    gl_light_shadowmap = true;
    gl_shadowmap_quality = 1024;
    gl_shadowmap_filter = 3;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    bd_godrays_enable = true;
    bd_godrays_strength = 0.30f;
    bd_godrays_length = 1.2f;
    bd_godrays_threshold = 0.70f;
    bd_lensflare_enable = true;
    bd_lensflare_strength = 0.15f;
    bd_clarity_enable = true;
    bd_clarity_strength = 0.20f;
    bd_bloom_radius = 1.5f;
    return;
  case 41: // Divine Radiance
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.2f;
    bd_bloom_radius = 1.6f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.20f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.15f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 13;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    gl_exposure_scale = 1.30f;
    gl_exposure_min = 0.38f;
    gl_exposure_base = 0.40f;
    gl_exposure_speed = 0.06f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.25f;
    bd_whitebalance_enable = true;
    bd_whitebalance_temperature = 0.20f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = false;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.0f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.0f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 1.0f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    bd_godrays_enable = true;
    bd_godrays_strength = 0.45f;
    bd_godrays_length = 1.3f;
    bd_godrays_threshold = 0.65f;
    bd_lensflare_enable = false;
    bd_lensflare_strength = 0.0f;
    bd_clarity_enable = false;
    bd_clarity_strength = 0.0f;
    gl_ssao = 2;
    gl_fxaa = 0;
    gl_light_shadowmap = true;
    gl_shadowmap_quality = 1024;
    gl_shadowmap_filter = 2;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    return;
  case 42: // Analog Cinema
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.0f;
    bd_bloom_radius = 1.2f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.40f;
    gl_crt_mode = 0;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.10f;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.25f;
    bd_filmgrain_scale = 2.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.10f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 13;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.20f;
    bd_whitebalance_enable = true;
    bd_whitebalance_temperature = 0.10f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = true;
    bd_grade_lift_r = -0.02f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.03f;
    bd_grade_gamma_r = 1.05f;
    bd_grade_gamma_g = 1.05f;
    bd_grade_gamma_b = 1.05f;
    bd_grade_gain_r = 1.06f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 0.94f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    bd_godrays_enable = false;
    bd_godrays_strength = 0.0f;
    bd_godrays_length = 1.0f;
    bd_godrays_threshold = 0.75f;
    bd_lensflare_enable = true;
    bd_lensflare_strength = 0.35f;
    bd_clarity_enable = false;
    bd_clarity_strength = 0.0f;
    gl_ssao = 2;
    gl_fxaa = 0;
    gl_light_shadowmap = true;
    gl_shadowmap_quality = 1024;
    gl_shadowmap_filter = 2;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    return;
  case 43: // Clarity Max
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = false;
    bd_bloom_strength = 1.4f;
    bd_bloom_radius = 1.0f;
    bd_vignette_enable = false;
    bd_vignette_strength = 0.0f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.40f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 1;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    gl_exposure_scale = 1.20f;
    gl_exposure_min = 0.40f;
    gl_exposure_base = 0.40f;
    gl_exposure_speed = 0.06f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.30f;
    bd_whitebalance_enable = false;
    bd_whitebalance_temperature = 0.0f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = false;
    bd_grade_lift_r = 0.0f;
    bd_grade_lift_g = 0.0f;
    bd_grade_lift_b = 0.0f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 1.0f;
    bd_grade_gain_g = 1.0f;
    bd_grade_gain_b = 1.0f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    bd_godrays_enable = false;
    bd_godrays_strength = 0.0f;
    bd_godrays_length = 1.0f;
    bd_godrays_threshold = 0.75f;
    bd_lensflare_enable = false;
    bd_lensflare_strength = 0.0f;
    bd_clarity_enable = true;
    bd_clarity_strength = 0.50f;
    gl_ssao = 0;
    gl_fxaa = 3;
    gl_light_shadowmap = false;
    gl_shadowmap_quality = 512;
    gl_shadowmap_filter = 1;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    return;
  case 44: // Dreamlike
    bd_postfx_enable = true;
    bd_postfx_quality = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.8f;
    bd_bloom_radius = 2.0f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.15f;
    gl_crt_mode = 0;
    bd_chromatic_enable = false;
    bd_chromatic_strength = 0.0f;
    bd_filmgrain_enable = false;
    bd_filmgrain_strength = 0.0f;
    bd_filmgrain_scale = 1.0f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.10f;
    bd_retro_pixel_enable = false;
    bd_retro_pixel_scale = 1.0f;
    bd_vhs_enable = false;
    bd_vhs_strength = 0.0f;
    bd_vhs_scanline = 0.0f;
    bd_vhs_jitter = 0.0f;
    bd_vhs_tracking = 0.0f;
    bd_vhs_ghosting = 0.0f;
    bd_vhs_noise = 0.0f;
    bd_vhs_evil = 0.0f;
    bd_vhs_panic_enable = false;
    bd_colorgrade_mode = 0;
    bd_colorgrade_strength = 0.0f;
    bd_colorgrade_lut = 0;
    gl_tonemap = 8;
    gl_atmosphere = 0;
    gl_atmosphere_intensity = 1.0f;
    gl_atmosphere_contrast = 1.0f;
    gl_exposure_scale = 1.10f;
    gl_exposure_min = 0.32f;
    gl_exposure_base = 0.34f;
    gl_exposure_speed = 0.04f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.25f;
    bd_whitebalance_enable = true;
    bd_whitebalance_temperature = 0.10f;
    bd_whitebalance_tint = 0.0f;
    bd_grade_enable = true;
    bd_grade_lift_r = 0.02f;
    bd_grade_lift_g = 0.02f;
    bd_grade_lift_b = 0.03f;
    bd_grade_gamma_r = 1.0f;
    bd_grade_gamma_g = 1.0f;
    bd_grade_gamma_b = 1.0f;
    bd_grade_gain_r = 0.98f;
    bd_grade_gain_g = 0.98f;
    bd_grade_gain_b = 0.98f;
    bd_hueshift_enable = false;
    bd_hueshift_degrees = 0.0f;
    bd_posterize_enable = false;
    bd_posterize_levels = 6.0f;
    bd_edgeglow_enable = false;
    bd_edgeglow_strength = 0.0f;
    bd_edgeglow_threshold = 0.1f;
    bd_godrays_enable = true;
    bd_godrays_strength = 0.30f;
    bd_godrays_length = 1.2f;
    bd_godrays_threshold = 0.70f;
    bd_lensflare_enable = false;
    bd_lensflare_strength = 0.0f;
    bd_clarity_enable = false;
    bd_clarity_strength = 0.0f;
    gl_ssao = 2;
    gl_fxaa = 0;
    gl_light_shadowmap = true;
    gl_shadowmap_quality = 1024;
    gl_shadowmap_filter = 2;
    bd_dynlight_max_per_surface = 0;
    bd_dynlight_cull_distance = 0.0f;
    bd_shadowmap_max_lights = 1024;
    return;
  case 45: // Soft Bloom
    ApplyGraphicsPreset(2);
    gl_tonemap = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.05f;
    bd_bloom_radius = 1.8f;
    bd_bloom_threshold = 0.78f;
    bd_bloom_knee = 0.70f;
    bd_bloom_intensity = 0.85f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.12f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.12f;
    return;
  case 46: // Neon Bloom
    ApplyGraphicsPreset(32);
    bd_bloom_enable = true;
    bd_bloom_strength = 1.75f;
    bd_bloom_radius = 2.0f;
    bd_bloom_threshold = 0.58f;
    bd_bloom_knee = 0.45f;
    bd_bloom_intensity = 1.30f;
    bd_chromatic_enable = true;
    bd_chromatic_strength = 0.08f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.42f;
    return;
  case 47: // Subtle Film
    ApplyGraphicsPreset(2);
    gl_tonemap = 13;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.08f;
    bd_filmgrain_scale = 1.5f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.12f;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.75f;
    bd_bloom_radius = 1.2f;
    bd_bloom_threshold = 0.92f;
    return;
  case 48: // Muted Pastels
    ApplyGraphicsPreset(35);
    gl_tonemap = 3;
    bd_colorgrade_mode = 3;
    bd_colorgrade_strength = 0.18f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.14f;
    bd_whitebalance_enable = true;
    bd_whitebalance_temperature = 0.08f;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.65f;
    bd_bloom_threshold = 0.88f;
    return;
  case 49: // Dark Ambient
    ApplyGraphicsPreset(18);
    gl_tonemap = 10;
    gl_exposure_scale = 0.80f;
    gl_exposure_min = 0.20f;
    gl_exposure_base = 0.22f;
    gl_exposure_speed = 0.06f;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.70f;
    bd_bloom_radius = 1.4f;
    bd_bloom_threshold = 0.86f;
    bd_bloom_intensity = 0.75f;
    bd_vignette_enable = true;
    bd_vignette_strength = 0.22f;
    return;
  case 50: // Bright Ambient
    ApplyGraphicsPreset(19);
    bd_bloom_enable = true;
    bd_bloom_strength = 0.65f;
    bd_bloom_radius = 1.3f;
    bd_bloom_threshold = 0.95f;
    return;
  case 51: // Sharp Retro
    ApplyGraphicsPreset(1);
    gl_tonemap = 5;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.48f;
    bd_retro_pixel_enable = true;
    bd_retro_pixel_scale = 2.0f;
    gl_fxaa = 3;
    bd_bloom_enable = false;
    return;
  case 52: // Soft Retro
    ApplyGraphicsPreset(3);
    gl_tonemap = 0;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.60f;
    bd_bloom_radius = 1.8f;
    bd_bloom_threshold = 0.82f;
    bd_bloom_knee = 0.75f;
    gl_crt_mode = 2;
    gl_crt_scanline = 0.12f;
    gl_crt_mask_intensity = 0.08f;
    return;
  case 53: // Noir Punch
    ApplyGraphicsPreset(10);
    gl_tonemap = 7;
    bd_colorgrade_mode = 5;
    bd_colorgrade_strength = 0.38f;
    bd_grade_enable = true;
    bd_grade_lift_r = -0.04f;
    bd_grade_lift_g = -0.04f;
    bd_grade_lift_b = -0.03f;
    bd_grade_gain_r = 1.08f;
    bd_grade_gain_g = 1.06f;
    bd_grade_gain_b = 1.02f;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.90f;
    bd_bloom_threshold = 0.80f;
    return;
  case 54: // Cel Shadows
    ApplyGraphicsPreset(34);
    gl_tonemap = 1;
    bd_posterize_enable = true;
    bd_posterize_levels = 5.0f;
    bd_edgeglow_enable = true;
    bd_edgeglow_strength = 0.22f;
    bd_edgeglow_threshold = 0.12f;
    bd_bloom_enable = false;
    return;
  case 55: // Watercolor Dream
    ApplyGraphicsPreset(44);
    gl_tonemap = 8;
    bd_bloom_strength = 1.35f;
    bd_bloom_radius = 2.0f;
    bd_bloom_threshold = 0.74f;
    bd_bloom_knee = 0.85f;
    bd_colorgrade_mode = 6;
    bd_colorgrade_strength = 0.30f;
    bd_sharpen_enable = false;
    bd_clarity_enable = false;
    return;
  case 56: // Overexposed
    ApplyGraphicsPreset(41);
    gl_tonemap = 3;
    gl_exposure_scale = 1.80f;
    gl_exposure_min = 0.55f;
    gl_exposure_base = 0.60f;
    gl_exposure_speed = 0.10f;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.60f;
    bd_bloom_radius = 1.7f;
    bd_bloom_threshold = 0.60f;
    bd_bloom_knee = 0.60f;
    bd_bloom_intensity = 1.20f;
    bd_godrays_strength = 0.38f;
    return;
  case 57: // Clean Lens
    ApplyGraphicsPreset(38);
    gl_tonemap = 14;
    bd_bloom_enable = false;
    bd_clarity_enable = true;
    bd_clarity_strength = 0.45f;
    bd_sharpen_enable = true;
    bd_sharpen_strength = 0.42f;
    gl_fxaa = 3;
    return;
  case 58: // Low Glow
    ApplyGraphicsPreset(6);
    gl_tonemap = 3;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.70f;
    bd_bloom_radius = 1.2f;
    bd_bloom_threshold = 1.00f;
    bd_bloom_knee = 0.35f;
    bd_bloom_intensity = 0.60f;
    bd_vignette_enable = false;
    return;
  case 59: // Spectral
    ApplyGraphicsPreset(30);
    gl_tonemap = 8;
    gl_exposure_scale = 0.95f;
    gl_exposure_min = 0.28f;
    gl_exposure_base = 0.30f;
    gl_exposure_speed = 0.04f;
    bd_hueshift_enable = true;
    bd_hueshift_degrees = 18.0f;
    bd_edgeglow_enable = true;
    bd_edgeglow_strength = 0.34f;
    bd_edgeglow_threshold = 0.08f;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.10f;
    bd_bloom_radius = 1.6f;
    bd_bloom_threshold = 0.72f;
    return;
  case 60: // Golden Film
    ApplyGraphicsPreset(42);
    bd_colorgrade_mode = 1;
    bd_colorgrade_strength = 0.32f;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.10f;
    bd_bloom_radius = 1.5f;
    bd_bloom_threshold = 0.78f;
    bd_vignette_strength = 0.25f;
    bd_lensflare_enable = true;
    bd_lensflare_strength = 0.22f;
    return;
  case 61: // Cold Facility
    ApplyGraphicsPreset(12);
    bd_whitebalance_enable = true;
    bd_whitebalance_temperature = -0.30f;
    bd_whitebalance_tint = 0.05f;
    bd_colorgrade_lut = 5;
    bd_bloom_enable = true;
    bd_bloom_strength = 0.80f;
    bd_bloom_threshold = 0.86f;
    bd_clarity_enable = true;
    bd_clarity_strength = 0.28f;
    return;
  case 62: // Violet Dusk
    ApplyGraphicsPreset(14);
    gl_tonemap = 8;
    bd_hueshift_enable = true;
    bd_hueshift_degrees = -12.0f;
    bd_colorgrade_mode = 7;
    bd_colorgrade_strength = 0.28f;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.20f;
    bd_bloom_radius = 1.6f;
    bd_bloom_threshold = 0.76f;
    return;
  case 63: // Soft Focus
    ApplyGraphicsPreset(44);
    gl_tonemap = 3;
    bd_bloom_strength = 1.30f;
    bd_bloom_radius = 2.0f;
    bd_bloom_threshold = 0.75f;
    bd_bloom_knee = 0.90f;
    bd_bloom_intensity = 1.05f;
    bd_sharpen_enable = false;
    bd_clarity_enable = false;
    bd_filmgrain_enable = true;
    bd_filmgrain_strength = 0.04f;
    return;
  case 64: // Arcade Neon
    ApplyGraphicsPreset(32);
    bd_posterize_enable = true;
    bd_posterize_levels = 5.0f;
    bd_vibrance_enable = true;
    bd_vibrance_strength = 0.50f;
    bd_bloom_enable = true;
    bd_bloom_strength = 1.30f;
    bd_bloom_radius = 1.5f;
    bd_bloom_threshold = 0.70f;
    bd_bloom_intensity = 1.10f;
    return;
  default:
    return;
  }
}

static void SetGraphicsPreset(FIntCVar &self) {
  if (self < 0)
    self = 0;
  if (self > MaxGraphicsPreset)
    self = MaxGraphicsPreset;

  FPresetApplyScope applyScope;
  ApplyGraphicsPreset(self);
  KeepPresetPlayable(self);

  if (self <= 0 || C_InInitialCallbackReplay())
    return;

  // Auto-pair the matching named lighting/fog presets by moving the selectors
  // themselves, so every menu and preset browser reflects the actual look.
  // A selector the user set explicitly (anything other than Custom or the
  // last auto-paired value) is left untouched: explicit choice always wins.
  const FPresetPairing &pairing = GGraphicsPresetPairing[self];

  if (pairing.lighting > 0 &&
      (bd_lighting_preset == 0 || bd_lighting_preset == bd_autopaired_lighting)) {
    bd_autopaired_lighting = pairing.lighting;
    if (bd_lighting_preset != pairing.lighting)
      bd_lighting_preset = pairing.lighting; // fires ApplyLightingPreset
  }

  if (pairing.fog > 0 &&
      (bd_fog_preset == 0 || bd_fog_preset == bd_autopaired_fog)) {
    bd_autopaired_fog = pairing.fog;
    if (bd_fog_preset != pairing.fog)
      bd_fog_preset = pairing.fog; // fires ApplyFogPreset
  }
}

//==========================================================================
//
// CVARs
//
//==========================================================================
CUSTOM_CVAR(Bool, gl_bloom, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (bd_bloom_enable != self)
    bd_bloom_enable = self;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, gl_bloom_amount, 1.4f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.1f)
    self = 0.1f;
  if (self > 4.0f)
    self = 4.0f;

  if (bd_bloom_strength != self)
    bd_bloom_strength = self;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, gl_exposure_scale, 1.3f, CVAR_ARCHIVE) {
  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, gl_exposure_min, 0.35f, CVAR_ARCHIVE) {
  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, gl_exposure_base, 0.35f, CVAR_ARCHIVE) {
  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, gl_exposure_speed, 0.05f, CVAR_ARCHIVE) {
  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Int, gl_tonemap, 0, CVAR_ARCHIVE) {
  if (self < 0 || self > MaxSelectableTonemap)
    self = 0;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Int, gl_atmosphere, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 10)
    self = 10;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, gl_atmosphere_intensity, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 2.0f)
    self = 2.0f;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, gl_atmosphere_contrast, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 2.0f)
    self = 2.0f;

  OnPresetFeatureChanged(self);
}

CVAR(Bool, gl_lens, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, gl_lens_k, -0.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_lens_kcube, 0.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_lens_chromatic, 1.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, gl_fxaa, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0 || self >= IFXAAShader::Count) {
    self = 0;
  }
}

CUSTOM_CVAR(Int, gl_ssao, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0 || self > 3)
    self = 0;
}

CUSTOM_CVAR(Int, gl_ssao_portals, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
}

CVAR(Float, gl_ssao_strength, 0.7f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_ssao_debug, 0, 0)
CVAR(Float, gl_ssao_bias, 0.2f, 0)
CVAR(Float, gl_ssao_radius, 80.0f, 0)
CUSTOM_CVAR(Float, gl_ssao_blur, 16.0f, 0) {
  if (self < 0.1f)
    self = 0.1f;
}

CUSTOM_CVAR(Float, gl_ssao_exponent, 1.8f, 0) {
  if (self < 0.1f)
    self = 0.1f;
}

CUSTOM_CVAR(Float, gl_paltonemap_powtable, 2.0f,
            CVAR_ARCHIVE | CVAR_NOINITCALL) {
  screen->UpdatePalette();
}

CUSTOM_CVAR(Bool, gl_paltonemap_reverselookup, true,
            CVAR_ARCHIVE | CVAR_NOINITCALL) {
  screen->UpdatePalette();
}

CVAR(Float, gl_menu_blur, -1.0f, CVAR_ARCHIVE)

// Archived auto-pair trackers; see the comment at the top of this file.
CVAR(Int, bd_autopaired_lighting, 0,
     CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
CVAR(Int, bd_autopaired_fog, 0,
     CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)

CUSTOM_CVAR(Int, bd_graphics_preset, 0,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL) {
  // The auto-pair state itself survives config load (it is archived), so no
  // lazy re-inference is needed here; SetGraphicsPreset consults the loaded
  // tracker values directly.
  SetGraphicsPreset(self);
}

CUSTOM_CVAR(Bool, bd_bloom_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (gl_bloom != self)
    gl_bloom = self;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_bloom_strength, 1.4f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.1f)
    self = 0.1f;
  if (self > 4.0f)
    self = 4.0f;

  if (gl_bloom_amount != self)
    gl_bloom_amount = self;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_bloom_radius, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.5f)
    self = 0.5f;
  if (self > 3.0f)
    self = 3.0f;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_bloom_threshold, 0.92f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 4.0f)
    self = 4.0f;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_bloom_knee, 0.55f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.01f)
    self = 0.01f;
  if (self > 2.0f)
    self = 2.0f;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_bloom_intensity, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 4.0f)
    self = 4.0f;

  OnPresetFeatureChanged(self);
}

CVAR(Bool, bd_preset_locked, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Bool, bd_postfx_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Int, bd_postfx_quality, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 3)
    self = 3;

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Bool, bd_vignette_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
  {
    EnsurePostFxActive();
    if (bd_vignette_strength <= 0.0f)
      bd_vignette_strength = 0.45f;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_vignette_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
  {
    EnsurePostFxActive();
    if (!bd_vignette_enable)
      bd_vignette_enable = true;
  }

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_vibrance_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
  {
    EnsurePostFxActive();
    if (bd_vibrance_strength <= 0.0f)
      bd_vibrance_strength = 0.5f;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_vibrance_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
  {
    EnsurePostFxActive();
    if (!bd_vibrance_enable)
      bd_vibrance_enable = true;
  }

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_whitebalance_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
  {
    EnsurePostFxActive();
    if (bd_whitebalance_temperature == 0.0f && bd_whitebalance_tint == 0.0f)
      bd_whitebalance_temperature = 0.1f;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_whitebalance_temperature, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < -1.0f)
    self = -1.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self != 0.0f)
  {
    EnsurePostFxActive();
    if (!bd_whitebalance_enable)
      bd_whitebalance_enable = true;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_whitebalance_tint, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < -1.0f)
    self = -1.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self != 0.0f)
  {
    EnsurePostFxActive();
    if (!bd_whitebalance_enable)
      bd_whitebalance_enable = true;
  }

  OnPresetFeatureChanged(self);
}

static bool GradeComponentsNeutral()
{
  return bd_grade_lift_r == 0.0f && bd_grade_lift_g == 0.0f &&
         bd_grade_lift_b == 0.0f && bd_grade_gamma_r == 1.0f &&
         bd_grade_gamma_g == 1.0f && bd_grade_gamma_b == 1.0f &&
         bd_grade_gain_r == 1.0f && bd_grade_gain_g == 1.0f &&
         bd_grade_gain_b == 1.0f;
}

static void OnGradeComponentChanged(FFloatCVar &self, float neutral)
{
  if (self != neutral)
  {
    EnsurePostFxActive();
    if (!bd_grade_enable)
      bd_grade_enable = true;
  }

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_grade_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
  {
    EnsurePostFxActive();
    if (GradeComponentsNeutral())
    {
      bd_grade_gain_r = 1.1f;
      bd_grade_gain_g = 1.1f;
      bd_grade_gain_b = 1.1f;
    }
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_grade_lift_r, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < -0.5f)
    self = -0.5f;
  if (self > 0.5f)
    self = 0.5f;

  OnGradeComponentChanged(self, 0.0f);
}
CUSTOM_CVAR(Float, bd_grade_lift_g, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < -0.5f)
    self = -0.5f;
  if (self > 0.5f)
    self = 0.5f;

  OnGradeComponentChanged(self, 0.0f);
}
CUSTOM_CVAR(Float, bd_grade_lift_b, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < -0.5f)
    self = -0.5f;
  if (self > 0.5f)
    self = 0.5f;

  OnGradeComponentChanged(self, 0.0f);
}
CUSTOM_CVAR(Float, bd_grade_gamma_r, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.5f)
    self = 0.5f;
  if (self > 2.0f)
    self = 2.0f;

  OnGradeComponentChanged(self, 1.0f);
}
CUSTOM_CVAR(Float, bd_grade_gamma_g, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.5f)
    self = 0.5f;
  if (self > 2.0f)
    self = 2.0f;

  OnGradeComponentChanged(self, 1.0f);
}
CUSTOM_CVAR(Float, bd_grade_gamma_b, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.5f)
    self = 0.5f;
  if (self > 2.0f)
    self = 2.0f;

  OnGradeComponentChanged(self, 1.0f);
}
CUSTOM_CVAR(Float, bd_grade_gain_r, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.5f)
    self = 0.5f;
  if (self > 2.0f)
    self = 2.0f;

  OnGradeComponentChanged(self, 1.0f);
}
CUSTOM_CVAR(Float, bd_grade_gain_g, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.5f)
    self = 0.5f;
  if (self > 2.0f)
    self = 2.0f;

  OnGradeComponentChanged(self, 1.0f);
}
CUSTOM_CVAR(Float, bd_grade_gain_b, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.5f)
    self = 0.5f;
  if (self > 2.0f)
    self = 2.0f;

  OnGradeComponentChanged(self, 1.0f);
}

CUSTOM_CVAR(Bool, bd_hueshift_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
  {
    EnsurePostFxActive();
    if (bd_hueshift_degrees == 0.0f)
      bd_hueshift_degrees = 15.0f;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_hueshift_degrees, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < -180.0f)
    self = -180.0f;
  if (self > 180.0f)
    self = 180.0f;

  if (self != 0.0f)
  {
    EnsurePostFxActive();
    if (!bd_hueshift_enable)
      bd_hueshift_enable = true;
  }

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_posterize_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_posterize_levels, 6.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 2.0f)
    self = 2.0f;
  if (self > 16.0f)
    self = 16.0f;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_edgeglow_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
  {
    EnsurePostFxActive();
    if (bd_edgeglow_strength <= 0.0f)
      bd_edgeglow_strength = 0.5f;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_edgeglow_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
  {
    EnsurePostFxActive();
    if (!bd_edgeglow_enable)
      bd_edgeglow_enable = true;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_edgeglow_threshold, 0.1f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_godrays_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
  {
    EnsurePostFxActive();
    if (bd_godrays_strength <= 0.0f)
      bd_godrays_strength = 0.3f;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_godrays_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
  {
    EnsurePostFxActive();
    if (!bd_godrays_enable)
      bd_godrays_enable = true;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_godrays_length, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.25f)
    self = 0.25f;
  if (self > 2.0f)
    self = 2.0f;

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_godrays_threshold, 0.75f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_lensflare_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
  {
    EnsurePostFxActive();
    if (bd_lensflare_strength <= 0.0f)
      bd_lensflare_strength = 0.5f;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_lensflare_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
  {
    EnsurePostFxActive();
    if (!bd_lensflare_enable)
      bd_lensflare_enable = true;
  }

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_clarity_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
  {
    EnsurePostFxActive();
    if (bd_clarity_strength <= 0.0f)
      bd_clarity_strength = 0.5f;
  }

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_clarity_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
  {
    EnsurePostFxActive();
    if (!bd_clarity_enable)
      bd_clarity_enable = true;
  }

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_chromatic_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_chromatic_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_filmgrain_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_filmgrain_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_filmgrain_scale, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 1.0f)
    self = 1.0f;
  if (self > 8.0f)
    self = 8.0f;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_sharpen_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_sharpen_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_retro_pixel_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_retro_pixel_scale, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 1.0f)
    self = 1.0f;
  if (self > 16.0f)
    self = 16.0f;

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_vhs_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_vhs_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_vhs_scanline, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_vhs_jitter, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_vhs_tracking, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_vhs_ghosting, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_vhs_noise, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_vhs_evil, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
CUSTOM_CVAR(Bool, bd_vhs_panic_enable, false,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Int, bd_colorgrade_mode, 0,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 8)
    self = 8;

  if (self > 0)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_colorgrade_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  if (self > 0.0f)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Int, bd_colorgrade_lut, 0,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 8)
    self = 8;

  if (self > 0)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}

CUSTOM_CVAR(Int, bd_dynlight_falloff_mode, 0,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 2)
    self = 2;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_dynlight_falloff_exponent, 2.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.5f)
    self = 0.5f;
  if (self > 8.0f)
    self = 8.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Int, bd_lighting_preset, 0,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL) {
  if (self < 0)
    self = 0;
  if (self > MaxLightingPreset)
    self = MaxLightingPreset;

  // A user-driven selection (menu, console, config) is an explicit choice:
  // drop the auto-pair tracking so graphics presets stop overriding it.
  if (!IsApplyingPreset())
    bd_autopaired_lighting = 0;

  FPresetApplyScope applyScope;
  ApplyLightingPreset(self);
}

CUSTOM_CVAR(Float, bd_dynlight_intensity, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 3.0f)
    self = 3.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_dynlight_saturation, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 2.0f)
    self = 2.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_dynlight_range_scale, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.10f)
    self = 0.10f;
  if (self > 4.0f)
    self = 4.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_dynlight_falloff_softness, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_dynlight_wrap, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 0.95f)
    self = 0.95f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_dynlight_indirect, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_dynlight_shadow_strength, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnLightingFeatureChanged(self);
}

// Performance knobs. These only clamp: they must not trigger the
// OnLightingFeatureChanged / OnPresetFeatureChanged style callbacks.
CUSTOM_CVAR(Int, bd_dynlight_max_per_surface, 0,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 256)
    self = 256;
}

CUSTOM_CVAR(Float, bd_dynlight_cull_distance, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 65536.0f)
    self = 65536.0f;
}

CUSTOM_CVAR(Int, bd_shadowmap_max_lights, 1024,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 64)
    self = 64;
  if (self > 1024)
    self = 1024;
}

CUSTOM_CVAR(Float, bd_light_temperature, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < -1.0f)
    self = -1.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_light_ambient_floor, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 0.5f)
    self = 0.5f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_light_specular_scale, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 3.0f)
    self = 3.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_emissive_boost, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 2.0f)
    self = 2.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_gi_ambient_enable, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  OnLightingFeatureChanged(self);
}
CUSTOM_CVAR(Float, bd_gi_ambient_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_light_contrast, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 2.0f)
    self = 2.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_specular_power_scale, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.25f)
    self = 0.25f;
  if (self > 4.0f)
    self = 4.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_rimlight_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_rimlight_power, 3.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.5f)
    self = 0.5f;
  if (self > 8.0f)
    self = 8.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_ambient_gradient_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Color, bd_ambient_gradient_color, 0x8899bb,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_dynlight_flicker, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_aerial_strength, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_aerial_distance, 2048.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 256.0f)
    self = 256.0f;
  if (self > 16384.0f)
    self = 16384.0f;

  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Color, bd_specular_tint, 0xffffff,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_sprite_lighting_refine, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  OnLightingFeatureChanged(self);
}

CUSTOM_CVAR(Int, bd_fog_preset, 0,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL) {
  if (self < 0)
    self = 0;
  if (self > MaxFogPreset)
    self = MaxFogPreset;

  // A user-driven selection (menu, console, config) is an explicit choice:
  // drop the auto-pair tracking so graphics presets stop overriding it.
  if (!IsApplyingPreset())
    bd_autopaired_fog = 0;

  FPresetApplyScope applyScope;
  ApplyFogPreset(self);
}

CUSTOM_CVAR(Int, bd_fog_mode, 1,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 2)
    self = 2;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_sector_fog_scale, 1.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 5.0f)
    self = 5.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_density, 155.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 512.0f)
    self = 512.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Color, bd_fog_color, 0xc8c8be,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Int, bd_fog_color_mode, 0,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 2)
    self = 2;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_color_strength, 0.65f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_sky_strength, 0.85f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_thick_distance, 384.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 8192.0f)
    self = 8192.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_thick_multiplier, 8.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 1.0f)
    self = 1.0f;
  if (self > 64.0f)
    self = 64.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Int, bd_fog_gradient_mode, 1,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 2)
    self = 2;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Color, bd_fog_gradient_color, 0x6b746b,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_gradient_strength, 0.35f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_gradient_scale, 1.15f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 8.0f)
    self = 8.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_direction_yaw, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < -180.0f)
    self = -180.0f;
  if (self > 180.0f)
    self = 180.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_direction_pitch, 0.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < -89.0f)
    self = -89.0f;
  if (self > 89.0f)
    self = 89.0f;

  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Int, bd_fog_quality, 2,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 2)
    self = 2;
  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_height_falloff, 0.72f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 4.0f)
    self = 4.0f;
  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_turbulence, 0.22f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 0.5f)
    self = 0.5f;
  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_turbulence_scale, 0.008f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0001f)
    self = 0.0001f;
  if (self > 0.1f)
    self = 0.1f;
  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_sky_horizon, 0.78f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;
  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Float, bd_fog_min_visibility, 0.05f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0.0f)
    self = 0.0f;
  if (self > 1.0f)
    self = 1.0f;
  OnFogFeatureChanged(self);
}

CUSTOM_CVAR(Bool, bd_vis_autoscale, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {}

CUSTOM_CVAR(Float, bd_vis_autoscale_reference, 4096.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 1024.0f)
    self = 1024.0f;
  if (self > 262144.0f) // MAX_MAP_COORD in doomdef.h
    self = 262144.0f;
}

CUSTOM_CVAR(Float, bd_vis_autoscale_max, 64.0f,
            CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 1.0f)
    self = 1.0f;
  if (self > 256.0f)
    self = 256.0f;
}

CUSTOM_CVAR(Int, gl_crt_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 3)
    self = 3;
  if (self > 0)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}

CVAR(Float, gl_crt_distortion, 0.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_crt_zoom, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_crt_scanline, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_crt_scanline_density, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_crt_scanline_sharpness, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_crt_mask_intensity, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, gl_ntsc_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) {
  if (self < 0)
    self = 0;
  if (self > 1)
    self = 1;
  if (self > 0)
    EnsurePostFxActive();

  OnPresetFeatureChanged(self);
}
