"""Branching NPC dialogue trees for BiasedDoom Python mods.

``bd_dialogue`` is an engine-shipped, dependency-free package (it only
needs ``biaseddoom``, and optionally composes with ``bd_quests``,
``bd_vtm``, and ``bd_dnd``) that gives mods a classic RPG conversation
system:

- **Data-driven trees.** A :class:`Dialogue` is a named bundle of
  :class:`Node` entries (speaker + body text + optional live-sprite
  portrait); every node carries :class:`Choice` rows that route to other
  nodes or end the conversation. Tree references are validated up front:
  a ``next``/``fail_next`` that names no node raises ``ValueError``
  listing the dangling ID.
- **Gated choices.** Choices may hide behind a ``condition(ctx)``
  predicate, lock behind a ``bd_vtm`` faction reputation floor
  (``faction_gate=("Anarchs", 1)`` renders ``[LOCKED]`` and is
  non-selectable until ``factions.reputation(name) >= min_standing``),
  or roll a ``bd_dnd`` skill check on selection
  (``skill_check=("persuasion", 14)``: success routes to ``next``,
  failure to ``fail_next`` — or ``next`` when ``fail_next`` is None).
  Choices can also run an ``effect(ctx)`` hook and write the native
  Strife journal line via ``log=("text", number)`` (``bd.set_player_log``).
- **Real-time sessions.** :class:`DialogueSession` runs a tree against a
  live NPC actor handle. Dialogue is **real-time** — the world is not
  paused; the NPC's velocity is simply zeroed while it talks. One
  session at a time per mod runtime (a second :meth:`start` warns and
  no-ops), and a stale NPC handle auto-ends the conversation.
- **Optional overlay UI.** ``bd_dialogue.ui.DialogueUI`` renders the
  active session as a Dear ImGui window from an ``imgui_frame`` handler:
  live NPC sprite portrait (``imgui.image(npc_ref)``), colored speaker
  name, wrapped body text, numbered selectable choices with their
  annotations, and a ~3 s result flash after each skill check.

Minimal usage::

    import biaseddoom as bd
    import bd_dialogue
    from bd_dialogue import Choice, Dialogue, Node

    dialogue = Dialogue("guard")
    dialogue.add_node(Node("start", "Guard", "Halt. State your business."))
    dialogue.add_node(Node("pass", "Guard", "Fine. Move along."))
    dialogue.node("start").add_choice(Choice(
        "Let me through. [Persuade]", next="pass",
        skill_check=("persuasion", 14)))
    dialogue.node("start").add_choice(Choice("Never mind.", end=True))
    dialogue.node("pass").add_choice(Choice("Thanks.", end=True))

    @bd.on("map_load")
    def spawn_npc(event):
        npc = bd.spawn("ZombieMan", 512.0, -3520.0, 0.0, tid=9500)
        npc.set_flag("FRIENDLY", True)
        session = bd_dialogue.DialogueSession(dialogue, npc,
                                              character=my_character)
        session.start()

Condition/effect context
------------------------

``condition(ctx)`` and ``effect(ctx)`` receive a dict with the keys
``session``, ``npc``, ``character``, ``factions``, ``quest_log``,
``player_index``, and ``check_result`` (the rich result dict of the
skill check that just ran on this choice, or None). Effects run *after*
the roll, so an effect can branch on ``check_result["success"]``.

RNG doubles for tests
---------------------

:meth:`DialogueSession.choose` rolls through
``character.skill_check(skill, dc)`` by default; assign
``session.rng`` (any object with ``randint(lo, hi)`` or ``int(lo, hi)``,
like ``bd_dnd``'s own convention) and the roll goes through
``character.skill_check(skill, dc, rng=session.rng)`` instead, so tests
can script exact branches without touching the engine RNG.

Persistence
-----------

Sessions are **transient**: there is no mid-dialogue save support (a
savegame load drops any active session; outcomes already committed to
``bd_quests``/``bd_vtm``/inventory persist through those packs' own
``bd.state`` handling). Nothing from this package is stored in
``bd.state``.

Coexistence
-----------

``bd_dialogue`` is a pure Python/ImGui layer; it does not touch the
engine's native Strife conversation system (see
``examples/python/30_conversation_quests``), so both can run side by
side on the same map.
"""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional, Tuple

import biaseddoom as bd

__all__ = [
    "Node", "Choice", "Dialogue", "DialogueSession",
    "active_session", "FLASH_TICS", "__version__",
]

__version__ = "1.0.0"

#: How long the skill-check result flash stays visible (map tics, ~3 s).
FLASH_TICS = 3 * 35


# --- model ---------------------------------------------------------------------


class Node:
    """One line of dialogue: speaker, body text, and outgoing choices.

    ``portrait=True`` tells the UI to render the NPC's live sprite via
    ``imgui.image(npc_ref)`` next to this node; set False for narrator /
    intercom lines where no face should show. Choices are appended with
    :meth:`add_choice` (or by assigning :attr:`choices` directly).
    """

    def __init__(self, id: str, speaker: str, text: str,
                 portrait: bool = True) -> None:
        self.id: str = str(id)
        self.speaker: str = str(speaker)
        self.text: str = str(text)
        self.portrait: bool = bool(portrait)
        self.choices: List[Choice] = []

    def add_choice(self, choice: Choice) -> Choice:
        """Append a choice and return it (for chaining)."""
        self.choices.append(choice)
        return choice

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Node {self.id!r} speaker={self.speaker!r}>"


class Choice:
    """One selectable reply on a :class:`Node`.

    - ``next``: target node ID on selection (or on a passed skill
      check). ``next=None`` or ``end=True`` ends the conversation.
    - ``condition``: ``condition(ctx) -> bool``; when it returns False
      the choice is hidden entirely.
    - ``effect``: ``effect(ctx)`` run on selection, after the skill
      roll (``ctx["check_result"]``) and before routing.
    - ``skill_check``: ``(skill, dc)`` — choosing rolls
      ``character.skill_check(skill, dc)``; success routes to ``next``,
      failure to ``fail_next`` (or ``next`` when ``fail_next`` is None).
      With ``show_failed=True`` (default) the row is annotated
      ``(DC <dc> <Skill>)``. When the session has no character the row
      renders ``(unavailable)`` and is non-selectable.
    - ``faction_gate``: ``(faction_name, min_standing)`` — needs
      ``factions.reputation(name) >= min_standing`` (bd_vtm API);
      otherwise the row renders ``[LOCKED]`` and is non-selectable.
    - ``log``: ``(text, number)`` — writes the native Strife journal
      line via ``bd.set_player_log(text, player_index)``. ``number`` is
      kept for API compatibility/future use; the engine's free-text log
      API takes only the text.
    - ``end``: True ends the conversation on selection.
    """

    def __init__(self, text: str, next: Optional[str] = None,
                 condition: Optional[Callable[[Dict[str, Any]], bool]] = None,
                 effect: Optional[Callable[[Dict[str, Any]], None]] = None,
                 skill_check: Optional[Tuple[str, int]] = None,
                 fail_next: Optional[str] = None,
                 show_failed: bool = True,
                 faction_gate: Optional[Tuple[str, int]] = None,
                 log: Optional[Tuple[str, int]] = None,
                 end: bool = False) -> None:
        self.text: str = str(text)
        self.next: Optional[str] = None if next is None else str(next)
        self.condition = condition
        self.effect = effect
        if skill_check is not None:
            skill_check = (str(skill_check[0]), int(skill_check[1]))
        self.skill_check: Optional[Tuple[str, int]] = skill_check
        self.fail_next: Optional[str] = (None if fail_next is None
                                         else str(fail_next))
        self.show_failed: bool = bool(show_failed)
        if faction_gate is not None:
            faction_gate = (str(faction_gate[0]), int(faction_gate[1]))
        self.faction_gate: Optional[Tuple[str, int]] = faction_gate
        if log is not None:
            log = (str(log[0]), int(log[1]))
        self.log: Optional[Tuple[str, int]] = log
        self.end: bool = bool(end)

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Choice {self.text!r} -> {self.next!r}>"


class Dialogue:
    """A validated tree of :class:`Node` entries.

    ``start`` names the entry node (default ``"start"``). References are
    validated by :meth:`validate` — automatically when a
    :class:`DialogueSession` is constructed — and every dangling
    ``next``/``fail_next`` raises ``ValueError`` naming the missing ID.
    """

    def __init__(self, id: str, start: str = "start") -> None:
        self.id: str = str(id)
        self.start: str = str(start)
        self._nodes: Dict[str, Node] = {}
        self._order: List[str] = []

    def add_node(self, node: Node) -> Node:
        """Register a node (by unique ID) and return it."""
        if node.id in self._nodes:
            raise ValueError(f"dialogue {self.id!r} already has node "
                             f"{node.id!r}")
        self._nodes[node.id] = node
        self._order.append(node.id)
        return node

    def node(self, node_id: str) -> Optional[Node]:
        """Return the node with this ID, or None."""
        return self._nodes.get(str(node_id))

    def all(self) -> List[Node]:
        """All nodes, in registration order."""
        return [self._nodes[nid] for nid in self._order]

    def validate(self) -> None:
        """Raise ValueError for a missing start node or dangling targets."""
        if self.start not in self._nodes:
            raise ValueError(f"dialogue {self.id!r}: start node "
                             f"{self.start!r} does not exist")
        dangling: List[str] = []
        for node in self.all():
            for choice in node.choices:
                for target in (choice.next, choice.fail_next):
                    if target is not None and target not in self._nodes:
                        dangling.append(f"{node.id!r} -> {target!r}")
        if dangling:
            raise ValueError(f"dialogue {self.id!r}: dangling choice "
                             f"target(s): {', '.join(dangling)}")

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<Dialogue {self.id!r} nodes={len(self._order)}>"


# --- runtime -------------------------------------------------------------------

#: The single active session (one conversation at a time per runtime).
_active_session: Optional["DialogueSession"] = None


def active_session() -> Optional["DialogueSession"]:
    """The currently active :class:`DialogueSession`, or None."""
    return _active_session


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
            bd.warn(f"bd_dialogue: callback {callback!r} raised: {exc!r}")


class DialogueSession:
    """Runs a :class:`Dialogue` against a live NPC actor handle.

    ``npc_ref`` is the conversation partner (velocity zeroed on
    :meth:`start`; staleness auto-ends the session). ``character`` is a
    ``bd_dnd.Character``-like object used for ``skill_check`` choices
    (None renders them ``(unavailable)``); ``factions`` is a
    ``bd_vtm.Factions``-like object used for ``faction_gate`` choices
    (None locks every gate); ``quest_log`` and ``player_index`` are
    passed through to the condition/effect context. ``faction`` names
    the NPC's faction so the UI can color the speaker line by
    reputation. ``rng`` (optional) is forwarded to
    ``character.skill_check(..., rng=rng)`` for scripted test doubles.

    :attr:`on_end` is a callback list fired once with ``(session)`` when
    the conversation ends; :attr:`end_reason` is one of ``"choice"``
    (a choice ended it), ``"stale_npc"``, or ``"manual"``.
    """

    def __init__(self, dialogue: Dialogue, npc_ref: Any,
                 character: Any = None, factions: Any = None,
                 quest_log: Any = None, player_index: int = 0,
                 faction: Optional[str] = None, rng: Any = None) -> None:
        dialogue.validate()
        self.dialogue: Dialogue = dialogue
        self.npc_ref: Any = npc_ref
        self.character: Any = character
        self.factions: Any = factions
        self.quest_log: Any = quest_log
        self.player_index: int = int(player_index)
        self.faction: Optional[str] = (None if faction is None
                                       else str(faction))
        #: Optional scripted RNG double forwarded to skill_check().
        self.rng: Any = rng
        self.active_node: Optional[Node] = None
        self.active: bool = False
        self.end_reason: str = ""
        #: Last skill-check outcome for the UI flash: {"text", "success",
        #: "total", "dc", "skill", "until_tic"} or None.
        self.last_check: Optional[Dict[str, Any]] = None
        self.on_end: List[Callable[[DialogueSession], None]] = []

    # -- lifecycle -------------------------------------------------------------

    def start(self) -> bool:
        """Begin the conversation at the dialogue's start node.

        One session at a time: starting while another session is active
        warns and returns False. Returns True on success.
        """
        global _active_session
        if _active_session is not None and _active_session.active:
            bd.warn(f"bd_dialogue: session for dialogue "
                    f"{self.dialogue.id!r} not started — another "
                    f"conversation is already active")
            return False
        if not self._npc_alive(report=False):
            bd.warn(f"bd_dialogue: cannot start dialogue "
                    f"{self.dialogue.id!r}: the NPC handle is stale")
            return False
        try:
            self.npc_ref.set_velocity(0.0, 0.0, 0.0)
        except Exception as exc:
            bd.warn(f"bd_dialogue: could not stop the NPC: {exc!r}")
        self.active = True
        self.end_reason = ""
        self.active_node = self.dialogue.node(self.dialogue.start)
        _active_session = self
        return True

    def end(self, reason: str = "manual") -> None:
        """End the conversation (idempotent); fires ``on_end`` once."""
        global _active_session
        if not self.active:
            return
        self.active = False
        self.end_reason = str(reason)
        if _active_session is self:
            _active_session = None
        _fire(self.on_end, self)

    # -- introspection ---------------------------------------------------------

    def context(self) -> Dict[str, Any]:
        """The ctx dict passed to condition/effect callables."""
        check_result = None
        if self.last_check is not None:
            check_result = self.last_check.get("result")
        return {
            "session": self,
            "npc": self.npc_ref,
            "character": self.character,
            "factions": self.factions,
            "quest_log": self.quest_log,
            "player_index": self.player_index,
            "check_result": check_result,
        }

    def choices(self) -> List[Tuple[Choice, bool, str]]:
        """Visible choices for the active node as (choice, enabled, annotation).

        Choices whose ``condition`` fails are omitted; locked/unavailable
        choices appear with ``enabled=False`` and a ``[LOCKED]`` /
        ``(unavailable)`` annotation. Auto-ends (and returns []) when the
        NPC handle went stale.
        """
        if not self.active or self.active_node is None:
            return []
        if not self._npc_alive():
            return []
        ctx = self.context()
        entries: List[Tuple[Choice, bool, str]] = []
        for choice in self.active_node.choices:
            if choice.condition is not None:
                try:
                    if not choice.condition(ctx):
                        continue
                except Exception as exc:
                    bd.warn(f"bd_dialogue: condition for "
                            f"{choice.text!r} raised: {exc!r}")
                    continue
            enabled = True
            annotations: List[str] = []
            if choice.faction_gate is not None:
                name, minimum = choice.faction_gate
                if not self._gate_open(name, minimum):
                    enabled = False
                    annotations.append("[LOCKED]")
            if choice.skill_check is not None:
                skill, dc = choice.skill_check
                if self.character is None:
                    enabled = False
                    annotations.append("(unavailable)")
                elif choice.show_failed:
                    annotations.append(
                        f"(DC {dc} {skill.replace('_', ' ').title()})")
            entries.append((choice, enabled, " ".join(annotations)))
        return entries

    # -- selection -------------------------------------------------------------

    def choose(self, visible_index: int) -> Any:
        """Run the visible choice at ``visible_index`` (see :meth:`choices`).

        Disabled or out-of-range selections are no-ops. Returns the
        skill-check result dict when a check ran, True for a plain
        choice, and False when nothing happened.
        """
        if not self.active or self.active_node is None:
            return False
        if not self._npc_alive():
            return False
        entries = self.choices()
        if not (0 <= int(visible_index) < len(entries)):
            bd.warn(f"bd_dialogue: choice index {visible_index} out of "
                    f"range ({len(entries)} visible)")
            return False
        choice, enabled, _annotation = entries[int(visible_index)]
        if not enabled:
            return False

        result = None
        success = True
        if choice.skill_check is not None:
            skill, dc = choice.skill_check
            result = self._roll_check(skill, dc)
            success = bool(result is not None and result.get("success"))

        if choice.effect is not None:
            try:
                choice.effect(self.context())
            except Exception as exc:
                bd.warn(f"bd_dialogue: effect of {choice.text!r} "
                        f"raised: {exc!r}")

        if choice.log is not None:
            text, _number = choice.log
            try:
                bd.set_player_log(text, self.player_index)
            except Exception as exc:
                bd.warn(f"bd_dialogue: could not write the player log: "
                        f"{exc!r}")

        if choice.end:
            self.end("choice")
            return result if result is not None else True
        target = choice.next if success else (choice.fail_next
                                              if choice.fail_next is not None
                                              else choice.next)
        if target is None:
            self.end("choice")
            return result if result is not None else True
        node = self.dialogue.node(target)
        if node is None:  # unreachable: validate() ran at construction
            bd.warn(f"bd_dialogue: dangling target {target!r}; ending")
            self.end("choice")
            return False
        self.active_node = node
        return result if result is not None else True

    # -- internals -------------------------------------------------------------

    def _roll_check(self, skill: str, dc: int) -> Optional[Dict[str, Any]]:
        """Run the skill check, record the UI flash, announce the result."""
        try:
            if self.rng is not None:
                result = self.character.skill_check(skill, dc, rng=self.rng)
            else:
                result = self.character.skill_check(skill, dc)
        except Exception as exc:
            bd.warn(f"bd_dialogue: skill check {skill!r} failed to roll: "
                    f"{exc!r}")
            result = None
        success = bool(result is not None and result.get("success"))
        total = result.get("total") if isinstance(result, dict) else "?"
        text = (f"({'Success' if success else 'Failed'}) "
                f"{total} vs DC {int(dc)}")
        self.last_check = {
            "text": text, "success": success, "total": total,
            "dc": int(dc), "skill": str(skill),
            "until_tic": _level_time() + FLASH_TICS,
            "result": result,
        }
        try:
            bd.center_message(text)
        except Exception as exc:
            bd.warn(f"bd_dialogue: could not announce the check: {exc!r}")
        return result

    def _gate_open(self, name: str, minimum: int) -> bool:
        """True when the faction reputation floor is satisfied."""
        if self.factions is None:
            return False
        try:
            return int(self.factions.reputation(name)) >= int(minimum)
        except Exception as exc:
            bd.warn(f"bd_dialogue: faction gate for {name!r} failed: "
                    f"{exc!r}")
            return False

    def _npc_alive(self, report: bool = True) -> bool:
        """Check the NPC handle; auto-end (stale_npc) when it went stale."""
        try:
            alive = self.npc_ref is not None and bool(self.npc_ref.valid)
        except Exception:
            alive = False
        if not alive and report and self.active:
            bd.warn(f"bd_dialogue: the NPC handle went stale — ending "
                    f"dialogue {self.dialogue.id!r}")
            self.end("stale_npc")
        return alive

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        node = self.active_node.id if self.active_node else "-"
        return f"<DialogueSession {self.dialogue.id!r} node={node}>"


def __getattr__(name: str) -> Any:
    # Lazy convenience accessor so `bd_dialogue.DialogueUI` works without
    # paying for the UI module unless it is actually used.
    if name == "DialogueUI":
        from .ui import DialogueUI
        return DialogueUI
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
