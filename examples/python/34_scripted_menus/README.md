# Overture Menu Kit

A complete, keyboard-first menu suite scripted entirely in Python on top of
the expanded **`bd.imgui`** API: a title menu, a pause menu that dims the
game, a live settings screen, a scrolling credits screen, help and
confirm-quit popups, and a docked tool panel with an event log and a
monster monitor. Every widget redraws itself each frame from plain Python
state; the engine's native menus are untouched.

## Architecture

Four modules, all listed in the root `PYTHON` manifest in dependency order.
Each sibling self-registers a stable `sys.modules` alias (a live proxy), so
later entries reach it with a plain `import`, the same sibling-import
mechanics `hello_world` gets via `bd.import_script`.

| Module | Role |
|---|---|
| `pyscripts/content.py` | **Pure data**: the title/pause menu item tables, the settings schema (defaults, ranges, choices), the font catalog (NotoSans at 15/18/24 from `read_bytes`, vector default at 13/16/20), the theme list, accent constants, key bindings, popup ids, help topics, credits text, screen layout constants, and every UI string. No engine calls at import. |
| `pyscripts/systems.py` | **Rules and engine hooks**: the pure-Python `MenuModel` (screen stack push/pop/replace/top, wrapping selection, activation dispatch through a wired callback) and the schema-driven `Settings` (clamping, dirty flag, reset, snapshot/restore). `engine_start` registers the fonts from `bd.read_bytes` bytes, restores the settings from `bd.state`, applies theme/accent/scale, and arms `bind esc menu_toggle` through the `pyui`/`ui_command` bridge; `map_load` spawns three calm fixture monsters and takes a health reading; `save`/`load` persist the settings snapshot. The Esc frame poll and the console alias collapse through a per-tic debounce. |
| `pyscripts/ui.py` | **The interface**: centered stacked buttons (`calc_text_size` cursor math, `set_item_default_focus` on the model's selection, digit hotkeys 1-9, Up/Down/Enter), the full-screen dim layer, the settings screen (UI scale, font family combo from `list_fonts`, font size via `push_font` + `set_window_font_scale`, theme combo, `color_edit4` accent persisted with `set_style_color`, opacity via `set_next_window_bg_alpha`, Reset/Back), the help and confirm-quit popups (`open_popup`/`begin_popup`, No default-focused), level-time credits scrolling, the F2 docked tool panel (`dock_space_over_viewport` + two `FirstUseEver` windows), and the Ctrl+M mute toast. All inert headless; one `bd.warn` per frame at worst, balanced begin/end. |
| `pyscripts/main.py` | **Thin bootstrap**: the `BD_EXAMPLE_AUTOTEST=1` schedule (model, settings, persistence, read_bytes, font registry, UI scale, style accessors, the console bridge, the fixture) and the `BD_EXAMPLE_SCREENSHOT=1` poses. |

## What it teaches

- **Menus as pure state.** `MenuModel` knows screens and selection indexes
  but nothing about ImGui; the UI renders the model and forwards clicks and
  keys. Activation is one wired callback, so headless tests dispatch items
  without a frame.
- **The expanded font registry.** `add_font_ttf` (from `bd.read_bytes`
  bytes), `add_font_default`, duplicate rejection, `remove_font` /
  `clear_fonts`, `list_fonts`, and the `set_default_font` /
  `get_default_font` round trip. Fonts are any-event calls, legal from
  `engine_start`.
- **Scaling and style as any-event calls.** `set_ui_scale` (clamped 0.5 to
  4.0, compositional), `style_theme` (scale-preserving), `set_style_color`
  round trips, and `set_style_var`. The style accessors need one rendered
  frame: headless they raise the documented `RuntimeError` ("no frame has
  run yet"), which the autotest asserts as the contract.
- **Keyboard-first input.** `is_key_pressed` for Esc/digits/arrows/Enter,
  `shortcut(Key.M, Mod.Ctrl)` for the mute chord, and the `pyui`
  `ui_command` bridge (`bind esc menu_toggle`) as a console-level fallback;
  both Esc paths debounce per tic so one press is one transition.
- **Popups and focus.** `open_popup` on request, `begin_popup` guards,
  `close_current_popup` with popup priority over the screen stack, and
  `set_item_default_focus` pinning the safe default (No on quit).
- **Docking.** `dock_space_over_viewport` plus `set_next_window_dock_id`
  with `Cond.FirstUseEver` suggests an initial layout while keeping every
  window user-movable and resizable.
- **Persistence.** Settings snapshot into `bd.state` under a namespaced key
  on `save` and restore on `load`; the autotest drives the handlers
  directly for the round trip.

## A note on the font payload

`bd.read_bytes` is container-scoped: it only sees lumps inside the example's
own PK3. The engine ships the same NotoSans-Regular.ttf face at
`widgets/noto/NotoSans-Regular.ttf`, but a mod script cannot read across
containers, so this example carries its own copy under `fonts/` (the
packaging script includes a top-level `fonts/` directory automatically).

## Running it

```bash
./build/biaseddoom -iwad /path/to/DOOM2.WAD \
    -file examples/python/34_scripted_menus \
    -python -stdout +map MAP01
```

At the title screen the main menu appears on its own; in a map, **Esc**
opens the pause menu (closing any popup first). **1-9** activate items,
**Up/Down** move, **Enter** confirms, **F1** opens help, **F2** docks the
tool panel, **Ctrl+M** mutes. The settings screen applies everything live
and persists it into savegames.

Headless autotest (deterministic, no frames rendered):

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad /path/to/DOOM2.WAD -file examples/python/34_scripted_menus \
    -python -stdout -nosound -nointro +map map01 -scripttest 400 ff
# -> SCRIPT TEST: PASS
```

The autotest asserts: the MenuModel transitions and wrapping selection;
settings defaults/clamping/dirty/reset; the `bd.state` persistence round
trip; the `read_bytes` font contract; the font registry semantics
(duplicates, removal, listing, default round trip, clear); the UI scale
clamp; the style accessors (round trip rendered, documented RuntimeError
headless); the `pyui`/`ui_command` bridge with the per-tic toggle
debounce; the quit and mute flows; and the spawned fixture.

Documentation captures (three poses under a display server):

```bash
BD_EXAMPLE_SCREENSHOT=1 xvfb-run -a ./build/biaseddoom \
    -iwad /path/to/DOOM2.WAD -file examples/python/34_scripted_menus \
    -python -nosound +map MAP01
# -> /tmp/overture_1.png (title menu), /tmp/overture_2.png (pause + settings),
#    /tmp/overture_3.png (help popup)
```

## Expanding it

- **New screens**: add items to `MENU_ITEMS` (or a new table) in
  `content.py`, one behavior branch in `systems._on_menu_activate`, and one
  draw branch in `ui._draw_screens`; register the id in `SCREEN_IDS`.
- **New settings**: one schema entry in `content.SETTINGS_SCHEMA`; the
  settings screen renders sliders/combos from the schema and `set_setting`
  applies the engine side effect.
- **New popups**: an id in `content.py`, request/open/begin/end in the
  `_draw_popups` pattern, and a line in `_popup_open` so Esc closes it
  first.
- **Re-skin**: every string, color, and layout number lives in
  `content.py`; no logic lives there.
