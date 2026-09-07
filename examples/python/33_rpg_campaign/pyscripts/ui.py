"""Ashvale Crossing - UI: creation wizard, dialogue, shop, sheet, journal.

Every window is immediate-mode ImGui drawn from the single ``imgui_frame``
handler at the bottom; headless safety is structural (the event never fires
under ``-headless`` or ``-scripttest``). Each window mirrors the framework
guard discipline: one ``bd.warn`` per frame at worst and always a balanced
``begin``/``end`` (same contract as ``bd_dnd.sheet.CharacterSheet``).

- **The founding window** drives the shared ``systems.wizard``: name input,
  three class radios, a per-class briefing panel (concept, hit die, saves,
  trained skills, starting gear, and the level-by-level features, all derived
  from the live class), one-click ``CLASS_PRESETS`` buttons that fill scores
  and skills through ``content.apply_preset``, a score-method radio
  (standard / point buy / rolled), per-ability + and - buttons annotated with
  the derived modifier and what each ability governs, a class-skill checklist
  with per-skill blurbs, a Finish button (``systems.finish_creation``), and a
  validation error line fed by the wizard's ``ValueError``.
- **The talk prompt** renders ``manager.prompt(pawn)`` each frame while no
  conversation is active, with the framework's hardcoded ``[E]`` prefix
  swapped for the live Custom Action 1 binding.
- **The dialogue window** renders the active session like the sibling
  fixture: speaker colored by standing, wrapped prose, the skill-check flash,
  and numbered selectable choices with their annotations.
- **The shop window** embeds the shipped ``bd_npcs.ShopUI`` bound to Dobb's
  shop; the "Show me your wares." choice sets ``systems.shop_open`` and the
  window mirrors that flag.
- **Sheet + journal**: ``bd_dnd.sheet.CharacterSheet`` (created by
  systems at the founding so it exists headless too; toggle on **K** or
  Custom Action 2) and ``bd_quests.journal_ui.JournalUI`` (toggle on
  **J**).

Manifest entry 3 of 4.
"""

import biaseddoom as bd
import bd_dialogue
import bd_dnd
import bd_quests
from bd_npcs.services import ShopUI
from bd_quests.journal_ui import JournalUI, bind_journal_toggle

try:
    import ashvale_content as content
    import ashvale_systems as systems
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="ashvale_content")
    systems = bd.import_script("pyscripts/systems.py",
                               module_name="ashvale_systems")

_autowarp_done = False
_name_buf = ""
_creation_error = ""
_preset_note = ""
_journal = None
_sheet_bound = False
_shop_ui = None

#: Speaker/standing colors (r, g, b, a).
_STANDING_COLORS = {
    "hostile": (0.95, 0.35, 0.30, 1.0),
    "cold": (0.80, 0.55, 0.45, 1.0),
    "neutral": (0.90, 0.75, 0.45, 1.0),
    "warm": (0.45, 0.90, 0.55, 1.0),
    "trusted": (0.40, 0.85, 0.95, 1.0),
}
_ANNOTATION_COLOR = (0.85, 0.70, 0.30, 1.0)
_LOCK_COLOR = (0.95, 0.35, 0.30, 1.0)
_FLASH_OK = (0.35, 0.90, 0.40, 1.0)
_FLASH_FAIL = (0.95, 0.45, 0.35, 1.0)


@bd.on("engine_start")
def setup_ui(event):
    """The journal exists before any hero does; bind its J toggle now."""
    global _journal
    if _journal is None:
        _journal = JournalUI(title=content.JOURNAL_TITLE)
        bind_journal_toggle(key="j", journal=_journal)


# --- creation wizard --------------------------------------------------------------------


def _draw_class_briefing(imgui, cls):
    """The "how this class plays" panel: concept, facts, and features.

    Every fact is derived from the live CharacterClass (through
    ``content.class_briefing``), so the briefing always matches what the
    wizard will enforce at Finish.
    """
    concept = content.CLASS_CONCEPTS.get(cls.name, {})
    if concept.get("concept"):
        imgui.text_wrapped(concept["concept"])
    if concept.get("tip"):
        imgui.push_style_color(imgui.Col.Text, *_ANNOTATION_COLOR)
        try:
            imgui.text_wrapped("Tip: " + concept["tip"])
        finally:
            imgui.pop_style_color()
    briefing = content.class_briefing(cls)
    imgui.text(f"Hit die {briefing['hit_die']}   Primary "
               f"{briefing['primary']}   Saves {briefing['saves']}")
    imgui.text_wrapped(f"Trained skills: {briefing['skills']}")
    imgui.text_wrapped(f"Starting gear: {briefing['equipment']}")
    if briefing["features"]:
        imgui.text("Class features:")
        for level, name, description in briefing["features"]:
            imgui.bullet_text(f"Lv{level} {name}")
            if description:
                imgui.push_style_color(imgui.Col.Text, *_ANNOTATION_COLOR)
                try:
                    imgui.text_wrapped(f"    {description}")
                finally:
                    imgui.pop_style_color()


def _draw_presets(imgui, wizard):
    """One-click builds for the chosen class (standard array + skills)."""
    global _preset_note, _creation_error
    presets = content.CLASS_PRESETS.get(wizard.class_.name, ())
    if not presets:
        return
    imgui.text("Presets (fill scores and skills in one click):")
    for preset in presets:
        if imgui.small_button(f"{preset['name']}###ashvale_preset_"
                              f"{wizard.class_.name}_{preset['id']}"):
            try:
                content.apply_preset(wizard, preset)
                _preset_note = f"{preset['name']}: {preset['concept']}"
                _creation_error = ""
            except ValueError as exc:
                _creation_error = str(exc)
    if _preset_note:
        imgui.push_style_color(imgui.Col.Text, *_ANNOTATION_COLOR)
        try:
            imgui.text_wrapped(_preset_note)
        finally:
            imgui.pop_style_color()


def _draw_ability_scores(imgui, wizard):
    """The score method radios and the per-ability steppers, annotated.

    Each row shows the derived modifier next to the score and a one-line
    blurb naming what the ability governs, so the numbers read as rules,
    not trivia.
    """
    global _creation_error
    imgui.push_style_color(imgui.Col.Text, *_ANNOTATION_COLOR)
    try:
        imgui.text_wrapped(content.MODIFIER_HINT)
    finally:
        imgui.pop_style_color()
    for method, label in (("standard_array", "Standard array"),
                          ("point_buy", "Point buy"),
                          ("rolled", "Rolled")):
        if imgui.radio_button(f"{label}###ashvale_method_{method}",
                              wizard.method == method):
            if method == "standard_array":
                wizard.use_standard_array()
            elif method == "point_buy":
                wizard.use_point_buy()
            else:
                wizard.use_rolled()
            _creation_error = ""

    scores = wizard.scores
    for ability in bd_dnd.ABILITIES:
        value = scores.get(ability)
        if value is None:
            label = f"{ability.upper():>3}:  -"
        else:
            label = f"{ability.upper():>3}: {value:2d}  " \
                    f"({bd_dnd.modifier(value):+d})"
        imgui.text(label)
        imgui.same_line(150.0)
        if imgui.small_button(f"-###ashvale_minus_{ability}"):
            wizard.set_score(ability, (value or 10) - 1)
        imgui.same_line()
        if imgui.small_button(f"+###ashvale_plus_{ability}"):
            wizard.set_score(ability, (value or 10) + 1)
        imgui.same_line()
        imgui.text_disabled(content.ABILITY_BLURBS.get(ability, ""))
    if wizard.method == "point_buy":
        imgui.text(f"Points remaining: {wizard.points_remaining}")
    elif wizard.method == "standard_array":
        imgui.text_disabled("Standard array: 15 14 13 12 10 8, used once")


def _draw_skill_picks(imgui, wizard):
    """The class-skill checklist, each pick annotated with its blurb."""
    global _creation_error
    quota = min(3, len(wizard.class_.class_skills))
    imgui.text(f"Class skills ({len(wizard.skills)}/{quota})")
    for skill in wizard.class_.class_skills:
        pretty = skill.replace("_", " ").title()
        ability = bd_dnd.SKILLS.get(skill, "?").upper()
        selected = skill in wizard.skills
        if imgui.selectable(f"{pretty} ({ability})###ashvale_skill_{skill}",
                            selected):
            try:
                if selected:
                    wizard.unassign_skill(skill)
                else:
                    wizard.assign_skill(skill)
                _creation_error = ""
            except ValueError as exc:
                _creation_error = str(exc)
        blurb = content.SKILL_BLURBS.get(skill)
        if blurb:
            imgui.same_line()
            imgui.text_disabled(blurb)


def _draw_creation(imgui):
    wizard = systems.wizard
    if wizard is None or systems.hero is not None:
        return
    # The founding window keeps to the right half; the journal sits at
    # (40, 60) and Dobb's ShopUI at (420, 60). Width and position fit the
    # 1280-wide documentation capture (720 + 540 = 1260).
    imgui.set_next_window_pos(720.0, 40.0, imgui.Cond.FirstUseEver)
    imgui.set_next_window_size(540.0, 0.0, imgui.Cond.FirstUseEver)
    expanded = imgui.begin(content.WIZARD_TITLE)
    # Interactive only: freeze the world while the wizard is up on a live
    # map (headless autotest never renders, and main.py disables this for
    # the scheduled screenshot pose, so neither driver stalls).
    if expanded and systems.player_pawn() is not None:
        systems.pause_for_creation()
    try:
        if not expanded:
            return
        global _name_buf, _creation_error
        changed, _name_buf = imgui.input_text(content.WIZARD_NAME_HINT,
                                              _name_buf, 40)
        if changed:
            try:
                wizard.set_name(_name_buf)
                _creation_error = ""
            except ValueError as exc:
                _creation_error = str(exc)

        imgui.separator()
        imgui.text("Class:")
        imgui.spacing()
        for cls in content.CLASS_LIST:
            if imgui.radio_button(f"{cls.name}###ashvale_class_{cls.name}",
                                  wizard.class_ is cls):
                try:
                    wizard.choose_class(cls)
                    for skill in list(wizard.skills):
                        wizard.unassign_skill(skill)
                    _creation_error = ""
                except ValueError as exc:
                    _creation_error = str(exc)

        if wizard.class_ is None:
            imgui.text_disabled("Pick a class to see how it plays.")
            return

        imgui.separator()
        _draw_class_briefing(imgui, wizard.class_)

        imgui.separator()
        _draw_presets(imgui, wizard)

        imgui.separator()
        imgui.text("Ability scores")
        _draw_ability_scores(imgui, wizard)

        imgui.separator()
        _draw_skill_picks(imgui, wizard)

        imgui.separator()
        if imgui.button(content.WIZARD_FINISH_LABEL):
            try:
                systems.finish_creation(wizard)
                _creation_error = ""
            except ValueError as exc:
                _creation_error = str(exc)
        if _creation_error:
            imgui.text_colored(*_LOCK_COLOR, _creation_error)
    finally:
        imgui.end()


# --- talk prompt --------------------------------------------------------------------------


def _draw_prompt(imgui):
    if bd_dialogue.active_session() is not None:
        return
    pawn = systems.player_pawn()
    if pawn is None or systems.manager is None:
        return
    try:
        prompt = systems.manager.prompt(pawn)
    except Exception:
        return
    if not prompt:
        return
    # The framework prompt hardcodes its own "[E] " prefix; swap in the
    # live Custom Action 1 binding so the hint names the actual key.
    if prompt.startswith("[E] "):
        prompt = prompt[4:]
    prompt = f"[{systems.action_key_hint(1)}] {prompt}"
    imgui.set_next_window_pos(660.0, 620.0, imgui.Cond.FirstUseEver)
    expanded = imgui.begin(content.PROMPT_WINDOW_ID,
                           flags=imgui.WindowFlags.NoTitleBar)
    try:
        if expanded:
            imgui.text(prompt)
    finally:
        imgui.end()


# --- dialogue window ------------------------------------------------------------------------


def _draw_portrait(imgui, session):
    if not imgui.begin_child("ashvale_portrait", (96.0, 112.0), border=True):
        imgui.end_child()
        return
    try:
        try:
            imgui.image(session.npc_ref, w=0.0, h=104.0)
        except Exception:
            imgui.text_disabled("(no portrait)")
    finally:
        imgui.end_child()


def _draw_flash(imgui, session):
    flash = session.last_check
    if flash is None:
        return
    try:
        if bd.level_time() > int(flash.get("until_tic", 0)):
            return
    except Exception:
        return
    color = _FLASH_OK if flash.get("success") else _FLASH_FAIL
    imgui.text_colored(*color, str(flash.get("text", "")))


def _draw_dialogue(imgui, session):
    node = session.active_node
    if node is None:
        return
    if node.portrait:
        _draw_portrait(imgui, session)
    ctx = session.context()
    standing = str(ctx.get("standing") or "neutral")
    imgui.text_colored(*_STANDING_COLORS.get(standing,
                                             _STANDING_COLORS["neutral"]),
                       node.speaker)
    imgui.spacing()
    imgui.text_wrapped(node.text)
    _draw_flash(imgui, session)
    imgui.separator()
    entries = session.choices()
    if not entries:
        imgui.text_disabled("(say nothing)")
        return
    for index, (choice, enabled, annotation) in enumerate(entries):
        label = f"{index + 1}. {choice.text}"
        if not enabled:
            suffix = f"  {annotation}" if annotation else ""
            imgui.text_disabled(f"{label}{suffix}")
            continue
        if imgui.selectable(f"{label}###ashvale_choice_{index}", False):
            session.choose(index)
            break  # the choice list (or the session) may have changed
        if annotation:
            imgui.same_line()
            color = _LOCK_COLOR if annotation.startswith("[") \
                else _ANNOTATION_COLOR
            imgui.text_colored(*color, annotation)


def _draw_dialogue_window(imgui):
    session = systems.current_session()
    if session is None:
        return
    if not session.active:
        return
    imgui.set_next_window_pos(60.0, 360.0, imgui.Cond.FirstUseEver)
    imgui.set_next_window_size(560.0, 0.0, imgui.Cond.FirstUseEver)
    expanded = imgui.begin(f"{content.DIALOGUE_TITLE}###ashvale_dialogue",
                           flags=bd.imgui.WindowFlags.NoCollapse)
    try:
        if expanded:
            _draw_dialogue(imgui, session)
    finally:
        imgui.end()


# --- shop window ------------------------------------------------------------------------------


def _draw_shop(imgui):
    global _shop_ui
    if systems.dobb_shop is None:
        return
    if _shop_ui is None:
        _shop_ui = ShopUI(systems.dobb_shop, title=content.SHOP_TITLE)
    _shop_ui.visible = bool(systems.shop_open)
    if not _shop_ui.visible:
        return
    _shop_ui.draw()
    if not _shop_ui.visible:  # the window's close button hid it
        systems.shop_open = False
        _shop_ui.visible = True


# --- sheet + journal ----------------------------------------------------------------------------


def _draw_sheet_and_journal(imgui):
    global _sheet_bound
    if _journal is not None:
        _journal.draw()
    sheet = systems.ensure_hero_sheet()
    if sheet is not None:
        if not _sheet_bound:
            _sheet_bound = True
            bd_dnd.bind_sheet_toggle(sheet, key="k")
        sheet.draw()


# --- frame -----------------------------------------------------------------------------------------


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

    imgui = bd.imgui
    for draw_fn in (_draw_creation, _draw_prompt, _draw_dialogue_window,
                    _draw_shop, _draw_sheet_and_journal):
        try:
            draw_fn(imgui)
        except Exception as exc:
            # One warn per frame at worst; the window stack stays balanced.
            bd.warn(f"ashvale ui draw error in {draw_fn.__name__}: {exc!r}")


# --- sibling-import registration ----------------------------------------------------

import sys as _sys
import types as _types


class _LiveAlias(_types.ModuleType):
    """sys.modules alias that reads/writes through to the module's live
    globals (the engine registers manifest modules under a mangled name only
    after execution, so the real module object cannot be aliased from inside
    itself)."""

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


_sys.modules.setdefault("ashvale_ui", _LiveAlias("ashvale_ui", globals()))
del _sys, _types
