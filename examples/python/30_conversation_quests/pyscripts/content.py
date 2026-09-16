"""The Confessor - pure content: fixture constants, prose, and factories.

This module is **import-safe**: it performs no engine calls at import time.
Everything is either plain data or a factory that the systems layer invokes
from event handlers. All narrative prose lives here so the tone of the
fixture can be re-voiced without touching logic.

Loaded as a ``PYTHON`` manifest entry (first, in dependency order). The
trailing ``sys.modules`` alias lets later manifest entries reach it with a
plain ``import confessor_content``; the manifest itself mangles module
names, mirroring how ``hello_world`` registers siblings via
``bd.import_script(..., module_name=...)``.
"""

# --- fixture constants (must match DIALOG01 / LANGUAGE / MAPINFO) -----------

NPC_CLASS = "QuestScribe"        # conversation ID 920 in MAPINFO
NPC_NAME = "The Confessor"
LOG_NUMBER = 77
# SetLogNumber stores the $TXT_LOGTEXT<n> *label* in the player's log (so a
# language switch re-translates it); bd.player_log() returns that label.
LOG_LABEL = "$TXT_LOGTEXT77"
# DIALOG01 defines a single page and Doom II ships no dialogue lumps, so the
# page lands at index 0 of the level's StrifeDialogues array. Reply indices
# follow the choice order inside the page.
FIRST_NODE = 0
REPLY_ACCEPT = 0
REPLY_DECLINE = 1

# --- quest definition ---------------------------------------------------------

QUEST_ID = "rite_of_confession"
QUEST_NAME = "The Price of Absolution"
QUEST_DESCRIPTION = ("The Confessor trades absolution for secrets. Kneel, "
                     "and give him yours.")
QUEST_GIVER = NPC_NAME
QUEST_FACTION = "The Ashen Order"
OBJECTIVE_ID = "confess"
OBJECTIVE_TEXT = "Confess your sins to the Confessor"
REWARD_MESSAGE = "Absolution granted - your secret now belongs to him."

# --- booth candle (a slow flame for the Confessor's sector) -------------------

CANDLE_BASE = 104        # well under MAP01's indoor light: the booth dims
CANDLE_AMPLITUDE = 24    # how far the walk may wander from the base
CANDLE_PERIOD = 13       # tics between steps (slow, breathing flame)

# --- quest-giver marker (the gold "!" over the Confessor) ----------------------

# Display-list ids: the bd.ui toolkit owns every id >= 900000 (see
# docs/scripting/python.md, "Layers and id ownership"); this example owns the
# 97000-97009 block, well clear of the toolkit and of the low ids other
# examples use.
MARKER_TEXT_ID = 97000   # the floating "!" label above the Confessor
MARKER_RING_ID = 97001   # the gold ground ring at his feet
MARKER_REFRESH_TICS = 7  # the map-local marker task re-syncs this often
MARKER_TEXT = "!"
MARKER_COLOR = (255, 205, 70)  # quest-giver gold
MARKER_TEXT_HEIGHT = 0.03      # screen-height fraction: big enough to read
MARKER_RING_RADIUS = 26.0

# --- prose: toasts and the Rite panel -----------------------------------------

SPAWN_TOAST = "The Confessor waits in the ash-light - walk up and press USE to kneel."
# The accept reply's log is *numeric* (log = "LOG77"), so conversation_reply
# reports log_string=None and the LANGUAGE label cannot be resolved from
# Python (no localization binding exists in the bd API). The toast therefore
# carries a fixed themed line; a free-text log_string would be toasted
# verbatim (see systems._toast_reply).
TOAST_ACCEPT = "the rite is sealed - your sin has a price now"
# The accept reply also hands over Shell ammo (giveitem = 921) through the
# native pickup path; name the payout so it is visible even off-menu.
TOAST_REWARD = "alms for the honest: a box of shells"
TOAST_DECLINE = "the Confessor watches you leave, unmoved"
# A decline never fails the quest: the marker stays lit and the conversation
# can simply be re-entered. This toast says so out loud.
TOAST_OFFER_STANDS = "The offer stands. Kneel again when you are ready."
TOAST_STARTED = "the Confessor opens the black book"

RITE_WAITING = "He waits, patient as the grave."
RITE_ACCEPTED = "'Alms for the honest. Go - and sin no louder.'"
RITE_DECLINED = "'Then carry them, penitent. They only get heavier.'"
# Plain hint line the Rite panel shows while the quest is INACTIVE.
RITE_HINT_INACTIVE = "Kneel before the Confessor with USE (the marked figure ahead)."

#: Themed Rite-panel line per (node, reply_index) the fixture can produce.
RITE_LINES = {
    (FIRST_NODE, REPLY_ACCEPT): RITE_ACCEPTED,
    (FIRST_NODE, REPLY_DECLINE): RITE_DECLINED,
}


def build_quest():
    """Construct the confession quest (call from an engine_start handler)."""
    from bd_quests import Objective, Quest

    quest = Quest(QUEST_ID, QUEST_NAME,
                  description=QUEST_DESCRIPTION,
                  giver=QUEST_GIVER, faction=QUEST_FACTION)
    quest.add_objective(Objective(OBJECTIVE_ID, OBJECTIVE_TEXT))
    quest.rewards["message"] = REWARD_MESSAGE
    return quest


# --- sibling-import registration (see the module docstring) --------------------

import sys as _sys
import types as _types


class _LiveAlias(_types.ModuleType):
    """sys.modules alias that reads/writes through to the module's live
    globals. The engine registers manifest modules in sys.modules under a
    mangled name *after* execution, so a manifest module cannot alias its
    real module object; this proxy is the next best thing."""

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


_sys.modules.setdefault("confessor_content",
                        _LiveAlias("confessor_content", globals()))
del _sys, _types
