"""Registered world NPCs with disposition-aware talk targeting.

A mod describes each NPC once with :class:`NPCDefinition` (actor class,
spawn placement, tint, dialogue source, services) and hands the
definitions to :class:`NPCManager`. On ``map_load`` the manager spawns
every NPC friendly and still, optionally tinted and facing the player,
and after a savegame load it re-binds the restored world actors by TID
instead of duplicating them.

Talk targeting is range based: :meth:`NPCManager.nearest` picks the
live registered NPC closest to the player's pawn (within each
definition's ``talk_range``), :meth:`NPCManager.prompt` renders the
human-readable "[E] Speak to ..." line for the HUD, and
:meth:`NPCManager.begin_talk` opens a ``bd_dialogue.DialogueSession``
whose ctx carries the disposition keys (``disposition``, ``standing``,
``npc_id``, ``dispositions``) on top of the standard dialogue context.
One conversation at a time: ``begin_talk`` no-ops while another session
is active.

Definitions are never persisted. The manager persists only the
npc_id -> TID map (via :meth:`NPCManager.arm_persistence`) so a
savegame load can re-bind actors; starting disposition values are
written only for NPCs the store has never seen.
"""

from __future__ import annotations

import math
from typing import Any, Dict, List, Optional, Tuple

import biaseddoom as bd

from .disposition import Disposition

__all__ = ["NPCDefinition", "NPCManager", "PROMPT_FORMAT"]

#: Human-readable talk prompt template; ``{name}`` is the NPC's display
#: name and ``{standing}`` its current standing. Examples may restyle
#: the prompt by rebinding this module-level constant.
PROMPT_FORMAT = "[E] Speak to {name} ({standing})"


def _player_pawn() -> Any:
    """Live handle to the console player's pawn, or None."""
    try:
        player = bd.player(0)
        if player is None or not player.valid:
            return None
        pawn = player.actor
        if pawn is not None and pawn.valid:
            return pawn
    except Exception:
        pass
    return None


#: Cached default session subclass (built on first use so importing
#: bd_npcs never imports bd_dialogue at module import time).
_SESSION_CLASS: Optional[type] = None


def _disposition_session_class() -> type:
    """Build (once) the DialogueSession subclass injecting disposition ctx."""
    global _SESSION_CLASS
    if _SESSION_CLASS is not None:
        return _SESSION_CLASS
    import bd_dialogue

    class _DispositionSession(bd_dialogue.DialogueSession):
        """DialogueSession whose ctx also carries disposition data.

        Adds ``npc_id`` (str), ``dispositions`` (the store),
        ``disposition`` (int), and ``standing`` (str) on top of the
        standard :meth:`DialogueSession.context` keys, then merges the
        mod's ``extra_ctx`` mapping last so mod keys always win.
        """

        def __init__(self, dialogue: Any, npc_ref: Any,
                     dispositions: Any = None, npc_id: str = "",
                     extra_ctx: Optional[Dict[str, Any]] = None,
                     **kwargs: Any) -> None:
            super().__init__(dialogue, npc_ref, **kwargs)
            self._bd_dispositions: Any = dispositions
            self._bd_npc_id: str = str(npc_id)
            self._bd_extra_ctx: Dict[str, Any] = dict(extra_ctx or {})

        def context(self) -> Dict[str, Any]:
            ctx = super().context()
            ctx["npc_id"] = self._bd_npc_id
            ctx["dispositions"] = self._bd_dispositions
            if self._bd_dispositions is not None:
                ctx["disposition"] = self._bd_dispositions.get(
                    self._bd_npc_id)
                ctx["standing"] = self._bd_dispositions.standing(
                    self._bd_npc_id)
            ctx.update(self._bd_extra_ctx)
            return ctx

    _SESSION_CLASS = _DispositionSession
    return _SESSION_CLASS


class NPCDefinition:
    """Definition of one registered NPC (pure data plus its dialogue).

    - ``id``: unique key for registration, dispositions, and saves.
    - ``display_name``: human-readable name for prompts and UI.
    - ``actor_class``: engine actor class to spawn (e.g. ``"ZombieMan"``).
    - ``tint``: packed ``0xRRGGBB`` int applied as an ``(r, g, b)`` sprite
      tint, or None for the class default.
    - ``dialogue``: a ``bd_dialogue.Dialogue`` instance, a callable
      returning one, or None. ``dialogue_factory`` is an accepted alias;
      when both are given ``dialogue`` wins.
    - ``services``: iterable of :class:`bd_npcs.services.Service` objects
      the NPC offers (the manager stores them; the mod runs them).
    - ``start_disposition``: seed for the disposition store on first
      meeting; never overwrites a saved value.
    - ``talk_range``: max ``pawn.distance_to`` for targeting (192.0).
    - ``spawn_offset``: ``(dx, dy)`` from the player pawn at spawn time,
      or an absolute ``(x, y, z)`` when ``spawn_absolute`` is True.
    - ``tid_base``: when nonzero the manager assigns stable TIDs as
      ``tid_base + registration index`` so savegames can re-bind actors.
    - ``face_player``: rotate the actor toward the pawn after spawning.

    Empty ``id``/``display_name``/``actor_class`` raise ``ValueError``,
    as do a non-positive ``talk_range`` or a wrongly sized
    ``spawn_offset``.
    """

    def __init__(self, id: str, display_name: str,  # noqa: A002
                 actor_class: str, tint: Optional[int] = None,
                 dialogue: Any = None,
                 dialogue_factory: Any = None, services: Any = (),
                 start_disposition: int = 0, talk_range: float = 192.0,
                 spawn_offset: Any = (0.0, 0.0),
                 spawn_absolute: bool = False, tid_base: int = 0,
                 face_player: bool = True) -> None:
        for label, value in (("id", id), ("display_name", display_name),
                             ("actor_class", actor_class)):
            if not str(value or "").strip():
                raise ValueError(f"bd_npcs: NPCDefinition {label} must not "
                                 f"be empty")
        self.id: str = str(id)
        self.display_name: str = str(display_name)
        self.actor_class: str = str(actor_class)
        if tint is not None and not isinstance(tint, int):
            raise ValueError("bd_npcs: tint must be an int (packed "
                             "0xRRGGBB) or None")
        self.tint: Optional[int] = tint
        self.dialogue: Any = dialogue
        self.dialogue_factory: Any = dialogue_factory
        self.services: Tuple[Any, ...] = tuple(services or ())
        self.start_disposition: int = int(start_disposition)
        self.talk_range: float = float(talk_range)
        if self.talk_range <= 0.0:
            raise ValueError("bd_npcs: talk_range must be positive")
        self.spawn_absolute: bool = bool(spawn_absolute)
        offset = tuple(spawn_offset or ())
        wanted = 3 if self.spawn_absolute else 2
        if len(offset) != wanted:
            raise ValueError(f"bd_npcs: spawn_offset must have {wanted} "
                             f"component(s) when spawn_absolute is "
                             f"{self.spawn_absolute}")
        self.spawn_offset: Tuple[float, ...] = tuple(
            float(component) for component in offset)
        self.tid_base: int = int(tid_base)
        if self.tid_base < 0:
            raise ValueError("bd_npcs: tid_base must not be negative")
        self.face_player: bool = bool(face_player)

    @property
    def has_dialogue(self) -> bool:
        """True when a dialogue source (instance or factory) is set."""
        return self.dialogue is not None or self.dialogue_factory is not None

    def build_dialogue(self) -> Any:
        """Resolve the dialogue: call the factory or return the instance."""
        source = (self.dialogue if self.dialogue is not None
                  else self.dialogue_factory)
        if source is None:
            return None
        if callable(source):
            return source()
        return source

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return (f"<NPCDefinition {self.id!r} class={self.actor_class!r} "
                f"services={len(self.services)}>")


class NPCManager:
    """Registry and world binder for :class:`NPCDefinition` entries.

    ``dispositions`` is the :class:`~bd_npcs.disposition.Disposition`
    store (a fresh one is created when omitted). Register definitions at
    import or ``engine_start`` time; call :meth:`spawn_all` from a
    ``map_load`` handler, passing ``event.get("from_savegame", False)``:
    on a savegame load the manager first tries ``bd.actor_ref(tid)``
    with the persisted TID, so the restored world actor is adopted
    instead of duplicated.

    :meth:`arm_persistence` registers the ``save``/``load`` handlers
    exactly once, round-tripping the npc_id -> TID map through
    ``bd.state[dispositions.state_key]["tids"]`` (the manager shares the
    store's state key so everything lands in one bucket). Definitions
    and actor handles are never persisted.

    :meth:`retire` takes an NPC off duty (the recruit who joins the
    party as a follower): the actor leaves the world, the manager stops
    tracking and respawning it, and the retired set round-trips through
    the same state bucket. Standings survive retirement.
    """

    def __init__(self, dispositions: Optional[Disposition] = None) -> None:
        self.dispositions: Disposition = (dispositions if dispositions
                                          is not None else Disposition())
        self._defs: Dict[str, NPCDefinition] = {}
        self._handles: Dict[str, Any] = {}
        self._tids: Dict[str, int] = {}
        self._retired: set = set()
        self._persistence_armed: bool = False

    # -- registration ------------------------------------------------------------

    @property
    def definitions(self) -> Tuple[NPCDefinition, ...]:
        """All registered definitions, in registration order."""
        return tuple(self._defs.values())

    @property
    def npc_ids(self) -> Tuple[str, ...]:
        """The registered NPC ids, in registration order."""
        return tuple(self._defs.keys())

    def register(self, definition: NPCDefinition) -> NPCDefinition:
        """Register a definition; a duplicate id warns and replaces."""
        if not isinstance(definition, NPCDefinition):
            raise ValueError(f"bd_npcs: register needs an NPCDefinition, "
                             f"got {definition!r}")
        if definition.id in self._defs:
            bd.warn(f"bd_npcs: NPC {definition.id!r} already registered; "
                    f"replacing the previous definition")
        self._defs[definition.id] = definition
        return definition

    def definition(self, npc_id: str) -> Optional[NPCDefinition]:
        """The definition for ``npc_id``, or None."""
        return self._defs.get(str(npc_id))

    # -- retirement (an NPC leaves duty) ------------------------------------------

    @property
    def retired(self) -> Tuple[str, ...]:
        """The retired NPC ids, in sorted order."""
        return tuple(sorted(self._retired))

    def is_retired(self, npc_id: str) -> bool:
        """True when ``npc_id`` has been retired."""
        return str(npc_id) in self._retired

    def retire(self, npc_id: str, destroy_actor: bool = True) -> bool:
        """Take a live NPC off duty: gone from the world, not forgotten.

        The actor is destroyed (default) or simply released
        (``destroy_actor=False``, for a mod adopting the actor itself,
        e.g. turning a recruit into a follower), the handle and the
        persisted TID are dropped so :meth:`nearest`, :meth:`prompt`,
        and the savegame rebind stop tracking it, and later
        :meth:`spawn_all` calls skip the definition, on this map and
        after a checkpoint load (the retired set round-trips through the
        same ``bd.state`` bucket as the TIDs). The disposition standing
        is kept: a companion who joined you still has its standing.
        Returns True when the id was registered (known), False
        otherwise.
        """
        npc_id = str(npc_id)
        if npc_id not in self._defs:
            return False
        handle = self.actor_for(npc_id)
        if destroy_actor and handle is not None:
            try:
                handle.destroy()
            except Exception as exc:
                bd.warn(f"bd_npcs: could not destroy retired NPC "
                        f"{npc_id!r}: {exc!r}")
        self._handles.pop(npc_id, None)
        self._tids.pop(npc_id, None)
        self._retired.add(npc_id)
        return True

    # -- spawning ------------------------------------------------------------------

    def spawn_all(self, from_savegame: bool = False) -> List[Any]:
        """Spawn every registered NPC; returns the live actor handles.

        With ``from_savegame`` a stored TID mapping is tried first:
        ``bd.actor_ref(tid)`` adopts the actor the savegame restored
        instead of spawning a duplicate. Otherwise (and on any failure,
        which only warns) the NPC is spawned via ``bd.spawn`` at the
        definition's position: absolute, or the player pawn plus the
        offset (falling back to the offset from the map origin with a
        warning when no pawn exists). Every spawned actor is flagged
        FRIENDLY and STANDSTILL, its speed zeroed, optionally tinted and
        rotated toward the player. The definition's ``start_disposition``
        seeds the store only when the NPC id is unknown to it, so loaded
        saves keep their standings.
        """
        spawned: List[Any] = []
        pawn = _player_pawn()
        if pawn is None:
            bd.warn("bd_npcs: no player pawn; NPCs spawn at their "
                    "offsets from the map origin")
        for index, definition in enumerate(self._defs.values()):
            if definition.id in self._retired:
                continue  # off duty (e.g. recruited away): never respawns
            handle = None
            if from_savegame:
                handle = self._rebind(definition)
            if handle is None:
                handle = self._spawn_one(definition, index, pawn)
            if handle is None:
                continue  # spawn failure already warned; never raise here
            self._finish_spawn(definition, handle, pawn)
            self._handles[definition.id] = handle
            try:
                tid = int(handle.tid)
            except Exception:
                tid = 0
            if tid:
                self._tids[definition.id] = tid
            else:
                self._tids.pop(definition.id, None)
            if not self.dispositions.has(definition.id):
                self.dispositions.set(definition.id,
                                      definition.start_disposition)
            spawned.append(handle)
        return spawned

    def _rebind(self, definition: NPCDefinition) -> Any:
        """Adopt the savegame-restored actor for a definition, or None."""
        tid = self._tids.get(definition.id)
        if not tid:
            return None
        try:
            ref = bd.actor_ref(tid)
        except Exception as exc:
            bd.warn(f"bd_npcs: could not re-resolve TID {tid} for "
                    f"{definition.id!r}: {exc!r}")
            return None
        if ref is None:
            return None
        try:
            if ref.valid and ref.alive:
                return ref
        except Exception:
            return None
        return None

    def _spawn_one(self, definition: NPCDefinition, index: int,
                   pawn: Any) -> Any:
        """Spawn one NPC actor; returns the handle or None (warned)."""
        if definition.spawn_absolute:
            x, y, z = definition.spawn_offset
        elif pawn is not None:
            dx, dy = definition.spawn_offset
            x, y, z = pawn.x + dx, pawn.y + dy, pawn.z
        else:
            dx, dy = definition.spawn_offset
            x, y, z = dx, dy, 0.0
        tid = (definition.tid_base + index) if definition.tid_base else 0
        try:
            return bd.spawn(definition.actor_class, float(x), float(y),
                            float(z), angle=0.0, tid=tid, force=True)
        except Exception as exc:
            bd.warn(f"bd_npcs: spawn of {definition.id!r} "
                    f"({definition.actor_class}) failed: {exc!r}")
            return None

    def _finish_spawn(self, definition: NPCDefinition, handle: Any,
                      pawn: Any) -> None:
        """Apply the shared post-spawn treatment (flags, tint, facing)."""
        for flag, value in (("FRIENDLY", True), ("STANDSTILL", True)):
            try:
                handle.set_flag(flag, value)
            except Exception as exc:
                bd.warn(f"bd_npcs: flag {flag} not applied on "
                        f"{definition.id!r}: {exc!r}")
        try:
            handle.speed = 0.0
        except Exception as exc:
            bd.warn(f"bd_npcs: speed not zeroed on {definition.id!r}: "
                    f"{exc!r}")
        if definition.tint is not None:
            try:
                packed = int(definition.tint)
                handle.tint = ((packed >> 16) & 255, (packed >> 8) & 255,
                               packed & 255)
            except Exception as exc:
                bd.warn(f"bd_npcs: tint not applied on {definition.id!r}: "
                        f"{exc!r}")
        if definition.face_player and pawn is not None:
            try:
                handle.angle = math.degrees(
                    math.atan2(pawn.y - handle.y, pawn.x - handle.x)) \
                    % 360.0
            except Exception as exc:
                bd.warn(f"bd_npcs: {definition.id!r} could not face the "
                        f"player: {exc!r}")

    # -- targeting -----------------------------------------------------------------

    def actor_for(self, npc_id: str) -> Any:
        """Live actor handle for ``npc_id``, or None.

        The stored handle is returned while it stays valid and alive;
        otherwise the persisted TID is re-resolved through
        ``bd.actor_ref``. Dead or unresolvable handles are pruned
        lazily here.
        """
        npc_id = str(npc_id)
        stale = False
        handle = self._handles.get(npc_id)
        if handle is not None:
            try:
                if handle.valid and handle.alive:
                    return handle
                stale = True
            except Exception:
                stale = True
        tid = self._tids.get(npc_id)
        if tid:
            ref = None
            try:
                ref = bd.actor_ref(tid)
            except Exception:
                ref = None
            if ref is not None:
                try:
                    if ref.valid and ref.alive:
                        self._handles[npc_id] = ref
                        return ref
                except Exception:
                    pass
        if stale:
            self._handles.pop(npc_id, None)
        return None

    def nearest(self, pawn: Any, max_range: Any = None
                ) -> Tuple[Optional[NPCDefinition], Any]:
        """Nearest live NPC to ``pawn`` within talk range.

        Returns ``(definition, handle)``, or ``(None, None)`` when no
        registered NPC is in range. Range is ``definition.talk_range``
        unless ``max_range`` overrides it. Invalid or dead handles are
        excluded (and pruned lazily through :meth:`actor_for`).
        """
        return self._nearest(pawn, max_range)

    def _nearest(self, pawn: Any, max_range: Any = None,
                 require_dialogue: bool = False
                 ) -> Tuple[Optional[NPCDefinition], Any]:
        best: Optional[Tuple[NPCDefinition, Any]] = None
        best_distance = 0.0
        for definition in self._defs.values():
            if require_dialogue and not definition.has_dialogue:
                continue
            handle = self.actor_for(definition.id)
            if handle is None:
                continue
            try:
                distance = float(pawn.distance_to(handle))
            except Exception:
                continue
            limit = (float(max_range) if max_range is not None
                     else definition.talk_range)
            if distance > limit:
                continue
            if best is None or distance < best_distance:
                best, best_distance = (definition, handle), distance
        if best is None:
            return None, None
        return best

    # -- talking -------------------------------------------------------------------

    def prompt(self, pawn: Any) -> Optional[str]:
        """Human-readable talk prompt for the nearest NPC, or None.

        Uses :meth:`nearest` and :data:`PROMPT_FORMAT`; returns None
        when nobody registered is in range.
        """
        definition, _handle = self.nearest(pawn)
        if definition is None:
            return None
        standing = self.dispositions.standing(definition.id)
        return PROMPT_FORMAT.format(name=definition.display_name,
                                    standing=standing)

    def begin_talk(self, pawn: Any, character: Any = None,
                   factions: Any = None, quest_log: Any = None,
                   extra_ctx: Optional[Dict[str, Any]] = None,
                   session_class: Optional[type] = None) -> Any:
        """Start a dialogue session with the nearest talkable NPC.

        Picks the nearest registered NPC carrying a dialogue within
        talk range, builds its dialogue (calling a factory or using the
        instance), and starts a ``bd_dialogue.DialogueSession``. By
        default the session is a disposition-injecting subclass whose
        ``context()`` adds ``disposition`` (int), ``standing`` (str),
        ``npc_id``, and ``dispositions`` (the store), then merges
        ``extra_ctx``; pass ``session_class`` to take over ctx
        construction entirely (``extra_ctx`` is then ignored).

        One conversation at a time: returns None when another session is
        active, when nobody talkable is in range, or when ``start()``
        fails. A dialogue that fails validation raises ``ValueError``
        (a mod bug, reported as-is); everything else only warns.
        """
        import bd_dialogue  # late: keeps bd_npcs import-time dependency-free
        try:
            if bd_dialogue.active_session() is not None:
                return None
        except Exception as exc:
            bd.warn(f"bd_npcs: could not query the active dialogue "
                    f"session: {exc!r}")
            return None
        definition, handle = self._nearest(pawn, require_dialogue=True)
        if definition is None:
            return None
        dialogue = definition.build_dialogue()
        if dialogue is None:
            return None
        cls = (session_class if session_class is not None
               else _disposition_session_class())
        try:
            if session_class is None:
                session = cls(dialogue, handle, character=character,
                              factions=factions, quest_log=quest_log,
                              dispositions=self.dispositions,
                              npc_id=definition.id, extra_ctx=extra_ctx)
            else:
                session = cls(dialogue, handle, character=character,
                              factions=factions, quest_log=quest_log)
        except ValueError:
            raise  # broken dialogue tree: a mod bug, reported as-is
        except Exception as exc:
            bd.warn(f"bd_npcs: could not build the dialogue session for "
                    f"{definition.id!r}: {exc!r}")
            return None
        try:
            started = session.start()
        except Exception as exc:
            bd.warn(f"bd_npcs: starting the dialogue with "
                    f"{definition.id!r} failed: {exc!r}")
            return None
        if not started:
            return None
        return session

    def end_talk(self) -> None:
        """End the active dialogue session, if any (guarded no-op)."""
        try:
            import bd_dialogue
            session = bd_dialogue.active_session()
        except Exception as exc:
            bd.warn(f"bd_npcs: could not query the active dialogue "
                    f"session: {exc!r}")
            return
        if session is None:
            return
        try:
            session.end()
        except Exception as exc:
            bd.warn(f"bd_npcs: could not end the dialogue session: "
                    f"{exc!r}")

    # -- persistence ---------------------------------------------------------------

    def _bucket(self) -> Dict[str, Any]:
        key = self.dispositions.state_key
        bucket = bd.state.get(key)
        if not isinstance(bucket, dict):
            bucket = {}
            bd.state[key] = bucket
        return bucket

    def arm_persistence(self) -> None:
        """Register the save/load handlers exactly once per manager.

        The npc_id -> TID map round-trips through
        ``bd.state[dispositions.state_key]["tids"]`` so a savegame load
        can re-bind the restored actors (see :meth:`spawn_all`).
        """
        if self._persistence_armed:
            return
        self._persistence_armed = True
        manager = self

        @bd.on("save")
        def _on_save(event: Dict[str, Any]) -> None:
            try:
                manager._save_tids()
            except Exception as exc:
                bd.warn(f"bd_npcs: could not serialize NPC tids: {exc!r}")

        @bd.on("load")
        def _on_load(event: Dict[str, Any]) -> None:
            try:
                manager._load_tids()
            except Exception as exc:
                bd.warn(f"bd_npcs: could not restore NPC tids: {exc!r}")

    def _save_tids(self) -> None:
        bucket = self._bucket()
        bucket["tids"] = {npc_id: int(tid)
                          for npc_id, tid in self._tids.items()
                          if tid}
        bucket["retired"] = sorted(self._retired)

    def _load_tids(self) -> None:
        bucket = bd.state.get(self.dispositions.state_key)
        data = bucket.get("tids") if isinstance(bucket, dict) else None
        if isinstance(data, dict):
            tids: Dict[str, int] = {}
            for npc_id, tid in data.items():
                try:
                    tid = int(tid)
                except (TypeError, ValueError):
                    continue
                if tid > 0:
                    tids[str(npc_id)] = tid
            self._tids = tids
        retired = bucket.get("retired") if isinstance(bucket, dict) else None
        if isinstance(retired, (list, tuple)):
            self._retired = {str(npc_id) for npc_id in retired}

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return (f"<NPCManager npcs={len(self._defs)} "
                f"tids={len(self._tids)}>")
