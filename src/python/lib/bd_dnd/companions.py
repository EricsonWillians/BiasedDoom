"""World-bound party companions for :mod:`bd_dnd`.

:class:`Companion` binds a :class:`~bd_dnd.Party` member to a follower
actor in the world. The companion actor shadows the player, fights
alongside them, and its actor health *is* the member's RPG hit points —
the two pools are kept in sync in both directions::

    import biaseddoom as bd
    import bd_dnd

    party = bd_dnd.Party([fighter, mage], name="The Delvers")
    party_state = bd_dnd.PartyState(party)

    sage_guardian = bd_dnd.Companion("Mage", class_name="DoomImp")
    party_state.add_companion(sage_guardian)   # persist it with the party

    @bd.on("map_load")
    def begin(event):
        sage_guardian.bind(party)              # spawn near the player
        party_state.arm_persistence()          # save/load round-trip

Behaviour summary:

- **Follow AI.** A deterministic scheduler loop (every
  :attr:`Companion.TICK_INTERVAL` tics) keeps the actor near the player
  pawn: beyond ``teleport_distance`` it teleports to the player's side;
  beyond ``follow_distance`` it thrusts toward the player at a capped
  speed and faces them.
- **Combat.** The engine's Actor ``target``/``master`` getsets are
  writable, so combat is real: the companion tracks the player's recent
  attackers (``actor_damaged`` on the player pawn, remembered for
  :attr:`Companion.ATTACKER_MEMORY_TICS`), assigns the freshest living
  one as its ``target``, and nudges the native chase AI with
  ``set_state("See")``. From there the actor class's own A_Chase /
  missile logic does the fighting — accuracy, reaction time, and target
  retention belong to the monster class, not to this module. The actor
  is flagged ``FRIENDLY`` (never hunts players) and ``COUNTKILL`` is
  cleared (never pollutes the kill count); its ``master`` is the player
  pawn.
- **Health sync.** ``actor_damaged`` on the companion actor lowers the
  member's ``hp`` (clamped); member-side changes through
  :meth:`Character.set_hp`/:meth:`~Character.rest`/:meth:`~Character.level_up`
  fire ``Character.on_hp_changed`` and heal/wound the actor to match.
  Direct ``member.hp = ...`` writes bypass the hook — use ``set_hp``.
  Boundary: heals applied straight to the *actor* (including
  :class:`~bd_dnd.DamageSaveRule` refunds) do not raise ``member.hp``;
  keep companion healing on the RPG side so both pools move together.
- **Death and revival.** When the companion actor dies the member is
  incapacitated (``hp`` 0) and ``on_companion_died`` fires;
  :meth:`Companion.revive` respawns the actor near the player at half
  the member's max HP (rounded up). A dead companion never respawns on
  its own.
- **Persistence.** Only plain data is serialized (member name, actor
  class, follow distances, current TID, dead flag) — never Actor
  handles. After a checkpoint load the companion re-binds to the
  restored world actor by the TID it had at save time; when the TID is
  gone (destroyed corpse, hub transition) it respawns near the player.
  A fresh TID is allocated on every (re)spawn so a lingering corpse
  never shadows the living actor. Register companions on the
  :class:`~bd_dnd.PartyState` (``add_companion``) for the round-trip.

All randomness stays with the engine's deterministic script RNG — the
follow loop itself rolls no dice. Every engine call is defensive: a
failure warns through ``bd.warn`` and never raises out of a handler.
"""

from __future__ import annotations

import math
from typing import Any, Callable, Dict, List, Optional

import biaseddoom as bd

from . import Character, Party, _fire, _is_player_actor, _level_time

_INT = int


class Companion:
    """Bind one party member to a follower actor in the world.

    ``member_name`` must name a member of the :class:`~bd_dnd.Party`
    passed to :meth:`bind` (validated there, so the companion may be
    constructed before the party exists). ``class_name`` is any
    shootable monster class (default ``"DoomImp"``); ``follow_distance``
    / ``teleport_distance`` are world units.

    Callbacks: ``on_companion_died`` fires with ``(companion, member)``
    when the actor dies. Inspect :attr:`dead`, :attr:`bound`, and
    :attr:`tid` for status; :meth:`actor` resolves the live handle.
    """

    #: Follow-loop cadence in tics (~3.5 Hz at 35 tics/second).
    TICK_INTERVAL: int = 10
    #: First TID tried when auto-allocating (kept clear of map/scripts
    #: conventions like the 9000-range used by the bundled examples).
    TID_BASE: int = 31000
    #: Maximum horizontal speed (map units per tic) while following.
    FOLLOW_SPEED: float = 8.0
    #: How long a player attacker stays a valid companion target.
    ATTACKER_MEMORY_TICS: int = 350
    #: Spawn/catch-up lateral offset from the player pawn, in units.
    SPAWN_OFFSET: float = 48.0

    def __init__(self, member_name: str, class_name: str = "DoomImp",
                 follow_distance: float = 96.0,
                 teleport_distance: float = 512.0,
                 tid: Optional[int] = None) -> None:
        self.member_name: str = str(member_name)
        self.class_name: str = str(class_name)
        self.follow_distance: float = float(follow_distance)
        self.teleport_distance: float = float(teleport_distance)
        if self.follow_distance <= 0.0:
            raise ValueError("follow_distance must be positive")
        if self.teleport_distance <= self.follow_distance:
            raise ValueError("teleport_distance must exceed follow_distance")
        self.tid: Optional[int] = (_INT(tid) if tid else None)
        self.party: Optional[Party] = None
        self.member: Optional[Character] = None
        self.bound: bool = False
        self.dead: bool = False
        self.on_companion_died: List[Callable[["Companion", Character],
                                              None]] = []
        self._task: Optional[int] = None
        self._syncing: bool = False
        self._handlers_registered: bool = False
        # (attacker handle, expiry tic) — freshest last; handles are
        # validated each tick, never persisted.
        self._attackers: List[Any] = []
        self._target_tid: int = 0  # last target we assigned (dedupe)

    # -- player / actor resolution ---------------------------------------------

    @staticmethod
    def _player_pawn() -> Any:
        """Live handle to the local player's pawn, or None."""
        try:
            player = bd.player(0)
            if player is None or not player.valid:
                return None
            pawn = player.actor
            return pawn if pawn is not None and pawn.valid else None
        except Exception:
            return None

    def actor(self) -> Any:
        """Resolve the live Actor handle for the current TID, or None."""
        if not self.tid:
            return None
        try:
            ref = bd.actor_ref(self.tid)
            if ref is not None and ref.valid:
                return ref
        except Exception:
            pass
        return None

    def _is_own_actor(self, ref: Any) -> bool:
        """True when the handle/snapshot names this companion's actor."""
        if ref is None or not self.tid:
            return False
        try:
            return _INT(ref.tid) == self.tid
        except Exception:
            return False

    # -- binding ---------------------------------------------------------------

    def bind(self, party: Party, spawn_near_player: bool = True) -> bool:
        """Attach to ``party`` and spawn the follower actor.

        ``member_name`` must exist in the party (``ValueError``
        otherwise). With ``spawn_near_player`` (default) the actor
        spawns at the player pawn plus a small offset when a map is
        live; off-map the spawn is deferred to the next ``map_load``.
        Returns True when the actor is in the world after the call.

        Re-binding is safe: event handlers register once, the follow
        loop re-arms, and an existing live actor is adopted rather than
        duplicated. Because the engine offers no event unregistration,
        the handlers stay registered for the session and no-op once the
        companion is disbanded.
        """
        member = party.get(self.member_name)
        if member is None:
            raise ValueError(f"bd_dnd: no party member named "
                             f"{self.member_name!r}")
        self.party = party
        if self.member is not member:
            self.member = member
            member.on_hp_changed.append(self._on_member_hp_changed)
        self.bound = True
        self._register_handlers()
        self._arm_follow_loop()
        spawned = False
        if spawn_near_player:
            existing = self.actor()
            if existing is not None and existing.alive and not self.dead:
                self._adopt(existing)  # re-bind, don't duplicate
                spawned = True
            else:
                spawned = self._spawn_near_player()
        return spawned

    def disband(self) -> None:
        """Stop following (the actor stays in the world, flags kept)."""
        self.bound = False
        if self._task is not None:
            try:
                bd.cancel_task(self._task)
            except Exception:
                pass
            self._task = None

    def _register_handlers(self) -> None:
        """Register the engine event handlers exactly once."""
        if self._handlers_registered:
            return
        self._handlers_registered = True

        @bd.on("map_load")
        def _on_map_load(event: Dict[str, Any]) -> None:
            if self.bound:
                self._on_world_ready()

        @bd.on("actor_damaged")
        def _on_actor_damaged(event: Dict[str, Any]) -> None:
            if self.bound:
                self._on_damaged(event)

        @bd.on("actor_died")
        def _on_actor_died(event: Dict[str, Any]) -> None:
            if self.bound:
                self._on_died(event)

    def _arm_follow_loop(self) -> None:
        """(Re)arm the repeating follow task (idempotent)."""
        if self._task is not None:
            try:
                bd.cancel_task(self._task)
            except Exception:
                pass
            self._task = None
        try:
            self._task = bd.schedule(self._follow_tick,
                                     delay=self.TICK_INTERVAL,
                                     repeat=self.TICK_INTERVAL,
                                     map_local=False)
        except Exception as exc:
            bd.warn(f"bd_dnd: could not schedule companion follow loop: "
                    f"{exc!r}")
            self._task = None

    # -- spawning / adoption -----------------------------------------------------

    def _allocate_tid(self) -> int:
        """Pick a free TID, preferring the persisted/current one."""
        candidate = self.tid or self.TID_BASE
        try:
            guard = 0
            while bd.actor_ref(candidate) is not None and guard < 1000:
                candidate += 1
                guard += 1
        except Exception:
            pass  # off-map: trust the candidate
        self.tid = candidate
        return candidate

    def _spawn_at(self, x: float, y: float, z: float, angle: float = 0.0
                  ) -> Any:
        """Spawn the actor with a fresh TID; returns the handle or None."""
        tid = self._allocate_tid()
        try:
            ref = bd.spawn(self.class_name, float(x), float(y), float(z),
                           angle=float(angle), tid=tid, force=True)
        except Exception as exc:
            bd.warn(f"bd_dnd: companion spawn ({self.class_name}) failed: "
                    f"{exc!r}")
            return None
        self._adopt(ref)
        return ref

    def _spawn_near_player(self) -> bool:
        """Spawn beside the player pawn; False when off-map / no pawn."""
        pawn = self._player_pawn()
        if pawn is None:
            return False
        try:
            ref = self._spawn_at(pawn.x + self.SPAWN_OFFSET, pawn.y, pawn.z,
                                 angle=pawn.angle)
        except Exception as exc:
            bd.warn(f"bd_dnd: companion spawn position failed: {exc!r}")
            return False
        return ref is not None

    def _adopt(self, ref: Any) -> None:
        """Apply companion semantics to a (fresh or restored) actor."""
        if ref is None:
            return
        try:
            tid = _INT(ref.tid)
            if tid:
                self.tid = tid
        except Exception:
            pass
        for flag, value in (("FRIENDLY", True), ("COUNTKILL", False)):
            try:
                ref.set_flag(flag, value)
            except Exception as exc:
                bd.warn(f"bd_dnd: companion flag {flag} not applied: {exc!r}")
        pawn = self._player_pawn()
        if pawn is not None:
            try:
                ref.master = pawn  # friendly AI treats the pawn as owner
            except Exception as exc:
                bd.warn(f"bd_dnd: companion master not assigned: {exc!r}")
        self._sync_actor_health(ref)
        self.dead = False

    def _sync_actor_health(self, ref: Any) -> None:
        """Force the actor's health to the member's RPG hp (no events)."""
        member = self.member
        if member is None or ref is None:
            return
        self._syncing = True
        try:
            if _INT(ref.health) != member.hp:
                ref.health = member.hp
        except Exception as exc:
            bd.warn(f"bd_dnd: companion health sync failed: {exc!r}")
        finally:
            self._syncing = False

    def _on_world_ready(self) -> None:
        """map_load: re-bind by TID, or respawn when the actor is gone.

        A checkpoint load restores the world — including the companion
        actor and its TID — so the common case is plain adoption. A lost
        TID (destroyed corpse, hub hop) respawns near the player unless
        the companion is marked dead (then it waits for revive()).
        """
        self._arm_follow_loop()  # repeat tasks are not savegame state
        ref = self.actor()
        if ref is not None:
            if ref.alive:
                self._adopt(ref)
            else:
                self.dead = True  # the corpse came back; still dead
            return
        if not self.dead and self.member is not None and self.member.hp > 0:
            self._spawn_near_player()

    # -- follow loop ---------------------------------------------------------------

    def _follow_tick(self) -> None:
        """One follow-AI step; scheduled every TICK_INTERVAL tics."""
        if not self.bound or self.dead:
            return
        try:
            if bd.current_map() is None:
                return
        except Exception:
            return
        pawn = self._player_pawn()
        if pawn is None:
            return
        ref = self.actor()
        if ref is None:
            self._on_world_ready()  # invalid actor: rebind or respawn
            return
        if not ref.alive:
            return  # death handler marks us dead; tolerate the gap
        self._update_target(ref, pawn)
        try:
            distance = ref.distance_to(pawn)
        except Exception:
            return
        if distance > self.teleport_distance:
            try:
                ref.set_position(pawn.x + self.SPAWN_OFFSET, pawn.y, pawn.z,
                                 check=False, fog=True)
                ref.set_velocity(0.0, 0.0, 0.0)
            except Exception as exc:
                bd.warn(f"bd_dnd: companion catch-up teleport failed: {exc!r}")
        elif distance > self.follow_distance:
            try:
                dx, dy = pawn.x - ref.x, pawn.y - ref.y
                planar = math.hypot(dx, dy)
                if planar > 1.0:
                    speed = min(self.FOLLOW_SPEED,
                                max(1.0, (planar - self.follow_distance) / 4.0))
                    ref.set_velocity(dx / planar * speed,
                                     dy / planar * speed,
                                     ref.velocity_z)
                    ref.angle = math.degrees(math.atan2(dy, dx)) % 360.0
            except Exception as exc:
                bd.warn(f"bd_dnd: companion follow thrust failed: {exc!r}")

    # -- combat ------------------------------------------------------------------

    def _update_target(self, ref: Any, pawn: Any) -> None:
        """Point the companion's native AI at the player's attacker.

        ``Actor.target`` is writable, so this is a real target
        assignment: the freshest still-living recent attacker becomes
        the target and a ``See``-state nudge (only on target change, so
        attack animations are never restarted) hands control to the
        class's own chase/missile brain.
        """
        now = _level_time()
        kept = []
        for handle, expiry in self._attackers:
            try:
                if expiry >= now and handle is not None and handle.valid \
                        and handle.alive:
                    kept.append((handle, expiry))
            except Exception:
                continue
        self._attackers = kept[-8:]
        attacker = self._attackers[-1][0] if self._attackers else None
        try:
            current = ref.target
            current_tid = _INT(current.tid) if current is not None else 0
        except Exception:
            current, current_tid = None, 0
        try:
            new_tid = _INT(attacker.tid) if attacker is not None else 0
        except Exception:
            attacker, new_tid = None, 0
        if attacker is None:
            if current is not None:
                try:
                    ref.target = None  # stand down; resume following
                except Exception:
                    pass
            self._target_tid = 0
            return
        # Handles are fresh objects per call: compare by TID, falling
        # back to identity for TID-less monsters (same handle object is
        # what we stored, so `is` works for the common repeat case).
        same = (current is attacker) or \
               (new_tid != 0 and current_tid == new_tid)
        if same:
            return
        try:
            ref.target = attacker
        except Exception as exc:
            bd.warn(f"bd_dnd: companion target assignment failed: {exc!r}")
            return
        self._target_tid = new_tid
        try:
            ref.set_state("See")  # nudge the native chase AI
        except Exception:
            pass  # class without a See state: stays defensive only

    # -- health sync ---------------------------------------------------------------

    def _on_damaged(self, event: Dict[str, Any]) -> None:
        """actor_damaged: player attackers tracked; own damage -> member hp."""
        victim = event.get("actor_ref")
        if _is_player_actor(victim):
            source = event.get("source_ref")
            try:
                usable = source is not None and source.valid \
                    and source.alive and not _is_player_actor(source) \
                    and not self._is_own_actor(source)
            except Exception:
                usable = False
            if usable:
                self._attackers.append(
                    (source, _level_time() + self.ATTACKER_MEMORY_TICS))
                self._attackers = self._attackers[-8:]
            return
        if self._syncing or self.dead or self.member is None:
            return
        if not self._is_own_actor(victim):
            return
        try:
            damage = _INT(event.get("damage") or 0)
        except (TypeError, ValueError):
            return
        if damage <= 0:
            return
        self._syncing = True
        try:
            self.member.set_hp(self.member.hp - damage)
        finally:
            self._syncing = False

    def _on_member_hp_changed(self, member: Character) -> None:
        """Member-side hp change (set_hp/rest/level_up) -> actor health."""
        if self._syncing or self.dead:
            return
        ref = self.actor()
        if ref is None or not ref.alive:
            return
        try:
            actor_hp = _INT(ref.health)
        except Exception:
            return
        self._syncing = True
        try:
            if actor_hp < member.hp:
                ref.heal(member.hp - actor_hp, maximum=member.max_hp)
            elif actor_hp > member.hp:
                ref.damage(actor_hp - member.hp)
        except Exception as exc:
            bd.warn(f"bd_dnd: companion hp sync to actor failed: {exc!r}")
        finally:
            self._syncing = False

    # -- death and revival ---------------------------------------------------------

    def _on_died(self, event: Dict[str, Any]) -> None:
        """actor_died: the companion's death incapacitates the member."""
        if self.dead or self.member is None:
            return
        matched = False
        ref = event.get("actor_ref")
        if self._is_own_actor(ref):
            matched = True
        else:
            snapshot = event.get("actor")
            if isinstance(snapshot, dict) and self.tid:
                try:
                    matched = _INT(snapshot.get("tid") or 0) == self.tid
                except (TypeError, ValueError):
                    matched = False
        if not matched:
            return
        self.dead = True
        self._attackers = []
        self._syncing = True
        try:
            self.member.set_hp(0)
        finally:
            self._syncing = False
        _fire(self.on_companion_died, self, self.member)
        try:
            _announce(f"{self.member.name}'s companion has fallen!")
        except Exception:
            pass

    def revive(self) -> bool:
        """Respawn the companion near the player at half the member's max HP.

        Sets the member's hp first (rounded-up half, minimum 1 — the
        on_hp_changed hook finds no live actor and no-ops), then spawns
        a fresh actor with a new TID and syncs its health. Returns True
        when the actor is back in the world.
        """
        member = self.member
        if member is None:
            return False
        self.dead = False
        member.set_hp(max(1, (member.max_hp + 1) // 2))
        self.tid = None  # never share a TID with the old corpse
        return self._spawn_near_player()

    # -- persistence ---------------------------------------------------------------

    def serialize(self) -> Dict[str, Any]:
        """Plain JSON-able descriptor (no Actor handles, no callables)."""
        return {
            "member_name": self.member_name,
            "class_name": self.class_name,
            "follow_distance": self.follow_distance,
            "teleport_distance": self.teleport_distance,
            "tid": self.tid or 0,
            "dead": bool(self.dead),
        }

    def restore(self, data: Any) -> None:
        """Re-read a :meth:`serialize` descriptor; tolerant of junk.

        Only the descriptor is restored here — the actual world re-bind
        happens on the next ``map_load`` (the checkpoint's world is not
        resolvable mid-``load``-event), which adopts the restored actor
        by TID or respawns near the player when the TID is gone.
        """
        if not isinstance(data, dict):
            return
        if str(data.get("member_name", self.member_name)) != self.member_name:
            return
        try:
            self.class_name = str(data.get("class_name", self.class_name))
            self.follow_distance = float(
                data.get("follow_distance", self.follow_distance))
            self.teleport_distance = float(
                data.get("teleport_distance", self.teleport_distance))
            saved_tid = _INT(data.get("tid") or 0)
            if saved_tid:
                self.tid = saved_tid
            self.dead = bool(data.get("dead", self.dead))
        except (TypeError, ValueError) as exc:
            bd.warn(f"bd_dnd: companion descriptor partially restored: "
                    f"{exc!r}")

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        state = "dead" if self.dead else ("bound" if self.bound else "unbound")
        return (f"<Companion {self.member_name!r} {self.class_name} "
                f"tid={self.tid} {state}>")


def _announce(message: str) -> None:
    """Center-screen message that degrades to a log line when off-map."""
    try:
        bd.center_message(message)
    except Exception:
        bd.log(f"bd_dnd: {message}")
