"""The Confessor - thin bootstrap and the headless autotest schedule.

This module is deliberately small: content (prose/factories) lives in
:mod:`confessor_content`, event wiring and rules in
:mod:`confessor_systems`, and the ImGui Rite panel in :mod:`confessor_ui`.
What remains here is the deterministic autotest (``BD_EXAMPLE_AUTOTEST=1``)
and the screenshot pose (``BD_EXAMPLE_SCREENSHOT=1``).

The autotest drives the whole native-Strife flow headlessly along the
real player path: it declines first (asserting the quest stays startable,
the offer-stands toast lands, and the quest-giver marker keeps burning),
then re-enters the conversation and accepts, asserting both conversation
event payloads, the ``INCONVERSATION`` flag lifecycle, the log label
stored by ``bd.player_log()``, the granted shells, the reward toast, the
quest completion, and the marker stand-down; plus the horror dressing
(the booth candle program armed after map load, every toast in
``bd_horror``'s history).

Manifest entry 4 of 4 (runs after its siblings have self-registered).
"""

import os

import biaseddoom as bd
import bd_quests
from bd_quests import Quest
from bd_horror import toasts

try:
    import confessor_content as content
    import confessor_systems as systems
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="confessor_content")
    systems = bd.import_script("pyscripts/systems.py",
                               module_name="confessor_systems")

AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"
SCREENSHOT = os.environ.get("BD_EXAMPLE_SCREENSHOT") == "1"

NPC_CLASS = content.NPC_CLASS
LOG_NUMBER = content.LOG_NUMBER
LOG_LABEL = content.LOG_LABEL
FIRST_NODE = content.FIRST_NODE
REPLY_ACCEPT = content.REPLY_ACCEPT
REPLY_DECLINE = content.REPLY_DECLINE


# --- run-mode scheduling ------------------------------------------------------------


@bd.on("map_load")
def schedule_run_modes(event):
    if AUTOTEST:
        bd.schedule(autotest_start_first, delay=3)
    if SCREENSHOT:
        bd.schedule(screenshot_pose, delay=bd.TICRATE)
        # The native conversation menu freezes the world ~20 gametics after
        # opening (dialogue freeze), which stops the Python task clock
        # entirely (OnWorldPreTick lives in the paused world tick). The
        # screenshot and quit therefore run as two close pre-freeze tasks:
        # the capture needs one rendered frame before quit lands, and the
        # console buffer processes quit even while the world is paused.
        bd.schedule(lambda: bd.execute("screenshot /tmp/confessor_rite"),
                    delay=bd.TICRATE + 9)
        bd.schedule(lambda: bd.execute("quit"), delay=bd.TICRATE + 15)


def screenshot_pose():
    """Open the native conversation menu for a documentation capture."""
    pawn = systems.player_pawn()
    npc = systems.npc_ref
    if pawn is not None and npc is not None and npc.valid:
        pawn.set_position(npc.x - 72.0, npc.y, npc.z, check=False)
        pawn.angle = 0.0
        bd.start_conversation(npc)



# --- autotest driver ----------------------------------------------------------------


def autotest_start_first():
    npc = systems.npc_ref
    bd.assert_true(npc is not None and npc.valid, "QuestScribe spawned")
    if npc is None or not npc.valid:
        return
    bd.assert_true(npc.class_name == NPC_CLASS,
                   "spawned actor is a QuestScribe")
    bd.assert_true(systems.candle_active(),
                   "booth candle program armed after map load")
    # The quest-giver marker burns while the quest can be accepted.
    quest = bd_quests.log.get(content.QUEST_ID)
    bd.assert_true(quest is not None and quest.state == Quest.INACTIVE,
                   "quest inactive before the first talk")
    bd.assert_true(systems.marker_state["shown"] is True,
                   "quest-giver marker shown before accept")
    bd.assert_true(systems.marker_state["draws"] >= 1,
                   "marker registered on the display list")
    started = bd.start_conversation(npc)
    bd.assert_true(started is True, "start_conversation returned True")
    bd.schedule(autotest_check_first_menu, delay=2)


def autotest_check_first_menu():
    npc = systems.npc_ref
    # conversation_started fires synchronously out of P_StartConversation.
    bd.assert_true(len(systems.started_events) == 1,
                   "conversation_started fired once")
    if systems.started_events:
        ev = systems.started_events[-1]
        bd.assert_true(ev.get("player_index") == 0,
                       "started: player_index is 0")
        bd.assert_true(ev.get("npc_class") == NPC_CLASS,
                       "started: npc_class is QuestScribe")
        npc_ev = ev.get("npc_ref")
        bd.assert_true(npc_ev is not None and npc_ev.valid,
                       "started: npc_ref is a live handle")
        pc = ev.get("pc_ref")
        pawn = systems.player_pawn()
        bd.assert_true(pc is not None and pawn is not None and pc == pawn,
                       "started: pc_ref is the local player's pawn")
    # The menu is open while the NPC carries the internal in-conversation
    # flag (read through the ZScript accessor - see ZSCRIPT).
    bd.assert_true(npc.call_zscript("InConversation") == 1,
                   "NPC in conversation (menu open) after start")
    # Talking itself starts the quest (the offer is on the table now).
    quest = bd_quests.log.get(content.QUEST_ID)
    bd.assert_true(quest is not None and quest.state == Quest.ACTIVE,
                   "talking started the quest")
    # Decline first, through the console-command netcode bridge; HandleReply
    # commits it on the next gametic and fires conversation_reply. The
    # quest must survive a refusal.
    systems.pick_reply(FIRST_NODE, REPLY_DECLINE)
    bd.schedule(autotest_check_decline_reply, delay=8)


def autotest_check_decline_reply():
    npc = systems.npc_ref
    bd.assert_true(len(systems.reply_events) == 1,
                   "conversation_reply fired once")
    if systems.reply_events:
        ev = systems.reply_events[-1]
        bd.assert_true(ev.get("player_index") == 0,
                       "decline: player_index is 0")
        bd.assert_true(ev.get("node") == FIRST_NODE, "decline: node is 0")
        bd.assert_true(ev.get("reply_index") == REPLY_DECLINE,
                       "decline: reply_index is 1")
        bd.assert_true(ev.get("log_number") == -1,
                       "decline: no log number")
        bd.assert_true(ev.get("log_string") is None,
                       "decline: no log string")
        bd.assert_true(ev.get("next_node") == -1,
                       "decline: next_node is -1 (no nextpage)")
        bd.assert_true(ev.get("item_changed") is False,
                       "decline: no inventory change")
    bd.assert_true(bd.player_log() is None,
                   "decline reply left the log untouched")
    bd.assert_true(
        any(entry.get("text") == content.TOAST_DECLINE
            for entry in toasts.history),
        "decline toast recorded in bd_horror history")
    bd.assert_true(
        any(entry.get("kind") == "quest"
            and entry.get("text") == content.TOAST_OFFER_STANDS
            for entry in toasts.history),
        "offer-stands toast recorded after a decline")
    bd.assert_true(systems.last_rite_line == content.RITE_DECLINED,
                   "the Rite panel tracks the declined confession")
    bd.assert_true(npc.call_zscript("InConversation") == 0,
                   "conversation closed after the decline")
    quest = bd_quests.log.get(content.QUEST_ID)
    bd.assert_true(quest is not None and quest.state == Quest.ACTIVE,
                   "quest still active (startable) after a decline")
    bd.assert_true(systems.marker_state["shown"] is True,
                   "quest-giver marker still shown after a decline")
    # The offer stands: re-enter the conversation and accept this time.
    started = bd.start_conversation(npc)
    bd.assert_true(started is True,
                   "conversation re-entered after a decline")
    bd.schedule(autotest_check_second_menu, delay=2)


def autotest_check_second_menu():
    npc = systems.npc_ref
    bd.assert_true(len(systems.started_events) == 2,
                   "conversation_started fired again for the second talk")
    bd.assert_true(npc.call_zscript("InConversation") == 1,
                   "NPC in conversation (menu open) after re-entry")
    # Accept through the same netcode bridge; HandleReply commits it on
    # the next gametic and fires conversation_reply.
    systems.pick_reply(FIRST_NODE, REPLY_ACCEPT)
    bd.schedule(autotest_final_asserts, delay=8)


def autotest_final_asserts():
    npc = systems.npc_ref
    bd.assert_true(len(systems.reply_events) == 2,
                   "second conversation_reply fired")
    if len(systems.reply_events) >= 2:
        ev = systems.reply_events[-1]
        bd.assert_true(ev.get("player_index") == 0, "reply: player_index is 0")
        bd.assert_true(ev.get("node") == FIRST_NODE, "reply: node is 0")
        bd.assert_true(ev.get("reply_index") == REPLY_ACCEPT,
                       "reply: reply_index is 0")
        bd.assert_true(ev.get("log_number") == LOG_NUMBER,
                       "reply: log_number is 77")
        bd.assert_true(ev.get("log_string") is None,
                       "reply: log_string unset for numeric logs")
        bd.assert_true(ev.get("next_node") == -1,
                       "reply: next_node is -1 (no nextpage)")
        bd.assert_true(ev.get("item_changed") is True,
                       "reply: item_changed (shells given)")
        npc_ev = ev.get("npc_ref")
        bd.assert_true(npc_ev is not None and npc_ev.valid,
                       "reply: npc_ref is a live handle")
    bd.assert_true(bd.player_log() == LOG_LABEL,
                   "player_log holds the $TXT_LOGTEXT77 label")
    bd.assert_true(
        any(entry.get("kind") == "quest" and entry.get("text") == content.TOAST_ACCEPT
            for entry in toasts.history),
        "log-update toast recorded in bd_horror history")
    bd.assert_true(
        any(entry.get("kind") == "loot" and entry.get("text") == content.TOAST_REWARD
            for entry in toasts.history),
        "reward toast naming the shells recorded in bd_horror history")
    bd.assert_true(systems.last_rite_line == content.RITE_ACCEPTED,
                   "the Rite panel tracks the accepted confession")
    pawn = systems.player_pawn()
    if pawn is not None:
        bd.assert_true(pawn.inventory_count("Shell") >= 1,
                       "reply gave shells through the native pickup path")
    bd.assert_true(npc.call_zscript("InConversation") == 0,
                   "conversation closed after the reply")
    quest = bd_quests.log.get(content.QUEST_ID)
    bd.assert_true(quest is not None and quest.state == Quest.COMPLETED,
                   "quest completed via track_conversation_log")
    # The marker stands down once the rite is sealed.
    bd.assert_true(systems.marker_state["shown"] is False,
                   "quest-giver marker cleared on accept")
    bd.assert_true(systems.marker_state["clears"] >= 1,
                   "marker clear ran against the display list")
    bd.log("CONFESSION RITE AUTOTEST assertions complete")
