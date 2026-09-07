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
  the systems layer drives at the start of the run.
- **bd_npcs**: four ``NPCDefinition`` entries (spawn offsets probe-verified on
  MAP01), a ``Shop``, a ``HealerService``, and a ``TrainerService``.
- **bd_dialogue**: one validated ``Dialogue`` tree per talkable NPC with
  skill checks, disposition gates, conditions, effects, and player-log writes.
- **bd_quests**: three quests with kill/pickup trackers and xp/disposition
  reward hooks.
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
    "yard_monster": "ZombieMan",     # Heretic: "Gargoyle"; Hexen: "Ettin"
    "sera_class": "ZombieMan",       # Heretic: "Disciple";  Hexen: "Zombie"
    "dobb_class": "ShotgunGuy",      # Heretic: "UndeadWarrior"; Hexen: "Afrit"
    "wren_class": "DoomImp",         # Heretic: "SabreClink"; Hexen: "Afrit"
    "korr_class": "Demon",           # Heretic: "Weredragon"; Hexen: "Centaur"
    "companion_class": "Demon",      # Heretic: "Weredragon"; Hexen: "Centaur"
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


def _grant_press_on(character):
    character.grant_resource("press_on", 1)


def _apply_skirmisher(character):
    notes = dict(getattr(character, "mod_notes", {}) or {})
    notes["skirmisher_damage"] = 1
    character.mod_notes = notes


def _grant_uncanny_step(character):
    character.grant_resource("uncanny_step", 1)


def _grant_kindled_light(character):
    character.grant_resource("light", 3)


def _grant_warding_flame(character):
    character.grant_resource("warding_flame", 1)


#: The classic standard array as an unordered multiset.
STANDARD_ARRAY = (15, 14, 13, 12, 10, 8)

MERCENARY = bd_dnd.CharacterClass(
    name="Mercenary", hit_die=10,
    primary_abilities=("str",),
    proficient_saves=("str", "con"),
    class_skills=("athletics", "intimidation", "perception"),
    features={
        1: [{"id": "second_wind", "name": "Second Wind",
             "description": "Dig in and rally: one second_wind charge per "
                            "rest.",
             "apply": _grant_second_wind}],
        2: [{"id": "press_on", "name": "Press On",
             "description": "Numb to the ache: one press_on charge per rest.",
             "apply": _grant_press_on}],
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
             "description": "+1 damage when you strike from motion (a mod "
                            "note the combat layer reads).",
             "apply": _apply_skirmisher}],
        2: [{"id": "uncanny_step", "name": "Uncanny Step",
             "description": "Slip the grapple: one uncanny_step charge per "
                            "rest.",
             "apply": _grant_uncanny_step}],
    },
    resources={"uncanny_step": 1},
    starting_equipment=(("Clip", 1), ("Shell", 2), ("GreenArmor", 1)),
)

LIGHTKEEPER = bd_dnd.CharacterClass(
    name="Lightkeeper", hit_die=8,
    primary_abilities=("wis",),
    proficient_saves=("wis", "cha"),
    class_skills=("insight", "medicine", "religion", "arcana"),
    features={
        1: [{"id": "kindled_light", "name": "Kindled Light",
             "description": "Carry the ember: three light charges per rest.",
             "apply": _grant_kindled_light}],
        2: [{"id": "warding_flame", "name": "Warding Flame",
             "description": "Raise a ring of pale fire: one warding_flame "
                            "charge per rest.",
             "apply": _grant_warding_flame}],
    },
    resources={"light": 3},
    starting_equipment=(("Clip", 1), ("Stimpack", 2), ("GreenArmor", 1)),
)

CLASSES_BY_ID = {cls.name: cls for cls in (MERCENARY, SCOUT, LIGHTKEEPER)}
CLASS_LIST = (MERCENARY, SCOUT, LIGHTKEEPER)

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
QUEST_PROVE_WORTH = "prove_worth"

CLEAR_YARD_NAME = "Clear the Yard"
CLEAR_YARD_DESCRIPTION = (
    "The west hall is full of Ashvale's own dead, walking. Put down five of "
    "them before they remember the way to the living quarters."
)
FETCH_CACHE_NAME = "The Buried Cache"
FETCH_CACHE_DESCRIPTION = (
    "Dobb stashed a crate of clips at the far west end of the entry hall "
    "before the dead came back. Walk the line and bring it home."
)
PROVE_WORTH_NAME = "Prove Your Worth"
PROVE_WORTH_DESCRIPTION = (
    "Sera let slip that the crossing tests strangers before it trusts them. "
    "Sweep the yard and the gate will know your name."
)

CLEAR_YARD_REWARD_MESSAGE = "The west hall breathes again. Sera nods at you."
FETCH_CACHE_REWARD_MESSAGE = "The cache, right where Dobb left it."
PROVE_WORTH_REWARD_MESSAGE = "The crossing has one more blade it can count on."
ACCEPT_LOG_TEXT = ("Sera Voss wants the west hall cleared: five of the risen "
                   "dead, put down for good.")
RUMOR_LOG_TEXT = ("Sera confessed the bridge did not fall; it was opened, "
                  "from the Ashvale side, for a price she still pays.")
NO_ONE_NEAR = "No one close enough to talk."

QUEST_IDS = (QUEST_CLEAR_YARD, QUEST_FETCH_CACHE, QUEST_PROVE_WORTH)


def build_quests():
    """The three campaign quests (fresh objects, wired further by systems)."""
    from bd_quests import Objective, Quest

    clear = Quest(QUEST_CLEAR_YARD, CLEAR_YARD_NAME,
                  description=CLEAR_YARD_DESCRIPTION,
                  giver=NPC_DISPLAY_NAMES["sera"])
    clear.add_objective(Objective("kill_zombies",
                                  "Put down the risen dead", count=5))
    clear.rewards["xp"] = 300
    clear.rewards["disposition"] = [("sera", 20)]
    clear.rewards["message"] = CLEAR_YARD_REWARD_MESSAGE

    fetch = Quest(QUEST_FETCH_CACHE, FETCH_CACHE_NAME,
                  description=FETCH_CACHE_DESCRIPTION,
                  giver=NPC_DISPLAY_NAMES["dobb"])
    fetch.add_objective(Objective("recover_cache", "Recover the cache"))
    fetch.rewards["xp"] = 150
    fetch.rewards["give"] = [("Shell", 4)]
    fetch.rewards["message"] = FETCH_CACHE_REWARD_MESSAGE

    prove = Quest(QUEST_PROVE_WORTH, PROVE_WORTH_NAME,
                  description=PROVE_WORTH_DESCRIPTION,
                  giver=NPC_DISPLAY_NAMES["sera"])
    prove.add_objective(Objective("yard_cleared", "Sweep the yard for Sera"))
    prove.rewards["xp"] = 100
    prove.rewards["message"] = PROVE_WORTH_REWARD_MESSAGE

    return {QUEST_CLEAR_YARD: clear, QUEST_FETCH_CACHE: fetch,
            QUEST_PROVE_WORTH: prove}


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
    """The persuasion-gated rumor: on success Sera opens up and the hidden
    starter quest (prove_worth) activates."""
    import bd_quests
    import biaseddoom as bd

    result = ctx.get("check_result")
    if not (isinstance(result, dict) and result.get("success")):
        return
    quest = _quest_of(ctx, QUEST_PROVE_WORTH)
    if quest is not None and quest.state == bd_quests.Quest.INACTIVE:
        quest.start()
    try:
        bd.set_player_log(RUMOR_LOG_TEXT, ctx.get("player_index", 0))
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
    _run_wren_service(0)


def _train_effect(ctx):
    _run_wren_service(1)


def sera_dialogue():
    """Sera Voss: smalltalk, the persuasion-gated rumor, the quest handout."""
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
                      "sleep. Prove the yard can trust you and maybe I say "
                      "more."))
    dlg.add_node(Node("rumor_no", name,
                      "Tales are for friends, stranger. Earn the telling."))
    dlg.add_node(Node("errand", name,
                      "The west hall is full of our own dead, walking. Put "
                      "down five of them and the crossing will owe you."))
    dlg.add_node(Node("farewell", name, "Keep your blade loose."))

    start = dlg.node("start")
    start.add_choice(Choice("How does the crossing fare?", next="smalltalk"))
    start.add_choice(Choice("What really happened at the bridge?",
                            next="rumor_ok", fail_next="rumor_no",
                            skill_check=("persuasion", RUMOR_DC),
                            effect=_sera_rumor_effect))
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
    dlg.node("farewell").add_choice(Choice("Leave.", end=True))
    return dlg


def dobb_dialogue():
    """Dobb: the trade node flips systems.shop_open for the ShopUI window."""
    from bd_dialogue import Choice, Dialogue, Node

    name = NPC_DISPLAY_NAMES["dobb"]
    dlg = Dialogue("dobb")
    dlg.add_node(Node("start", name,
                      "Supplies are low and my patience is lower. Buy, or "
                      "talk."))
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
    start.add_choice(Choice("Any news?", next="smalltalk"))
    start.add_choice(Choice("Farewell.", next="farewell"))
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
                      "Steady. The flesh forgets; the bill does not."))
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
