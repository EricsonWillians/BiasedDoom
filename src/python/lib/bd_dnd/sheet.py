"""Dear ImGui character sheet for :mod:`bd_dnd`.

:class:`CharacterSheet` renders a :class:`~bd_dnd.Character` as an ImGui
window: a name/level header with the proficiency bonus, HP and XP
progress bars (the XP bar shows progress toward the next level), the six
ability scores as a three-column table (name, score, modifier), any
per-rest resource pools, and a scrolling color-coded roll log (success
green / failure red / critical gold) fed by the character's
:attr:`~bd_dnd.Character.roll_log`.

Like every ``bd.imgui`` consumer, :meth:`CharacterSheet.draw` is only
legal inside an ``imgui_frame`` handler::

    import biaseddoom as bd
    import bd_dnd
    from bd_dnd.sheet import CharacterSheet

    hero = bd_dnd.Character("Crawler", bd_dnd.AbilityScores(str=16))
    sheet = CharacterSheet(hero)

    @bd.on("imgui_frame")
    def draw(event):
        sheet.draw()

Visibility flips with :meth:`CharacterSheet.toggle` or the window's own
close button (they stay in sync automatically). To drive the toggle from
the console or a key bind, use :func:`bind_sheet_toggle`: console
``alias``/``bind`` can only run *console commands*, so the helper routes
through the engine's ``pyui`` command and the ``ui_command`` event::

    bd_dnd.bind_sheet_toggle(sheet, key="k")
    # the player can now press K or run `toggle_sheet` at the console

:class:`PartySheet` renders a :class:`~bd_dnd.Party`: a selectable
roster column plus the active member's full sheet body beside it. Both
sheets share the same body renderer (:func:`_draw_character_body`).
"""

from __future__ import annotations

from typing import Any, Dict, Optional

import biaseddoom as bd

from . import ABILITIES, Character, Party

# ImGuiCol_PlotHistogram in the vendored ImGui (libraries/imgui/imgui.h);
# the binding bounds-checks raw indices against ImGuiCol_COUNT, so the
# worst case of a vendored-update index shift is a recolored bar.
# progress_bar() takes its fill color from this entry.
_COL_PLOT_HISTOGRAM = 44

_HP_COLOR = (0.75, 0.15, 0.15, 1.0)
_XP_COLOR = (0.85, 0.70, 0.20, 1.0)
_SUCCESS_COLOR = (0.45, 0.90, 0.45, 1.0)
_FAIL_COLOR = (0.95, 0.35, 0.30, 1.0)
_CRIT_COLOR = (1.00, 0.85, 0.25, 1.0)
_NEUTRAL_COLOR = (0.85, 0.85, 0.85, 1.0)

#: How many roll-log entries the sheet lists (newest first).
LOG_ROWS = 30


class CharacterSheet:
    """Immediate-mode ImGui window rendering a :class:`Character`.

    The window starts visible; the close button and :meth:`toggle` stay
    in sync because ``begin``'s ``open`` result is fed back into
    :attr:`visible` every frame. ``state`` is an optional
    :class:`~bd_dnd.CharacterState` — accepted for future-proofing and
    symmetry with the rest of the package; the sheet reads the character
    directly.
    """

    def __init__(self, character: Character, state: Any = None,
                 title: str = "Character Sheet") -> None:
        self.character: Character = character
        self.state: Any = state
        self.title: str = str(title)
        self.visible: bool = True

    def toggle(self) -> bool:
        """Flip visibility and return the new state."""
        self.visible = not self.visible
        return self.visible

    def draw(self) -> None:
        """Submit the sheet window. Call from an ``imgui_frame`` handler.

        Internally guarded: a rendering error produces one ``bd.warn``
        per frame at worst and always balances ``begin`` with ``end``.
        """
        if not self.visible:
            return
        imgui = bd.imgui
        imgui.set_next_window_pos(40.0, 60.0, imgui.Cond.FirstUseEver)
        imgui.set_next_window_size(340.0, 0.0, imgui.Cond.FirstUseEver)
        expanded, self.visible = imgui.begin(
            f"{self.title}###bd_dnd_sheet", self.visible)
        try:
            if expanded:
                self._draw_contents(imgui)
        except Exception as exc:
            bd.warn(f"bd_dnd sheet draw error: {exc!r}")
        finally:
            imgui.end()

    # -- internals -------------------------------------------------------------

    def _draw_contents(self, imgui) -> None:
        _draw_character_body(imgui, self.character)

    @staticmethod
    def _xp_fraction(character: Character) -> float:
        """Progress through the current level band, 0.0 - 1.0."""
        table = character.XP_TABLE
        next_xp = character.xp_for_next_level()
        if next_xp is None:
            return 1.0
        base = table[character.level - 1] if character.level - 1 < len(table) else 0
        span = next_xp - base
        if span <= 0:
            return 1.0
        return max(0.0, min(1.0, (character.xp - base) / span))

    @staticmethod
    def _xp_overlay(character: Character) -> str:
        next_xp = character.xp_for_next_level()
        if next_xp is None:
            return f"XP {character.xp} (MAX)"
        return f"XP {character.xp}/{next_xp}"

    @staticmethod
    def _draw_log_entry(imgui, entry) -> None:
        critical = entry.get("critical")
        success = entry.get("success")
        if critical:
            color = _CRIT_COLOR
        elif success is True:
            color = _SUCCESS_COLOR
        elif success is False:
            color = _FAIL_COLOR
        else:
            color = _NEUTRAL_COLOR
        total = entry.get("total")
        dc = entry.get("dc")
        verdict = ""
        if critical == "hit":
            verdict = " CRIT!"
        elif critical == "miss":
            verdict = " fumble"
        dc_text = f" vs DC {dc}" if dc is not None else ""
        detail = entry.get("detail") or ""
        suffix = f"  [{detail}]" if detail else ""
        imgui.text_colored(
            *color,
            f"{entry.get('label', 'roll')}: {total}{dc_text}{verdict}{suffix}")


# --- shared body renderer --------------------------------------------------------


def _draw_character_body(imgui, character: Character) -> None:
    """Draw the full sheet body for one character (no window frame).

    Shared by :class:`CharacterSheet` and :class:`PartySheet`; only legal
    between ``imgui.begin`` and ``imgui.end``.
    """
    imgui.text(f"{character.name} - level {character.level}")
    imgui.same_line()
    imgui.text_disabled(f"proficiency +{character.proficiency}")

    imgui.push_style_color(_COL_PLOT_HISTOGRAM, *_HP_COLOR)
    imgui.progress_bar(
        character.hp / character.max_hp if character.max_hp > 0 else 0.0,
        overlay=f"HP {character.hp}/{character.max_hp}")
    imgui.pop_style_color()

    imgui.push_style_color(_COL_PLOT_HISTOGRAM, *_XP_COLOR)
    imgui.progress_bar(CharacterSheet._xp_fraction(character),
                       overlay=CharacterSheet._xp_overlay(character))
    imgui.pop_style_color()

    if imgui.begin_table("bd_dnd_abilities", 3):
        try:
            imgui.table_setup_column("Ability")
            imgui.table_setup_column("Score")
            imgui.table_setup_column("Mod")
            imgui.table_headers_row()
            for name in ABILITIES:
                imgui.table_next_row()
                imgui.table_next_column()
                imgui.text(name.upper())
                imgui.table_next_column()
                imgui.text(str(character.abilities.score(name)))
                imgui.table_next_column()
                imgui.text(f"{character.abilities.mod(name):+d}")
        finally:
            imgui.end_table()

    if character.resource_max:
        imgui.separator()
        pools = []
        for name in sorted(character.resource_max):
            current = character.resources.get(name, 0)
            pools.append(f"{name} {current}/{character.resource_max[name]}")
        imgui.text("Resources: " + ", ".join(pools))

    imgui.separator()
    imgui.text_disabled(f"Roll log ({len(character.roll_log)})")
    if imgui.begin_child("bd_dnd_roll_log_child", size=(0.0, 160.0),
                         border=True):
        for entry in reversed(character.roll_log[-LOG_ROWS:]):
            CharacterSheet._draw_log_entry(imgui, entry)
    imgui.end_child()


# --- party sheet -----------------------------------------------------------------


class PartySheet:
    """Immediate-mode ImGui window rendering a :class:`Party`.

    A two-column layout: a selectable roster on the left (clicking a
    member calls ``party.set_active``) and the active member's full
    sheet body on the right (the same renderer
    :class:`CharacterSheet` uses). The window starts visible; the close
    button and :meth:`toggle` stay in sync because ``begin``'s ``open``
    result is fed back into :attr:`visible` every frame. ``state`` is an
    optional :class:`~bd_dnd.PartyState` — accepted for future-proofing
    and symmetry; the sheet reads the party directly.
    """

    def __init__(self, party: Party, state: Any = None,
                 title: str = "Party") -> None:
        self.party: Party = party
        self.state: Any = state
        self.title: str = str(title)
        self.visible: bool = True

    def toggle(self) -> bool:
        """Flip visibility and return the new state."""
        self.visible = not self.visible
        return self.visible

    def draw(self) -> None:
        """Submit the party window. Call from an ``imgui_frame`` handler.

        Internally guarded: a rendering error produces one ``bd.warn``
        per frame at worst and always balances ``begin`` with ``end``.
        """
        if not self.visible:
            return
        imgui = bd.imgui
        imgui.set_next_window_pos(40.0, 60.0, imgui.Cond.FirstUseEver)
        imgui.set_next_window_size(560.0, 0.0, imgui.Cond.FirstUseEver)
        expanded, self.visible = imgui.begin(
            f"{self.title}###bd_dnd_party_sheet", self.visible)
        try:
            if expanded:
                self._draw_contents(imgui)
        except Exception as exc:
            bd.warn(f"bd_dnd party sheet draw error: {exc!r}")
        finally:
            imgui.end()

    # -- internals -------------------------------------------------------------

    def _draw_contents(self, imgui) -> None:
        members = self.party.members
        if not members:
            imgui.text_disabled("The party is empty.")
            return
        if not imgui.begin_table("bd_dnd_party_layout", 2):
            return
        try:
            imgui.table_next_row()
            imgui.table_next_column()
            imgui.text_disabled(f"{self.party.name}")
            for member in members:
                selected = member is self.party.active
                label = (f"{member.name} (Lv {member.level})"
                         f"###party_member_{member.name}")
                if imgui.selectable(label, selected):
                    try:
                        self.party.set_active(member.name)
                    except Exception as exc:
                        bd.warn(f"bd_dnd: party set_active failed: {exc!r}")
            imgui.table_next_column()
            active = self.party.active
            if active is not None:
                _draw_character_body(imgui, active)
        finally:
            imgui.end_table()


# --- console/key toggle binding ------------------------------------------------

#: command name -> the sheet-like object that command toggles.
_bound_toggle_commands: Dict[str, Any] = {}


def bind_sheet_toggle(sheet: Any, key: Optional[str] = None,
                      command: str = "toggle_sheet") -> Any:
    """Bind a console command (and optional key) to toggle a sheet window.

    Registers the console alias ``command`` as ``pyui <command>``, so
    running ``toggle_sheet`` at the console fires a ``ui_command`` event
    whose ``command`` payload equals the alias name; a handler
    registered here flips ``visible`` on ``sheet`` (a
    :class:`CharacterSheet` or :class:`PartySheet` — anything with a
    ``toggle()``). When ``key`` is given (e.g. ``"k"``), a ``bind`` is
    queued too. Returns ``sheet``. Mirrors
    ``bd_quests.journal_ui.bind_journal_toggle``.

    Safe to call from ``engine_start`` (``bd.execute`` queues console
    commands before any map is loaded) and idempotent per command name:
    calling again with the same ``command`` re-issues the alias/bind but
    does not register a second ``ui_command`` handler (the engine offers
    no event unregistration). One toggle target per command name —
    rebinding a command to a different sheet warns and keeps the first.
    """
    command = str(command)
    try:
        bd.execute(f'alias {command} "pyui {command}"')
        if key:
            bd.execute(f"bind {key} {command}")
    except Exception as exc:
        bd.warn(f"bd_dnd: could not register the sheet toggle alias/bind: "
                f"{exc!r}")
    existing = _bound_toggle_commands.get(command)
    if existing is not None:
        if existing is not sheet:
            bd.warn(f"bd_dnd: {command!r} already toggles another sheet; "
                    f"keeping the first binding")
        return existing
    _bound_toggle_commands[command] = sheet

    @bd.on("ui_command")
    def _on_ui_command(event, _command=command, _sheet=sheet):
        if event.get("command") != _command:
            return
        try:
            _sheet.toggle()
        except Exception as exc:
            bd.warn(f"bd_dnd: sheet toggle failed: {exc!r}")

    return sheet
