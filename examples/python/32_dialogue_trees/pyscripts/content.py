"""The Interrogation — pure content: prose, the tree, factories.

**Import-safe**: no engine calls at import time. The dialogue tree, the
quest, the faction registry, and the player character are built by factory
functions the systems layer invokes from event handlers. Every string a
player can read lives here, so the fixture can be re-voiced without
touching logic.

Loaded as manifest entry 1 of 4; the trailing ``_LiveAlias`` registration
lets later manifest entries ``import inquisition_content`` (the engine
registers manifest modules under mangled names only after execution — the
proxy reads through to this module's live globals).

The tree keeps the original fixture's four branches (a smalltalk loop, a
faction-gated rumor (bd_vtm), a persuasion-gated discount (bd_dnd), and a
quest handout (bd_quests + native player log)) and adds three more:

- **Intimidation.** Mirrors the persuasion route with
  ``skill_check=("intimidation", 12)``: success grants rifle rounds and
  a +dread bump (the Inquisitor fears what the dark answers to); failure
  routes to a threat node and erodes his attitude by ATTITUDE_DROP.
- **The mark.** A hidden choice on the root node, revealed only when the
  session context carries ``dread >= DREAD_MARK_THRESHOLD`` (the example's
  session wrapper feeds ``bd_horror.HorrorState.dread.level`` into the
  ctx). It leads to a unique confession node whose closing choice writes a
  second native player-log line.
- **The greeting.** A hidden "You again." choice on the root node,
  revealed only when the session ctx carries ``attitude <=
  ATTITUDE_GREETING_THRESHOLD`` (the systems layer tracks the
  Inquisitor's disposition as a plain int and injects it, plus its
  standing word, into the session ctx). It leads to a greeting node whose
  body line is rewritten per standing by the choice's effect.
"""

# --- fixture constants ----------------------------------------------------------

NPC_TID = 9500
TALK_RANGE = 128.0
NPC_OFFSET = 96.0          # ahead of the player start
CRATE_OFFSET = 224.0       # ahead of the player start
NPC_CLASS = "ZombieMan"
NPC_NAME = "The Inquisitor"
NPC_TINT = (140, 90, 100)  # ash and old blood

QUEST_ID = "ash_tithe"
CRATE_CLASS = "ShellBox"   # reliable pickup even at full health

FACTION = "The Hollow Choir"
RUMOR_GATE = 1

DISCOUNT_DC = 12
DISCOUNT_SHELLS = 20
INTIMIDATION_DC = 12
# Rifle rounds, not shells: the autotest's player is already near the
# 50-shell cap by the time the threat lands, and give_inventory clamps.
INTIMIDATION_AMMO_CLASS = "Clip"
INTIMIDATION_AMMO_COUNT = 30
#: Dread granted to the room when the intimidation lands.
INTIMIDATION_DREAD = 5.0
#: Attitude the failed threat costs the Inquisitor (systems.shift_attitude).
ATTITUDE_DROP = 20
#: Dread level at which the mark choice reveals itself.
DREAD_MARK_THRESHOLD = 50.0
#: Attitude at (below) which the "You again." greeting reveals itself.
ATTITUDE_GREETING_THRESHOLD = -50

LOG_TEXT = ("The Inquisitor's reliquary lies in the yard where it fell. "
            "Bring it back. Do not look inside.")
LOG_TEXT_MARK = ("The mark on your throat pulsed, and the Inquisitor knelt "
                 "to YOU. Whatever you are - he fears it.")

QUEST_NAME = "The Ash Tithe"
QUEST_DESCRIPTION = ("Recover the Inquisitor's reliquary from the yard. "
                     "He never said what rattles inside it.")
REWARD_MESSAGE = "The Hollow Choir will sing your name kindly."
NO_ONE_NEAR = "No one close enough to question."

# Booth candle over the Inquisitor's sector (untagged on MAP01, so the
# systems layer binds it by position).
CANDLE_BASE = 140
CANDLE_AMPLITUDE = 24
CANDLE_PERIOD = 12

# The hidden choice's text is a fixture constant: the autotest asserts on
# its absence/presence by exact match.
MARK_CHOICE_TEXT = "[The mark on your throat pulses] 'You know what I am.'"

# The attitude-gated greeting: exact text for the same reason.
YOU_AGAIN_CHOICE_TEXT = "You again."

#: The greeting node's body per attitude standing (systems.attitude_standing
#: computes the word from the live ctx value; the choice effect rewrites
#: the node before routing).
GREETING_LINES = {
    "hostile": ("You again. The Choir keeps a separate ledger for your "
                "kind, penitent, and every page of it is red."),
    "cold": ("You again. The Choir's ledger remembers more than I do. "
             "Speak."),
    "neutral": ("You again. The ash keeps no appointments; state your "
                "business."),
    "warm": ("You again, penitent. The Choir's coin is honest and so is "
             "your face. What do you need?"),
    "trusted": ("You again, friend of the Choir. The counting house is "
                "yours to count in. Speak, and it is done."),
}


# --- factories --------------------------------------------------------------------


def build_factions():
    """The faction registry (bd_vtm), with the Choir registered."""
    import bd_vtm

    factions = bd_vtm.Factions()
    factions.add(bd_vtm.Faction(FACTION))
    return factions


def build_character():
    """The player character (bd_dnd): a silver tongue and a heavy shadow."""
    import bd_dnd

    return bd_dnd.Character(
        "The Penitent",
        bd_dnd.AbilityScores(str=12, dex=12, con=12, int=10, wis=10, cha=14),
        hit_die=8,
        proficient_skills=("persuasion", "intimidation"),
    )


def build_quest(factions):
    """The fetch quest (bd_quests); completion grants Choir reputation."""
    from bd_quests import Objective, Quest

    quest = Quest(QUEST_ID, QUEST_NAME,
                  description=QUEST_DESCRIPTION,
                  giver=NPC_NAME, faction=FACTION)
    quest.add_objective(Objective("fetch_reliquary",
                                  "Recover the Inquisitor's reliquary"))
    quest.rewards["message"] = REWARD_MESSAGE
    quest.on_complete = lambda q: factions.change_reputation(
        FACTION, 1, "returned the reliquary unopened")
    return quest


# --- choice conditions and effects (called by bd_dialogue with a ctx dict) ---------


def _quest_inactive(ctx):
    import bd_quests

    quest_log = ctx.get("quest_log")
    quest = quest_log.get(QUEST_ID) if quest_log is not None else None
    return quest is not None and quest.state == bd_quests.Quest.INACTIVE


def _accept_quest(ctx):
    quest_log = ctx.get("quest_log")
    quest = quest_log.get(QUEST_ID) if quest_log is not None else None
    if quest is not None:
        quest.start()


def _dread_marked(ctx):
    """Hidden until the session ctx carries enough dread (see systems)."""
    try:
        return float(ctx.get("dread") or 0.0) >= DREAD_MARK_THRESHOLD
    except (TypeError, ValueError):
        return False


def _grant_ammo(ctx, class_name, amount):
    import biaseddoom as bd

    player = bd.player(ctx.get("player_index", 0))
    pawn = player.actor if player is not None else None
    if pawn is not None and pawn.valid:
        pawn.give_inventory(str(class_name), int(amount))
        bd.play_ui_sound("misc/p_pkup", volume=0.8)


def _discount_effect(ctx):
    result = ctx.get("check_result")
    if result and result.get("success"):
        _grant_ammo(ctx, "Shell", DISCOUNT_SHELLS)


def _intimidation_effect(ctx):
    result = ctx.get("check_result")
    if result and result.get("success"):
        _grant_ammo(ctx, INTIMIDATION_AMMO_CLASS, INTIMIDATION_AMMO_COUNT)
        # The room itself grows afraid.
        horror = ctx.get("horror")
        if horror is not None:
            try:
                horror.dread.set_level(horror.dread.level + INTIMIDATION_DREAD)
            except Exception:
                pass
        return
    # A failed threat erodes the Inquisitor's attitude (the example-local
    # disposition the session ctx carries as shift_attitude).
    shift = ctx.get("shift_attitude")
    if shift is not None:
        try:
            shift(-ATTITUDE_DROP)
        except Exception:
            pass


def _you_again_condition(ctx):
    """Hidden until the Inquisitor's attitude freezes over (see systems)."""
    try:
        return int(ctx.get("attitude") or 0) <= ATTITUDE_GREETING_THRESHOLD
    except (TypeError, ValueError):
        return False


# --- the tree ---------------------------------------------------------------------


def build_dialogue():
    """Construct the Inquisitor's validated dialogue tree."""
    from bd_dialogue import Choice, Dialogue, Node

    dlg = Dialogue("inquisition")
    dlg.add_node(Node("start", NPC_NAME,
                      "You stand in the ash-light of my counting house, "
                      "penitent. Speak - every word you spend here is "
                      "weighed."))
    dlg.add_node(Node("smalltalk", NPC_NAME,
                      "Trade? The dead pay in silence and the living pay "
                      "in lies. Business, as ever, is honest suffering."))
    dlg.add_node(Node("rumor", NPC_NAME,
                      "The Choir moves reliquaries through the old tunnels "
                      "at dusk, when the dark runs thin. You heard none of "
                      "this from me."))
    dlg.add_node(Node("discount_ok", NPC_NAME,
                      "A silver tongue... rare meat. Take the shells - the "
                      "Choir discounts for the eloquent."))
    dlg.add_node(Node("discount_no", NPC_NAME,
                      "A clumsy tongue. Full price. The apocalypse did not "
                      "devalue iron."))
    dlg.add_node(Node("intimidate_ok", NPC_NAME,
                      "Enough. ENOUGH. Take the rounds - take them and "
                      "keep your shadows away from my name. Whatever "
                      "whispers to you... tell it I was kind."))
    dlg.add_node(Node("intimidate_no", NPC_NAME,
                      "You threaten ME? I have flensed braver men for "
                      "smaller insults. Choose your next word like it is "
                      "your last."))
    dlg.add_node(Node("errand", NPC_NAME,
                      "My reliquary lies in the yard where it fell. Bring "
                      "it back unopened, and the Choir will sing your name "
                      "kindly."))
    dlg.add_node(Node("confession", NPC_NAME,
                      "You wear the mark... Forgive me. I am not the "
                      "hunter here - I never was. Take MY confession "
                      "instead: it was I who let the dark in."))
    dlg.add_node(Node("farewell", NPC_NAME,
                      "Go. The dark walks with you - I can smell it."))
    dlg.add_node(Node("greeting", NPC_NAME, GREETING_LINES["neutral"]))

    start = dlg.node("start")
    start.add_choice(Choice("How does business fare in the ashes?",
                            next="smalltalk"))
    start.add_choice(Choice("What does the Choir whisper?", next="rumor",
                            faction_gate=(FACTION, RUMOR_GATE)))
    start.add_choice(Choice("A penitent's coin is thin - show me mercy on "
                            "shells.",
                            next="discount_ok", fail_next="discount_no",
                            skill_check=("persuasion", DISCOUNT_DC),
                            effect=_discount_effect))
    start.add_choice(Choice("Sell to me, or the dark learns your name.",
                            next="intimidate_ok", fail_next="intimidate_no",
                            skill_check=("intimidation", INTIMIDATION_DC),
                            effect=_intimidation_effect))
    start.add_choice(Choice("Is there work for the damned?", next="errand",
                            condition=_quest_inactive, effect=_accept_quest,
                            log=(LOG_TEXT, 0)))
    start.add_choice(Choice(MARK_CHOICE_TEXT, next="confession",
                            condition=_dread_marked))
    start.add_choice(Choice("Another time, Inquisitor.", next="farewell"))

    # The attitude-gated greeting: the effect rewrites the node's body to
    # the current standing's line before routing (a closure, so it can
    # reach the node object; appended after farewell so every existing
    # choice index is stable).
    greeting = dlg.node("greeting")

    def _you_again_effect(ctx):
        standing = str(ctx.get("attitude_standing") or "neutral")
        greeting.text = GREETING_LINES.get(standing,
                                           GREETING_LINES["neutral"])

    start.add_choice(Choice(YOU_AGAIN_CHOICE_TEXT, next="greeting",
                            condition=_you_again_condition,
                            effect=_you_again_effect))

    dlg.node("smalltalk").add_choice(Choice("Tell me again.",
                                            next="smalltalk"))
    dlg.node("smalltalk").add_choice(Choice("Enough.", next="start"))
    dlg.node("rumor").add_choice(Choice("Chilling.", next="start"))
    dlg.node("discount_ok").add_choice(Choice("The Choir is generous.",
                                              end=True))
    dlg.node("discount_no").add_choice(Choice("Full price, then.",
                                              next="start"))
    dlg.node("intimidate_ok").add_choice(Choice("Wise.", end=True))
    dlg.node("intimidate_no").add_choice(Choice("...I misspoke.",
                                                next="start"))
    dlg.node("errand").add_choice(Choice("It will be done.", end=True))
    dlg.node("confession").add_choice(Choice("I accept your confession.",
                                             log=(LOG_TEXT_MARK, 0),
                                             end=True))
    dlg.node("farewell").add_choice(Choice("Leave.", end=True))
    dlg.node("greeting").add_choice(Choice("Business. Now.", next="start"))
    return dlg


# --- sibling-import registration ----------------------------------------------------

import sys as _sys
import types as _types


class _LiveAlias(_types.ModuleType):
    """sys.modules alias that reads/writes through to the module's live
    globals (the engine registers manifest modules under a mangled name
    only after execution, so the real module object cannot be aliased
    from inside itself)."""

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


_sys.modules.setdefault("inquisition_content",
                        _LiveAlias("inquisition_content", globals()))
del _sys, _types
