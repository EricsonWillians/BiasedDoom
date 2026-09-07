"""Persistent per-NPC disposition standings for :mod:`bd_npcs`.

A :class:`Disposition` store keeps one integer value per NPC id, clamped
to [-100, 100], and maps every value to a named standing. Values shift
through gameplay (quest outcomes, dialogue effects, services rendered)
and ride along in savegames via ``bd.state``.

Thresholds contract
-------------------

:data:`DEFAULT_THRESHOLDS` is a tuple of ``(lower_bound, standing)``
pairs. A value qualifies for every bound it meets or exceeds, and the
LAST matching entry wins:

- value < -50 is ``hostile``
- value < -10 is ``cold``
- value < 40 is ``neutral``
- value < 75 is ``warm``
- 75 or more is ``trusted``

So ``-50`` reads as ``cold``, ``-10`` as ``neutral``, ``40`` as ``warm``,
and ``75`` as ``trusted`` (the lower bound is inclusive). Unknown NPCs
read as ``0`` (neutral). Values are clamped to [-100, 100] on every
write, so the ``-100`` hostile bound and the ``75`` trusted bound are
both reachable through the public API.

Persistence
-----------

:meth:`Disposition.arm_persistence` registers the engine ``save``/``load``
handlers exactly once (mirroring ``bd_rpg.RpgState``); the snapshot lives
under ``bd.state[state_key]["disposition"]``. Dispositions survive
savegames: a loaded game restores every NPC's standing exactly, and
managers must never overwrite a restored value with a definition's
``start_disposition`` (see :mod:`bd_npcs.npcs`).
"""

from __future__ import annotations

from typing import Any, Dict

import biaseddoom as bd

__all__ = ["STANDINGS", "DEFAULT_THRESHOLDS", "Disposition", "STATE_KEY"]

#: Key under which bd_npcs persists its state in ``bd.state``.
STATE_KEY = "bd_npcs"

#: Named standings, coldest to warmest.
STANDINGS = ("hostile", "cold", "neutral", "warm", "trusted")

#: (lower bound, standing) pairs; the LAST entry whose bound a value
#: meets or exceeds wins. See the module docstring for the contract.
DEFAULT_THRESHOLDS = ((-100, "hostile"), (-50, "cold"), (-10, "neutral"),
                      (40, "warm"), (75, "trusted"))

#: Value range every write is clamped to.
MIN_VALUE = -100
MAX_VALUE = 100


def _standing_of(value: int) -> str:
    """Map a disposition value to its standing name."""
    result = STANDINGS[0]
    for bound, standing in DEFAULT_THRESHOLDS:
        if value >= bound:
            result = standing
    return result


class Disposition:
    """Per-NPC disposition values with named standings.

    Values are plain ints in [-100, 100] keyed by NPC id; :meth:`get`
    returns 0 (neutral baseline) for unknown ids, :meth:`set` clamps,
    and :meth:`shift` adds a delta and returns the new value.
    :meth:`standing` names the current tier through
    :data:`DEFAULT_THRESHOLDS`.

    :meth:`arm_persistence` registers the ``save``/``load`` handlers
    exactly once per store, round-tripping :meth:`snapshot` through
    ``bd.state[state_key]["disposition"]`` (merging into the existing
    bucket so a manager can share the same state key). Dispositions
    survive savegames; see the module docstring for the thresholds
    contract.
    """

    def __init__(self, state_key: str = STATE_KEY) -> None:
        self.state_key: str = str(state_key)
        self._values: Dict[str, int] = {}
        self._persistence_armed: bool = False

    # -- values ----------------------------------------------------------------

    def get(self, npc_id: str) -> int:
        """The disposition value for ``npc_id`` (0 when unknown)."""
        try:
            return int(self._values.get(str(npc_id), 0))
        except (TypeError, ValueError):
            return 0

    def has(self, npc_id: str) -> bool:
        """True when the store carries an explicit value for ``npc_id``.

        Managers use this to seed a definition's ``start_disposition``
        only for NPCs the player has never met, so a loaded save never
        has a saved standing overwritten.
        """
        return str(npc_id) in self._values

    def set(self, npc_id: str, value: int) -> int:
        """Set the value clamped to [-100, 100]; returns the stored value."""
        try:
            value = int(value)
        except (TypeError, ValueError):
            raise ValueError(f"bd_npcs: disposition value must be an int, "
                             f"got {value!r}")
        value = max(MIN_VALUE, min(MAX_VALUE, value))
        self._values[str(npc_id)] = value
        return value

    def shift(self, npc_id: str, delta: int) -> int:
        """Add ``delta`` to the current value; returns the new value."""
        try:
            delta = int(delta)
        except (TypeError, ValueError):
            raise ValueError(f"bd_npcs: disposition delta must be an int, "
                             f"got {delta!r}")
        return self.set(npc_id, self.get(npc_id) + delta)

    def standing(self, npc_id: str) -> str:
        """The standing name for ``npc_id`` (see the thresholds contract)."""
        return _standing_of(self.get(npc_id))

    # -- persistence -------------------------------------------------------------

    def snapshot(self) -> Dict[str, Any]:
        """Plain JSON-able snapshot (a version tag plus the values)."""
        return {"version": 1,
                "values": {key: int(value)
                           for key, value in self._values.items()}}

    def restore(self, data: Any) -> None:
        """Tolerant restore of a :meth:`snapshot` payload.

        Accepts the snapshot dict (reading its ``values`` mapping) or a
        raw ``{npc_id: value}`` mapping; anything malformed is skipped
        and every value is re-clamped to [-100, 100].
        """
        if not isinstance(data, dict):
            return
        values = data.get("values", data)
        if not isinstance(values, dict):
            return
        for key, value in values.items():
            try:
                value = int(value)
            except (TypeError, ValueError):
                continue
            self._values[str(key)] = max(MIN_VALUE, min(MAX_VALUE, value))

    def save(self) -> None:
        """Write the snapshot into ``bd.state`` (call from a save event)."""
        bucket = bd.state.get(self.state_key)
        if not isinstance(bucket, dict):
            bucket = {}
            bd.state[self.state_key] = bucket
        bucket["disposition"] = self.snapshot()

    def load(self) -> None:
        """Restore from ``bd.state`` (call from a load event)."""
        bucket = bd.state.get(self.state_key)
        data = bucket.get("disposition") if isinstance(bucket, dict) else None
        self.restore(data)

    def arm_persistence(self) -> None:
        """Register the save/load handlers exactly once per store."""
        if self._persistence_armed:
            return
        self._persistence_armed = True
        store = self

        @bd.on("save")
        def _on_save(event: Dict[str, Any]) -> None:
            try:
                store.save()
            except Exception as exc:
                bd.warn(f"bd_npcs: could not serialize dispositions: "
                        f"{exc!r}")

        @bd.on("load")
        def _on_load(event: Dict[str, Any]) -> None:
            try:
                store.load()
            except Exception as exc:
                bd.warn(f"bd_npcs: could not restore dispositions: "
                        f"{exc!r}")

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return (f"<Disposition key={self.state_key!r} "
                f"npcs={len(self._values)}>")
