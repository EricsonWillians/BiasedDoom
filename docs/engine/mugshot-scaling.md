# Mugshot Scaling And Positioning

**System:** BiasedDoom status bars

**Components:** stock Doom ZScript HUD and legacy SBARINFO

**Status:** Implemented in BiasedDoom 4.15.4

## Purpose

Classic Doom face graphics occupy a small status-bar slot, normally 24×29 virtual pixels. High-resolution replacements can report much larger display dimensions and overwhelm that layout. BiasedDoom separates the solution into two layers:

1. A HUD author can give a legacy SBARINFO `drawmugshot` command an explicit base width and height.
2. A player can scale and reposition the resolved portrait at runtime without editing the mod.

The same player transform is applied by the stock Doom ZScript status bar and by the C++ legacy SBARINFO renderer.

## Player Transform

The archived global CVars are:

| CVar | Range | Default | Effect |
|---|---:|---:|---|
| `hud_mugshot_scale` | 0.25–4.00 | 1.00 | Uniformly scales the resolved base size |
| `hud_mugshot_xoffset` | -160–160 | 0 | Moves the portrait horizontally |
| `hud_mugshot_yoffset` | -100–100 | 0 | Moves the portrait vertically |

Scaling is anchored to the portrait slot's horizontal center and bottom edge. A larger portrait therefore grows upward and outward from the classic face position rather than falling below the status bar.

For base size `(W, H)`, scale `s`, and offsets `(ox, oy)`, the renderer uses:

```text
drawW = round(W × s)
drawH = round(H × s)
drawX = baseX + (W - drawW) / 2 + ox
drawY = baseY + H - drawH + oy
```

The menu exposes these controls under `Options -> HUD Options -> Mugshot options` and includes a command that resets all three CVars.

## Legacy SBARINFO Extension

The drawing command is `drawmugshot`. BiasedDoom accepts an optional width and height after its coordinates:

```c
drawmugshot ["<default face>"], <accuracy>, [<flags>,] <x>, <y> [, <width>, <height>];
```

The stock Doom status bar remains valid:

```c
drawmugshot "STF", 5, 143, 168;
```

A high-resolution replacement can be constrained to the classic slot before the player's transform is applied:

```c
drawmugshot "STF", 5, 143, 168, 24, 29;
```

Width and height are independent forced display dimensions. Omitting both uses the active face texture's display size. A non-positive dimension falls back to the texture dimension for that axis.

Mugshot-state declarations can also provide position and target-size overrides. When a matching state supplies them, its positive width/height and coordinates take precedence over the values on `drawmugshot`; the player transform is applied afterward.

## High-resolution skin faces

An `S_SKIN` definition can use `facescale` to decouple the source portrait
resolution from its logical HUD dimensions:

```
face = GVF
facescale = 0.125
faceoverlay = true
```

This example makes a 280×248 face texture occupy the same 35×31 logical
footprint as Doom's conventional face graphics. The renderer retains the
280×248 pixels, so it is sharper instead of being pre-downscaled. With
`faceoverlay = true`, the stock Doom fullscreen HUD also shows it in a
reserved lower-left 112×99 portrait area above health and armor. The player
can toggle that area with `hud_fullscreen_mugshot` and adjust it within its
non-overlapping range with `hud_fullscreen_mugshot_scale`. Give each
skin its own face prefix: the logical dimensions are stored on the resolved
texture and are therefore shared by that texture name.

## Rendering Order

The legacy path resolves the portrait in this order:

1. Obtain the current face for the player, skin, health, and mugshot state.
2. Start with target dimensions and coordinates from `drawmugshot`.
3. Apply positive dimensions and coordinates from the active or matching mugshot-state override.
4. Fall back independently to the texture's display width or height where no positive target exists.
5. Apply the player's uniform scale and offsets.
6. Draw the selected texture at the resulting forced size.

This ordering lets a mod define its intended layout while still allowing each player to compensate for a portrait pack or screen setup.

## Implementation Map

| Path | Responsibility |
|---|---|
| `src/g_statusbar/shared_sbar.cpp` | CVar definitions, persistence, and clamping |
| `src/g_statusbar/sbarinfo_commands.cpp` | `drawmugshot` parsing, state override resolution, and final transform |
| `wadsrc/static/zscript/ui/statusbar/doom_sbar.zs` | Stock Doom ZScript status-bar transform |
| `wadsrc/static/menudef.txt` | Mugshot options menu |
| `wadsrc/static/language.csv` | Menu labels and descriptions |

## Compatibility

- Existing SBARINFO without width and height keeps its original base-size behavior at the default player settings.
- The target-size extension is optional and does not require redefining every face texture in `TEXTURES`.
- Player settings are global and archived, so they persist across restarts and mod combinations.
- This feature changes presentation only; mugshot state selection and animation timing remain compatible with the inherited GZDoom behavior.
