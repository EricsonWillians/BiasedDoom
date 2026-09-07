"""The Sunken Crypt — bd_horror-themed ImGui interface.

The character sheet is rendered as a *reliquary*: a near-black ossuary
window over dried-blood accents, in three sections — "Vessel" (the
ability scores as key/value reliquary plates), "Vitality" (blood and
ember bars for HP and XP, plus the party's resolve and the crypt's
dread), and "Omens" (the roll log as colored portents: sickly green for
successes, dried blood for failures, ember for criticals). The party
sheet is styled the same way, with a selectable roster of the dead and
the walking.

Everything is a no-op headless: ``imgui_frame`` never fires under
``-headless``, and :func:`draw_frame` also early-outs on
``bd.headless()`` so autotests never depend on the overlay.
"""

from __future__ import annotations

from typing import Any, List, Optional

import biaseddoom as bd
import bd_dnd
from bd_horror import theme, toasts
from bd_horror.theme import PALETTE

try:
    import crypt_content as content
except ImportError:  # standalone manifest load: self-register the sibling
    content = bd.import_script("pyscripts/content.py",
                               module_name="crypt_content")

#: How many omen (roll log) entries the reliquary lists, newest first.
OMEN_ROWS = 14

# Roll-outcome -> PALETTE key: success is sickly, failure is blood,
# criticals burn ember, and unopposed rolls are bone.
_OMEN_TONES = {"hit": "ember", "miss": "ember"}


def _omen_tone(entry: Any) -> str:
    critical = entry.get("critical")
    if critical:
        return "ember"
    success = entry.get("success")
    if success is True:
        return "sickly"
    if success is False:
        return "blood"
    return "bone"


def _omen_line(entry: Any) -> str:
    total = entry.get("total")
    dc = entry.get("dc")
    verdict = ""
    if entry.get("critical") == "hit":
        verdict = " CRIT!"
    elif entry.get("critical") == "miss":
        verdict = " fumble"
    dc_text = f" vs DC {dc}" if dc is not None else ""
    detail = entry.get("detail") or ""
    suffix = f"  [{detail}]" if detail else ""
    return f"{entry.get('label', 'roll')}: {total}{dc_text}{verdict}{suffix}"


def _xp_fraction(character: "bd_dnd.Character") -> float:
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


def _xp_overlay(character: "bd_dnd.Character") -> str:
    next_xp = character.xp_for_next_level()
    if next_xp is None:
        return f"XP {character.xp} (MOST HIGH)"
    return f"XP {character.xp}/{next_xp}"


def _draw_reliquary_body(character: "bd_dnd.Character", epithet: str = "",
                         horror: Any = None, show_omens: bool = True,
                         key_width: float = 130.0) -> None:
    """Draw the three reliquary sections for one character.

    Only legal between ``imgui.begin`` and ``imgui.end`` with the horror
    theme applied. ``key_width`` narrows the key column for the party
    chapel's slimmer right-hand pane.
    """
    imgui = bd.imgui
    theme.section("Vessel")
    theme.kv_row("Name", character.name, key_width=key_width)
    if epithet:
        theme.kv_row("Epithet", epithet, key_width=key_width)
    theme.kv_row("Station",
                 f"level {character.level}  (proficiency "
                 f"+{character.proficiency})", key_width=key_width)
    for name in bd_dnd.ABILITIES:
        score = character.abilities.score(name)
        mod = character.abilities.mod(name)
        theme.kv_row(name.upper(), f"{score}  ({mod:+d})",
                     key_width=key_width)

    theme.section("Vitality")
    hp_frac = (character.hp / character.max_hp) if character.max_hp else 0.0
    theme.bar("Blood", hp_frac, overlay=f"HP {character.hp}/{character.max_hp}",
              tone="blood", pulse=hp_frac <= 0.25)
    theme.bar("Experience", _xp_fraction(character),
              overlay=_xp_overlay(character), tone="ember")
    if horror is not None:
        dread = horror.dread.level
        theme.bar("Dread", dread / 100.0, overlay=f"{dread:.0f}%",
                  tone="bruise", pulse=dread >= 75)
    for pool in sorted(character.resource_max):
        current = character.resources.get(pool, 0)
        theme.kv_row(pool.capitalize(),
                     f"{current}/{character.resource_max[pool]}",
                     key_width=key_width)

    if show_omens:
        theme.section("Omens")
        log = character.roll_log
        if not log:
            theme.faded_text("The crypt has not spoken yet.")
        else:
            if imgui.begin_child("##crypt_omens", size=(0.0, 150.0),
                                 border=True):
                for entry in reversed(log[-OMEN_ROWS:]):
                    imgui.text_colored(*PALETTE[_omen_tone(entry)],
                                       _omen_line(entry))
            imgui.end_child()


class ReliquarySheet:
    """The hero's character sheet as a bd_horror reliquary window.

    Interface-compatible with ``bd_dnd.sheet.CharacterSheet`` (``visible``
    /``toggle()``/``draw()``) so ``bind_sheet_toggle`` and the autotest's
    toggle chain work unchanged. The window starts visible; the close
    button and :meth:`toggle` stay in sync because ``begin``'s ``open``
    result is fed back into :attr:`visible` every frame.
    """

    def __init__(self, character: "bd_dnd.Character", state: Any = None,
                 title: str = "Reliquary", horror: Any = None) -> None:
        self.character: "bd_dnd.Character" = character
        self.state: Any = state
        self.title: str = str(title)
        self.horror: Any = horror
        self.visible: bool = True

    def toggle(self) -> bool:
        """Flip visibility and return the new state."""
        self.visible = not self.visible
        return self.visible

    def draw(self) -> None:
        """Submit the reliquary window. ``imgui_frame`` handlers only."""
        if not self.visible:
            return
        imgui = bd.imgui
        theme.apply()
        try:
            expanded, self.visible = theme.begin_window(
                f"{self.title}###sunken_crypt_sheet",
                pos=(40.0, 60.0), size=(360.0, 0.0), open=self.visible)
            try:
                if expanded:
                    _draw_reliquary_body(self.character,
                                         epithet=content.HERO_EPITHET,
                                         horror=self.horror)
            finally:
                imgui.end()
        except Exception as exc:
            bd.warn(f"sunken crypt reliquary draw error: {exc!r}")
        finally:
            theme.clear()


class PartyReliquary:
    """The party roster as a bd_horror side chapel.

    A selectable roster on the left (clicking a member calls
    ``party.set_active``) and the active member's reliquary body on the
    right. Members at 0 hp (their companion slain) are rendered faded.
    Same ``visible``/``toggle()``/``draw()`` contract as
    :class:`ReliquarySheet`.
    """

    EPITHETS = {content.HERO_NAME: content.HERO_EPITHET,
                content.WARDEN_NAME: content.WARDEN_EPITHET}

    def __init__(self, party: "bd_dnd.Party", state: Any = None,
                 title: str = "The Last Descent") -> None:
        self.party: "bd_dnd.Party" = party
        self.state: Any = state
        self.title: str = str(title)
        self.visible: bool = True

    def toggle(self) -> bool:
        """Flip visibility and return the new state."""
        self.visible = not self.visible
        return self.visible

    def draw(self) -> None:
        """Submit the party window. ``imgui_frame`` handlers only."""
        if not self.visible:
            return
        imgui = bd.imgui
        theme.apply()
        try:
            expanded, self.visible = theme.begin_window(
                f"{self.title}###sunken_crypt_party",
                pos=(420.0, 60.0), size=(560.0, 0.0), open=self.visible,
                flags=imgui.WindowFlags.AlwaysAutoResize)
            try:
                if expanded:
                    self._draw_contents(imgui)
            finally:
                imgui.end()
        except Exception as exc:
            bd.warn(f"sunken crypt party draw error: {exc!r}")
        finally:
            theme.clear()

    def _draw_contents(self, imgui: Any) -> None:
        members: List[Any] = self.party.members
        if not members:
            theme.faded_text("No one descends with you.")
            return
        if not imgui.begin_table("##crypt_party_layout", 2):
            return
        try:
            imgui.table_next_row()
            imgui.table_next_column()
            theme.section("Roster")
            for member in members:
                selected = member is self.party.active
                dead = member.hp <= 0
                label = (f"{member.name} (Lv {member.level})"
                         f"###crypt_member_{member.name}")
                if dead:
                    imgui.push_style_color(imgui.Col.Text,
                                           *PALETTE["marrow"])
                if imgui.selectable(label, selected):
                    try:
                        self.party.set_active(member.name)
                    except Exception as exc:
                        bd.warn(f"sunken crypt: set_active failed: {exc!r}")
                if dead:
                    imgui.pop_style_color()
                if dead:
                    theme.faded_text("  (fallen)")
            imgui.table_next_column()
            active = self.party.active
            if active is not None:
                _draw_reliquary_body(
                    active, epithet=self.EPITHETS.get(active.name, ""),
                    horror=None, show_omens=False, key_width=96.0)
        finally:
            imgui.end_table()


# --- frame glue --------------------------------------------------------------------

#: UI context: set once by main.py's setup and read every frame.
ctx: dict = {}


def draw_frame() -> None:
    """Draw the whole overlay for one ``imgui_frame`` event.

    No-op when headless or when every window is closed. The toast stack
    renders regardless of window visibility (it self-guards headless).
    """
    try:
        if bd.headless():
            return
    except Exception:
        pass
    sheet = ctx.get("sheet")
    if sheet is not None and sheet.visible:
        sheet.draw()
        ctx["sheet_drawn_ok"] = True
    party_sheet = ctx.get("party_sheet")
    if party_sheet is not None and party_sheet.visible:
        party_sheet.draw()
    toasts.draw_toasts()
