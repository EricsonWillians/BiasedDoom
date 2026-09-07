# ImGui Overlays

A persistent **Dear ImGui control panel** driven entirely by Python: health
progress bar, a spawn-distance slider, a button that spawns an imp in front
of you, a kill counter fed by `actor_died`, and a live plot of your recent
health. A screen-top main menu bar toggles the panel and the stock ImGui
demo window.

## What it teaches

- The `imgui_frame` event: your handler runs once per rendered frame, and
  every `bd.imgui` call is only legal inside it (else `RuntimeError`)
- Immediate-mode state ownership: `begin()` with `open=` returns
  `(expanded, open)`; assign `open` back to your own variable so the
  window's close button and the menu toggle stay in sync
- Widgets return `(changed, value)` tuples — keep the value in a module
  global and pass it back in next frame
- Mixing overlay UI with gameplay: the spawn button calls `bd.spawn`
  directly, and `actor_died` keeps the counter updated between frames

## Running it

```bash
tools/play-python-example.py     # choose 26_imgui_overlays
```

Or directly:

```bash
./build/biaseddoom -python -iwad ~/games/doom2.wad \
    -file examples/python/26_imgui_overlays +map map01
```

## The `py_imgui` CVar

The whole overlay is gated by the **`py_imgui`** CVar (default `true`):
`py_imgui false` hides every ImGui window (window state persists while
hidden). Scripts can flip it with `bd.imgui.set_master_visible(...)` from any
event — this example does so at startup, since an ImGui demo is pointless
with the overlay hidden. Note that the CVar is archived, so the change
survives a normal exit. The **`py_imgui_demo`** console command toggles the
stock Dear ImGui demo window, handy for exploring what the widgets look like.

While the overlay wants the mouse or keyboard, the engine routes `EV_GUI_*`
events to it instead of the game, so clicking a slider does not fire your
weapon. `bd.imgui.want_capture_mouse()` / `want_capture_keyboard()` expose
that state to scripts.
