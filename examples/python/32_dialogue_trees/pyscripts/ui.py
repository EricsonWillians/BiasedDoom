"""The Interrogation - UI: the bd_horror-skinned dialogue window.

A full horror reskin of the conversation overlay, **composed in the
example** rather than subclassing ``bd_dialogue.ui.DialogueUI`` (the
shipped widget's look is hard-coded; the session API - ``choices()``,
``choose()``, ``last_check``, ``active_node`` - carries everything this
window needs):

- the whole window under ``bd_horror.theme.apply()`` / ``clear()``;
- the Inquisitor's live sprite in a ``theme.frame_image`` portrait plate;
- his name in faction color (sickly when the Choir trusts you, fresh
  blood when it hates you, ember while undecided);
- body text wrapped in bone;
- skill-check results as *judgment lines* - "The Inquisitor is swayed."
  in sickly green, "He sees the lie." in bright wound red - for the
  flash's ~3 s lifetime;
- choices as numbered selectable rows (mouse click or ImGui keyboard
  navigation, exactly like the framework UI - the ``1.``/``2.`` prefixes
  are visual hints only) with their gate/DC/bonus annotations in wound
  red ("(DC 12 Persuasion, you +4)", composed by the session wrapper),
  locked rows dimmed to marrow;
- a Dread bar in the footer, pulsing once the hunt begins.

Node body text is drawn as the session wrapper composes it, including
the hidden-choice hint lines it appends near the dread/attitude
thresholds.

Headless safety is structural: every ImGui call lives inside the
``imgui_frame`` handler, which never fires under ``-headless``. The
handler also carries the no-``+map`` autowarp fallback and renders the
``bd_horror`` toast queue.

Manifest entry 3 of 4.
"""

import biaseddoom as bd
from bd_horror import theme, toasts
from bd_horror.theme import PALETTE

try:
    import inquisition_content as content
    import inquisition_systems as systems
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="inquisition_content")
    systems = bd.import_script("pyscripts/systems.py",
                               module_name="inquisition_systems")

_autowarp_done = False

#: Judgment lines for the skill-check result flash.
_JUDGMENT_OK = ("The Inquisitor is swayed.", PALETTE["sickly"])
# (the pack's "blood" is too dark for text on the void background; the
#  designated text-safe fresh blood is "wound")
_JUDGMENT_FAIL = ("He sees the lie.", PALETTE["wound"])


def _level_time():
    try:
        return bd.level_time()
    except Exception:
        return 0


def _speaker_color(session):
    """Faction-reputation speaker color: sickly / wound / ember."""
    try:
        if session.factions is not None and session.faction is not None:
            rep = int(session.factions.reputation(session.faction))
            if rep >= 1:
                return PALETTE["sickly"]
            if rep <= -1:
                return PALETTE["wound"]
    except Exception:
        pass
    return PALETTE["ember"]


def _draw_portrait(session):
    try:
        theme.frame_image(session.npc_ref, size=96.0)
    except Exception:
        # Stale handle mid-frame or a texture lookup failure: the portrait
        # is decorative, so degrade to text.
        theme.faded_text("(no face remains)")


def _draw_judgment(session):
    flash = session.last_check
    if flash is None or _level_time() > int(flash.get("until_tic", 0)):
        return
    line, color = _JUDGMENT_OK if flash.get("success") else _JUDGMENT_FAIL
    imgui = bd.imgui
    imgui.push_style_color(imgui.Col.Text, *color)
    try:
        imgui.text_wrapped(f"{line}  {flash.get('text', '')}")
    finally:
        imgui.pop_style_color()


def _draw_choices(session):
    imgui = bd.imgui
    entries = session.choices()
    if not entries:
        theme.faded_text("(say nothing)")
        return
    for index, (choice, enabled, annotation) in enumerate(entries):
        label = f"{index + 1}. {choice.text}"
        if not enabled:
            suffix = f"  {annotation}" if annotation else ""
            imgui.text_disabled(f"{label}{suffix}")
            continue
        # "###" keeps the ImGui identity stable across renavigation.
        if imgui.selectable(f"{label}###inq_choice_{index}", False):
            session.choose(index)
            break  # the choice list (or the session) may have changed
        if annotation:
            imgui.same_line()
            imgui.text_colored(*PALETTE["wound"], annotation)


def _draw_window(session):
    node = session.active_node
    if node is None:
        return
    if node.portrait:
        _draw_portrait(session)
    imgui = bd.imgui
    imgui.text_colored(*_speaker_color(session), node.speaker)
    imgui.spacing()
    imgui.text_wrapped(node.text)  # bone under the theme
    _draw_judgment(session)
    imgui.separator()
    _draw_choices(session)
    imgui.spacing()
    try:
        level = float(systems.horror.dread.level)
    except Exception:
        level = 0.0
    theme.bar("Dread", level / 100.0, overlay=f"{level:.0f}",
              tone="blood", pulse=level >= 75)
    imgui.spacing()


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

    session = systems.current_session()
    if session is not None and not session.active:
        session = None  # conversation over: the window vanishes
    if session is not None:
        theme.apply()
        try:
            if theme.begin_window("The Interrogation###inquisition",
                                  pos=(60, 380), size=(560, 0),
                                  flags=bd.imgui.WindowFlags.NoCollapse):
                try:
                    _draw_window(session)
                except Exception as exc:
                    bd.warn(f"inquisition ui draw error: {exc!r}")
            bd.imgui.end()
        finally:
            theme.clear()
    toasts.draw_toasts()


# --- sibling-import registration ----------------------------------------------------

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


_sys.modules.setdefault("inquisition_ui",
                        _LiveAlias("inquisition_ui", globals()))
del _sys, _types
