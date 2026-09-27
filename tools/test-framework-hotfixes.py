#!/usr/bin/env python3
"""Offline regression checks for engine-shipped Python framework hotfixes.

These tests use a small stub for the engine's ``biaseddoom`` module so they
run without an IWAD or a display. They cover the actor-handle identity,
map-transition scheduling, and shop delivery semantics that depend on engine
API contracts.
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

    def __eq__(self, other):
        return (isinstance(other, ActorHandle) and
                (self.slot, self.generation) == (other.slot, other.generation))

    def __hash__(self):
        return hash((self.slot, self.generation))


class Actor:
    valid = True


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


def main():
    tests = (
        test_status_handle_dedupe,
        test_elements_handle_dedupe,
        test_vtm_restore_survives_map_unload,
        test_shop_delivery_and_restock,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS: {len(tests)} framework hotfix regression checks")


if __name__ == "__main__":
    main()
