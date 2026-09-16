"""The Delve, bd_horror-themed ImGui interface.

Two windows, both no-ops headless (``imgui_frame`` never fires under
``-headless``, and :func:`draw_frame` also early-outs on
``bd.headless()``):

- **The founding window**: shown once, on the first map of an interactive
  session, while the world sits engine-paused. A name field (prefilled
  "Delver") and three class cards (Fighter / Rogue / Cleric), each with
  its concept line, preset standard array, hit die, and class active with
  its effect and "[C]" key. One click drives the same
  ``systems.found_hero`` the autotest calls: a real CreationWizard run,
  no shortcuts.
- **The Delver window**: the hero's full character window in the
  bd_horror skin, every value read live: Body (Blood bar, sheet pool,
  plus a body bar, pawn health: one pool, one truth), Experience (XP bar
  and "N to level L+1"), Abilities (six scores with modifiers) and Skills
  (each class skill's total bonus, modifier + proficiency + mastery, with
  its live Doom context: bash/pick DCs and needed rolls, CQB and
  dead-eye ranges, the sanctuary threshold, the examine), Vocation
  (class, hit die, saves, features), Active (charges and the refill
  hint), Contract (both objectives with progress and the map's
  modifier), Examine (the crosshair target's data), and Boons (pending
  and taken this run). Custom Action 1 (auto-bound to Q, alias
  ``toggle_sheet``) flips it.

- **The boon chooser**: a world-paused pick-one-of-three that opens
  whenever a level-up queues a boon (mouse buttons or keys 1/2/3, both
  driving ``systems.pick_boon``).
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

try:
    import crypt_systems as systems
except ImportError:
    systems = bd.import_script("pyscripts/systems.py",
                               module_name="crypt_systems")


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


class ReliquarySheet:
    """The hero's character sheet as a bd_horror reliquary window.

    Interface-compatible with ``bd_dnd.sheet.CharacterSheet`` (``visible``
    /``toggle()``/``draw()``) so ``bind_sheet_toggle`` and the autotest's
    toggle chain work unchanged. The window starts visible; the close
    button and :meth:`toggle` stay in sync because ``begin``'s ``open``
    result is fed back into :attr:`visible` every frame.
    """

    def __init__(self, character: "bd_dnd.Character", state: Any = None,
                 title: str = "Reliquary") -> None:
        self.character: "bd_dnd.Character" = character
        self.state: Any = state
        self.title: str = str(title)
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
                f"{self.title}###delve_sheet",
                pos=(40.0, 60.0), size=(380.0, 0.0), open=self.visible)
            try:
                if expanded:
                    self._draw_body()
            finally:
                imgui.end()
        except Exception as exc:
            bd.warn(f"delve reliquary draw error: {exc!r}")
        finally:
            theme.clear()

    def _draw_body(self) -> None:
        character = self.character
        imgui = bd.imgui
        pawn = systems.player_pawn()

        theme.section("Body")
        hp_frac = (character.hp / character.max_hp) if character.max_hp else 0.0
        theme.bar("Blood", hp_frac,
                  overlay=f"Blood {character.hp}/{character.max_hp}",
                  tone="blood", pulse=hp_frac <= 0.25)
        if pawn is not None:
            body_frac = max(0.0, min(1.0, pawn.health / 100.0))
            theme.bar("Body", body_frac, overlay=f"Body {pawn.health}",
                      tone="bruise", pulse=pawn.health <= 25)
            theme.faded_text("One pool, one truth: the sheet and the body "
                             "heal and bleed at the same ratio.")

        theme.section("Experience")
        theme.bar("Experience", _xp_fraction(character),
                  overlay=_xp_overlay(character), tone="ember")
        next_xp = character.xp_for_next_level()
        if next_xp is not None:
            theme.faded_text(f"{next_xp - character.xp} to level "
                             f"{character.level + 1}.")
        else:
            theme.faded_text("MOST HIGH.")

        theme.section("Abilities")
        for name in bd_dnd.ABILITIES:
            score = character.abilities.score(name)
            mod = character.abilities.mod(name)
            theme.kv_row(name.upper(), f"{score}  ({mod:+d})")

        cls = getattr(character, "cls", None)
        if cls is not None:
            theme.section("Vocation")
            theme.kv_row("Class", f"{cls.name} (d{cls.hit_die})")
            if cls.proficient_saves:
                theme.kv_row("Saves",
                             ", ".join(a.upper() for a in cls.proficient_saves))
            for level in sorted(cls.features):
                for feature in cls.features[level]:
                    theme.kv_row(f"Lv{level}", str(feature["name"]))

        skills = sorted(getattr(character, "proficient_skills", ()) or ())
        if skills:
            theme.section("Skills")
            for skill in skills:
                bonus = systems.skill_bonus_total(character, skill)
                label = skill.replace("_", " ").title()
                theme.kv_row(label, f"{bonus:+d}")
                context = _skill_context(character, skill, bonus)
                if context:
                    theme.faded_text(context)

        spec = content.CLASS_ACTIVES.get(getattr(character, "class_id", ""))
        if spec is not None:
            theme.section("Active")
            charges = character.resources.get(spec["resource"], 0)
            theme.kv_row(f"[C] {spec['name']}",
                         f"{charges}/{character.resource_max.get(spec['resource'], 0)}")
            theme.faded_text(f"{spec['effect']}  Rest (V) refills.")

        theme.section("Contract")
        quest = systems._contract_quest()
        if quest is None:
            theme.faded_text("No quarry here.")
        else:
            for objective in quest.objectives:
                mark = "x" if objective.done else " "
                theme.kv_row(f"[{mark}]",
                             f"{objective.text} "
                             f"({objective.progress}/{objective.count})")
            modifier = systems.contract_state.get("modifier")
            if isinstance(modifier, dict) and modifier.get("id") not in (
                    None, "plain"):
                theme.kv_row("Modifier", modifier["name"])
            theme.kv_row("Depth", str(systems.delve_depth()))

        theme.section("Examine")
        if systems.examine_state.get("line"):
            imgui.text_wrapped(systems.examine_state["line"])
        else:
            theme.faded_text("Nothing in the crosshair.")

        theme.section("Boons")
        taken = systems.boon_state.get("taken") or []
        pending = systems.boon_pending()
        if pending:
            theme.kv_row("Pending", f"{pending} (choosing...)")
        if taken:
            names = {entry["id"]: entry["name"] for entry in content.BOONS}
            for boon_id in taken:
                theme.kv_row("Taken", names.get(boon_id, boon_id))
        if not taken and not pending:
            theme.faded_text("None yet; levels bring them.")


def _skill_context(character: Any, skill: str, bonus: int) -> str:
    """The live Doom-context line under one class skill (Delver window)."""
    if skill == "athletics":
        dc = (systems.door_bash.dc if systems.door_bash is not None
              else content.DOOR_DC)
        need = max(1, dc - bonus)
        return (f"bash DC {dc}: need {need}+ on d20; CQB "
                f"+{content.CQB_BONUS} within {int(content.CQB_RANGE)}u")
    if skill == "perception":
        return (f"trap sense; dead-eye +{content.DEADEYE_BONUS} past "
                f"{int(content.DEADEYE_RANGE)}u")
    if skill == "sleight_of_hand":
        dc = content.DOOR_DC - content.ROGUE_DOOR_DC_DELTA
        need = max(1, dc - bonus)
        return f"pick DC {dc}: need {need}+ on d20"
    if skill == "religion":
        return (f"sanctuary at {systems.sanctuary_threshold(character)}; "
                f"wards the nightmare (WIS)")
    if skill == "insight":
        return "examine the crosshair"
    return ""


# --- the founding window ---------------------------------------------------------

#: Window state: open flag, the editable name, and the last founding
#: error (wizard validation failure, shown in red).
founding = {"open": False, "name": content.HERO_DEFAULT_NAME, "error": ""}


def open_founding() -> None:
    """Open the founding window (first interactive map of a session)."""
    founding["open"] = True


def close_founding() -> None:
    founding["open"] = False


def _draw_class_card(imgui: Any, cls: Any) -> None:
    """One class card: concept, preset array, hit die, active, the button."""
    preset = content.CLASS_PRESETS[cls.name]
    spec = content.CLASS_ACTIVES[cls.name]
    theme.section(cls.name)
    imgui.text_wrapped(preset["concept"])
    imgui.text_disabled(preset["tip"])
    scores = preset["scores"]
    imgui.text("  ".join(f"{a.upper()} {scores[a]}"
                         for a in ("str", "dex", "con")))
    imgui.text("  ".join(f"{a.upper()} {scores[a]}"
                         for a in ("int", "wis", "cha")))
    imgui.same_line()
    imgui.text_disabled("(standard array)")
    imgui.text(f"Hit die d{cls.hit_die}")
    imgui.text_colored(*PALETTE["ember"],
                       f"[C] {spec['name']}: {spec['effect']}")
    if imgui.button(f"Found as {cls.name}###delve_found_{cls.name}",
                    w=-1.0):
        try:
            systems.found_hero(cls.name, founding["name"].strip()
                               or content.HERO_DEFAULT_NAME)
            founding["error"] = ""
            close_founding()
        except ValueError as exc:
            founding["error"] = str(exc)
        except Exception as exc:
            founding["error"] = f"founding failed: {exc!r}"


def draw_founding() -> None:
    """Submit the founding window. ``imgui_frame`` handlers only."""
    if not founding["open"]:
        return
    imgui = bd.imgui
    theme.apply()
    try:
        imgui.set_next_window_pos(420.0, 40.0, imgui.Cond.FirstUseEver)
        imgui.set_next_window_size(430.0, 0.0, imgui.Cond.FirstUseEver)
        expanded = imgui.begin("The Delve: found your delver"
                               "###delve_founding",
                               flags=imgui.WindowFlags.NoCollapse)
        try:
            if expanded:
                imgui.text_wrapped(
                    "The guild hands out one life and one contract per "
                    "map. Choose who spends it.")
                changed, founding["name"] = imgui.input_text(
                    "Name###delve_name", founding["name"], max_length=24)
                imgui.separator()
                for cls in content.CLASS_LIST:
                    _draw_class_card(imgui, cls)
                if founding["error"]:
                    imgui.text_colored(*PALETTE["blood"],
                                       founding["error"])
        finally:
            imgui.end()
    except Exception as exc:
        bd.warn(f"delve founding draw error: {exc!r}")
    finally:
        theme.clear()


# --- the boon chooser (world-paused, pick one of three) --------------------------

_chooser_was_open = False


def draw_boon_chooser() -> None:
    """The level-up boon chooser: three cards, mouse or keys 1/2/3.

    Opens world-paused whenever ``systems.boon_pending()`` reports a
    queued pick (the pause reuses the founding window's machinery);
    every affordance drives ``systems.pick_boon``.
    """
    global _chooser_was_open
    if systems.hero is None or systems.boon_pending() <= 0:
        if _chooser_was_open:
            _chooser_was_open = False
            systems.unpause_for_creation()
        return
    if not _chooser_was_open:
        _chooser_was_open = True
        systems.pause_for_creation()
    imgui = bd.imgui
    theme.apply()
    try:
        imgui.set_next_window_pos(450.0, 130.0, imgui.Cond.FirstUseEver)
        imgui.set_next_window_size(400.0, 0.0, imgui.Cond.FirstUseEver)
        expanded = imgui.begin("The guild offers a boon###delve_boons",
                               flags=imgui.WindowFlags.NoCollapse)
        try:
            if expanded:
                imgui.text_wrapped("Choose one boon (keys 1, 2, 3):")
                for index, entry in enumerate(content.BOONS):
                    if imgui.button(
                            f"{index + 1}. {entry['name']}"
                            f"###delve_boon_{entry['id']}", w=-1.0):
                        systems.pick_boon(entry["id"])
                    imgui.text_disabled(f"    {entry['effect']}")
                for index, entry in enumerate(content.BOONS):
                    try:
                        if imgui.is_key_pressed(
                                getattr(imgui.Key, str(index + 1))):
                            systems.pick_boon(entry["id"])
                    except Exception:
                        pass
        finally:
            imgui.end()
    except Exception as exc:
        bd.warn(f"delve boon chooser draw error: {exc!r}")
    finally:
        theme.clear()


# --- frame glue --------------------------------------------------------------------

#: UI context: set once by main.py's setup and read every frame.
ctx: dict = {}


def draw_frame() -> None:
    """Draw the whole overlay for one ``imgui_frame`` event.

    No-op when headless. The founding window owns the frame while it is
    open; the reliquary draws whenever visible; toasts ride on top.
    """
    try:
        if bd.headless():
            return
    except Exception:
        pass
    if founding["open"]:
        draw_founding()
        toasts.draw_toasts()
        return
    if systems.boon_pending() > 0 or _chooser_was_open:
        draw_boon_chooser()
        toasts.draw_toasts()
        return
    sheet = ctx.get("sheet")
    if sheet is not None and sheet.visible:
        sheet.draw()
        ctx["sheet_drawn_ok"] = True
    toasts.draw_toasts()
