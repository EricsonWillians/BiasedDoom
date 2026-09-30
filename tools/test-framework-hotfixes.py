#!/usr/bin/env python3
"""Offline regression checks for engine-shipped Python framework hotfixes.

These tests use a small stub for the engine's ``biaseddoom`` module so they
run without an IWAD or a display. They cover the actor-handle identity,
map-transition scheduling, and shop delivery semantics that depend on engine
API contracts, plus the second remediation batch: dialogue map-unload
cleanup, stale-session talk recovery, hub/savegame NPC adoption, and
hub-restore light-program state.
"""

from __future__ import annotations

import importlib
import sys
import types
from pathlib import Path

sys.dont_write_bytecode = True

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "src" / "python" / "lib"))


bd = types.ModuleType("biaseddoom")
bd.state = {}
bd.warnings = []
bd.played_sounds = []
bd.tasks = []


def warn(message):
    bd.warnings.append(str(message))


def log(_message):
    pass


def play_ui_sound(name):
    bd.played_sounds.append(str(name))


def schedule(fn, delay=1, repeat=None, map_local=True):
    task = {
        "fn": fn,
        "delay": delay,
        "repeat": repeat,
        "map_local": map_local,
    }
    bd.tasks.append(task)
    return task


bd.warn = warn
bd.log = log
bd.play_ui_sound = play_ui_sound
bd.schedule = schedule


# --- second-remediation-batch stub surface ------------------------------------

bd.handlers = {}


def on(event_name):
    """``bd.on`` decorator stub recording handlers for ``dispatch``."""
    def decorator(fn):
        bd.handlers.setdefault(str(event_name), []).append(fn)
        return fn
    return decorator


def dispatch(event_name, event=None):
    """Invoke every handler registered for ``event_name``."""
    for fn in list(bd.handlers.get(str(event_name), [])):
        fn(event if event is not None else {})


bd.on = on

bd.actors_by_tid = {}
bd.spawn_calls = []


class SpawnedActor:
    """bd.spawn handle stub; assigning tid mirrors the engine's
    contract that the actor becomes resolvable through bd.actor_ref."""

    def __init__(self, class_name, tid=0):
        self.class_name = str(class_name)
        self._tid = 0
        self.x = self.y = self.z = 0.0
        self.angle = 0.0
        self.speed = 1.0
        self.alive = True
        self.valid = True
        self.tid = tid

    @property
    def tid(self):
        return self._tid

    @tid.setter
    def tid(self, value):
        if self._tid and bd.actors_by_tid.get(self._tid) is self:
            del bd.actors_by_tid[self._tid]
        self._tid = int(value)
        if self._tid:
            bd.actors_by_tid[self._tid] = self

    def set_flag(self, name, value):
        pass

    def destroy(self):
        self.alive = False


def spawn(class_name, x, y, z, angle=0.0, tid=0, force=False):
    bd.spawn_calls.append((str(class_name), int(tid)))
    return SpawnedActor(class_name, tid)


def actor_ref(tid):
    return bd.actors_by_tid.get(int(tid))


def player(index):
    return None


def current_map():
    return "MAP01"


bd.sector_map = {}


class Sector:
    def __init__(self, index, light=160):
        self.index = int(index)
        self.light = int(light)


def sectors(tag=None):
    return list(bd.sector_map.get(tag, []))


def cancel_task(task):
    bd.tasks[:] = [entry for entry in bd.tasks if entry is not task]


def level_time():
    return 0


bd.spawn = spawn
bd.actor_ref = actor_ref
bd.player = player
bd.current_map = current_map
bd.sectors = sectors
bd.cancel_task = cancel_task
bd.level_time = level_time
sys.modules["biaseddoom"] = bd


def unload_map():
    """Mirror the engine cancelling map-local scheduled tasks."""
    bd.tasks[:] = [task for task in bd.tasks if not task["map_local"]]


def run_tasks():
    """Run and clear every pending one-shot task."""
    pending = list(bd.tasks)
    bd.tasks.clear()
    for task in pending:
        task["fn"]()


class ActorHandle:
    """Handle stub matching the engine's slot+generation equality."""

    def __init__(self, slot, generation, actor=None):
        self.slot = slot
        self.generation = generation
        self._actor = actor

    @property
    def valid(self):
        return self._actor is not None and self._actor.valid

    @property
    def alive(self):
        return self.valid

    def set_velocity(self, vx, vy, vz):
        pass

    def __eq__(self, other):
        return (isinstance(other, ActorHandle) and
                (self.slot, self.generation) == (other.slot, other.generation))

    def __hash__(self):
        return hash((self.slot, self.generation))


class Actor:
    valid = True

    def set_velocity(self, vx, vy, vz):
        pass


class Pawn:
    """Pawn stub with give_inventory's documented total-count semantics."""

    def __init__(self, inventory=None, maximum=None):
        self.inventory = dict(inventory or {"Coin": 10})
        self.maximum = dict(maximum or {})

    def inventory_count(self, class_name):
        return self.inventory.get(class_name, 0)

    def take_inventory(self, class_name, amount):
        self.inventory[class_name] = self.inventory.get(class_name, 0) - amount

    def give_inventory(self, class_name, amount):
        current = self.inventory.get(class_name, 0)
        limit = self.maximum.get(class_name)
        if limit is None or current < limit:
            self.inventory[class_name] = current + amount
        return self.inventory.get(class_name, 0)

    def distance_to(self, other):
        return 64.0


def test_status_handle_dedupe():
    status_module = importlib.import_module("bd_rpg.status")
    engine = status_module.StatusEngine.__new__(status_module.StatusEngine)
    engine._tracked = []

    first = ActorHandle(7, 3)
    second = ActorHandle(7, 3)
    engine._track(first)
    engine._track(second)
    assert len(engine._tracked) == 1, "same actor registered twice"

    other = ActorHandle(8, 3)
    engine._track(other)
    assert len(engine._tracked) == 2, "different actor was not registered"


def test_elements_handle_dedupe():
    elements = importlib.import_module("bd_rpg.elements")
    elements._retained.clear()
    elements._retain(ActorHandle(11, 1))
    elements._retain(ActorHandle(11, 1))
    assert len(elements._retained) == 1, "duplicate actor pin retained"
    elements._retained.clear()


def test_vtm_restore_survives_map_unload():
    vtm = importlib.import_module("bd_vtm")
    actor = Actor()
    actor.speed = 2.0
    handle = ActorHandle(13, 1, actor)

    bd.tasks.clear()
    vtm._restore_later(handle, "speed", 1.0, delay=10)
    assert len(bd.tasks) == 1
    assert bd.tasks[0]["map_local"] is False, "VtM revert is still map-local"

    unload_map()
    assert len(bd.tasks) == 1, "VtM revert was cancelled by a map change"
    run_tasks()
    assert handle.speed == 1.0, "VtM attribute revert did not run"


def test_shop_delivery_and_restock():
    services = importlib.import_module("bd_npcs.services")
    shop = services.Shop(currency_class="Coin", stock=(
        {"class_name": "Clip", "price": 2, "max_count": 3,
         "restock_tics": 20},
        {"class_name": "Medkit", "price": 5, "max_count": 1,
         "restock_tics": 0},
    ))

    pawn = Pawn()
    result = shop.buy(pawn, 0)
    assert result["ok"], f"normal purchase failed: {result}"
    assert pawn.inventory_count("Coin") == 8
    assert pawn.inventory_count("Clip") == 1
    assert shop._counts[0] == 2

    # give_inventory returns the actor's total, so the engine API alone
    # cannot prove delivery when the give is refused at max amount.
    limited = Pawn(inventory={"Coin": 10, "Medkit": 1},
                   maximum={"Medkit": 1})
    result = shop.buy(limited, 1)
    assert not result["ok"], "purchase at max inventory unexpectedly succeeded"
    assert limited.inventory_count("Coin") == 10, "failed purchase was not refunded"
    assert limited.inventory_count("Medkit") == 1
    assert shop._counts[1] == 1

    assert len(bd.tasks) == 1, "restock task was not scheduled"
    assert bd.tasks[0]["map_local"] is False, "restock task is still map-local"
    unload_map()
    assert len(bd.tasks) == 1, "restock task was cancelled by a map change"
    run_tasks()
    assert shop._counts[0] == 3, "restock task did not restore stock"


def _small_dialogue(dialogue_id):
    """One-node/one-choice dialogue tree that passes validation."""
    bd_dialogue = importlib.import_module("bd_dialogue")
    dialogue = bd_dialogue.Dialogue(dialogue_id)
    dialogue.add_node(bd_dialogue.Node("start", "NPC", "Hello."))
    dialogue.node("start").add_choice(bd_dialogue.Choice("Bye.", end=True))
    return bd_dialogue, dialogue


def test_dialogue_map_unload_ends_session():
    bd_dialogue, dialogue = _small_dialogue("unload_test")
    npc = Actor()
    session = bd_dialogue.DialogueSession(dialogue, ActorHandle(31, 1, npc))
    assert session.start(), "dialogue session did not start"
    assert bd_dialogue.active_session() is session

    dispatch("map_unload", {})

    assert not session.active, "map_unload left the session active"
    assert session.end_reason == "map_change", \
        f"unexpected end reason {session.end_reason!r}"
    assert bd_dialogue.active_session() is None, \
        "map_unload did not clear the active session slot"


def test_begin_talk_recovers_from_stale_session():
    bd_dialogue, dialogue = _small_dialogue("stale_test")
    npcs = importlib.import_module("bd_npcs.npcs")

    doomed = Actor()
    stale = bd_dialogue.DialogueSession(dialogue, ActorHandle(41, 1, doomed))
    assert stale.start()
    doomed.valid = False  # the conversation partner dies mid-session

    manager = npcs.NPCManager()
    manager.register(npcs.NPCDefinition(
        "keeper", "Shop Keeper", "ZombieMan", dialogue=dialogue))
    manager._handles["keeper"] = SpawnedActor("ZombieMan", 4100)
    manager._tids["keeper"] = 4100

    session = manager.begin_talk(
        Pawn(), session_class=bd_dialogue.DialogueSession)
    assert session is not None, "stale session blocked a new conversation"
    assert session.active
    assert not stale.active and stale.end_reason == "stale_npc"
    assert bd_dialogue.active_session() is session
    session.end("manual")


def test_spawn_all_from_hub_adopts_restored_actor():
    npcs = importlib.import_module("bd_npcs.npcs")
    manager = npcs.NPCManager()
    manager.register(npcs.NPCDefinition(
        "warden", "Warden", "ZombieMan", tid_base=5000))

    bd.spawn_calls.clear()
    spawned = manager.spawn_all()
    assert len(spawned) == 1
    assert bd.spawn_calls == [("ZombieMan", 5000)]
    assert manager._tids["warden"] == 5000

    # Hub snapshot reopen: the engine restores the actor with its TID,
    # so spawn_all(from_hub=True) must adopt it instead of duplicating.
    bd.spawn_calls.clear()
    restored = manager.spawn_all(from_hub=True)
    assert bd.spawn_calls == [], "hub restore spawned a duplicate NPC"
    assert len(restored) == 1
    assert restored[0] is bd.actors_by_tid[5000]


def test_spawn_without_tid_base_gets_persistent_free_tid():
    npcs = importlib.import_module("bd_npcs.npcs")
    manager = npcs.NPCManager()
    manager.register(npcs.NPCDefinition("drifter", "Drifter", "DoomImp"))

    spawned = manager.spawn_all()
    assert len(spawned) == 1
    handle = spawned[0]
    assert handle.tid > 0, "tid_base-less NPC got no nonzero TID"
    assert manager._tids["drifter"] == handle.tid, \
        "auto TID was not persisted for rebind"
    assert bd.actors_by_tid[handle.tid] is handle

    # A later savegame/hub restore adopts the same actor via that TID.
    again = manager.spawn_all(from_savegame=True)
    assert len(again) == 1
    assert again[0] is handle, "restore did not adopt the auto-TID actor"


def test_dead_npc_restore_does_not_respawn_a_duplicate():
    npcs = importlib.import_module("bd_npcs.npcs")
    manager = npcs.NPCManager()
    manager.register(npcs.NPCDefinition(
        "fallen", "Fallen Guard", "ZombieMan", tid_base=5100))

    live = manager.spawn_all()[0]
    live.alive = False  # a restored corpse remains addressable by its TID
    bd.spawn_calls.clear()

    restored = manager.spawn_all(from_savegame=True)
    assert restored == [], "dead restored NPC was treated as live"
    assert bd.spawn_calls == [], "dead restored NPC spawned a duplicate"
    assert manager.dead == ("fallen",), "dead NPC was not remembered"

    manager._save_tids()
    assert bd.state[manager.dispositions.state_key]["dead"] == ["fallen"], \
        "dead NPC state was not serialized"


def test_horror_hub_map_load_keeps_light_state():
    atmosphere = importlib.import_module("bd_horror.atmosphere")
    sector = Sector(index=3, light=144)
    bd.sector_map[5] = [sector]

    manager = atmosphere.LightManager()
    program = manager.candle([5])
    assert program.originals == {3: 144}, \
        "fresh arm did not sample the original light"

    # Hub snapshot reopen: the map comes back holding the program's
    # dimmed value; a restore-style map_load must keep the captured
    # originals and in-memory state instead of re-sampling the base.
    program.state["value"] = 120.0
    sector.light = 96
    bd.tasks.clear()
    dispatch("map_load", {"from_hub": True, "from_savegame": False})
    assert program.originals == {3: 144}, \
        "hub map_load re-sampled the original lights"
    assert program.state["base"] == 144 and program.state["value"] == 120.0
    manager.clear()
    bd.tasks.clear()


def main():
    tests = (
        test_status_handle_dedupe,
        test_elements_handle_dedupe,
        test_vtm_restore_survives_map_unload,
        test_shop_delivery_and_restock,
        test_dialogue_map_unload_ends_session,
        test_begin_talk_recovers_from_stale_session,
        test_spawn_all_from_hub_adopts_restored_actor,
        test_spawn_without_tid_base_gets_persistent_free_tid,
        test_dead_npc_restore_does_not_respawn_a_duplicate,
        test_horror_hub_map_load_keeps_light_state,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS: {len(tests)} framework hotfix regression checks")


if __name__ == "__main__":
    main()
