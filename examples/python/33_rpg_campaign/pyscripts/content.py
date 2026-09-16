"""Ashvale Crossing - pure content: classes, NPCs, quests, trees, strings.

**Import-safe**: no engine calls at import time. The three character classes,
the four NPC definitions (with their dialogue trees), the three quests, and
every player-visible string are built by factory functions and plain data
tables the systems layer instantiates from event handlers. Every number a
designer might tune lives here, so the fixture can be re-voiced and re-balanced
without touching logic.

Loaded as manifest entry 1 of 4; the trailing ``_LiveAlias`` registration lets
later manifest entries ``import ashvale_content`` (the engine registers
manifest modules under mangled names only after execution, so the proxy reads
through to this module's live globals).

The capstone combines every shipped RPG framework in one mini-campaign on
Doom II MAP01:

- **bd_dnd**: three ``CharacterClass`` definitions and the ``CreationWizard``
  the systems layer drives at the start of the run, plus the creation
  guidance layer: ``CLASS_CONCEPTS``/``ABILITY_BLURBS``/``SKILL_BLURBS``/
  ``MODIFIER_HINT`` explain the rules in the wizard window, ``CLASS_PRESETS``
  ships three one-click builds per class, and ``apply_preset``/
  ``class_briefing`` are the shared helpers the UI and the autotest both use.
  ``CLASS_ACTIVES`` defines each class's one usable active (Custom Action 3,
  auto-bound to C): a per-rest resource spent on a real pawn effect.
- **bd_npcs**: four ``NPCDefinition`` entries (spawn offsets probe-verified on
  MAP01), a ``Shop``, a ``HealerService``, and a ``TrainerService``.
- **bd_dialogue**: one validated ``Dialogue`` tree per talkable NPC with
  skill checks, disposition gates, conditions, effects, and player-log writes.
- **bd_quests**: three quests (yard kill tracker, cache pickup tracker, and
  the Korr recruitment pointer) with xp/disposition reward hooks.
"""

# --- fixture constants ----------------------------------------------------------

EXAMPLE_TITLE = "Ashvale Crossing"
PARTY_NAME = "The Ashvale Watch"
HERO_DEFAULT_NAME = "Rook"
KORR_NAME = "Korr"
CHECKPOINT_NAME = "ashvale_crossing_example"
SHOP_STATE_KEY = "ashvale_shop"

TALK_RANGE = 192.0
NPC_TID_BASE = 9100            # sera 9100, dobb 9101, wren 9102, korr 9103
YARD_TIDS = (9201, 9202, 9203, 9204, 9205)
CACHE_TID = 9209

#: Display-list ids for the in-world quest markers and the persistent HUD
#: strip. A fresh 95000+ range, clear of the frameworks' own hud_text ids
#: (90001+) and the companion TID base (31000+). ``marker_state`` in the
#: systems layer keys on these ids.
MARKER_SERA_ID = 95001         # gold "!" over Sera while clear_yard waits
MARKER_DOBB_ID = 95002         # gold "!" over Dobb while fetch_cache waits
MARKER_KORR_ID = 95003         # gold "!" over Korr while watch_grows is open
MARKER_YARD_BEACON_ID = 95004  # vertical beacon at YARD_CENTER (yard active)
MARKER_CACHE_BEACON_ID = 95005  # vertical beacon at CACHE_POS (fetch active)
HUD_STRIP_STATUS_ID = 95010    # bottom strip, line 1: hero status
HUD_STRIP_OBJECTIVE_ID = 95011  # bottom strip, line 2: tracked objective
#: The marker gold (r, g, b) shared by the "!" labels and the beacons.
MARKER_GOLD = (255, 200, 60)

#: Currency candidates, most wanted first. The systems layer spawns the first
#: candidate at a far map edge on the first map_load and destroys it; on
#: failure the shop and services fall back to the next entry. GZDoom ships the
#: Strife ``Coin`` definition in the base zscript, so the probe succeeds even
#: on Doom II; on configurations where it is missing, "Clip" (10 bullets as
#: one unit of trade) keeps the economy working. Heretic/Hexen mods would list
#: "GoldCoin" (Heretic) or "GoldCoin"/"SilverCoin" (Hexen) first.
CURRENCY_CLASSES = ("Coin", "Clip")
CURRENCY_NAMES = {"Coin": "gold", "Clip": "clips"}
COIN_PROBE_POS = (2048.0, 2048.0, 0.0)   # far corner of the void, never seen

# --- the game table ---------------------------------------------------------------
#: Every spawn class name in one table. The engine exposes no IWAD query, so a
#: mod picks the table for its game (or runs try/except spawn probes per name).
#: Heretic/Hexen substitutions are noted per entry.
GAME_CONTENT = {
    "yard_monster": "ZombieMan",     # Heretic: "HereticImp"; Hexen: "Ettin"
    "sera_class": "ZombieMan",       # Heretic: "Wizard";  Hexen: "Ettin"
    "dobb_class": "ShotgunGuy",      # Heretic: "Knight"; Hexen: "FireDemon"
    "wren_class": "DoomImp",         # Heretic: "Clink"; Hexen: "FireDemon"
    "korr_class": "Demon",           # Heretic: "Beast"; Hexen: "Centaur"
    "companion_class": "Demon",      # Heretic: "Beast"; Hexen: "Centaur"
    "cache_class": "ClipBox",        # Heretic: "CrossbowAmmo"; Hexen: "Mana1"
}

# --- MAP01 fixture geometry (probe-verified, see README) ---------------------------
#: The west entry hall (floor 56, pillars at the failed probe spots). The yard
#: line is a straight row of five verified-open zombie slots; the cache sits
#: at the far west end. Player 1 start is (-96, 784, 56) facing north.
YARD_SPAWNS = ((-224.0, 800.0, 56.0), (-256.0, 800.0, 56.0),
               (-288.0, 800.0, 56.0), (-320.0, 800.0, 56.0),
               (-352.0, 800.0, 56.0))
YARD_CENTER = (-288.0, 800.0, 56.0)
CACHE_POS = (-416.0, 800.0, 56.0)

#: NPC spawn offsets relative to the player start (dx, dy); each slot was
#: fit-probed with the exact actor class on MAP01.
NPC_SPAWNS = {
    "sera": (0.0, -48.0),       # ZombieMan at (-96, 736)
    "dobb": (-64.0, -48.0),     # ShotgunGuy at (-160, 736)
    "wren": (96.0, 16.0),       # DoomImp at (0, 800)
    "korr": (0.0, 48.0),        # Demon at (-96, 832)
}
NPC_TINTS = {
    "sera": 0xC07020,           # ember brown
    "dobb": 0x7090C0,           # quartermaster blue
    "wren": 0x70C070,           # healer green
    "korr": 0xC05050,           # worn red
}
NPC_DISPLAY_NAMES = {
    "sera": "Sera Voss",
    "dobb": "Dobb the Quartermaster",
    "wren": "Wren",
    "korr": "Korr",
}

# --- character classes (bd_dnd) ------------------------------------------------------

import bd_dnd


def _grant_second_wind(character):
    character.grant_resource("second_wind", 1)


def _raise_second_wind_max(character):
    """Press On: one more Second Wind charge per rest."""
    character.grant_resource(
        "second_wind", int(character.resource_max.get("second_wind", 0)) + 1)


def _apply_skirmisher(character):
    notes = dict(getattr(character, "mod_notes", {}) or {})
    notes["skirmisher_damage"] = 1
    character.mod_notes = notes


def _grant_uncanny_step(character):
    character.grant_resource("uncanny_step", 1)


def _grant_kindled_light(character):
    character.grant_resource("light", 3)


def _raise_light_max(character):
    """Warding Flame: one more light charge per rest."""
    character.grant_resource(
        "light", int(character.resource_max.get("light", 0)) + 1)


#: The classic standard array as an unordered multiset.
STANDARD_ARRAY = (15, 14, 13, 12, 10, 8)

MERCENARY = bd_dnd.CharacterClass(
    name="Mercenary", hit_die=10,
    primary_abilities=("str",),
    proficient_saves=("str", "con"),
    class_skills=("athletics", "intimidation", "perception"),
    features={
        1: [{"id": "second_wind", "name": "Second Wind",
             "description": "Press [C] to rally: heal your hit die plus "
                            "your level, one second_wind charge per rest.",
             "apply": _grant_second_wind}],
        2: [{"id": "press_on", "name": "Press On",
             "description": "Numb to the ache: Second Wind holds a second "
                            "charge per rest.",
             "apply": _raise_second_wind_max}],
    },
    resources={"second_wind": 1},
    starting_equipment=(("Clip", 2), ("Shell", 4), ("GreenArmor", 1)),
)

SCOUT = bd_dnd.CharacterClass(
    name="Scout", hit_die=8,
    primary_abilities=("dex",),
    proficient_saves=("dex", "int"),
    class_skills=("stealth", "sleight_of_hand", "perception", "acrobatics"),
    features={
        1: [{"id": "skirmisher", "name": "Skirmisher",
             "description": "+1 damage on every hit you land (a mod note "
                            "the combat layer reads).",
             "apply": _apply_skirmisher}],
        2: [{"id": "uncanny_step", "name": "Uncanny Step",
             "description": "Press [C] to blur for five seconds: damage "
                            "taken drops to about a third. One uncanny_step "
                            "charge per rest.",
             "apply": _grant_uncanny_step}],
    },
    resources={},
    starting_equipment=(("Clip", 1), ("Shell", 2), ("GreenArmor", 1)),
)

LIGHTKEEPER = bd_dnd.CharacterClass(
    name="Lightkeeper", hit_die=8,
    primary_abilities=("wis",),
    proficient_saves=("wis", "cha"),
    class_skills=("insight", "medicine", "religion", "arcana"),
    features={
        1: [{"id": "kindled_light", "name": "Kindled Light",
             "description": "Carry the ember: press [C] to loose a radiant "
                            "burst, three light charges per rest.",
             "apply": _grant_kindled_light}],
        2: [{"id": "warding_flame", "name": "Warding Flame",
             "description": "The light burns brighter: a fourth light "
                            "charge per rest.",
             "apply": _raise_light_max}],
    },
    resources={"light": 3},
    starting_equipment=(("Clip", 1), ("Stimpack", 2), ("GreenArmor", 1)),
)

CLASSES_BY_ID = {cls.name: cls for cls in (MERCENARY, SCOUT, LIGHTKEEPER)}
CLASS_LIST = (MERCENARY, SCOUT, LIGHTKEEPER)

#: The one usable class active per class, fired by Custom Action 3
#: (auto-bound to C, console alias ``class_active``). Each spends one charge
#: of the named per-rest resource and applies a real pawn effect; the systems
#: layer reads this table, so a new class only needs a row here plus its
#: effect branch in ``systems.use_class_active``.
CLASS_ACTIVES = {
    "Mercenary": {"id": "second_wind", "name": "Second Wind",
                  "resource": "second_wind",
                  "effect": "Heal your hit die plus your level."},
    "Scout": {"id": "uncanny_step", "name": "Uncanny Step",
              "resource": "uncanny_step",
              "effect": "Blur for five seconds: you take about a third of "
                        "the damage."},
    "Lightkeeper": {"id": "light", "name": "Light",
                    "resource": "light",
                    "effect": "A radiant burst burns everything within 160 "
                              "units (12 + 2 per level, fire)."},
}

#: How long the Scout's blur holds, in tics (five seconds).
UNCANNY_STEP_TICS = 175
#: The blur's incoming-damage multiplier while it holds.
UNCANNY_STEP_FACTOR = 0.35
#: The Lightkeeper burst radius, in map units.
LIGHT_RADIUS = 160.0

#: A sensible standard-array assignment per class (the autotest and the
#: screenshot pose use the Mercenary row).
SUGGESTED_ARRAYS = {
    "Mercenary": {"str": 15, "dex": 13, "con": 14, "int": 8, "wis": 12,
                  "cha": 10},
    "Scout": {"str": 10, "dex": 15, "con": 13, "int": 12, "wis": 14,
              "cha": 8},
    "Lightkeeper": {"str": 8, "dex": 12, "con": 13, "int": 10, "wis": 15,
                    "cha": 14},
}

# --- creation guidance (what the rules mean, in this mod) ----------------------------

#: One line per ability score: what it governs at Ashvale's table. Kept
#: short enough to ride the wizard's ability rows unclipped.
ABILITY_BLURBS = {
    "str": "Melee muscle; bashing and forcing.",
    "dex": "Speed and nerve; stealth and locks.",
    "con": "Toughness; feeds hit points.",
    "int": "Lore and reason; hexes and history.",
    "wis": "Senses; traps, wounds, the hidden.",
    "cha": "Presence; every negotiation.",
}

#: How a score becomes a modifier, and how a check works.
MODIFIER_HINT = ("Modifier = (score - 10) / 2, rounded down: 8 is -1, "
                 "10 is +0, 14 and 15 are +2, 16 is +3. A check rolls "
                 "d20 + modifier (+2 when trained in the skill) against "
                 "the DC.")

#: One line per class skill: what it does here and (its ability).
SKILL_BLURBS = {
    "athletics": "Force doors, climb, grapple.",
    "intimidation": "Cow the unwilling into talking or backing down.",
    "perception": "Spot traps, ambushes, and the hidden.",
    "stealth": "Move unseen past the dead.",
    "sleight_of_hand": "Pick locks and pockets.",
    "acrobatics": "Keep footing and slip hazards.",
    "insight": "Read a liar before he finishes lying.",
    "medicine": "Stabilize and treat wounds; Wren can train it.",
    "religion": "Know the rites of the old faiths.",
    "arcana": "Know the workings of hexes.",
}

#: Per class: a concept line and a build tip for the wizard's briefing.
CLASS_CONCEPTS = {
    "Mercenary": {
        "concept": "A paid blade of the watch, first through the gate.",
        "tip": "Put the best scores in STR and CON; Intimidation runs "
               "on CHA.",
    },
    "Scout": {
        "concept": "The crossing's eyes, lockpicks, and quiet knife.",
        "tip": "DEX first, always; WIS keeps a Scout alive between "
               "fights.",
    },
    "Lightkeeper": {
        "concept": "Carrier of the kindled light against the dark.",
        "tip": "WIS first; CHA carries every negotiation the light "
               "cannot.",
    },
}

#: One-click builds per class. Each preset is a complete, valid standard
#: array assignment plus the class skill picks; ``apply_preset`` drives
#: the wizard through one, so the UI's preset buttons and the autotest
#: share exactly the same path. Scores stay inside the standard array
#: multiset on purpose: a preset never beats what a careful player could
#: build by hand.
CLASS_PRESETS = {
    "Mercenary": (
        {"id": "pit_fighter", "name": "Pit Fighter",
         "concept": "Lead with the blade: the best Athletics checks and "
                    "the most hit points.",
         "scores": {"str": 15, "dex": 13, "con": 14, "int": 8, "wis": 12,
                    "cha": 10},
         "skills": ("athletics", "intimidation", "perception")},
        {"id": "watch_sergeant", "name": "Watch Sergeant",
         "concept": "Command presence: Intimidation (CHA) lands harder, "
                    "and the front line still holds.",
         "scores": {"str": 15, "dex": 10, "con": 13, "int": 8, "wis": 12,
                    "cha": 14},
         "skills": ("athletics", "intimidation", "perception")},
        {"id": "old_survivor", "name": "Old Survivor",
         "concept": "The hardest to bury: top CON for hit points, sharp "
                    "Perception for what waits in the yard.",
         "scores": {"str": 14, "dex": 12, "con": 15, "int": 8, "wis": 13,
                    "cha": 10},
         "skills": ("athletics", "intimidation", "perception")},
    ),
    "Scout": (
        {"id": "ghost", "name": "Ghost",
         "concept": "Unseen and alert: the best Stealth and Perception "
                    "in the crossing.",
         "scores": {"str": 10, "dex": 15, "con": 12, "int": 13, "wis": 14,
                    "cha": 8},
         "skills": ("stealth", "perception", "acrobatics")},
        {"id": "lockpick", "name": "Lockpick",
         "concept": "Fingers first: Sleight of Hand for locks and lifts, "
                    "with the wits to case the mark.",
         "scores": {"str": 10, "dex": 15, "con": 12, "int": 14, "wis": 13,
                    "cha": 8},
         "skills": ("stealth", "sleight_of_hand", "perception")},
        {"id": "skirmisher_scout", "name": "Skirmisher",
         "concept": "Fights in the open: more hit points, Acrobatics to "
                    "slip what cannot be dodged.",
         "scores": {"str": 10, "dex": 15, "con": 14, "int": 13, "wis": 12,
                    "cha": 8},
         "skills": ("acrobatics", "perception", "sleight_of_hand")},
    ),
    "Lightkeeper": (
        {"id": "chirurgeon", "name": "Chirurgeon",
         "concept": "The field medic: Medicine keeps the watch "
                    "breathing.",
         "scores": {"str": 8, "dex": 10, "con": 12, "int": 13, "wis": 15,
                    "cha": 14},
         "skills": ("insight", "medicine", "religion")},
        {"id": "exorcist", "name": "Exorcist",
         "concept": "Student of the hex: Arcana and Religion name the "
                    "enemy before it speaks.",
         "scores": {"str": 8, "dex": 10, "con": 12, "int": 14, "wis": 15,
                    "cha": 13},
         "skills": ("arcana", "religion", "insight")},
        {"id": "confessor", "name": "Confessor",
         "concept": "Reads every soul in the room: Insight first, a warm "
                    "voice second.",
         "scores": {"str": 8, "dex": 10, "con": 13, "int": 12, "wis": 15,
                    "cha": 14},
         "skills": ("insight", "medicine", "arcana")},
    ),
}


def apply_preset(wizard, preset):
    """Drive ``wizard`` through one preset (standard array + skills).

    Shared by the UI's preset buttons and the autotest. The class must
    already be chosen (``wizard.choose_class``); the method is reset to
    the standard array, every score is assigned from the preset, and the
    skill picks are replaced with the preset's. Returns the wizard.
    """
    wizard.use_standard_array()
    for ability, value in preset["scores"].items():
        wizard.set_score(ability, value)
    for skill in list(wizard.skills):
        wizard.unassign_skill(skill)
    for skill in preset["skills"]:
        wizard.assign_skill(skill)
    return wizard


def class_briefing(cls):
    """Derived fact lines about one class, for the wizard's briefing panel.

    Reads everything off the live ``CharacterClass`` object, so the panel
    can never drift from the rules the ``CreationWizard`` will enforce.
    """
    equipment = ", ".join(f"{count}x {name}"
                          for name, count in cls.starting_equipment)
    features = []
    for level in sorted(cls.features):
        for feature in cls.features[level]:
            features.append((level, feature["name"], feature["description"]))
    return {
        "hit_die": f"d{cls.hit_die}",
        "primary": ", ".join(a.upper() for a in cls.primary_abilities),
        "saves": ", ".join(a.upper() for a in cls.proficient_saves),
        "skills": ", ".join(s.replace("_", " ").title()
                            for s in cls.class_skills),
        "equipment": equipment or "none",
        "features": features,
    }


def build_korr():
    """Korr the recruitable companion, as a bd_dnd Character."""
    return bd_dnd.Character(
        KORR_NAME,
        bd_dnd.AbilityScores(str=16, dex=12, con=14, int=8, wis=10, cha=10),
        hit_die=10,
        proficient_skills=("athletics", "perception"),
        proficient_saves=("str", "con"),
    )


# --- quests (bd_quests) ----------------------------------------------------------------

QUEST_CLEAR_YARD = "clear_yard"
QUEST_FETCH_CACHE = "fetch_cache"
QUEST_WATCH_GROWS = "watch_grows"

CLEAR_YARD_NAME = "Clear the Yard"
CLEAR_YARD_DESCRIPTION = (
    "Walk west from the gate, past the pillar row, into the west hall: five "
    "of Ashvale's own dead walk there. Put all five down. Sera pays 300 XP "
    "and her trust (disposition +20)."
)
FETCH_CACHE_NAME = "The Buried Cache"
FETCH_CACHE_DESCRIPTION = (
    "Dobb stashed a crate of clips at the far west end of the entry hall, "
    "past the pillar row. Walk the hall west until it ends, pick the crate "
    "up, and the job pays 150 XP plus four shells."
)
WATCH_GROWS_NAME = "The Watch Grows"
WATCH_GROWS_DESCRIPTION = (
    "Korr holds the watch post inside the camp. Speak with him about "
    "fighting beside you and the crossing gains a second blade: 150 XP."
)

CLEAR_YARD_REWARD_MESSAGE = "The west hall breathes again. Sera nods at you."
FETCH_CACHE_REWARD_MESSAGE = "The cache, right where Dobb left it."
WATCH_GROWS_REWARD_MESSAGE = "The watch grows by one blade."
ACCEPT_LOG_TEXT = ("Sera Voss wants the west hall cleared: five of the risen "
                   "dead, put down for good. Walk west past the pillar row.")
FETCH_LOG_TEXT = ("Dobb wants his stashed cache: a crate of clips at the far "
                  "west end of the entry hall, past the pillar row.")
RUMOR_LOG_TEXT = ("Sera confessed the bridge did not fall; it was opened, "
                  "from the Ashvale side, for a price she still pays.")
NO_ONE_NEAR = "No one close enough to talk."

QUEST_IDS = (QUEST_CLEAR_YARD, QUEST_FETCH_CACHE, QUEST_WATCH_GROWS)


def build_quests():
    """The three campaign quests (fresh objects, wired further by systems)."""
    from bd_quests import Objective, Quest

    clear = Quest(QUEST_CLEAR_YARD, CLEAR_YARD_NAME,
                  description=CLEAR_YARD_DESCRIPTION,
                  giver=NPC_DISPLAY_NAMES["sera"])
    clear.add_objective(Objective(
        "kill_zombies",
        "Put down the five risen dead in the west hall (walk west past the "
        "pillar row)", count=5))
    clear.rewards["xp"] = 300
    clear.rewards["disposition"] = [("sera", 20)]
    clear.rewards["message"] = CLEAR_YARD_REWARD_MESSAGE

    fetch = Quest(QUEST_FETCH_CACHE, FETCH_CACHE_NAME,
                  description=FETCH_CACHE_DESCRIPTION,
                  giver=NPC_DISPLAY_NAMES["dobb"])
    fetch.add_objective(Objective(
        "recover_cache",
        "Recover the cache at the far west end of the entry hall"))
    fetch.rewards["xp"] = 150
    fetch.rewards["give"] = [("Shell", 4)]
    fetch.rewards["message"] = FETCH_CACHE_REWARD_MESSAGE

    watch = Quest(QUEST_WATCH_GROWS, WATCH_GROWS_NAME,
                  description=WATCH_GROWS_DESCRIPTION,
                  giver=NPC_DISPLAY_NAMES["sera"])
    watch.add_objective(Objective("recruit_korr",
                                  "Speak with Korr at the watch post"))
    watch.rewards["xp"] = 150
    watch.rewards["message"] = WATCH_GROWS_REWARD_MESSAGE

    return {QUEST_CLEAR_YARD: clear, QUEST_FETCH_CACHE: fetch,
            QUEST_WATCH_GROWS: watch}


# --- shop and services (bd_npcs) ---------------------------------------------------------

#: Stock entries: (class_name, price, max_count, restock_tics).
SHOP_STOCK = (
    {"class_name": "Shell", "price": 8, "max_count": 12,
     "restock_tics": 35 * 35},
    {"class_name": "ClipBox", "name": "Clip box", "price": 15,
     "max_count": 6, "restock_tics": 35 * 35},
    {"class_name": "Stimpack", "price": 12, "max_count": 8,
     "restock_tics": 35 * 35},
    {"class_name": "GreenArmor", "price": 90, "max_count": 1,
     "restock_tics": 0},
)
SHOP_SELLABLES = ("Shell", "ClipBox")

HEALER_COST = 15
HEALER_MIN_STANDING = "cold"
TRAINER_SKILL = "medicine"
TRAINER_COST = 20


# --- dialogue trees (bd_dialogue) ------------------------------------------------------------

ACCEPT_DISPOSITION_DELTA = 10
RUMOR_DC = 12


def _quest_of(ctx, quest_id):
    quest_log = ctx.get("quest_log")
    return quest_log.get(quest_id) if quest_log is not None else None


def _clear_yard_inactive(ctx):
    import bd_quests

    quest = _quest_of(ctx, QUEST_CLEAR_YARD)
    return quest is not None and quest.state == bd_quests.Quest.INACTIVE


def _sera_accept_effect(ctx):
    """Accepting the yard job: Sera warms to you and the yard wakes up.

    The zombie spawn itself rides on the quest's on_start hook (systems); the
    disposition shift is dialogue-local, so it lives here.
    """
    quest = _quest_of(ctx, QUEST_CLEAR_YARD)
    if quest is not None:
        quest.start()
    dispositions = ctx.get("dispositions")
    if dispositions is not None:
        try:
            dispositions.shift("sera", ACCEPT_DISPOSITION_DELTA)
        except Exception:
            pass


def _sera_rumor_effect(ctx):
    """The persuasion-gated rumor: on success Sera opens up, points the
    hero at Korr (starting watch_grows when it has not started), and writes
    the rumor to the player log."""
    import bd_quests
    import biaseddoom as bd

    result = ctx.get("check_result")
    if not (isinstance(result, dict) and result.get("success")):
        return
    quest = _quest_of(ctx, QUEST_WATCH_GROWS)
    if quest is not None and quest.state == bd_quests.Quest.INACTIVE:
        quest.start()
    try:
        bd.set_player_log(RUMOR_LOG_TEXT, ctx.get("player_index", 0))
    except Exception:
        pass


def _watch_grows_offer(ctx):
    """The trust pointer is offered once the yard job is done, while the
    watch has not yet grown."""
    import bd_quests

    clear = _quest_of(ctx, QUEST_CLEAR_YARD)
    watch = _quest_of(ctx, QUEST_WATCH_GROWS)
    return (clear is not None and clear.state == bd_quests.Quest.COMPLETED
            and watch is not None
            and watch.state == bd_quests.Quest.INACTIVE)


def _watch_grows_effect(ctx):
    """Sera vouches for Korr: the watch_grows quest begins."""
    import bd_quests

    watch = _quest_of(ctx, QUEST_WATCH_GROWS)
    if watch is not None and watch.state == bd_quests.Quest.INACTIVE:
        watch.start()


def _fetch_cache_inactive(ctx):
    """True while Dobb's cache errand has not been accepted."""
    import bd_quests

    quest = _quest_of(ctx, QUEST_FETCH_CACHE)
    return quest is not None and quest.state == bd_quests.Quest.INACTIVE


def _fetch_inactive_now():
    """The same check read off the shared log directly (dialogue build time,
    where no ctx exists yet)."""
    try:
        import bd_quests

        quest = bd_quests.log.get(QUEST_FETCH_CACHE)
        return quest is not None and quest.state == bd_quests.Quest.INACTIVE
    except Exception:
        return False


def _dobb_fetch_effect(ctx):
    """Accepting the cache errand starts fetch_cache; a cache the player
    already grabbed before the ask pays out on the spot."""
    import bd_quests
    import biaseddoom as bd

    quest = _quest_of(ctx, QUEST_FETCH_CACHE)
    if quest is None:
        return
    if quest.state == bd_quests.Quest.INACTIVE:
        quest.start()
    if quest.state != bd_quests.Quest.ACTIVE:
        return
    try:
        if bd.actor_ref(CACHE_TID) is None:
            bd_quests.log.complete_objective(QUEST_FETCH_CACHE,
                                             "recover_cache", 1)
    except Exception:
        pass


def _korr_recruit_gate(ctx):
    """Visible once Sera is warm (>= 40) or the yard job is done, and hidden
    again after Korr has signed on."""
    dispositions = ctx.get("dispositions")
    sera_warm = False
    if dispositions is not None:
        try:
            sera_warm = int(dispositions.get("sera")) >= 40
        except Exception:
            sera_warm = False
    yard_done = False
    try:
        import bd_quests
        quest = _quest_of(ctx, QUEST_CLEAR_YARD)
        yard_done = quest is not None and quest.state == bd_quests.Quest.COMPLETED
    except Exception:
        yard_done = False
    already = False
    try:
        import ashvale_systems as systems
        already = bool(systems.korr_recruited)
    except Exception:
        already = False
    return (sera_warm or yard_done) and not already


def _recruit_korr_effect(ctx):
    try:
        import ashvale_systems as systems
        systems.recruit_korr()
    except Exception:
        pass


def _open_shop_effect(ctx):
    try:
        import ashvale_systems as systems
        systems.shop_open = True
    except Exception:
        pass


def _run_wren_service(index):
    """Dialogue bridge: run one of Wren's services through the systems ctx."""
    try:
        import ashvale_systems as systems
        systems.run_service("wren", index)
    except Exception:
        pass


def _heal_effect(ctx):
    try:
        import ashvale_systems as systems
        systems.run_healer()
    except Exception:
        pass


def _train_effect(ctx):
    _run_wren_service(1)


def sera_dialogue():
    """Sera Voss: smalltalk, the persuasion-gated rumor (which points at
    Korr), the yard job handout, and the post-yard trust pointer."""
    from bd_dialogue import Choice, Dialogue, Node

    name = NPC_DISPLAY_NAMES["sera"]
    dlg = Dialogue("sera")
    dlg.add_node(Node("start", name,
                      "You are the one who walked in through the dead "
                      "fields. Ashvale does not open its gate for "
                      "strangers."))
    dlg.add_node(Node("smalltalk", name,
                      "Fifteen souls at winter's end. The dead came back "
                      "hungry. We count the living on one hand now."))
    dlg.add_node(Node("rumor_ok", name,
                      "An honest face... The bridge did not fall. It was "
                      "opened, from our side, for a price I still pay in "
                      "sleep. You want a blade at your back: speak with "
                      "Korr at the watch post."))
    dlg.add_node(Node("rumor_no", name,
                      "Tales are for friends, stranger. Earn the telling."))
    dlg.add_node(Node("errand", name,
                      "The west hall is full of our own dead, walking. "
                      "Follow the hall west, past the pillar row, and put "
                      "down the five you find there. The crossing will owe "
                      "you."))
    dlg.add_node(Node("trust_korr", name,
                      "Korr. He has held the watch post since the first "
                      "night and never once run. Tell him the yard is "
                      "clean and he will fight beside you."))
    dlg.add_node(Node("farewell", name, "Keep your blade loose."))

    start = dlg.node("start")
    start.add_choice(Choice("How does the crossing fare?", next="smalltalk"))
    start.add_choice(Choice("What really happened at the bridge?",
                            next="rumor_ok", fail_next="rumor_no",
                            skill_check=("persuasion", RUMOR_DC),
                            effect=_sera_rumor_effect))
    start.add_choice(Choice("Who else can I trust here?",
                            condition=_watch_grows_offer,
                            effect=_watch_grows_effect, next="trust_korr"))
    start.add_choice(Choice("I need work.", condition=_clear_yard_inactive,
                            effect=_sera_accept_effect, next="errand",
                            log=(ACCEPT_LOG_TEXT, 0)))
    start.add_choice(Choice("Another time.", next="farewell"))
    dlg.node("smalltalk").add_choice(Choice("Tell me again.",
                                            next="smalltalk"))
    dlg.node("smalltalk").add_choice(Choice("Back.", next="start"))
    dlg.node("rumor_ok").add_choice(Choice("I will carry that with me.",
                                           next="start"))
    dlg.node("rumor_no").add_choice(Choice("Fair.", next="start"))
    dlg.node("errand").add_choice(Choice("It will be done.", end=True))
    dlg.node("trust_korr").add_choice(Choice("I will find him.", end=True))
    dlg.node("farewell").add_choice(Choice("Leave.", end=True))
    return dlg


#: Dobb's greeting while the cache errand is still open (he mentions the
#: stash); once the job is accepted or done he goes back to pure trade.
DOBB_START_CACHE = (
    "Supplies are low and my patience is lower. And there is a crate of "
    "clips sitting at the far west end of the entry hall that nobody here "
    "has the legs to fetch. Buy, talk, or make yourself useful.")
DOBB_START_TRADE = "Supplies are low and my patience is lower. Buy, or talk."


def dobb_dialogue():
    """Dobb: the trade node flips systems.shop_open for the ShopUI window,
    and the cache errand starts here (the start node's text mentions the
    stash while fetch_cache is still waiting to be taken)."""
    from bd_dialogue import Choice, Dialogue, Node

    name = NPC_DISPLAY_NAMES["dobb"]
    dlg = Dialogue("dobb")
    start_text = DOBB_START_CACHE if _fetch_inactive_now() \
        else DOBB_START_TRADE
    dlg.add_node(Node("start", name, start_text))
    dlg.add_node(Node("cache_where", name,
                      "A crate of clips, stashed at the far west end of the "
                      "entry hall, past the pillar row. Walk the hall west "
                      "until it ends; the crate sits on the back line. Bring "
                      "it home and I will make it worth the boots."))
    dlg.add_node(Node("trading", name,
                      "Shells, clips, stims. The armor is the last of its "
                      "kind; when it goes, it goes."))
    dlg.add_node(Node("smalltalk", name,
                      "The quartermaster before me hoarded for the end. The "
                      "end came. Now I hoard for the beginning."))
    dlg.add_node(Node("farewell", name, "Watch the yard."))

    start = dlg.node("start")
    start.add_choice(Choice("Show me your wares.", effect=_open_shop_effect,
                            next="trading"))
    start.add_choice(Choice("Anything you need moved?",
                            condition=_fetch_cache_inactive,
                            effect=_dobb_fetch_effect, next="cache_where",
                            log=(FETCH_LOG_TEXT, 0)))
    start.add_choice(Choice("Any news?", next="smalltalk"))
    start.add_choice(Choice("Farewell.", next="farewell"))
    dlg.node("cache_where").add_choice(Choice("Consider it moved.",
                                              end=True))
    dlg.node("trading").add_choice(Choice("Back.", next="start"))
    dlg.node("smalltalk").add_choice(Choice("Again.", next="smalltalk"))
    dlg.node("smalltalk").add_choice(Choice("Back.", next="start"))
    dlg.node("farewell").add_choice(Choice("Leave.", end=True))
    return dlg


def wren_dialogue():
    """Wren: services straight from the tree (heal and train effects)."""
    from bd_dialogue import Choice, Dialogue, Node

    name = NPC_DISPLAY_NAMES["wren"]
    dlg = Dialogue("wren")
    dlg.add_node(Node("start", name, "Sit. Blood first, talk after."))
    dlg.add_node(Node("healed", name,
                      "Steady. The flesh forgets; the bill does not. "
                      "Breathe easy: your wind returns with the "
                      "mending."))
    dlg.add_node(Node("trained", name,
                      "Your hands learned something. Mine only held them."))
    dlg.add_node(Node("smalltalk", name,
                      "The green ones in the walls sing at night. Not words. "
                      "Shapes."))
    dlg.add_node(Node("farewell", name, "Bleed less."))

    start = dlg.node("start")
    start.add_choice(Choice("Patch me up.", effect=_heal_effect,
                            next="healed"))
    start.add_choice(Choice("Train my field medicine.", effect=_train_effect,
                            next="trained"))
    start.add_choice(Choice("What do you see out there?", next="smalltalk"))
    start.add_choice(Choice("Farewell.", next="farewell"))
    dlg.node("healed").add_choice(Choice("Back.", next="start"))
    dlg.node("trained").add_choice(Choice("Back.", next="start"))
    dlg.node("smalltalk").add_choice(Choice("Shapes?", next="smalltalk"))
    dlg.node("smalltalk").add_choice(Choice("Back.", next="start"))
    dlg.node("farewell").add_choice(Choice("Leave.", end=True))
    return dlg


def korr_dialogue():
    """Korr: the recruitment branch, gated on Sera's trust or a cleared yard."""
    from bd_dialogue import Choice, Dialogue, Node

    name = NPC_DISPLAY_NAMES["korr"]
    dlg = Dialogue("korr")
    dlg.add_node(Node("start", name,
                      "I watched you from the rampart. You move like someone "
                      "with somewhere to be."))
    dlg.add_node(Node("joined", name,
                      "Then the yard will remember both our names. My blade "
                      "is yours while Ashvale stands."))
    dlg.add_node(Node("smalltalk", name,
                      "I want the dead to lie down. Everything else is "
                      "noise."))
    dlg.add_node(Node("farewell", name, "Keep walking."))

    start = dlg.node("start")
    start.add_choice(Choice("Fight beside me.", condition=_korr_recruit_gate,
                            effect=_recruit_korr_effect, next="joined"))
    start.add_choice(Choice("What do you want?", next="smalltalk"))
    start.add_choice(Choice("Farewell.", next="farewell"))
    dlg.node("joined").add_choice(Choice("To the end.", end=True))
    dlg.node("smalltalk").add_choice(Choice("Again.", next="smalltalk"))
    dlg.node("smalltalk").add_choice(Choice("Back.", next="start"))
    dlg.node("farewell").add_choice(Choice("Leave.", end=True))
    return dlg


DIALOGUE_FACTORIES = {
    "sera": sera_dialogue,
    "dobb": dobb_dialogue,
    "wren": wren_dialogue,
    "korr": korr_dialogue,
}


# --- prose for the UI ----------------------------------------------------------------

WIZARD_TITLE = "Founding - Who Walks Into Ashvale"
WIZARD_FINISH_LABEL = "Take up the badge"
WIZARD_NAME_HINT = "Name"
SHEET_TITLE = "The Ashvale Watch - Character"
JOURNAL_TITLE = "The Ashvale Watch - Journal"
SHOP_TITLE = "Dobb's Stores"
DIALOGUE_TITLE = "Ashvale Crossing"
PROMPT_WINDOW_ID = "###ashvale_prompt"

TOAST_RECRUIT = "Korr falls in beside you; the watch grows by one blade."
CENTER_RECRUIT = "Korr joins the watch!"
LEVEL_UP_SOUND = "misc/p_pkup"
CURRENCY_FALLBACK_LOG = ("no '%s' class to spawn; falling back to '%s' as "
                         "the trade currency")
CURRENCY_OK_LOG = "currency probe OK: trading in %s"

#: The three staggered onboarding pointers sent after the founding (systems
#: fills the {placeholders} with the live key bindings and the class active).
ONBOARD_TALK = ("Sera Voss is marked with a gold ! overhead: walk up and "
                "press [{talk_key}] to talk.")
ONBOARD_WINDOWS = ("[{journal_key}] opens the quest journal, "
                   "[{sheet_key}] the character sheet.")
ONBOARD_ACTIVE = "[{active_key}] {active_name}: {active_effect}"
#: HUD strip, line 2 fallback while no quest is being tracked.
HUD_NO_OBJECTIVE = ("No open job. Whoever carries a gold ! overhead has "
                    "work for you.")
HUD_PRE_FOUNDING = "Ashvale Crossing: found your character at the gate window."


# --- sibling-import registration ----------------------------------------------------

import sys as _sys
import types as _types


class _LiveAlias(_types.ModuleType):
    """sys.modules alias that reads/writes through to the module's live
    globals (the engine registers manifest modules under a mangled name only
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


_sys.modules.setdefault("ashvale_content",
                        _LiveAlias("ashvale_content", globals()))
del _sys, _types
