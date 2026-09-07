"""Elemental combat RPG framework for BiasedDoom Python mods.

``bd_rpg`` is an engine-shipped, dependency-free package (it only needs
``biaseddoom``) that gives mods a classic action-RPG combat layer:

- **Damage types.** :class:`DamageTypes` is a registry of named damage
  types with UI colors and sounds; ``physical``, ``fire``, ``ice``,
  ``poison``, ``acid``, ``shock``, ``holy``, and ``dark`` come
  pre-registered on the shared :data:`damage_types` registry.
- **Affinities.** :func:`set_affinity` / :func:`affinity_of` keep
  per-actor damage-type multipliers in ``bd.actor_data`` (1.0 neutral,
  0.0 immune, 0.5 resist, 2.0 weak); :func:`set_class_affinity` defines
  class-level defaults evaluated lazily by exact class-name match, with
  per-actor entries overriding them.
- **Attack resolution.** :func:`resolve_attack` runs the full pipeline —
  optional d20-style hit check, crit roll, ``NdM+K`` raw roll, affinity
  multiplier, flat soak (``min(soak, dmg - 1)`` so at least 1 gets
  through unless immune) — and applies the result through the native
  damage path, where the ``actor_before_damage`` mutable filter has the
  last word. Every roll uses the engine's deterministic script RNG.
- **Status effects.** The :class:`StatusEngine` singleton :data:`status`
  applies timed, stacking effects (``refresh``/``stack``/``independent``
  rules) driven by ONE consolidated repeating task, with built-ins
  ``burning`` (fire DoT), ``poisoned``, ``slowed``, ``stunned``, and
  ``regenerating``.
- **Loot.** :class:`LootTable` weighted drops, :class:`LootRules` wiring
  them to ``actor_died`` with exact player attribution and rarity
  flash/sound feedback, plus the genre-neutral kill-XP glue
  :func:`award_kill_xp` / :func:`track_kill_xp`.
- **Persistence.** :class:`RpgState` mirrors ``bd_vtm``'s pattern:
  ``save()``/``load()`` round-trip the player pawn's affinities and the
  status timers of TID-tagged tracked actors through
  ``bd.state["bd_rpg"]`` on the engine's ``save``/``load`` events.

Minimal usage::

    import biaseddoom as bd
    import bd_rpg

    state = bd_rpg.RpgState()

    @bd.on("engine_start")
    def setup(event):
        bd_rpg.set_class_affinity("DoomImp", "fire", 2.0)   # imps burn
        bd_rpg.set_class_affinity("Demon", "slime", 0.0)    # pinkies immune
        state.arm_persistence()

    @bd.on("map_load")
    def begin(event):
        imp = bd.spawn("DoomImp", 512.0, 512.0, 0.0)
        result = bd_rpg.resolve_attack(bd.player(0).actor, imp,
                                       {"damage": "2d6+3", "type": "fire",
                                        "crit_chance": 0.1})
        bd.log(f"hit for {result['final']}"
               f"{' CRIT' if result['critical'] else ''}")

Layering with the native damage filter
--------------------------------------

``resolve_attack`` *computes* damage; it never bypasses the engine. The
final amount is applied via ``Actor.damage``, so a mod's
``actor_before_damage`` handler can still rewrite or cancel the resolved
hit (that is how the bundled example implements its "stoneskin" shrine:
a status flag plus ``event["cancel"] = True``). Conversely, damage the
resolver never applies — misses, immunities — produces no
``actor_damaged`` at all.

The actor_data handle rule
--------------------------

``bd.actor_data`` is keyed by actor slot and is purged not only on actor
destruction/map change but also when the **last live Python handle** to
the actor is garbage-collected. Every bd_rpg write path
(:func:`set_affinity`, :func:`set_soak`, :meth:`StatusEngine.apply`)
retains a handle for you. If your own mod code writes into
``bd.actor_data`` directly (like the example's elite affix markers), pin
the handle in a module-level list or the payload silently vanishes with
the handle.

Persistence contract
--------------------

``bd.actor_data`` is **not** saved in savegames, so per-actor affinities,
soak, and live status instances vanish on a checkpoint load.
:class:`RpgState` covers the two cases mods actually need:

- the **player pawn's** affinity map and soak (re-applied to the new
  pawn on the ``map_load`` after a load);
- **status timers** of tracked actors that carry a nonzero TID —
  serialized as ``(tid, name, remaining, stacks)`` and re-applied by TID
  after the load (the classic TID-rebind recipe from the main Python
  manual). Re-application uses the effect *name*, so built-ins re-attach
  their behavior automatically; for custom effects pass
  ``RpgState(status_definitions={name: {"tick": fn, "on_expire": fn}})``.
  TID-less actors (including the player pawn) cannot be re-resolved —
  re-apply their statuses from your own ``map_load`` handler.

Definitions are **never** persisted: damage-type registrations, class
affinities, loot tables, and XP tables are script-side constants and are
expected to be re-created at import / ``engine_start`` time, exactly
like quest definitions in ``bd_quests``.
"""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional

import biaseddoom as bd

__all__ = [
    "DamageTypes", "damage_types",
    "set_affinity", "affinity_of", "set_class_affinity", "class_affinities",
    "set_soak", "soak_of", "resolve_attack",
    "StatusEngine", "status",
    "LootTable", "LootRules", "RARITIES",
    "award_kill_xp", "kill_xp", "track_kill_xp",
    "RpgState", "STATE_KEY", "ACTOR_KEY", "__version__",
]

__version__ = "1.0.0"

#: Key under which RpgState persists itself in ``bd.state``.
STATE_KEY = "bd_rpg"

#: Reserved ``bd.actor_data`` key for the pack's per-actor payloads.
ACTOR_KEY = "bd_rpg"


class RpgState:
    """Persistent container for bd_rpg runtime state, via ``bd.state``.

    Mirrors ``bd_vtm.VtMState``: :meth:`save`/:meth:`load` round-trip
    through ``bd.state["bd_rpg"]`` and :meth:`arm_persistence` registers
    the engine ``save``/``load``/``map_load`` handlers exactly once. What
    is persisted (and what is not) is documented in the package
    docstring — in short: player affinities/soak, and status timers of
    TID-tagged tracked actors. ``status_definitions`` maps custom status
    names to ``{"tick": fn, "on_expire": fn}`` so restored timers
    re-attach their callbacks; built-ins need no entry.
    """

    def __init__(self, state_key: str = STATE_KEY,
                 status_definitions: Optional[Dict[str, Dict[str, Any]]] = None,
                 status_engine: Any = None) -> None:
        self.state_key: str = str(state_key)
        self.status_definitions: Dict[str, Dict[str, Any]] = dict(
            status_definitions or {})
        self._engine: Any = status_engine  # None -> module singleton
        self._persistence_armed: bool = False
        self._pending: Optional[Dict[str, Any]] = None

    # -- engine access -------------------------------------------------------

    def _status_engine(self) -> Any:
        if self._engine is not None:
            return self._engine
        from .status import status
        return status

    # -- persistence -----------------------------------------------------------

    def serialize(self) -> Dict[str, Any]:
        """Plain JSON-able snapshot (no handles, no callables)."""
        payload: Dict[str, Any] = {"version": 1}
        try:
            player = bd.player(0)
            pawn = player.actor if player is not None else None
            if pawn is not None and pawn.valid:
                pack = bd.actor_data(pawn).get(ACTOR_KEY) or {}
                aff = pack.get("aff")
                if isinstance(aff, dict) and aff:
                    payload["player_affinities"] = {
                        str(k): float(v) for k, v in aff.items()}
                soak = pack.get("soak")
                if soak:
                    payload["player_soak"] = int(soak)
        except Exception as exc:
            bd.warn(f"bd_rpg: could not snapshot player affinities: {exc!r}")
        engine = self._status_engine()
        timers: List[Dict[str, Any]] = []
        for ref in engine.tracked_refs():
            try:
                if not ref.valid or ref.tid == 0:
                    continue  # TID recipe: only re-resolvable actors persist
                for entry in engine.snapshot(ref):
                    timers.append({"tid": int(ref.tid), **entry})
            except Exception:
                continue
        if timers:
            payload["status_timers"] = timers
        return payload

    def restore(self, data: Any) -> None:
        """Stash a :meth:`serialize` snapshot; applied on the next map_load.

        Actor re-resolution needs a loaded world, which the ``load`` event
        does not guarantee — so the snapshot is deferred and applied by
        the ``map_load`` handler that :meth:`arm_persistence` registers.
        Tolerant of junk: anything malformed is simply skipped.
        """
        if isinstance(data, dict):
            self._pending = data

    def apply_pending(self) -> None:
        """Apply a stashed snapshot against the live world (map_load time)."""
        data, self._pending = self._pending, None
        if not isinstance(data, dict):
            return
        affinities = data.get("player_affinities")
        soak = data.get("player_soak")
        if isinstance(affinities, dict) or soak:
            try:
                from .elements import set_affinity, set_soak
                player = bd.player(0)
                pawn = player.actor if player is not None else None
            except Exception as exc:
                bd.warn(f"bd_rpg: could not reach the player pawn after "
                        f"load: {exc!r}")
                pawn = None
            if pawn is not None:
                if isinstance(affinities, dict):
                    for name, mult in affinities.items():
                        try:
                            set_affinity(pawn, name, float(mult))
                        except Exception as exc:
                            bd.warn(f"bd_rpg: could not restore affinity "
                                    f"{name!r}: {exc!r}")
                if soak:
                    try:
                        set_soak(pawn, int(soak))
                    except Exception as exc:
                        bd.warn(f"bd_rpg: could not restore player soak: "
                                f"{exc!r}")
        timers = data.get("status_timers")
        if not isinstance(timers, list):
            return
        engine = self._status_engine()
        for entry in timers:
            if not isinstance(entry, dict):
                continue
            try:
                tid = int(entry.get("tid", 0))
                name = str(entry.get("name", ""))
                remaining = max(1, int(entry.get("remaining", 1)))
                stacks = max(1, int(entry.get("stacks", 1)))
            except (TypeError, ValueError):
                continue
            if tid == 0 or not name:
                continue
            try:
                ref = bd.actor_ref(tid)
            except Exception:
                ref = None
            if ref is None:
                continue
            custom = self.status_definitions.get(name) or {}
            try:
                engine.apply(ref, name, duration_tics=remaining,
                             stacks=stacks, rule="independent",
                             tick=custom.get("tick"),
                             on_expire=custom.get("on_expire"))
            except Exception as exc:
                bd.warn(f"bd_rpg: could not restore status {name!r} on "
                        f"TID {tid}: {exc!r}")

    def save(self) -> None:
        """Write the snapshot into ``bd.state`` (call from a save event)."""
        bd.state[self.state_key] = self.serialize()

    def load(self) -> None:
        """Restore from ``bd.state`` (call from a load event)."""
        self.restore(bd.state.get(self.state_key))

    def arm_persistence(self) -> None:
        """Register the save/load/map_load handlers exactly once per state."""
        if self._persistence_armed:
            return
        self._persistence_armed = True
        state = self

        @bd.on("save")
        def _on_save(event: Dict[str, Any]) -> None:
            try:
                state.save()
            except Exception as exc:
                bd.warn(f"bd_rpg: could not serialize RPG state: {exc!r}")

        @bd.on("load")
        def _on_load(event: Dict[str, Any]) -> None:
            try:
                state.load()
            except Exception as exc:
                bd.warn(f"bd_rpg: could not restore RPG state: {exc!r}")

        @bd.on("map_load")
        def _on_map_load(event: Dict[str, Any]) -> None:
            try:
                state.apply_pending()
            except Exception as exc:
                bd.warn(f"bd_rpg: could not re-apply RPG state after "
                        f"load: {exc!r}")


def __getattr__(name: str) -> Any:
    # Lazy re-exports so `bd_rpg.resolve_attack` (and friends) work while
    # submodules are only imported when actually used (mirrors bd_quests
    # and bd_dnd). importlib.import_module sidesteps the hasattr probe
    # `from . import x` performs, which would recurse back into __getattr__.
    import importlib
    if name in ("DamageTypes", "damage_types", "set_affinity", "affinity_of",
                "set_class_affinity", "class_affinities", "set_soak",
                "soak_of", "resolve_attack"):
        return getattr(importlib.import_module(".elements", __name__), name)
    if name in ("StatusEngine", "status"):
        module = importlib.import_module(".status", __name__)
        value = getattr(module, name)
        if name == "status":
            # Importing the submodule sets the package attribute `status`
            # to the module object, shadowing the singleton on later
            # accesses; re-bind the attribute so `bd_rpg.status` always
            # means the StatusEngine singleton (the documented API).
            globals()["status"] = value
        return value
    if name in ("LootTable", "LootRules", "RARITIES", "award_kill_xp",
                "kill_xp", "track_kill_xp"):
        return getattr(importlib.import_module(".loot", __name__), name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
