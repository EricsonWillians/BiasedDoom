"""Overture Menu Kit - pure content: menus, settings schema, fonts, strings.

**Import-safe**: no engine calls at import time. Every menu definition, the
settings schema with its defaults, the font catalog, the theme list, the key
bindings table, the screen layout constants, and every player-visible string
live here as plain data, so the suite can be re-skinned and re-voiced without
touching logic.

Loaded as manifest entry 1 of 4; the trailing ``_LiveAlias`` registration lets
later manifest entries ``import overture_content`` (the engine registers
manifest modules under mangled names only after execution, so the proxy reads
through to this module's live globals).

The font payload is the engine's NotoSans-Regular.ttf face. ``bd.read_bytes``
is container-scoped (it only sees lumps inside this example's PK3), so the
example ships its own copy under ``fonts/``; the packaging script includes a
top-level ``fonts/`` directory automatically. The engine ships the identical
bytes at ``widgets/noto/NotoSans-Regular.ttf``.
"""

# --- example identity ----------------------------------------------------------------

EXAMPLE_TITLE = "Overture Menu Kit"
SETTINGS_STATE_KEY = "overture_settings"

# --- fonts ---------------------------------------------------------------------------
#: VFS path of the shipped TTF, read with ``bd.read_bytes`` (container-scoped).
NOTO_FONT_PATH = "fonts/NotoSans-Regular.ttf"
NOTO_FONT_SIZES = (15.0, 18.0, 24.0)
DEFAULT_FONT_SIZES = (13.0, 16.0, 20.0)


def noto_font_name(size):
    """Registry name of the NotoSans face at one of NOTO_FONT_SIZES."""
    return "NotoSans%d" % int(size)


def default_font_name(size):
    """Registry name of the vector default face at one of DEFAULT_FONT_SIZES."""
    return "Default%d" % int(size)


#: Font the UI starts on unless the settings say otherwise.
DEFAULT_FONT_NAME = noto_font_name(18.0)

# --- settings schema -------------------------------------------------------------------
#: Every tunable with its default and validation range. The systems layer
#: builds the live Settings object from this table alone.
SETTINGS_SCHEMA = {
    "ui_scale": {"default": 1.0, "min": 0.75, "max": 2.0},
    "font_name": {"default": DEFAULT_FONT_NAME},
    "font_size": {"default": 18.0, "min": 15.0, "max": 28.0},
    "theme": {"default": "dark", "choices": ("dark", "classic", "light")},
    "accent": {"default": (0.82, 0.42, 0.16, 1.0)},  # rgba, ember orange
    "opacity": {"default": 0.95, "min": 0.30, "max": 1.0},
}
SETTINGS_KEYS = tuple(SETTINGS_SCHEMA)

THEMES = ("dark", "classic", "light")

#: Accent application: hovered buttons brighten by this factor (plus a flat
#: lift), FrameBg becomes a darkened tint of the accent.
ACCENT_HOVER_FACTOR = 1.3
ACCENT_HOVER_LIFT = 0.10
ACCENT_FRAME_SHADE = 0.4

# --- screens and menus -------------------------------------------------------------------
#: Screen ids the model knows, in stacking order of appearance.
SCREEN_IDS = ("title", "pause", "settings", "credits")

TITLE_MENU_ITEMS = (
    {"id": "new_game", "label": "New Game"},
    {"id": "settings", "label": "Settings"},
    {"id": "credits", "label": "Credits"},
    {"id": "quit", "label": "Quit"},
)

PAUSE_MENU_ITEMS = (
    {"id": "resume", "label": "Resume"},
    {"id": "settings", "label": "Settings"},
    {"id": "help", "label": "Help"},
    {"id": "quit_to_title", "label": "Quit to Title"},
)

MENU_ITEMS = {"title": TITLE_MENU_ITEMS, "pause": PAUSE_MENU_ITEMS}

# --- key bindings --------------------------------------------------------------------------
#: The whole keyboard contract in one table; the README renders this.
KEY_BINDINGS = (
    ("Esc", "Open or close the pause menu; closes a popup first"),
    ("F1", "Open the help popup"),
    ("F2", "Toggle the tool panel (event log + monster monitor)"),
    ("Ctrl+M", "Mute or unmute the audio"),
    ("1-9", "Activate the numbered item of the current menu"),
    ("Up/Down, Enter", "Move the selection, activate the selected item"),
)

HOTKEY_DIGITS = "123456789"

# --- popups ---------------------------------------------------------------------------------
HELP_POPUP_ID = "###overture_help"
CONFIRM_QUIT_POPUP_ID = "###overture_confirm_quit"

HELP_TOPICS = (
    ("Navigation",
     "The menus are keyboard-first. Up and Down move the selection, Enter "
     "activates it, and the number keys 1-9 jump straight to an item. Esc "
     "backs out of a screen and closes popups before it touches the stack."),
    ("Settings",
     "UI scale rescales every widget, the font family and size restyle the "
     "text live, the theme swaps the palette, and the accent recolors the "
     "buttons. Everything applies immediately and persists into savegames."),
    ("Tool Panel",
     "F2 docks an event log and a monster monitor to the viewport. Both "
     "windows stay movable and resizable; the docking only suggests their "
     "first position."),
    ("Mute",
     "Ctrl+M toggles the mute flag. The toast line at the bottom of the "
     "screen confirms the new state for a few seconds."),
)

CREDITS_LINES = (
    "OVERTURE MENU KIT",
    "",
    "A keyboard-first menu suite scripted entirely in Python",
    "on top of the engine's Dear ImGui overlay.",
    "",
    "Fonts: Noto Sans (OFL), served from the example PK3",
    "UI: Dear ImGui 1.92.8, docking branch, drawn by bd.imgui",
    "Engine: BiasedDoom, embedded CPython scripting",
    "",
    "Every window you can see redraws itself every frame",
    "from plain Python state. No native menus were harmed.",
    "",
    "Press Esc to go back.",
)

# --- screen layout constants ------------------------------------------------------------------
MENU_WINDOW_WIDTH = 360.0
MENU_BUTTON_WIDTH = 300.0
MENU_BUTTON_HEIGHT = 0.0       # 0 lets ImGui pick the frame height
TITLE_MENU_POS = (760.0, 240.0)
PAUSE_MENU_POS = (780.0, 330.0)
SETTINGS_POS = (740.0, 220.0)
CREDITS_POS = (560.0, 160.0)
TOOL_LOG_POS = (24.0, 40.0)
TOOL_LOG_SIZE = (400.0, 220.0)
TOOL_MONSTER_POS = (440.0, 40.0)
TOOL_MONSTER_SIZE = (360.0, 220.0)
DIM_ALPHA = 0.55
HELP_POPUP_WIDTH = 520.0
CONFIRM_POPUP_WIDTH = 380.0
CREDITS_SCROLL_SPEED = 26.0    # pixels per second of animation clock
MUTE_TOAST_SECONDS = 3.0

# --- UI strings ---------------------------------------------------------------------------------
TITLE_MENU_TITLE = "Overture - Main Menu"
PAUSE_MENU_TITLE = "Paused"
SETTINGS_TITLE = "Settings"
CREDITS_TITLE = "Credits"
TOOL_LOG_TITLE = "Overture - Event Log"
TOOL_MONSTER_TITLE = "Overture - Monster Monitor"
TOAST_MUTED = "Audio muted (Ctrl+M to restore)"
TOAST_UNMUTED = "Audio on"
CONFIRM_QUIT_TEXT = "Leave the game? Unsaved progress will be lost."
CONFIRM_QUIT_YES = "Yes, quit"
CONFIRM_QUIT_NO = "No, stay"
HELP_TITLE = "Help"
SETTINGS_RESET_LABEL = "Reset to defaults"
SETTINGS_BACK_LABEL = "Back"
SETTINGS_DIRTY_MARKER = " *"
#: The menu toggle footer hint; the "%s" is the live display name of the
#: Custom Action 4 binding (see systems.menu_toggle_hint).
FOOTER_ESCAPE_HINT = "%s: back   F1: help   F2: tools   Ctrl+M: mute"
FOOTER_DIGIT_HINT = "1-%d: activate   Up/Down: select   Enter: confirm"
FOOTER_TITLE_HINT = "A mod menu drawn by bd.imgui every frame"
LOG_CAPACITY = 64
MUTE_LOG_TEXT = "Ctrl+M: mute toggled"

# --- the map fixture -----------------------------------------------------------------------------
#: Three calm monsters on the probe-verified MAP01 yard line (see
#: 33_rpg_campaign for the probe notes): x -224..-288 at y 800, floor 56.
#: They stand friendly and still so the pause menu and the monitor have
#: something stable to show.
FIXTURE_MONSTERS = (
    {"class_name": "ZombieMan", "pos": (-224.0, 800.0, 56.0)},
    {"class_name": "ShotgunGuy", "pos": (-256.0, 800.0, 56.0)},
    {"class_name": "DoomImp", "pos": (-288.0, 800.0, 56.0)},
)

AUTOTEST_DONE_LOG = "OVERTURE MENU KIT AUTOTEST assertions complete"


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


_sys.modules.setdefault("overture_content",
                        _LiveAlias("overture_content", globals()))
del _sys, _types
