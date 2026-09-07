"""The Confessor — UI: the bd_horror-themed Rite panel and toasts.

A small Dear ImGui panel rendered next to the native conversation menu:
the quest state out of ``bd_quests``, whether the Confessor's handle is
live, and the last line of the rite. Everything is skinned through
``bd_horror.theme`` and toasts render through ``bd_horror.toasts``.

Headless safety is structural: every ImGui call lives inside the
``imgui_frame`` handler, which never fires under ``-headless`` — the UI is
a pure no-op there. The handler also carries the autowarp fallback for
runs launched without ``+map`` (same pattern as the other examples).

Manifest entry 3 of 4.
"""

import biaseddoom as bd
from bd_horror import theme, toasts

try:
    import confessor_content as content
    import confessor_systems as systems
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="confessor_content")
    systems = bd.import_script("pyscripts/systems.py",
                               module_name="confessor_systems")

_autowarp_done = False


def _draw_rite_panel():
    if not theme.begin_window("The Rite###confessor_rite",
                              pos=(80, 330), size=(340, 0)):
        return
    theme.section("Penance")
    theme.kv_row("Quest", systems.quest_state_text())
    npc = systems.npc_ref
    try:
        present = npc is not None and npc.valid
    except Exception:
        present = False
    theme.kv_row("Confessor", "holding court" if present else "absent")
    theme.omen_text(systems.last_rite_line)


@bd.on("imgui_frame")
def draw(event):
    global _autowarp_done
    # Headless runs (-scripttest) may launch without +map: queue a warp on
    # the first rendered frame when nothing loaded a level yet. With
    # -headless no frames render and this handler simply never runs.
    if not _autowarp_done:
        _autowarp_done = True
        try:
            if not bd.current_map():
                bd.execute("map map01")
        except RuntimeError:
            pass
    theme.apply()
    try:
        _draw_rite_panel()
        bd.imgui.end()
    finally:
        theme.clear()
    toasts.draw_toasts()


# --- sibling-import registration ---------------------------------------------------

import sys as _sys
import types as _types


class _LiveAlias(_types.ModuleType):
    """sys.modules alias that reads/writes through to the module's live
    globals (the engine registers manifest modules under a mangled name
    only after execution, so the real module object cannot be aliased
    from inside itself)."""

    def __init__(self, name, namespace):
        super().__init__(name)
        object.__setattr__(self, "_bd_live_ns", namespace)

    def __getattr__(self, key):
        try:
            return object.__getattribute__(self, "_bd_live_ns")[key]
        except KeyError:
            raise AttributeError(key)

    def __setattr__(self, key, value):
        object.__getattribute__(self, "_bd_live_ns")[key] = value


_sys.modules.setdefault("confessor_ui", _LiveAlias("confessor_ui", globals()))
del _sys, _types
