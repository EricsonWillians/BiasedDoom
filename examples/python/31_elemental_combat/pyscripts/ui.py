"""Pyre & Rime — bd_horror-themed ImGui combat HUD.

One window: the three element sigils (Pyre ember / Rime sickly / Rot
bruise, the active sigil ringed in blood — clicking one turns the
focus), the bearer's afflictions as per-element-toned bars with their
remaining tics overlaid, the kill/loot litany as crimson omen lines
(capped at six), and the souls bar. The toast stack renders beside it.

Everything is a no-op headless: ``imgui_frame`` never fires under
``-headless``, and :func:`draw_frame` also early-outs on
``bd.headless()`` so the autotest never depends on the overlay.
"""

from __future__ import annotations

from typing import Any

import biaseddoom as bd
import bd_rpg
from bd_horror import theme, toasts
from bd_horror.theme import PALETTE

try:
    import pyre_content as content
except ImportError:  # standalone manifest load: self-register the sibling
    content = bd.import_script("pyscripts/content.py",
                               module_name="pyre_content")
try:
    import pyre_systems as systems
except ImportError:
    systems = bd.import_script("pyscripts/systems.py",
                               module_name="pyre_systems")

#: How many litany lines the HUD lists (newest last).
LITANY_ROWS = 6

#: status name -> PALETTE key for its bar. Elemental afflictions take
#: their element's tone; the idol's ward is bone; anything else bleeds.
STATUS_TONES = {
    "burning": "ember",     # pyre
    "slowed": "sickly",     # rime
    "poisoned": "bruise",   # rot
    "stoneskin": "bone",
}

# ImGuiStyleVar_FrameBorderSize in the vendored ImGui (enum
# ImGuiStyleVar_, 0-based; see bd_horror.theme for the index table).
_VAR_FRAME_BORDER_SIZE = 13


def _tone_for_status(name: str) -> str:
    return STATUS_TONES.get(str(name), "blood")


def _sigil_button(imgui: Any, element: str, active: bool) -> bool:
    """One element sigil; True on the frame it is clicked.

    The button takes its element's tone (dimmed when inactive); the
    active sigil is ringed in blood via a thicker crimson frame border.
    """
    tone = PALETTE[content.ELEMENT_TONES[element]]
    if active:
        button_color = (tone[0], tone[1], tone[2], 1.0)
    else:
        button_color = (tone[0] * 0.35, tone[1] * 0.35, tone[2] * 0.35,
                        1.0)
    imgui.push_style_color(imgui.Col.Button, *button_color)
    if active:
        imgui.push_style_color(imgui.Col.Border, *PALETTE["crimson"])
        imgui.push_style_var(_VAR_FRAME_BORDER_SIZE, 2.0)
    try:
        clicked = imgui.button(element.upper(), w=76.0, h=0.0)
    finally:
        if active:
            imgui.pop_style_var()
            imgui.pop_style_color()
        imgui.pop_style_color()
    return clicked


def draw_frame(ctx: dict) -> None:
    """Draw the combat HUD for one ``imgui_frame`` event (no-op headless).

    ``ctx`` carries ``panel_visible`` (a one-element list, fed back from
    the window's close button) and sets ``panel_drawn_ok`` for the
    screenshot path.
    """
    try:
        if bd.headless():
            return
    except Exception:
        pass
    panel_visible = ctx["panel_visible"]
    imgui = bd.imgui
    if panel_visible[0]:
        theme.apply()
        try:
            expanded, panel_visible[0] = theme.begin_window(
                "Pyre & Rime###rite31", pos=(500.0, 60.0),
                size=(360.0, 0.0), open=panel_visible[0],
                flags=imgui.WindowFlags.AlwaysAutoResize)
            try:
                if expanded:
                    _draw_contents(imgui)
            finally:
                imgui.end()
        except Exception as exc:
            bd.warn(f"pyre & rime HUD draw error: {exc!r}")
        finally:
            theme.clear()
        ctx["panel_drawn_ok"] = True
    toasts.draw_toasts()


def _draw_contents(imgui: Any) -> None:
    theme.section("Elemental Focus")
    for i, element in enumerate(content.ELEMENTS):
        if i:
            imgui.same_line()
        if _sigil_button(imgui, element, systems.focus[0] == element):
            try:
                systems.focus[0] = element
                r, g, b = content.ELEMENT_COLORS[element]
                bd.screen_flash(r, g, b, 0.15)
            except Exception as exc:
                bd.warn(f"pyre & rime: sigil click failed: {exc!r}")
    theme.faded_text(content.ELEMENT_PROSE[systems.focus[0]])
    theme.kv_row("Rites", "[F] cycle focus   [G] burst")

    theme.section("Afflictions")
    pawn = systems.player_pawn()
    shown = False
    if pawn is not None:
        for name, info in sorted(systems.status.list(pawn).items()):
            shown = True
            duration = max(1, info["duration"])
            frac = max(0.0, min(1.0, info["remaining"] / duration))
            theme.bar("", frac, overlay=f"{name}  {info['remaining']}t",
                      tone=_tone_for_status(name))
    if not shown:
        theme.faded_text("(the flesh is quiet)")

    theme.section("Souls")
    xp = bd_rpg.kill_xp(0)
    theme.bar("", (xp % 100) / 100.0, overlay=f"{xp} souls gathered",
              tone="blood")

    theme.section("Litany")
    lines = systems.combat_log[-LITANY_ROWS:]
    if not lines:
        theme.faded_text(content.LITANY_QUIET)
    for line in lines:
        theme.omen_text(line)
