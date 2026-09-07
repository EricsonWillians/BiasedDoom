"""Dear ImGui overlays: a control panel window plus a main menu bar.

Teaches the ``bd.imgui`` submodule: an ``imgui_frame`` handler that submits
widgets every rendered frame, a persistent window whose visibility is driven
by its own close button and a main-menu-bar toggle, a health progress bar
and line plot fed by the live player, a slider bound to script state, a
button that spawns an imp via ``bd.spawn``, and a collapsing header with a
kill counter fed by the ``actor_died`` event.

The overlay is gated by the ``py_imgui`` CVar (on by default); the
``py_imgui_demo`` console command opens the stock Dear ImGui demo window.
"""

import math

import biaseddoom as bd

imgui = bd.imgui

# --- tunables ---------------------------------------------------------------

HISTORY_LENGTH = 120     # samples kept in the health plot

# --- module state -----------------------------------------------------------

panel_visible = True
demo_open = False
spawn_distance = 96.0    # slider-bound, in map units
kill_count = 0
health_history = []
autowarp_done = False


def player_body():
    """Live handle to the local player's body, or None."""
    try:
        player = bd.player(0)
        if player is None or not player.valid:
            return None
        body = player.actor
        return body if body is not None and body.valid else None
    except RuntimeError:
        return None


def spawn_imp():
    """Spawn a DoomImp spawn_distance units in front of the player."""
    body = player_body()
    if body is None:
        return
    angle = math.radians(body.angle)
    x = body.x + math.cos(angle) * spawn_distance
    y = body.y + math.sin(angle) * spawn_distance
    try:
        imp = bd.spawn("DoomImp", x, y, body.z, angle=(body.angle + 180.0) % 360.0)
        if imp is not None and imp.valid:
            bd.play_ui_sound("imp/sight", volume=0.5)
    except RuntimeError:
        pass  # world mutation unavailable (e.g. multiplayer observer)


# --- events -------------------------------------------------------------------

@bd.on("engine_start")
def engine_started(event):
    # This example demonstrates the overlay, so make sure it is not hidden by
    # a saved py_imgui=false. set_master_visible flips that very CVar and is
    # one of the two bd.imgui calls legal outside imgui_frame.
    imgui.set_master_visible(True)


@bd.on("actor_died")
def count_kill(event):
    global kill_count
    ref = event.get("actor_ref")
    try:
        if ref is not None and ref.valid and ref.is_monster:
            kill_count += 1
    except RuntimeError:
        pass


@bd.on("map_load")
def reset_state(event):
    global kill_count
    kill_count = 0
    del health_history[:]


# --- overlay --------------------------------------------------------------------

@bd.on("imgui_frame")
def draw_overlay(event):
    global panel_visible, demo_open, spawn_distance, autowarp_done

    # imgui_frame fires at the title screen too, before any level exists.
    # Headless runs (-scripttest) launch without +map, so queue a warp on the
    # first rendered frame when nothing loaded a level yet. When the user
    # passed +map, current_map() is already set and nothing happens.
    if not autowarp_done:
        autowarp_done = True
        try:
            if not bd.current_map():
                bd.execute("map map01")
        except RuntimeError:
            pass

    # (b) Screen-top main menu bar with toggles.
    if imgui.begin_main_menu_bar():
        if imgui.begin_menu("BiasedDoom"):
            if imgui.menu_item("Control Panel", selected=panel_visible):
                panel_visible = not panel_visible
            if imgui.menu_item("ImGui Demo", selected=demo_open):
                demo_open = not demo_open
            imgui.end_menu()
        imgui.end_main_menu_bar()

    if demo_open:
        demo_open = imgui.show_demo_window(demo_open)

    if not panel_visible:
        return

    # (a) Persistent control panel window.
    imgui.set_next_window_pos(24, 40, imgui.Cond.FirstUseEver)
    imgui.set_next_window_size(360, 0, imgui.Cond.FirstUseEver)
    # Passing our bool as open= gives the window a close button and returns
    # (expanded, open); assigning open back keeps the close button working.
    expanded, panel_visible = imgui.begin("BiasedDoom ImGui", panel_visible)
    if expanded:
        imgui.text("Dear ImGui overlay, driven by Python")
        imgui.separator()

        body = player_body()
        health = None
        try:
            if body is not None and body.alive:
                health = body.health
        except RuntimeError:
            health = None

        if health is None:
            imgui.text_disabled("No active player")
        else:
            health_history.append(health)
            if len(health_history) > HISTORY_LENGTH:
                del health_history[:-HISTORY_LENGTH]
            fraction = max(0.0, min(1.0, health / 100.0))
            imgui.text("Health")
            imgui.progress_bar(fraction, w=-1, h=14, overlay=f"{health} HP")

        imgui.separator()
        changed, spawn_distance = imgui.slider_float(
            "Spawn distance", spawn_distance, 32.0, 256.0, format="%.0f units")
        if imgui.button("Spawn Imp"):
            spawn_imp()
        if imgui.is_item_hovered():
            imgui.set_tooltip("Spawn a DoomImp in front of you")

        if imgui.collapsing_header("Stats"):
            # image() accepts a lump name (sprite names like "PISGA0" work
            # via the sprite-namespace fallback) or an Actor handle, which
            # draws the actor's current sprite frame. w/h = 0 uses the
            # texture's natural display size.
            try:
                imgui.image("PISGA0", tint=(1.0, 1.0, 1.0, 1.0),
                            border=(0.6, 0.6, 0.6, 1.0))
                if body is not None and body.valid:
                    imgui.same_line()
                    imgui.image(body)
                if imgui.is_item_hovered():
                    imgui.set_tooltip("Live sprite of the player actor")
            except ValueError:
                pass  # texture missing in a non-DOOM game
            imgui.bullet_text(f"Monsters killed: {kill_count}")
            imgui.bullet_text(f"Map: {event.get('map', '?')}")

        if health_history:
            imgui.plot_lines("Health history", health_history,
                             scale_min=0.0, scale_max=100.0, w=0, h=48)
    imgui.end()
