"""Dear ImGui HUD overlay for :mod:`bd_vtm`.

:class:`VtMHud` renders a :class:`~bd_vtm.VtMState` as an ImGui window:
a red blood-pool bar with ``current/max`` overlay, hunger as five
ASCII pips (``***--``), humanity and masquerade bars, and a list of the
registered disciplines with their remaining cooldowns (``ready`` or
``123t``).

Like every ``bd.imgui`` consumer, :meth:`VtMHud.draw` is only legal
inside an ``imgui_frame`` handler::

    import biaseddoom as bd
    import bd_vtm
    from bd_vtm.hud import VtMHud

    state = bd_vtm.VtMState(generation=13)
    hud = VtMHud(state)

    @bd.on("imgui_frame")
    def draw(event):
        hud.draw()

Visibility flips with :meth:`VtMHud.toggle` or the window's own close
button (they stay in sync automatically). To drive the toggle from the
console or a key bind, use :func:`bind_hud_toggle`: console
``alias``/``bind`` can only run *console commands*, so the helper routes
through the engine's ``pyui`` command and the ``ui_command`` event::

    bd_vtm.bind_hud_toggle(hud, key="h")
    # the player can now press H or run `toggle_hud` at the console
"""

from __future__ import annotations

from typing import Dict, Optional

import biaseddoom as bd

from . import Hunger, Humanity, Masquerade, VtMState

# ImGuiCol_PlotHistogram in the vendored ImGui (libraries/imgui/imgui.h);
# the binding bounds-checks raw indices against ImGuiCol_COUNT, so the
# worst case of a vendored-update index shift is a recolored bar.
# progress_bar() takes its fill color from this entry.
# Vendored ImGui 1.92.8 enum: ImGuiCol_PlotHistogram is 46 (44 is PlotLines;
# an earlier CheckboxSelectedBg entry shifted the classic indices).
_COL_PLOT_HISTOGRAM = 46

_BLOOD_COLOR = (0.62, 0.06, 0.09, 1.0)
_HUMANITY_COLOR = (0.85, 0.80, 0.62, 1.0)
_MASQUERADE_COLOR = (0.45, 0.30, 0.65, 1.0)
_HUNGER_CALM = (0.55, 0.85, 0.45, 1.0)
_HUNGER_HIGH = (0.95, 0.65, 0.20, 1.0)
_HUNGER_FRENZY = (0.95, 0.25, 0.20, 1.0)


class VtMHud:
    """Immediate-mode ImGui window rendering a :class:`VtMState`.

    The window starts visible; the close button and :meth:`toggle` stay
    in sync because ``begin``'s ``open`` result is fed back into
    :attr:`visible` every frame.
    """

    def __init__(self, state: VtMState, title: str = "Vitae",
                 show_disciplines: bool = True) -> None:
        self.state: VtMState = state
        self.title: str = str(title)
        self.visible: bool = True
        self.show_disciplines: bool = bool(show_disciplines)

    def toggle(self) -> bool:
        """Flip visibility and return the new state."""
        self.visible = not self.visible
        return self.visible

    def draw(self) -> None:
        """Submit the HUD window. Call from an ``imgui_frame`` handler.

        Internally guarded: a rendering error produces one ``bd.warn``
        per frame at worst and always balances ``begin`` with ``end``.
        """
        if not self.visible:
            return
        imgui = bd.imgui
        imgui.set_next_window_pos(40.0, 420.0, imgui.Cond.FirstUseEver)
        imgui.set_next_window_size(300.0, 0.0, imgui.Cond.FirstUseEver)
        expanded, self.visible = imgui.begin(self.title, self.visible)
        try:
            if expanded:
                self._draw_contents(imgui)
        except Exception as exc:
            bd.warn(f"bd_vtm hud draw error: {exc!r}")
        finally:
            imgui.end()

    # -- internals -------------------------------------------------------------

    def _draw_contents(self, imgui) -> None:
        state = self.state

        blood = state.blood
        blood_frac = blood.current / blood.max if blood.max > 0 else 0.0
        imgui.push_style_color(_COL_PLOT_HISTOGRAM, *_BLOOD_COLOR)
        imgui.progress_bar(blood_frac,
                           overlay=f"Blood {blood.current}/{blood.max} "
                                   f"(gen {blood.generation})")
        imgui.pop_style_color()

        level = state.hunger.level
        pips = "*" * level + "-" * (Hunger.MAX_LEVEL - level)
        if level >= Hunger.FRENZY_MIN:
            color = _HUNGER_FRENZY
        elif level > 0:
            color = _HUNGER_HIGH
        else:
            color = _HUNGER_CALM
        imgui.text_colored(*color, f"Hunger {pips} ({level}/{Hunger.MAX_LEVEL})")

        humanity = state.humanity
        imgui.push_style_color(_COL_PLOT_HISTOGRAM, *_HUMANITY_COLOR)
        imgui.progress_bar(humanity.rating / Humanity.MAX_RATING,
                           overlay=f"Humanity {humanity.rating}/"
                                   f"{Humanity.MAX_RATING}")
        imgui.pop_style_color()

        masquerade = state.masquerade
        imgui.push_style_color(_COL_PLOT_HISTOGRAM, *_MASQUERADE_COLOR)
        imgui.progress_bar(masquerade.level / Masquerade.MAX_LEVEL,
                           overlay=f"Masquerade {masquerade.level}/"
                                   f"{Masquerade.MAX_LEVEL}")
        imgui.pop_style_color()

        if self.show_disciplines and state.disciplines.all():
            imgui.separator()
            for discipline in state.disciplines.all():
                remaining = discipline.ready_in()
                if remaining > 0:
                    imgui.text_disabled(f"{discipline.name}: {remaining}t")
                else:
                    imgui.text(f"{discipline.name}: ready "
                               f"({discipline.blood_cost} blood)")


# --- console/key toggle binding ------------------------------------------------

#: command name -> the VtMHud that command toggles (one handler each).
_bound_toggle_commands: Dict[str, VtMHud] = {}


def bind_hud_toggle(hud: VtMHud, key: Optional[str] = None,
                    command: str = "toggle_hud") -> VtMHud:
    """Bind a console command (and optional key) to toggle a HUD window.

    Registers the console alias ``command`` as ``pyui <command>``, so
    running ``toggle_hud`` at the console fires a ``ui_command`` event
    whose ``command`` payload equals the alias name; a handler
    registered here flips :attr:`VtMHud.visible` on ``hud``. When
    ``key`` is given (e.g. ``"h"``), a ``bind`` is queued too. Returns
    ``hud``. Mirrors ``bd_quests.journal_ui.bind_journal_toggle``.

    Safe to call from ``engine_start`` (``bd.execute`` queues console
    commands before any map is loaded) and idempotent per command name:
    calling again with the same ``command`` re-issues the alias/bind but
    does not register a second ``ui_command`` handler (the engine offers
    no event unregistration). One toggle target per command name —
    rebinding a command to a different HUD warns and keeps the first.
    """
    command = str(command)
    try:
        bd.execute(f'alias {command} "pyui {command}"')
        if key:
            bd.execute(f"bind {key} {command}")
    except Exception as exc:
        bd.warn(f"bd_vtm: could not register the HUD toggle alias/bind: "
                f"{exc!r}")
    existing = _bound_toggle_commands.get(command)
    if existing is not None:
        if existing is not hud:
            bd.warn(f"bd_vtm: {command!r} already toggles another HUD; "
                    f"keeping the first binding")
        return existing
    _bound_toggle_commands[command] = hud

    @bd.on("ui_command")
    def _on_ui_command(event, _command=command, _hud=hud):
        if event.get("command") != _command:
            return
        try:
            _hud.toggle()
        except Exception as exc:
            bd.warn(f"bd_vtm: HUD toggle failed: {exc!r}")

    return hud
