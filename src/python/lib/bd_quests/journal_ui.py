"""Dear ImGui quest journal overlay for :mod:`bd_quests`.

:class:`JournalUI` renders a :class:`~bd_quests.QuestLog` as an ImGui
window: one collapsing header per active quest (description, giver, colored
faction line, objectives as state checkboxes with ``x/y`` progress), with
completed and failed quests collected in a separate collapsed section.

Like every ``bd.imgui`` consumer, :meth:`JournalUI.draw` is only legal
inside an ``imgui_frame`` handler::

    import biaseddoom as bd
    import bd_quests
    from bd_quests.journal_ui import JournalUI

    journal = JournalUI()  # renders bd_quests.log

    @bd.on("imgui_frame")
    def draw(event):
        journal.draw()

Toggling
--------

Visibility flips with :meth:`JournalUI.toggle` (or the window's own close
button, which stays in sync automatically). To drive the toggle from the
console or a key bind, use :func:`bind_journal_toggle`: console
``alias``/``bind`` can only run *console commands*, so the helper routes
through the engine's ``pyui`` command and the ``ui_command`` event::

    journal = bd_quests.bind_journal_toggle(key="j")
    # the player can now press J or run `toggle_journal` at the console
"""

from __future__ import annotations

from typing import Dict, Optional

import biaseddoom as bd

from . import Quest, QuestLog
from . import log as _default_log

#: Text color (r, g, b, a) for faction labels.
_FACTION_COLOR = (0.45, 0.8, 1.0, 1.0)
#: Text color for completed quests in the finished section.
_COMPLETED_COLOR = (0.35, 0.9, 0.4, 1.0)
#: Text color for failed quests in the finished section.
_FAILED_COLOR = (0.95, 0.35, 0.3, 1.0)
#: Text color for the giver line.
_GIVER_COLOR = (0.9, 0.75, 0.45, 1.0)

# ImGuiTreeNodeFlags_DefaultOpen: active quest headers start expanded so the
# objectives are visible without a click. (The binding accepts the raw flag;
# bd.imgui only names the Cond/Col/WindowFlags constant groups.)
_HEADER_DEFAULT_OPEN = 32


class JournalUI:
    """Immediate-mode ImGui window rendering a quest log.

    ``log`` defaults to the shared :data:`bd_quests.log` singleton;
    ``title`` is the window title. The window starts visible; the close
    button and :meth:`toggle` stay in sync because ``begin``'s ``open``
    result is fed back into :attr:`visible` every frame.
    """

    def __init__(self, log: Optional[QuestLog] = None, title: str = "Journal") -> None:
        self.log: QuestLog = log if log is not None else _default_log
        self.title: str = str(title)
        self.visible: bool = True

    def toggle(self) -> bool:
        """Flip visibility and return the new state."""
        self.visible = not self.visible
        return self.visible

    def draw(self) -> None:
        """Submit the journal window. Call from an ``imgui_frame`` handler.

        Internally guarded: a rendering error produces one ``bd.warn`` per
        frame at worst and always balances ``begin`` with ``end``, so a
        broken quest entry cannot corrupt the ImGui window stack.
        """
        if not self.visible:
            return
        imgui = bd.imgui
        imgui.set_next_window_pos(40.0, 60.0, imgui.Cond.FirstUseEver)
        imgui.set_next_window_size(400.0, 0.0, imgui.Cond.FirstUseEver)
        expanded, self.visible = imgui.begin(self.title, self.visible)
        try:
            if expanded:
                self._draw_contents(imgui)
        except Exception as exc:
            bd.warn(f"bd_quests journal draw error: {exc!r}")
        finally:
            imgui.end()

    # -- internals -------------------------------------------------------------

    def _draw_contents(self, imgui) -> None:
        active = self.log.active()
        finished = [quest for quest in self.log.all()
                    if quest.state in (Quest.COMPLETED, Quest.FAILED)]
        if not active and not finished:
            imgui.text_disabled("No quests yet.")
            return
        for quest in active:
            self._draw_active_quest(imgui, quest)
        if finished:
            if active:
                imgui.separator()
            if imgui.collapsing_header("Completed / Failed"):
                imgui.indent()
                for quest in finished:
                    self._draw_finished_quest(imgui, quest)
                imgui.unindent()

    def _draw_active_quest(self, imgui, quest: Quest) -> None:
        # "###id" keeps the ImGui identity stable if two quests share a name.
        if not imgui.collapsing_header(f"{quest.name}###{quest.id}",
                                       flags=_HEADER_DEFAULT_OPEN):
            return
        imgui.indent()
        if quest.description:
            imgui.text_wrapped(quest.description)
        if quest.giver:
            imgui.text_colored(*_GIVER_COLOR, f"Giver: {quest.giver}")
        if quest.faction:
            imgui.text_colored(*_FACTION_COLOR, f"Faction: {quest.faction}")
        current = quest.current_objective
        for obj in quest.objectives:
            label = obj.text
            if obj.count > 1:
                label = f"{obj.text} ({obj.progress}/{obj.count})"
            label = f"{label}###{quest.id}:{obj.id}"
            if obj.done:
                # The checkbox reflects engine state only: the returned
                # (changed, value) tuple is deliberately discarded, so the
                # widget is a read-only indicator.
                imgui.checkbox(label, True)
            elif quest.parallel or obj is current:
                imgui.checkbox(label, False)
            else:
                # Locked future step of a sequential quest.
                imgui.text_disabled(obj.text)
        imgui.unindent()

    def _draw_finished_quest(self, imgui, quest: Quest) -> None:
        if quest.state == Quest.COMPLETED:
            imgui.text_colored(*_COMPLETED_COLOR, quest.name)
        else:
            reason = f" — {quest.fail_reason}" if quest.fail_reason else ""
            imgui.text_colored(*_FAILED_COLOR, f"{quest.name} (failed{reason})")


# --- console/key toggle binding ------------------------------------------------

_default_journal: Optional[JournalUI] = None
#: command name -> the JournalUI that command toggles (one handler each).
_bound_toggle_commands: Dict[str, JournalUI] = {}


def default_journal() -> JournalUI:
    """The shared :class:`JournalUI` that :func:`bind_journal_toggle` toggles.

    Created lazily on first use; it renders the shared
    :data:`bd_quests.log` singleton.
    """
    global _default_journal
    if _default_journal is None:
        _default_journal = JournalUI()
    return _default_journal


def bind_journal_toggle(key: Optional[str] = None,
                        command: str = "toggle_journal",
                        journal: Optional[JournalUI] = None) -> JournalUI:
    """Bind a console command (and optional key) to toggle a journal window.

    Registers the console alias ``command`` as ``pyui <command>``, so
    running ``toggle_journal`` at the console fires a ``ui_command``
    event whose ``command`` payload equals the alias name; a handler
    registered here flips :attr:`JournalUI.visible` on the target
    journal (the shared :func:`default_journal` unless ``journal`` is
    given). When ``key`` is given (e.g. ``"j"``), a ``bind`` is queued
    too. Returns the journal being toggled.

    Safe to call from ``engine_start`` (``bd.execute`` queues console
    commands before any map is loaded) and idempotent per command name:
    calling again with the same ``command`` re-issues the alias/bind but
    does not register a second ``ui_command`` handler (the engine offers
    no event unregistration). One toggle target per command name —
    rebinding a command to a different journal warns and keeps the
    first.
    """
    command = str(command)
    target = journal if journal is not None else default_journal()
    try:
        bd.execute(f'alias {command} "pyui {command}"')
        if key:
            bd.execute(f"bind {key} {command}")
    except Exception as exc:
        bd.warn(f"bd_quests: could not register the journal toggle "
                f"alias/bind: {exc!r}")
    existing = _bound_toggle_commands.get(command)
    if existing is not None:
        if existing is not target:
            bd.warn(f"bd_quests: {command!r} already toggles another "
                    f"journal; keeping the first binding")
        return existing
    _bound_toggle_commands[command] = target

    @bd.on("ui_command")
    def _on_ui_command(event, _command=command, _journal=target):
        if event.get("command") != _command:
            return
        try:
            _journal.toggle()
        except Exception as exc:
            bd.warn(f"bd_quests: journal toggle failed: {exc!r}")

    return target
