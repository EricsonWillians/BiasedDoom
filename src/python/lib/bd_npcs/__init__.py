"""NPC hub layer for BiasedDoom Python mods.

``bd_npcs`` is an engine-shipped, dependency-free package (it only
needs ``biaseddoom``, and optionally composes with ``bd_dialogue`` and
``bd_dnd``) that gives mods registered NPCs with persistent
dispositions, nearest-NPC talk targeting, services, and a shop:

- **Registered NPCs.** :class:`NPCDefinition` describes one NPC (actor
  class, spawn placement relative to the player or absolute, packed
  tint, dialogue source, services, stable TIDs); :class:`NPCManager`
  registers definitions (a duplicate id warns and replaces), spawns
  them on map load (friendly, still, speed zeroed, optionally tinted
  and facing the player), and adopts the savegame-restored actors by
  TID instead of duplicating them.
- **Disposition.** :class:`Disposition` keeps one value per NPC in
  [-100, 100] with named standings (``hostile``/``cold``/``neutral``/
  ``warm``/``trusted``); a definition's ``start_disposition`` seeds only
  NPCs the player has never met, so a loaded save never loses a
  standing.
- **Talk targeting.** :meth:`NPCManager.nearest` picks the live NPC
  closest to the pawn within talk range; :meth:`NPCManager.prompt`
  renders the "[E] Speak to ..." line (format restylable through
  ``bd_npcs.npcs.PROMPT_FORMAT``); :meth:`NPCManager.begin_talk`
  starts a ``bd_dialogue.DialogueSession`` whose ctx adds ``disposition``
  (int), ``standing`` (str), ``npc_id``, and ``dispositions`` (the
  store) on top of the standard dialogue keys. One conversation at a
  time: ``begin_talk`` returns None while another session is active.
- **Services.** :class:`Service` is the offer contract
  (``available(npc_id, ctx)`` / ``run(npc_id, ctx) -> {"ok", "message"}``
  with a documented ctx dict); :class:`HealerService` heals the pawn
  (and a bd_dnd character's RPG hp) for a currency fee behind a
  standing floor, :class:`TrainerService` rolls bd_dnd use-based
  mastery advancement checks.
- **Shop.** :class:`Shop` is a currency-based store (stock counts,
  restock timers, buy/sell math with refund-on-failure); only the
  counts persist (:meth:`Shop.stock_snapshot`/:meth:`Shop.restore`).
  :class:`ShopUI` renders it as a guarded Dear ImGui window from an
  ``imgui_frame`` handler, like ``bd_dnd.sheet.CharacterSheet``.

Minimal usage::

    import biaseddoom as bd
    import bd_npcs

    manager = bd_npcs.NPCManager()

    manager.register(bd_npcs.NPCDefinition(
        "elder", "Village Elder", "ZombieMan",
        dialogue=lambda: build_elder_dialogue(),
        services=(bd_npcs.HealerService(cost=5),),
        spawn_offset=(64.0, 0.0),
        tid_base=9500))

    @bd.on("map_load")
    def begin(event):
        manager.dispositions.arm_persistence()
        manager.arm_persistence()
        manager.spawn_all(from_savegame=event.get("from_savegame", False))

    def try_talk():
        pawn = bd.player(0).actor
        prompt = manager.prompt(pawn)       # "[E] Speak to ..." or None
        if prompt is not None:
            session = manager.begin_talk(pawn, character=hero)

Persistence contract
---------------------

Three things ride in ``bd.state["bd_npcs"]`` (or the custom state key
of the manager's disposition store): the disposition values under
``"disposition"``, the npc_id -> TID map under ``"tids"``, and the
shop stock counts wherever the mod stashes :meth:`Shop.stock_snapshot`.
Definitions, dialogue trees, services, and actor handles are never
persisted; re-register and re-arm at import or ``engine_start`` time.
Both ``arm_persistence`` methods register their engine handlers
exactly once, mirroring ``bd_rpg.RpgState``.

Coexistence
-----------

Like ``bd_dialogue``, this is a pure Python/ImGui layer; it does not
touch the engine's native Strife conversation system (see
``examples/python/30_conversation_quests``), so both can run side by
side on the same map.
"""

from __future__ import annotations

from typing import Any

__all__ = [
    "NPCDefinition", "NPCManager", "Disposition", "STANDINGS",
    "Service", "HealerService", "TrainerService", "Shop", "ShopUI",
    "__version__",
]

__version__ = "1.0.0"


def __getattr__(name: str) -> Any:
    # Lazy re-exports so `bd_npcs.NPCManager` (and friends) work without
    # paying for a submodule until it is actually used (mirrors bd_quests,
    # bd_dnd, and bd_rpg). importlib.import_module sidesteps the hasattr
    # probe `from . import x` performs, which would recurse back into
    # __getattr__.
    import importlib
    if name in ("NPCDefinition", "NPCManager"):
        return getattr(importlib.import_module(".npcs", __name__), name)
    if name in ("Disposition", "STANDINGS"):
        return getattr(importlib.import_module(".disposition", __name__),
                       name)
    if name in ("Service", "HealerService", "TrainerService", "Shop",
                "ShopUI"):
        return getattr(importlib.import_module(".services", __name__), name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
