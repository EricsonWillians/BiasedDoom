"""The Interrogation - thin bootstrap and the headless autotest schedule.

Deliberately small: the tree and prose live in :mod:`inquisition_content`,
session/atmosphere rules in :mod:`inquisition_systems`, and the
bd_horror-skinned ImGui window in :mod:`inquisition_ui`. What remains here
is the deterministic autotest (``BD_EXAMPLE_AUTOTEST=1``) and the
screenshot pose (``BD_EXAMPLE_SCREENSHOT=1``).

The autotest drives the framework headlessly with scripted RNG doubles and
asserts: model validation, the talk ui_command path plus the synthetic
Custom Action 1 press path (event payload, session start, release edge),
smalltalk routing,
the faction gate locked/unlocked, both persuasion and both intimidation
branches (shells/rounds only on success, dread bump on a successful
threat), the skill-check annotations naming the character's bonus
("(DC 12 Persuasion, you +4)", cross-checked against
``systems.skill_bonus``), the marker lifecycle (gold "!" before first
contact, retired by it, the "[Q] Talk" label in range with no session,
hidden during a session and out of range, intro toast fired), the
hidden-choice hint lines (dread hint on the smalltalk node within 20
below the mark threshold, attitude hint on the start node at <= -30,
both lifting once their choice is revealed), the attitude ctx keys with
failed threats eroding the
Inquisitor's attitude by 20 each until the "You again." greeting unlocks
at -50 with a standing-keyed body line, the
player-log writes (errand + the mark's second line), the hidden mark
choice absent below dread 50 and present at/above it, quest start +
pickup-driven completion, the one-session guard, stale-NPC auto-end, the
dusk candle program, and the stalker director's suppression during a
session. The run ends via ``-scripttest``'s own PASS/FAIL accounting.

Manifest entry 4 of 4 (runs after its siblings have self-registered).
"""

import os

import biaseddoom as bd
import bd_dialogue
from bd_dialogue import Choice, Dialogue, Node
import bd_quests
from bd_horror import toasts
from bd_quests import Quest

try:
    import inquisition_content as content
    import inquisition_systems as systems
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="inquisition_content")
    systems = bd.import_script("pyscripts/systems.py",
                               module_name="inquisition_systems")

AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"
SCREENSHOT = os.environ.get("BD_EXAMPLE_SCREENSHOT") == "1"

QUEST_ID = content.QUEST_ID
CRATE_CLASS = content.CRATE_CLASS
DISCOUNT_SHELLS = content.DISCOUNT_SHELLS
LOG_TEXT = content.LOG_TEXT
LOG_TEXT_MARK = content.LOG_TEXT_MARK
TALK_RANGE = content.TALK_RANGE

# Start-node visible indices (quest inactive, dread below the threshold,
# attitude above the greeting gate): 0 smalltalk, 1 rumor, 2 persuasion,
# 3 intimidation, 4 handout, 5 farewell. The handout hides once the quest
# leaves INACTIVE; the mark choice appears (index 5, or 4 once the handout
# hides) at dread >= 50; the "You again." greeting appends after farewell
# once the Inquisitor's attitude reaches -50.
IDX_SMALLTALK = 0
IDX_RUMOR = 1
IDX_PERSUASION = 2
IDX_INTIMIDATION = 3
IDX_HANDOUT = 4
IDX_FAREWELL = 5


class _Roller:
    """Scripted rng test double: yields clamped values in order."""

    def __init__(self, values):
        self._values = list(values)
        self._index = 0

    def randint(self, lo, hi):
        value = self._values[self._index % len(self._values)]
        self._index += 1
        return max(lo, min(hi, value))


def raises_value_error(fn):
    """True when fn() raises ValueError (bd.assert_true never raises)."""
    try:
        fn()
    except ValueError:
        return True
    except Exception:
        return False
    return False


def _choice_labels(entries):
    return [(choice.text, enabled, annotation)
            for choice, enabled, annotation in entries]


# --- run-mode scheduling ------------------------------------------------------------


@bd.on("map_load")
def schedule_run_modes(event):
    if event.get("from_savegame"):
        return
    if AUTOTEST:
        pawn = systems.player_pawn()
        if pawn is not None:
            pawn.damage_factor = 0.0  # nothing may kill the test driver
        bd.schedule(autotest_model, delay=10)
        bd.schedule(autotest_spawned, delay=25)
        bd.schedule(autotest_markers_intro, delay=30)
        bd.schedule(autotest_talk_command, delay=40)
        bd.schedule(autotest_talk_asserts, delay=50)
        bd.schedule(autotest_markers_retired, delay=58)
        bd.schedule(autotest_custom_action_press, delay=62)
        bd.schedule(autotest_custom_action_asserts, delay=74)
        bd.schedule(autotest_custom_action_release, delay=86)
        bd.schedule(autotest_smalltalk, delay=102)
        bd.schedule(autotest_gate_locked, delay=110)
        bd.schedule(autotest_no_character, delay=125)
        bd.schedule(autotest_marker_talk_label, delay=136)
        bd.schedule(autotest_check_success, delay=145)
        bd.schedule(autotest_check_fail_and_quest, delay=170)
        bd.schedule(autotest_quest_condition, delay=195)
        bd.schedule(autotest_pickup, delay=215)
        bd.schedule(autotest_marker_far, delay=232)
        bd.schedule(autotest_quest_done, delay=240)
        bd.schedule(autotest_marker_out_of_range, delay=250)
        bd.schedule(autotest_gate_open, delay=260)
        bd.schedule(autotest_session_guard, delay=290)
        bd.schedule(autotest_intimidation_success, delay=310)
        bd.schedule(autotest_intimidation_fail, delay=340)
        bd.schedule(autotest_attitude, delay=355)
        bd.schedule(autotest_hidden_choice, delay=370)
        bd.schedule(autotest_candle_and_stalker, delay=405)
        bd.schedule(autotest_stale_npc, delay=435)
        bd.schedule(autotest_finish, delay=465)
    if SCREENSHOT:
        bd.schedule(screenshot_pose, delay=bd.TICRATE, map_local=False)
        # A *failed* persuasion attempt, then back to the start node: the
        # shot shows the judgment line in wound red AND the full annotated
        # choice list ([LOCKED], the DCs, the revealed mark choice).
        bd.schedule(screenshot_choose, delay=bd.TICRATE + 15,
                    map_local=False)
        bd.schedule(screenshot_back, delay=bd.TICRATE + 25,
                    map_local=False)
        bd.schedule(lambda: bd.execute("screenshot /tmp/interrogation"),
                    delay=bd.TICRATE + 35, map_local=False)
        bd.schedule(lambda: bd.execute("quit"), delay=3 * bd.TICRATE,
                    map_local=False)


def screenshot_pose():
    """Pose the player in front of the Inquisitor and open the window."""
    pawn = systems.player_pawn()
    npc = systems.npc_ref
    if pawn is not None and npc is not None and npc.valid:
        pawn.set_position(npc.x - 64.0, npc.y, npc.z, check=False)
        pawn.angle = 0.0
    # Raise the dread so the hidden mark choice and the vignette show.
    systems.horror.dread.set_level(60.0)
    bd.execute("talk")


def screenshot_choose():
    session = systems.current_session()
    if session is not None and session.active:
        session.rng = _Roller([4])  # 4 + 2 + 2 = 8 < 12: a failed haggle
        session.choose(IDX_PERSUASION)


def screenshot_back():
    session = systems.current_session()
    if session is not None and session.active:
        session.choose(0)  # "Full price, then." -> back to the start node


# --- autotest steps -----------------------------------------------------------------


def autotest_model():
    dialogue = systems.dialogue
    bd.assert_true("bd_dialogue" in str(bd_dialogue.__file__),
                   "bd_dialogue imported from the shipped lib")
    bd.assert_true(dialogue.node("start") is not None
                   and dialogue.node("farewell") is not None,
                   "dialogue nodes resolve by id")
    bd.assert_true(len(dialogue.all()) == 11, "eleven nodes registered")
    bad = Dialogue("bad")
    bad.add_node(Node("start", "X", "hi"))
    bad.node("start").add_choice(Choice("go", next="nowhere"))
    bd.assert_true(raises_value_error(bad.validate),
                   "dangling next raises ValueError at validation")
    bad2 = Dialogue("bad2")
    bad2.add_node(Node("start", "X", "hi"))
    bad2.node("start").add_choice(Choice("go", next="start",
                                         fail_next="missing"))
    bd.assert_true(raises_value_error(bad2.validate),
                   "dangling fail_next raises ValueError at validation")
    bad3 = Dialogue("bad3", start="nope")
    bad3.add_node(Node("start", "X", "hi"))
    bd.assert_true(raises_value_error(bad3.validate),
                   "missing start node raises ValueError")
    bd.assert_true(raises_value_error(
        lambda: dialogue.add_node(Node("start", "Dup", "dup"))),
        "duplicate node id raises ValueError")
    bd.assert_true(raises_value_error(
        lambda: bd_dialogue.DialogueSession(bad, systems.npc_ref)),
        "session construction validates the tree")


def autotest_spawned():
    pawn = systems.player_pawn()
    npc = systems.npc_ref
    bd.assert_true(pawn is not None, "player pawn available")
    bd.assert_true(npc is not None and npc.valid
                   and npc.alive, "inquisitor NPC live")
    if npc is not None and npc.valid:
        bd.assert_true(npc.get_flag("FRIENDLY"),
                       "inquisitor is friendly")
        if pawn is not None:
            bd.assert_true(pawn.distance_to(npc) < TALK_RANGE,
                           "inquisitor spawns within talk range")
    crates = bd.actor_refs(CRATE_CLASS)
    bd.assert_true(len(crates) >= 1, "reliquary crate spawned in the yard")


def autotest_markers_intro():
    """Pre-contact: the gold "!" floats over the Inquisitor, no talk label
    yet, and the intro toast named the live binding and the annotations."""
    bd.assert_true(systems.marker_state["mark"] is True
                   and systems.marker_state["talk"] is False
                   and systems.marker_state["talked"] is False,
                   "gold '!' marker up before the first conversation")
    expected = content.INTRO_TOAST.format(key=systems.action_key_hint(1))
    bd.assert_true(any(entry.get("text") == expected
                       for entry in toasts.history),
                   "intro toast names the live binding and the DC/bonus "
                   "annotations")


def autotest_talk_command():
    """The real interaction path: alias talk -> pyui -> ui_command.

    Console commands issued through bd.execute are processed by the
    console command buffer, so the ui_command handler (and therefore the
    session start) lands on a later tic; autotest_talk_asserts checks
    the outcome."""
    bd.execute("talk")


def autotest_talk_asserts():
    active = bd_dialogue.active_session()
    bd.assert_true(active is not None and active.active,
                   "talk command started a session")
    bd.assert_true(systems.current_session() is active,
                   "the example session is the active one (UI reads it)")
    if active is None:
        return
    bd.assert_true(active.active_node is not None
                   and active.active_node.id == "start",
                   "session opens at the start node")
    ctx = active.context()
    bd.assert_true("dread" in ctx and "horror" in ctx,
                   "the session ctx carries dread and horror")
    entries = active.choices()
    labels = _choice_labels(entries)
    bd.assert_true(len(entries) == 6,
                   "six choices visible before the quest is accepted")
    bd.assert_true(labels[IDX_RUMOR][1] is False
                   and "[LOCKED]" in labels[IDX_RUMOR][2],
                   "rumor choice locked pre-reputation")
    bd.assert_true(labels[IDX_PERSUASION][1] is True
                   and labels[IDX_PERSUASION][2]
                   == "(DC 12 Persuasion, you +4)",
                   "persuasion choice annotated with its DC and your bonus")
    bd.assert_true(labels[IDX_INTIMIDATION][1] is True
                   and labels[IDX_INTIMIDATION][2]
                   == "(DC 12 Intimidation, you +4)",
                   "intimidation choice annotated with its DC and your bonus")
    bd.assert_true(systems.skill_bonus(systems.character, "persuasion") == 4
                   and systems.skill_bonus(systems.character,
                                           "intimidation") == 4,
                   "the +4 is ability mod +2 (cha 14) and proficiency +2")
    bd.assert_true(all(text != content.MARK_CHOICE_TEXT
                       for text, _en, _ann in labels),
                   "mark choice hidden while dread is low")


def autotest_markers_retired():
    """First contact happened and a refresh ran: the "!" is retired, and
    no talk label shows while the session it opened is still active."""
    bd.assert_true(systems.marker_state["talked"] is True,
                   "the first conversation set the talked flag")
    bd.assert_true(systems.marker_state["mark"] is False,
                   "the first conversation retired the '!' marker")
    bd.assert_true(systems.marker_state["talk"] is False,
                   "no talk label while a session is open")


def autotest_custom_action_press():
    """End the console-path session, then synthetically press Custom
    Action 1 (the engine surfaces the press on the next gametic's scan)."""
    active = bd_dialogue.active_session()
    bd.assert_true(active is not None and active.active,
                   "session active before the custom action press")
    if active is not None:
        active.end()
    bd.assert_true(bd_dialogue.active_session() is None,
                   "ending cleared the session before the synthetic press")
    bd.set_custom_action(1, True)


def autotest_custom_action_asserts():
    """The press fired the documented payload and started a fresh session
    through the same handler the console alias uses."""
    active = bd_dialogue.active_session()
    bd.assert_true((1, True) in systems.action_log,
                   "the custom_action event carried "
                   "{'action': 1, 'pressed': True}")
    bd.assert_true(active is not None and active.active,
                   "custom action 1 press started a session")
    if active is None:
        return
    bd.assert_true(active.active_node is not None
                   and active.active_node.id == "start",
                   "the custom-action session opens at the start node")
    bd.set_custom_action(1, False)


def autotest_custom_action_release():
    """The release edge fired too and left the pressed-open session alone."""
    bd.assert_true((1, False) in systems.action_log,
                   "the release edge fired with "
                   "{'action': 1, 'pressed': False}")
    active = bd_dialogue.active_session()
    bd.assert_true(active is not None and active.active,
                   "the release edge left the session open")


def autotest_smalltalk():
    active = bd_dialogue.active_session()
    bd.assert_true(active is not None and active.active,
                   "session still active for the smalltalk branch")
    if active is None:
        return
    active.choose(IDX_SMALLTALK)
    bd.assert_true(active.active_node.id == "smalltalk",
                   "smalltalk choice routes to the smalltalk node")
    active.choose(0)  # "Tell me again." -> loops
    bd.assert_true(active.active_node.id == "smalltalk",
                   "smalltalk loop routes back to itself")
    active.choose(1)  # "Enough."
    bd.assert_true(active.active_node.id == "start",
                   "smalltalk exit routes back to start")


def autotest_gate_locked():
    active = bd_dialogue.active_session()
    bd.assert_true(active is not None, "session active for the gate test")
    if active is None:
        return
    result = active.choose(IDX_RUMOR)  # locked rumor choice: a no-op
    bd.assert_true(result is False
                   and active.active_node.id == "start",
                   "choosing a locked choice is a no-op")


def autotest_no_character():
    """End the current session, then a character-less session renders the
    skill-check choices '(unavailable)' and non-selectable."""
    active = bd_dialogue.active_session()
    bd.assert_true(active is not None, "session active to end politely")
    if active is None:
        return
    active.choose(IDX_FAREWELL)  # -> the farewell node
    bd.assert_true(active.active_node.id == "farewell",
                   "farewell choice routes to the farewell node")
    active.choose(0)  # "Leave." ends the conversation
    bd.assert_true(not active.active and active.end_reason == "choice"
                   and bd_dialogue.active_session() is None,
                   "an end=True choice closes the session")
    plain = bd_dialogue.DialogueSession(systems.dialogue, systems.npc_ref,
                                        quest_log=bd_quests.log)
    bd.assert_true(plain.start(), "character-less session starts")
    entries = plain.choices()
    labels = _choice_labels(entries)
    bd.assert_true(labels[IDX_RUMOR][1] is False
                   and "[LOCKED]" in labels[IDX_RUMOR][2],
                   "no factions object locks every faction gate")
    bd.assert_true(labels[IDX_PERSUASION][1] is False
                   and "(unavailable)" in labels[IDX_PERSUASION][2],
                   "no character renders persuasion '(unavailable)'")
    bd.assert_true(labels[IDX_INTIMIDATION][1] is False
                   and "(unavailable)" in labels[IDX_INTIMIDATION][2],
                   "no character renders intimidation '(unavailable)'")
    bd.assert_true(plain.choose(IDX_PERSUASION) is False
                   and plain.active_node.id == "start",
                   "choosing an unavailable choice is a no-op")
    # A raw framework session carries no dread key: the mark stays hidden
    # even at high dread (the wrapper is what exposes it).
    systems.horror.dread.set_level(80.0)
    bd.assert_true(all(choice.text != content.MARK_CHOICE_TEXT
                       for choice, _en, _ann in plain.choices()),
                   "raw sessions never see the mark choice")
    systems.horror.dread.set_level(0.0)
    plain.end()
    bd.assert_true(bd_dialogue.active_session() is None,
                   "manual end() clears the active session")
    # The example's wrapper with no character attached falls back to the
    # framework's rendering too (no bonus without a sheet).
    wrapped = systems.InquisitionSession(systems.dialogue, systems.npc_ref,
                                         quest_log=bd_quests.log,
                                         horror_state=systems.horror)
    bd.assert_true(wrapped.start(), "character-less wrapped session starts")
    wrapped_labels = _choice_labels(wrapped.choices())
    bd.assert_true(wrapped_labels[IDX_PERSUASION][1] is False
                   and "(unavailable)" in wrapped_labels[IDX_PERSUASION][2]
                   and "you +" not in wrapped_labels[IDX_PERSUASION][2],
                   "wrapped session without a character keeps '(unavailable)'")
    wrapped.end()
    bd.assert_true(bd_dialogue.active_session() is None,
                   "wrapped character-less session ended")


def autotest_marker_talk_label():
    """Post-contact, in talk range, no session open: the context label is
    up and names the live Custom Action 1 binding."""
    bd.assert_true(systems.marker_state["talked"] is True
                   and systems.marker_state["mark"] is False,
                   "the '!' stays retired after first contact")
    bd.assert_true(systems.marker_state["talk"] is True,
                   "talk label up in range with no session open")
    expected = content.TALK_LABEL_TEMPLATE.format(
        key=systems.action_key_hint(1))
    bd.assert_true(systems.marker_state["talk_text"] == expected,
                   "talk label names the live binding")


def autotest_check_success():
    """Persuasion success: routes to discount_ok and grants the shells."""
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the persuasion branch")
    if session is None:
        return
    pawn = systems.player_pawn()
    shells_before = pawn.inventory_count("Shell") if pawn is not None else 0
    session.rng = _Roller([10])  # 10 + 2 (cha 14) + 2 (prof) = 14 >= 12
    result = session.choose(IDX_PERSUASION)
    bd.assert_true(isinstance(result, dict) and result["success"]
                   and result["total"] == 14,
                   "scripted roller drives the success branch (10+2+2=14)")
    bd.assert_true(session.active_node.id == "discount_ok",
                   "success routes to the discount_ok node")
    bd.assert_true(session.last_check is not None
                   and session.last_check["text"] == "(Success) 14 vs DC 12",
                   "result flash recorded for the UI")
    if pawn is not None:
        bd.assert_true(pawn.inventory_count("Shell")
                       == shells_before + DISCOUNT_SHELLS,
                       "the discount grants 20 shells on success")
    session.choose(0)  # "The Choir is generous." ends it
    bd.assert_true(not session.active, "discount_ok exit ends the session")


def autotest_check_fail_and_quest():
    """Persuasion failure routes to discount_no and grants nothing; then
    the quest handout starts the quest and writes the native log."""
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the failure branch")
    if session is None:
        return
    pawn = systems.player_pawn()
    shells_before = pawn.inventory_count("Shell") if pawn is not None else 0
    session.rng = _Roller([4])  # 4 + 2 + 2 = 8 < 12
    result = session.choose(IDX_PERSUASION)
    bd.assert_true(isinstance(result, dict) and not result["success"]
                   and result["total"] == 8,
                   "scripted roller drives the failure branch (4+2+2=8)")
    bd.assert_true(session.active_node.id == "discount_no",
                   "failure routes to fail_next (discount_no)")
    bd.assert_true(session.last_check["text"] == "(Failed) 8 vs DC 12",
                   "failure flash recorded for the UI")
    if pawn is not None:
        bd.assert_true(pawn.inventory_count("Shell") == shells_before,
                       "a failed haggle grants no shells")
    session.choose(0)  # "Full price, then." -> start
    bd.assert_true(session.active_node.id == "start",
                   "discount_no routes back to start")
    # Quest handout: starts the quest AND writes the native player log.
    session.choose(IDX_HANDOUT)  # "Is there work for the damned?"
    quest = bd_quests.log.get(QUEST_ID)
    bd.assert_true(quest is not None and quest.state == Quest.ACTIVE,
                   "accepting the errand starts the quest")
    bd.assert_true(session.active_node.id == "errand",
                   "the handout routes to the errand node")
    bd.assert_true(bd.player_log() == LOG_TEXT,
                   "log= wrote the native Strife journal line")
    session.choose(0)  # "It will be done." ends it
    bd.assert_true(not session.active, "errand exit ends the session")


def autotest_quest_condition():
    """The handout choice's condition hides it once the quest is active."""
    session = systems.start_talk()
    bd.assert_true(session is not None, "session restarted post-quest")
    if session is None:
        return
    entries = session.choices()
    texts = [choice.text for choice, _enabled, _ann in entries]
    bd.assert_true(len(entries) == 5
                   and "Is there work for the damned?" not in texts,
                   "condition() hides the handout once the quest is active")
    session.end()


def autotest_pickup():
    """Walk (teleport) onto the crate; track_pickup credits the objective."""
    pawn = systems.player_pawn()
    crate_pos = systems.crate_pos
    bd.assert_true(pawn is not None and crate_pos is not None,
                   "pickup fixture available")
    if pawn is None or crate_pos is None:
        return
    # set_position is a raw SetOrigin (no touch check), so nudge the pawn
    # into real movement: the next tick's P_TryMove overlaps the crate and
    # fires the native pickup path.
    pawn.set_position(*crate_pos, check=False)
    pawn.set_velocity(2.0, 0.0, 0.0)


def autotest_marker_far():
    """Park the player well beyond talk range (the crate sits at the exact
    edge of it, too marginal for a marker assert); the out-of-range check
    itself runs in autotest_marker_out_of_range after a refresh pass."""
    pawn = systems.player_pawn()
    npc = systems.npc_ref
    bd.assert_true(pawn is not None and npc is not None and npc.valid,
                   "marker out-of-range fixture available")
    if pawn is None or npc is None or not npc.valid:
        return
    pawn.set_position(npc.x + 4.0 * TALK_RANGE, npc.y, npc.z, check=False)


def autotest_quest_done():
    quest = bd_quests.log.get(QUEST_ID)
    bd.assert_true(quest is not None, "quest registered")
    if quest is None:
        return
    obj = quest.objective("fetch_reliquary")
    bd.assert_true(obj is not None and obj.done,
                   "picking up the crate completed the objective")
    bd.assert_true(quest.state == Quest.COMPLETED,
                   "the fetch quest completed")
    bd.assert_true(systems.factions.reputation(content.FACTION) == 1,
                   "quest completion granted Choir reputation +1")


def autotest_marker_out_of_range():
    """Beyond talk range with no session open, the talk label hides."""
    pawn = systems.player_pawn()
    npc = systems.npc_ref
    bd.assert_true(pawn is not None and npc is not None and npc.valid,
                   "out-of-range fixture available")
    if pawn is not None and npc is not None and npc.valid:
        bd.assert_true(pawn.distance_to(npc) > TALK_RANGE,
                       "player parked beyond talk range")
    bd.assert_true(bd_dialogue.active_session() is None,
                   "no session open for the out-of-range check")
    bd.assert_true(systems.marker_state["talk"] is False
                   and systems.marker_state["talk_text"] == "",
                   "talk label hides out of range")


def autotest_gate_open():
    """Post-reputation: the rumor branch unlocks and routes."""
    pawn = systems.player_pawn()
    npc = systems.npc_ref
    bd.assert_true(pawn is not None and npc is not None
                   and npc.valid, "gate-open fixture available")
    if pawn is None or npc is None or not npc.valid:
        return
    # The player is at the crate (224 units out) - walk back into range.
    pawn.set_position(npc.x - 64.0, npc.y, npc.z, check=False)
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the unlocked rumor")
    if session is None:
        return
    entries = session.choices()
    labels = _choice_labels(entries)
    bd.assert_true(labels[IDX_RUMOR][1] is True and labels[IDX_RUMOR][2] == "",
                   "rumor choice unlocked post-reputation")
    session.choose(IDX_RUMOR)
    bd.assert_true(session.active_node.id == "rumor",
                   "the unlocked rumor routes to its node")
    session.choose(0)  # "Chilling." -> start
    bd.assert_true(session.active_node.id == "start",
                   "rumor exit routes back to start")
    session.end()


def autotest_session_guard():
    """One conversation at a time: a second start warns and no-ops."""
    npc = systems.npc_ref
    first = bd_dialogue.DialogueSession(systems.dialogue, npc,
                                        character=systems.character,
                                        factions=systems.factions,
                                        quest_log=bd_quests.log)
    bd.assert_true(first.start(), "first session starts")
    second = bd_dialogue.DialogueSession(systems.dialogue, npc,
                                         character=systems.character,
                                         factions=systems.factions,
                                         quest_log=bd_quests.log)
    bd.assert_true(not second.start() and not second.active,
                   "second start() no-ops while another session is active")
    bd.assert_true(bd_dialogue.active_session() is first,
                   "the first session keeps the active slot")
    first.end()
    bd.assert_true(bd_dialogue.active_session() is None,
                   "ending releases the active slot")


def autotest_intimidation_success():
    """Intimidation success: rounds granted and the room grows afraid."""
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the intimidation branch")
    if session is None:
        return
    pawn = systems.player_pawn()
    ammo_class = content.INTIMIDATION_AMMO_CLASS
    rounds_before = (pawn.inventory_count(ammo_class)
                     if pawn is not None else 0)
    dread_before = systems.horror.dread.level
    session.rng = _Roller([10])  # 10 + 2 (cha 14) + 2 (prof) = 14 >= 12
    result = session.choose(IDX_INTIMIDATION)
    bd.assert_true(isinstance(result, dict) and result["success"]
                   and result["total"] == 14,
                   "scripted roller drives intimidation success (14 vs 12)")
    bd.assert_true(session.active_node.id == "intimidate_ok",
                   "intimidation success routes to intimidate_ok")
    if pawn is not None:
        bd.assert_true(pawn.inventory_count(ammo_class)
                       == rounds_before + content.INTIMIDATION_AMMO_COUNT,
                       "the threat grants 30 rifle rounds on success")
    # Allow one decay tick of slack (decay is 1.0 per 35-tic tick).
    bd.assert_true(systems.horror.dread.level
                   >= dread_before + content.INTIMIDATION_DREAD - 1.0,
                   "a successful threat raises the dread")
    session.choose(0)  # "Wise." ends it
    bd.assert_true(not session.active, "intimidate_ok exit ends the session")


def autotest_intimidation_fail():
    """Intimidation failure routes to the threat node, grants nothing, and
    erodes the Inquisitor's attitude by ATTITUDE_DROP."""
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the intimidation failure")
    if session is None:
        return
    pawn = systems.player_pawn()
    ammo_class = content.INTIMIDATION_AMMO_CLASS
    rounds_before = (pawn.inventory_count(ammo_class)
                     if pawn is not None else 0)
    session.rng = _Roller([4])  # 4 + 2 + 2 = 8 < 12
    result = session.choose(IDX_INTIMIDATION)
    bd.assert_true(isinstance(result, dict) and not result["success"]
                   and result["total"] == 8,
                   "scripted roller drives intimidation failure (8 vs 12)")
    bd.assert_true(session.active_node.id == "intimidate_no",
                   "intimidation failure routes to the threat node")
    if pawn is not None:
        bd.assert_true(pawn.inventory_count(ammo_class) == rounds_before,
                       "a failed threat grants no rounds")
    session.choose(0)  # "...I misspoke." -> start
    bd.assert_true(session.active_node.id == "start",
                   "the threat node routes back to start")
    session.end()


def autotest_attitude():
    """The attitude ctx: keys exist, failed threats erode the Inquisitor,
    and the "You again." greeting unlocks at -50 with a standing-keyed
    body line. Runs after autotest_intimidation_fail (attitude -20)."""
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the attitude branch")
    if session is None:
        return
    ctx = session.context()
    bd.assert_true(isinstance(ctx.get("attitude"), int)
                   and ctx.get("attitude") == systems.attitude
                   and ctx.get("attitude_standing")
                   == systems.attitude_standing(),
                   "the session ctx carries attitude and attitude_standing")
    texts = [choice.text for choice, _en, _ann in session.choices()]
    bd.assert_true(content.YOU_AGAIN_CHOICE_TEXT not in texts,
                   "the 'You again.' greeting stays hidden above -50")
    bd.assert_true(content.HINT_START_ATTITUDE
                   not in session.active_node.text,
                   "no attitude hint at -20 (above the -30 hint line)")
    # Two more failed threats: -20 -> -40 -> -60. Each fails the room
    # (4 + 2 + 2 = 8 < 12), nothing granted, attitude eroded by 20.
    for expected in (-40, -60):
        session.rng = _Roller([4])
        session.choose(IDX_INTIMIDATION)
        session.choose(0)  # "...I misspoke." -> start
        bd.assert_true(systems.attitude == expected,
                       f"a failed threat drops the attitude to {expected}")
        texts = [choice.text for choice, _en, _ann in session.choices()]
        hidden = content.YOU_AGAIN_CHOICE_TEXT not in texts
        hinted = content.HINT_START_ATTITUDE in session.active_node.text
        if expected == -60:
            bd.assert_true(not hidden,
                           "the 'You again.' greeting appears at -60")
            bd.assert_true(not hinted,
                           "the attitude hint lifts once the greeting is "
                           "revealed")
        else:
            bd.assert_true(hidden,
                           "the greeting stays hidden at -40 (above -50)")
            bd.assert_true(hinted,
                           "the attitude hint appears at -40 (greeting "
                           "still hidden)")
    session.choose(texts.index(content.YOU_AGAIN_CHOICE_TEXT))
    bd.assert_true(session.active_node.id == "greeting",
                   "the greeting choice routes to the greeting node")
    bd.assert_true(session.active_node.text
                   == content.GREETING_LINES[systems.attitude_standing()],
                   "the greeting body matches the hostile standing")
    session.choose(0)  # "Business. Now." -> start
    bd.assert_true(session.active_node.id == "start",
                   "the greeting routes back to the start node")
    session.end()


def autotest_hidden_choice():
    """The mark choice: absent below dread 50, present at/above it, and it
    writes the second native player-log line. Along the way: the dread
    hint line on the smalltalk node within 20 below the threshold."""
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the mark branch")
    if session is None:
        return
    systems.horror.dread.set_level(10.0)
    entries = session.choices()
    texts = [choice.text for choice, _en, _ann in entries]
    bd.assert_true(content.MARK_CHOICE_TEXT not in texts,
                   "mark choice ABSENT at dread < 50")
    # The discoverability hint lives on the smalltalk node: visit it at
    # dread 10 (no hint), 35 (within 20 of the threshold: hint), 60 (the
    # mark choice is revealed, so the hint lifts and the base text
    # returns exactly).
    session.choose(IDX_SMALLTALK)
    bd.assert_true(session.active_node.id == "smalltalk",
                   "hint detour routed to the smalltalk node")
    session.choices()  # choices() is what composes the hint lines
    base_text = session.active_node.text
    bd.assert_true(content.HINT_SMALLTALK_DREAD not in base_text,
                   "no dread hint far below the threshold")
    systems.horror.dread.set_level(35.0)
    session.choices()
    bd.assert_true(content.HINT_SMALLTALK_DREAD in session.active_node.text,
                   "dread hint appears within 20 below the mark threshold")
    systems.horror.dread.set_level(60.0)
    session.choices()
    bd.assert_true(session.active_node.text == base_text,
                   "the dread hint lifts once the mark choice is revealed")
    session.choose(1)  # "Enough." -> start
    bd.assert_true(session.active_node.id == "start",
                   "the hint detour returned to the start node")
    entries = session.choices()
    texts = [choice.text for choice, _en, _ann in entries]
    bd.assert_true(content.MARK_CHOICE_TEXT in texts,
                   "mark choice PRESENT at dread >= 50")
    if content.MARK_CHOICE_TEXT not in texts:
        session.end()
        return
    session.choose(texts.index(content.MARK_CHOICE_TEXT))
    bd.assert_true(session.active_node is not None
                   and session.active_node.id == "confession",
                   "the mark choice routes to the confession node")
    session.choose(0)  # "I accept your confession." -> log + end
    bd.assert_true(not session.active, "the confession ends the session")
    bd.assert_true(bd.player_log() == LOG_TEXT_MARK,
                   "the confession wrote the second player-log line")
    systems.horror.dread.set_level(0.0)  # leave the room calm for the rest


def autotest_candle_and_stalker():
    """Atmosphere wiring: the dusk candle is armed, and the stalker
    director holds while a session is active."""
    bd.assert_true(systems.candle_active(),
                   "dusk candle program armed after map load")
    bd.assert_true(systems.horror.stalker.enabled is True,
                   "stalker director hunts while no session is active")
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the suppression test")
    if session is None:
        return
    bd.assert_true(systems.horror.stalker.enabled is False,
                   "stalker director suppressed during the session")
    session.end()
    bd.assert_true(systems.horror.stalker.enabled is True,
                   "stalker director resumes when the session ends")


def autotest_stale_npc():
    """Destroying the NPC mid-dialogue auto-ends the session."""
    stale_session = bd_dialogue.DialogueSession(systems.dialogue,
                                                systems.npc_ref,
                                                character=systems.character,
                                                factions=systems.factions,
                                                quest_log=bd_quests.log)
    ended = []
    stale_session.on_end.append(lambda s: ended.append(s.end_reason))
    bd.assert_true(stale_session.start(), "stale-test session starts")
    systems.npc_ref.destroy()
    bd.assert_true(stale_session.choices() == []
                   and not stale_session.active
                   and stale_session.end_reason == "stale_npc"
                   and ended == ["stale_npc"]
                   and bd_dialogue.active_session() is None,
                   "a stale NPC handle auto-ends the session")


def autotest_finish():
    bd.assert_true(systems.marker_state["mark"] is False
                   and systems.marker_state["talk"] is False,
                   "markers cleared after the Inquisitor was destroyed")
    bd.log("INTERROGATION AUTOTEST assertions complete")
