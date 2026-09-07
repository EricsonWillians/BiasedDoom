"""Overture Menu Kit - UI: every window, drawn from one imgui_frame handler.

Immediate-mode ImGui submitted from the single ``imgui_frame`` handler at
the bottom; headless safety is structural (the event never fires under
``-headless`` or ``-scripttest``, and this module only defines functions at
import). The guard discipline mirrors the sibling fixtures: one ``bd.warn``
per frame at worst and a balanced begin/end even when a draw raises.

- **Title menu** (title screen or forced for the screenshot pose):
  vertically stacked buttons centered with ``calc_text_size``/cursor math,
  the model's selection pinned with ``set_item_default_focus``, digit
  hotkeys 1-9, Up/Down/Enter navigation, footer hints.
- **Pause menu**: dims the game with a full-screen window, then the same
  stacked-button treatment. Custom Action 4 (primary) closes a popup
  first, then unwinds the stack (see systems.request_menu_toggle for the
  debounce); the Esc poll runs only when Custom Action 4 is unbound.
- **Settings**: UI scale, font family (registry combo), font size (applied
  per window through ``push_font`` + ``set_window_font_scale``), theme,
  accent (persistent ``set_style_color`` on Button/ButtonHovered/FrameBg),
  opacity (per-window background alpha), Reset and Back. Everything applies
  live through systems.set_setting and persists via bd.state.
- **Help popup** (F1) and **confirm-quit popup** (No default-focused) use
  the open_popup/begin_popup contract.
- **Credits**: lines scrolling upward by ``bd.level_time`` via
  ``set_cursor_pos``.
- **Tool panel** (F2): a viewport dock space hosting an event-log window
  and a monster monitor (player health reading + live fixture bars), all
  FirstUseEver-placed so the windows stay user-movable and resizable.
- **Mute toast** (Ctrl+M chord through ``shortcut``): a few seconds of
  bottom-center confirmation.

Manifest entry 3 of 4.
"""

import os

import biaseddoom as bd

try:
    import overture_content as content
    import overture_systems as systems
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="overture_content")
    systems = bd.import_script("pyscripts/systems.py",
                               module_name="overture_systems")

_AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"

_SELECT_COLOR = (0.95, 0.80, 0.35, 1.0)
_HINT_COLOR = (0.75, 0.75, 0.75, 1.0)

_frame_count = 0


# --- helpers --------------------------------------------------------------------------


def _apply_font(imgui):
    """Push the configured font, scaled toward the configured size.

    Returns True when a font was pushed (caller must pop_font()).
    """
    name = str(systems.settings.get("font_name"))
    if name == "Default":
        return False
    size = float(systems.settings.get("font_size"))
    base = None
    try:
        for entry in imgui.list_fonts():
            if entry[0] == name:
                base = float(entry[1])
                break
    except Exception:
        return False
    if not base:
        return False
    imgui.push_font(name)
    imgui.set_window_font_scale(size / base)
    return True


def _popup_open(imgui):
    return (imgui.is_popup_open(content.HELP_POPUP_ID)
            or imgui.is_popup_open(content.CONFIRM_QUIT_POPUP_ID))


# --- hotkeys ----------------------------------------------------------------------------


def _hotkeys(imgui):
    """Frame-gated keyboard: chords, function keys, Esc, menu navigation."""
    if imgui.shortcut(imgui.Key.M, imgui.Mod.Ctrl):
        systems.toggle_mute()
    if imgui.is_key_pressed(imgui.Key.F2):
        systems.tool_panel_visible = not systems.tool_panel_visible
    if imgui.is_key_pressed(imgui.Key.F1):
        systems.request_help()

    popups_open = _popup_open(imgui)
    if systems.esc_fallback_active() \
            and imgui.is_key_pressed(imgui.Key.Escape):
        if popups_open:
            # A popup outranks the stack: Esc closes it first. Cancel the
            # quit handshake when its popup is the one closing.
            if imgui.is_popup_open(content.CONFIRM_QUIT_POPUP_ID):
                systems.cancel_quit()
            imgui.close_current_popup()
        else:
            systems.request_menu_toggle()
    if popups_open:
        return

    screen_id = systems.active_screen()
    items = content.MENU_ITEMS.get(screen_id)
    if not items:
        return
    if imgui.is_key_pressed(imgui.Key.Down, True):
        systems.model.move_selection(1, len(items), screen_id)
    if imgui.is_key_pressed(imgui.Key.Up, True):
        systems.model.move_selection(-1, len(items), screen_id)
    if imgui.is_key_pressed(imgui.Key.Enter) \
            or imgui.is_key_pressed(imgui.Key.KeyPadEnter):
        systems.model.activate_selected(items, screen_id)
    for number, digit in enumerate(content.HOTKEY_DIGITS[:len(items)],
                                   start=1):
        if imgui.is_key_pressed(getattr(imgui.Key, digit)):
            systems.model.set_selection(number - 1, len(items), screen_id)
            systems.model.activate_selected(items, screen_id)
            break


# --- the dim layer ------------------------------------------------------------------------


def _draw_dim(imgui):
    screen_id = systems.active_screen()
    if screen_id is None or not systems.in_map():
        return
    imgui.set_next_window_pos(0.0, 0.0, imgui.Cond.Always)
    imgui.set_next_window_size(100000.0, 100000.0, imgui.Cond.Always)
    imgui.set_next_window_bg_alpha(content.DIM_ALPHA)
    flags = (imgui.WindowFlags.NoTitleBar | imgui.WindowFlags.NoResize
             | imgui.WindowFlags.NoMove | imgui.WindowFlags.NoScrollbar)
    expanded = imgui.begin("###overture_dim", flags=flags)
    try:
        if expanded:
            imgui.text_disabled("")
    finally:
        imgui.end()


# --- menu screens ---------------------------------------------------------------------------


def _draw_menu_window(imgui, screen_id, title, pos):
    """The shared stacked-button treatment for the title and pause menus."""
    items = content.MENU_ITEMS[screen_id]
    selected = systems.model.selection(screen_id)
    imgui.set_next_window_pos(pos[0], pos[1], imgui.Cond.FirstUseEver)
    imgui.set_next_window_size(content.MENU_WINDOW_WIDTH, 0.0,
                               imgui.Cond.FirstUseEver)
    imgui.set_next_window_bg_alpha(float(systems.settings.get("opacity")))
    expanded = imgui.begin(title)
    pushed = False
    try:
        if expanded:
            pushed = _apply_font(imgui)
            try:
                if screen_id == "title":
                    imgui.text_disabled(content.FOOTER_TITLE_HINT)
                    imgui.spacing()
                win_w, _h = imgui.get_window_size()
                for index, item in enumerate(items):
                    label = f"{index + 1}.  {item['label']}"
                    _cx, cy = imgui.get_cursor_pos()
                    imgui.set_cursor_pos(
                        max(0.0, (win_w - content.MENU_BUTTON_WIDTH) * 0.5),
                        cy)
                    if imgui.button(label, content.MENU_BUTTON_WIDTH,
                                    content.MENU_BUTTON_HEIGHT):
                        systems.model.set_selection(index, len(items),
                                                    screen_id)
                        systems.model.activate_selected(items, screen_id)
                    if index == selected:
                        # The model's selection owns keyboard focus.
                        imgui.set_item_default_focus()
                        imgui.same_line()
                        imgui.text_colored(*_SELECT_COLOR, "<-")
                imgui.spacing()
                imgui.separator()
                imgui.text_disabled(systems.menu_toggle_hint())
                imgui.text_disabled(
                    content.FOOTER_DIGIT_HINT % min(9, len(items)))
            finally:
                if pushed:
                    imgui.pop_font()
    finally:
        imgui.end()


def _draw_settings(imgui):
    settings = systems.settings
    marker = content.SETTINGS_DIRTY_MARKER if settings.is_dirty() else ""
    imgui.set_next_window_pos(content.SETTINGS_POS[0], content.SETTINGS_POS[1],
                              imgui.Cond.FirstUseEver)
    imgui.set_next_window_size(content.MENU_WINDOW_WIDTH + 60.0, 0.0,
                               imgui.Cond.FirstUseEver)
    imgui.set_next_window_bg_alpha(float(settings.get("opacity")))
    expanded = imgui.begin(content.SETTINGS_TITLE + marker)
    pushed = False
    try:
        if expanded:
            pushed = _apply_font(imgui)
            try:
                changed, value = imgui.slider_float(
                    "UI scale", float(settings.get("ui_scale")), 0.75, 2.0,
                    format="%.2f")
                if changed:
                    systems.set_setting("ui_scale", value)

                names = systems.known_font_names()
                current = str(settings.get("font_name"))
                index = names.index(current) if current in names else 0
                changed, index = imgui.combo("Font family", index, names)
                if changed and 0 <= index < len(names):
                    systems.set_setting("font_name", names[index])

                changed, value = imgui.slider_float(
                    "Font size", float(settings.get("font_size")), 15.0,
                    28.0, format="%.0f")
                if changed:
                    systems.set_setting("font_size", value)

                themes = list(content.THEMES)
                theme = str(settings.get("theme"))
                index = themes.index(theme) if theme in themes else 0
                changed, index = imgui.combo("Theme", index, themes)
                if changed and 0 <= index < len(themes):
                    systems.set_setting("theme", themes[index])

                accent = settings.get("accent")
                changed, r, g, b, a = imgui.color_edit4(
                    "Accent", accent[0], accent[1], accent[2], accent[3])
                if changed:
                    systems.set_setting("accent", (r, g, b, a))

                changed, value = imgui.slider_float(
                    "Opacity", float(settings.get("opacity")), 0.30, 1.0,
                    format="%.2f")
                if changed:
                    systems.set_setting("opacity", value)

                imgui.separator()
                if imgui.button(content.SETTINGS_RESET_LABEL):
                    systems.reset_settings()
                imgui.same_line()
                if imgui.button(content.SETTINGS_BACK_LABEL):
                    systems.model.pop()
                imgui.text_disabled("Live-applied; persisted via bd.state.")
            finally:
                if pushed:
                    imgui.pop_font()
    finally:
        imgui.end()


def _draw_credits(imgui):
    imgui.set_next_window_pos(content.CREDITS_POS[0], content.CREDITS_POS[1],
                              imgui.Cond.FirstUseEver)
    imgui.set_next_window_size(560.0, 380.0, imgui.Cond.FirstUseEver)
    imgui.set_next_window_bg_alpha(float(systems.settings.get("opacity")))
    expanded = imgui.begin(content.CREDITS_TITLE)
    pushed = False
    try:
        if expanded:
            pushed = _apply_font(imgui)
            try:
                line_height = imgui.get_font_size() + 4.0
                total = len(content.CREDITS_LINES) * line_height
                window_h = 340.0
                span = window_h + total
                # Scroll upward forever by level time (tics -> seconds).
                clock = float(bd.level_time()) / 35.0
                y = window_h - (clock * content.CREDITS_SCROLL_SPEED) % span
                imgui.set_cursor_pos(20.0, y)
                for line in content.CREDITS_LINES:
                    imgui.text(line)
            finally:
                if pushed:
                    imgui.pop_font()
    finally:
        imgui.end()


def _draw_screens(imgui):
    screen_id = systems.active_screen()
    if screen_id == "title":
        _draw_menu_window(imgui, "title", content.TITLE_MENU_TITLE,
                          content.TITLE_MENU_POS)
    elif screen_id == "pause":
        _draw_menu_window(imgui, "pause", content.PAUSE_MENU_TITLE,
                          content.PAUSE_MENU_POS)
    elif screen_id == "settings":
        _draw_settings(imgui)
    elif screen_id == "credits":
        _draw_credits(imgui)


# --- popups ---------------------------------------------------------------------------------


def _draw_help_popup(imgui):
    if systems.help_popup_requested:
        systems.help_popup_requested = False
        imgui.open_popup(content.HELP_POPUP_ID)
    if not imgui.begin_popup(content.HELP_POPUP_ID):
        return
    try:
        imgui.text(content.HELP_TITLE)
        imgui.separator()
        if imgui.begin_child("###overture_help_body",
                             (content.HELP_POPUP_WIDTH - 40.0, 260.0),
                             border=True):
            try:
                for heading, body in content.HELP_TOPICS:
                    imgui.text_colored(*_SELECT_COLOR, heading)
                    imgui.text_wrapped(body)
                    imgui.spacing()
            finally:
                imgui.end_child()
        else:
            imgui.end_child()
        if imgui.button("Close###overture_help_close"):
            imgui.close_current_popup()
    finally:
        imgui.end_popup()


def _draw_confirm_quit(imgui):
    if systems.quit_confirm_requested:
        systems.quit_confirm_requested = False
        imgui.open_popup(content.CONFIRM_QUIT_POPUP_ID)
    if not imgui.begin_popup(content.CONFIRM_QUIT_POPUP_ID):
        return
    try:
        imgui.text_wrapped(content.CONFIRM_QUIT_TEXT)
        imgui.separator()
        if imgui.button(content.CONFIRM_QUIT_YES):
            systems.confirm_quit(quit_engine=not _AUTOTEST)
            imgui.close_current_popup()
        imgui.same_line()
        if imgui.button(content.CONFIRM_QUIT_NO):
            systems.cancel_quit()
            imgui.close_current_popup()
        # "No" is the safe default: focus it on the popup's first frame.
        imgui.set_item_default_focus()
    finally:
        imgui.end_popup()


def _draw_popups(imgui):
    _draw_help_popup(imgui)
    _draw_confirm_quit(imgui)


# --- the docked tool panel --------------------------------------------------------------------


def _draw_tool_panel(imgui):
    if not systems.tool_panel_visible:
        return
    dock_id = 0
    try:
        dock_id = imgui.dock_space_over_viewport()
    except Exception:
        dock_id = 0  # docking unavailable: the windows just float

    if dock_id:
        imgui.set_next_window_dock_id(dock_id, imgui.Cond.FirstUseEver)
    imgui.set_next_window_pos(content.TOOL_LOG_POS[0],
                              content.TOOL_LOG_POS[1],
                              imgui.Cond.FirstUseEver)
    imgui.set_next_window_size(content.TOOL_LOG_SIZE[0],
                               content.TOOL_LOG_SIZE[1],
                               imgui.Cond.FirstUseEver)
    expanded, log_open = imgui.begin(content.TOOL_LOG_TITLE, True)
    try:
        if not log_open:
            systems.tool_panel_visible = False
        if expanded:
            imgui.text_disabled("fed by engine events:")
            imgui.separator()
            for line in list(systems.event_log):
                imgui.text_wrapped(line)
    finally:
        imgui.end()

    if dock_id:
        imgui.set_next_window_dock_id(dock_id, imgui.Cond.FirstUseEver)
    imgui.set_next_window_pos(content.TOOL_MONSTER_POS[0],
                              content.TOOL_MONSTER_POS[1],
                              imgui.Cond.FirstUseEver)
    imgui.set_next_window_size(content.TOOL_MONSTER_SIZE[0],
                               content.TOOL_MONSTER_SIZE[1],
                               imgui.Cond.FirstUseEver)
    expanded, monitor_open = imgui.begin(content.TOOL_MONSTER_TITLE, True)
    try:
        if not monitor_open:
            systems.tool_panel_visible = False
        if expanded:
            health = systems.player_health()
            imgui.text("Player")
            if health is None:
                imgui.progress_bar(0.0, w=-1, h=0, overlay="no pawn")
            else:
                fraction = max(0.0, min(1.0, health / 100.0))
                imgui.progress_bar(fraction, w=-1, h=0,
                                   overlay=f"{health} HP")
            imgui.separator()
            live = []
            for ref in systems.fixture_monsters:
                try:
                    if ref is not None and ref.valid and ref.alive:
                        live.append(ref)
                except RuntimeError:
                    continue
            imgui.text(f"Fixture monsters: {len(live)}")
            fields = []
            if live:
                try:
                    fields = bd.actor_field_batch(live, ["health",
                                                         "class_name"])
                except Exception:
                    fields = []
            for ref, field in zip(live, fields):
                hp, class_name = field[0], field[1]
                hp = int(hp) if hp is not None else 0
                class_name = class_name or "monster"
                fraction = max(0.0, min(1.0, hp / 100.0))
                imgui.progress_bar(fraction, w=-1, h=0,
                                   overlay=f"{class_name}: {hp} HP")
    finally:
        imgui.end()


# --- the mute toast -----------------------------------------------------------------------------


def _draw_toast(imgui):
    clock = systems.safe_clock()
    if clock < 0.0 or not systems.mute_toast_text:
        return
    if clock - systems.mute_toast_clock > content.MUTE_TOAST_SECONDS:
        return
    imgui.set_next_window_pos(760.0, 880.0, imgui.Cond.FirstUseEver)
    flags = (imgui.WindowFlags.NoTitleBar | imgui.WindowFlags.NoResize
             | imgui.WindowFlags.AlwaysAutoResize)
    expanded = imgui.begin("###overture_toast", flags=flags)
    try:
        if expanded:
            imgui.text_colored(*_SELECT_COLOR, systems.mute_toast_text)
    finally:
        imgui.end()


# --- frame ----------------------------------------------------------------------------------------


@bd.on("imgui_frame")
def draw(event):
    global _frame_count
    _frame_count += 1
    imgui = bd.imgui
    for draw_fn in (_hotkeys, _draw_dim, _draw_screens, _draw_popups,
                    _draw_tool_panel, _draw_toast):
        try:
            draw_fn(imgui)
        except Exception as exc:
            # One warn per frame at worst; the window stack stays balanced.
            bd.warn(f"overture ui draw error in {draw_fn.__name__}: "
                    f"{exc!r}")


# --- sibling-import registration ----------------------------------------------------

import sys as _sys
import types as _types


class _LiveAlias(_types.ModuleType):
    """sys.modules alias that reads/writes through to the module's live
    globals (the engine registers manifest modules under mangled names only
    after execution, so the real module object cannot be aliased from inside
    itself)."""

    def __init__(self, name, namespace):
        super().__init__(name)
        object.__setattr__(self, "_bd_live_ns", namespace)

    def __getattr__(self, key):
        try:
            return object.__getattribute__(self, "_bd_live_ns")[key]
        except KeyError:
            raise AttributeError(key)

    def __setattr__(self, key, value):
        object.__getattribute__(self, "_bd_live_ns")[key] = value


_sys.modules.setdefault("overture_ui", _LiveAlias("overture_ui", globals()))
del _sys, _types
