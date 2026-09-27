"""Vampire chronicle RPG framework for BiasedDoom Python mods.

``bd_vtm`` is an engine-shipped, dependency-free package (it only needs
``biaseddoom``, and optionally integrates with ``bd_quests``) that gives
mods a *Vampire: The Masquerade*-inspired rules layer — generic, evocative
mechanics, no licensed assets:

- **Blood pool.** :class:`BloodPool` tracks vitae by generation
  (13th-gen fledglings hold 10 points, 7th-gen elders 20; the
  :attr:`BloodPool.MAX_BY_GENERATION` table is a plain moddable dict).
  An optional per-night upkeep task drains 1 point every
  ``upkeep_tics`` of map time.
- **Hunger.** :class:`Hunger` derives a 0-5 hunger level from how empty
  the blood pool is and runs deterministic frenzy checks
  (``roll 1d10 <= effective hunger``) through the engine's script RNG.
- **Humanity.** :class:`Humanity` models the 0-10 morality track with
  classic degeneration rolls (loss is resisted unless the roll beats the
  current rating) and a ``frenzy_bonus`` consequence below 4.
- **Disciplines.** :class:`Discipline` wraps a mod-supplied effect
  callable with blood costs, map-time cooldowns, and guarded execution;
  :func:`celerity`, :func:`obfuscate`, :func:`potence`, and
  :func:`dominate` are engine-implemented builtins.
- **Feeding.** :func:`feed` drains a victim actor through its live
  handle, refills the pool, applies humanity loss on a kill, and reports
  witnesses to a :class:`Masquerade` tracker.
- **Masquerade.** :class:`Masquerade` counts violations (0-5) and fires
  ``on_breach`` callbacks at 5.
- **Factions.** :class:`Factions` keeps a faction-vs-faction disposition
  matrix plus per-faction player reputation; :class:`GatedQuest` /
  :func:`requires_faction` gate ``bd_quests`` quests behind reputation
  without monkey-patching.
- **Persistence.** :class:`VtMState` owns all of the above and mirrors
  ``bd_quests``: ``save()``/``load()`` round-trip through
  ``bd.state["bd_vtm"]`` on the engine's ``save``/``load`` events.
- **HUD.** ``bd_vtm.hud.VtMHud`` renders the state as a Dear ImGui
  window from an ``imgui_frame`` handler;
  ``bd_vtm.hud.bind_hud_toggle`` wires a console alias and an optional
  key bind (through the engine's ``pyui`` command and the ``ui_command``
  event) to flip its visibility.

Minimal usage::

    import biaseddoom as bd
    import bd_vtm

    state = bd_vtm.VtMState(generation=13)
    state.register_discipline(bd_vtm.celerity(level=1))

    @bd.on("map_load")
    def begin(event):
        state.start_systems()     # blood upkeep + masquerade decay
        state.arm_persistence()   # save/load round-trip via bd.state

Determinism
-----------

All randomness flows through the engine's deterministic script RNG
(``bd.randint`` via the ``rng=`` parameters, defaulting to the ``bd``
module itself). The script RNG stream position is serialized into
savegames, so frenzy and degeneration rolls resume exactly after a
checkpoint load. For isolated unit-style tests every roll accepts any
object exposing ``randint(lo, hi)`` (or ``int(lo, hi)``, matching
``bd.rng()`` streams) — a scripted test double works.

Engine-honesty notes
--------------------

The builtins are implemented with the shipped Actor handle primitives
only, and their limits are documented per function: ``obfuscate`` is
purely visual (alpha), it does not break already-acquired monster
targeting; ``dominate`` pacifies (damage output zeroed) rather than
charming, because the Python API exposes no friendly/AI override.
"""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional

import biaseddoom as bd

__all__ = [
    "BloodPool", "Hunger", "Humanity", "Discipline", "DisciplineRegistry",
    "celerity", "obfuscate", "potence", "dominate", "feed",
    "Masquerade", "Faction", "Factions", "GatedQuest", "requires_faction",
    "VtMState", "STATE_KEY", "__version__",
]

__version__ = "1.1.0"

#: Key under which VtMState persists itself in ``bd.state``.
STATE_KEY = "bd_vtm"


# --- helpers -----------------------------------------------------------------


def _roll(rng: Any, lo: int, hi: int) -> int:
    """Inclusive ``lo..hi`` roll through the engine's deterministic RNG.

    ``rng`` may be the ``biaseddoom`` module itself (``bd.randint``), a
    ``bd.rng()`` stream (``.int``), or any test double exposing either.
    """
    roller = getattr(rng, "randint", None)
    if roller is None:
        roller = getattr(rng, "int", None)
    if roller is None:
        raise TypeError("rng must provide randint(lo, hi) or int(lo, hi)")
    return int(roller(lo, hi))


def _level_time() -> int:
    """Current map time in tics, or 0 when no level is loaded."""
    try:
        return bd.level_time()
    except Exception:
        return 0


def _fire(callbacks: List[Callable], *args: Any) -> None:
    """Invoke every registered callback; a buggy hook only warns."""
    for callback in list(callbacks):
        try:
            callback(*args)
        except Exception as exc:
            bd.warn(f"bd_vtm: callback {callback!r} raised: {exc!r}")


def _same_actor(a: Any, b: Any) -> bool:
    """Heuristic identity between two Actor handles.

    ``bd.actor_refs`` returns fresh handle objects per call, so ``is``
    comparison is not enough. Python gameplay mutation (and therefore
    :func:`feed`) is single-player only, so matching all player pawns
    together is safe.
    """
    if a is b:
        return True
    try:
        if a.tid != 0 and a.tid == b.tid:
            return True
        if a.is_player and b.is_player:
            return True
        if a.class_name == b.class_name and a.distance_to(b) < 1.0:
            return True
    except Exception:
        pass
    return False


def _restore_later(actor: Any, attr: str, value: Any, delay: int) -> None:
    """Schedule restoring one actor attribute, guarded against staleness.

    The task is deliberately *not* map-local: hub map transitions keep
    actors alive while map-local tasks are cancelled on unload, which
    would leave a discipline boost (speed, alpha, damage) permanently
    applied. The ``actor.valid`` guard makes firing after a real map
    change safe — a stale handle is simply skipped with a warning.
    """

    def revert() -> None:
        try:
            if actor is not None and actor.valid:
                setattr(actor, attr, value)
        except Exception as exc:
            bd.warn(f"bd_vtm: could not restore {attr} on an actor: {exc!r}")

    bd.schedule(revert, delay=max(1, int(delay)), map_local=False)


# --- blood pool ----------------------------------------------------------------


class BloodPool:
    """Vitae reservoir sized by generation, with optional nightly upkeep.

    ``current`` starts full at ``max`` (derived from
    :attr:`MAX_BY_GENERATION`; unknown generations get
    :attr:`DEFAULT_MAX`). Every mutation notifies the
    ``on_blood_changed`` callback list with the pool itself.

    Upkeep models the classic "waking each night costs 1 blood": when
    armed with :meth:`start_upkeep`, a repeating engine task spends 1
    point every ``upkeep_tics`` of map time (default one minute,
    ``35 * 60``). The task is a no-op while no map is loaded and while
    the pool is empty.
    """

    #: Maximum blood pool per generation. Plain dict — mods may edit it.
    MAX_BY_GENERATION: Dict[int, int] = {
        13: 10, 12: 11, 11: 12, 10: 13, 9: 14, 8: 15, 7: 20,
    }
    #: Pool size for generations missing from the table.
    DEFAULT_MAX: int = 10
    #: Default upkeep period: one "night" per minute of map time.
    DEFAULT_UPKEEP_TICS: int = 35 * 60

    def __init__(self, generation: int = 13,
                 upkeep_tics: Optional[int] = None) -> None:
        self.generation: int = int(generation)
        self.max: int = int(self.MAX_BY_GENERATION.get(self.generation,
                                                       self.DEFAULT_MAX))
        self.current: int = self.max
        self.upkeep_tics: int = (int(upkeep_tics) if upkeep_tics
                                 else self.DEFAULT_UPKEEP_TICS)
        self.on_blood_changed: List[Callable[[BloodPool], None]] = []
        self._upkeep_task: Optional[int] = None

    def spend(self, n: int = 1) -> bool:
        """Spend ``n`` blood; False (no change) when the pool is short."""
        n = int(n)
        if n < 0:
            raise ValueError("cannot spend a negative amount of blood")
        if self.current < n:
            return False
        self.current -= n
        _fire(self.on_blood_changed, self)
        return True

    def gain(self, n: int = 1) -> int:
        """Gain up to ``n`` blood (clamped at max); returns the real gain."""
        n = int(n)
        if n < 0:
            raise ValueError("cannot gain a negative amount of blood")
        before = self.current
        self.current = min(self.max, self.current + n)
        if self.current != before:
            _fire(self.on_blood_changed, self)
        return self.current - before

    def start_upkeep(self) -> None:
        """(Re)arm the repeating nightly-upkeep task. Call from map_load."""
        self.stop_upkeep()
        try:
            self._upkeep_task = bd.schedule(self._upkeep,
                                            delay=self.upkeep_tics,
                                            repeat=self.upkeep_tics,
                                            map_local=False)
        except Exception as exc:
            bd.warn(f"bd_vtm: could not schedule blood upkeep: {exc!r}")
            self._upkeep_task = None

    def stop_upkeep(self) -> None:
        """Cancel the upkeep task when one is running."""
        if self._upkeep_task is not None:
            try:
                bd.cancel_task(self._upkeep_task)
            except Exception:
                pass
            self._upkeep_task = None

    def _upkeep(self) -> None:
        try:
            if bd.current_map() is None:
                return
        except Exception:
            return
        if self.current > 0:
            self.spend(1)

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<BloodPool {self.current}/{self.max} gen {self.generation}>"


# --- hunger --------------------------------------------------------------------


class Hunger:
    """0-5 hunger track derived from blood-pool fullness.

    :meth:`update_from_pool` maps the pool fraction to a hunger level
    (the hungrier the vampire, the emptier the veins)::

        pool empty        -> 5
        pool <= 20% full  -> 4
        pool <= 40% full  -> 3
        pool <= 60% full  -> 2
        pool <= 80% full  -> 1
        otherwise         -> 0

    :meth:`check_frenzy` is a WTA-style roll: at effective hunger 4+
    (level plus ``bonus``, e.g. :attr:`Humanity.frenzy_bonus`), roll
    ``1d10`` through the deterministic script RNG; a roll at or below the
    effective hunger means frenzy and fires every ``on_frenzy`` callback
    with ``(hunger, roll)``.
    """

    MAX_LEVEL: int = 5
    FRENZY_MIN: int = 4
    #: (inclusive pool-fraction ceiling, hunger level), checked in order.
    THRESHOLDS = ((0.20, 4), (0.40, 3), (0.60, 2), (0.80, 1))

    def __init__(self, level: int = 0) -> None:
        self.level: int = max(0, min(self.MAX_LEVEL, int(level)))
        self.on_frenzy: List[Callable[[Hunger, int], None]] = []

    def update_from_pool(self, pool: BloodPool) -> int:
        """Recompute the hunger level from a blood pool; returns it."""
        if pool.max <= 0 or pool.current <= 0:
            self.level = self.MAX_LEVEL
        else:
            fraction = pool.current / pool.max
            self.level = 0
            for ceiling, level in self.THRESHOLDS:
                if fraction <= ceiling:
                    self.level = level
                    break
        return self.level

    def check_frenzy(self, rng: Any = bd, bonus: int = 0) -> bool:
        """Deterministic frenzy roll; True (and ``on_frenzy``) on frenzy."""
        effective = min(self.MAX_LEVEL, self.level + int(bonus))
        if effective < self.FRENZY_MIN:
            return False
        roll = _roll(rng, 1, 10)
        if roll > effective:
            return False
        _fire(self.on_frenzy, self, roll)
        return True

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Hunger {self.level}/{self.MAX_LEVEL}>"


# --- humanity ------------------------------------------------------------------


class Humanity:
    """0-10 morality track with classic degeneration rolls.

    :meth:`lose` performs a degeneration check: roll ``1d10`` through the
    deterministic script RNG; the loss is **resisted** unless the roll
    beats the current rating (``roll > rating``). At rating 10 a loss is
    therefore always resisted; low ratings are fragile. :meth:`gain`
    needs no roll. Both notify ``on_humanity_changed`` callbacks with
    ``(humanity, delta, reason)``.

    Consequence hook: at rating 3 or lower the Beast is stronger, so
    :attr:`frenzy_bonus` returns 1 — add it to hunger checks (this is
    what :meth:`VtMState.frenzy_check` does).

    The default roller is the engine's ``bd`` module; tests may pass any
    object with ``randint(lo, hi)``/``int(lo, hi)`` per call or install
    one as :attr:`rng`.
    """

    MIN_RATING: int = 0
    MAX_RATING: int = 10
    #: At or below this rating, frenzy checks worsen by ``frenzy_bonus``.
    FRENZY_PENALTY_THRESHOLD: int = 3

    def __init__(self, rating: int = 7, rng: Any = None) -> None:
        self.rating: int = max(self.MIN_RATING,
                               min(self.MAX_RATING, int(rating)))
        self.rng: Any = rng if rng is not None else bd
        self.on_humanity_changed: List[Callable[[Humanity, int, str], None]] = []

    @property
    def frenzy_bonus(self) -> int:
        """Extra effective hunger for frenzy checks at low humanity."""
        return 1 if self.rating <= self.FRENZY_PENALTY_THRESHOLD else 0

    def lose(self, n: int = 1, reason: str = "", rng: Any = None) -> bool:
        """Degeneration check; True when the loss was applied."""
        n = int(n)
        if n <= 0 or self.rating <= self.MIN_RATING:
            return False
        roller = rng if rng is not None else self.rng
        roll = _roll(roller, 1, 10)
        if roll <= self.rating:
            return False  # resisted the degeneration
        before = self.rating
        self.rating = max(self.MIN_RATING, self.rating - n)
        _fire(self.on_humanity_changed, self, self.rating - before,
              str(reason))
        return True

    def gain(self, n: int = 1, reason: str = "") -> bool:
        """Raise the rating (clamped at 10); True when it changed."""
        n = int(n)
        if n <= 0 or self.rating >= self.MAX_RATING:
            return False
        before = self.rating
        self.rating = min(self.MAX_RATING, self.rating + n)
        _fire(self.on_humanity_changed, self, self.rating - before,
              str(reason))
        return True

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Humanity {self.rating}>"


# --- disciplines -----------------------------------------------------------------


class Discipline:
    """A blood-powered ability: cost, map-time cooldown, guarded effect.

    ``effect`` is a mod-supplied callable ``(user_actor,
    target_actor_or_none) -> None``. :meth:`use` runs the full contract:

    1. optional ``validate(user, target) -> problem_string_or_None``
       hook (fail: ``bd.warn`` + False, nothing spent);
    2. cooldown check against map time (fail: ``bd.warn`` + False);
    3. blood spend from the bound :class:`BloodPool` (fail: ``bd.warn``
       + False);
    4. the effect call itself, wrapped in try/except — a buggy effect
       warns instead of propagating out of an engine callback (the blood
       and cooldown are still spent; the power fizzled).

    Bind a pool by assigning :attr:`pool` or by registering the
    discipline on a :class:`VtMState`. Without a pool the blood cost is
    skipped (documented escape hatch for pool-less users).
    """

    def __init__(self, name: str, blood_cost: int = 1, cooldown_tics: int = 70,
                 effect: Optional[Callable[[Any, Any], None]] = None,
                 description: str = "",
                 validate: Optional[Callable[[Any, Any], Optional[str]]] = None
                 ) -> None:
        self.name: str = str(name)
        self.blood_cost: int = max(0, int(blood_cost))
        self.cooldown_tics: int = max(0, int(cooldown_tics))
        self.effect = effect
        self.description: str = str(description)
        self.validate = validate
        #: Blood pool charged by use(); VtMState registers one for you.
        self.pool: Optional[BloodPool] = None
        self._cooldown_until: int = 0

    def ready_in(self) -> int:
        """Tics until the discipline can be used again (0 = ready)."""
        return max(0, self._cooldown_until - _level_time())

    def ready(self) -> bool:
        """True when the cooldown has elapsed."""
        return self.ready_in() <= 0

    def use(self, user: Any, target: Any = None) -> bool:
        """Attempt the discipline; see the class docstring for the steps."""
        if self.validate is not None:
            try:
                problem = self.validate(user, target)
            except Exception as exc:
                bd.warn(f"bd_vtm: {self.name} validation failed: {exc!r}")
                return False
            if problem:
                bd.warn(f"bd_vtm: {self.name}: {problem}")
                return False
        remaining = self.ready_in()
        if remaining > 0:
            bd.warn(f"bd_vtm: {self.name} is on cooldown "
                    f"({remaining} tics left)")
            return False
        if self.pool is not None and self.blood_cost > 0:
            if not self.pool.spend(self.blood_cost):
                bd.warn(f"bd_vtm: not enough blood for {self.name} "
                        f"(need {self.blood_cost}, have {self.pool.current})")
                return False
        self._cooldown_until = _level_time() + self.cooldown_tics
        if self.effect is not None:
            try:
                self.effect(user, target)
            except Exception as exc:
                bd.warn(f"bd_vtm: {self.name} effect raised: {exc!r}")
        return True

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Discipline {self.name!r} cost {self.blood_cost}>"


class DisciplineRegistry:
    """Name-keyed, data-driven discipline collection."""

    def __init__(self) -> None:
        self._disciplines: Dict[str, Discipline] = {}
        self._order: List[str] = []

    def register(self, discipline: Discipline) -> Discipline:
        """Register a discipline under its (unique) name and return it."""
        if discipline.name in self._disciplines:
            raise ValueError(f"duplicate discipline {discipline.name!r}")
        self._disciplines[discipline.name] = discipline
        self._order.append(discipline.name)
        return discipline

    def get(self, name: str) -> Optional[Discipline]:
        """Return the discipline with this name, or None."""
        return self._disciplines.get(str(name))

    def all(self) -> List[Discipline]:
        """All disciplines, in registration order."""
        return [self._disciplines[name] for name in self._order]


# -- built-in disciplines --------------------------------------------------------


def celerity(level: int = 1, duration_tics: int = 175, cooldown_tics: int = 350,
             blood_cost: int = 1) -> Discipline:
    """Supernatural speed: ``user.speed *= 1 + 0.25*level`` for a duration.

    The original speed is captured at activation and restored by a
    scheduled task (guarded against the actor going stale). Re-using
    celerity while a previous boost is still active stacks
    multiplicatively but each revert restores its own captured value —
    keep ``cooldown_tics >= duration_tics`` (the default) to avoid it.
    """
    level = max(1, int(level))

    def effect(user: Any, target: Any) -> None:
        original = user.speed
        user.speed = original * (1.0 + 0.25 * level)
        _restore_later(user, "speed", original, duration_tics)

    return Discipline("celerity", blood_cost=blood_cost,
                      cooldown_tics=cooldown_tics, effect=effect,
                      description=f"Move {25 * level}% faster for "
                                  f"{duration_tics} tics.")


def obfuscate(duration_tics: int = 175, cooldown_tics: int = 350,
              blood_cost: int = 1) -> Discipline:
    """Unseen presence: the user fades to alpha 0.15 for a duration.

    Engine-honesty note: this is *visual* concealment only. The Python
    API exposes no monster-AI "lose target" hook, so enemies that have
    already acquired the user keep attacking; combine with distance or
    line-of-sight breaks for a real stealth effect.
    """

    def effect(user: Any, target: Any) -> None:
        original = user.alpha
        user.alpha = 0.15
        _restore_later(user, "alpha", original, duration_tics)

    return Discipline("obfuscate", blood_cost=blood_cost,
                      cooldown_tics=cooldown_tics, effect=effect,
                      description=f"Fade to near-invisibility for "
                                  f"{duration_tics} tics.")


def potence(level: int = 1, duration_tics: int = 175,
            cooldown_tics: int = 350, blood_cost: int = 1) -> Discipline:
    """Brutal strength: ``user.damage_multiply *= 1 + level`` for a duration.

    Same capture/restore semantics as :func:`celerity`.
    """
    level = max(1, int(level))

    def effect(user: Any, target: Any) -> None:
        original = user.damage_multiply
        user.damage_multiply = original * (1.0 + level)
        _restore_later(user, "damage_multiply", original, duration_tics)

    return Discipline("potence", blood_cost=blood_cost,
                      cooldown_tics=cooldown_tics, effect=effect,
                      description=f"Deal {100 * (level + 1)}% damage for "
                                  f"{duration_tics} tics.")


def dominate(duration_tics: int = 105, cooldown_tics: int = 350,
             blood_cost: int = 1) -> Discipline:
    """Command the Beast in another: pacify a monster for a duration.

    The target's ``damage_multiply`` is zeroed (it stops dealing damage)
    and its velocity is cancelled once; the damage multiplier is restored
    after ``duration_tics``. Engine-honesty note: the Python API exposes
    no friendly-flag or AI-target override, so true charm/fear effects
    are out of scope — pacification is the honest approximation. The
    target must be a living monster.
    """

    def validate(user: Any, target: Any) -> Optional[str]:
        if target is None:
            return "dominate requires a target"
        try:
            if not target.valid or not target.alive:
                return "the target is not alive"
            if not target.is_monster:
                return "dominate only affects monsters"
        except Exception:
            return "the target handle is stale"
        return None

    def effect(user: Any, target: Any) -> None:
        original = target.damage_multiply
        target.damage_multiply = 0.0
        try:
            target.set_velocity(0.0, 0.0, 0.0)
        except Exception:
            pass
        _restore_later(target, "damage_multiply", original, duration_tics)

    return Discipline("dominate", blood_cost=blood_cost,
                      cooldown_tics=cooldown_tics, effect=effect,
                      validate=validate,
                      description=f"Pacify a monster for {duration_tics} tics.")


# --- masquerade ------------------------------------------------------------------


class Masquerade:
    """0-5 breach track for being seen doing vampiric things.

    :meth:`violation` raises the level and fires ``on_breach`` callbacks
    with ``(masquerade, reason)`` whenever the level is at 5 after a
    violation — both on the crossing and on every further violation at
    the cap (an exposed vampire keeps making it worse). The optional
    decay task (:meth:`start_decay`) removes one level every
    ``decay_tics`` of map time: mortal authorities forget, slowly.
    """

    MAX_LEVEL: int = 5
    BREACH_LEVEL: int = 5
    DEFAULT_DECAY_TICS: int = 35 * 45

    def __init__(self, decay_tics: Optional[int] = None) -> None:
        self.level: int = 0
        self.decay_tics: int = (int(decay_tics) if decay_tics
                                else self.DEFAULT_DECAY_TICS)
        self.on_breach: List[Callable[[Masquerade, str], None]] = []
        self._decay_task: Optional[int] = None

    def violation(self, reason: str = "", amount: int = 1) -> int:
        """Record a violation; returns the new level."""
        self.level = min(self.MAX_LEVEL, self.level + max(0, int(amount)))
        if self.level >= self.BREACH_LEVEL:
            _fire(self.on_breach, self, str(reason))
        return self.level

    def reduce(self, n: int = 1) -> int:
        """Lower the level (cover-up work); returns the new level."""
        self.level = max(0, self.level - max(0, int(n)))
        return self.level

    def start_decay(self) -> None:
        """(Re)arm the repeating decay task. Call from map_load."""
        self.stop_decay()
        try:
            self._decay_task = bd.schedule(self._decay,
                                           delay=self.decay_tics,
                                           repeat=self.decay_tics,
                                           map_local=False)
        except Exception as exc:
            bd.warn(f"bd_vtm: could not schedule masquerade decay: {exc!r}")
            self._decay_task = None

    def stop_decay(self) -> None:
        """Cancel the decay task when one is running."""
        if self._decay_task is not None:
            try:
                bd.cancel_task(self._decay_task)
            except Exception:
                pass
            self._decay_task = None

    def _decay(self) -> None:
        try:
            if bd.current_map() is None:
                return
        except Exception:
            return
        if self.level > 0:
            self.level -= 1

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Masquerade {self.level}/{self.MAX_LEVEL}>"


# --- feeding ---------------------------------------------------------------------


def feed(predator: Any, victim: Any, blood_pool: BloodPool,
         humanity: Optional[Humanity] = None,
         masquerade: Optional[Masquerade] = None,
         drain: int = 5, blood_gain: int = 1,
         witness_radius: float = 512.0) -> Dict[str, Any]:
    """Drain blood from a living victim actor through the native damage path.

    The victim takes ``drain`` damage via its live handle (with the
    predator as the damage source); the pool gains ``blood_gain``. When
    the drain kills the victim and ``humanity`` is given, a degeneration
    check fires (``humanity.lose(1, "drained a victim dry")``).

    When ``masquerade`` is given, every other living actor within
    ``witness_radius`` (2D) of the predator that passes a native
    ``check_sight`` on the predator counts as a witness, and any witness
    triggers ``masquerade.violation("feeding witnessed")``.

    Proximity of predator and victim is deliberately *not* enforced —
    melee range is a mod-side policy (``predator.distance_to(victim)``).

    Returns ``{"drained": int, "killed": bool, "witnessed": bool}``.
    """
    result: Dict[str, Any] = {"drained": 0, "killed": False,
                              "witnessed": False}
    if predator is None or victim is None or blood_pool is None:
        bd.warn("bd_vtm: feed needs a predator, a victim, and a blood pool")
        return result
    try:
        if not predator.valid or not victim.valid:
            bd.warn("bd_vtm: feed called with a stale actor handle")
            return result
        if not victim.alive:
            bd.warn("bd_vtm: feed: the victim is already dead")
            return result
        dealt = victim.damage(max(1, int(drain)), source=predator)
    except Exception as exc:
        bd.warn(f"bd_vtm: feed failed: {exc!r}")
        return result
    try:
        result["drained"] = max(0, int(dealt))
    except (TypeError, ValueError):
        result["drained"] = int(drain)
    blood_pool.gain(int(blood_gain))
    try:
        result["killed"] = not victim.alive
    except Exception:
        result["killed"] = True  # handle went stale: the victim is gone
    if result["killed"] and humanity is not None:
        humanity.lose(1, "drained a victim dry")
    if masquerade is not None:
        try:
            nearby = bd.actor_refs(sphere=(predator.x, predator.y,
                                           float(witness_radius)))
        except Exception as exc:
            bd.warn(f"bd_vtm: witness scan failed: {exc!r}")
            nearby = []
        for other in nearby:
            if _same_actor(other, predator) or _same_actor(other, victim):
                continue
            try:
                if other.alive and other.check_sight(predator):
                    result["witnessed"] = True
                    break
            except Exception:
                continue
        if result["witnessed"]:
            masquerade.violation("feeding witnessed")
    return result


# --- factions --------------------------------------------------------------------


class Faction:
    """A named faction with a disposition table toward other factions.

    ``dispositions`` maps faction name to a standing in -2..+2
    (-2 hostile, -1 unfriendly, 0 neutral, +1 friendly, +2 allied).
    """

    def __init__(self, name: str,
                 dispositions: Optional[Dict[str, int]] = None) -> None:
        self.name: str = str(name)
        self.dispositions: Dict[str, int] = {
            str(other): max(-2, min(2, int(value)))
            for other, value in (dispositions or {}).items()
        }

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Faction {self.name!r}>"


class Factions:
    """Faction registry: disposition matrix plus player reputation.

    :meth:`standing` resolves faction-vs-faction disposition (either
    side's table wins; default 0 neutral). :meth:`reputation` /
    :meth:`change_reputation` track how each faction regards the player
    (unbounded integer, starts at 0); changes fire
    ``on_reputation_changed`` callbacks with ``(factions, name, value,
    delta, reason)``. Only reputations are persisted by
    :class:`VtMState` — faction definitions come from mod code, exactly
    like quest definitions in ``bd_quests``.
    """

    HOSTILE: int = -2
    UNFRIENDLY: int = -1
    NEUTRAL: int = 0
    FRIENDLY: int = 1
    ALLIED: int = 2

    def __init__(self) -> None:
        self._factions: Dict[str, Faction] = {}
        self._reputation: Dict[str, int] = {}
        self.on_reputation_changed: List[Callable] = []

    def add(self, faction: Faction) -> Faction:
        """Register a faction (by unique name) and return it."""
        if faction.name in self._factions:
            raise ValueError(f"duplicate faction {faction.name!r}")
        self._factions[faction.name] = faction
        return faction

    def get(self, name: str) -> Optional[Faction]:
        """Return the faction with this name, or None."""
        return self._factions.get(str(name))

    def standing(self, faction_a: Any, faction_b: Any) -> int:
        """Disposition of ``faction_a`` toward ``faction_b`` (-2..+2)."""
        name_a = faction_a.name if isinstance(faction_a, Faction) else str(faction_a)
        name_b = faction_b.name if isinstance(faction_b, Faction) else str(faction_b)
        if name_a == name_b:
            return self.ALLIED
        fa = self._factions.get(name_a)
        if fa is not None and name_b in fa.dispositions:
            return fa.dispositions[name_b]
        fb = self._factions.get(name_b)
        if fb is not None and name_a in fb.dispositions:
            return fb.dispositions[name_a]
        return self.NEUTRAL

    def reputation(self, name: str) -> int:
        """The player's current reputation with a faction (default 0)."""
        return int(self._reputation.get(str(name), 0))

    def change_reputation(self, name: str, delta: int, reason: str = "") -> int:
        """Adjust the player's reputation; returns the new value."""
        name = str(name)
        value = self.reputation(name) + int(delta)
        self._reputation[name] = value
        _fire(self.on_reputation_changed, self, name, value, int(delta),
              str(reason))
        return value

    def serialize_reputations(self) -> Dict[str, int]:
        """JSON-safe snapshot of player reputations."""
        return {name: int(value) for name, value in self._reputation.items()}

    def restore_reputations(self, data: Any) -> None:
        """Tolerant restore of a :meth:`serialize_reputations` snapshot."""
        if not isinstance(data, dict):
            return
        for name, value in data.items():
            try:
                self._reputation[str(name)] = int(value)
            except (TypeError, ValueError):
                continue


class GatedQuest:
    """``bd_quests`` integration: a quest wrapper gated behind reputation.

    The wrapped quest is added to a ``bd_quests.QuestLog`` as usual (add
    ``gate.quest`` — not the gate — so the log's persistence keeps its
    own semantics); scripts then call ``gate.start()`` instead of
    ``quest.start()``. When the player's reputation with ``faction`` is
    below ``min_standing``, ``start()`` no-ops with an on-screen
    message and returns False; the quest stays INACTIVE. No
    monkey-patching: the gate is a plain predicate + delegation wrapper.
    """

    def __init__(self, quest: Any, factions: Factions, faction: str,
                 min_standing: int = 1, message: Optional[str] = None) -> None:
        self.quest = quest
        self.factions: Factions = factions
        self.faction: str = str(faction)
        self.min_standing: int = int(min_standing)
        self.message: str = (str(message) if message else
                             f"They don't trust you yet. "
                             f"({self.faction} standing "
                             f"{self.min_standing}+ required)")

    def can_start(self) -> bool:
        """True when the player's reputation satisfies the gate."""
        return self.factions.reputation(self.faction) >= self.min_standing

    def start(self) -> bool:
        """Start the wrapped quest when the gate allows it."""
        if getattr(self.quest, "state", "inactive") != "inactive":
            return self.quest.start()  # delegate the stock no-op semantics
        if not self.can_start():
            try:
                bd.hud_text(self.message, id=0, y=0.35, color="red",
                            hold=3.0, fade=0.5)
            except Exception:
                pass
            bd.log(f"bd_vtm: quest {getattr(self.quest, 'id', '?')!r} "
                   f"blocked by faction gate ({self.faction} standing "
                   f"{self.min_standing}+ required)")
            return False
        return self.quest.start()

    def __getattr__(self, name: str) -> Any:
        # Delegate everything else (id, state, objectives, ...) to the
        # wrapped quest. Only called for attributes the gate lacks.
        quest = self.__dict__.get("quest")
        if quest is None:
            raise AttributeError(name)
        return getattr(quest, name)


def requires_faction(quest: Any, factions: Factions, faction: str,
                     min_standing: int = 1,
                     message: Optional[str] = None) -> GatedQuest:
    """Wrap ``quest`` in a :class:`GatedQuest` reputation gate."""
    return GatedQuest(quest, factions, faction, min_standing, message)


# --- persistent chronicle state ---------------------------------------------------


class VtMState:
    """One chronicle's worth of vampire state, persisted via ``bd.state``.

    Owns the :class:`BloodPool`, :class:`Hunger`, :class:`Humanity`,
    :class:`Masquerade`, :class:`Factions`, and a
    :class:`DisciplineRegistry`, and wires them together:

    - hunger auto-updates whenever the blood pool changes;
    - disciplines registered through :meth:`register_discipline`
      automatically draw from the blood pool;
    - :meth:`frenzy_check` rolls hunger plus the humanity
      ``frenzy_bonus``;
    - :meth:`save`/:meth:`load` serialize everything JSON-safe into
      ``bd.state["bd_vtm"]``; :meth:`arm_persistence` registers the
      engine ``save``/``load`` handlers exactly once (mirroring
      ``bd_quests``). Discipline cooldowns persist as *remaining tics*
      and are re-anchored to the map time of the loaded game.
    """

    def __init__(self, generation: int = 13,
                 disciplines: Optional[DisciplineRegistry] = None,
                 state_key: str = STATE_KEY) -> None:
        self.state_key: str = str(state_key)
        self.blood: BloodPool = BloodPool(generation)
        self.hunger: Hunger = Hunger()
        self.humanity: Humanity = Humanity()
        self.masquerade: Masquerade = Masquerade()
        self.factions: Factions = Factions()
        self.disciplines: DisciplineRegistry = (disciplines if disciplines
                                                is not None
                                                else DisciplineRegistry())
        self._persistence_armed: bool = False
        self.blood.on_blood_changed.append(self._on_blood_changed)
        for discipline in self.disciplines.all():
            discipline.pool = self.blood

    # -- wiring ----------------------------------------------------------------

    def _on_blood_changed(self, pool: BloodPool) -> None:
        self.hunger.update_from_pool(pool)

    def register_discipline(self, discipline: Discipline) -> Discipline:
        """Register a discipline and bind it to this state's blood pool."""
        discipline.pool = self.blood
        return self.disciplines.register(discipline)

    def frenzy_check(self, rng: Any = bd) -> bool:
        """Hunger frenzy roll with the humanity consequence applied."""
        return self.hunger.check_frenzy(rng=rng,
                                        bonus=self.humanity.frenzy_bonus)

    def start_systems(self) -> None:
        """(Re)arm blood upkeep and masquerade decay. Call from map_load."""
        self.blood.start_upkeep()
        self.masquerade.start_decay()

    # -- persistence -------------------------------------------------------------

    def serialize(self) -> Dict[str, Any]:
        """Plain JSON-able snapshot (no Actor handles, no callables)."""
        return {
            "version": 1,
            "blood": {
                "generation": self.blood.generation,
                "max": self.blood.max,
                "current": self.blood.current,
                "upkeep_tics": self.blood.upkeep_tics,
            },
            "hunger": {"level": self.hunger.level},
            "humanity": {"rating": self.humanity.rating},
            "masquerade": {"level": self.masquerade.level},
            "reputations": self.factions.serialize_reputations(),
            "cooldowns": {
                d.name: d.ready_in()
                for d in self.disciplines.all()
                if d.ready_in() > 0
            },
        }

    def restore(self, data: Any) -> None:
        """Restore from a :meth:`serialize` snapshot; tolerant of junk."""
        if not isinstance(data, dict):
            return
        blood = data.get("blood")
        if isinstance(blood, dict):
            try:
                generation = int(blood.get("generation", self.blood.generation))
                maximum = int(blood.get("max", self.blood.max))
                if maximum > 0:
                    self.blood.generation = generation
                    self.blood.max = maximum
                    self.blood.current = max(0, min(
                        maximum, int(blood.get("current", self.blood.current))))
                upkeep = blood.get("upkeep_tics")
                if upkeep is not None:
                    self.blood.upkeep_tics = max(1, int(upkeep))
            except (TypeError, ValueError):
                pass
        hunger = data.get("hunger")
        if isinstance(hunger, dict):
            try:
                self.hunger.level = max(0, min(Hunger.MAX_LEVEL,
                                               int(hunger.get("level", 0))))
            except (TypeError, ValueError):
                pass
        humanity = data.get("humanity")
        if isinstance(humanity, dict):
            try:
                self.humanity.rating = max(
                    Humanity.MIN_RATING,
                    min(Humanity.MAX_RATING,
                        int(humanity.get("rating", self.humanity.rating))))
            except (TypeError, ValueError):
                pass
        masquerade = data.get("masquerade")
        if isinstance(masquerade, dict):
            try:
                self.masquerade.level = max(
                    0, min(Masquerade.MAX_LEVEL,
                           int(masquerade.get("level", 0))))
            except (TypeError, ValueError):
                pass
        self.factions.restore_reputations(data.get("reputations"))
        cooldowns = data.get("cooldowns")
        if isinstance(cooldowns, dict):
            now = _level_time()
            for name, remaining in cooldowns.items():
                discipline = self.disciplines.get(str(name))
                if discipline is None:
                    continue
                try:
                    discipline._cooldown_until = now + max(0, int(remaining))
                except (TypeError, ValueError):
                    continue

    def save(self) -> None:
        """Write the snapshot into ``bd.state`` (call from a save event)."""
        bd.state[self.state_key] = self.serialize()

    def load(self) -> None:
        """Restore from ``bd.state`` (call from a load event)."""
        self.restore(bd.state.get(self.state_key))

    def arm_persistence(self) -> None:
        """Register the save/load handlers exactly once per state."""
        if self._persistence_armed:
            return
        self._persistence_armed = True
        state = self

        @bd.on("save")
        def _on_save(event: Dict[str, Any]) -> None:
            try:
                state.save()
            except Exception as exc:
                bd.warn(f"bd_vtm: could not serialize chronicle state: {exc!r}")

        @bd.on("load")
        def _on_load(event: Dict[str, Any]) -> None:
            try:
                state.load()
            except Exception as exc:
                bd.warn(f"bd_vtm: could not restore chronicle state: {exc!r}")


def __getattr__(name: str) -> Any:
    # Lazy convenience accessors so `bd_vtm.VtMHud` (and friends) work
    # without paying for the UI module unless it is actually used
    # (mirrors bd_quests).
    if name == "VtMHud":
        from .hud import VtMHud
        return VtMHud
    if name == "bind_hud_toggle":
        from .hud import bind_hud_toggle
        return bind_hud_toggle
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
