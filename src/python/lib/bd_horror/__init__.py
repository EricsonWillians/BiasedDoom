"""Horror UX framework for BiasedDoom Python mods.

``bd_horror`` is an engine-shipped, dependency-free package (it only needs
``biaseddoom``) that gives mods a coherent horror presentation and
atmosphere layer:

- **ImGui skin.** :mod:`bd_horror.theme` pushes a full horror style-color
  and style-var set over ``bd.imgui`` (near-black windows, dried-blood
  accents, bone text, square frames) and adds thin widget helpers:
  :func:`~bd_horror.theme.begin_window`, :func:`~bd_horror.theme.section`,
  :func:`~bd_horror.theme.bar` (per-tone fills, optional pulse),
  :func:`~bd_horror.theme.omen_text`, :func:`~bd_horror.theme.kv_row`,
  and :func:`~bd_horror.theme.frame_image` portrait plates. All of it is
  only legal inside ``imgui_frame`` handlers.
- **Toasts.** :mod:`bd_horror.toasts` queues short diegetic notifications
  (kinds ``info``/``quest``/``loot``/``omen``/``harm`` with per-kind
  palette colors, ASCII symbol prefixes, and stock-Doom UI sounds),
  rendered top-right with fade-in/hold/fade-out timing; a history ring
  buffer makes them assertable in headless autotests.
- **Atmosphere.** :mod:`bd_horror.atmosphere` is the dread machine:
  :class:`~bd_horror.atmosphere.Dread` (a 0-100 tension meter driven by
  darkness, nearby monsters, and player damage, with threshold callbacks,
  heartbeat, vignette, and whisper stings), the
  :class:`~bd_horror.atmosphere.LightManager` sector-light programs
  (``candle``/``fluorescent``/``blackout``),
  :class:`~bd_horror.atmosphere.StalkerDirector` (spawns monsters behind
  the player at high dread), and
  :class:`~bd_horror.atmosphere.HorrorState`, which persists dread plus
  program descriptors through ``bd.state`` exactly like
  ``bd_vtm.VtMState``.

Minimal usage::

    import biaseddoom as bd
    import bd_horror
    from bd_horror import theme, toasts

    state = bd_horror.HorrorState()

    @bd.on("map_load")
    def begin(event):
        state.start()             # dread tick + stalker windows
        state.arm_persistence()   # save/load round-trip via bd.state
        if not event.get("from_savegame") and not event.get("from_hub"):
            state.lights.candle([5], amplitude=24)
            toasts.toast("the air tastes of copper", kind="omen")

    @bd.on("imgui_frame")
    def draw(event):
        theme.apply()
        try:
            if theme.begin_window("The Dread", pos=(80, 60), size=(320, 0)):
                theme.bar("Dread", state.dread.level / 100.0,
                          overlay=f"{state.dread.level:.0f}%",
                          tone="blood", pulse=state.dread.level >= 75)
            bd.imgui.end()
        finally:
            theme.clear()
        toasts.draw_toasts()

Performance and determinism
---------------------------

Every world scan runs on a ``bd.schedule`` task of at least 35 tics and
every actor filter is pushed into ``bd.actor_refs`` keyword arguments, per
the budget rules in ``docs/development/python-performance.md``. All
randomness flows through the engine's deterministic script RNG
(``bd.random``/``bd.randint``/``bd.choice``), so light flicker and stalker
rolls resume exactly after a checkpoint load.
"""

from __future__ import annotations

from typing import Any

__all__ = [
    "Dread", "LightProgram", "LightManager", "StalkerDirector",
    "HorrorState", "theme", "toasts", "atmosphere",
    "STATE_KEY", "__version__",
]

__version__ = "1.0.0"

#: Key under which HorrorState persists itself in ``bd.state``.
STATE_KEY = "bd_horror"

_ATmosphere_EXPORTS = ("Dread", "LightProgram", "LightManager",
                       "StalkerDirector", "HorrorState")


def __getattr__(name: str) -> Any:
    # Lazy re-exports so `bd_horror.HorrorState` (and friends) work without
    # paying for a submodule until it is actually used (mirrors bd_quests).
    if name in _ATmosphere_EXPORTS:
        from . import atmosphere
        return getattr(atmosphere, name)
    if name in ("theme", "toasts", "atmosphere"):
        import importlib
        return importlib.import_module(f".{name}", __name__)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
