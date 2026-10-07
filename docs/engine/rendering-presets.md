# Rendering Presets, Bloom, and Sector Light Bleed

This guide covers the BiasedDoom presentation controls added around the
4.15.x graphics stack: preset browsers, the graphics/lighting/fog preset
layers and how they compose, bloom tuning, sector-light blending, and live
renderer backend switching.

## Menus

The main entry point is:

`Options -> Display Options -> Advanced -> Postprocess -> Rendering Presets`

This submenu is the single home for the preset system: the **Graphics
Preset** selector, the **Link Preset Layers** toggle, the **Light Style
Preset** and **Fog Preset** selectors, all three searchable browsers, and a
**Reset to Vanilla Doom** button that restores the stock look in one click.

Each preset family keeps its traditional cycling selector, plus a searchable
browser:

- **Browse Graphics Presets** — 100 complete image/renderer looks, each row
  showing its paired lighting/fog presets (e.g. `VHS Horror → Horror
  Contrast / Cinematic Layers`).
- **Browse Lighting Presets** — 75 dynamic-light/material styles.
- **Browse Fog Presets** — 53 atmosphere/fog treatments.

The browser marks the active preset as `Current`.

## How the families compose

The three families are layers with a single owner each:

- **Graphics presets** own the image pipeline: postfx filters, bloom, CRT/VHS,
  colorgrade, tonemap, atmosphere, exposure, and quality toggles (SSAO, FXAA,
  shadow maps, dynamic-light culling).
- **Lighting presets** own every dynamic-light/material CVar
  (`bd_dynlight_*`, `bd_light_*`, `bd_gi_*`, `bd_rimlight_*`, `bd_aerial_*`,
  `bd_ambient_gradient_*`, `bd_specular_*`).
- **Fog presets** own every fog/gradient CVar (`bd_fog_*`, `bd_sector_fog_scale`).

How they relate is controlled by one switch, **Link Preset Layers**
(`bd_preset_locked`, default **On**):

- **Linked** — the Graphics Preset is the master look: selecting one always
  moves the `bd_lighting_preset` / `bd_fog_preset` selectors to the paired
  values, so the menus always show which look is actually active. Picking a
  lighting or fog preset manually afterwards drops the graphics selector to
  **Custom** — you have visibly left the curated combination. Individual
  feature-slider tweaks never reset the selectors.
- **Unlinked** — the three layers are fully independent: graphics preset
  changes never touch the other two selectors, so you can mix any image
  pipeline with any lighting style and any fog.

**Reset to Vanilla Doom** (in the Rendering Presets menu, or the
`resetrenderpresets` console command) restores the stock look in one action:
graphics `Vanilla+`, lighting `Classic Balanced`, fog `Disabled` — the
selectors themselves show exactly what vanilla means.

The same lighting and fog controls remain available from the classic Lighting
menu and the `Postprocess -> Atmosphere / Fog` submenu.

## Graphics preset reference

Every graphics preset deliberately picks a **tonemap** and **exposure** so the
differences are clear and noticeable: ACES for modern/HDR looks,
Uncharted2/Lottes for clean or filmic, Reinhard for soft/natural,
None/Palette for retro/performance, and the Gothic / Gothic Noir / Silent
Hill / Graveyard / Moonlit / Bleach Bypass set for horror and stylized looks.
Exposure (`gl_exposure_scale`, default 1.3) is listed when a preset overrides
it; lower values darken horror looks, higher values brighten clean looks.

Each entry lists its auto-paired **lighting → fog** presets. Presets marked
*(extends N)* are layered: they inherit another graphics preset's pipeline and
override a few settings, so their look tracks the base preset.

### Modern & clean

| # | Preset | Tonemap | Lighting → Fog | Look |
|---|--------|---------|----------------|------|
| 2 | Modern Crisp | ACES (exp 1.20) | Modern Pretty → Disabled | The default "modern game" look: bloom, gentle vignette, sharpen. |
| 19 | Clean Visibility *(extends 2)* | Uncharted2 (exp 1.25) | Bright Playable → Disabled | Readability-first: weak bloom, clarity filter, higher exposure floor. |
| 38 | Competitive Clarity *(extends 2)* | Uncharted2 (exp 1.20) | Bright Playable → Disabled | Esports-style: no bloom, FXAA, sharpen + clarity. |
| 43 | Clarity Max | Uncharted2 (exp 1.20) | Crisp Tactical → Disabled | Maximum sharpen + clarity, no bloom, no atmosphere. |
| 57 | Clean Lens *(extends 38)* | ACES (exp 1.20) | Modern Pretty → Disabled | Competitive clarity with a filmic ACES finish. |
| 21 | Cool Clarity *(extends 12)* | ACES (exp 1.10) | Crisp Tactical → Natural Haze | Cool, crisp daylight with a light haze. |
| 31 | Cinematic Ultra | ACES (exp 1.25) | Warm Cinematic → Cinematic Layers | Full cinematic stack: SSAO, shadow maps, god rays, lens flare, grade. |
| 39 | HDR Showcase | ACES (exp 1.35) | PBR Showcase → Natural Haze | Max bloom energy + SSAO/shadow maps; built to show off PBR/glTF materials. |
| 40 | Maxed Out | ACES (exp 1.25) | Modern Pretty → Cinematic Layers | Everything on: SSAO high, FXAA, shadow maps, god rays, lens flare, clarity. |

### Performance

| # | Preset | Tonemap | Lighting → Fog | Look |
|---|--------|---------|----------------|------|
| 1 | Vanilla+ | None | Classic Balanced → Disabled | Classic Doom image with postfx available at low quality. |
| 6 | Low-End Performance | None | Classic Balanced → Disabled | Postfx off entirely; the baseline for weak GPUs. |
| 36 | Ultra Lightweight | None | Classic Balanced → Disabled | Aggressive dynamic-light culling (8/surface, 1500 range, 256 shadow lights). |
| 37 | Balanced Performance | Reinhard | Classic Balanced → Map Enhanced | FXAA + light vibrance, culled lights, map-authored fog only. |
| 58 | Low Glow *(extends 6)* | Reinhard | Soft Natural → Map Enhanced | Cheap gentle bloom over the low-end base. |

### Retro & CRT

| # | Preset | Tonemap | Lighting → Fog | Look |
|---|--------|---------|----------------|------|
| 3 | CRT Arcade | Bleach Bypass | Arcade Bright → Disabled | Scanline CRT, light grain and chromatic fringe, neon grade. |
| 23 | Readable CRT *(extends 3)* | Bleach Bypass | Arcade Bright → Disabled | The CRT look tuned down for actual gameplay. |
| 52 | Soft Retro *(extends 3)* | None | Classic Balanced → Disabled | Aperture-grille CRT with soft, wide bloom. |
| 33 | Retro Poster | Palette | Arcade Bright → Disabled | Pixelate ×2 + posterize + warm gain: a printed-poster look. |
| 51 | Sharp Retro *(extends 1)* | Palette | Arcade Bright → Disabled | Pixelate ×2 with maximum sharpen and FXAA. |
| 34 | Cel Comic | Uncharted2 | Studio Soft → Disabled | Posterize 8 + strong edge glow: comic-book ink. |
| 54 | Cel Shadows *(extends 34)* | Uncharted2 | Rim Drama → Disabled | Cel look with dramatic rim lighting and harder posterize. |
| 64 | Arcade Neon *(extends 32)* | ACES | Neon Glow → Disabled | Neon vibrance quantized by posterize 5. |

### Horror & found footage

| # | Preset | Tonemap | Lighting → Fog | Look |
|---|--------|---------|----------------|------|
| 7 | Silent Hill Fog | Silent Hill (exp 0.95) | Horror Contrast → Dense Horror | The signature town fog: Fogbound atmosphere at high intensity. |
| 4 | VHS Horror | Silent Hill (exp 1.00) | Horror Contrast → Cinematic Layers | Camcorder tape over a horror base: VHS artifacts + Silent Hill grade. |
| 24 | Action Horror *(extends 4)* | ACES (exp 1.05) | Horror Contrast → Cinematic Layers | Same tape, but brighter and punchier so combat stays readable. |
| 25 | VHS Found Footage *(extends 4)* | Silent Hill (exp 1.00) | Flickering Candlelight → Cinematic Layers | Heavier grain, subtle tracking, candle-lit scenes. |
| 26 | VHS Tape Rot *(extends 4)* | Silent Hill (exp 1.00) | Horror Contrast → Cinematic Layers | Degraded tape: maximum jitter, tracking errors, ghosting. |
| 27 | VHS Night Vision *(extends 4)* | Gothic (exp 1.30) | Ectoplasm → Blackout | Green-lit, brightened night-vision tape. |
| 28 | Possessed VHS *(extends 4)* | Gothic Noir (exp 1.00) | Void Dread → Blackout | The tape fights back: high "evil" distortion and darkness. |
| 16 | Analog Horror | Silent Hill (exp 0.90) | Analog Fluorescent → Analog Sepia | Aperture-grille CRT, pixelation, buzzing fluorescent light, sepia murk. |
| 22 | Dense Playable Fog *(extends 7)* | Reinhard (exp 0.95) | Soft Natural → Dense Horror | Silent-Hill density with values tuned to stay playable. |
| 8 | Ashen Graveyard | Graveyard (exp 0.95) | Void Dread → Cinematic Layers | Desaturated ash and cold dread. |
| 10 | Moonlit Noir | Moonlit (exp 0.90) | Moonlit Expanse → Blue Hour | Blue-night detective noir. |
| 53 | Noir Punch *(extends 10)* | Gothic Noir (exp 0.90) | Neon Noir → Blue Hour | Noir with hard contrast grade and neon accents. |
| 29 | Blood Moon Evil *(extends 11)* | Graveyard (exp 1.20) | Ruby Corridor → Crimson Eclipse | Red-lit ritual horror under a crimson sky. |
| 30 | Void Ritual *(extends 10)* | Gothic Noir (exp 0.85) | Void Dread → Blackout | Near-black occult darkness. |
| 49 | Dark Ambient *(extends 18)* | Graveyard (exp 0.80) | Deep Cavern → Cinematic Layers | The darkest ambient look that still reads. |
| 18 | Low Light Realism | Reinhard (exp 0.85) | Low Light Realism → Map Enhanced | Slow-adapting low exposure; respects map lighting. |

### Stylized & atmospheric

| # | Preset | Tonemap | Lighting → Fog | Look |
|---|--------|---------|----------------|------|
| 5 | Industrial Hell | Lottes Filmic (exp 1.15) | Hellfire Glow → Directional Dusk | Sodium-vapor atmosphere and rust grade over hot industrial light. |
| 9 | Toxic Reactor | Lottes Filmic (exp 1.15) | Ectoplasm → Toxic Haze | Radioactive green with heavy bloom. |
| 11 | Inferno Bloom | Lottes Filmic (exp 1.20) | Hellfire Glow → Dust Storm | Burning, high-energy bloom. |
| 12 | Frozen Wasteland | Bleach Bypass (exp 1.10) | Arctic Facility → Polar Whiteout | Cold, bleached, sharp. |
| 13 | Sodium Streets | Lottes Filmic (exp 1.05) | Amber Ember → Directional Dusk | Amber street-lamp glow against a bleak blue sky. |
| 14 | Cyberpunk Rain | ACES (exp 1.10) | Neon Noir → Blue Hour | Strong neon bloom, lens flares, otherworld atmosphere, rain-film shimmer. |
| 62 | Violet Dusk *(extends 14)* | Moonlit (exp 1.10) | Aurora Veil → Blue Hour | Cyberpunk shifted violet. |
| 20 | Warm Cinematic *(extends 13)* | Lottes Filmic (exp 1.05) | Warm Cinematic → Directional Dusk | Golden, filmic warmth. |
| 60 | Golden Film *(extends 42)* | Lottes Filmic | Golden Hour → Directional Dusk | Warm film stock with gentle lens flare. |
| 42 | Analog Cinema | Lottes Filmic | Warm Cinematic → Natural Haze | 35mm grain, lens flare, SSAO + shadow maps. |
| 35 | Sepia Archive | Reinhard | Dusty Archive → Natural Haze | Warm white balance, heavy grain, deep vignette: an old photograph. |
| 48 | Muted Pastels *(extends 35)* | Reinhard | Soft Natural → Morning Mist | The archive look softened into pastel tones. |
| 41 | Divine Radiance | Lottes Filmic (exp 1.30) | Cathedral Bloom → Cathedral Haze | Strong god rays through luminous cathedral haze. |
| 56 | Overexposed *(extends 41)* | Reinhard (exp 1.80) | Surgical White → Disabled | Deliberately blown-out, high-key brightness. |
| 65 | Absolution (Doom 64) | None (exp 1.30) | Absolution → Absolution | The notorious lights-off Doom 64 gloom: darkness lives in the lighting and fog layers because tonemaps, the low postfx quality path, and lowered exposure all lift the dark sky veil to grey. |
| 66 | Absolution: Ember *(extends 65)* | None (exp 1.30) | Ember Reliquary → Ember Gloom | Doom 64 darkness recolored as furnace-red light pools and smoke. |
| 67 | Absolution: Verdigris *(extends 65)* | None (exp 1.30) | Verdigris → Verdigris Veil | Oxidized-copper teal and green under the neutral dark-safe pipeline. |
| 68 | Absolution: Amethyst *(extends 65)* | None (exp 1.30) | Amethyst → Amethyst Veil | Violet crystal light catches a little extra bloom without turning the sky grey. |
| 69 | Absolution: Cobalt *(extends 65)* | None (exp 1.30) | Cobalt → Cobalt Night | Cold navy darkness and sharply readable blue highlights. |
| 70 | Gilded Reliquary *(extends 41)* | Lottes Filmic (exp 1.30) | Gilded Reliquary → Incense Gold | A playable gold cathedral: treasure-vault bloom and restrained god rays. |
| 71 | Jade Sanctuary *(extends 48)* | Reinhard | Jade Sanctuary → Jade Mist | Serene painted jade daylight: low glow, soft color, and clear combat values. |
| 72 | Ashen Eclipse *(extends 65)* | None (exp 1.30) | Ashen Eclipse → Ashfall | A dark volcanic night, with ash grain and ember accents instead of a crushed black image. |
| 73 | Pelagic Temple *(extends 45)* | Reinhard (exp 1.20) | Pelagic Temple → Abyssal Teal | Watery teal softness and tiny caustic edges, without Dreamlike shafts. |
| 74 | Roseglass Chapel *(extends 42)* | Lottes Filmic | Roseglass Chapel → Roseglass Haze | Stained-glass rose and gold rendered as restrained film stock. |
| 75 | Stormbound Citadel *(extends 53)* | Gothic Noir (exp 0.90) | Stormbound → Thunderhead | Rain-noir blue-grey fortress contrast with a controlled lightning flicker. |
| 76 | Autumnal Ruins *(extends 42)* | Lottes Filmic | Autumnal Ember → Autumn Mist | Warm woodland ruins with a subtle 35 mm texture and late-afternoon glow. |
| 77 | Aurora Winter *(extends 61)* | Bleach Bypass (exp 1.10) | Aurora Winter → Aurora Frost | Icy cyan, faint magenta rim, and crystalline facility clarity. |
| 78 | Solar Flare Bazaar *(extends 50)* | Uncharted2 (exp 1.25) | Solar Flare → Solar Dust | Sharp sun-gold action visibility with modest flare and shafts. |
| 79 | Bioluminescent Grotto *(extends 46)* | ACES | Bioluminescent Grotto → Luminous Grotto | Electric aqua cave contrast with a safe readable floor. |
| 80 | Bloodglass Eclipse *(extends 65)* | None (exp 1.30) | Bloodglass Eclipse → Bloodglass Veil | Ruby-magenta darkness using the neutral Absolution image pipeline. |
| 81 | Clockwork Brass *(extends 57)* | ACES (exp 1.20) | Clockwork Brass → Brass Smog | Polished brass mechanisms under clear PBR-style lens treatment. |
| 82 | Orchid Nebula *(extends 62)* | Moonlit (exp 1.10) | Orchid Nebula → Orchid Nebula | Violet space-fantasy with a controlled cyberpunk shimmer. |
| 83 | Neon Lotus *(extends 32)* | ACES | Neon Lotus → Lotus Neon Mist | Hot pink and cyan cyber-fantasy, deliberately below eye-strain intensity. |
| 84 | Frostfire Citadel *(extends 12)* | Bleach Bypass (exp 1.10) | Frostfire → Frostfire Haze | Bleached blue-steel architecture punctuated by restrained orange embers. |
| 85 | Mirage Oasis *(extends 20)* | Lottes Filmic (exp 1.05) | Mirage Oasis → Oasis Mirage | Turquoise shade and sunlit sand with long, readable horizons. |
| 86 | Phantom Carnival *(extends 52)* | None | Phantom Carnival → Carnival Smoke | A playful magenta haunted fairground projected through a gentle CRT. |
| 87 | Obsidian Monsoon *(extends 65)* | None (exp 1.30) | Obsidian Monsoon → Obsidian Rain | Near-black rain ambience with storm-blue navigation cues. |
| 88 | Prism Garden *(extends 55)* | Moonlit (exp 1.10) | Prism Garden → Prism Bloom | Bright pastel fantasy as a gentle watercolor painting. |
| 89 | Mushroom Moon *(extends 63)* | Reinhard (exp 1.10) | Mushroom Moon → Spore Moonlight | Hazy purple-blue spore photography that still reads as a game space. |
| 90 | Chrome Basilica *(extends 57)* | ACES (exp 1.20) | Chrome Basilica → Silver Haze | Polished sci-fi PBR clarity, restrained bloom, and pearl-cyan metal. |
| 91 | Phosphor Terminal *(extends 51)* | Palette | Phosphor Terminal → Terminal Bloom | Fine phosphor CRT/pixel texture with emerald and amber terminal light. |
| 92 | Porcelain Citadel *(extends 54)* | Uncharted2 | Porcelain Citadel → Porcelain Veil | Calm high-key graphic-novel daylight with crisp silhouettes. |
| 93 | Lantern Festival *(extends 60)* | Lottes Filmic | Lantern Festival → Lantern Smoke | Intimate lantern-lit film stock with gold glow and ink-blue night. |
| 94 | Glasshouse Rain *(extends 14)* | ACES (exp 1.10) | Glasshouse Rain → Glasshouse Rain | Emerald wet-neon greenhouse rain, tuned below eye-strain intensity. |
| 95 | Mercury Mirror *(extends 53)* | Gothic Noir (exp 0.90) | Mercury Mirror → Mercury Haze | Cool near-monochrome noir built to preserve reflective material detail. |
| 96 | Cinder Opera *(extends 47)* | Lottes Filmic | Cinder Opera → Velvet Smoke | Charcoal, oxblood velvet, and theatrical amber footlights. |
| 97 | Opaline Reef *(extends 55)* | Moonlit (exp 1.10) | Opaline Reef → Pearl Water | Bright pearl-and-coral watercolor exploration without underwater darkness. |
| 98 | Ultraviolet Archive *(extends 59)* | Moonlit (exp 1.10) | Ultraviolet Archive → Ultraviolet Ink Mist | Fluorescent manuscript fantasy with violet ink and cyan spectral edges. |
| 99 | Saffron Sandstorm *(extends 5)* | Lottes Filmic (exp 1.15) | Saffron Sandstorm → Saffron Sand | Weather-driven saffron sun and petrol-blue storm haze with a clear near field. |
| 100 | Polar Signal Station *(extends 52)* | None | Polar Signal Station → Polar Signal | Pale cyan retro projection and gentle radio texture over icy distance. |
| 44 | Dreamlike | Moonlit (exp 1.10) | Soft Dawn → Morning Mist | Wide soft bloom and god rays; a waking dream. |
| 55 | Watercolor Dream *(extends 44)* | Moonlit (exp 1.10) | Soft Dawn → Morning Mist | Dream-decay grade, no sharpen: painted edges. |
| 63 | Soft Focus *(extends 44)* | Reinhard (exp 1.10) | Soft Natural → Morning Mist | Gentle bloom knee, no sharpen: a soft lens. |
| 17 | Dream Decay | Moonlit (exp 1.00) | Aurora Veil → Morning Mist | A decaying dream: VHS shimmer, god rays, cyberpunk atmosphere. |
| 59 | Spectral *(extends 30)* | Moonlit (exp 0.95) | Ectoplasm → Cathedral Haze | Ghostly: hue-shifted edge glow over the void-ritual base. |
| 32 | Neon Vibrance | ACES | Neon Glow → Disabled | Maximum vibrance and chromatic energy. |
| 46 | Neon Bloom *(extends 32)* | ACES | Neon Glow → Disabled | The neon look with bloom pushed to the playable limit. |
| 45 | Soft Bloom *(extends 2)* | Reinhard (exp 1.20) | Soft Natural → Natural Haze | Low-threshold, wide, soft bloom. |
| 47 | Subtle Film *(extends 2)* | Lottes Filmic (exp 1.20) | Soft Natural → Map Enhanced | A light grain and gentle bloom over modern crisp. |
| 50 | Bright Ambient *(extends 19)* | Uncharted2 (exp 1.25) | Overcast Day → Natural Haze | High ambient floor: everything readable, nothing crushed. |
| 61 | Cold Facility *(extends 12)* | Bleach Bypass (exp 1.10) | Arctic Facility → Natural Haze | Cold white balance and clarity: institutional sci-fi. |
| 15 | Bleach Bunker | Bleach Bypass (exp 1.35) | Cold Industrial → Natural Haze | High-key, desaturated concrete interiors. |

## Lighting preset reference

Lighting presets shape dynamic lights and materials: falloff model (Linear /
Inverse-square / Power), intensity, saturation, color temperature, ambient
floor, specular and emissive response, GI-style ambient fill, rim light,
ambient gradients, flicker, and aerial perspective. The character column
summarizes the intent.

| # | Preset | Character |
|---|--------|-----------|
| 1 | Classic Balanced | Linear falloff, neutral everything: the legacy Doom light behavior. |
| 2 | Modern Pretty | Inverse-square, slight warmth, gentle GI fill and wrap: a safe modern upgrade. |
| 3 | Warm Cinematic | Power falloff, warm temperature, stronger specular and emissive response. |
| 4 | Horror Contrast | Dim, desaturated, slightly cold, steep power falloff: pools of light in darkness. |
| 5 | Neon Glow | Highly saturated, strong emissive boost, cool tint: signs and energy weapons pop. |
| 6 | PBR Showcase | Maximum specular scale and emissive boost for metallic-roughness materials. |
| 7 | Bright Playable | Raised ambient floor and GI fill: visibility first, never crushed blacks. |
| 8 | Soft Natural | Warm-neutral with very soft falloff: daylight interiors. |
| 9 | Crisp Tactical | Tight, low-saturation lights with hard shadows. |
| 10 | Low Light Realism | Very dim, cold, minimal fill: flashlight territory. |
| 11 | Hellfire Glow | Hot temperature, burning specular and emissive response. |
| 12 | Void Dread | Extremely dark and cold; almost no ambient. |
| 13 | Studio Soft | Soft wrap and low contrast, like a photo studio. |
| 14 | Overcast Day | Flat, cool, high ambient floor, low contrast. |
| 15 | Golden Hour | Warm low sun: strong specular, light rim, warm gradient. |
| 16 | Cold Industrial | Very cold, hard specular, high contrast: fluorescents on concrete. |
| 17 | Pitch Black | Zero ambient floor with faint rim: darkness as a mechanic. |
| 18 | Arcade Bright | Bright, saturated, soft: coin-op energy. |
| 19 | Rim Drama | Strong rim light and contrast for silhouette drama. |
| 20 | Gradient Ambience | Cool ambient gradient washes the scene. |
| 21 | Flickering Candlelight | Warm, heavily flickering point lights. |
| 22 | Aerial Vista | Long-range aerial perspective with a cool gradient. |
| 23 | Candlelit Crypt | Warm crypt light: flicker, rim, short-range aerial depth. |
| 24 | Moonlit Expanse | Cold blue moonlight with wide aerial perspective. |
| 25 | Emergency Strobe | Aggressive flicker with red specular tint. |
| 26 | Aurora Veil | Saturated teal-green gradients and glow. |
| 27 | Dusty Archive | Warm, dusty amber with soft aerial depth. |
| 28 | Ruby Corridor | Saturated red light with red-tinted specular and rim. |
| 29 | Surgical White | Cold, bright, desaturated: operating-room clarity. |
| 30 | Ectoplasm | Heavily saturated green glow with matching gradients. |
| 31 | Storm Front | Cold, flickering storm light with rain-distance aerial fade. |
| 32 | Amber Ember | Very warm ember light with gentle flicker. |
| 33 | Deep Cavern | Near-zero ambient, cold, high contrast: cave darkness. |
| 34 | Cathedral Bloom | Warm luminous volume: strong GI, golden gradient, long aerial. |
| 35 | Neon Noir | Cold, ultra-saturated violet noir with hard rim. |
| 36 | Desert Heat | The hottest temperature, high floor, long heat-haze aerial. |
| 37 | Arctic Facility | The coldest temperature, desaturated, sharp specular. |
| 38 | Soft Dawn | Warm-soft rose dawn with gentle aerial fade. |
| 39 | Analog Fluorescent | Cold, desaturated, buzzing flicker: found-footage institutions. |
| 40 | Absolution | Pitch-black Doom 64 gloom: strong light diminishing, saturated colored light pools, no GI ambient (its sector-bleed feed washes the sky veil on outdoor levels). |
| 41 | Ember Reliquary | Absolution darkness with furnace-red light pools and ember-tinted specular. |
| 42 | Verdigris | Oxidized copper: dark teal-green pools, cool stone, and bright mint highlights. |
| 43 | Amethyst | Saturated violet crystal pools with a sharp, readable specular response. |
| 44 | Cobalt | Midnight-blue light pools and cold navy distance contrast. |
| 45 | Gilded Reliquary | Warm gold shafts, polished brass highlights, and a friendly exploration floor. |
| 46 | Jade Sanctuary | Soft jade-green ambient fill for serene fantasy exploration. |
| 47 | Ashen Eclipse | Smoky ash contrast with restrained warm ember accents. |
| 48 | Pelagic Temple | Clear teal water-light and long submerged depth. |
| 49 | Roseglass Chapel | Rose-gold glow with stained-glass coloured rim light. |
| 50 | Stormbound | Blue-grey lightning contrast with a controlled storm flicker. |
| 51 | Autumnal Ember | Inviting amber light for ruins, foliage, and late-day scenes. |
| 52 | Aurora Winter | Icy cyan light with a faint magenta spectral rim. |
| 53 | Solar Flare | Saturated sun-gold action light with a generous ambient floor. |
| 54 | Bioluminescent Grotto | Aqua cave glow and safe low-light exploration ambience. |
| 55 | Bloodglass Eclipse | Ruby pools and magenta rims with hard, readable silhouettes. |
| 56 | Clockwork Brass | Antique bronze warmth and precise mechanical specular highlights. |
| 57 | Orchid Nebula | Soft purple space-light with pink spectral glints. |
| 58 | Neon Lotus | High-chroma pink and cyan that keeps the world floor visible. |
| 59 | Frostfire | Cold blue field with restrained orange fire accents. |
| 60 | Mirage Oasis | Turquoise shade, sunlit sand, and long desert distance. |
| 61 | Phantom Carnival | Candy-magenta light with a gentle mischievous flicker. |
| 62 | Obsidian Monsoon | Charcoal storm contrast and cold silver flashes. |
| 63 | Prism Garden | Pastel, high-readability colour without neon clipping. |
| 64 | Mushroom Moon | Magenta bioluminescence in playable twilight darkness. |
| 65 | Chrome Basilica | Cool pearl stone, precise silver specular, and material-forward cyan reflections. |
| 66 | Phosphor Terminal | Emerald and amber terminal light with a safe, readable world floor. |
| 67 | Porcelain Citadel | Pale blue-white daylight with soft graphic shadows and long visibility. |
| 68 | Lantern Festival | Warm paper-lantern pools set against a deep ink-blue night. |
| 69 | Glasshouse Rain | Emerald wet-specular glow and cyan reflected shade with gentle rain flicker. |
| 70 | Mercury Mirror | Restrained silver illumination with high-density reflective material detail. |
| 71 | Cinder Opera | Theatrical amber footlights, oxblood shadows, and a restrained stage flicker. |
| 72 | Opaline Reef | Coral-cyan daylight, pearly fill, and bright exploration readability. |
| 73 | Ultraviolet Archive | Ultraviolet ink pools with crisp cyan manuscript rims. |
| 74 | Saffron Sandstorm | Hot saffron sun meeting a petrol-blue storm ceiling. |
| 75 | Polar Signal Station | Cold cyan-violet signal light with a subtle radio flicker. |

## Fog preset reference

Fog presets own the depth-cueing layer: global Silent-Hill-style fog
(`bd_fog_mode 1`) or sector-boost mode (`2`) that only enhances map-authored
fog, plus density, color policy, sky blending, thick-fog distance walls,
height falloff, turbulence, and directional gradients.

| # | Preset | Character |
|---|--------|-----------|
| 1 | Disabled | All fog enhancements off. |
| 2 | Map Enhanced | Respects map-authored fog; only improves its integration. |
| 3 | Natural Haze | Light, gameplay-friendly distance haze. |
| 4 | Cinematic Layers | Moderate layered global fog for depth. |
| 5 | Dense Horror | Thick greenish-grey horror fog with a hard gradient. |
| 6 | Directional Dusk | Warm dusk haze with directional falloff. |
| 7 | Morning Mist | Soft, cool white-blue mist. |
| 8 | Toxic Haze | Green-yellow, thick mid-distance fog. |
| 9 | Blackout | Near-field visibility behind an oppressive black wall. |
| 10 | Green Valley | Soft vegetation haze. |
| 11 | Blue Hour | Deep blue dusk falloff. |
| 12 | Crimson Eclipse | Directional red-brown gloom. |
| 13 | Underwater | Dense blue-green depth haze. |
| 14 | Dust Storm | Warm, rolling, highly turbulent dust. |
| 15 | Polar Whiteout | Bright, cold, low-contrast distance loss. |
| 16 | Cathedral Haze | Luminous vertical shafts with gentle depth. |
| 17 | Analog Sepia | Murky brown-black found-footage haze. |
| 18 | Absolution | Doom 64-inspired oppressive blue-violet gloom. |
| 19 | Ember Gloom | Furnace-red smoke and dark ember depth walls for Absolution maps. |
| 20 | Verdigris Veil | Dark oxidized teal-green fog with navigable lit space. |
| 21 | Amethyst Veil | Saturated violet depth that keeps silhouettes legible. |
| 22 | Cobalt Night | Midnight-blue distance for cool, dark outdoor levels. |
| 23 | Incense Gold | Luminous gold cathedral haze with generous visibility. |
| 24 | Jade Mist | Soft green sanctuary haze for exploration. |
| 25 | Ashfall | Volcanic grey-brown fog with ember-coloured air. |
| 26 | Abyssal Teal | Dense underwater teal with a readable near field. |
| 27 | Roseglass Haze | Pink-violet chapel air with a gentle vertical lift. |
| 28 | Thunderhead | Blue-grey storm banks with directional movement. |
| 29 | Autumn Mist | Honeyed low fog for warm ruins and woodland maps. |
| 30 | Aurora Frost | Pale cyan fog with an aurora-like high gradient. |
| 31 | Solar Dust | Bright sun-gold distance for fast outdoor encounters. |
| 32 | Luminous Grotto | Aqua cave mist with a clear near field. |
| 33 | Bloodglass Veil | Crimson-magenta depth without Blackout-level visibility loss. |
| 34 | Brass Smog | Antique bronze haze with soft rolling movement. |
| 35 | Orchid Nebula | Lavender depth and a subtle star-cloud gradient. |
| 36 | Lotus Neon Mist | Pink-cyan fog with generous playable distance. |
| 37 | Frostfire Haze | Cold blue distance with an ember-coloured floor. |
| 38 | Oasis Mirage | Turquoise air and sun-warm directional distance. |
| 39 | Carnival Smoke | Sweet purple haze with mild festive motion. |
| 40 | Obsidian Rain | Storm-black distance with enough blue to navigate. |
| 41 | Prism Bloom | Pale rainbow-pastel distance with excellent visibility. |
| 42 | Spore Moonlight | Rich purple-blue mushroom mist, dark but readable. |
| 43 | Silver Haze | Thin steel distance for polished material-showcase maps. |
| 44 | Terminal Bloom | Gentle green phosphor air that keeps targets readable. |
| 45 | Porcelain Veil | Pale blue-white distance with crisp dark silhouettes. |
| 46 | Lantern Smoke | Warm lantern near-air receding into cool, navigable night. |
| 47 | Glasshouse Rain | Green-blue rain air with soft greenhouse movement. |
| 48 | Mercury Haze | Thin steel-grey air that leaves reflective detail intact. |
| 49 | Velvet Smoke | Burgundy theatre haze with a safe amber middle distance. |
| 50 | Pearl Water | Clear pale aqua-pink depth for bright coral exploration. |
| 51 | Ultraviolet Ink Mist | Violet manuscript air edged by cyan spectral distance. |
| 52 | Saffron Sand | Turbulent gold dust with a deliberately clear near field. |
| 53 | Polar Signal | Clear icy mist with a cyan directional broadcast veil. |

### Sky fog (physical horizon matching)

The sky is geometry at infinite distance, so its fog is derived from the same
atmospheric model as the level geometry instead of a hand-tuned overlay. For
each fog-dome vertex the engine evaluates the transmittance to *infinity*
through the exponential height fog used by the fragment shader
(`T(e) = exp2(sigma / (k * ln2 * sin e))` at elevation `e`), floors it with the
same `bd_fog_min_visibility` term, and tints it with the same
`bd_fog_gradient_*` spatial gradient evaluation as `getFogColor()`.

Consequences:

- **No horizon seam on open maps** (e.g. Doom2 MAP13): as elevation approaches
  the horizon, coverage approaches `1 - bd_fog_min_visibility`, which is exactly
  the saturation coverage of far walls, so sky and geometry meet in the same
  color by construction.
- The elevation profile tracks `bd_fog_height_falloff` and the effective
  density automatically, for every preset.
- The veil follows **any** fog that is actually applied to geometry — biased
  global fog (`bd_fog_mode 1`), sector boost (`2`), *and* plain map-authored
  fog (MAPINFO `fogdensity` / sector colormaps with `bd_fog_mode 0`). Black
  fog (distance light-diminishing) never veils the sky, and MAPINFO `skyfog`
  remains as a flat alpha floor.

`bd_fog_sky_strength` is the horizon-match dial (0 disables the sky veil, 1 is
the physically exact match), while `bd_fog_height_falloff` is the zenith dial:
it controls how quickly the sky clears with elevation — the same curve that
makes geometry fog lie low in valleys. Fog presets are tuned around this
pairing: scenic presets (Natural Haze, Morning Mist, Green Valley, Cathedral
Haze) keep zenith coverage near 0.5 so the sky texture stays in the
composition, atmospheric ones (Cinematic Layers, Directional Dusk, Blue Hour,
Crimson Eclipse) sit near 0.65–0.8, and oppressive ones (Dense Horror, Toxic
Haze, Blackout, Underwater, Dust Storm, Polar Whiteout, Analog Sepia) let the
sky become the fog. Thick-fog walls feed the sky only a fraction (0.35) of
their density multiplier: the horizon match is density-independent, and the
full multiplier would saturate the sky to the zenith and erase its texture.
`bd_fog_sky_horizon` is deprecated and no longer read: the physical curve
replaces the hand-tuned one; the CVar stays registered so old presets and INI
files keep working. Verified by `tools/test-sky-fog.sh` and
`tools/test-fog-presets.sh`, which measure the horizon junction on a generated
open map for explicit settings and for every fog preset.

## Combining presets by hand

Auto-pairings are starting points, not rules. To compose your own look, turn
off **Link Preset Layers** first; graphics, lighting, and fog selectors then
remain independent until you turn linking back on. With linking enabled, a
new graphics selection deliberately re-applies its named lighting/fog pair,
and choosing lighting or fog directly visibly changes the graphics selector to
**Custom**.

1. Turn off **Link Preset Layers**.
2. Pick a graphics preset for the image pipeline (tonemap, filters, exposure).
3. Pick a lighting preset for the dynamic-light character.
4. Pick a fog preset for depth cueing.

Example recipes:

- **Modern AAA**: graphics 31 (Cinematic Ultra), then lighting 6 (PBR
  Showcase) and fog 3 (Natural Haze) for a brighter showcase.
- **Survival horror**: graphics 7 (Silent Hill Fog), then lighting 17 (Pitch
  Black) — the fog stays dense while darkness becomes the enemy.
- **Boomer-shooter night patrol**: graphics 14 (Cyberpunk Rain), then fog 1
  (Disabled) for crisp rooftops while keeping neon bloom.
- **Hand-tuned looks**: enable `bd_preset_locked` before adjusting individual
  sliders so your tweaks retain the visible curated selector labels.

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
