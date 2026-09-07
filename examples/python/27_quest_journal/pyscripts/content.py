"""Whispers in the Walls — content: pure data tables and factories.

This module makes **no engine calls at import time** (and none at all —
the factories only build plain ``bd_quests`` data objects). systems.py
turns these tables into a living campaign, ui.py renders them, and
main.py wires both to engine events.

Expansion guide: adding a quest is ONE new row in :data:`QUESTS` plus,
when it needs event wiring, one tracker line in ``systems.setup_engine``.
Everything else (the Grimoire, persistence, toasts, the favor ledger fed
by ``reward_xp``) picks it up for free.
"""

from bd_quests import Objective, Quest

# --- map geometry (Doom II MAP01, verified with in-engine probes) --------------

#: The ritual circle: MAP01's raised walkway over the shotgun room carries
#: sector tag 5 (sector 21, light 128); RITUAL_SPOT sits solidly inside it.
RITUAL_TAG = 5
RITUAL_SPOT = (96.0, 448.0, 152.0)

#: How long the lights die when the rite begins, and how much later the
#: fluorescent flicker takes over.
BLACKOUT_TICS = 35
FLICKER_DELAY_TICS = 44

# --- cast -----------------------------------------------------------------------

#: The three desecrated pages (a custom ZScript pickup, see the ZSCRIPT
#: lump), scattered in an east-west line across the flat entry hall
#: (sector 7, floor 56 — verified with in-engine probes). The autotest
#: walks the line west to east from PAGE_WALK_START.
PAGE_CLASS = "RitualPage"
PAGE_TIDS = (9101, 9102, 9103)
PAGE_POSITIONS = ((-160.0, 800.0, 56.0), (-96.0, 800.0, 56.0),
                  (-32.0, 800.0, 56.0))
PAGE_WALK_START = (-184.0, 800.0, 56.0)
PAGE_WALK_ANGLE = 0.0                 # face due east, down the line

#: The whispering dead: the pack that rises when the circle is entered.
WAVE_CLASS = "ZombieMan"
WAVE_TIDS = (9111, 9112, 9113)
WAVE_RADIUS = 96.0                    # spawn ring around the circle's heart
WAVE_TINT = (70, 70, 96)              # ash-blue drowned dead

#: The Choir: the thing the rite was always going to summon.
CHOIR_CLASS = "BaronOfHell"
CHOIR_TID = 9121
CHOIR_TINT = (150, 24, 40)            # liturgical red
CHOIR_OFFSETS = ((112.0, 0.0), (-112.0, 0.0), (0.0, 112.0), (0.0, -112.0))

CHECKPOINT_NAME = "whispers_in_the_walls_example"

# --- the campaign ----------------------------------------------------------------

#: One row per quest. ``objectives`` run sequentially; ``starts`` marks who
#: is available from map load (the rest are unsealed by systems.py when
#: their predecessor completes).
QUESTS = [
    {
        "id": "pages",
        "name": "The Desecrated Pages",
        "description": (
            "Three leaves were torn from the parish choirbook and fed to "
            "the walls of the entry hall. The mortar has been whispering "
            "ever since. Take the pages back; every one recovered is a "
            "voice the dark must do without."
        ),
        "giver": "The Sexton's Note",
        "faction": "The Quiet Parish",
        "starts": True,
        "objectives": [
            ("recover_pages", "Recover the desecrated pages", 3),
        ],
        "reward_give": [("Shell", 8)],
        "reward_message": "The pages shiver in your grip.",
        "reward_xp": 75,
    },
    {
        "id": "rite",
        "name": "Cleanse the Ritual Site",
        "description": (
            "Above the cistern room the old congregation poured a circle "
            "into the walkway and knelt down inside it. None of them stood "
            "up again. Step into the circle and finish the rite they "
            "began. Once it has begun, do not step out of the circle "
            "until the whispering stops."
        ),
        "giver": "The Sexton's Note",
        "faction": "The Quiet Parish",
        "starts": False,
        "objectives": [
            ("enter_circle", "Step into the ritual circle", 1),
            ("silence_dead", "Put down the whispering dead", 3),
        ],
        "reward_give": [],
        "reward_message": "The circle gutters out.",
        "fail_reason": "the circle was abandoned mid-rite",
    },
    {
        "id": "choir",
        "name": "Silence the Whispering Dead",
        "description": (
            "The rite was never an ending. It was a summons, and what "
            "answered it now wears a butcher's body and sings with the "
            "voices of everything it has eaten. Silence the Choir."
        ),
        "giver": "The Quiet Parish",
        "faction": "The Quiet Parish",
        "starts": False,
        "objectives": [
            ("silence_choir", "Silence the Choir", 1),
        ],
        "reward_give": [],
        "reward_message": "Silence, at last.",
        "reward_xp": 150,
    },
]

# --- prose -----------------------------------------------------------------------

#: Toast lines. ``{p}``/``{n}`` are filled with objective progress.
TOAST_PAGE_PROGRESS = "a page folds itself into your coat ({p}/{n})"
TOAST_PAGES_DONE = ("the pages shiver in your grip. the circle waits above "
                    "the cistern.")
TOAST_XP_REWARD = "the parish owes you {total} favor (+{amount})"
TOAST_RITE_ENTERED = "the light dies. something kneels down beside you."
TOAST_WAVE_PROGRESS = "another whisper throttles silent ({p}/{n})"
TOAST_RITE_DONE = "the circle gutters out. above you, the Choir draws breath."
TOAST_RITE_FAILED = "the circle drinks your absence. the rite is broken."
TOAST_CHOIR_DONE = "the walls forget your name. for now."
TOAST_SCREENSHOT_OMEN = "the mortar has learned your name"

# --- factories ---------------------------------------------------------------------


def build_quests():
    """Instantiate the campaign's quests from :data:`QUESTS` (fresh objects)."""
    quests = []
    for spec in QUESTS:
        quest = Quest(spec["id"], spec["name"], description=spec["description"],
                      giver=spec["giver"], faction=spec["faction"])
        for obj_id, text, count in spec["objectives"]:
            quest.add_objective(Objective(obj_id, text, count=count))
        quest.rewards["give"] = list(spec["reward_give"])
        quest.rewards["message"] = spec["reward_message"]
        if spec.get("reward_xp"):
            quest.rewards["xp"] = int(spec["reward_xp"])
        quests.append(quest)
    return quests


def row(quest_id):
    """The content row for a quest id, or None."""
    for entry in QUESTS:
        if entry["id"] == quest_id:
            return entry
    return None
