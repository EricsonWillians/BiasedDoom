"""Weighted loot tables, death-drop rules, and kill-XP glue for ``bd_rpg``.

This submodule is imported lazily by ``bd_rpg``; import it directly only
when you want the names without the package prefix.
"""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional, Union

import biaseddoom as bd

from .elements import _roll


# --- rarity tiers ------------------------------------------------------------------

#: Rarity presentation tiers: a screen-flash color/alpha for the killer
#: (the console player) plus an optional UI sound lump. Plain dict — mods
#: may edit it. ``flash`` 0.0 skips the flash entirely.
RARITIES: Dict[str, Dict[str, Any]] = {
    "common": {"color": (210, 210, 210), "flash": 0.0, "sound": None},
    "uncommon": {"color": (96, 220, 112), "flash": 0.20,
                 "sound": "misc/p_pkup"},
    "rare": {"color": (96, 144, 255), "flash": 0.28,
             "sound": "misc/i_pkup"},
    "legendary": {"color": (255, 170, 40), "flash": 0.35,
                  "sound": "misc/secret"},
}


class LootTable:
    """A weighted list of spawnable drops.

    Entries are ``(class_name, count, weight, condition)``; ``condition``
    is an optional no-arg callable evaluated at roll time (a raising or
    False condition excludes the entry, with a ``bd.warn``). All randomness
    flows through the deterministic script RNG (``rng`` defaults to the
    ``bd`` module), so rolls resume exactly after a checkpoint load.
    """

    def __init__(self, name: str = "") -> None:
        self.name: str = str(name)
        self._entries: List[Dict[str, Any]] = []

    def add(self, class_name: str, count: int = 1, weight: int = 1,
            condition: Optional[Callable[[], bool]] = None) -> "LootTable":
        """Append a drop entry and return the table (for chaining)."""
        class_name = str(class_name).strip()
        if not class_name:
            raise ValueError("loot entry needs an actor class name")
        if int(weight) < 0:
            raise ValueError("loot weight must be >= 0")
        self._entries.append({
            "class_name": class_name,
            "count": max(1, int(count)),
            "weight": int(weight),
            "condition": condition,
        })
        return self

    def entries(self) -> List[Dict[str, Any]]:
        """Copies of all entries (conditions omitted — not JSON-safe)."""
        return [{"class_name": e["class_name"], "count": e["count"],
                 "weight": e["weight"]} for e in self._entries]

    def roll(self, rng: Any = bd) -> Optional[Dict[str, Any]]:
        """Deterministic weighted pick; None when nothing is eligible.

        Returns ``{"class_name", "count"}`` for the winning entry.
        """
        eligible: List[Dict[str, Any]] = []
        for entry in self._entries:
            if entry["weight"] <= 0:
                continue
            condition = entry.get("condition")
            if condition is not None:
                try:
                    if not condition():
                        continue
                except Exception as exc:
                    bd.warn(f"bd_rpg: loot condition for "
                            f"{entry['class_name']!r} raised: {exc!r}")
                    continue
            eligible.append(entry)
        total = sum(entry["weight"] for entry in eligible)
        if total <= 0:
            return None
        pick = _roll(rng, 1, total)
        cumulative = 0
        for entry in eligible:
            cumulative += entry["weight"]
            if pick <= cumulative:
                return {"class_name": entry["class_name"],
                        "count": entry["count"]}
        return {"class_name": eligible[-1]["class_name"],
                "count": eligible[-1]["count"]}

    def drop_for(self, victim_ref: Any,
                 killer_player_index: Optional[int] = None) -> Any:
        """Roll once and spawn the winning entry at the victim's position.

        Returns the spawned Actor handle, or None when the roll produced
        nothing or the spawn failed (warned). ``count`` above 1 spawns a
        stack only for classes that merge natively (ammo); otherwise the
        single spawned actor stands for the drop — adjust by spawning more
        in an ``on_drop`` listener if your drop needs fan-out.
        """
        entry = self.roll()
        if entry is None:
            return None
        try:
            if victim_ref is None or not victim_ref.valid:
                return None
            x, y, z = victim_ref.x, victim_ref.y, victim_ref.z
        except Exception:
            return None
        try:
            return bd.spawn(entry["class_name"], x, y, z, force=True)
        except Exception as exc:
            bd.warn(f"bd_rpg: could not spawn loot "
                    f"{entry['class_name']!r}: {exc!r}")
            return None

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<LootTable {self.name!r} {len(self._entries)} entries>"


class LootRules:
    """Wire loot tables to ``actor_died`` with exact killer attribution.

    Rules are ``(target, table, rarity)`` triples checked in registration
    order; the first match wins. ``target`` is either a class name
    (case-insensitive exact match against the victim's class) or a
    predicate receiving the full ``actor_died`` event dict.

    By default only deaths whose ``attacker_player_index`` names a player
    pawn drop loot (the same exact-attribution policy as
    ``bd_quests.track_kills``); construct with ``require_player=False``
    to let any death drop. On a drop the killer gets the rarity flash
    (:data:`RARITIES` color via ``bd.screen_flash``) and rarity sound
    (``bd.play_ui_sound``), and every ``on_drop`` listener fires with
    ``(event, entry, rarity, spawned_ref)``. Construct with
    ``screen_feedback=False`` to skip the screen flash (full-view tints
    can fight the player's aim); the rarity sound still plays.
    """

    def __init__(self, require_player: bool = True,
                 screen_feedback: bool = True) -> None:
        self.require_player: bool = bool(require_player)
        self.screen_feedback: bool = bool(screen_feedback)
        self._rules: List[tuple] = []
        self._armed: bool = False
        self.on_drop: List[Callable] = []

    def register(self, class_name_or_predicate: Any, table: LootTable,
                 rarity: str = "common") -> LootTable:
        """Register a drop rule and return its table (for chaining)."""
        rarity = str(rarity).strip().lower()
        if rarity not in RARITIES:
            raise ValueError(f"rarity must be one of "
                             f"{sorted(RARITIES)}")
        if not callable(class_name_or_predicate):
            class_name_or_predicate = str(class_name_or_predicate).lower()
        self._rules.append((class_name_or_predicate, table, rarity))
        self._arm()
        return table

    def _arm(self) -> None:
        """Register the consolidated ``actor_died`` handler exactly once."""
        if self._armed:
            return
        self._armed = True
        rules = self

        @bd.on("actor_died")
        def _on_death(event: Dict[str, Any]) -> None:
            try:
                rules._handle_death(event)
            except Exception as exc:
                bd.warn(f"bd_rpg: loot rule failed: {exc!r}")

    def _handle_death(self, event: Dict[str, Any]) -> None:
        killer_index = event.get("attacker_player_index")
        if self.require_player and killer_index is None:
            return
        victim_class = str((event.get("actor") or {}).get("class_name")
                           or "").lower()
        for target, table, rarity in self._rules:
            if callable(target):
                try:
                    if not target(event):
                        continue
                except Exception as exc:
                    bd.warn(f"bd_rpg: loot predicate raised: {exc!r}")
                    continue
            elif not victim_class or victim_class != target:
                continue
            ref = event.get("actor_ref")
            if ref is None:
                return
            spawned = table.drop_for(ref, killer_index)
            if spawned is None:
                return
            self._rarity_feedback(rarity)
            entry = {"class_name": spawned.class_name, "count": 1}
            for listener in list(self.on_drop):
                try:
                    listener(event, entry, rarity, spawned)
                except Exception as exc:
                    bd.warn(f"bd_rpg: on_drop listener raised: {exc!r}")
            return  # first matching rule wins

    def _rarity_feedback(self, rarity: str) -> None:
        spec = RARITIES.get(rarity) or RARITIES["common"]
        r, g, b = spec["color"]
        try:
            if self.screen_feedback \
                    and float(spec.get("flash") or 0.0) > 0.0:
                bd.screen_flash(r, g, b, float(spec["flash"]))
            if spec.get("sound"):
                bd.play_ui_sound(str(spec["sound"]))
        except Exception as exc:
            bd.warn(f"bd_rpg: rarity feedback failed: {exc!r}")


# --- kill XP glue ---------------------------------------------------------------------
#
# Genre-neutral experience plumbing with no rules-system dependency:
# award_kill_xp feeds per-player XP pools and notifies listeners;
# track_kill_xp turns actor_died events into awards from a mod-supplied
# class->XP table (exact player-credit attribution) or registers a plain
# listener callback.

_xp_pools: Dict[int, int] = {}
_xp_listeners: List[Callable[[int, int], None]] = []


def award_kill_xp(amount: int,
                  attacker_player_index: Optional[int] = None) -> int:
    """Credit kill XP to a player pool; returns the player's new total.

    ``attacker_player_index=None`` credits the console player (slot 0).
    Non-positive amounts are ignored. Every registered listener fires with
    ``(amount, player_index)``; a raising listener only warns.
    """
    index = int(attacker_player_index) if attacker_player_index is not None else 0
    amount = int(amount)
    if amount <= 0:
        return _xp_pools.get(index, 0)
    total = _xp_pools.get(index, 0) + amount
    _xp_pools[index] = total
    for listener in list(_xp_listeners):
        try:
            listener(amount, index)
        except Exception as exc:
            bd.warn(f"bd_rpg: XP listener raised: {exc!r}")
    return total


def kill_xp(player_index: int = 0) -> int:
    """Current XP total of a player pool."""
    return int(_xp_pools.get(int(player_index), 0))


def track_kill_xp(callback_or_table: Any,
                  player_index: Optional[int] = 0) -> Any:
    """Register kill-XP wiring; returns what was registered.

    - A **callable** is added as an XP listener ``(amount, player_index)``
      fired by :func:`award_kill_xp`.
    - A **mapping** of class name -> XP wires an ``actor_died`` handler
      that awards the tabled amount on matching kills. With the default
      ``player_index=0`` only kills credited to that exact player count
      (``attacker_player_index`` must equal it); pass ``None`` for the
      legacy any-death policy (monster infighting included).
    """
    if callable(callback_or_table):
        _xp_listeners.append(callback_or_table)
        return callback_or_table
    table = {str(name).lower(): int(xp)
             for name, xp in dict(callback_or_table).items()}
    wanted = None if player_index is None else int(player_index)

    @bd.on("actor_died")
    def _on_death(event: Dict[str, Any]) -> None:
        try:
            if wanted is not None and event.get("attacker_player_index") != wanted:
                return
            victim_class = str((event.get("actor") or {}).get("class_name")
                               or "").lower()
            amount = table.get(victim_class, 0)
            if amount > 0:
                award_kill_xp(amount, event.get("attacker_player_index"))
        except Exception as exc:
            bd.warn(f"bd_rpg: kill-XP tracker failed: {exc!r}")

    return _on_death
