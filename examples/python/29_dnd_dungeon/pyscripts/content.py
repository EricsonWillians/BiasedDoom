"""The Sunken Crypt — pure content: data, factories, and prose.

This module is deliberately free of engine interaction: importing it makes
no ``biaseddoom`` calls, registers no events, and schedules nothing. It
only *constructs* bd_dnd rules objects (plain Python) and holds the
scenario's constants, so every number a designer might tune lives here.
Morrow himself is data: the ``FIGHTER`` CharacterClass and a driven
``CreationWizard`` (see ``make_hero_via_wizard``) rebuild his probed
sheet exactly, level-1 features included.

The scenario: you are Morrow, a grave-hardened fighter delving the whole
WAD. The rules layer — kill XP from the full bestiary, level-toughened
blows, light-ruled rests that heal body and sheet alike, locked doors
bashed with Athletics, fireballs rolled with on a DEX save, and the
Hollow Warden with its bound crypt hound at your heel — rides every map
of the game, not one. MAP02 keeps the probed set pieces that make it the
crypt proper: the sealed red door on line 111 (bashed with Athletics
instead of the red key), the dart trap in the entrance chamber (sector 7,
tag 13), and the candle/corpse-light programs on tags 13/7/12.

Map probe findings (verified against doom2.wad and the live engine — see
README.md for how to reproduce): GZDoom translates Doom-format specials,
so the classic red door (raw special 135) appears as engine special 13
``Door_LockedRaise`` with args ``[7, 64, 0, 129, 0]`` (lock 129 = red
key) on lines 111/112, approached from (752, 1328) facing angle 270.
Using it without the key fires ``line_activation_failed`` with
``reason == "locked"``. ``Line.activate`` runs the special directly and
the engine's lock check rejects a null activator, so the bash lends the
Doom card keys to the player for a single native
``activate(activator, clear=True)`` and reclaims them afterwards
(systems.DoorBashRules).

Sector tags (verified live with ``bd.sectors(tag=...)``): sector 7 (the
player start room, light 144) carries tag 13; sectors 40-43 (the drowned
passage beyond the red door, light 128) carry tag 7; sector 47 (the
corpse-light hall, light 96) carries tag 12. The door approach point
sits in untagged sector 38 and the south corridor in untagged sector 0,
which makes them safe fixtures for the autotest's forced-darkness rest
experiments (no light program ever rewrites them).
"""

from __future__ import annotations

import bd_dnd

# --- the delvers ---------------------------------------------------------------

HERO_NAME = "Morrow"
HERO_EPITHET = "the Grave-Hardened"
HERO_PROSE = (
    "Morrow has buried more friends than he has kept. The crypt does not "
    "frighten him; what frightens him is that the crypt knows it."
)
#: Morrow's probed sheet: rolled-method creation (the wizard's set_score
#: accepts values above the standard array), two class-skill picks, and the
#: str/con save proficiencies of the Fighter class below.
HERO_ABILITY_SCORES = {"str": 16, "dex": 12, "con": 14, "int": 10,
                       "wis": 12, "cha": 8}
WARDEN_NAME = "The Hollow Warden"
WARDEN_EPITHET = "who died on watch and kept walking"
WARDEN_PROSE = (
    "It speaks little since the drowning. Its hound never leaves your "
    "shadow, and its hexes still bite."
)
PARTY_NAME = "The Last Descent"
COMPANION_CLASS = "Demon"        # the Warden's bound crypt hound
COMPANION_EPITHET = "crypt hound"


def make_hero() -> "bd_dnd.Character":
    """Morrow, the grave-hardened fighter (d10 hit die).

    Kept as the stable entry point; delegates to
    :func:`make_hero_via_wizard`, which builds the same sheet through the
    ``bd_dnd`` classes layer.
    """
    return make_hero_via_wizard()


def make_warden() -> "bd_dnd.Character":
    """The Hollow Warden, a hollowed-out hexer (d6 hit die)."""
    return bd_dnd.Character(
        WARDEN_NAME,
        bd_dnd.AbilityScores(int=16, wis=14),
        hit_die=6,
    )


def make_party(hero: "bd_dnd.Character") -> "bd_dnd.Party":
    """The two-member descent: the hero plus the Hollow Warden."""
    return bd_dnd.Party([hero, make_warden()], name=PARTY_NAME)


def make_companion() -> "bd_dnd.Companion":
    """The Warden's bound crypt hound (a friendly Demon follower)."""
    return bd_dnd.Companion(WARDEN_NAME, class_name=COMPANION_CLASS)


# --- the crypt (map probe constants, MAP02) --------------------------------------

DOOR_LINE = 111                        # sealed red door (lock 129)
DOOR_APPROACH = (752.0, 1328.0, 48.0)  # in front of the door, face angle 270
DOOR_FACE_ANGLE = 270.0
DOOR_TRACK_SECTOR = 43                 # the door's sector (tag 7)
DOOR_DC = 15                           # Athletics bash DC (any locked door)
#: Keys lent to the activator for one native door activation. Doom locks
#: 1-3 (card only) and 129-134 (any card or skull) all accept the cards,
#: per wadsrc/static/lockdefs.txt; a failed activation marks the line
#: unbashable instead of spamming.
BASH_KEY_CLASSES = ("RedCard", "BlueCard", "YellowCard")
TRAP_TAG = 13                          # sector 7: the entrance chamber
TRAP_DC = 13
TRAP_DAMAGE = "2d6"
TRAP_COOLDOWN_TICS = 175
START_POS = (1104.0, 1984.0, 0.0)
AWAY_POS = (1104.0, 1696.0, 0.0)       # untagged sector 0: test fixture
MONSTER_TIDS = (9301, 9302, 9303)      # two thralls + one imp = 100 XP
# (class_name, tid, lateral offset from the corridor, XP) — 25 + 25 + 50.
MONSTER_SPAWNS = (
    ("ZombieMan", 9301, -64.0),
    ("ZombieMan", 9302, 64.0),
    ("DoomImp", 9303, 0.0),
)
CHECKPOINT_NAME = "sunken_crypt_example"
#: The map that carries the probed set pieces (door bash fixture, dart
#: trap, light programs). Everywhere else the generic rules layer runs.
CRYPT_MAP = "MAP02"

# --- torch and darkness (bd_horror light programs) --------------------------------

#: Tallow candles guttering in the entrance chamber (tag 13, base 144).
CANDLE_TAGS = (13,)
CANDLE_AMPLITUDE = 26
CANDLE_PERIOD = 9
#: Grave-candles along the drowned passage beyond the red door (tag 7,
#: base 128) — a slower, meaner walk.
PASSAGE_TAGS = (7,)
PASSAGE_AMPLITUDE = 18
PASSAGE_PERIOD = 13
#: The corpse-light in the flooded hall (tag 12, sector 47, base 96): a
#: fluorescent-style program, steady with full dropouts into the black.
CORPSE_LIGHT_TAGS = (12,)
CORPSE_LIGHT_DROPOUT = 0.08
CORPSE_LIGHT_PERIOD = 4

#: A long rest is only safe where the light holds: sector light >= 160.
SANCTUARY_LIGHT = 160
#: Below that, sleep is a nightmare: DEX save vs. this DC.
NIGHTMARE_DC = 12
#: The resource the dark eats when the nightmare save fails.
NIGHTMARE_RESOURCE = "resolve"
NIGHTMARE_RESOURCE_CHARGES = 2
#: A survived nightmare grants half the missing HP, no resources.
FITFUL_HEAL_FRACTION = 0.5

# --- progression tuning (every map) ------------------------------------------------

#: Bonus damage per player level past the first, added to every hit the
#: local player lands on a monster (through the actor_before_damage
#: filter), capped so late levels stay sane.
LEVEL_DAMAGE_BONUS_CAP = 8
#: Second Wind: the active heal button (Custom Action 3). One charge per
#: rest; spending it heals the pawn by the hit die plus level.
SECOND_WIND_RESOURCE = "second_wind"
SECOND_WIND_CHARGES = 1
#: Reflex saves (DamageSaveRule) against incoming hits: DEX vs. this DC.
SAVE_DC = 12
SAVE_COOLDOWN_TICS = 35
#: How often the persistent progression strip refreshes.
HUD_REFRESH_TICS = 35

# --- the fighter class (bd_dnd classes layer) --------------------------------------


def _grant_grave_hardened(character: "bd_dnd.Character") -> None:
    """The level-1 Grave-Hardened feature: seed the resolve pool.

    Runs through ``bind_class`` at the end of ``CreationWizard.finish()``,
    so the pool exists from the moment Morrow is built (main.py no longer
    grants it manually at engine_start).
    """
    character.grant_resource(NIGHTMARE_RESOURCE, NIGHTMARE_RESOURCE_CHARGES)


def _grant_second_wind(character: "bd_dnd.Character") -> None:
    """The level-1 Second Wind feature: seed the active heal charge."""
    character.grant_resource(SECOND_WIND_RESOURCE, SECOND_WIND_CHARGES)


FIGHTER = bd_dnd.CharacterClass(
    name="Fighter",
    hit_die=10,
    primary_abilities=("str", "dex"),
    proficient_saves=("str", "con"),
    class_skills=("athletics", "perception"),
    features={
        1: [{"id": "grave_hardened",
             "name": "Grave-Hardened",
             "description": "The crypt does not frighten you; what "
                            "frightens you is that the crypt knows it. "
                            "Two resolve charges per rest.",
             "apply": _grant_grave_hardened},
            {"id": "second_wind",
             "name": "Second Wind",
             "description": "Dig in and rally on command: spend the "
                            "charge (Custom Action 3) to heal your "
                            "wounds by the hit die plus your level. "
                            "One charge per rest.",
             "apply": _grant_second_wind}],
        2: [{"id": "death_knell",
             "name": "Death Knell",
             "description": "Flavor only: you have heard the deep bell "
                            "often enough to know it will ring for you "
                            "last. (The actual knell wiring lives in "
                            "systems.wire_knell.)",
             "apply": None}],
    },
)


class _CreationDie:
    """Scripted die for the wizard's rolled-method creation.

    ``use_rolled`` needs an rng and Morrow's probed sheet overrides every
    roll afterwards, so the rolls themselves just return a fixed middling
    die; the engine's script RNG is never touched.
    """

    def randint(self, lo, hi):
        return (lo + hi) // 2


def make_hero_via_wizard() -> "bd_dnd.Character":
    """Morrow, rebuilt through the ``bd_dnd`` classes layer.

    The rolled score method unlocks ``set_score`` values above the
    standard array (Morrow's probed sheet predates it); the wizard then
    validates name, class, scores, and the two class-skill picks, and
    ``finish()`` binds the Fighter (``character.class_id``,
    ``character.cls``) while applying the level-1 Grave-Hardened and
    Second Wind features, which seed the resolve and second_wind pools.
    The result is the exact sheet this module has always shipped: str 16 /
    dex 12 / con 14 / int 10 / wis 12 / cha 8, d10 hit die, athletics and
    perception proficiency, str/con saves, resolve 2/2, second_wind 1/1.
    """
    wizard = bd_dnd.CreationWizard(rng=_CreationDie())
    wizard.choose_class(FIGHTER)
    wizard.set_name(HERO_NAME)
    wizard.use_rolled()
    for ability, score in HERO_ABILITY_SCORES.items():
        wizard.set_score(ability, score)
    wizard.assign_skill("athletics")
    wizard.assign_skill("perception")
    return wizard.finish()


# --- sounds (logical names; lumps verified present in doom2.wad) -------------------

#: Deep bell-ish toll for the companion's death: DSKNTDTH, the Hell
#: Knight's death bellow — the closest thing doom2.wad has to a knell.
DEATH_KNELL_SOUND = "knight/death"
LEVEL_UP_SOUND = "misc/p_pkup"

# --- prose / toast lines -------------------------------------------------------------

TOAST_INTRO = "The Sunken Crypt exhales."
TOAST_LEVEL_UP = "The crypt acknowledges your ascent."
TOAST_SANCTUARY = "The light keeps the dark from your dreams."
TOAST_NIGHTMARE = "The dark dreams with you."
TOAST_FITFUL = "You wake before the dream takes hold."
TOAST_SAVE = "You roll with the hit."
TOAST_SECOND_WIND = "You dig in; the wound closes."
TOAST_NO_WIND = "No wind left in you. Rest first."
TOAST_KNELL = "{name} falls silent; a deep bell tolls below."


def knell_toast(member_name: str) -> str:
    """The death-toll toast line for a party member's companion."""
    return TOAST_KNELL.format(name=str(member_name))
