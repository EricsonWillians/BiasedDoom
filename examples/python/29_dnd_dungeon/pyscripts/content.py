"""The Delve, pure content: data, factories, and prose.

This module is deliberately free of engine interaction: importing it makes
no ``biaseddoom`` calls, registers no events, and schedules nothing. It
only *constructs* bd_dnd rules objects (plain Python) and holds the
scenario's constants, so every number a designer might tune lives here.

The Delve is a map-agnostic D&D rules layer in the roguelike idiom of
15_roguelike_run: no probed fixtures, no map gating, one clear loop that
works identically on every map. You found a delver (Fighter, Rogue, or
Cleric class cards driving a real ``bd_dnd.CreationWizard``), and every
map deals you a contract: a blood tribute (put down N of the map's
monsters) and a crowned, empowered Warden (the map's most dangerous
monster, worth 5x XP). Kills pay XP with floating popups, levels heal and
toughen your blows, locked doors force or pick open on visible d20 rolls,
rests are safe only in the light and spawn nightmares in the dark, and
the Guild Hound fights at your side.

The only map probed anything is the autotest fixture below: MAP02's line
111 is a real locked red door, used to drive the genuine +use bash path
headlessly. Nothing else keys on a map name.
"""

from __future__ import annotations

import bd_dnd

# --- the delvers ---------------------------------------------------------------

HERO_DEFAULT_NAME = "Delver"
PARTY_NAME = "The Delve"
HOUND_NAME = "Guild Hound"
HOUND_EPITHET = "bred to smell the unquiet dead"
#: The hound's world body is a Demon; its sheet hp is deliberately meaty
#: (the actor's health syncs to the member's hp).
HOUND_HP = 60
COMPANION_CLASS = "Demon"


def make_hound_member() -> "bd_dnd.Character":
    """The Guild Hound as a party member (the Companion binds to it)."""
    return bd_dnd.Character(
        HOUND_NAME, bd_dnd.AbilityScores(con=14), hit_die=8,
        max_hp=HOUND_HP)


# --- the class cards (bd_dnd classes layer) -------------------------------------

#: Per-rest resource pools behind each class active.
ACTIVE_RESOURCES = {"Fighter": "second_wind", "Rogue": "uncanny_dodge",
                    "Cleric": "turn_the_unholy"}


def _grant_active_charge(resource: str):
    """Level-1 feature apply: seed the class active's charge pool."""

    def _apply(character: "bd_dnd.Character") -> None:
        character.grant_resource(resource, 1)

    return _apply


def _raise_active_max(resource: str):
    """Level-2 feature apply: +1 max charge (and a refill on level-up)."""

    def _apply(character: "bd_dnd.Character") -> None:
        character.grant_resource(resource,
                                 character.resource_max.get(resource, 0) + 1)

    return _apply


FIGHTER = bd_dnd.CharacterClass(
    name="Fighter",
    hit_die=10,
    primary_abilities=("str", "con"),
    proficient_saves=("str", "con"),
    class_skills=("athletics", "perception"),
    features={
        1: [{"id": "second_wind",
             "name": "Second Wind",
             "description": "Dig in and rally on command: press [C] to "
                            "heal your hit die plus your level, on the "
                            "sheet and on the body. One charge per rest.",
             "apply": _grant_active_charge("second_wind")}],
        2: [{"id": "seasoned",
             "name": "Seasoned",
             "description": "A second Second Wind charge per rest.",
             "apply": _raise_active_max("second_wind")}],
    },
)

ROGUE = bd_dnd.CharacterClass(
    name="Rogue",
    hit_die=8,
    primary_abilities=("dex", "int"),
    proficient_saves=("dex", "int"),
    class_skills=("sleight_of_hand", "perception"),
    features={
        1: [{"id": "uncanny_dodge",
             "name": "Uncanny Dodge",
             "description": "Press [C] to blur for five seconds: incoming "
                            "damage drops to about a third. One charge per "
                            "rest. Rogues pick locked doors (DEX "
                            "sleight_of_hand, DC - 2) instead of bashing "
                            "them.",
             "apply": _grant_active_charge("uncanny_dodge")}],
        2: [{"id": "uncanny_reserve",
             "name": "Uncanny Reserve",
             "description": "A second Uncanny Dodge charge per rest.",
             "apply": _raise_active_max("uncanny_dodge")}],
    },
)

CLERIC = bd_dnd.CharacterClass(
    name="Cleric",
    hit_die=8,
    primary_abilities=("wis", "cha"),
    proficient_saves=("wis", "cha"),
    class_skills=("religion", "insight"),
    features={
        1: [{"id": "turn_the_unholy",
             "name": "Turn the Unholy",
             "description": "Press [C] to loose a radiant burst: "
                            "12 + 2 per level fire damage to everything "
                            "within 160 units. One charge per rest.",
             "apply": _grant_active_charge("turn_the_unholy")}],
        2: [{"id": "litany",
             "name": "Litany",
             "description": "A second Turn the Unholy charge per rest.",
             "apply": _raise_active_max("turn_the_unholy")}],
    },
)

CLASSES_BY_ID = {cls.name: cls for cls in (FIGHTER, ROGUE, CLERIC)}
CLASS_LIST = (FIGHTER, ROGUE, CLERIC)

#: The one usable class active per class (Custom Action 3, auto-bound to
#: C, console alias ``class_active``). The systems layer reads this table;
#: a new class needs a row here plus an effect branch in
#: ``systems.use_class_active``.
CLASS_ACTIVES = {
    "Fighter": {"id": "second_wind", "name": "Second Wind",
                "resource": "second_wind",
                "effect": "Heal your hit die plus your level."},
    "Rogue": {"id": "uncanny_dodge", "name": "Uncanny Dodge",
              "resource": "uncanny_dodge",
              "effect": "Blur for five seconds: you take about a third "
                        "of the damage."},
    "Cleric": {"id": "turn_the_unholy", "name": "Turn the Unholy",
               "resource": "turn_the_unholy",
               "effect": "A radiant burst burns everything within 160 "
                         "units (12 + 2 per level, fire)."},
}

#: How long the Rogue's blur holds, in tics (five seconds).
UNCANNY_DODGE_TICS = 175
#: The blur's incoming-damage multiplier while it holds.
UNCANNY_DODGE_FACTOR = 0.35
#: The Cleric burst radius, in map units.
TURN_RADIUS = 160.0
#: Turn the Unholy base damage (plus 2 per level), fire-typed.
TURN_BASE_DAMAGE = 12

#: One-click standard-array builds per class card, plus the card prose.
CLASS_PRESETS = {
    "Fighter": {
        "scores": {"str": 15, "dex": 13, "con": 14, "int": 10, "wis": 12,
                   "cha": 8},
        "skills": ("athletics", "perception"),
        "concept": "A shield of the guild, first through the gate.",
        "tip": "STR and CON up front; doors answer to Athletics.",
    },
    "Rogue": {
        "scores": {"str": 10, "dex": 15, "con": 13, "int": 14, "wis": 12,
                   "cha": 8},
        "skills": ("sleight_of_hand", "perception"),
        "concept": "The guild's quiet knife and lockpick.",
        "tip": "DEX first; locks pick at DC - 2 and hits glance off.",
    },
    "Cleric": {
        "scores": {"str": 8, "dex": 12, "con": 13, "int": 10, "wis": 15,
                   "cha": 14},
        "skills": ("religion", "insight"),
        "concept": "A grave-tender carrying the last litany.",
        "tip": "WIS and CHA up front; the burst clears a surround.",
    },
}


def make_delver(class_name: str, hero_name: str = HERO_DEFAULT_NAME
                ) -> "bd_dnd.Character":
    """Build a delver of ``class_name`` through a driven CreationWizard.

    This is the same function the founding window's class cards call: one
    click equals a full wizard run (standard array, preset scores, the
    class's skill picks), and ``finish()`` binds the class, applying the
    level-1 feature package that seeds the class active's charge.
    """
    cls = CLASSES_BY_ID.get(str(class_name))
    if cls is None:
        raise ValueError(f"unknown delve class: {class_name!r}")
    preset = CLASS_PRESETS[cls.name]
    wizard = bd_dnd.CreationWizard()
    wizard.choose_class(cls)
    wizard.set_name(hero_name or HERO_DEFAULT_NAME)
    wizard.use_standard_array()
    for ability, score in preset["scores"].items():
        wizard.set_score(ability, score)
    for skill in preset["skills"]:
        wizard.assign_skill(skill)
    return wizard.finish()


# --- the per-map contract (bd_quests spine) ---------------------------------------

#: Blood tribute: put down at least this many of the map's monsters...
TRIBUTE_MIN = 3
#: ...or a quarter of the census, whichever is larger.
TRIBUTE_DIVISOR = 4
#: The Warden is the census monster with the highest XP-table value
#: (tiebreak: lowest tid), empowered: health x2.5, gold tint, gold ring
#: and overhead title, worth 5x XP however it dies.
WARDEN_HEALTH_MULT = 2.5
WARDEN_XP_MULT = 5
WARDEN_TINT = (255, 200, 60)
#: Map-placed monsters carry tid 0; the crowned Warden gets a real tid so
#: the contract survives a checkpoint by reference. The base bumps until
#: free (probed with bd.actor_ref).
WARDEN_TID_BASE = 9400
#: Contract completion reward: XP onto the sheet plus the paid-off line.
CONTRACT_XP = 200
#: bd.state key for the current map's contract bookkeeping.
CONTRACT_STATE_KEY = "delve_contract"


def tribute_goal_for(census: int) -> int:
    """The tribute count for a census (max(3, census // 4))."""
    return max(TRIBUTE_MIN, int(census) // TRIBUTE_DIVISOR)


def contract_quest_id(map_name: str) -> str:
    """The per-map quest id (the journal accumulates delve history)."""
    return f"delve_{str(map_name).lower()}"


def build_contract_quest(map_name: str, tribute_goal: int,
                         reward_xp: int = CONTRACT_XP) -> "object":
    """The per-map contract quest (fresh object; systems wires it further).

    Parallel objectives: the tribute and the Warden may fall in either
    order. Rewards: XP (dispatched through the log's ``on_xp_reward``
    sink) and the paid-off message. The reward scales with depth and the
    map's modifier; systems computes it.
    """
    from bd_quests import Objective, Quest

    title = str(map_name).upper()
    quest = Quest(contract_quest_id(map_name), f"The Delve: {title}",
                  description=("The guild's standing contract: spill the "
                               "tribute, slay the crowned Warden."),
                  giver="The Delvers' Guild")
    quest.parallel = True
    quest.add_objective(Objective(
        "tribute", f"Blood tribute: put down {tribute_goal} of the map's "
                   f"monsters", count=tribute_goal))
    quest.add_objective(Objective(
        "warden", f"Slay the Warden of {title}"))
    quest.rewards["xp"] = int(reward_xp)
    quest.rewards["message"] = (f"The Warden of {title} is slain; the "
                                f"delve is paid.")
    return quest


# --- combat loop tuning (every map) --------------------------------------------

#: Bonus damage per player level past the first, added to every hit the
#: local player lands on a monster (through the actor_before_damage
#: filter), capped so late levels stay sane.
LEVEL_DAMAGE_BONUS_CAP = 8
#: Reflex saves (the announced DamageSaveRule) against incoming hits:
#: DEX vs. this DC.
SAVE_DC = 12
SAVE_COOLDOWN_TICS = 35

# --- locked doors (every map) ----------------------------------------------------

#: Athletics bash DC (any locked door); Rogues pick instead, DEX
#: sleight_of_hand at DC - 2.
DOOR_DC = 15
ROGUE_DOOR_DC_DELTA = 2
#: Keys lent to the activator for one native door activation. Doom locks
#: 1-3 (card only) and 129-134 (any card or skull) all accept the cards,
#: per wadsrc/static/lockdefs.txt; a failed activation marks the line
#: unbashable instead of spamming.
BASH_KEY_CLASSES = ("RedCard", "BlueCard", "YellowCard")

#: AUTOTEST-ONLY fixture: MAP02's sealed red door (engine special 13
#: ``Door_LockedRaise``, lock 129) is a real locked door the headless test
#: can drive +use against. Nothing in the rules layer keys on these.
DOOR_LINE = 111
DOOR_APPROACH = (752.0, 1328.0, 48.0)
DOOR_FACE_ANGLE = 270.0
DOOR_TRACK_SECTOR = 43

# --- rest with teeth (every map) -------------------------------------------------

#: A long rest is only safe where the light holds: sector light >= 160.
SANCTUARY_LIGHT = 160
#: Below that, sleep rolls the announced nightmare save: DEX vs. this DC.
NIGHTMARE_DC = 12
#: A survived nightmare (saved) grants half the missing HP, no resources.
FITFUL_HEAL_FRACTION = 0.5
#: A failed nightmare spawns this many hostile Demons (rng 1..2) at
#: fit-checked ring spots around the sleeper, tinted dark and titled.
#: Each gets a fresh tid from this base (bumped until free, probed with
#: bd.actor_ref) so the autotest can resolve them.
NIGHTMARE_CLASS = "Demon"
NIGHTMARE_TINT = (56, 48, 80)
NIGHTMARE_TITLE = "Nightmare"
NIGHTMARE_RING_RADIUS = 96.0
NIGHTMARE_TID_BASE = 9500
#: Rest cooldown: thirty seconds of map time between rests.
REST_COOLDOWN_TICS = 35 * 30

# --- health unification ("Blood is the body") ---------------------------------------

#: The sheet's hp pool and the pawn's health are ONE pool, kept at the
#: same ratio (sheet.hp / sheet.max_hp == pawn.health / 100):
#:
#: - pawn damage removes ``damage * sheet.max_hp / 100`` sheet hp
#:   (actor_damaged reports what landed after armor, so armor integrates
#:   naturally by reducing pawn damage pre-sync);
#: - any pawn-health gain the diff detector sees (medikits, stimpacks,
#:   soulspheres, pawn.heal from RPG effects like the reflex refund)
#:   heals the sheet by the same ratio;
#: - RPG-side heals (rests, Second Wind, level-up) heal the pawn back
#:   toward the sheet's ratio (upward only);
#: - level-up and map_load hard-resync the pawn to the sheet's ratio
#:   (both directions), absorbing per-event rounding.
#:
#: Vanilla Doom health is therefore fully integrated: there is one
#: truth, shown as "Blood 8/12 (body 67)". Armor and ammo stay vanilla.
#: The diff detector's interval in tics.
HEALTH_SYNC_TICS = 10

# --- skills that matter in Doom -----------------------------------------------------

#: Athletics CQB training (requires proficiency): +2 damage on the
#: player's hits within this range of the target (map units).
CQB_RANGE = 96.0
CQB_BONUS = 2
#: Perception dead-eye (requires proficiency): +1 damage on the player's
#: hits beyond this range.
DEADEYE_RANGE = 512.0
DEADEYE_BONUS = 1
#: Perception trap sense: the first entry per map into a sector with
#: ``sector.damage > 0`` rolls Perception vs. this DC through the
#: visible announce path; success names the danger.
TRAP_SENSE_DC = 12
#: Religion (requires proficiency): the sanctuary threshold drops from
#: SANCTUARY_LIGHT to this, and the nightmare save rolls WIS instead of
#: DEX ("faith wards the dark").
RELIGION_SANCTUARY_LIGHT = 140
#: Insight: the crosshair examine probe runs on this tic interval; the
#: XP value and threat note show only with proficiency.
EXAMINE_PROBE_TICS = 7
EXAMINE_DISTANCE = 2048.0
#: The examine acquisition cone: a monster counts as "under the
#: crosshair" when its bearing from the pawn is within this floor
#: half-angle, widened at close range by the apparent body size below,
#: and its center pitch sits within EXAMINE_AIM_PITCH_DEG of the pawn's
#: pitch. Acquisition is pure geometry plus the native sight check; the
#: old zero-damage bd.line_attack probe painted bullet decals on every
#: wall it crossed, which read as phantom gunfire.
EXAMINE_AIM_HALF_ANGLE_DEG = 2.5
EXAMINE_AIM_BODY_UNITS = 24.0
EXAMINE_AIM_PITCH_DEG = 30.0
#: The pawn's eye sits this high above its feet (Doom's view height).
EXAMINE_EYE_HEIGHT = 41.0
#: Threat rule ("your judgment"): a monster reads "deadly for your
#: level" when its XP value is at least THREAT_MULTIPLIER times
#: DEADLY_BASE_BOUNTY times your level.
DEADLY_BASE_BOUNTY = 25
THREAT_MULTIPLIER = 4

# --- contract depth and modifiers ----------------------------------------------------

#: bd.state keys: contracts completed (depth) and the modifier seed.
DEPTH_STATE_KEY = "delve_depth"
SEED_STATE_KEY = "delve_seed"
DEFAULT_DELVE_SEED = 29
#: Depth scaling: the Warden's health multiplier grows per contract
#: completed (capped), its XP multiplier grows (capped), the tribute
#: goal grows by one per depth (capped at census - 1), and the contract
#: reward grows by 50 XP per depth.
WARDEN_HEALTH_MULT_PER_DEPTH = 0.25
WARDEN_HEALTH_MULT_CAP = 5.0
WARDEN_XP_MULT_PER_DEPTH = 1
WARDEN_XP_MULT_CAP = 10
TRIBUTE_GOAL_PER_DEPTH = 1
CONTRACT_XP_PER_DEPTH = 50
#: One modifier per map, rolled deterministically from
#: ``bd.state["delve_seed"]`` (default DEFAULT_DELVE_SEED) plus a stable
#: byte-sum salt of the map name. The plain contract keeps ~30% weight.
CONTRACT_MODIFIERS = (
    {"id": "iron_warden", "name": "IRON WARDEN", "weight": 14,
     "brief": "The Warden is twice as tough and pays two more shares."},
    {"id": "horde", "name": "HORDE", "weight": 14,
     "brief": "Twice the tribute, plus a hundred for the trouble."},
    {"id": "dark_delve", "name": "DARK DELVE", "weight": 14,
     "brief": "The light thins; the dark bites harder; the pay rises."},
    {"id": "guild_bounty", "name": "GUILD BOUNTY", "weight": 14,
     "brief": "The guild pays double for this one."},
    {"id": "bloodhound", "name": "BLOODHOUND", "weight": 14,
     "brief": "The hound hunts harder: half again its blood this map."},
    {"id": "plain", "name": "plain contract", "weight": 30,
     "brief": ""},
)
#: IRON WARDEN: the Warden's health multiplier doubles on top, and its
#: XP multiplier gains this many shares (capped).
IRON_WARDEN_XP_BONUS = 2
#: DARK DELVE: the sanctuary threshold rises by this, the nightmare DC
#: by DARK_DELVE_NIGHTMARE_DC_DELTA, and the contract reward multiplies.
DARK_DELVE_SANCTUARY_DELTA = 20
DARK_DELVE_NIGHTMARE_DC_DELTA = 2
DARK_DELVE_XP_MULT = 1.5
#: GUILD BOUNTY: the contract reward multiplies by this.
GUILD_BOUNTY_XP_MULT = 2
#: HORDE: the tribute goal doubles (still capped at census - 1) and the
#: contract reward gains this flat bonus.
HORDE_XP_BONUS = 100
#: BLOODHOUND: the hound member's max hp multiplies by this, this map.
BLOODHOUND_HP_MULT = 1.5

# --- level-up boons -------------------------------------------------------------------

#: Every level-up queues a pick-one-of-three. Effects apply through
#: ``systems.pick_boon`` (the same function the chooser buttons call);
#: pending and taken boons persist through bd.state.
BOONS = (
    {"id": "toughness", "name": "Toughness",
     "effect": "+2 max Blood (sheet max hp)."},
    {"id": "deadly", "name": "Deadly",
     "effect": "+1 damage on all your hits (through the level filter, "
               "stacks)."},
    {"id": "prepared", "name": "Prepared",
     "effect": "+1 max class-active charge."},
)
#: bd.state key for boon persistence.
BOONS_STATE_KEY = "delve_boons"
#: The Toughness boon's sheet max-hp gain.
TOUGHNESS_HP = 2

# --- display-list ids -------------------------------------------------------------

#: Below bd_horror's 888000 vignette base and the 999000+ toast range.
STRIP_ID = 887100          # strip line 1 (the hero)
STRIP2_ID = 887101         # strip line 2 (rest / contract / depth)
EXAMINE_LINE_ID = 887102   # the crosshair examine line (only while held)
XP_POPUP_BASE = 887300      # + this modulo XP_POPUP_SLOTS
XP_POPUP_SLOTS = 8
CHECK_POPUP_ID = 887310     # the floating d20 readout over the player
WARDEN_RING_ID = 887320
WARDEN_TITLE_ID = 887321
PLAYER_RING_ID = 887330     # level-up / contract fanfare bursts
NIGHTMARE_LABEL_BASE = 887340  # + nightmare index

#: How often the persistent strip refreshes.
HUD_REFRESH_TICS = 35
#: Seconds the d20 readout and XP popups float.
POPUP_SECONDS = 0.8
XP_POPUP_SECONDS = 0.7

CHECKPOINT_NAME = "the_delve_example"

# --- sounds (logical names; lumps verified present in doom2.wad) ------------------

DEATH_KNELL_SOUND = "knight/death"
LEVEL_UP_SOUND = "misc/p_pkup"
CONTRACT_SOUND = "misc/p_pkup"

# --- prose / toast lines -----------------------------------------------------------

INTRO_CENTER = "DELVE: {map}: spill the tribute, slay the Warden."
INTRO_TOAST_GOAL = ("The contract stands: put down the tribute, then "
                    "slay the crowned Warden.")
INTRO_TOAST_KEYS = "{wind} class active, {rest} rest, {sheet} sheet."
INTRO_TOAST_HOUND = "The Guild Hound fights at your side."
TOAST_CONTRACT = "New contract: {map}. The Warden wears gold."
TOAST_CONTRACT_DONE = "The delve is paid."
TOAST_NO_QUARRY = "No quarry here; the guild passes this place by."
TOAST_BASH_HINT = "Barred. Use it again to force it."
TOAST_SANCTUARY = "The light keeps the dark from your dreams."
TOAST_FITFUL = "You wake before the dream takes hold."
TOAST_NIGHTMARE = "The dark dreams with teeth."
TOAST_REST_COOLDOWN = "You cannot rest yet; the road still rings in you."
TOAST_KNELL = "{name} falls; the guild tolls its bell."
TOAST_WARDEN_SLAIN = "The Warden of {map} is slain."
TOAST_TRAP_SENSE = "Your skin prickles: the floor is death here."
TOAST_BOON = "The guild offers a boon; choose (1, 2, 3)."


def knell_toast(member_name: str) -> str:
    """The death-toll toast line for a party member's companion."""
    return TOAST_KNELL.format(name=str(member_name))
