"""Overture Menu Kit - thin bootstrap, the headless autotest, screenshot poses.

Deliberately small: menu data lives in :mod:`overture_content`, the model /
settings / engine wiring in :mod:`overture_systems`, and every window in
:mod:`overture_ui`. What remains here is the deterministic autotest
(``BD_EXAMPLE_AUTOTEST=1``) and the documentation screenshot poses
(``BD_EXAMPLE_SCREENSHOT=1``).

The autotest drives the real engine paths headlessly and asserts, in order:
the pure MenuModel (push/pop/replace/top, no-duplicate pushes, wrapping
selection, activation dispatch); the Settings schema (defaults, clamping,
dirty flag, reset, validation errors); the bd.state persistence round trip
(simulating save/load by calling the systems handlers directly); the
read_bytes contract (NotoSans bytes, junk path FileNotFoundError); the font
registry (duplicate rejection, remove/re-add, list_fonts contents, default
font round trip, clear_fonts resetting to the built-in Default); the UI
scale round trip with 0.5/4.0 clamping; the style accessors (round trips
when the overlay has a context, the documented "no frame has run yet"
RuntimeError when headless, range validation); the pyui/ui_command bridge
(menu_open/menu_close/menu_toggle with the per-tic debounce, the live
activation wiring, the quit handshake, the mute toggle); and the map
fixture (three live monsters, the health reading, the event log). The run
ends via ``-scripttest``'s own PASS/FAIL accounting.

Manifest entry 4 of 4 (runs after its siblings have self-registered).
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

AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"
SCREENSHOT = os.environ.get("BD_EXAMPLE_SCREENSHOT") == "1"


def _close(a, b):
    return abs(float(a) - float(b)) < 1e-4


def no_raise(label, fn):
    """Run an any-event imgui call, failing the test instead of raising."""
    try:
        return fn()
    except Exception as exc:
        bd.assert_true(False, f"{label} raised {type(exc).__name__}: {exc}")
        return None


# --- run-mode scheduling ----------------------------------------------------------------


@bd.on("map_load")
def schedule_run_modes(event):
    if event.get("from_savegame"):
        return
    if AUTOTEST:
        pawn = systems.player_pawn()
        if pawn is not None:
            pawn.damage_factor = 0.0  # nothing may kill the test driver
        bd.schedule(autotest_menu_model, delay=10)
        bd.schedule(autotest_settings, delay=25)
        bd.schedule(autotest_persistence, delay=45)
        bd.schedule(autotest_read_bytes, delay=65)
        bd.schedule(autotest_font_registry, delay=85)
        bd.schedule(autotest_ui_scale, delay=140)
        bd.schedule(autotest_style, delay=155)
        bd.schedule(autotest_bridge_commands, delay=180)
        bd.schedule(autotest_bridge_asserts, delay=200)
        bd.schedule(autotest_bridge_toggle_close, delay=215)
        bd.schedule(autotest_bridge_retoggle_close, delay=225)
        bd.schedule(autotest_fixture, delay=245)
        bd.schedule(autotest_finish, delay=265)
    if SCREENSHOT:
        bd.schedule(screenshot_pose_title, delay=bd.TICRATE,
                    map_local=False)
        bd.schedule(lambda: bd.execute("screenshot /tmp/overture_1"),
                    delay=2 * bd.TICRATE + 20, map_local=False)
        bd.schedule(screenshot_pose_pause_settings,
                    delay=3 * bd.TICRATE + 30, map_local=False)
        bd.schedule(lambda: bd.execute("screenshot /tmp/overture_2"),
                    delay=4 * bd.TICRATE + 50, map_local=False)
        bd.schedule(screenshot_pose_help, delay=5 * bd.TICRATE + 60,
                    map_local=False)
        bd.schedule(lambda: bd.execute("screenshot /tmp/overture_3"),
                    delay=6 * bd.TICRATE + 80, map_local=False)
        bd.schedule(lambda: bd.execute("quit"), delay=8 * bd.TICRATE + 40,
                    map_local=False)


# --- screenshot poses -------------------------------------------------------------------


def screenshot_pose_title():
    """Pose 1: the title menu over the dimmed game."""
    systems.force_title_menu = True


def screenshot_pose_pause_settings():
    """Pose 2: the pause menu with the settings screen on top."""
    systems.force_title_menu = False
    systems.close_menu()
    systems.model.push("pause")
    systems.model.push("settings")


def screenshot_pose_help():
    """Pose 3: the help popup open above the pause menu."""
    systems.close_menu()
    systems.model.push("pause")
    systems.request_help()


# --- autotest steps ---------------------------------------------------------------------


def autotest_menu_model():
    """The pure screen stack: transitions, selection wrap, dispatch."""
    m = systems.MenuModel(content.SCREEN_IDS)
    bd.assert_true(m.top() is None and not m.is_open(), "a fresh model is empty")
    m.push("title")
    m.push("settings")
    m.push("credits")
    bd.assert_true(m.depth == 3 and m.top() == "credits",
                   "push builds the stack")
    m.push("credits")
    bd.assert_true(m.depth == 3, "pushing the current top is a no-op")
    bd.assert_true(m.pop() == "credits" and m.top() == "settings",
                   "pop returns the popped screen")
    m.replace_top("pause")
    bd.assert_true(m.top() == "pause" and m.depth == 2,
                   "replace_top swaps the top screen")
    m.clear()
    bd.assert_true(m.top() is None and not m.is_open(),
                   "clear empties the stack")
    m.replace_top("title")
    bd.assert_true(m.top() == "title", "replace_top pushes onto an empty stack")

    bd.assert_true(m.set_selection(1, 4, "title") == 1, "set_selection stores")
    bd.assert_true(m.move_selection(1, 4, "title") == 2, "move forward")
    bd.assert_true(m.move_selection(2, 4, "title") == 0,
                   "selection wraps forward")
    bd.assert_true(m.move_selection(-1, 4, "title") == 3,
                   "selection wraps backward")
    bd.assert_true(m.set_selection(9, 4, "title") == 1, "set wraps too")
    bd.assert_true(m.set_selection(3, 0, "title") == 0,
                   "an empty screen selects 0")

    seen = []
    m.on_activate = lambda sid, iid: seen.append((sid, iid))
    items = content.TITLE_MENU_ITEMS
    m.set_selection(2, len(items), "title")
    resolved = m.activate_selected(items, "title")
    bd.assert_true(resolved == "credits",
                   "activate resolves the selected item id")
    bd.assert_true(seen == [("title", "credits")],
                   "activation dispatches through on_activate")
    m.on_activate = None
    bd.assert_true(m.activate_selected(items, "title") == "credits",
                   "activation is safe unwired")


def autotest_settings():
    """Defaults, clamping, dirty flag, reset, and validation errors."""
    s = systems.Settings()
    bd.assert_true(s.get("ui_scale") == 1.0, "default ui scale")
    bd.assert_true(s.get("font_name") == content.DEFAULT_FONT_NAME,
                   "default font name")
    bd.assert_true(s.get("theme") == "dark", "default theme")
    bd.assert_true(tuple(s.get("accent"))
                   == tuple(content.SETTINGS_SCHEMA["accent"]["default"]),
                   "default accent")
    bd.assert_true(not s.is_dirty(), "fresh settings are clean")

    s.set("ui_scale", 1.4)
    bd.assert_true(_close(s.get("ui_scale"), 1.4) and s.is_dirty(),
                   "a change applies and flags dirty")
    s.set("ui_scale", 1.4)
    bd.assert_true(_close(s.get("ui_scale"), 1.4) and s.is_dirty(),
                   "re-writing the same value is a no-op")
    bd.assert_true(_close(s.set("ui_scale", 99.0), 2.0),
                   "numbers clamp into the schema range")
    try:
        s.set("theme", "neon")
        bd.assert_true(False, "an off-choice theme must raise")
    except ValueError:
        pass
    try:
        s.set("accent", (1.0, 0.0))
        bd.assert_true(False, "a short accent must raise")
    except ValueError:
        pass
    bd.assert_true(tuple(s.set("accent", (2.0, -1.0, 0.5, 1.0)))
                   == (1.0, 0.0, 0.5, 1.0), "accent components clamp to 0..1")
    try:
        s.set("no_such_key", 1.0)
        bd.assert_true(False, "an unknown key must raise")
    except KeyError:
        pass

    s.reset()
    bd.assert_true(s.get("ui_scale") == 1.0 and not s.is_dirty(),
                   "reset restores the defaults and clears dirty")

    snap = systems.settings.snapshot()
    systems.settings.set("font_size", 22.0)
    bd.assert_true(systems.settings.is_dirty(), "the live settings flag dirty")
    systems.settings.restore(snap)
    bd.assert_true(not systems.settings.is_dirty(),
                   "restore clears the dirty flag")


def autotest_persistence():
    """Simulate save/load by calling the systems handlers directly."""
    systems.settings.set("ui_scale", 1.5)
    systems.settings.mark_clean()
    systems.persist_settings()
    stored = bd.state.get(content.SETTINGS_STATE_KEY)
    bd.assert_true(isinstance(stored, dict)
                   and _close(stored.get("ui_scale", 0.0), 1.5),
                   "bd.state carries the settings snapshot")

    systems.settings.set("ui_scale", 1.0)
    systems.restore_settings()
    bd.assert_true(_close(systems.settings.get("ui_scale"), 1.5),
                   "restore_settings round-trips through bd.state")

    systems.settings.restore({"ui_scale": 3.0, "bogus_key": 1})
    bd.assert_true(_close(systems.settings.get("ui_scale"), 2.0),
                   "a restored snapshot clamps into range")
    systems.settings.restore(None)
    bd.assert_true(_close(systems.settings.get("ui_scale"), 2.0),
                   "a missing snapshot keeps the current values")

    systems.settings.reset()
    systems.persist_settings()
    bd.assert_true(bd.state.get(content.SETTINGS_STATE_KEY)["theme"] == "dark",
                   "the persisted snapshot ends at the defaults")


def autotest_read_bytes():
    """The font payload and the FileNotFoundError contract."""
    try:
        data = bd.read_bytes(content.NOTO_FONT_PATH)
    except Exception as exc:
        bd.assert_true(False, f"read_bytes raised {exc!r}")
        return
    bd.assert_true(len(data) > 100 * 1024, "the NotoSans TTF is over 100 KiB")
    bd.assert_true(data[:4] == b"\x00\x01\x00\x00",
                   "NotoSans carries the TrueType magic")
    try:
        bd.read_bytes("no/such/font.ttf")
        bd.assert_true(False, "a junk path must raise FileNotFoundError")
    except FileNotFoundError:
        pass
    except Exception as exc:
        bd.assert_true(False,
                       f"junk path raised {type(exc).__name__}: {exc}")
    try:
        bd.read_bytes("../outside.bin")
        bd.assert_true(False, "a traversal path must raise FileNotFoundError")
    except FileNotFoundError:
        pass
    except Exception as exc:
        bd.assert_true(False,
                       f"traversal path raised {type(exc).__name__}: {exc}")


def autotest_font_registry():
    """The any-event font APIs, each wrapped so a raise fails the test."""
    imgui = bd.imgui
    data = no_raise("read NotoSans", lambda: bd.read_bytes(
        content.NOTO_FONT_PATH))
    if data is None:
        return

    # engine_start already registered the catalog: duplicates are rejected.
    for size in content.NOTO_FONT_SIZES:
        name = content.noto_font_name(size)
        dup = no_raise(f"duplicate {name}", lambda n=name, s=size:
                       imgui.add_font_ttf(n, data, s))
        bd.assert_true(dup is False,
                       f"duplicate {name} registration returns False")

    # remove + re-add reports True.
    name = content.noto_font_name(content.NOTO_FONT_SIZES[-1])
    bd.assert_true(no_raise("remove_font", lambda: imgui.remove_font(name))
                   is True, "removing a registered font returns True")
    bd.assert_true(no_raise("add_font_ttf", lambda: imgui.add_font_ttf(
        name, data, content.NOTO_FONT_SIZES[-1])) is True,
        "re-adding the font returns True")

    fonts = no_raise("list_fonts", imgui.list_fonts) or []
    by_name = {entry[0]: entry for entry in fonts}
    for size in content.NOTO_FONT_SIZES:
        entry = by_name.get(content.noto_font_name(size))
        bd.assert_true(entry is not None and _close(entry[1], size),
                       f"list_fonts carries NotoSans at {int(size)}px")
    for size in content.DEFAULT_FONT_SIZES:
        entry = by_name.get(content.default_font_name(size))
        bd.assert_true(entry is not None and entry[2] is False
                       and entry[3] is False,
                       f"the vector default at {int(size)}px is registered")

    vector = content.default_font_name(content.DEFAULT_FONT_SIZES[-1])
    bd.assert_true(no_raise("set_default_font",
                            lambda: imgui.set_default_font(vector)) is True,
                   "set_default_font accepts a registered name")
    bd.assert_true(no_raise("get_default_font", imgui.get_default_font)
                   == vector, "get_default_font round-trips")
    bd.assert_true(no_raise("set_default_font unknown",
                            lambda: imgui.set_default_font("NoSuchFont"))
                   is False, "set_default_font rejects unknown names")
    bd.assert_true(no_raise("remove_font unknown",
                            lambda: imgui.remove_font("NoSuchFont")) is False,
                   "remove_font rejects unknown names")

    no_raise("clear_fonts", imgui.clear_fonts)
    fonts = no_raise("list_fonts after clear", imgui.list_fonts) or []
    bd.assert_true(all(entry[3] for entry in fonts),
                   "clear_fonts leaves only built-in faces")
    bd.assert_true(not no_raise("get_default_font after clear",
                                imgui.get_default_font),
                   "clear_fonts resets the default to the built-in face")

    # Restore the catalog and the configured default for later frames.
    systems.register_fonts()
    bd.assert_true(no_raise(
        "restore default font",
        lambda: imgui.set_default_font(str(systems.settings.get(
            "font_name")))) is True, "the configured default font restores")


def autotest_ui_scale():
    """set/get round trip plus the documented 0.5..4.0 clamping."""
    imgui = bd.imgui
    bd.assert_true(no_raise("set_ui_scale 1.25",
                            lambda: imgui.set_ui_scale(1.25)) is True,
                   "set_ui_scale returns True")
    bd.assert_true(_close(no_raise("get_ui_scale", imgui.get_ui_scale), 1.25),
                   "the ui scale round-trips")
    no_raise("set_ui_scale 0.1", lambda: imgui.set_ui_scale(0.1))
    bd.assert_true(_close(imgui.get_ui_scale(), 0.5),
                   "the scale clamps at the 0.5 floor")
    no_raise("set_ui_scale 99", lambda: imgui.set_ui_scale(99.0))
    bd.assert_true(_close(imgui.get_ui_scale(), 4.0),
                   "the scale clamps at the 4.0 ceiling")
    no_raise("restore ui scale", lambda: imgui.set_ui_scale(
        float(systems.settings.get("ui_scale"))))


def autotest_style():
    """Style accessors: round trips, or the documented headless contract.

    get/set_style_color, get/set_style_var and style_theme need an
    initialized overlay (one rendered frame); headless they raise
    RuntimeError("...no frame has run yet"), which is the documented
    contract and is asserted as such. Range validation runs first, so it
    is assertable in both environments.
    """
    imgui = bd.imgui
    try:
        imgui.get_style_color(10 ** 6)
        bd.assert_true(False, "an out-of-range color index must raise")
    except ValueError:
        pass
    except Exception as exc:
        bd.assert_true(False,
                       f"range check raised {type(exc).__name__}: {exc}")

    try:
        imgui.set_ui_scale(1.3)
        before = imgui.get_style_color(imgui.Col.Button)
        bd.assert_true(isinstance(before, tuple) and len(before) == 4,
                       "get_style_color returns an rgba tuple")
        imgui.set_style_color(imgui.Col.Button, 0.10, 0.20, 0.30, 1.0)
        after = imgui.get_style_color(imgui.Col.Button)
        bd.assert_true(all(_close(a, b) for a, b in
                           zip(after, (0.10, 0.20, 0.30, 1.0))),
                       "set/get_style_color round-trips")
        imgui.set_style_color(imgui.Col.Button, *before)

        rounding = imgui.get_style_var(imgui.StyleVar.FrameRounding)
        bd.assert_true(isinstance(rounding, float),
                       "a scalar style var reads back as a float")
        padding = imgui.get_style_var(imgui.StyleVar.WindowPadding)
        bd.assert_true(isinstance(padding, tuple) and len(padding) == 2,
                       "an ImVec2 style var reads back as a tuple")
        imgui.set_style_var(imgui.StyleVar.FrameRounding, float(rounding))

        imgui.style_theme("light")
        bd.assert_true(_close(imgui.get_ui_scale(), 1.3),
                       "style_theme preserves the ui scale")
        try:
            imgui.style_theme("neon")
            bd.assert_true(False, "an unknown theme must raise")
        except ValueError:
            pass
        imgui.style_theme(str(systems.settings.get("theme")))
        systems.apply_accent()  # style_theme resets colors; re-tint
    except RuntimeError as exc:
        bd.assert_true("no frame has run yet" in str(exc),
                       f"headless style contract raised: {exc}")
    except Exception as exc:
        bd.assert_true(False,
                       f"style access raised {type(exc).__name__}: {exc}")
    finally:
        try:
            imgui.set_ui_scale(float(systems.settings.get("ui_scale")))
        except Exception:
            pass


def autotest_bridge_commands():
    """Queue the console-side menu commands (asserted once drained).

    ``pyui`` takes exactly one name argument, so each target screen has its
    own command word.
    """
    bd.execute("pyui menu_toggle")            # in a map: opens the pause menu
    bd.execute("pyui menu_open_credits")      # credits on top of pause
    bd.execute("pyui menu_bogus")             # unknown: ignored
    bd.execute("pyui menu_close")             # clears the stack
    bd.execute("pyui menu_open_settings")


def autotest_bridge_asserts():
    """The drained ui_command effects, debounce, wiring, and flows."""
    bd.assert_true(systems.model.top() == "settings"
                   and systems.model.depth == 1,
                   "menu_open/menu_close drove the stack through the console")

    first = systems.request_menu_toggle()
    second = systems.request_menu_toggle()
    bd.assert_true(first and not second,
                   "two toggles in one tic collapse through the debounce")
    bd.assert_true(not systems.model.is_open(),
                   "the surviving toggle popped the settings screen")

    # Live activation wiring: selecting Help on the pause menu.
    systems.model.push("pause")
    systems.model.set_selection(2, len(content.PAUSE_MENU_ITEMS), "pause")
    systems.model.activate_selected(content.PAUSE_MENU_ITEMS, "pause")
    bd.assert_true(systems.help_popup_requested,
                   "activating Help requests the popup")
    systems.help_popup_requested = False
    systems.model.clear()

    # The quit handshake: request -> confirm -> cancel -> confirm for real.
    systems.request_quit()
    bd.assert_true(systems.quit_confirm_requested
                   and not systems.quit_requested,
                   "request_quit arms the confirm popup only")
    systems.cancel_quit()
    bd.assert_true(not systems.quit_confirm_requested
                   and not systems.quit_requested,
                   "cancel_quit clears the handshake")
    systems.request_quit()
    systems.confirm_quit(quit_engine=False)
    bd.assert_true(systems.quit_requested and not systems.quit_confirm_requested,
                   "confirm_quit latches the quit request")
    systems.quit_requested = False

    # The mute toggle and its toast text.
    before = systems.muted
    systems.toggle_mute()
    bd.assert_true(systems.muted is (not before), "toggle_mute flips the flag")
    bd.assert_true(systems.mute_toast_text
                   in (content.TOAST_MUTED, content.TOAST_UNMUTED),
                   "the toast names the new mute state")
    systems.toggle_mute()
    bd.assert_true(systems.muted is before, "toggling again restores the flag")


def autotest_bridge_toggle_close():
    """A later tic's toggle opens the pause menu (debounce window passed)."""
    bd.assert_true(systems.request_menu_toggle() is True,
                   "a toggle in a new tic acts")
    bd.assert_true(systems.model.top() == "pause",
                   "the toggle opened the pause menu")


def autotest_bridge_retoggle_close():
    """And the tic after that, the toggle unwinds it again."""
    bd.assert_true(systems.model.is_open(), "the pause menu is still up")
    bd.assert_true(systems.request_menu_toggle() is True,
                   "the retoggle in a new tic acts")
    bd.assert_true(not systems.model.is_open(),
                   "the retoggle unwound the pause menu")
    systems.force_title_menu = False
    systems.close_menu()


def autotest_fixture():
    """The spawned monsters, the health reading, and the event log."""
    pawn = systems.player_pawn()
    bd.assert_true(pawn is not None, "the player pawn is available")
    live = 0
    for ref in systems.fixture_monsters:
        try:
            if ref is not None and ref.valid and ref.alive:
                live += 1
        except RuntimeError:
            pass
    bd.assert_true(live == len(content.FIXTURE_MONSTERS),
                   "all three fixture monsters are up")
    health = systems.player_health()
    bd.assert_true(health is not None and health > 0,
                   "the health bar reading is live")
    bd.assert_true(any("map_load" in line for line in systems.event_log),
                   "the event log caught the map load")
    bd.assert_true(len(systems.event_log) >= 2,
                   "the event log caught engine_start and map_load")


def autotest_finish():
    bd.log(content.AUTOTEST_DONE_LOG)


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


_sys.modules.setdefault("overture_main", _LiveAlias("overture_main",
                                                    globals()))
del _sys, _types
