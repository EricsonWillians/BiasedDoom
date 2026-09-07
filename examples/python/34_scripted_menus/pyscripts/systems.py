"""Overture Menu Kit - systems: the menu model, settings, and engine hooks.

Owns the behavior of the suite:

- **MenuModel.** A pure-Python screen stack (push/pop/replace/top) with a
  remembered selection index per screen. Activation dispatches through the
  ``on_activate`` callback the systems layer wires; no imgui imports here, so
  the model is unit-testable headless.
- **Settings.** Schema-driven state (ui_scale, font_name, font_size, theme,
  accent rgba, opacity) with clamping, a dirty flag, reset, and snapshot /
  restore. Persisted under a namespaced ``bd.state`` key via the save/load
  events and applied live through the any-event ``bd.imgui`` accessors.
- **Event wiring.** ``engine_start`` registers the font catalog from
  ``bd.read_bytes`` bytes plus the vector default sizes, restores the
  settings, applies theme/accent/scale, and arms ``bind esc menu_toggle``
  through the ``pyui``/``ui_command`` bridge (the frame also polls
  ``is_key_pressed(Escape)``; both paths collapse through a per-tic
  debounce). ``map_load`` spawns three calm fixture monsters and takes a
  health reading so the pause menu and the monitor have something to show.
  ``ui_command`` accepts ``menu_toggle``, ``menu_open <screen>`` and
  ``menu_close`` from the console.
- **Flow flags.** Mute toggle state with a toast clock, the quit-requested
  handshake (request, cancel, confirm), and the tool-panel visibility flag.

Manifest entry 2 of 4 (after content.py).
"""

from collections import deque

import biaseddoom as bd

try:
    import overture_content as content
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="overture_content")

imgui = bd.imgui


# --- the pure-Python menu model -------------------------------------------------------


class MenuModel:
    """Screen stack + per-screen selection. No imgui imports by design."""

    def __init__(self, screen_ids):
        self._stack = []
        self._selection = {str(screen_id): 0 for screen_id in screen_ids}
        self.on_activate = None  # wired by the systems layer

    # stack
    def push(self, screen_id):
        """Push a screen; pushing the current top again is a no-op."""
        screen_id = str(screen_id)
        if not self._stack or self._stack[-1] != screen_id:
            self._stack.append(screen_id)

    def pop(self):
        """Pop the top screen; returns it (None when the stack was empty)."""
        return self._stack.pop() if self._stack else None

    def replace_top(self, screen_id):
        """Replace the top screen (pushes when the stack is empty)."""
        screen_id = str(screen_id)
        if self._stack:
            self._stack[-1] = screen_id
        else:
            self._stack.append(screen_id)

    def clear(self):
        self._stack.clear()

    def top(self):
        return self._stack[-1] if self._stack else None

    @property
    def depth(self):
        return len(self._stack)

    def is_open(self):
        return bool(self._stack)

    # selection
    def selection(self, screen_id=None):
        """The remembered selection index (0 when unknown)."""
        sid = str(screen_id) if screen_id is not None else self.top()
        return self._selection.get(sid, 0)

    def set_selection(self, index, count, screen_id=None):
        """Clamp-wrap index into [0, count) and remember it. Returns it."""
        if count <= 0:
            return 0
        sid = str(screen_id) if screen_id is not None else self.top()
        index = int(index) % int(count)
        self._selection[sid] = index
        return index

    def move_selection(self, delta, count, screen_id=None):
        """Move the selection by delta, wrapping. Returns the new index."""
        sid = str(screen_id) if screen_id is not None else self.top()
        return self.set_selection(self.selection(sid) + int(delta), count,
                                  sid)

    def activate_selected(self, items, screen_id=None):
        """Resolve the selected item of ``items`` and dispatch it.

        Items may be dicts with an "id" key or plain ids. The wired
        ``on_activate(screen_id, item_id)`` runs when present. Returns the
        resolved item id (None when ``items`` is empty).
        """
        sid = str(screen_id) if screen_id is not None else self.top()
        if not items:
            return None
        index = self.selection(sid) % len(items)
        item = items[index]
        item_id = item.get("id") if isinstance(item, dict) else item
        if self.on_activate is not None:
            self.on_activate(sid, item_id)
        return item_id


# --- settings -------------------------------------------------------------------------


class Settings:
    """Schema-driven settings with clamping, a dirty flag and reset."""

    def __init__(self, schema=None):
        self._schema = schema if schema is not None else content.SETTINGS_SCHEMA
        self._values = {key: self._default(entry)
                        for key, entry in self._schema.items()}
        self._dirty = False

    @staticmethod
    def _default(entry):
        value = entry.get("default")
        return tuple(value) if isinstance(value, (list, tuple)) else value

    def keys(self):
        return tuple(self._values)

    def get(self, key):
        return self._values[key]

    def is_dirty(self):
        return self._dirty

    def mark_clean(self):
        self._dirty = False

    def set(self, key, value):
        """Validate + clamp ``value``, store it, flag dirty on change.

        Raises KeyError for an unknown key and ValueError for a value the
        schema cannot salvage.
        """
        if key not in self._schema:
            raise KeyError(f"unknown setting {key!r}")
        value = self._coerce(key, value)
        if value != self._values.get(key):
            self._values[key] = value
            self._dirty = True
        return value

    def _coerce(self, key, value):
        entry = self._schema[key]
        if key == "font_name":
            return str(value)
        if "choices" in entry:
            value = str(value)
            if value not in entry["choices"]:
                raise ValueError(
                    f"{value!r} is not one of {entry['choices']!r}")
            return value
        if key == "accent":
            rgba = tuple(float(c) for c in value)
            if len(rgba) != 4:
                raise ValueError("accent needs exactly four components")
            return tuple(max(0.0, min(1.0, c)) for c in rgba)
        try:
            value = float(value)
        except (TypeError, ValueError):
            raise ValueError(f"{value!r} is not a number") from None
        return max(float(entry["min"]), min(float(entry["max"]), value))

    def reset(self):
        """Restore every default; a fresh reset is not dirty."""
        self._values = {key: self._default(entry)
                        for key, entry in self._schema.items()}
        self._dirty = False

    def snapshot(self):
        """A JSON-safe copy of the values (accent as a list)."""
        result = {}
        for key, value in self._values.items():
            result[key] = list(value) if isinstance(value, tuple) else value
        return result

    def restore(self, data):
        """Coerce a snapshot (or any mapping) back in; clean afterwards.

        Unknown keys are ignored, invalid values raise ValueError so a
        corrupt save is loud instead of silent.
        """
        if not isinstance(data, dict):
            return
        for key in self._schema:
            if key in data:
                self.set(key, data[key])
        self._dirty = False


# --- module state ---------------------------------------------------------------------

model = MenuModel(content.SCREEN_IDS)
settings = Settings()
muted = False
mute_toast_clock = -1.0e9
mute_toast_text = ""
quit_requested = False
quit_confirm_requested = False
help_popup_requested = False
force_title_menu = False
tool_panel_visible = False
event_log = deque(maxlen=content.LOG_CAPACITY)
fixture_monsters = []
_menu_toggle_clock = -1
_engine_started = False


def safe_clock():
    """Animation clock in seconds: level time, or a frame fallback."""
    try:
        return float(bd.level_time()) / 35.0
    except RuntimeError:
        return -1.0


def in_map():
    try:
        return bd.current_map() is not None
    except RuntimeError:
        return False


def player_pawn():
    """Live handle to the local player's pawn, or None."""
    try:
        player = bd.player(0)
        if player is None or not player.valid:
            return None
        pawn = player.actor
        return pawn if pawn is not None and pawn.valid else None
    except RuntimeError:
        return None


def player_health():
    """The pawn's health (the fixture's 'health bar reading'), or None."""
    pawn = player_pawn()
    if pawn is None:
        return None
    try:
        return int(pawn.health)
    except Exception:
        return None


def active_screen():
    """The screen the UI should draw, or None when no menu is wanted."""
    top = model.top()
    if top is not None:
        return top
    if force_title_menu or not in_map():
        return "title"
    return None


def log_event(text):
    """Feed the tool panel log and the engine log."""
    event_log.append(str(text))
    try:
        bd.log(f"overture: {text}")
    except Exception:
        pass


# --- fonts and settings application ------------------------------------------------------


def register_fonts():
    """Register the font catalog (idempotent by registry name)."""
    try:
        existing = {name for name, _size, _bitmap, _built
                    in imgui.list_fonts()}
    except Exception:
        existing = set()
    data = None
    try:
        data = bd.read_bytes(content.NOTO_FONT_PATH)
    except Exception as exc:
        bd.warn(f"overture: could not read {content.NOTO_FONT_PATH}: {exc!r}")
    if data:
        for size in content.NOTO_FONT_SIZES:
            name = content.noto_font_name(size)
            if name not in existing:
                imgui.add_font_ttf(name, data, size)
    for size in content.DEFAULT_FONT_SIZES:
        name = content.default_font_name(size)
        if name not in existing:
            imgui.add_font_default(name, size)


def known_font_names():
    """Registry names plus the built-in 'Default' fallback."""
    try:
        names = [name for name, _size, _bitmap, _built in imgui.list_fonts()]
    except Exception:
        names = []
    if "Default" not in names:
        names.append("Default")
    return names


def apply_accent():
    """Recolor Button, ButtonHovered and FrameBg from the accent."""
    try:
        r, g, b, _a = settings.get("accent")
        hovered = tuple(min(1.0, c * content.ACCENT_HOVER_FACTOR
                            + content.ACCENT_HOVER_LIFT) for c in (r, g, b))
        shade = content.ACCENT_FRAME_SHADE
        imgui.set_style_color(imgui.Col.Button, r, g, b, 1.0)
        imgui.set_style_color(imgui.Col.ButtonHovered, hovered[0], hovered[1],
                              hovered[2], 1.0)
        imgui.set_style_color(imgui.Col.FrameBg, r * shade, g * shade,
                              b * shade, 1.0)
    except RuntimeError:
        pass  # headless: the overlay has no context yet


def apply_settings():
    """Push every setting into the engine (headless-safe)."""
    try:
        imgui.set_ui_scale(float(settings.get("ui_scale")))
    except Exception as exc:
        bd.warn(f"overture: set_ui_scale failed: {exc!r}")
    font_name = str(settings.get("font_name"))
    try:
        if font_name != "Default":
            imgui.set_default_font(font_name)
    except Exception as exc:
        bd.warn(f"overture: set_default_font failed: {exc!r}")
    try:
        imgui.style_theme(str(settings.get("theme")))
    except RuntimeError:
        pass  # headless: the overlay has no context yet
    apply_accent()


def set_setting(key, value):
    """Set one setting, apply its engine side effect, return the value."""
    applied = settings.set(key, value)
    if key == "ui_scale":
        imgui.set_ui_scale(float(applied))
    elif key == "font_name":
        if applied != "Default":
            imgui.set_default_font(applied)
    elif key == "theme":
        try:
            imgui.style_theme(str(applied))
        except RuntimeError:
            pass  # headless
    elif key == "accent":
        apply_accent()
    # font_size and opacity are read by the UI at draw time.
    return applied


def reset_settings():
    """Reset to defaults, apply them, clear the dirty flag."""
    settings.reset()
    apply_settings()


def persist_settings():
    """The save-handler body: snapshot into bd.state."""
    try:
        bd.state[content.SETTINGS_STATE_KEY] = settings.snapshot()
    except Exception as exc:
        bd.warn(f"overture: settings snapshot failed: {exc!r}")


def restore_settings():
    """The load-handler body: restore from bd.state when a snapshot exists."""
    try:
        data = bd.state.get(content.SETTINGS_STATE_KEY)
    except Exception as exc:
        bd.warn(f"overture: settings restore failed: {exc!r}")
        return
    if isinstance(data, dict):
        try:
            settings.restore(data)
        except ValueError as exc:
            bd.warn(f"overture: ignoring corrupt settings: {exc!r}")


# --- menu behavior -----------------------------------------------------------------------


def open_menu(screen_id):
    model.push(screen_id)


def close_menu():
    model.clear()


def request_menu_toggle():
    """Esc handling shared by the frame poll and the console alias.

    Both paths can fire for a single physical Esc press, so requests are
    debounced per animation tic. Popup closing is the UI's job (it owns the
    imgui popup scope); here we only manage the screen stack. Returns True
    when the request was acted on.
    """
    global _menu_toggle_clock
    clock = safe_clock()
    if clock >= 0.0 and clock == _menu_toggle_clock:
        return False
    _menu_toggle_clock = clock
    if model.is_open():
        model.pop()
    elif in_map():
        model.push("pause")
    else:
        model.push("title")
    return True


def request_help():
    global help_popup_requested
    help_popup_requested = True


def request_quit():
    global quit_confirm_requested
    quit_confirm_requested = True


def cancel_quit():
    global quit_requested, quit_confirm_requested
    quit_requested = False
    quit_confirm_requested = False


def confirm_quit(quit_engine=False):
    """The Yes branch of the confirm popup. Optionally exits the engine."""
    global quit_requested, quit_confirm_requested
    quit_requested = True
    quit_confirm_requested = False
    if quit_engine:
        try:
            bd.execute("quit")
        except Exception:
            pass


def toggle_mute():
    global muted, mute_toast_clock, mute_toast_text
    muted = not muted
    mute_toast_clock = safe_clock()
    mute_toast_text = content.TOAST_MUTED if muted else content.TOAST_UNMUTED
    log_event(content.MUTE_LOG_TEXT + (" (on)" if muted else " (off)"))


def _on_menu_activate(screen_id, item_id):
    """Route an activated item to its behavior."""
    if item_id == "new_game":
        close_menu()
        if not in_map():
            try:
                bd.execute("map map01")
            except Exception:
                pass
    elif item_id == "settings":
        model.push("settings")
    elif item_id == "credits":
        model.push("credits")
    elif item_id == "help":
        request_help()
    elif item_id == "resume":
        close_menu()
    elif item_id in ("quit", "quit_to_title"):
        request_quit()


model.on_activate = _on_menu_activate


# --- the map fixture ------------------------------------------------------------------------


def spawn_fixture():
    """Three calm monsters near the MAP01 yard line (probe-verified)."""
    global fixture_monsters
    fixture_monsters = []
    for index, spec in enumerate(content.FIXTURE_MONSTERS):
        try:
            ref = bd.spawn(spec["class_name"], *spec["pos"], angle=90.0,
                           tid=9600 + index, force=True)
            ref.set_flag("FRIENDLY", True)
            ref.set_flag("STANDSTILL", True)
            ref.speed = 0.0
            fixture_monsters.append(ref)
        except Exception as exc:
            bd.warn(f"overture: fixture spawn failed: {exc!r}")


# --- event wiring -----------------------------------------------------------------------------


@bd.on("engine_start")
def setup_example(event):
    global _engine_started
    if _engine_started:
        return
    _engine_started = True
    try:
        imgui.set_master_visible(True)
    except Exception:
        pass
    register_fonts()
    restore_settings()
    apply_settings()

    # Esc fallback: bind esc -> menu_toggle alias -> pyui -> ui_command.
    # The frame-level is_key_pressed(Escape) poll is primary; the debounce
    # in request_menu_toggle() collapses the two paths per tic.
    try:
        bd.execute('alias menu_toggle "pyui menu_toggle"')
        bd.execute("bind esc menu_toggle")
    except Exception as exc:
        bd.warn(f"overture: could not arm the Esc alias: {exc!r}")
    log_event("engine_start: fonts registered, settings applied")


@bd.on("save")
def on_save(event):
    persist_settings()


@bd.on("load")
def on_load(event):
    restore_settings()
    apply_settings()


@bd.on("ui_command")
def on_ui_command(event):
    command = str(event.get("command") or "")
    if command == "menu_toggle":
        request_menu_toggle()
    elif command == "menu_open_settings":
        model.push("settings")
    elif command == "menu_open_credits":
        model.push("credits")
    elif command == "menu_close":
        close_menu()
    # Unknown commands are ignored so other mods can share the bridge.


@bd.on("map_load")
def on_map(event):
    global fixture_monsters
    if event.get("from_savegame"):
        fixture_monsters = []
    else:
        spawn_fixture()
    health = player_health()
    log_event(f"map_load: {event.get('map', '?')} (player health "
              f"{health if health is not None else 'n/a'})")


@bd.on("map_unload")
def on_map_unload(event):
    global fixture_monsters
    fixture_monsters = []


@bd.on("actor_died")
def on_actor_died(event):
    ref = event.get("actor_ref")
    try:
        if ref is not None and ref.valid and ref.is_monster:
            log_event("actor_died: a monster went down")
    except RuntimeError:
        pass


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


_sys.modules.setdefault("overture_systems",
                        _LiveAlias("overture_systems", globals()))
del _sys, _types
