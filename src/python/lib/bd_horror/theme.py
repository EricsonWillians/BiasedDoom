"""Dear ImGui horror skin for BiasedDoom Python mods.

``bd_horror.theme`` restyles the engine's ``bd.imgui`` overlay for horror
mods: a near-black window over dried-blood accents, bone-colored text, and
square (rounding 0) frames. Everything is driven by the :data:`PALETTE`
dict of named ``(r, g, b, a)`` float tuples, so a mod can re-tint the whole
skin by editing one dict.

Frame-only contract
-------------------

Every function in this module submits ImGui calls, so — like every
``bd.imgui`` consumer — it is **only legal inside an ``imgui_frame``
handler** (the binding raises ``RuntimeError`` anywhere else; the one
exception is :func:`frame_image`'s internal ``image_size`` probe, which is
safe everywhere but only reached from here). The natural usage wraps your
window code in :func:`apply` / :func:`clear`::

    import biaseddoom as bd
    from bd_horror import theme

    @bd.on("imgui_frame")
    def draw(event):
        theme.apply()
        try:
            if theme.begin_window("The Dread", pos=(80, 60), size=(340, 0)):
                theme.section("Vitals")
                theme.bar("Blood", 0.65, overlay="65%", tone="blood")
                theme.bar("Panic", 0.30, tone="ember", pulse=True)
                theme.omen_text("Something stirs beneath the floor.")
                theme.kv_row("Kills", "12")
                theme.frame_image("PISGA0", size=64)
            bd.imgui.end()
        finally:
            theme.clear()

:func:`apply` pushes the full style-color set plus the style vars and
remembers exactly how many it pushed; :func:`clear` pops exactly that many.
Nesting ``apply`` twice without ``clear`` warns and no-ops, so an
unbalanced frame can never corrupt the global style stack.

All helpers accept an optional trailing ``imgui=`` module reference
(defaulting to ``bd.imgui``) so tests can pass a recording double.
"""

from __future__ import annotations

import math
from typing import Any, Dict, List, Optional, Tuple

import biaseddoom as bd

# ImGuiCol_PlotHistogram in the vendored ImGui 1.92.8
# (libraries/imgui/imgui.h, enum ImGuiCol_: Text=0 ... PlotHistogram=46).
# The binding bounds-checks raw indices against ImGuiCol_COUNT, so the
# worst case of a vendored-update index shift is a recolored bar fill.
# progress_bar() takes its fill color from this entry.
_COL_PLOT_HISTOGRAM = 46

# ImGuiStyleVar_* indices in the same vendored header (enum
# ImGuiStyleVar_, 0-based). The binding bounds-checks against
# ImGuiStyleVar_COUNT before pushing.
_VAR_ALPHA = 0
_VAR_WINDOW_PADDING = 2
_VAR_WINDOW_ROUNDING = 3
_VAR_WINDOW_BORDER_SIZE = 4
_VAR_FRAME_PADDING = 11
_VAR_FRAME_ROUNDING = 12
_VAR_FRAME_BORDER_SIZE = 13
_VAR_ITEM_SPACING = 14
_VAR_SCROLLBAR_SIZE = 18
_VAR_SCROLLBAR_ROUNDING = 19
_VAR_GRAB_ROUNDING = 22

#: Named horror colors as ImGui (r, g, b, a) float tuples (0.0 - 1.0).
#: Plain dict — mods may edit or extend it before calling apply().
PALETTE: Dict[str, Tuple[float, float, float, float]] = {
    "void":       (0.03, 0.02, 0.04, 1.0),   # near-black with a blue tinge
    "blood":      (0.48, 0.05, 0.08, 1.0),   # fresh arterial red
    "blood_dark": (0.22, 0.03, 0.05, 1.0),   # dried blood
    "crimson":    (0.72, 0.10, 0.12, 1.0),   # bright wound red
    "bone":       (0.82, 0.79, 0.70, 1.0),   # pale bone (body text)
    "marrow":     (0.55, 0.50, 0.42, 1.0),   # dimmed bone (disabled text)
    "ash":        (0.13, 0.12, 0.13, 1.0),   # cold gray (frame backgrounds)
    "ember":      (0.88, 0.42, 0.10, 1.0),   # candleflame orange
    "sickly":     (0.42, 0.55, 0.26, 1.0),   # jaundiced green
    "bruise":     (0.30, 0.18, 0.38, 1.0),   # deep violet
    "wound":      (0.86, 0.18, 0.16, 1.0),   # bright fresh blood (text-safe)
}


def _with_alpha(color: Tuple[float, float, float, float], alpha: float
                ) -> Tuple[float, float, float, float]:
    return (color[0], color[1], color[2], float(alpha))


def _color_rules(imgui: Any) -> List[Tuple[int, Tuple[float, float, float, float]]]:
    """The full horror style-color set, in push order."""
    col = imgui.Col
    pal = PALETTE
    return [
        (col.Text,            pal["bone"]),
        (col.TextDisabled,    pal["marrow"]),
        (col.WindowBg,        _with_alpha(pal["void"], 0.92)),
        (col.PopupBg,         _with_alpha(pal["void"], 0.95)),
        (col.Border,          pal["blood_dark"]),
        (col.FrameBg,         pal["ash"]),
        (col.FrameBgHovered,  pal["blood_dark"]),
        (col.FrameBgActive,   pal["blood"]),
        (col.TitleBg,         pal["blood_dark"]),
        (col.TitleBgActive,   pal["blood"]),
        (col.MenuBarBg,       _with_alpha(pal["void"], 0.90)),
        (col.CheckMark,       pal["ember"]),
        (col.SliderGrab,      pal["ember"]),
        (col.Button,          pal["blood_dark"]),
        (col.ButtonHovered,   pal["blood"]),
        (col.ButtonActive,    pal["crimson"]),
        (col.Header,          pal["blood_dark"]),
        (col.HeaderHovered,   pal["blood"]),
        (col.HeaderActive,    pal["crimson"]),
        (col.TableHeaderBg,   pal["blood_dark"]),
        (col.TableRowBg,      _with_alpha(pal["void"], 0.50)),
        (_COL_PLOT_HISTOGRAM, pal["blood"]),
    ]


# (var index, x, y-or-None-for-scalar), in push order.
_VAR_RULES: List[Tuple[int, float, Optional[float]]] = [
    (_VAR_WINDOW_PADDING,     10.0, 8.0),
    (_VAR_WINDOW_ROUNDING,    0.0,  None),
    (_VAR_WINDOW_BORDER_SIZE, 1.0,  None),
    (_VAR_FRAME_PADDING,      6.0,  4.0),
    (_VAR_FRAME_ROUNDING,     0.0,  None),
    (_VAR_FRAME_BORDER_SIZE,  1.0,  None),
    (_VAR_ITEM_SPACING,       8.0,  5.0),
    (_VAR_SCROLLBAR_SIZE,     10.0, None),
    (_VAR_SCROLLBAR_ROUNDING, 0.0,  None),
    (_VAR_GRAB_ROUNDING,      0.0,  None),
]

# Push counts tracked module-side so clear() pops exactly what apply()
# pushed, even if a future edit changes the rule tables.
_applied_colors: int = 0
_applied_vars: int = 0


def apply(imgui: Any = None) -> None:
    """Push the horror skin. ``imgui_frame`` handlers only.

    Idempotent in the sense that a second ``apply`` before ``clear`` warns
    once and does nothing (the style stack stays balanced).
    """
    global _applied_colors, _applied_vars
    imgui = imgui if imgui is not None else bd.imgui
    if _applied_colors or _applied_vars:
        bd.warn("bd_horror.theme: apply() called while already applied; "
                "call clear() first")
        return
    colors = _color_rules(imgui)
    pushed_colors = 0
    pushed_vars = 0
    try:
        for idx, color in colors:
            imgui.push_style_color(idx, *color)
            pushed_colors += 1
        for idx, x, y in _VAR_RULES:
            if y is None:
                imgui.push_style_var(idx, x)
            else:
                imgui.push_style_var(idx, x, y)
            pushed_vars += 1
    finally:
        # Even a partial push is recorded so clear() stays balanced.
        _applied_colors = pushed_colors
        _applied_vars = pushed_vars


def clear(imgui: Any = None) -> None:
    """Pop exactly what :func:`apply` pushed. ``imgui_frame`` only."""
    global _applied_colors, _applied_vars
    imgui = imgui if imgui is not None else bd.imgui
    if not _applied_colors and not _applied_vars:
        return
    try:
        if _applied_vars:
            imgui.pop_style_var(_applied_vars)
        if _applied_colors:
            imgui.pop_style_color(_applied_colors)
    finally:
        _applied_colors = 0
        _applied_vars = 0


# --- widget helpers (all imgui_frame-only) -------------------------------------


def begin_window(title: str, pos: Optional[Tuple[float, float]] = None,
                 size: Optional[Tuple[float, float]] = None, flags: int = 0,
                 open: Any = None, imgui: Any = None) -> Any:
    """Begin a themed window; returns ``begin()``'s result unchanged.

    ``pos``/``size`` are applied with ``Cond.FirstUseEver`` when given (the
    user may still move/resize afterwards). With ``open=None`` the result is
    a single bool (False when collapsed — still call ``imgui.end()``); with
    a bool it is the ``(expanded, open)`` tuple and the window gains a close
    button.
    """
    imgui = imgui if imgui is not None else bd.imgui
    if pos is not None:
        imgui.set_next_window_pos(float(pos[0]), float(pos[1]),
                                  imgui.Cond.FirstUseEver)
    if size is not None:
        imgui.set_next_window_size(float(size[0]), float(size[1]),
                                   imgui.Cond.FirstUseEver)
    if open is None:
        return imgui.begin(str(title), flags=flags)
    return imgui.begin(str(title), open, flags)


def section(text: str, imgui: Any = None) -> None:
    """Ornament header: ember-colored label over a full-width separator."""
    imgui = imgui if imgui is not None else bd.imgui
    imgui.text_colored(*PALETTE["ember"], str(text))
    imgui.separator()


def _level_time() -> int:
    try:
        return bd.level_time()
    except Exception:
        return 0


def bar(label: str, fraction: float, overlay: Optional[str] = None,
        tone: str = "blood", pulse: bool = False, imgui: Any = None) -> None:
    """Themed progress bar with a per-``tone`` fill color.

    ``tone`` is a :data:`PALETTE` key (``"blood"``, ``"ember"``,
    ``"sickly"``, ``"bone"``, ``"bruise"``; unknown tones fall back to
    blood). With ``pulse=True`` the fill alpha breathes with map time
    (one cycle per ~70 tics) — use it for low-health and rising-dread
    meters. The fill color rides the raw ``ImGuiCol_PlotHistogram`` entry
    (see the note at the top of this module).
    """
    imgui = imgui if imgui is not None else bd.imgui
    color = PALETTE.get(str(tone), PALETTE["blood"])
    if pulse:
        alpha = 0.55 + 0.35 * math.sin(_level_time() * 0.09)
        color = (color[0], color[1], color[2], max(0.15, min(1.0, alpha)))
    if label:
        imgui.text(str(label))
    imgui.push_style_color(_COL_PLOT_HISTOGRAM, *color)
    try:
        imgui.progress_bar(max(0.0, min(1.0, float(fraction))),
                           overlay=overlay)
    finally:
        imgui.pop_style_color()


def omen_text(text: str, imgui: Any = None) -> None:
    """Wrapped narrative text in crimson — for warnings and prophecies.

    Uses the brighter ``crimson`` (not the darker ``blood``) so the text
    stays readable against the near-black window background.
    """
    imgui = imgui if imgui is not None else bd.imgui
    imgui.push_style_color(imgui.Col.Text, *PALETTE["crimson"])
    try:
        imgui.text_wrapped(str(text))
    finally:
        imgui.pop_style_color()


def faded_text(text: str, imgui: Any = None) -> None:
    """Dimmed (marrow) text — for spent or irrelevant entries."""
    imgui = imgui if imgui is not None else bd.imgui
    imgui.push_style_color(imgui.Col.Text, *PALETTE["marrow"])
    try:
        imgui.text_wrapped(str(text))
    finally:
        imgui.pop_style_color()


def kv_row(key: str, value: str, key_width: float = 130.0,
           imgui: Any = None) -> None:
    """Two-column key/value line: marrow key, bone value, one row."""
    imgui = imgui if imgui is not None else bd.imgui
    imgui.text_colored(*PALETTE["marrow"], str(key))
    imgui.same_line(float(key_width))
    imgui.text(str(value))


_frame_counter: int = 0


def frame_image(ref_or_name: Any, size: float = 64.0, imgui: Any = None) -> None:
    """Bordered portrait plate for a texture lump name or Actor handle.

    Draws the texture (aspect-preserved, ``size`` as the long edge) on a
    void-dark plate with a blood-dark frame, via a bordered child region
    (its background/border come from the active style, so the plate follows
    the theme). Unknown texture names raise ``ValueError``, like
    ``imgui.image`` itself.
    """
    global _frame_counter
    imgui = imgui if imgui is not None else bd.imgui
    nat_w, nat_h = imgui.image_size(ref_or_name)
    scale = float(size) / max(float(nat_w), float(nat_h), 1.0)
    w = max(1.0, float(nat_w) * scale)
    h = max(1.0, float(nat_h) * scale)
    pad = 6.0
    _frame_counter += 1
    imgui.push_style_color(imgui.Col.FrameBg,
                           *_with_alpha(PALETTE["void"], 0.9))
    imgui.push_style_color(imgui.Col.Border, *PALETTE["blood_dark"])
    try:
        imgui.begin_child(f"##bdh_frame{_frame_counter}",
                          (w + 2 * pad, h + 2 * pad), border=True)
        try:
            imgui.indent(pad)
            imgui.image(ref_or_name, w, h)
            imgui.unindent(pad)
        finally:
            imgui.end_child()
    finally:
        imgui.pop_style_color(2)
