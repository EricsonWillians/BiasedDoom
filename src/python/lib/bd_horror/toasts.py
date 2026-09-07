"""Diegetic toast notifications for BiasedDoom horror mods.

``bd_horror.toasts`` is a tiny queue of short-lived screen messages
("you found the rusted key", "something is watching") rendered as one
borderless, auto-sized ImGui window in the top-right corner. Symbols are
deliberately plain ASCII (``*`` ``+`` ``=`` ``!`` ``x``) because the
default ImGui font has no glyphs beyond Latin-1 — prettier markers render
as ``?``.

Usage::

    import biaseddoom as bd
    from bd_horror import toasts

    @bd.on("secret_found")
    def secret(event):
        toasts.toast("a hidden cache opens", kind="quest")

    @bd.on("imgui_frame")
    def draw(event):
        toasts.draw_toasts()     # no-op when the queue is empty

Queue semantics: at most :data:`MAX_VISIBLE` entries are on screen; a new
toast past the limit drops the *oldest* visible entry. Each entry fades in
over :data:`FADE_IN_TICS`, holds for :data:`HOLD_TICS`, and fades out over
:data:`FADE_OUT_TICS` of map time (``bd.level_time``). Every toast (even a
dropped one) is kept in the :data:`history` ring buffer (last 20) so
autotests can assert on them without rendering.

``toast()`` itself is legal from any event (it never touches ImGui; the
per-kind sound is guarded). ``draw_toasts()`` submits ImGui calls and is
therefore only legal inside an ``imgui_frame`` handler — which also means
it never runs headless, where ``imgui_frame`` never fires.
"""

from __future__ import annotations

from collections import deque
from typing import Any, Deque, Dict, List, Optional, Tuple

import biaseddoom as bd

from .theme import PALETTE

#: Visible-queue cap; overflow drops the oldest entry.
MAX_VISIBLE: int = 5
#: Fade-in / hold / fade-out lifetimes in 35 Hz map tics.
FADE_IN_TICS: int = 10
HOLD_TICS: int = 120
FADE_OUT_TICS: int = 20
#: How many past toasts the history ring keeps for autotests.
HISTORY_SIZE: int = 20

# ImGuiStyleVar_Alpha in the vendored ImGui 1.92.8 (libraries/imgui/
# imgui.h, enum ImGuiStyleVar_). Pushed per entry to fade it.
_VAR_ALPHA = 0

#: kind -> (ASCII symbol prefix, PALETTE key, UI sound, volume). Sounds are
#: stock Doom II logical names from wadsrc/static/filter/game-doomchex/
#: sndinfo.txt; "misc/secret" resolves to the engine-shipped
#: sounds/dssecret.flac, the rest map to lumps present in doom2.wad.
KINDS: Dict[str, Tuple[str, str, str, float]] = {
    "info":  ("*", "bone",    "switches/normbutn", 0.4),
    "quest": ("+", "ember",   "misc/secret",       0.5),
    "loot":  ("=", "sickly",  "misc/i_pkup",       0.5),
    "omen":  ("!", "crimson", "misc/spawn",        0.6),
    "harm":  ("x",  "wound",   "demon/pain",       0.5),
}

_queue: List[Dict[str, Any]] = []
history: Deque[Dict[str, Any]] = deque(maxlen=HISTORY_SIZE)

#: Last frame's toast-window (w, h) in pixels, used to right-align the
#: next frame (immediate-mode layout remembers no sizes).
_last_size: Tuple[float, float] = (280.0, 0.0)

#: Display width in pixels, measured once (see _display_width_of).
_display_width: Optional[float] = None


def _level_time() -> int:
    try:
        return bd.level_time()
    except Exception:
        return 0


def _alpha(entry: Dict[str, Any], now: int) -> float:
    """0.0 - 1.0 opacity for an entry at map time ``now``; -1 when expired."""
    age = now - int(entry["born"])
    if age < 0:
        age = 0  # map time rewound (checkpoint load): refade, don't expire
    if age < FADE_IN_TICS:
        return age / FADE_IN_TICS
    age -= FADE_IN_TICS
    if age < HOLD_TICS:
        return 1.0
    age -= HOLD_TICS
    if age < FADE_OUT_TICS:
        return 1.0 - age / FADE_OUT_TICS
    return -1.0


def _prune(now: Optional[int] = None) -> None:
    now = _level_time() if now is None else now
    alive = [entry for entry in _queue if _alpha(entry, now) >= 0.0]
    _queue[:] = alive


def toast(text: str, kind: str = "info", sound: bool = True) -> Dict[str, Any]:
    """Queue a toast; returns the entry dict. Callable from any event.

    ``kind`` must be one of :data:`KINDS` (``info``/``quest``/``loot``/
    ``omen``/``harm``) — an unknown kind raises ``ValueError`` so typos
    fail loudly in development. When the visible queue is full the oldest
    entry is dropped; everything still lands in :data:`history`.
    """
    kind = str(kind).lower()
    if kind not in KINDS:
        raise ValueError(f"bd_horror.toasts: unknown kind {kind!r} "
                         f"(expected one of {sorted(KINDS)})")
    entry = {"text": str(text), "kind": kind, "born": _level_time()}
    _queue.append(entry)
    history.append(entry)
    while len(_queue) > MAX_VISIBLE:
        _queue.pop(0)
    if sound:
        try:
            bd.play_ui_sound(KINDS[kind][2], volume=KINDS[kind][3])
        except Exception:
            pass  # no sound device / headless: toasts stay silent
    return entry


def pending_count() -> int:
    """Visible (not yet expired) toasts right now."""
    _prune()
    return len(_queue)


def clear() -> None:
    """Empty the visible queue. The history ring is kept for autotests."""
    _queue.clear()


def _display_width_of(imgui: Any) -> float:
    """Display width in pixels, measured once and cached.

    The bd.imgui binding exposes no viewport size query, so the first
    toast frame opens the (empty) main menu bar for one frame and reads
    its width — the menu bar always spans the full display. That is a
    single-frame artifact on the first frame a toast is visible; until the
    probe succeeds a conservative 1280 is assumed.
    """
    global _display_width
    if _display_width is None:
        try:
            if imgui.begin_main_menu_bar():
                _display_width = float(imgui.get_window_size()[0])
                imgui.end_main_menu_bar()
        except Exception:
            pass
    return _display_width or 1280.0


def draw_toasts(imgui: Any = None) -> None:
    """Render the queue as one top-right window. ``imgui_frame`` only.

    The window is borderless, immovable, auto-resizing, and translucent.
    The binding exposes no no-inputs window flag, so the stack remains
    technically clickable — it is small and hugging the corner, which is
    the documented trade-off. No-op when the queue is empty or the engine
    is headless (``imgui_frame`` never fires then anyway).
    """
    global _last_size
    imgui = imgui if imgui is not None else bd.imgui
    try:
        if bd.headless():
            return
    except Exception:
        pass
    _prune()
    if not _queue:
        return
    now = _level_time()
    flags = (imgui.WindowFlags.NoTitleBar | imgui.WindowFlags.NoResize
             | imgui.WindowFlags.NoMove | imgui.WindowFlags.AlwaysAutoResize
             | imgui.WindowFlags.NoScrollbar)
    # Right-align: measured display width minus last frame's window width
    # (AlwaysAutoResize recomputes it as the stack grows/shrinks; the
    # one-frame lag is invisible).
    x = max(8.0, _display_width_of(imgui) - _last_size[0] - 12.0)
    imgui.set_next_window_pos(x, 36.0, imgui.Cond.Always)
    imgui.set_next_window_bg_alpha(0.80)
    imgui.push_style_color(imgui.Col.WindowBg, 0.02, 0.01, 0.03, 0.85)
    try:
        expanded = imgui.begin("##bd_horror_toasts", flags=flags)
    except Exception:
        imgui.pop_style_color()
        raise
    try:
        if expanded:
            for entry in _queue:  # oldest on top, newest at the bottom
                alpha = _alpha(entry, now)
                if alpha < 0.0:
                    continue
                symbol, color_key = KINDS[entry["kind"]][0], KINDS[entry["kind"]][1]
                color = PALETTE.get(color_key, PALETTE["bone"])
                imgui.push_style_var(_VAR_ALPHA, max(0.0, min(1.0, alpha)))
                try:
                    imgui.text_colored(color[0], color[1], color[2], 1.0,
                                       f"{symbol} {entry['text']}")
                finally:
                    imgui.pop_style_var()
    except Exception as exc:
        bd.warn(f"bd_horror toasts draw error: {exc!r}")
    finally:
        try:
            _last_size = imgui.get_window_size()
        except Exception:
            pass
        imgui.end()
        imgui.pop_style_color()
