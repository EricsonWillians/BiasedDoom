"""The dread machine: atmospheric horror systems for BiasedDoom mods.

``bd_horror.atmosphere`` bundles three cooperating systems plus a
persistence container:

- **Dread.** :class:`Dread` is a 0-100 tension meter on ONE repeating
  35-tic task: it rises while the player stands in darkness (sector light
  below 96) or with monsters nearby, decays otherwise, and spikes by
  ``damage_spike`` whenever the console player is hurt. Crossing 25/50/75
  or 100 in either direction fires the ``on_threshold`` callback list.
  Driven effects (each a constructor flag): a heartbeat whose interval
  shrinks from 105 to 35 tics as dread climbs, a four-edge display-list
  vignette whose alpha tracks dread, and whisper stings on upward
  crossings.
- **Light programs.** :class:`LightManager` animates sector lights by
  tag: :meth:`~LightManager.candle` (smooth random walk),
  :meth:`~LightManager.fluorescent` (steady with random full dropouts),
  and :meth:`~LightManager.blackout` (force to 0, restore after a
  duration). Programs write ``sector.light`` only when the value changes,
  capture and restore the original light levels, and re-bind to sector
  tags on every ``map_load`` — they are TID-free, so savegames and map
  transitions rebind them naturally.
- **Stalkers.** :class:`StalkerDirector` rolls once per ``window_tics``
  while dread is at least 75 and spawns a monster *behind* the player,
  capped at ``max_alive`` concurrent stalkers.
- **Persistence.** :class:`HorrorState` owns all of the above and mirrors
  the ``bd_vtm.VtMState`` pattern: ``arm_persistence()`` round-trips the
  dread level and the active light-program descriptors through
  ``bd.state["bd_horror"]`` on the engine ``save``/``load`` events.

Minimal usage::

    import biaseddoom as bd
    import bd_horror

    state = bd_horror.HorrorState()

    @bd.on("map_load")
    def begin(event):
        state.start()             # dread tick + stalker windows
        state.arm_persistence()   # save/load round-trip via bd.state
        if not event.get("from_savegame") and not event.get("from_hub"):
            state.lights.candle([5], amplitude=24)
            state.stalker.enabled = True

Performance contract (docs/development/python-performance.md)
-------------------------------------------------------------

- **Rule 3 (throttle hot callbacks):** every world interaction here runs
  on ``bd.schedule`` tasks of at least 35 tics (dread tick, stalker
  windows); candle/fluorescent steps write at most a handful of sector
  lights. Nothing scans actors per tic.
- **Rule 1 (push filters down):** monster proximity uses
  ``bd.actor_refs(sphere=(x, y, r))`` so the native thinker scan discards
  far actors without a CPython crossing each.
- The vignette rides the persistent display list (``bd.draw_rect``): four
  crossings per slow dread tick, zero per rendered frame.

Determinism
-----------

All randomness flows through the engine's deterministic script RNG
(``bd.random``/``bd.randint``), whose stream position is serialized into
savegames. Light-program runtime state (candle walk position, fluorescent
dropout, blackout remainder) round-trips through the persistence
descriptors, so a checkpoint load resumes the exact light sequence.
"""

from __future__ import annotations

import math
from typing import Any, Callable, Dict, List, Optional, Sequence

import biaseddoom as bd

#: Key under which HorrorState persists itself in ``bd.state``.
STATE_KEY = "bd_horror"

# Display-list ids reserved for the vignette edges. The bd.ui toolkit
# allocates from 900000 (toast/announce at 999000+); bd_horror stays well
# below that range.
_VIGNETTE_ID_BASE = 888000
_VIGNETTE_LAYER = 8

# Stock Doom II sounds (logical names verified against wadsrc/static/
# filter/game-doomchex/sndinfo.txt; the lumps DSSAWHIT / DSITMBK /
# DSSGTSIT are present in doom2.wad).
_HEARTBEAT_SOUND = "weapons/sawhit"  # DSSAWHIT: short dull thud
_WHISPER_SOUND = "misc/spawn"        # DSITMBK: airy reversed shimmer
_STALKER_SOUND = "demon/sight"       # DSSGTSIT: pinky waking growl

#: Whisper lines for upward threshold crossings. ASCII only (the HUD font
#: is Latin-1 at best).
WHISPERS: Sequence[str] = (
    "it knows your name",
    "behind you",
    "the dark is breathing",
    "do not look at the walls",
    "it is already inside",
)


def _level_time() -> int:
    """Current map time in tics, or 0 when no level is loaded."""
    try:
        return bd.level_time()
    except Exception:
        return 0


def _player_pawn() -> Any:
    """Live handle to the console player's pawn, or None."""
    try:
        player = bd.player(0)
        pawn = player.actor if player is not None else None
        if pawn is not None and pawn.valid:
            return pawn
    except Exception:
        pass
    return None


def _fire(callbacks: List[Callable], *args: Any) -> None:
    """Invoke every registered callback; a buggy hook only warns."""
    for callback in list(callbacks):
        try:
            callback(*args)
        except Exception as exc:
            bd.warn(f"bd_horror: callback {callback!r} raised: {exc!r}")


def _clamp_level(value: float) -> float:
    return max(0.0, min(100.0, float(value)))


# --- dread ---------------------------------------------------------------------


class Dread:
    """0-100 tension meter with threshold callbacks and driven effects.

    The internal tick runs on ONE repeating ``bd.schedule`` task
    (``tick_tics``, default 35 — once a second). Per tick:

    - player sector light (``bd.sector_at`` on the pawn's position) below
      :attr:`DARK_LIGHT` adds ``rise_dark``;
    - each living monster within ``monster_radius`` (capped at
      ``monster_cap``) adds ``rise_monster``;
    - otherwise the level decays by ``decay``;
    - ``actor_damaged`` events against the console player's pawn add a
      ``damage_spike`` through :meth:`on_player_damage`.

    Crossing any of :attr:`THRESHOLDS` fires every ``on_threshold``
    callback with ``(dread, threshold, rising)`` in both directions.

    Driven effects, each toggleable via a constructor flag: ``heartbeat``
    (a quiet stock thud every ``lerp(105, 35, level/100)`` tics),
    ``vignette`` (four edge rects on the persistent display list, alpha
    tracking the level), and ``whispers`` (rate-limited HUD line plus
    shimmer sound on upward crossings).
    """

    MIN_LEVEL: float = 0.0
    MAX_LEVEL: float = 100.0
    THRESHOLDS: Sequence[float] = (25.0, 50.0, 75.0, 100.0)
    #: Sector light below this counts as darkness.
    DARK_LIGHT: int = 96
    #: Minimum map-tic gap between whisper stings.
    STING_COOLDOWN_TICS: int = 175

    def __init__(self, tick_tics: int = 35, rise_dark: float = 2.0,
                 rise_monster: float = 0.75, decay: float = 1.0,
                 monster_radius: float = 256.0, monster_cap: int = 4,
                 damage_spike: float = 15.0, heartbeat: bool = True,
                 vignette: bool = True, whispers: bool = True) -> None:
        self.tick_tics: int = max(1, int(tick_tics))
        self.rise_dark: float = float(rise_dark)
        self.rise_monster: float = float(rise_monster)
        self.decay: float = float(decay)
        self.monster_radius: float = float(monster_radius)
        self.monster_cap: int = max(1, int(monster_cap))
        self.damage_spike: float = float(damage_spike)
        self.heartbeat: bool = bool(heartbeat)
        self.vignette: bool = bool(vignette)
        self.whispers: bool = bool(whispers)
        self.on_threshold: List[Callable[[Dread, float, bool], None]] = []
        self._level: float = 0.0
        self._task: Optional[int] = None
        self._damage_wired: bool = False
        self._next_heartbeat: int = 0
        self._last_sting: int = -10 ** 9
        self._vignette_drawn: bool = False

    @property
    def level(self) -> float:
        """Current dread level, 0.0 - 100.0."""
        return self._level

    def set_level(self, value: float) -> None:
        """Debug/test setter: jump the level, firing threshold crossings."""
        self._set(value, fire=True)

    def on_player_damage(self, amount: float = 1.0) -> None:
        """Apply the damage spike (wired to ``actor_damaged`` by start())."""
        try:
            if float(amount) <= 0.0:
                return
        except (TypeError, ValueError):
            return
        self._set(self._level + self.damage_spike)

    def start(self) -> None:
        """(Re)arm the repeating tick and the damage hook. Idempotent."""
        self.stop()
        try:
            self._task = bd.schedule(self._tick, delay=self.tick_tics,
                                     repeat=self.tick_tics, map_local=False)
        except Exception as exc:
            bd.warn(f"bd_horror: could not schedule the dread tick: {exc!r}")
            self._task = None
        self._wire_damage()

    def stop(self) -> None:
        """Cancel the tick task and clear the vignette."""
        if self._task is not None:
            try:
                bd.cancel_task(self._task)
            except Exception:
                pass
            self._task = None
        self._clear_vignette()

    # -- internals -----------------------------------------------------------

    def _wire_damage(self) -> None:
        if self._damage_wired:
            return
        self._damage_wired = True
        dread = self

        @bd.on("actor_damaged")
        def _on_actor_damaged(event: Dict[str, Any]) -> None:
            ref = event.get("actor_ref")
            try:
                if ref is None or not ref.is_player:
                    return
            except Exception:
                return
            try:
                dread.on_player_damage(float(event.get("damage") or 0))
            except Exception as exc:
                bd.warn(f"bd_horror: damage spike failed: {exc!r}")

    def _set(self, value: float, fire: bool = True) -> None:
        value = _clamp_level(value)
        old = self._level
        if value == old:
            return
        self._level = value
        if not fire:
            return
        for threshold in self.THRESHOLDS:
            if old < threshold <= value:
                self._crossed(threshold, True)
            elif value < threshold <= old:
                self._crossed(threshold, False)

    def _crossed(self, threshold: float, rising: bool) -> None:
        if rising:
            self._sting(threshold)
        _fire(self.on_threshold, self, threshold, rising)

    def _sting(self, threshold: float) -> None:
        """Whisper sting on an upward crossing, rate-limited. Draws RNG."""
        if not self.whispers:
            return
        now = _level_time()
        if now - self._last_sting < self.STING_COOLDOWN_TICS:
            return
        self._last_sting = now
        line = bd.choice(list(WHISPERS))
        try:
            bd.hud_text(line, id=_VIGNETTE_ID_BASE + 10, y=0.22,
                        color="red", hold=3.0, fade=1.0)
        except Exception:
            pass  # no active status bar (headless, intermission)
        try:
            bd.play_ui_sound(_WHISPER_SOUND, volume=0.6)
        except Exception:
            pass

    def _tick(self) -> None:
        try:
            if bd.current_map() is None:
                return
        except Exception:
            return
        pawn = _player_pawn()
        if pawn is None:
            return
        delta = 0.0
        rising = False
        try:
            sector = bd.sector_at(pawn.x, pawn.y)
        except Exception:
            sector = None
        if sector is not None:
            try:
                if int(sector.light) < self.DARK_LIGHT:
                    delta += self.rise_dark
                    rising = True
            except Exception:
                pass
        count = self._count_monsters(pawn)
        if count > 0:
            delta += self.rise_monster * count
            rising = True
        if not rising:
            delta = -self.decay
        self._set(self._level + delta)
        self._effects_tick()

    def _count_monsters(self, pawn: Any) -> int:
        # Rule 1 of docs/development/python-performance.md: the radius
        # filter runs inside the native thinker scan; only the (few)
        # nearby survivors cross into Python for the is_monster check.
        try:
            refs = bd.actor_refs(sphere=(pawn.x, pawn.y,
                                         float(self.monster_radius)))
        except Exception as exc:
            bd.warn(f"bd_horror: monster proximity scan failed: {exc!r}")
            return 0
        count = 0
        for ref in refs:
            if count >= self.monster_cap:
                break
            try:
                if ref.is_monster and ref.alive and not ref.is_player:
                    count += 1
            except Exception:
                continue
        return count

    def _effects_tick(self) -> None:
        now = _level_time()
        if self.heartbeat and now >= self._next_heartbeat:
            frac = self._level / self.MAX_LEVEL
            interval = 105 + frac * (35 - 105)  # lerp(105, 35, dread/100)
            self._next_heartbeat = now + max(1, int(round(interval)))
            try:
                bd.play_ui_sound(_HEARTBEAT_SOUND,
                                 volume=0.25 + 0.45 * frac)
            except Exception:
                pass  # no sound device / headless
        if self.vignette:
            self._update_vignette()

    def _update_vignette(self) -> None:
        # The display list persists between frames, so these four
        # crossings happen only on the slow dread tick (performance rule 3).
        frac = self._level / self.MAX_LEVEL
        try:
            if frac <= 0.02:
                self._clear_vignette()
                return
            alpha = min(0.65, 0.65 * frac)
            color = (6, 3, 5)
            bd.draw_rect(id=_VIGNETTE_ID_BASE + 0, x=0.0, y=0.0, w=1.0,
                         h=0.16, color=color, alpha=alpha,
                         layer=_VIGNETTE_LAYER)
            bd.draw_rect(id=_VIGNETTE_ID_BASE + 1, x=0.0, y=0.84, w=1.0,
                         h=0.16, color=color, alpha=alpha,
                         layer=_VIGNETTE_LAYER)
            bd.draw_rect(id=_VIGNETTE_ID_BASE + 2, x=0.0, y=0.0, w=0.10,
                         h=1.0, color=color, alpha=alpha,
                         layer=_VIGNETTE_LAYER)
            bd.draw_rect(id=_VIGNETTE_ID_BASE + 3, x=0.90, y=0.0, w=0.10,
                         h=1.0, color=color, alpha=alpha,
                         layer=_VIGNETTE_LAYER)
            self._vignette_drawn = True
        except Exception:
            pass  # headless or no renderer: the vignette simply never shows

    def _clear_vignette(self) -> None:
        if not self._vignette_drawn:
            return
        self._vignette_drawn = False
        for i in range(4):
            try:
                bd.draw_clear(_VIGNETTE_ID_BASE + i)
            except Exception:
                pass

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Dread {self._level:.1f}>"


# --- light programs --------------------------------------------------------------


class LightProgram:
    """One animated light effect over the sectors of one or more tags.

    Created through :class:`LightManager` (``candle`` / ``fluorescent`` /
    ``blackout``), not directly. A program resolves its sectors with
    ``bd.sectors(tag=...)`` at arm time, captures each sector's original
    light, and writes ``sector.light`` only when the computed value
    changes. ``stop()`` restores the originals; programs rebind by tag on
    every ``map_load``, so they survive map transitions and — through
    :class:`HorrorState` — savegames, with no TIDs involved.

    Runtime state (``state``) and ``originals`` are plain JSON-able data
    so :meth:`descriptor` can snapshot the whole program into ``bd.state``.
    """

    KINDS = ("candle", "fluorescent", "blackout")

    def __init__(self, kind: str, tags: Sequence[int],
                 params: Optional[Dict[str, Any]] = None,
                 originals: Optional[Dict[int, int]] = None,
                 state: Optional[Dict[str, Any]] = None) -> None:
        kind = str(kind)
        if kind not in self.KINDS:
            raise ValueError(f"bd_horror: unknown light program kind {kind!r}")
        if isinstance(tags, int):
            tags = [tags]
        self.kind: str = kind
        self.tags: List[int] = [int(tag) for tag in tags]
        self.params: Dict[str, Any] = dict(params or {})
        self.originals: Dict[int, int] = dict(originals or {})
        self.state: Dict[str, Any] = dict(state or {})
        self._sectors: List[Any] = []
        self._task: Optional[int] = None
        self._last_written: Dict[int, int] = {}
        self._manager: Optional[LightManager] = None

    # -- arming ----------------------------------------------------------------

    def arm(self, fresh: bool = True) -> None:
        """(Re)bind sectors and (re)start the program's task.

        ``fresh=True`` recaptures the original light levels and resets the
        runtime state (normal map transitions). ``fresh=False`` keeps the
        in-memory/descriptor state (savegame restores, where the native
        save already holds the program's light values).
        """
        self._cancel_task()
        self._sectors = self._resolve()
        if not self._sectors:
            return  # no matching tag in this map; inert until next rearm
        if fresh or not self.originals:
            self.originals = {}
            for sector in self._sectors:
                try:
                    self.originals[int(sector.index)] = int(sector.light)
                except Exception:
                    continue
            self._init_state()
        self._last_written = {}
        self._start_task()

    def disarm(self, restore: bool = True) -> None:
        """Cancel the task and drop sector handles.

        With ``restore=True`` the original light levels are written back.
        On ``map_unload`` restore is impossible (world mutation is blocked
        during the event and the map is going away), so the manager calls
        this with ``restore=False``.
        """
        self._cancel_task()
        if restore:
            self._write_originals()
        self._sectors = []
        self._last_written = {}

    # -- per-kind behavior -------------------------------------------------------

    def _init_state(self) -> None:
        """Reset runtime state after a fresh arm (base sampling lives here).

        Deliberately RNG-free: arming must not consume script-RNG draws,
        or the rewound stream after a checkpoint load would drift.
        """
        if self.kind == "candle":
            base = self.params.get("base")
            if base is None:
                base = self._sample_base()
            base = max(0, min(255, int(base)))
            self.state = {"base": base, "value": float(base)}
        elif self.kind == "fluorescent":
            base = self.params.get("base")
            if base is None:
                base = self._sample_base()
            base = max(0, min(255, int(base)))
            self.state = {"base": base, "dropout": 0}
        elif self.kind == "blackout":
            self.state = {"remaining": max(1, int(self.params.get(
                "duration_tics", 70)))}

    def _sample_base(self) -> int:
        """The first resolved sector's current light, at arm time."""
        try:
            return int(self._sectors[0].light)
        except Exception:
            return 160

    def _start_task(self) -> None:
        if self.kind == "blackout":
            self._write_all(0)
            remaining = max(1, int(self.state.get("remaining", 1)))
            self._task = bd.schedule(self._complete_blackout,
                                     delay=remaining, map_local=True)
            return
        period = max(1, int(self.params.get(
            "period", 9 if self.kind == "candle" else 2)))
        self._task = bd.schedule(self._step, delay=period, repeat=period,
                                 map_local=True)

    def _step(self) -> None:
        """One animation step (candle walk / fluorescent dropout check)."""
        if not self._sectors:
            return
        if self.kind == "candle":
            amplitude = max(0, int(self.params.get("amplitude", 24)))
            base = int(self.state.get("base", 160))
            value = float(self.state.get("value", float(base)))
            value += (bd.random() * 2.0 - 1.0) * amplitude * 0.5
            value = max(float(base - amplitude),
                        min(float(base + amplitude), value))
            self.state["value"] = value
            self._write_all(int(round(value)))
        elif self.kind == "fluorescent":
            base = int(self.state.get("base", 160))
            dropout = int(self.state.get("dropout", 0))
            if dropout > 0:
                self.state["dropout"] = dropout - 1
                self._write_all(8)
                return
            chance = float(self.params.get("dropout_chance", 0.06))
            if bd.random() < chance:
                period = max(1, int(self.params.get("period", 2)))
                tics = bd.randint(2, 5)
                self.state["dropout"] = max(1, int(round(tics / period)))
                self._write_all(8)
            else:
                self._write_all(base)

    def _complete_blackout(self) -> None:
        self._task = None
        self._write_originals()
        manager = self._manager
        if manager is not None:
            manager._remove(self)

    # -- sector I/O ----------------------------------------------------------------

    def _resolve(self) -> List[Any]:
        sectors: List[Any] = []
        seen = set()
        for tag in self.tags:
            try:
                found = bd.sectors(tag=tag)
            except Exception as exc:
                bd.warn(f"bd_horror: sector lookup for tag {tag} failed: "
                        f"{exc!r}")
                continue
            for sector in found:
                try:
                    index = int(sector.index)
                except Exception:
                    continue
                if index in seen:
                    continue
                seen.add(index)
                sectors.append(sector)
        return sectors

    def _write_all(self, light: int) -> None:
        """Write a light level to every bound sector, only on change."""
        light = max(0, min(255, int(light)))
        for sector in self._sectors:
            try:
                index = int(sector.index)
                if self._last_written.get(index) == light:
                    continue
                sector.light = light
                self._last_written[index] = light
            except Exception:
                continue

    def _write_originals(self) -> None:
        for sector in self._sectors:
            try:
                index = int(sector.index)
            except Exception:
                continue
            original = self.originals.get(index)
            if original is None:
                continue
            try:
                if self._last_written.get(index) != original:
                    sector.light = int(original)
                    self._last_written[index] = int(original)
            except Exception:
                continue

    def _cancel_task(self) -> None:
        if self._task is not None:
            try:
                bd.cancel_task(self._task)
            except Exception:
                pass
            self._task = None

    # -- persistence -----------------------------------------------------------------

    def descriptor(self) -> Dict[str, Any]:
        """JSON-able snapshot: kind, tags, params, originals, live state."""
        return {
            "kind": self.kind,
            "tags": list(self.tags),
            "params": dict(self.params),
            "originals": {str(index): int(light)
                          for index, light in self.originals.items()},
            "state": dict(self.state),
        }

    @classmethod
    def from_descriptor(cls, data: Any) -> Optional["LightProgram"]:
        """Rebuild a program from a :meth:`descriptor` snapshot (tolerant)."""
        if not isinstance(data, dict):
            return None
        kind = data.get("kind")
        if kind not in cls.KINDS:
            return None
        tags = data.get("tags")
        if not isinstance(tags, list):
            return None
        try:
            tags = [int(tag) for tag in tags]
        except (TypeError, ValueError):
            return None
        params = data.get("params")
        state = data.get("state")
        originals: Dict[int, int] = {}
        saved_originals = data.get("originals")
        if isinstance(saved_originals, dict):
            for key, value in saved_originals.items():
                try:
                    originals[int(key)] = int(value)
                except (TypeError, ValueError):
                    continue
        try:
            return cls(kind, tags,
                       params=params if isinstance(params, dict) else {},
                       originals=originals,
                       state=state if isinstance(state, dict) else {})
        except Exception as exc:
            bd.warn(f"bd_horror: could not rebuild a light program: {exc!r}")
            return None

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<LightProgram {self.kind} tags={self.tags}>"


class LightManager:
    """Owns the active light programs and their map-event rebinding.

    Programs are created with :meth:`candle`, :meth:`fluorescent`, and
    :meth:`blackout`; several programs coexist (distinct tags recommended —
    overlapping sectors are last-writer-wins per step). The manager lazily
    registers ``map_load``/``map_unload`` handlers on first use: programs
    re-bind (and, on fresh maps, re-sample their originals) on every map
    load, and disarm without restoring on unload. Transient blackouts do
    not cross map transitions; everything else does.

    Savegame persistence of program *state* is :class:`HorrorState`'s job
    (:meth:`descriptors` / :meth:`load_descriptors`); standalone managers
    re-arm after a load with their in-memory state, which may be ahead of
    the rewound world.
    """

    def __init__(self) -> None:
        self.programs: List[LightProgram] = []
        self._events_armed: bool = False

    # -- creation ------------------------------------------------------------

    def candle(self, tags: Any, base: Optional[int] = None,
               amplitude: int = 24, period: int = 9) -> LightProgram:
        """Smooth random walk around ``base`` (sampled at arm time if None).

        Every ``period`` tics the light level takes a bounded random step
        (drawn from the deterministic script RNG) inside
        ``base +/- amplitude``.
        """
        return self._add(LightProgram("candle", tags, {
            "base": None if base is None else int(base),
            "amplitude": max(0, int(amplitude)),
            "period": max(1, int(period)),
        }))

    def fluorescent(self, tags: Any, base: Optional[int] = None,
                    dropout_chance: float = 0.06,
                    period: int = 2) -> LightProgram:
        """Steady light with random full dropouts (2-5 tics) to near-dark.

        Every ``period`` tics the program rolls the deterministic RNG
        against ``dropout_chance``; on a hit the light drops to 8 for a
        2-5-tic flicker, otherwise it holds ``base`` (sampled at arm time
        when None).
        """
        return self._add(LightProgram("fluorescent", tags, {
            "base": None if base is None else int(base),
            "dropout_chance": max(0.0, min(1.0, float(dropout_chance))),
            "period": max(1, int(period)),
        }))

    def blackout(self, tags: Any, duration_tics: int = 70) -> LightProgram:
        """Force the sectors to light 0, then restore after the duration.

        One-shot: the program restores the original lights and removes
        itself when the duration elapses, and it never crosses a map
        transition (a blackout in flight at save time does persist through
        :class:`HorrorState`, with its remaining tics re-anchored).
        """
        return self._add(LightProgram("blackout", tags, {
            "duration_tics": max(1, int(duration_tics)),
        }))

    # -- bulk control ------------------------------------------------------------

    def stop_all(self, restore: bool = True) -> None:
        """Stop every program (restoring original lights by default)."""
        for program in list(self.programs):
            try:
                program.disarm(restore=restore)
            except Exception as exc:
                bd.warn(f"bd_horror: could not stop {program!r}: {exc!r}")

    def clear(self) -> None:
        """Stop (restoring) and forget every program."""
        self.stop_all(restore=True)
        self.programs = []

    def descriptors(self) -> List[Dict[str, Any]]:
        """JSON-able snapshots of every active program."""
        return [program.descriptor() for program in self.programs]

    def load_descriptors(self, data: Any) -> None:
        """Replace all programs from snapshots; re-arms when a map is up."""
        self.stop_all(restore=False)
        self.programs = []
        if not isinstance(data, list):
            return
        for entry in data:
            program = LightProgram.from_descriptor(entry)
            if program is None:
                continue
            program._manager = self
            self.programs.append(program)
            try:
                if bd.current_map() is not None:
                    program.arm(fresh=False)
            except Exception as exc:
                bd.warn(f"bd_horror: could not re-arm {program!r}: {exc!r}")

    # -- internals ---------------------------------------------------------------

    def _add(self, program: LightProgram) -> LightProgram:
        self._arm_events()
        program._manager = self
        self.programs.append(program)
        try:
            if bd.current_map() is not None:
                program.arm(fresh=True)
        except Exception as exc:
            bd.warn(f"bd_horror: could not arm {program!r}: {exc!r}")
        return program

    def _remove(self, program: LightProgram) -> None:
        try:
            self.programs.remove(program)
        except ValueError:
            pass

    def _arm_events(self) -> None:
        """Register the map-load/unload rebinding exactly once."""
        if self._events_armed:
            return
        self._events_armed = True
        manager = self

        @bd.on("map_load")
        def _on_map_load(event: Dict[str, Any]) -> None:
            # from_savegame/from_hub restores keep the descriptor
            # originals and in-memory state (the restored map already
            # holds the program's light values, and re-sampling a
            # blacked-out sector as the base would strand it at 0);
            # HorrorState's load handler re-arms again afterwards with
            # the persisted state.
            fresh = not (bool(event.get("from_savegame"))
                         or bool(event.get("from_hub")))
            for program in list(manager.programs):
                try:
                    program.arm(fresh=fresh)
                except Exception as exc:
                    bd.warn(f"bd_horror: could not re-arm {program!r}: "
                            f"{exc!r}")

        @bd.on("map_unload")
        def _on_map_unload(event: Dict[str, Any]) -> None:
            # Mutation is blocked during map_unload: disarm without
            # restoring. Blackouts are transient and end with the map.
            for program in list(manager.programs):
                try:
                    program.disarm(restore=False)
                except Exception:
                    pass
                if program.kind == "blackout":
                    manager._remove(program)


# --- stalker director --------------------------------------------------------------


class StalkerDirector:
    """Spawns a monster behind the player while dread runs high.

    Every ``window_tics`` (one repeating task) the director checks dread;
    at or above :attr:`DREAD_GATE` (75) it rolls the deterministic script
    RNG against ``chance`` and, on success, spawns a ``class_name`` monster
    ``spawn_radius`` map units *behind* the player (opposite the player's
    facing). The spot is validated with ``bd.sector_at`` — if the point
    lands in the void the spawn is skipped cleanly. At most ``max_alive``
    stalkers coexist; stale handles are pruned each window.

    ``spawn_log`` records one dict per resolved window outcome for
    autotests (``{"spawned": bool, ...}``, with positions on success).
    """

    #: Dread level at or above which windows are rolled.
    DREAD_GATE: float = 75.0

    def __init__(self, dread: Optional[Dread] = None, enabled: bool = False,
                 class_name: str = "Demon", max_alive: int = 2,
                 window_tics: int = 350, chance: float = 0.35,
                 spawn_radius: float = 192.0) -> None:
        self.dread: Optional[Dread] = dread
        self.enabled: bool = bool(enabled)
        self.class_name: str = str(class_name)
        self.max_alive: int = max(1, int(max_alive))
        self.window_tics: int = max(35, int(window_tics))
        self.chance: float = max(0.0, min(1.0, float(chance)))
        self.spawn_radius: float = float(spawn_radius)
        self.spawn_log: List[Dict[str, Any]] = []
        self.windows_rolled: int = 0
        self._stalkers: List[Any] = []
        self._task: Optional[int] = None

    def start(self) -> None:
        """(Re)arm the repeating window task. Idempotent."""
        self.stop()
        try:
            self._task = bd.schedule(self._window, delay=self.window_tics,
                                     repeat=self.window_tics,
                                     map_local=False)
        except Exception as exc:
            bd.warn(f"bd_horror: could not schedule stalker windows: "
                    f"{exc!r}")
            self._task = None

    def stop(self) -> None:
        """Cancel the window task (live stalkers are left alone)."""
        if self._task is not None:
            try:
                bd.cancel_task(self._task)
            except Exception:
                pass
            self._task = None

    def stalker_count(self) -> int:
        """Live, valid stalkers right now."""
        self._prune()
        return len(self._stalkers)

    # -- internals ---------------------------------------------------------------

    def _window(self) -> None:
        self.windows_rolled += 1
        if not self.enabled:
            return
        try:
            if bd.current_map() is None:
                return
        except Exception:
            return
        if self.dread is None or self.dread.level < self.DREAD_GATE:
            return
        self._prune()
        if len(self._stalkers) >= self.max_alive:
            self.spawn_log.append({"spawned": False, "reason": "capped"})
            return
        if bd.random() >= self.chance:
            self.spawn_log.append({"spawned": False, "reason": "roll"})
            return
        self._spawn()

    def _spawn(self) -> None:
        pawn = _player_pawn()
        if pawn is None:
            self.spawn_log.append({"spawned": False, "reason": "no_player"})
            return
        try:
            angle = math.radians(float(pawn.angle))
            x = float(pawn.x) - math.cos(angle) * self.spawn_radius
            y = float(pawn.y) - math.sin(angle) * self.spawn_radius
        except Exception:
            self.spawn_log.append({"spawned": False, "reason": "stale_pawn"})
            return
        try:
            sector = bd.sector_at(x, y)
        except Exception:
            sector = None
        if sector is None:
            # Behind the player is the void / outside the map: skip cleanly.
            self.spawn_log.append({"spawned": False, "reason": "no_sector"})
            return
        # Occupancy pre-check (native-filtered, one crossing): a previous
        # stalker or a wandering monster standing on the spot would make
        # bd.spawn fail anyway, so skip with a clean reason instead.
        try:
            occupants = bd.actor_refs(sphere=(x, y, 40.0))
        except Exception:
            occupants = []
        for occupant in occupants:
            try:
                if occupant.alive:
                    self.spawn_log.append({"spawned": False,
                                           "reason": "occupied"})
                    return
            except Exception:
                continue
        try:
            z = float(sector.floor_height)
        except Exception:
            z = float(pawn.floor_z)
        try:
            ref = bd.spawn(self.class_name, x, y, z, angle=float(pawn.angle))
        except Exception as exc:
            bd.warn(f"bd_horror: stalker spawn failed: {exc!r}")
            self.spawn_log.append({"spawned": False, "reason": "spawn_error"})
            return
        if ref is None:
            self.spawn_log.append({"spawned": False, "reason": "blocked"})
            return
        self._stalkers.append(ref)
        self.spawn_log.append({
            "spawned": True, "x": x, "y": y,
            "player_x": float(pawn.x), "player_y": float(pawn.y),
            "player_angle": float(pawn.angle),
        })
        try:
            bd.play_ui_sound(_STALKER_SOUND, volume=0.7)
        except Exception:
            pass

    def _prune(self) -> None:
        alive = []
        for ref in self._stalkers:
            try:
                if ref is not None and ref.valid and ref.alive:
                    alive.append(ref)
            except Exception:
                continue
        self._stalkers = alive


# --- persistent horror state -------------------------------------------------------


class HorrorState:
    """One mod's worth of horror state, persisted via ``bd.state``.

    Owns a :class:`Dread`, a :class:`LightManager`, and a
    :class:`StalkerDirector` (pre-bound to the dread meter), and mirrors
    the ``bd_vtm.VtMState`` persistence pattern: :meth:`arm_persistence`
    registers the engine ``save``/``load`` handlers exactly once, and the
    snapshot in ``bd.state["bd_horror"]`` carries the dread level plus the
    active light-program descriptors (kind, tags, params, originals, and
    runtime state). Programs are sector-tag based, so after a load they
    rebind to the restored map with no TIDs.
    """

    def __init__(self, state_key: str = STATE_KEY,
                 dread: Optional[Dread] = None, **dread_kwargs: Any) -> None:
        self.state_key: str = str(state_key)
        self.dread: Dread = dread if dread is not None else Dread(**dread_kwargs)
        self.lights: LightManager = LightManager()
        self.stalker: StalkerDirector = StalkerDirector(dread=self.dread)
        self._persistence_armed: bool = False

    def start(self) -> None:
        """(Re)arm the dread tick and stalker windows. Call from map_load."""
        self.dread.start()
        self.stalker.start()

    def stop(self) -> None:
        """Cancel dread/stalker tasks and stop every light program."""
        self.dread.stop()
        self.stalker.stop()
        self.lights.stop_all()

    # -- persistence -------------------------------------------------------------

    def serialize(self) -> Dict[str, Any]:
        """Plain JSON-able snapshot (no handles, no callables)."""
        return {
            "version": 1,
            "dread": self.dread.level,
            "programs": self.lights.descriptors(),
        }

    def restore(self, data: Any) -> None:
        """Restore from a :meth:`serialize` snapshot; tolerant of junk.

        The dread level is restored *silently* (no threshold crossings
        fire): the crossing hooks already ran in the pre-save timeline,
        and re-firing them would double-spend the rewound RNG stream.
        """
        if not isinstance(data, dict):
            return
        try:
            self.dread._set(float(data.get("dread", self.dread.level)),
                            fire=False)
        except (TypeError, ValueError):
            pass
        try:
            self.lights.load_descriptors(data.get("programs"))
        except Exception as exc:
            bd.warn(f"bd_horror: could not restore light programs: {exc!r}")

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
                bd.warn(f"bd_horror: could not serialize horror state: "
                        f"{exc!r}")

        @bd.on("load")
        def _on_load(event: Dict[str, Any]) -> None:
            try:
                state.load()
            except Exception as exc:
                bd.warn(f"bd_horror: could not restore horror state: {exc!r}")
