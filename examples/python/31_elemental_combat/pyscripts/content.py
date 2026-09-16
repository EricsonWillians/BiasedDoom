"""Pyre & Rime — pure content: elements, elites, relics, and prose.

Importing this module makes no engine calls and registers nothing; it
only holds the scenario's constants and pure bd_rpg factory functions
(constructing a ``LootTable`` is plain Python). Every number a designer
might tune lives here.

The rite (Doom II MAP01): you carry an elemental focus cycled with F
between **Pyre**, **Rime**, and **Rot**; your native weapon hits are
retyped and affinity-scaled through the ``actor_before_damage`` mutable
filter, and G hurls an elemental burst at the nearest horror, resolved
end-to-end by ``bd_rpg.resolve_attack``. The horde ahead burns eagerly
(imps are weak to Pyre) and the pinkies are rot-proof (immune to Rot).
Two orange **Cinder Thralls** set you ablaze when their blows connect;
the pale **Rime-Bound** chills you to the bone. Behind you stands the
**Warding Idol** (a Soulsphere prop): its blessing turns your skin to
granite — and its price is the light, drunk from the room around it.
The Rime-Bound carries a relic: when it dies, something old surfaces.

The goal: **break the horde**. A persistent control strip (a display-list
``bd.draw_text``) shows the live focus, the real key bindings, and the
horde count; struck monsters float throttled affinity labels; the elites
wear their weakness over their heads. When all nine arena monsters lie
dead, a one-time souls bonus lands through the kill-XP wiring.
"""

from __future__ import annotations

from fractions import Fraction

import bd_rpg

# --- the three elements -------------------------------------------------------------

ELEMENTS = ("pyre", "rime", "rot")

#: Damage-type registry colors (RGB 0-255) for the custom types.
ELEMENT_COLORS = {
    "pyre": (224, 88, 20),
    "rime": (150, 190, 255),
    "rot": (120, 190, 70),
}

#: bd_horror PALETTE keys per element, for sigils and status bars.
ELEMENT_TONES = {
    "pyre": "ember",
    "rime": "sickly",
    "rot": "bruise",
}

ELEMENT_PROSE = {
    "pyre": "Pyre — the hunger that remembers the sun.",
    "rime": "Rime — the patience of deep water, frozen mid-breath.",
    "rot": "Rot — the quiet argument that all things lose.",
}

#: Class-level affinity defaults: imps burn eagerly, pinkies are
#: rot-proof. (class_name, element, multiplier)
CLASS_AFFINITIES = (
    ("DoomImp", "pyre", 2.0),
    ("Demon", "rot", 0.0),
)

#: class -> XP for kill awards.
KILL_XP_TABLE = {"DoomImp": 20, "Demon": 30}

# --- the elite roster -----------------------------------------------------------------

#: Each elite: TID, display name, world tint, and the affix its blows
#: apply ("pyre" -> burning, "rime" -> slowed).
ELITES = (
    {"tid": 9430, "name": "Cinder Thrall", "tint": (255, 140, 40),
     "affix": "pyre"},
    {"tid": 9431, "name": "Cinder Thrall", "tint": (255, 140, 40),
     "affix": "pyre"},
    {"tid": 9432, "name": "Rime-Bound", "tint": (150, 190, 255),
     "affix": "rime"},
)
ELITE_TIDS = tuple(elite["tid"] for elite in ELITES)
ELITE_BY_TID = {elite["tid"]: elite for elite in ELITES}
ELITE_CLASS = "DoomImp"
BURNING_TICS = 175   # a Cinder Thrall's parting gift
CHILLED_TICS = 140   # the Rime-Bound's grip

# --- the horde and the fixtures ----------------------------------------------------------

HORDE_IMP_TIDS = (9410, 9411, 9412, 9413)
PINKY_TIDS = (9420, 9421)
STATUS_IMP_TID = 9401           # TID-tagged imp for the persistence recipe
DUMMY_TIDS = (9450, 9451, 9452, 9453, 9454, 9455)  # autotest targets
STATUS_DUMMY_TID = 9460         # autotest status-semantics target
CHECKPOINT_NAME = "pyre_rime_example"

# --- the goal: break the horde -----------------------------------------------------

#: Every arena monster TID that counts toward the win: the pyre-weak
#: imps, the rot-proof pinkies, and the three elites (the status imp and
#: the autotest dummies are fixtures, not horde).
HORDE_TIDS = HORDE_IMP_TIDS + PINKY_TIDS + ELITE_TIDS
HORDE_TID_SET = frozenset(HORDE_TIDS)

#: One-time souls bonus when the last horde monster falls, awarded
#: through bd_rpg.award_kill_xp so the litany listener sings it too.
HORDE_BONUS_XP = 100

CENTER_HORDE_BROKEN = "THE HORDE IS BROKEN"
TOAST_HORDE_BROKEN = "The horde is broken; the rite is complete."
LITANY_HORDE_BROKEN = "The horde is broken"
STRIP_VICTORY = "HORDE BROKEN - the rite is complete"

# --- the Warding Idol --------------------------------------------------------------------

IDOL_CLASS = "Soulsphere"       # the shrine prop
IDOL_WARD_TICS = 350            # stoneskin duration
IDOL_BLACKOUT_TICS = 140        # how long the idol drinks the light
TOAST_IDOL = "The idol drinks the light."

# --- the horde-pen corpse-light ----------------------------------------------------------

#: A slow fluorescent program over the horde pen: steady light, random
#: full dropouts into the black. (The pen's sector index is probed at
#: map_load — MAP01's arena sectors are untagged, so this binds by
#: sector index; see systems.SectorLightProgram.)
HORDE_LIGHT_PERIOD = 14
HORDE_LIGHT_DROPOUT = 0.05

# --- the spoils -----------------------------------------------------------------------------

#: Rare table (Cinder Thralls): (class_name, count, weight, litany name).
RARE_LOOT = (
    ("Shell", 4, 6, "Grave-Shot"),
    ("Stimpack", 1, 3, "Blood Vial"),
    ("Medikit", 1, 2, "Leech Sack"),
    ("GreenArmor", 1, 1, "Graveplate"),
)

#: Legendary relic table (the Rime-Bound): (class, count, weight, relic).
RELIC_LOOT = (
    ("Soulsphere", 1, 2, "Ashen Idol"),
    ("HealthBonus", 1, 5, "Fingerbone Charm"),
    ("ArmorBonus", 1, 3, "Choir Bell"),
)

#: class_name -> litany display name, both tables combined.
DISPLAY_NAMES = {class_name: display
                 for class_name, _count, _weight, display
                 in RARE_LOOT + RELIC_LOOT}
#: Relic names only (for legendary toasts and autotest assertions).
RELIC_NAMES = {class_name: display
               for class_name, _count, _weight, display in RELIC_LOOT}


def make_rare_loot() -> "bd_rpg.LootTable":
    """The Cinder Thralls' spoils (rare)."""
    table = bd_rpg.LootTable("thrall-spoils")
    for class_name, count, weight, _display in RARE_LOOT:
        table.add(class_name, count=count, weight=weight)
    return table


def make_relic_loot() -> "bd_rpg.LootTable":
    """The Rime-Bound's relic (legendary)."""
    table = bd_rpg.LootTable("rime-relic")
    for class_name, count, weight, _display in RELIC_LOOT:
        table.add(class_name, count=count, weight=weight)
    return table


def display_name(class_name: str) -> str:
    """Litany display name for a dropped class (falls back to the raw
    class name for unlisted drops)."""
    return DISPLAY_NAMES.get(str(class_name), str(class_name))


# --- feedback ----------------------------------------------------------------------------

#: Death rattle for the elite roster: DSSGTDTH, the pinky's wet
#: death-rattle — verified present in doom2.wad.
RATTLE_SOUND = "demon/death"

# --- the control strip (persistent display-list text) ------------------------------

STRIP_ID = 73102           # stable display-list id; redraw replaces by id
STRIP_LAYER = 8            # above the world markers and default HUD items
STRIP_Y = 0.955            # bottom of the screen
STRIP_HEIGHT = 0.018       # normalized screen-height fraction per line
STRIP_REFRESH_TICS = 35    # map-local refresh cadence (picks up rebinds)
STRIP_VICTORY_COLOR = (240, 200, 80)

# --- per-hit affinity feedback labels ------------------------------------------------

FEEDBACK_ID_BASE = 73300   # per-monster label ids (base + tid)
FEEDBACK_SECONDS = 0.7     # transient label lifetime
FEEDBACK_THROTTLE_TICS = 10  # one label per monster per this many tics
FEEDBACK_OFFSET_Z = 16.0
FEEDBACK_HEIGHT = 0.02

#: Label colors by affinity band: weakness burns ember-bright,
#: resistance reads sickly, immunity is greyed out.
FEEDBACK_STRONG_COLOR = (255, 140, 40)   # multiplier > 1
FEEDBACK_WEAK_COLOR = (140, 170, 80)     # 0 < multiplier < 1
FEEDBACK_IMMUNE_COLOR = (150, 150, 150)  # multiplier 0


def feedback_text(element: str, mult: float):
    """The affinity label for a retyped hit, or None for a neutral (x1)
    hit: "x2 PYRE" on weakness, "x1/2 ROT" on resistance, "IMMUNE" on
    immunity."""
    mult = float(mult)
    if mult <= 0.0:
        return "IMMUNE"
    if mult == 1.0:
        return None
    if mult > 1.0:
        return f"x{mult:g} {str(element).upper()}"
    frac = Fraction(mult).limit_denominator(8)
    return f"x{frac.numerator}/{frac.denominator} {str(element).upper()}"


# --- elite weakness labels ---------------------------------------------------------

#: affix -> the element that elite fears: fire-born thralls crack under
#: rime, the rime-bound melts under pyre, rot burns away under pyre.
AFFIX_WEAKNESS = {"pyre": "rime", "rime": "pyre", "rot": "pyre"}

WEAKNESS_ID_BASE = 73200   # per-elite label ids (base + roster index)
WEAKNESS_OFFSET_Z = 8.0
WEAKNESS_HEIGHT = 0.016


def weakness_label(name: str, affix: str) -> str:
    """The overhead label for an elite: "Cinder Thrall - weak: RIME"."""
    weakness = AFFIX_WEAKNESS.get(str(affix))
    if weakness is None:
        return str(name)
    return f"{name} - weak: {weakness.upper()}"

# --- prose / litany lines -------------------------------------------------------------------

TOAST_INTRO = "The rite begins. Choose your element; the horde has already chosen you."
LITANY_QUIET = "(the pyre is quiet... for now)"


def toast_relic(relic_name: str) -> str:
    return f"A relic surfaces: {relic_name}."
