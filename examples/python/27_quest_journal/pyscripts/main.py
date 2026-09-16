"""Whispers in the Walls: bootstrap, event wiring, and the autotest driver.

An occult-investigation mini-campaign on Doom II MAP01, built on the
shipped ``bd_quests`` and ``bd_horror`` packages. Architecture:

- ``content.py``: pure data and factories (quests, cast, prose). No
  engine calls at import time.
- ``systems.py``: directors and game rules built from content (quest
  trackers, the rite's blackout/flicker, the stalker, the fail branch).
- ``ui.py``: the Grimoire, a ``bd_horror.theme``-skinned ImGui journal.
- ``main.py`` (this file): imports the siblings, registers every engine
  event, and drives the deterministic autotest.

The PYTHON manifest lists all four modules; the runtime execs each one,
and this file imports the other three as siblings via ``bd.import_script``
(the hello_world pattern). The libraries are definition-only at import,
so their manifest execution is inert and all wiring happens here, once.

The journal toggles with the J key or the ``toggle_journal`` console
command, wired manually through the ``alias``/``pyui``/``ui_command``
bridge (the same pattern ``bd_quests.bind_journal_toggle`` uses, but
aimed at our custom themed window).

Autotest: ``BD_EXAMPLE_AUTOTEST=1`` drives the whole campaign headlessly
and deterministically: the fail branch (synthetic sector-exit probe),
the opening beat (goal centered, guidance whispers toasted), the
page-marker lifecycle (lit over all three pages before the walk, cleared
after it), a real three-page pickup walk, the ritual blackout (fired and
restored), the rite's plain stay-inside rule toast, the beacon lifecycle
(lit while the rite is current, guttered when it completes),
player-attributed wave kills, the quest chain unsealing, the Choir's
center-screen announcement, the quest xp reward sink crediting the favor
ledger with exact amounts, a checkpoint save/load round-trip, and the
journal toggle. The run ends via ``-scripttest``'s own PASS/FAIL
accounting.

``BD_EXAMPLE_SCREENSHOT=1`` schedules a screenshot a few seconds in, for
documentation captures.
"""

import os

import biaseddoom as bd
import bd_quests
from bd_quests import Objective, Quest
from bd_horror import toasts

content = bd.import_script("pyscripts/content.py", module_name="witw_content")
systems = bd.import_script("pyscripts/systems.py", module_name="witw_systems")
ui = bd.import_script("pyscripts/ui.py", module_name="witw_ui")

AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"
SCREENSHOT = os.environ.get("BD_EXAMPLE_SCREENSHOT") == "1"

# --- module state ------------------------------------------------------------

world = None        # systems.World, built at engine_start
grimoire = None     # ui.Grimoire, built at engine_start
autowarp_done = False
grimoire_drawn_ok = False


# --- engine wiring --------------------------------------------------------------


@bd.on("engine_start")
def on_engine_start(event):
    global world, grimoire
    bd.imgui.set_master_visible(True)
    world = systems.build_world()
    systems.setup_engine(world, content)
    grimoire = ui.Grimoire(world.log, title="Grimoire",
                           xp_ledger=world.xp_ledger)
    # Console/key toggle, wired manually through the pyui bridge: running
    # `toggle_journal` (or pressing J) fires a ui_command event that flips
    # the Grimoire's visibility.
    bd.execute('alias toggle_journal "pyui toggle_journal"')
    bd.execute("bind j toggle_journal")


@bd.on("ui_command")
def on_ui_command(event):
    if event.get("command") == "toggle_journal" and grimoire is not None:
        grimoire.toggle()


@bd.on("map_load")
def on_map(event):
    systems.setup_map(world, content, event)

    if event.get("from_savegame"):
        # The checkpoint restored the world and bd_quests' load handler
        # restored the quests; only the post-load test steps are needed.
        if AUTOTEST:
            bd.schedule(autotest_kill_third, delay=10)
            bd.schedule(autotest_chain_asserts, delay=16)
            bd.schedule(autotest_kill_choir, delay=24)
            bd.schedule(autotest_choir_asserts, delay=30)
            bd.schedule(autotest_journal_toggle_off, delay=34)
            bd.schedule(autotest_journal_toggle_on, delay=40)
            bd.schedule(autotest_final_asserts, delay=48)
        return

    pawn = systems.player_pawn()
    if pawn is None:
        return

    if AUTOTEST:
        pawn.damage_factor = 0.0  # nothing may kill the test driver
        bd.schedule(autotest_fail_branch_probe, delay=5)
        bd.schedule(autotest_intro_asserts, delay=6)
        bd.schedule(autotest_walk_pages, delay=8)
        bd.schedule(autotest_stop_walk, delay=88)
        bd.schedule(autotest_pages_asserts, delay=96)
        bd.schedule(autotest_enter_ritual, delay=104)
        bd.schedule(autotest_rite_begin_asserts, delay=108)
        bd.schedule(autotest_kill_two, delay=118)
        bd.schedule(autotest_mid_asserts, delay=126)
        bd.schedule(autotest_blackout_restored, delay=144)
        bd.schedule(autotest_flicker_assert, delay=152)
        bd.schedule(autotest_save, delay=158)
        bd.schedule(autotest_load, delay=172)
    if SCREENSHOT:
        bd.schedule(screenshot_omen, delay=30)
        bd.schedule(lambda: bd.execute("screenshot /tmp/witw_grimoire"),
                    delay=88)
        bd.schedule(lambda: bd.execute("quit"), delay=175)


# --- autotest: the fail branch (deterministic synthetic drive) ---------------------


def autotest_fail_branch_probe():
    """Prove the mid-rite exit fail branch with a probe quest log.

    Uses a separate QuestLog (its own bd.state key) so the probe quest
    never clutters the real Grimoire, but drives the same production
    RiteDirector code path the engine's sector_exited event uses.
    """
    probe_log = bd_quests.QuestLog(state_key="witw_probe")
    quest = Quest("probe_rite", "Probe Rite")
    quest.add_objective(Objective("enter_circle", "enter"))
    quest.add_objective(Objective("silence_dead", "wave", count=3))
    probe_log.add(quest)
    director = systems.RiteDirector(
        probe_log, "probe_rite", content.RITUAL_TAG,
        fail_reason=content.row("rite")["fail_reason"],
        fail_toast=content.TOAST_RITE_FAILED)
    bd.assert_true(not director.rite_active(),
                   "probe rite inert before the circle is entered")
    bd.assert_true(not director.on_sector_exited({"tags": [content.RITUAL_TAG]}),
                   "leaving the circle before the rite begins is harmless")
    quest.start()
    probe_log.complete_objective("probe_rite", "enter_circle")
    bd.assert_true(director.rite_active(),
                   "probe rite live once the wave objective is current")
    bd.assert_true(not director.on_sector_exited({"tags": [999]}),
                   "leaving an unrelated sector does not fail the rite")
    fired = director.on_sector_exited({"tags": [content.RITUAL_TAG]})
    bd.assert_true(fired, "mid-rite exit drives the fail branch")
    bd.assert_true(quest.state == Quest.FAILED
                   and "abandoned" in quest.fail_reason,
                   "probe rite failed with the documented reason")
    bd.assert_true(any(entry["kind"] == "omen" and "absence" in entry["text"]
                       for entry in toasts.history),
                   "fail branch recorded an omen toast")
    world.probe_log = probe_log  # post-load persistence assert


# --- autotest: the opening beat and the page markers --------------------------------


def autotest_intro_asserts():
    # The walk starts at tic 8, so all three pages are still on the floor
    # when this runs: every one of them must already carry its markers.
    bd.assert_true(any(content.INTRO_CENTER in line
                       for line in world.center_log),
                   "intro centered the goal on a fresh map")
    markers = systems.marker_state["pages"]
    bd.assert_true(len(markers) == len(content.PAGE_TIDS)
                   and all(tid in markers for tid in content.PAGE_TIDS),
                   "a ring and label mark every uncollected page")


# --- autotest: quest 1, the pages ----------------------------------------------------


def autotest_walk_pages():
    # item_picked only fires for real world pickups (Inventory.Touch),
    # which require genuine movement: teleports never touch items. Line up
    # west of the page line and run east through all three. +speed makes
    # it a run: headless console walking is slow (~1 unit/tic).
    pawn = systems.player_pawn()
    if pawn is not None:
        pawn.set_position(content.PAGE_WALK_START[0], content.PAGE_WALK_START[1],
                          content.PAGE_WALK_START[2], check=False, fog=False)
        pawn.angle = content.PAGE_WALK_ANGLE
    bd.execute("+speed")
    bd.execute("+forward")


def autotest_stop_walk():
    bd.execute("-forward")
    bd.execute("-speed")


def autotest_pages_asserts():
    pages = world.log.get("pages")
    bd.assert_true(pages.state == Quest.COMPLETED,
                   "pages completed by walking over all three")
    pawn = systems.player_pawn()
    if pawn is not None:
        bd.assert_true(pawn.inventory_count(content.PAGE_CLASS) == 3,
                       "all three desecrated pages in inventory")
        bd.assert_true(pawn.inventory_count("Shell") >= 8,
                       "pages reward shells granted")
    bd.assert_true(world.log.get("rite").state == Quest.ACTIVE,
                   "the rite unsealed when the pages were recovered")
    bd.assert_true(world.log.get("choir").state == Quest.INACTIVE,
                   "the Choir stays sealed until the rite is done")
    bd.assert_true(sum(1 for entry in toasts.history
                       if entry["kind"] == "loot") >= 3,
                   "page pickup whispers recorded as loot toasts")
    bd.assert_true(world.xp_ledger.get("pages")
                   == content.row("pages")["reward_xp"]
                   and sum(world.xp_ledger.values())
                   == content.row("pages")["reward_xp"],
                   "pages completion fired the xp sink with the exact "
                   "amount")
    bd.assert_true(not systems.marker_state["pages"],
                   "page markers cleared once every page was recovered")
    bd.assert_true(any("press J" in entry["text"]
                       for entry in toasts.history),
                   "intro whisper named the journal key")
    bd.assert_true(any("markers light the way" in entry["text"]
                       for entry in toasts.history),
                   "intro whisper named the guiding markers")


# --- autotest: quest 2, the rite -----------------------------------------------------


def autotest_enter_ritual():
    pawn = systems.player_pawn()
    if pawn is not None:
        pawn.set_position(content.RITUAL_SPOT[0], content.RITUAL_SPOT[1],
                          content.RITUAL_SPOT[2], check=False, fog=False)


def autotest_rite_begin_asserts():
    rite = world.log.get("rite")
    bd.assert_true(rite.objective("enter_circle").done,
                   "sector tracking completed the circle entry")
    bd.assert_true(world.director.rite_active(),
                   "the rite is live once the circle is entered")
    sector = bd.sector_at(content.RITUAL_SPOT[0], content.RITUAL_SPOT[1])
    bd.assert_true(world.ritual_light_before is not None
                   and sector is not None and int(sector.light) == 0,
                   "blackout fired on ritual sector entry")
    bd.assert_true(world.horror.stalker.enabled,
                   "the stalker wakes while the rite runs")
    for tid in content.WAVE_TIDS:
        ref = bd.actor_ref(tid)
        bd.assert_true(ref is not None and ref.valid and ref.alive,
                       f"whispering dead {tid} rose with the rite")
    bd.assert_true(any(entry["kind"] == "omen"
                       and "light dies" in entry["text"]
                       for entry in toasts.history),
                   "rite entry omen toast recorded")
    bd.assert_true(any("Stay inside the circle" in entry["text"]
                       for entry in toasts.history),
                   "rite start stated the stay-inside rule plainly")


def autotest_kill_two():
    # Kills are sourced to the player pawn so track_kills' default
    # killer="player" exact credit counts them.
    pawn = systems.player_pawn()
    for tid in content.WAVE_TIDS[:2]:
        ref = bd.actor_ref(tid)
        if ref is not None and ref.valid:
            ref.damage(1000, source=pawn)


def autotest_mid_asserts():
    rite = world.log.get("rite")
    bd.assert_true(rite.state == Quest.ACTIVE,
                   "rite active before checkpoint")
    bd.assert_true(rite.objective("silence_dead").progress == 2,
                   "rite at 2/3 whispers before checkpoint")
    bd.assert_true(world.log.get("choir").state == Quest.INACTIVE,
                   "the Choir remains sealed mid-rite")
    bd.assert_true(sum(1 for entry in toasts.history
                       if "throttles" in entry["text"]) >= 2,
                   "wave progress whispers recorded")
    bd.assert_true(systems.marker_state["beacon"],
                   "the rite beacon burns while the rite is current")


def autotest_blackout_restored():
    sector = bd.sector_at(content.RITUAL_SPOT[0], content.RITUAL_SPOT[1])
    bd.assert_true(sector is not None
                   and int(sector.light) == world.ritual_light_before,
                   "blackout restored the ritual sector's original light")


def autotest_flicker_assert():
    bd.assert_true(world.flicker_armed
                   and any(p.kind == "fluorescent"
                           for p in world.horror.lights.programs),
                   "fluorescent flicker armed after the blackout")


def autotest_save():
    bd.save_checkpoint(content.CHECKPOINT_NAME,
                       description="Whispers in the Walls autotest")


def autotest_load():
    bd.load_checkpoint(content.CHECKPOINT_NAME)


# --- autotest: post-load, quest 3, the Choir ------------------------------------------


def autotest_kill_third():
    # The checkpoint was taken with the third whisperer still alive, so it
    # is back; re-resolve by TID (handles never survive a savegame).
    ref = bd.actor_ref(content.WAVE_TIDS[2])
    bd.assert_true(ref is not None and ref.valid,
                   "third whisperer re-resolved by TID after checkpoint load")
    if ref is not None and ref.valid:
        ref.damage(1000, source=systems.player_pawn())


def autotest_chain_asserts():
    rite = world.log.get("rite")
    bd.assert_true(rite.state == Quest.COMPLETED,
                   "rite completed after the checkpoint round-trip")
    choir = world.log.get("choir")
    bd.assert_true(choir.state == Quest.ACTIVE,
                   "the Choir quest chains from the rite's completion")
    ref = bd.actor_ref(content.CHOIR_TID)
    bd.assert_true(world.choir_spawned and ref is not None and ref.valid
                   and ref.alive,
                   "the Choir spawned when the rite completed")
    bd.assert_true(not world.horror.stalker.enabled,
                   "the stalker sleeps once the rite is over")
    bd.assert_true(any(entry["kind"] == "omen" and "Choir" in entry["text"]
                       for entry in toasts.history),
                   "rite completion omen toast recorded")
    bd.assert_true(any(content.CHOIR_ANNOUNCE in line
                       for line in world.center_log),
                   "the Choir's arrival was announced center-screen")


def autotest_kill_choir():
    pawn = systems.player_pawn()
    ref = bd.actor_ref(content.CHOIR_TID)
    if ref is not None and ref.valid:
        ref.damage(3000, source=pawn)


def autotest_choir_asserts():
    choir = world.log.get("choir")
    bd.assert_true(choir.state == Quest.COMPLETED,
                   "the Choir silenced by a player-attributed kill")
    bd.assert_true(any(entry["kind"] == "quest"
                       and "forget your name" in entry["text"]
                       for entry in toasts.history),
                   "campaign completion toast recorded")
    bd.assert_true(world.xp_ledger.get("choir")
                   == content.row("choir")["reward_xp"]
                   and sum(world.xp_ledger.values())
                   == content.row("pages")["reward_xp"]
                   + content.row("choir")["reward_xp"],
                   "choir completion added its exact xp to the favor "
                   "ledger")
    bd.assert_true(not systems.marker_state["beacon"],
                   "the rite beacon guttered once the rite was done")


# --- autotest: the toggle and the final sweep -------------------------------------------


def autotest_journal_toggle_off():
    # The alias registered at engine_start routes `toggle_journal` through
    # pyui -> ui_command -> our handler -> grimoire.toggle().
    bd.assert_true(grimoire.visible, "grimoire visible before console toggle")
    bd.execute("toggle_journal")


def autotest_journal_toggle_on():
    bd.assert_true(not grimoire.visible,
                   "toggle_journal hid the grimoire via ui_command")
    bd.execute("toggle_journal")


def autotest_final_asserts():
    for quest_id in ("pages", "rite", "choir"):
        bd.assert_true(world.log.get(quest_id).state == Quest.COMPLETED,
                       f"{quest_id} completed after the round-trip")
    probe = world.probe_log.get("probe_rite") if world.probe_log else None
    bd.assert_true(probe is not None and probe.state == Quest.FAILED,
                   "probe quest's failure preserved across the round-trip")
    saved = bd.state.get(bd_quests.STATE_KEY)
    bd.assert_true(isinstance(saved, dict) and saved.get("quests"),
                   "bd_quests state present in bd.state after load")
    bd.assert_true(isinstance(bd.state.get("bd_horror"), dict),
                   "bd_horror state present in bd.state after load")
    pawn = systems.player_pawn()
    if pawn is not None:
        bd.assert_true(pawn.inventory_count(content.PAGE_CLASS) == 3,
                       "pages kept in inventory across the round-trip")
        bd.assert_true(pawn.inventory_count("Shell") >= 8,
                       "reward shells kept across the round-trip")
    bd.assert_true(grimoire.visible,
                   "toggle_journal showed the grimoire again (idempotent)")
    # imgui_frame never fires under -headless; the draw smoke check is
    # rendering-dependent and legitimately skipped there.
    if not bd.headless():
        bd.assert_true(grimoire_drawn_ok, "grimoire drew inside imgui_frame")
    bd.log("WHISPERS IN THE WALLS AUTOTEST assertions complete")


# --- screenshot drive ---------------------------------------------------------------


def screenshot_omen():
    toasts.toast(content.TOAST_SCREENSHOT_OMEN, kind="omen")


# --- the overlay ---------------------------------------------------------------------


@bd.on("imgui_frame")
def draw_ui(event):
    global autowarp_done, grimoire_drawn_ok
    # Headless runs (-scripttest) launch without +map: queue a warp on the
    # first rendered frame when nothing loaded a level yet.
    if not autowarp_done:
        autowarp_done = True
        try:
            if not bd.current_map():
                bd.execute("map map01")
        except RuntimeError:
            pass
    if grimoire is not None and grimoire.visible:
        grimoire.draw()
        grimoire_drawn_ok = True
    toasts.draw_toasts()  # no-op when the queue is empty
