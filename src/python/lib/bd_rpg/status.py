"""Timed status-effect engine for ``bd_rpg``.

Status state lives in ``bd.actor_data`` under ``ACTOR_KEY``/``"status"``;
the module singleton :data:`status` drives every effect from ONE
consolidated repeating ``bd.schedule`` task. Never schedule per-effect
tasks: the task queue is bounded (``py_max_tasks``) and every task shares
the whole-tic Python budget — see the engine's
``docs/development/python-performance.md`` ("Throttle hot callbacks").
"""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional

import biaseddoom as bd

from .elements import ACTOR_KEY

#: Stacking rules accepted by :meth:`StatusEngine.apply`.
RULES = ("refresh", "stack", "independent")


# --- built-in effects -------------------------------------------------------------
#
# Each built-in is a behavior table: an optional tick interval plus apply /
# tick callables. on_apply callables record (attribute, original) pairs in
# inst["restores"] so expiry/dispel can undo their mutations. Restores use
# capture-on-apply semantics (identical to bd_vtm's disciplines): re-applied
# effects end their previous instance first, so the captured original is
# always the unmodified value.


def _restore_attributes(ref: Any, inst: Dict[str, Any]) -> None:
    """Undo an instance's recorded attribute mutations (best effort)."""
    restores = inst.get("restores") or []
    if not restores:
        return
    try:
        if ref is None or not ref.valid:
            return
        for attr, value in restores:
            try:
                setattr(ref, attr, value)
            except Exception as exc:
                bd.warn(f"bd_rpg: could not restore {attr} on an actor: "
                        f"{exc!r}")
    except Exception:
        pass  # stale handle: nothing to restore


def _burning_tick(ref: Any, inst: Dict[str, Any]) -> None:
    if ref.alive:
        ref.damage(2 * max(1, int(inst.get("stacks", 1))),
                   damage_type="Fire")


def _poisoned_tick(ref: Any, inst: Dict[str, Any]) -> None:
    if ref.alive:
        ref.damage(max(1, int(inst.get("stacks", 1))),
                   damage_type="Poison")


def _regenerating_tick(ref: Any, inst: Dict[str, Any]) -> None:
    if ref.alive:
        ref.heal(max(1, int(inst.get("stacks", 1))))


def _slowed_apply(ref: Any, inst: Dict[str, Any]) -> None:
    original = ref.speed
    inst.setdefault("restores", []).append(("speed", original))
    ref.speed = original * 0.5


def _stunned_apply(ref: Any, inst: Dict[str, Any]) -> None:
    original = ref.damage_multiply
    inst.setdefault("restores", []).append(("damage_multiply", original))
    ref.damage_multiply = 0.0
    try:
        ref.set_velocity(0.0, 0.0, 0.0)
    except Exception:
        pass


def _stunned_tick(ref: Any, inst: Dict[str, Any]) -> None:
    try:
        ref.set_velocity(0.0, 0.0, 0.0)
    except Exception:
        pass


_BUILTINS: Dict[str, Dict[str, Any]] = {
    # burning: fire DoT, 2 damage per stack every 35 tics.
    "burning": {"interval": 35, "tick": _burning_tick},
    # poisoned: 1 damage per stack every 25 tics.
    "poisoned": {"interval": 25, "tick": _poisoned_tick},
    # slowed: speed halved while active, restored on expiry/dispel.
    "slowed": {"interval": None, "on_apply": _slowed_apply},
    # stunned: damage output zeroed (restored) and velocity pinned to 0.
    "stunned": {"interval": 1, "on_apply": _stunned_apply,
                "tick": _stunned_tick},
    # regenerating: heal 1 per stack every 35 tics.
    "regenerating": {"interval": 35, "tick": _regenerating_tick},
}


class StatusEngine:
    """Manager for timed, stacking status effects on actors.

    Effects are stored per actor in ``bd.actor_data`` (auto-purged on
    actor destruction and map change, never saved in savegames) and are
    driven by a single consolidated repeating task armed lazily on the
    first :meth:`apply`. Tick callbacks receive ``(ref, effect_record)``;
    invalid/stale refs are purged silently on each engine tick.

    Stacking rules on re-application of an active effect:

    - ``"refresh"`` (default): the old instance is ended (its restores
      run) and a fresh instance replaces it — the timer resets.
    - ``"stack"``: the existing instance gains ``stacks`` and its timer
      extends to at least the new duration.
    - ``"independent"``: every application is its own instance with its
      own timer; queries aggregate across instances.

    Built-in effect names: ``burning``, ``poisoned``, ``slowed``,
    ``stunned``, ``regenerating`` (see this module's docstring tables).
    Passing an explicit ``tick``/``on_expire`` overrides the built-in
    behavior for that application; custom names are plain timed effects.
    """

    def __init__(self) -> None:
        self._tracked: List[Any] = []
        self._task: Optional[int] = None
        self._rearm_wired: bool = False

    # -- application ---------------------------------------------------------

    def apply(self, ref: Any, name: str, duration_tics: int, stacks: int = 1,
              rule: str = "refresh",
              tick: Optional[Callable[[Any, Dict[str, Any]], None]] = None,
              on_expire: Optional[Callable[[Any, Dict[str, Any]], None]] = None,
              data: Optional[Dict[str, Any]] = None,
              interval: Optional[int] = None) -> Dict[str, Any]:
        """Apply a status effect to an actor; returns the instance record.

        The returned record is the live dict stored in the actor's data —
        ``remaining`` counts down in tics, ``duration`` remembers the
        initial length (for UI progress), ``stacks`` is the stack count.

        ``interval`` is the tick period in tics: the built-in default for
        built-in names, otherwise every tic (1) when a ``tick`` callable
        is given, or None (never ticks) for pure timed markers.
        """
        name = str(name).strip().lower()
        if not name:
            raise ValueError("status name must not be empty")
        duration = int(duration_tics)
        if duration < 1:
            raise ValueError("status duration must be >= 1 tic")
        stacks = max(1, int(stacks))
        rule = str(rule)
        if rule not in RULES:
            raise ValueError(f"rule must be one of {RULES}")
        builtin = _BUILTINS.get(name) or {}
        if interval is None:
            interval = builtin.get("interval")
            if interval is None and tick is not None:
                interval = 1
        elif interval is not None:
            interval = max(1, int(interval))
        inst: Dict[str, Any] = {
            "name": name,
            "remaining": duration,
            "duration": duration,
            "stacks": stacks,
            "interval": interval,
            "tick_in": interval,
            "tick": tick if tick is not None else builtin.get("tick"),
            "on_expire": on_expire,
            "data": dict(data or {}),
            "restores": [],
        }
        recs = self._status_map(ref, create=True)
        instances = recs.setdefault(name, [])
        if rule == "refresh" and instances:
            for old in list(instances):
                self._end_instance(ref, old, expired=False)
                instances.remove(old)
        elif rule == "stack" and instances:
            current = instances[0]
            current["stacks"] = int(current.get("stacks", 1)) + stacks
            current["remaining"] = max(int(current.get("remaining", 0)),
                                       duration)
            current["duration"] = max(int(current.get("duration", 0)),
                                      duration)
            self._track(ref)
            self._arm()
            return current
        self._begin_instance(ref, inst, builtin)
        instances.append(inst)
        self._track(ref)
        self._arm()
        return inst

    # -- queries ---------------------------------------------------------------

    def has(self, ref: Any, name: str) -> bool:
        """True while the actor carries at least one instance of ``name``."""
        return self.instances(ref, name) > 0

    def stacks(self, ref: Any, name: str) -> int:
        """Total stacks of ``name`` across all of the actor's instances."""
        return sum(max(1, int(inst.get("stacks", 1)))
                   for inst in self._instances(ref, name))

    def remaining(self, ref: Any, name: str) -> int:
        """Longest remaining duration (tics) of ``name`` (0 when absent)."""
        remaining = [int(inst.get("remaining", 0))
                     for inst in self._instances(ref, name)]
        return max(remaining) if remaining else 0

    def instances(self, ref: Any, name: str) -> int:
        """Number of live instances of ``name`` on the actor."""
        return len(self._instances(ref, name))

    def list(self, ref: Any) -> Dict[str, Dict[str, int]]:
        """All live effects on the actor: name -> {remaining, duration,
        stacks, instances} (aggregated across instances)."""
        summary: Dict[str, Dict[str, int]] = {}
        for name, instances in self._all_instances(ref).items():
            if not instances:
                continue
            summary[name] = {
                "remaining": max(int(i.get("remaining", 0)) for i in instances),
                "duration": max(int(i.get("duration", 0)) for i in instances),
                "stacks": sum(max(1, int(i.get("stacks", 1))) for i in instances),
                "instances": len(instances),
            }
        return summary

    # -- removal ---------------------------------------------------------------

    def dispel(self, ref: Any, name: str) -> bool:
        """Remove every instance of ``name`` (restores run, no on_expire)."""
        name = str(name).strip().lower()
        recs = self._status_map(ref, create=False)
        instances = recs.get(name) or []
        if not instances:
            return False
        for inst in list(instances):
            self._end_instance(ref, inst, expired=False)
        recs.pop(name, None)
        self._maybe_untrack(ref, recs)
        return True

    def dispel_all(self, ref: Any) -> int:
        """Remove every status from the actor; returns how many ended."""
        recs = self._status_map(ref, create=False)
        ended = 0
        for name in list(recs.keys()):
            for inst in list(recs.get(name) or []):
                self._end_instance(ref, inst, expired=False)
                ended += 1
            recs.pop(name, None)
        self._maybe_untrack(ref, recs)
        return ended

    # -- persistence support (used by bd_rpg.RpgState) ---------------------------

    def tracked_refs(self) -> List[Any]:
        """Live handles currently carrying status state (may be stale)."""
        return list(self._tracked)

    def snapshot(self, ref: Any) -> List[Dict[str, Any]]:
        """JSON-safe timer snapshot of one actor's live effects.

        Returns ``[{"name", "remaining", "stacks"}, ...]`` — one entry per
        instance. Only timers are captured; custom tick/on_expire callables
        are mod-side code and never serialize (re-attach them through
        ``RpgState(status_definitions=...)`` on load).
        """
        out: List[Dict[str, Any]] = []
        for name, instances in self._all_instances(ref).items():
            for inst in instances:
                try:
                    out.append({
                        "name": str(name),
                        "remaining": max(1, int(inst.get("remaining", 1))),
                        "stacks": max(1, int(inst.get("stacks", 1))),
                    })
                except (TypeError, ValueError):
                    continue
        return out

    # -- the consolidated pump -------------------------------------------------

    def _arm(self) -> None:
        """Arm the single consolidated repeating task (exactly once)."""
        self._wire_rearm()
        if self._task is not None:
            return
        try:
            self._task = bd.schedule(self._pump, delay=1, repeat=1,
                                     map_local=False)
        except Exception as exc:
            bd.warn(f"bd_rpg: could not schedule the status pump: {exc!r}")
            self._task = None

    def _wire_rearm(self) -> None:
        """Re-arm the pump on every map_load (registered exactly once).

        Whether scheduled tasks survive a checkpoint load is an engine
        detail the pack does not rely on: cancelling a possibly-stale task
        id and rescheduling is idempotent either way (mirrors how bd_vtm
        re-arms its upkeep tasks from map_load).
        """
        if self._rearm_wired:
            return
        self._rearm_wired = True
        engine = self

        @bd.on("map_load")
        def _on_map_load(event: Dict[str, Any]) -> None:
            if engine._task is not None:
                try:
                    bd.cancel_task(engine._task)
                except Exception:
                    pass
                engine._task = None
            if engine._tracked:
                engine._arm()

    def _pump(self) -> None:
        if not self._tracked:
            return
        for ref in list(self._tracked):
            try:
                if not ref.valid:
                    raise ReferenceError("stale actor handle")
                recs = self._status_map(ref, create=False)
            except Exception:
                # Purge silently: destroyed actors and map changes drop
                # their actor_data, which is exactly the intended cleanup.
                self._untrack(ref)
                continue
            if not recs:
                self._untrack(ref)
                continue
            for name in list(recs.keys()):
                instances = recs.get(name) or []
                for inst in list(instances):
                    if inst not in instances:
                        continue
                    inst["remaining"] = int(inst.get("remaining", 0)) - 1
                    interval = inst.get("interval")
                    if interval and inst["remaining"] > 0:
                        inst["tick_in"] = int(inst.get("tick_in",
                                                       interval)) - 1
                        if inst["tick_in"] <= 0:
                            inst["tick_in"] = int(interval)
                            self._fire_tick(ref, inst)
                    if inst["remaining"] <= 0:
                        self._end_instance(ref, inst, expired=True)
                        try:
                            instances.remove(inst)
                        except ValueError:
                            pass
                if not instances:
                    recs.pop(name, None)
            self._maybe_untrack(ref, recs)

    # -- internals ----------------------------------------------------------------

    def _begin_instance(self, ref: Any, inst: Dict[str, Any],
                        builtin: Dict[str, Any]) -> None:
        on_apply = builtin.get("on_apply")
        if on_apply is not None:
            try:
                on_apply(ref, inst)
            except Exception as exc:
                bd.warn(f"bd_rpg: on_apply for status {inst['name']!r} "
                        f"failed: {exc!r}")

    def _end_instance(self, ref: Any, inst: Dict[str, Any],
                      expired: bool) -> None:
        _restore_attributes(ref, inst)
        if expired:
            callback = inst.get("on_expire")
            if callback is not None:
                try:
                    callback(ref, inst)
                except Exception as exc:
                    bd.warn(f"bd_rpg: on_expire for status "
                            f"{inst.get('name')!r} raised: {exc!r}")

    def _fire_tick(self, ref: Any, inst: Dict[str, Any]) -> None:
        callback = inst.get("tick")
        if callback is None:
            return
        try:
            callback(ref, inst)
        except Exception as exc:
            bd.warn(f"bd_rpg: tick for status {inst.get('name')!r} "
                    f"raised: {exc!r}")

    def _status_map(self, ref: Any, create: bool) -> Dict[str, List]:
        data = bd.actor_data(ref)
        pack = data.get(ACTOR_KEY)
        if pack is None:
            if not create:
                return {}
            pack = {}
            data[ACTOR_KEY] = pack
        recs = pack.get("status")
        if recs is None:
            if not create:
                return {}
            recs = {}
            pack["status"] = recs
        return recs

    def _instances(self, ref: Any, name: str) -> List[Dict[str, Any]]:
        try:
            recs = self._status_map(ref, create=False)
        except Exception:
            return []
        return [inst for inst in (recs.get(str(name).strip().lower()) or [])
                if int(inst.get("remaining", 0)) > 0]

    def _all_instances(self, ref: Any) -> Dict[str, List[Dict[str, Any]]]:
        try:
            recs = self._status_map(ref, create=False)
        except Exception:
            return {}
        return {str(name): [inst for inst in (instances or [])
                            if int(inst.get("remaining", 0)) > 0]
                for name, instances in recs.items()}

    def _track(self, ref: Any) -> None:
        # Actor handles compare by actor identity (slot + generation), not
        # object identity: ``bd.actor_refs`` hands out fresh handle objects
        # per call, so an ``is`` check would leak duplicate entries for the
        # same actor. ``==`` never touches the engine, so stale handles are
        # safe to compare (``_untrack``'s ``list.remove`` already uses it).
        for existing in self._tracked:
            if existing == ref:
                return
        self._tracked.append(ref)

    def _untrack(self, ref: Any) -> None:
        try:
            self._tracked.remove(ref)
        except ValueError:
            pass

    def _maybe_untrack(self, ref: Any, recs: Dict[str, Any]) -> None:
        if not recs:
            self._untrack(ref)


#: Shared status engine. Most mods should use this singleton.
status = StatusEngine()
