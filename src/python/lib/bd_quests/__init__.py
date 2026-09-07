"""Data-driven quest framework for BiasedDoom Python mods.

``bd_quests`` is an engine-shipped, dependency-free package (it only needs
``biaseddoom``) that gives mods a classic RPG quest journal:

- **Data-driven definitions.** A :class:`Quest` is a named bundle of
  :class:`Objective` entries plus optional rewards and lifecycle hooks.
- **Engine-event auto-wiring.** :class:`QuestLog` tracker helpers
  (:meth:`~QuestLog.track_kills`, :meth:`~QuestLog.track_pickup`,
  :meth:`~QuestLog.track_sector`,
  :meth:`~QuestLog.track_conversation_log`) register the matching ``bd.on``
  handlers for you, so objectives progress themselves as the player fights,
  loots, explores, and talks.
- **Cross-system rewards.** Beyond items and messages, a quest reward may
  carry ``"xp"`` (dispatched to the log's ``on_xp_reward`` callbacks, so
  mods can wire quest completion to a rules engine such as ``bd_dnd``) and
  ``"disposition"`` deltas (dispatched to ``on_disposition_reward``
  callbacks, e.g. a ``bd_npcs`` store), keeping this package free of hard
  dependencies.
- **Savegame persistence.** The log serializes every quest into
  ``bd.state`` on ``save`` and restores it on ``load``; quest state survives
  checkpoints and savegames with no mod-side bookkeeping.
- **Optional journal UI.** ``bd_quests.journal_ui.JournalUI`` renders the
  log as a Dear ImGui window from an ``imgui_frame`` handler;
  ``bd_quests.journal_ui.bind_journal_toggle`` wires a console alias and
  an optional key bind (through the engine's ``pyui`` command and the
  ``ui_command`` event) to flip its visibility.

Minimal usage::

    import biaseddoom as bd
    import bd_quests
    from bd_quests import Objective, Quest

    @bd.on("engine_start")
    def setup(event):
        quest = Quest("clean_sweep", "Clean Sweep",
                      description="Clear the yard of imps.", giver="Sgt. Vega",
                      faction="UAC")
        quest.add_objective(Objective("kill_imps", "Eliminate imps", count=5))
        quest.rewards["give"] = [("Shell", 4)]
        quest.rewards["message"] = "Yard cleared!"
        bd_quests.log.add(quest)
        bd_quests.log.track_kills("clean_sweep", "kill_imps", "DoomImp", 5)

    @bd.on("map_load")
    def begin(event):
        bd_quests.log.get("clean_sweep").start()

Sequential vs. parallel objectives
----------------------------------

By default a quest is **sequential**: only the first unfinished objective is
"current" (:attr:`Quest.current_objective`); progress calls against later
objectives are rejected with a warning until the current one completes. Set
``quest.parallel = True`` to make every unfinished objective active at once.

Kill-credit semantics
---------------------

The engine's ``actor_died`` event carries exact killer attribution:
``attacker_ref`` / ``attacker_class`` / ``attacker_player_index`` report
the ``Die`` source — the shooter for missile kills (the missile itself
stays in ``inflictor``), the attacker for hitscan and melee, the bomb
owner for explosions, and ``None`` for environmental deaths (crushers,
damaging terrain, falling) and for scripted ``Actor.damage`` calls made
without a source. :meth:`QuestLog.track_kills` therefore defaults to
``killer="player"``: only deaths whose attacker is a player pawn count
toward the objective. Pass ``killer_class="..."`` to additionally
require an exact killer class, or ``killer="any"`` for the legacy
approximate policy where any matching death counts (monster infighting
and crushers included).

Persistence and the TID-rebind rule
-----------------------------------

Only quest IDs, states, objective progress, and fail reasons are persisted —
never ``Actor`` handles, TIDs, or other live objects. The auto-wiring
trackers resolve every target lazily from the event payload, so there is
nothing to rebind after a load. If your own hooks retain handles, store TIDs
in ``bd.state`` and re-resolve them with ``bd.actor_ref`` in a ``load``
handler, exactly as the main Python manual prescribes.

Failure contract
----------------

Every engine interaction inside ``bd_quests`` event handlers is guarded: if
a tracker handler raises, the exception is reported once via ``bd.warn`` and
the affected quest is marked **failed** (with the exception text as the
reason) instead of crashing or disabling the game-loop callback. User hooks
(``on_start`` and friends) are guarded separately and only produce a
``bd.warn`` — a buggy hook never fails its quest.

The tracker helpers register **global** event handlers. Call them from
``engine_start`` or ``map_load`` (module import time works too); they resolve
their class/tag/log-number targets lazily per event, so map data never has
to exist at registration time.
"""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional, Sequence, Set, Tuple, Union

import biaseddoom as bd

__all__ = ["Objective", "Quest", "QuestLog", "log", "STATE_KEY", "__version__"]

__version__ = "1.1.0"

#: Key under which the default QuestLog persists itself in ``bd.state``.
STATE_KEY = "bd_quests"


class Objective:
    """One measurable step of a quest.

    ``count`` is the number of progress units required (``1`` = a single
    yes/no step). Runtime state lives in ``progress`` (units credited so
    far) and ``done``; both are persisted by the owning quest log.
    """

    def __init__(self, id: str, text: str, count: int = 1) -> None:
        if int(count) < 1:
            raise ValueError("objective count must be >= 1")
        self.id: str = str(id)
        self.text: str = str(text)
        self.count: int = int(count)
        self.progress: int = 0
        self.done: bool = False

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        state = "done" if self.done else f"{self.progress}/{self.count}"
        return f"<Objective {self.id!r} {state}>"


class Quest:
    """A named quest with objectives, rewards, and lifecycle hooks.

    States are the string constants :data:`INACTIVE`, :data:`ACTIVE`,
    :data:`COMPLETED`, and :data:`FAILED`. Hooks are plain attributes —
    assign a callable and it is invoked (exceptions are caught and reported
    with ``bd.warn``)::

        quest.on_complete = lambda q: bd.play_ui_sound("misc/secret")

    Rewards are a plain dict: ``{"give": [(class_name, count), ...],
    "message": str, "xp": int, "disposition": [(npc_id, delta), ...]}``.
    On completion the items are given to the console player's pawn via its
    ``Actor`` handle and the message is shown with ``bd.center_message``.
    An ``"xp"`` reward is dispatched to the owning log's
    :attr:`QuestLog.on_xp_reward` callbacks (wire one to a rules engine
    such as ``bd_dnd`` to turn quest completion into character XP), and a
    ``"disposition"`` reward is dispatched to
    :attr:`QuestLog.on_disposition_reward` callbacks (wire one to a
    ``bd_npcs`` store to shift NPC standings on quest outcomes).
    """

    INACTIVE = "inactive"
    ACTIVE = "active"
    COMPLETED = "completed"
    FAILED = "failed"
    STATES = (INACTIVE, ACTIVE, COMPLETED, FAILED)

    def __init__(self, id: str, name: str, description: str = "",
                 giver: str = "", faction: Optional[str] = None) -> None:
        self.id: str = str(id)
        self.name: str = str(name)
        self.description: str = str(description)
        self.giver: str = str(giver)
        self.faction: Optional[str] = None if faction is None else str(faction)
        self.state: str = Quest.INACTIVE
        self.fail_reason: str = ""
        #: When True, all unfinished objectives accept progress at once.
        self.parallel: bool = False
        self.objectives: List[Objective] = []
        self.rewards: Dict[str, Any] = {"give": [], "message": ""}
        # Lifecycle hooks (assign callables or leave as None).
        self.on_start: Optional[Callable[[Quest], None]] = None
        self.on_objective_progress: Optional[Callable[[Quest, Objective], None]] = None
        self.on_objective_complete: Optional[Callable[[Quest, Objective], None]] = None
        self.on_complete: Optional[Callable[[Quest], None]] = None
        self.on_fail: Optional[Callable[[Quest], None]] = None

    # -- structure -----------------------------------------------------------

    def add_objective(self, obj: Objective) -> Objective:
        """Append an objective and return it (for chaining)."""
        if any(existing.id == obj.id for existing in self.objectives):
            raise ValueError(f"quest {self.id!r} already has objective {obj.id!r}")
        self.objectives.append(obj)
        return obj

    def objective(self, objective_id: str) -> Optional[Objective]:
        """Return the objective with this ID, or None."""
        for obj in self.objectives:
            if obj.id == objective_id:
                return obj
        return None

    @property
    def current_objective(self) -> Optional[Objective]:
        """The first unfinished objective in sequential mode.

        In parallel mode every unfinished objective is current, so this
        returns None; use :meth:`active_objectives` instead.
        """
        if self.parallel:
            return None
        for obj in self.objectives:
            if not obj.done:
                return obj
        return None

    def active_objectives(self) -> List[Objective]:
        """Objectives currently accepting progress.

        Sequential quests expose only their current objective; parallel
        quests expose every unfinished one.
        """
        if self.parallel:
            return [obj for obj in self.objectives if not obj.done]
        current = self.current_objective
        return [current] if current is not None else []

    def all_done(self) -> bool:
        """True when the quest has objectives and every one is done."""
        return bool(self.objectives) and all(obj.done for obj in self.objectives)

    # -- lifecycle -----------------------------------------------------------

    def start(self) -> bool:
        """Move from inactive to active; no-op (False) in any other state."""
        if self.state != Quest.INACTIVE:
            return False
        self.state = Quest.ACTIVE
        self._fire("on_start", self)
        return True

    def complete(self) -> bool:
        """Complete the quest, applying rewards; no-op unless active."""
        if self.state != Quest.ACTIVE:
            return False
        self.state = Quest.COMPLETED
        self._apply_rewards()
        self._fire("on_complete", self)
        return True

    def fail(self, reason: str = "") -> bool:
        """Fail the quest; no-op once completed or already failed."""
        if self.state in (Quest.COMPLETED, Quest.FAILED):
            return False
        self.state = Quest.FAILED
        self.fail_reason = str(reason)
        self._fire("on_fail", self)
        return True

    def activate_next_objective(self) -> Optional[Objective]:
        """Reveal the next objective in sequential mode and return it.

        Sequential currency is *derived* from completion state (the first
        unfinished objective), so completing the current objective already
        reveals the next; this method simply returns the new current
        objective (or None when the quest is finished or parallel). The
        derived design is what makes persistence trivial — there is no
        cursor to serialize.
        """
        return self.current_objective

    # -- internals -----------------------------------------------------------

    def _fire(self, hook_name: str, *args: Any) -> None:
        hook = getattr(self, hook_name, None)
        if hook is None:
            return
        try:
            hook(*args)
        except Exception as exc:
            bd.warn(f"bd_quests: hook {hook_name} of quest {self.id!r} "
                    f"raised: {exc!r}")

    def _apply_rewards(self) -> None:
        rewards = self.rewards or {}
        gives = rewards.get("give") or []
        message = rewards.get("message") or ""
        pawn = None
        if gives:
            try:
                player = bd.player()
                pawn = player.actor if player is not None else None
            except Exception as exc:
                bd.warn(f"bd_quests: cannot reach the player to grant rewards "
                        f"for quest {self.id!r}: {exc!r}")
        if pawn is not None:
            for entry in gives:
                try:
                    class_name = str(entry[0])
                    count = int(entry[1]) if len(entry) > 1 else 1
                    pawn.give_inventory(class_name, count)
                except Exception as exc:
                    bd.warn(f"bd_quests: reward {entry!r} for quest "
                            f"{self.id!r} failed: {exc!r}")
        if message:
            try:
                bd.center_message(str(message))
            except Exception as exc:
                bd.warn(f"bd_quests: reward message for quest {self.id!r} "
                        f"failed: {exc!r}")
        xp = rewards.get("xp")
        if xp:
            log = getattr(self, "_log", None)
            callbacks = getattr(log, "on_xp_reward", None) if log else None
            if callbacks:
                for callback in callbacks:
                    try:
                        callback(int(xp), self)
                    except Exception as exc:
                        bd.warn(f"bd_quests: xp reward callback for quest "
                                f"{self.id!r} raised: {exc!r}")
        shifts = rewards.get("disposition") or []
        if shifts:
            log = getattr(self, "_log", None)
            callbacks = (getattr(log, "on_disposition_reward", None)
                         if log else None)
            if callbacks:
                for entry in shifts:
                    try:
                        npc_id = str(entry[0])
                        delta = int(entry[1]) if len(entry) > 1 else 0
                    except Exception as exc:
                        bd.warn(f"bd_quests: malformed disposition reward "
                                f"{entry!r} for quest {self.id!r}: {exc!r}")
                        continue
                    for callback in callbacks:
                        try:
                            callback(npc_id, delta, self)
                        except Exception as exc:
                            bd.warn(f"bd_quests: disposition reward "
                                    f"callback for quest {self.id!r} "
                                    f"raised: {exc!r}")

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Quest {self.id!r} {self.state}>"


class QuestLog:
    """Registry of quests with event auto-wiring and bd.state persistence.

    The module-level singleton :data:`log` is enough for most mods; create
    additional logs only for fully independent quest namespaces, and give
    each one a unique ``state_key`` so their ``bd.state`` entries do not
    collide.
    """

    def __init__(self, state_key: str = STATE_KEY) -> None:
        self._quests: Dict[str, Quest] = {}
        self._order: List[str] = []
        self._state_key: str = str(state_key)
        self._persistence_armed: bool = False
        self._kill_class_cache: Dict[Tuple[str, bool], Set[str]] = {}
        #: Reward sinks. An ``"xp"`` quest reward fires every
        #: ``on_xp_reward`` callback as ``callback(amount, quest)``; a
        #: ``"disposition"`` reward fires every ``on_disposition_reward``
        #: callback as ``callback(npc_id, delta, quest)``. Wire these to
        #: the mod's rules engine (e.g. ``character.award_xp``) and NPC
        #: store (e.g. ``dispositions.shift``); callbacks run after the
        #: give/message rewards and never block completion.
        self.on_xp_reward: List[Callable[[int, Quest], None]] = []
        self.on_disposition_reward: List[Callable[[str, int, Quest], None]] \
            = []

    # -- registry --------------------------------------------------------------

    def add(self, quest: Quest) -> Quest:
        """Register a quest (by unique ID) and return it."""
        if quest.id in self._quests:
            raise ValueError(f"duplicate quest id {quest.id!r}")
        self._quests[quest.id] = quest
        self._order.append(quest.id)
        quest._log = self  # reward dispatch (xp/disposition) targets this log
        self._arm_persistence()
        return quest

    def get(self, quest_id: str) -> Optional[Quest]:
        """Return the quest with this ID, or None."""
        return self._quests.get(str(quest_id))

    def all(self) -> List[Quest]:
        """All quests, in registration order."""
        return [self._quests[qid] for qid in self._order]

    def active(self) -> List[Quest]:
        """All quests currently in the ACTIVE state."""
        return [q for q in self.all() if q.state == Quest.ACTIVE]

    def by_faction(self, name: str) -> List[Quest]:
        """All quests whose faction string equals ``name``."""
        return [q for q in self.all() if q.faction == name]

    # -- progression -------------------------------------------------------------

    def complete_objective(self, quest_id: str, objective_id: str,
                           amount: int = 1) -> bool:
        """Credit progress to one objective.

        Fires ``on_objective_progress``/``on_objective_complete`` hooks and
        auto-completes the quest when its last objective finishes. In
        sequential mode only the current objective accepts progress; calling
        for a later objective warns and returns False. Returns True when
        progress was applied.
        """
        quest = self.get(quest_id)
        if quest is None:
            bd.warn(f"bd_quests: complete_objective: unknown quest {quest_id!r}")
            return False
        if quest.state != Quest.ACTIVE:
            bd.warn(f"bd_quests: quest {quest_id!r} is {quest.state}, not active")
            return False
        obj = quest.objective(objective_id)
        if obj is None:
            bd.warn(f"bd_quests: quest {quest_id!r} has no objective "
                    f"{objective_id!r}")
            return False
        if obj.done:
            return False
        if not quest.parallel and quest.current_objective is not obj:
            bd.warn(f"bd_quests: objective {objective_id!r} of quest "
                    f"{quest_id!r} is not current (sequential quest)")
            return False
        obj.progress = min(obj.count, obj.progress + max(1, int(amount)))
        quest._fire("on_objective_progress", quest, obj)
        if obj.progress >= obj.count:
            obj.done = True
            quest._fire("on_objective_complete", quest, obj)
            quest.activate_next_objective()
            if quest.all_done():
                quest.complete()
        return True

    def fail_quest(self, quest_id: str, reason: str = "") -> bool:
        """Fail a quest by ID. Returns False for unknown/finished quests."""
        quest = self.get(quest_id)
        if quest is None:
            bd.warn(f"bd_quests: fail_quest: unknown quest {quest_id!r}")
            return False
        return quest.fail(reason)

    # -- event auto-wiring ---------------------------------------------------

    def track_kills(self, quest_id: str, objective_id: str, class_name: str,
                    count: int, subclasses: bool = True, *,
                    killer: str = "player",
                    killer_class: Optional[str] = None) -> Callable:
        """Progress an objective as matching monsters die (``actor_died``).

        With ``subclasses=True`` (default) deaths of any class derived from
        ``class_name`` count; the descendant set is resolved lazily through
        ``bd.actors.children_of`` on the first matching event.

        Kill credit is **exact by default**: ``killer="player"`` counts
        only deaths whose ``actor_died`` attacker is a player pawn
        (``attacker_player_index`` is not None — missile kills credit the
        shooter, hitscan/melee the attacker, explosions the bomb owner).
        This is a deliberate breaking change from the original release,
        which counted any matching death because the engine exposed no
        killer field; pass ``killer="any"`` to restore that approximate
        policy (infighting, crushers, and source-less scripted kills then
        count too). ``killer_class`` adds an exact, case-insensitive
        filter on the killer's class name (``attacker_class``) in either
        mode. See the module docstring for the full semantics.
        """
        killer = str(killer).lower()
        if killer not in ("player", "any"):
            raise ValueError("killer must be 'player' or 'any'")
        self._wiring_target(quest_id, objective_id).count = max(1, int(count))
        log = self
        base = str(class_name)
        use_subclasses = bool(subclasses)
        want_killer_class = (str(killer_class).lower()
                             if killer_class is not None else None)

        @bd.on("actor_died")
        def _on_death(event: Dict[str, Any]) -> None:
            snapshot = event.get("actor") or {}
            victim_class = str(snapshot.get("class_name") or "")
            if not victim_class:
                return
            if victim_class.lower() not in log._kill_classes(base, use_subclasses):
                return
            if killer == "player" and event.get("attacker_player_index") is None:
                return
            if want_killer_class is not None:
                attacker_class = event.get("attacker_class")
                if (not attacker_class
                        or str(attacker_class).lower() != want_killer_class):
                    return
            log._guarded(quest_id, f"kill tracker for {base!r}",
                         lambda: log.complete_objective(quest_id, objective_id, 1))

        return _on_death

    def track_pickup(self, quest_id: str, objective_id: str, class_name: str,
                     count: int) -> Callable:
        """Progress an objective as the player picks items up (``item_picked``).

        Each pickup credits the event's ``amount`` (normally 1). Note the
        engine fires ``item_picked`` only for real world pickups
        (``Inventory.Touch``); items granted through ``give_inventory`` do
        not produce the event.
        """
        self._wiring_target(quest_id, objective_id).count = max(1, int(count))
        log = self
        wanted = str(class_name).lower()

        @bd.on("item_picked")
        def _on_pickup(event: Dict[str, Any]) -> None:
            picked = str(event.get("class_name") or "").lower()
            if picked != wanted:
                return
            amount = event.get("amount")
            try:
                amount = max(1, int(amount))
            except (TypeError, ValueError):
                amount = 1
            log._guarded(quest_id, f"pickup tracker for {class_name!r}",
                         lambda: log.complete_objective(quest_id, objective_id,
                                                        amount))

        return _on_pickup

    def track_sector(self, quest_id: str, objective_id: str,
                     tags: Union[int, Sequence[int]]) -> Callable:
        """Complete an objective when the player enters a tagged sector.

        ``tags`` is one tag or a sequence of tags; entering any sector whose
        tag list intersects it completes the objective (full remaining
        progress is credited at once).
        """
        self._wiring_target(quest_id, objective_id)
        log = self
        if isinstance(tags, int):
            wanted_tags = frozenset([tags])
        else:
            wanted_tags = frozenset(int(tag) for tag in tags)

        @bd.on("sector_entered")
        def _on_sector(event: Dict[str, Any]) -> None:
            event_tags = event.get("tags") or []
            try:
                hit = any(int(tag) in wanted_tags for tag in event_tags)
            except (TypeError, ValueError):
                return
            if not hit:
                return

            def apply() -> None:
                quest = log.get(quest_id)
                obj = quest.objective(objective_id) if quest is not None else None
                if obj is None or obj.done:
                    return
                log.complete_objective(quest_id, objective_id,
                                       obj.count - obj.progress)

            log._guarded(quest_id, f"sector tracker for tags "
                                   f"{sorted(wanted_tags)}", apply)

        return _on_sector

    def track_conversation_log(self, quest_id: str, objective_id: str,
                               log_number: int) -> Callable:
        """Complete an objective from a Strife dialogue reply log number.

        When a ``conversation_reply`` with a matching ``log_number`` commits,
        the quest auto-starts if it is still inactive (the classic
        "quest from dialogue" idiom), then the objective is completed.
        """
        self._wiring_target(quest_id, objective_id)
        log = self
        wanted_log = int(log_number)

        @bd.on("conversation_reply")
        def _on_reply(event: Dict[str, Any]) -> None:
            if event.get("log_number") != wanted_log:
                return

            def apply() -> None:
                quest = log.get(quest_id)
                if quest is None:
                    return
                if quest.state == Quest.INACTIVE:
                    quest.start()
                obj = quest.objective(objective_id)
                if obj is None or obj.done:
                    return
                log.complete_objective(quest_id, objective_id,
                                       obj.count - obj.progress)

            log._guarded(quest_id, f"conversation tracker for log "
                                   f"{wanted_log}", apply)

        return _on_reply

    # -- persistence -----------------------------------------------------------

    def serialize(self) -> Dict[str, Any]:
        """Plain JSON-able snapshot of every quest (no handles, no TIDs)."""
        return {
            "version": 1,
            "quests": [
                {
                    "id": quest.id,
                    "state": quest.state,
                    "fail_reason": quest.fail_reason,
                    "objectives": [
                        {"id": obj.id, "progress": obj.progress, "done": obj.done}
                        for obj in quest.objectives
                    ],
                }
                for quest in self.all()
            ],
        }

    def restore(self, data: Any) -> None:
        """Restore quest state from a :meth:`serialize` snapshot.

        Idempotent and tolerant: unknown quest/objective IDs, missing keys,
        and malformed values are skipped; quest definitions themselves
        (text, counts, rewards) always come from the mod's code.
        """
        if not isinstance(data, dict):
            return
        entries = data.get("quests")
        if not isinstance(entries, list):
            return
        for entry in entries:
            if not isinstance(entry, dict):
                continue
            quest = self._quests.get(str(entry.get("id", "")))
            if quest is None:
                continue
            state = entry.get("state")
            if state in Quest.STATES:
                quest.state = state
            reason = entry.get("fail_reason")
            if isinstance(reason, str):
                quest.fail_reason = reason
            saved_objectives = entry.get("objectives")
            if not isinstance(saved_objectives, list):
                continue
            for saved in saved_objectives:
                if not isinstance(saved, dict):
                    continue
                obj = quest.objective(str(saved.get("id", "")))
                if obj is None:
                    continue
                try:
                    obj.progress = max(0, min(obj.count,
                                              int(saved.get("progress", 0))))
                except (TypeError, ValueError):
                    pass
                obj.done = bool(saved.get("done", obj.done))
                if obj.done:
                    obj.progress = obj.count

    # -- internals ---------------------------------------------------------------

    def _wiring_target(self, quest_id: str, objective_id: str) -> Objective:
        """Resolve the (quest, objective) pair a tracker will drive."""
        quest = self.get(quest_id)
        if quest is None:
            raise KeyError(f"bd_quests: unknown quest {quest_id!r}")
        obj = quest.objective(objective_id)
        if obj is None:
            raise KeyError(f"bd_quests: quest {quest_id!r} has no objective "
                           f"{objective_id!r}")
        return obj

    def _guarded(self, quest_id: str, context: str, action: Callable[[], None]) -> None:
        """Run a tracker action; on failure warn and fail the quest.

        This is the framework's failure contract: a quest bug must never
        propagate out of an engine event handler (which would get the
        callback disabled by the runtime), so exceptions are contained here.
        """
        try:
            action()
        except Exception as exc:
            bd.warn(f"bd_quests: {context} failed for quest {quest_id!r}: "
                    f"{exc!r}")
            quest = self.get(quest_id)
            if quest is not None:
                try:
                    quest.fail(f"internal error in {context}: {exc}")
                except Exception:
                    pass

    def _resolve_class_name(self, class_name: str) -> str:
        """Engine-cased class name for a case-insensitive user-supplied name."""
        resolved = bd.actors.resolve(class_name)
        if resolved is not None:
            return resolved
        wanted = class_name.lower()
        for name in bd.actors.names():
            if name.lower() == wanted:
                return name
        raise ValueError(f"unknown actor class {class_name!r}")

    def _kill_classes(self, class_name: str, subclasses: bool) -> Set[str]:
        """Lowercased set of class names a kill tracker accepts."""
        key = (class_name.lower(), bool(subclasses))
        cached = self._kill_class_cache.get(key)
        if cached is not None:
            return cached
        names = {class_name.lower()}
        if subclasses:
            try:
                real_name = self._resolve_class_name(class_name)
                names.update(name.lower()
                             for name in bd.actors.children_of(real_name))
            except Exception as exc:
                bd.warn(f"bd_quests: cannot resolve descendants of "
                        f"{class_name!r} ({exc!r}); tracking the exact class only")
        self._kill_class_cache[key] = names
        return names

    def _arm_persistence(self) -> None:
        """Register the save/load handlers exactly once per log."""
        if self._persistence_armed:
            return
        self._persistence_armed = True
        log = self

        @bd.on("save")
        def _on_save(event: Dict[str, Any]) -> None:
            try:
                bd.state[log._state_key] = log.serialize()
            except Exception as exc:
                bd.warn(f"bd_quests: could not serialize quest state: {exc!r}")

        @bd.on("load")
        def _on_load(event: Dict[str, Any]) -> None:
            try:
                log.restore(bd.state.get(log._state_key))
            except Exception as exc:
                bd.warn(f"bd_quests: could not restore quest state: {exc!r}")


#: Shared quest registry. Most mods should use this singleton.
log = QuestLog()


def __getattr__(name: str) -> Any:
    # Lazy convenience accessors so `bd_quests.JournalUI` (and friends)
    # work without paying for the UI module unless it is actually used.
    if name == "JournalUI":
        from .journal_ui import JournalUI
        return JournalUI
    if name == "bind_journal_toggle":
        from .journal_ui import bind_journal_toggle
        return bind_journal_toggle
    if name == "default_journal":
        from .journal_ui import default_journal
        return default_journal
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
