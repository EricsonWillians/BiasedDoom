"""The Last Feeding — content: pure data tables and factories.

This module makes **no engine calls at import time** (and none at all).
systems.py builds the live chronicle from these tables, ui.py renders
them, and main.py wires both to engine events.

The fiction: you are a 13th-generation fledgling, thin-blooded and three
nights dead, cornered in a transit concourse where the last mortal herd
of the district still huddles. The Sabbat have followed you across the
river. Feed, keep the Masquerade, and do not let the Beast drive.
"""

# --- cast ------------------------------------------------------------------------

#: The mortal herd: tinted zombiemen standing in as vessels (their damage
#: output is zeroed by systems.py). Names feed the feeding toasts.
MORTALS = [
    {"name": "Willa Croft, night nurse", "tid": 9201, "cls": "ZombieMan",
     "tint": (255, 216, 176)},
    {"name": "Eddie Voss, missed his train", "tid": 9202, "cls": "ZombieMan",
     "tint": (235, 200, 160)},
    {"name": "Father Abram, lapsed", "tid": 9203, "cls": "ZombieMan",
     "tint": (210, 180, 150)},
]
MORTAL_TIDS = tuple(m["tid"] for m in MORTALS)
MORTAL_CLASS = "ZombieMan"

#: The Sabbat shovelhead pack: dark-red shotgun guys, fully hostile.
SABBAT = [
    {"name": "shovelhead 'Brick'", "tid": 9211, "cls": "ShotgunGuy"},
    {"name": "shovelhead 'Moth'", "tid": 9212, "cls": "ShotgunGuy"},
    {"name": "shovelhead 'Candle'", "tid": 9213, "cls": "ShotgunGuy"},
]
SABBAT_TIDS = tuple(s["tid"] for s in SABBAT)
SABBAT_CLASS = "ShotgunGuy"
SABBAT_TINT = (150, 40, 40)

#: The answering pack: spawned around the player on a Masquerade breach.
AMBUSH_CLASS = "ShotgunGuy"
AMBUSH_TIDS = (9221, 9222, 9223)
AMBUSH_TINT = (110, 20, 20)
AMBUSH_RADIUS = 320.0
AMBUSH_COOLDOWN_TICS = 700     # one answering pack per night, roughly

CHECKPOINT_NAME = "the_last_feeding_example"

# --- the hunt (tunables) -----------------------------------------------------------

#: Cowering: mortals within this radius who can see the predator scramble
#: away (a slow scheduled thrust, capped at COWER_MAX_SPEED).
COWER_RADIUS = 128.0
COWER_TICS = 4
COWER_SPEED = 5.0
COWER_MAX_SPEED = 7.0

#: Darkness-aware feeding: below this sector light the witness radius is
#: halved — the dark keeps your secrets.
DARK_LIGHT = 96
WITNESS_RADIUS_LIT = 512.0
WITNESS_DARK_FACTOR = 0.5

#: Hunger drives the bd_horror dread meter one-to-one-ish.
DREAD_PER_HUNGER = 20.0

#: Frenzy vignette storm: red screen pulses and a racing heartbeat for the
#: duration of a frenzy.
STORM_TICS = 70
STORM_PULSE_TICS = 7
STORM_HEARTBEAT = "weapons/sawhit"   # stock sound, short dull thud

# --- disciplines ---------------------------------------------------------------------

#: Registered in order; factories live in bd_vtm.
DISCIPLINES = ("celerity", "obfuscate", "potence", "dominate")

# --- factions -------------------------------------------------------------------------

FACTIONS = [
    ("Camarilla", {"Sabbat": -2, "Anarchs": 0}),
    ("Anarchs", {"Sabbat": -2}),
    ("Sabbat", {"Camarilla": -2, "Anarchs": -2}),
]

# --- quests -----------------------------------------------------------------------------

QUESTS = [
    {
        "id": "first_night",
        "name": "The First Night",
        "description": (
            "Sabbat shovelheads followed you across the river. They wear "
            "their dead like trophies and they do not care who sees. Put "
            "them down before the Masquerade is just one more corpse."
        ),
        "giver": "The Barkeep",
        "faction": "Anarchs",
        "objectives": [("put_down_pack", "Put down the Sabbat pack", 3)],
        "reward_message": "The pack is dealt with. Word travels.",
    },
    {
        "id": "street_cred",
        "name": "Street Cred",
        "description": (
            "The Anarchs owe you a breath. Prove the first night was not "
            "luck: thin one more shovelhead from the herd."
        ),
        "giver": "The Barkeep",
        "faction": "Anarchs",
        "objectives": [("prove_it", "Put down another shovelhead", 1)],
        "reward_message": "The Anarchs nod your way.",
    },
]

# --- prose --------------------------------------------------------------------------------

TOAST_FEED_SIP = "the Kiss closes over {name}'s throat"
TOAST_FEED_KILL = "drained dry - the Beast licks its teeth"
TOAST_FEED_NOBODY = "no vessel within reach"
TOAST_BREACH = "the Masquerade tears - the Sabbat answer with teeth"
TOAST_FRENZY = "THE BEAST TAKES OVER"
TOAST_FIRST_NIGHT_DONE = "the pack is dealt with. word travels."
MSG_FRENZY = "THE BEAST TAKES OVER"
MSG_GATE_OPEN = "The Anarchs will talk to you now."


def mortal_name(tid):
    """Roster name for a mortal TID, for diegetic feeding toasts."""
    for m in MORTALS:
        if m["tid"] == tid:
            return m["name"].split(",")[0]
    return "a vessel"
