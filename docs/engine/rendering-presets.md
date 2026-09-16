# Rendering Presets, Bloom, and Sector Light Bleed

This guide covers the BiasedDoom presentation controls added around the
4.15.x graphics stack: preset browsers, bloom tuning, sector-light blending,
and live renderer backend switching.

## Menus

The main entry point is:

`Options -> Display Options -> Advanced -> Postprocess`

Each preset family keeps its traditional cycling selector, plus a searchable
browser:

- **Browse Graphics Presets** — 64 complete image/renderer looks.
- **Browse Lighting Presets** — 38 dynamic-light/material styles.
- **Browse Fog Presets** — 16 atmosphere/fog treatments.

The browser marks the active preset as `Current`. Selecting a preset applies it
without resetting the other families. Changing an individual feature afterward
marks only that family as `Custom`; the other preset selectors remain intact.

The same lighting and fog controls remain available from the classic Lighting
menu and the `Postprocess -> Atmosphere / Fog` submenu.

## Bloom

Bloom now uses radius-matched Gaussian weights, mirrored edge sampling, and
energy-preserving mip transfers. The final composite applies the highlight
rolloff once, so large-radius bloom remains visible without repeatedly dimming
the pyramid.

| CVar | Default | Purpose |
|------|---------|---------|
| `bd_bloom_enable` | `false` | Enable bloom. |
| `bd_bloom_strength` | `1.4` | Base bloom width/energy. |
| `bd_bloom_radius` | `1.0` | Additional blur radius scale. |
| `bd_bloom_threshold` | `0.92` | Luminance threshold for extraction. |
| `bd_bloom_knee` | `0.55` | Soft knee around the threshold. |
| `bd_bloom_intensity` | `1.0` | Final bloom output multiplier. |

Lower thresholds bloom more of the scene; larger knees produce a smoother
transition. Radius changes no longer break the filter weights, and mirrored
sampling prevents bright lights from streaking at screen edges.

## Sector Light Bleed

Sector light bleed smooths the hard light discontinuity between adjacent floor
and ceiling sectors. BiasedDoom generates a low-resolution world-space light
map at level load and refreshes it when sector light levels or colors change.

| CVar | Default | Purpose |
|------|---------|---------|
| `bd_sectorlight_bleed` | `true` | Enable the effect. |
| `bd_sectorlight_distance` | `192` | Maximum bleed distance in map units. |
| `bd_sectorlight_strength` | `1.0` | Blend strength, from 0 to 1. |

The generator skips sky sectors, fog boundaries, disconnected portal groups,
and 3D-floor control sectors. Maps using `LEVEL3_NOLIGHTFADE` opt out. The
texture is retained for live renderer switches and re-uploaded to the new
backend.

The shader preserves the current lighting mode: software-style dimming,
hardware light colors, fog shaping, glows, and dynamic lights still apply on
top of the blended sector light.

## Live Backend Switching

`vid_preferbackend` can switch the active hardware backend at a frame boundary
on platforms with live-switch support:

| Value | Backend |
|-------|---------|
| `0` | OpenGL |
| `1` | Vulkan |
| `2` | OpenGLES |
| `3` | OpenGLES compatibility alias |

When live switching is unavailable, the console reports that a restart is
required. Level geometry, lightmaps, sector bleed maps, materials, 2D drawers,
and post-process resources are rebuilt or invalidated during a live switch.
