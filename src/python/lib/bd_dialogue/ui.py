"""Dear ImGui dialogue overlay for :mod:`bd_dialogue`.

:class:`DialogueUI` renders a :class:`~bd_dialogue.DialogueSession` as an
ImGui window: the NPC's live sprite portrait in a bordered child (via
``imgui.image(npc_ref)``, with a text fallback when the texture lookup
fails), the speaker name colored by faction reputation (when the session
carries ``factions`` + ``faction``), the wrapped body text, a ~3 s
skill-check result flash, and the visible choices as numbered selectable
rows with their annotations (``[LOCKED]``, ``(unavailable)``,
``(DC 14 Persuasion)``); locked/unavailable rows render in disabled
colors and cannot be chosen.

Like every ``bd.imgui`` consumer, :meth:`DialogueUI.draw` is only legal
inside an ``imgui_frame`` handler::

    import biaseddoom as bd
    from bd_dialogue.ui import DialogueUI

    ui = DialogueUI()

    @bd.on("imgui_frame")
    def draw(event):
        ui.draw()   # hidden unless a session is attached and active

Visibility
----------

There is no toggle binding: sessions drive visibility.
:meth:`attach` shows the window for a session, :meth:`detach` hides it,
and ``draw()`` hides itself automatically once the session ends (so a
conversation closed by a choice simply makes the window vanish). The
window has no close button — closing a conversation is a dialogue
decision (an explicit "Farewell." choice), not a window-manager action.

Input
-----

Choices are activated by mouse click or by ImGui's built-in keyboard
navigation (arrow keys + ENTER; the engine enables
``ImGuiConfigFlags_NavEnableKeyboard`` by default). The number prefixes
("1.", "2.", ...) are visual hints only: ``bd.imgui`` exposes no
per-key state query (no ``is_key_down``), so direct 1-9 hotkeys cannot
be implemented from Python without a C++ change.
"""

from __future__ import annotations

from typing import Any, Optional, Tuple

import biaseddoom as bd

from . import DialogueSession, _level_time

#: Speaker color when the NPC's faction is friendly toward the player.
_FRIENDLY_COLOR = (0.4, 0.9, 0.5, 1.0)
#: Speaker color when the NPC's faction is hostile toward the player.
_HOSTILE_COLOR = (0.95, 0.4, 0.35, 1.0)
#: Speaker color when no faction data applies (neutral/default).
_DEFAULT_SPEAKER_COLOR = (0.9, 0.75, 0.45, 1.0)
#: Result flash colors (success / failure).
_FLASH_OK_COLOR = (0.35, 0.9, 0.4, 1.0)
_FLASH_FAIL_COLOR = (0.95, 0.45, 0.35, 1.0)

#: Bordered portrait child size (logical pixels).
_PORTRAIT_W = 96.0
_PORTRAIT_H = 112.0


class DialogueUI:
    """Immediate-mode ImGui window rendering the active dialogue session.

    ``attach(session)`` binds a session (and auto-detaches any previous
    one); ``detach()`` clears it. ``draw()`` is a no-op when no session
    is attached or the attached session is no longer active — which is
    also what makes the window headless-safe: when ``imgui_frame`` never
    fires, ``draw()`` is simply never called.
    """

    def __init__(self, session: Optional[DialogueSession] = None,
                 title: str = "Dialogue") -> None:
        self.session: Optional[DialogueSession] = None
        self.title: str = str(title)
        if session is not None:
            self.attach(session)

    def attach(self, session: DialogueSession) -> DialogueSession:
        """Render ``session`` from now on; returns it."""
        self.session = session
        return session

    def detach(self) -> None:
        """Stop rendering any session."""
        self.session = None

    def draw(self) -> None:
        """Submit the dialogue window. Call from an ``imgui_frame`` handler.

        Internally guarded: a rendering error produces one ``bd.warn``
        per frame at worst and always balances ``begin`` with ``end``.
        """
        session = self.session
        if session is None:
            return
        if not session.active:
            self.session = None  # conversation over: hide and release
            return
        imgui = bd.imgui
        imgui.set_next_window_pos(60.0, 420.0, imgui.Cond.FirstUseEver)
        imgui.set_next_window_size(460.0, 0.0, imgui.Cond.FirstUseEver)
        expanded = imgui.begin(f"{self.title}###bd_dialogue",
                               flags=imgui.WindowFlags.NoCollapse)
        try:
            if expanded:
                self._draw_contents(imgui, session)
        except Exception as exc:
            bd.warn(f"bd_dialogue ui draw error: {exc!r}")
        finally:
            imgui.end()

    # -- internals -------------------------------------------------------------

    def _draw_contents(self, imgui, session: DialogueSession) -> None:
        node = session.active_node
        if node is None:
            return
        if node.portrait:
            self._draw_portrait(imgui, session)
        imgui.text_colored(*self._speaker_color(session), node.speaker)
        imgui.spacing()
        imgui.text_wrapped(node.text)
        self._draw_flash(imgui, session)
        imgui.separator()
        self._draw_choices(imgui, session)

    def _draw_portrait(self, imgui, session: DialogueSession) -> None:
        if not imgui.begin_child("npc_portrait", (_PORTRAIT_W, _PORTRAIT_H),
                                 border=True):
            imgui.end_child()
            return
        try:
            imgui.image(session.npc_ref, w=0.0, h=_PORTRAIT_H - 8.0)
        except Exception:
            # Stale handle mid-frame or a texture lookup failure: the
            # portrait is decorative, so degrade to text.
            imgui.text_disabled("(no portrait)")
        finally:
            imgui.end_child()

    def _speaker_color(self, session: DialogueSession) -> Tuple[float, ...]:
        if session.factions is None or session.faction is None:
            return _DEFAULT_SPEAKER_COLOR
        try:
            if session.factions.get(session.faction) is None:
                return _DEFAULT_SPEAKER_COLOR
            rep = int(session.factions.reputation(session.faction))
        except Exception:
            return _DEFAULT_SPEAKER_COLOR
        if rep >= 1:
            return _FRIENDLY_COLOR
        if rep <= -1:
            return _HOSTILE_COLOR
        return _DEFAULT_SPEAKER_COLOR

    def _draw_flash(self, imgui, session: DialogueSession) -> None:
        flash = session.last_check
        if flash is None:
            return
        if _level_time() > int(flash.get("until_tic", 0)):
            return
        color = _FLASH_OK_COLOR if flash.get("success") else _FLASH_FAIL_COLOR
        imgui.text_colored(*color, str(flash.get("text", "")))

    def _draw_choices(self, imgui, session: DialogueSession) -> None:
        entries = session.choices()
        if not entries:
            imgui.text_disabled("(say nothing)")
            return
        for index, (choice, enabled, annotation) in enumerate(entries):
            label = f"{index + 1}. {choice.text}"
            if annotation:
                label = f"{label} {annotation}"
            if not enabled:
                imgui.text_disabled(label)
                continue
            # "###" keeps the ImGui identity stable across renavigation.
            if imgui.selectable(f"{label}###choice_{index}", False):
                session.choose(index)
                break  # the choice list (or the session) may have changed
