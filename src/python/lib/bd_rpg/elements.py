"""Damage types, affinities, and the attack resolver for ``bd_rpg``.

This submodule is imported lazily by ``bd_rpg`` (see the package
``__getattr__``); import it directly only when you want the names without
the package prefix.
"""

from __future__ import annotations

import re
from typing import Any, Callable, Dict, List, Optional, Tuple

import biaseddoom as bd

#: Reserved key under which every bd_rpg per-actor payload lives inside
#: ``bd.actor_data(ref)``. Sub-keys: ``"aff"`` (affinities), ``"soak"``
#: (flat damage soak), ``"status"`` (status engine records, see status.py).
ACTOR_KEY = "bd_rpg"


# --- engine RNG helpers ---------------------------------------------------------


def _roll(rng: Any, lo: int, hi: int) -> int:
    """Inclusive ``lo..hi`` roll; accepts ``bd``, ``bd.rng()`` streams, doubles."""
    roller = getattr(rng, "randint", None)
    if roller is None:
        roller = getattr(rng, "int", None)
    if roller is None:
        raise TypeError("rng must provide randint(lo, hi) or int(lo, hi)")
    return int(roller(lo, hi))


def _rng_float(rng: Any) -> float:
    """Next float in [0, 1); accepts ``bd``, ``bd.rng()`` streams, doubles."""
    source = getattr(rng, "random", None)
    if source is None:
        source = getattr(rng, "float", None)
    if source is None:
        raise TypeError("rng must provide random() or float()")
    return float(source())


# --- per-actor payload ----------------------------------------------------------
#
# Engine-honesty note: ``bd.actor_data`` dicts are keyed by the handle's
# actor slot and are purged when the LAST live Python handle to the actor
# is garbage-collected (not only on actor destruction). bd_rpg therefore
# retains every handle it annotates in ``_retained``; stale handles are
# pruned lazily once the list grows. Mods writing their own actor_data
# payloads must pin a handle the same way or the data silently vanishes
# with the handle.

_retained: List[Any] = []


def _retain(ref: Any) -> None:
    """Keep one Python handle alive so the actor's data dict survives."""
    for existing in _retained:
        if existing is ref:
            return
    _retained.append(ref)
    if len(_retained) > 64:
        kept = []
        for handle in _retained:
            try:
                if handle.valid:
                    kept.append(handle)
            except Exception:
                pass
        _retained[:] = kept


def _pack_data(ref: Any, create: bool = True) -> Dict[str, Any]:
    """Return the actor's ``bd.actor_data`` sub-dict reserved for bd_rpg."""
    data = bd.actor_data(ref)
    pack = data.get(ACTOR_KEY)
    if pack is None:
        if not create:
            return {}
        pack = {}
        data[ACTOR_KEY] = pack
    return pack


# --- damage type registry ---------------------------------------------------------


class DamageTypes:
    """Registry of named damage types with presentation metadata.

    Types are plain names (case-insensitive) carrying an ``(r, g, b)``
    color and an optional sound lump for UI feedback. The registry is a
    script-side definition: it is **not** persisted by
    :class:`bd_rpg.RpgState` — re-register your custom types at import or
    ``engine_start`` time, exactly like the pre-registered set here.
    """

    #: Types registered by default: physical, fire, ice, poison, acid,
    #: shock, holy, dark.
    BUILTINS: Dict[str, Dict[str, Any]] = {
        "physical": {"color": (210, 200, 190), "sound": None},
        "fire": {"color": (255, 96, 32), "sound": None},
        "ice": {"color": (120, 200, 255), "sound": None},
        "poison": {"color": (96, 220, 96), "sound": None},
        "acid": {"color": (176, 255, 64), "sound": None},
        "shock": {"color": (255, 240, 96), "sound": None},
        "holy": {"color": (255, 250, 214), "sound": None},
        "dark": {"color": (154, 64, 210), "sound": None},
    }

    def __init__(self, register_builtins: bool = True) -> None:
        self._types: Dict[str, Dict[str, Any]] = {}
        self._order: List[str] = []
        if register_builtins:
            for name, spec in self.BUILTINS.items():
                self.register(name, color=spec["color"], sound=spec["sound"])

    def register(self, name: str, color: Tuple[int, int, int] = (255, 255, 255),
                 sound: Optional[str] = None) -> Dict[str, Any]:
        """Register (or redefine) a damage type; returns its entry dict."""
        key = str(name).strip().lower()
        if not key:
            raise ValueError("damage type name must not be empty")
        try:
            r, g, b = (int(color[0]), int(color[1]), int(color[2]))
        except (TypeError, ValueError, IndexError) as exc:
            raise ValueError("color must be an (r, g, b) triple") from exc
        entry = {"name": key, "color": (r, g, b),
                 "sound": None if sound is None else str(sound)}
        if key not in self._types:
            self._order.append(key)
        self._types[key] = entry
        return dict(entry)

    def get(self, name: str) -> Optional[Dict[str, Any]]:
        """Return a copy of the type entry, or None when unregistered."""
        entry = self._types.get(str(name).strip().lower())
        return dict(entry) if entry is not None else None

    def all(self) -> List[Dict[str, Any]]:
        """All registered type entries, in registration order."""
        return [dict(self._types[name]) for name in self._order]


#: Shared damage type registry. Mods may register additional types on it.
damage_types = DamageTypes()


# --- affinities ------------------------------------------------------------------
#
# Affinities are per-damage-type multipliers on damage TAKEN: 1.0 neutral,
# 0.0 immune, 0.5 resistant, 2.0 weak. Per-actor affinities live in
# bd.actor_data under ACTOR_KEY/"aff"; class-level defaults live in a plain
# module dict and are matched lazily by exact class name (no per-actor
# copies, no subclass walking). A per-actor entry overrides the class
# default for that type. Neither is engine-persisted: RpgState mirrors the
# PLAYER's affinities into bd.state; monsters re-derive theirs from class
# defaults, and mods re-apply per-monster affinities on map_load.

_class_affinities: Dict[str, Dict[str, float]] = {}


def set_affinity(ref: Any, damage_type: str, mult: float) -> None:
    """Set one actor's affinity multiplier for a damage type."""
    aff = _pack_data(ref).setdefault("aff", {})
    aff[str(damage_type).strip().lower()] = float(mult)
    _retain(ref)


def affinity_of(ref: Any, damage_type: str) -> float:
    """Effective affinity of an actor toward a damage type (default 1.0).

    Per-actor override first, then the exact class-name default, then 1.0.
    Returns 1.0 (neutral) for stale handles rather than raising.
    """
    key = str(damage_type).strip().lower()
    try:
        aff = _pack_data(ref, create=False).get("aff") or {}
        if key in aff:
            return float(aff[key])
        class_name = str(ref.class_name).lower()
    except Exception:
        return 1.0
    class_aff = _class_affinities.get(class_name) or {}
    return float(class_aff.get(key, 1.0))


def set_class_affinity(class_name: str, damage_type: str, mult: float) -> None:
    """Set a class-level default affinity, matched lazily by class name.

    Evaluated per resolution against ``ref.class_name`` (exact,
    case-insensitive); never copied onto actors. Script-side definition —
    not persisted by :class:`bd_rpg.RpgState`.
    """
    table = _class_affinities.setdefault(str(class_name).strip().lower(), {})
    table[str(damage_type).strip().lower()] = float(mult)


def class_affinities() -> Dict[str, Dict[str, float]]:
    """Snapshot of all class-level affinity defaults (for inspection)."""
    return {name: dict(table) for name, table in _class_affinities.items()}


def set_soak(ref: Any, amount: int) -> None:
    """Set an actor's flat post-affinity damage soak (default 0)."""
    _pack_data(ref)["soak"] = max(0, int(amount))
    _retain(ref)


def soak_of(ref: Any) -> int:
    """An actor's flat soak value (0 for stale handles)."""
    try:
        return max(0, int(_pack_data(ref, create=False).get("soak", 0)))
    except Exception:
        return 0


# --- dice notation -----------------------------------------------------------------
#
# Deliberate duplication: bd_dnd ships a richer dice roller, but bd_rpg keeps
# its own minimal parser so the two packs stay independent (bd_rpg only needs
# ``biaseddoom``). If you use both packs, prefer bd_dnd.roll for checks and
# let resolve_attack handle its own damage rolls.

_DICE_RE = re.compile(r"^\s*(\d*)\s*[dD]\s*(\d+)\s*(?:([+-])\s*(\d+))?\s*$")


def _parse_dice(spec: Any) -> Tuple[int, int, int]:
    """Parse ``NdM+K`` notation (or a bare int) into (count, sides, bonus)."""
    if isinstance(spec, bool):
        raise TypeError("damage must be an int or NdM+K notation, not bool")
    if isinstance(spec, int):
        return (0, 0, int(spec))
    if isinstance(spec, str):
        text = spec.strip()
        try:
            return (0, 0, int(text))
        except ValueError:
            pass
        match = _DICE_RE.match(text)
        if match is None:
            raise ValueError(f"bad dice notation {spec!r}")
        count = int(match.group(1)) if match.group(1) else 1
        sides = int(match.group(2))
        if count < 1 or sides < 1:
            raise ValueError(f"bad dice notation {spec!r}")
        bonus = int(match.group(4)) if match.group(4) else 0
        if match.group(3) == "-":
            bonus = -bonus
        return (count, sides, bonus)
    raise TypeError("damage must be an int or NdM+K notation string")


def _roll_dice(spec: Any, rng: Any) -> int:
    """Total of an NdM+K roll through the deterministic script RNG."""
    count, sides, bonus = _parse_dice(spec)
    total = bonus
    for _ in range(count):
        total += _roll(rng, 1, sides)
    return total


# --- the attack resolver ------------------------------------------------------------


def resolve_attack(attacker_ref: Any, defender_ref: Any,
                   attack: Dict[str, Any], rng: Any = bd) -> Dict[str, Any]:
    """Resolve one RPG attack against a defender actor and apply the damage.

    ``attack`` is a plain dict::

        {"damage": 10 or "2d6+3",   # int or NdM+K dice notation
         "type": "fire",            # DamageTypes name (default "physical")
         "accuracy": int | None,    # None skips the hit check entirely
         "defense": int | None,     # defender's evasion (None/0 = none)
         "crit_chance": 0.05,       # probability, rolled with bd.random
         "crit_mult": 2.0}          # raw-damage multiplier on a crit

    Pipeline (every roll flows through ``rng``, defaulting to the engine's
    deterministic, savegame-serialized script RNG):

    1. **Hit check** (skipped when ``accuracy`` is None):
       ``rng.randint(1, 20) + accuracy`` vs. ``10 + defense`` — a d20-style
       attack roll; the attack misses when the total is lower.
    2. **Crit roll**: ``rng.random() < crit_chance`` multiplies the raw
       damage by ``crit_mult``.
    3. **Raw roll**: the ``NdM+K`` damage total.
    4. **Affinity**: multiplied by :func:`affinity_of` for the attack type
       (0.0 immune, 0.5 resist, 2.0 weak). An immune defender (multiplier
       0) takes ``final == 0`` and soak is never consulted.
    5. **Soak**: the defender's flat soak (``actor_data`` ``"bd_rpg"``
       ``"soak"``, default 0) absorbs ``min(soak, dmg - 1)`` — at least 1
       point always gets through unless the defender is immune.
    6. **Apply**: ``defender_ref.damage(final, damage_type=type,
       source=attacker_ref)``. This flows through the native
       ``actor_before_damage`` filter: the resolver computes the number,
       but the native filter has the last word (it can rewrite or cancel
       the hit; a cancelled hit deals nothing and fires no
       ``actor_damaged``).

    Returns ``{"hit", "critical", "raw", "multiplier", "soak", "final",
    "killed", "type"}`` — ``soak`` is the amount absorbed. A miss or a
    stale defender applies no damage. All engine interactions are guarded:
    failures warn and degrade to a no-hit result instead of raising out of
    the caller's event handler.
    """
    attack = dict(attack or {})
    dtype = str(attack.get("type", "physical")).strip().lower() or "physical"
    result: Dict[str, Any] = {"hit": False, "critical": False, "raw": 0,
                              "multiplier": 1.0, "soak": 0, "final": 0,
                              "killed": False, "type": dtype}
    try:
        if defender_ref is None or not defender_ref.valid:
            bd.warn("bd_rpg: resolve_attack called with a stale defender")
            return result
        if not defender_ref.alive:
            return result
    except Exception:
        bd.warn("bd_rpg: resolve_attack called with a stale defender")
        return result

    accuracy = attack.get("accuracy")
    if accuracy is not None:
        defense = attack.get("defense") or 0
        roll = _roll(rng, 1, 20) + int(accuracy)
        if roll < 10 + int(defense):
            return result
    result["hit"] = True

    raw = _roll_dice(attack.get("damage", 1), rng)
    crit_chance = float(attack.get("crit_chance", 0.0) or 0.0)
    if crit_chance > 0.0 and _rng_float(rng) < crit_chance:
        result["critical"] = True
        raw = int(raw * float(attack.get("crit_mult", 2.0) or 2.0))
    result["raw"] = raw

    multiplier = affinity_of(defender_ref, dtype)
    result["multiplier"] = multiplier
    dmg = int(raw * multiplier)
    if dmg > 0:
        soak = soak_of(defender_ref)
        absorbed = min(soak, dmg - 1) if soak > 0 else 0
        result["soak"] = absorbed
        dmg -= absorbed
    final = max(0, dmg)
    result["final"] = final
    if final > 0:
        try:
            defender_ref.damage(final, damage_type=dtype, source=attacker_ref)
        except Exception as exc:
            bd.warn(f"bd_rpg: applying resolved damage failed: {exc!r}")
        try:
            result["killed"] = not defender_ref.alive
        except Exception:
            result["killed"] = True  # handle went stale: the defender is gone
    return result
