"""The Delve, systems: rules wiring, the contract, the visible d20.

Everything here is *behavior*: the founding (class cards driving real
``bd_dnd.CreationWizard`` runs), the per-map delve contract (census,
crowned Warden, bd_quests quest), the visible-d20 announcer, the generic
locked-door bash/pick dispatcher (any locked line, any map),
level-toughened blows through the ``actor_before_damage`` filter, kill
XP with floating popups, the class actives, rest-with-teeth (sanctuary /
fitful / nightmare-made-real), the Guild Hound's party wiring, the slim
progression strip, and the savegame cold restore. No engine events are
registered at import time: ``main.py`` calls the ``wire_*``/``found_*``
functions from its own handlers, so this module is inert when the engine
executes it standalone from the PYTHON manifest.
"""

from __future__ import annotations

import math
from typing import Any, Dict, List, Optional, Tuple

import biaseddoom as bd
import bd_dnd
import bd_horror
import bd_quests
from bd_horror import toasts

try:
    import crypt_content as content
except ImportError:  # standalone manifest load: self-register the sibling
    content = bd.import_script("pyscripts/content.py",
                               module_name="crypt_content")


def player_pawn() -> Any:
    """Live handle to the local player's pawn, or None."""
    try:
        player = bd.player(0)
        if player is None or not player.valid:
            return None
        pawn = player.actor
        return pawn if pawn is not None and pawn.valid else None
    except RuntimeError:
        return None


def current_map_name() -> str:
    """The current map lump name ("" off-map)."""
    try:
        return str(bd.current_map() or "")
    except Exception:
        return ""


#: The founded hero (None until the founding window or the headless
#: default founds one). Hero-dependent event handlers read this at event
#: time and no-op before founding.
hero: Optional["bd_dnd.Character"] = None
hero_state: Optional[Any] = None
party: Optional["bd_dnd.Party"] = None
party_state: Optional[Any] = None
companion: Optional["bd_dnd.Companion"] = None
door_bash: Optional["DoorBashRules"] = None
reflex_save: Optional["ReflexSaveRule"] = None
#: The toughened-blows filter handler registered at founding (exactly
#: once per session; the autotest drives synthetic events through it).
level_damage_handler: Optional[Any] = None

#: Autotest-visible death-knell log (member names, in order).
companion_died_log: List[str] = []


# --- visible d20 rolls --------------------------------------------------------------

#: Every announced check, newest last: {"label", "text", "success",
#: "total", "dc"}. The headless autotest asserts on this.
check_log: List[Dict[str, Any]] = []


def announce_check(label: str, result: Dict[str, Any],
                   center: bool = True) -> Dict[str, Any]:
    """Make one check visible: center message + floating d20 readout.

    The readout rides the rich result dict every bd_dnd check/save
    returns: "d20 {kept} + {mod} = {total} vs DC {dc}", gold on success,
    red on failure, floating over the player for a moment (the roguelike
    popup idiom). Headless drawing calls are safe no-ops; the bookkeeping
    in ``check_log`` is the assertable half. ``center=False`` skips the
    center-screen message (the reflex save uses it: it rolls on every
    incoming hit, and a center banner mid-combat blocks the view); the
    floating d20 still shows.
    """
    roll = result.get("roll") or {}
    kept = roll.get("kept")
    mod = int(result.get("ability_mod") or 0) + int(result.get("prof") or 0)
    total = result.get("total")
    dc = result.get("dc")
    success = bool(result.get("success"))
    text = (f"{label}: d20 {kept} + {mod} = {total} vs DC {dc} "
            f"({'success' if success else 'fail'})")
    entry = {"label": str(label), "text": text, "success": success,
             "total": total, "dc": dc}
    check_log.append(entry)
    if center:
        try:
            bd.center_message(text)
        except Exception:
            pass
    pawn = player_pawn()
    if pawn is not None:
        try:
            color = (255, 200, 60) if success else (220, 60, 50)
            bd.draw_world_text(pawn, id=content.CHECK_POPUP_ID, text=text,
                               offset_z=44.0, color=color, height=0.03,
                               outline=True, occlude=False,
                               duration=content.POPUP_SECONDS)
        except Exception:
            pass  # headless / world mutating
    return entry


# --- health unification ("Blood is the body") -----------------------------------

#: The health-sync bookkeeping: the armed map-local diff task, the last
#: pawn health the detector rebaselined to, and the reentrancy guard
#: (the companion module uses the same ``_syncing`` pattern).
health_sync: Dict[str, Any] = {"task": None, "last_pawn_health": None,
                               "syncing": False}


def _is_player_pawn(ref: Any) -> bool:
    try:
        return ref is not None and ref.valid and bool(ref.is_player)
    except Exception:
        return False


def _sheet_loss_for(damage: int, character: "bd_dnd.Character") -> int:
    """Sheet hp lost when the pawn takes ``damage`` (ratio contract)."""
    if character is None or character.max_hp <= 0 or damage <= 0:
        return 0
    return int(damage * character.max_hp / 100 + 0.5)


def _on_pawn_damaged(event: Dict[str, Any]) -> None:
    """actor_damaged hook: pawn damage removes sheet hp by ratio.

    actor_damaged reports what landed after armor, so armor integrates
    naturally by reducing pawn damage pre-sync. Registered globally (it
    self-gates on the founded hero); the ``syncing`` guard keeps the
    hard-resync's own downward correction from double-charging.
    """
    if hero is None or health_sync["syncing"]:
        return
    victim = event.get("actor_ref")
    if not _is_player_pawn(victim):
        return
    try:
        damage = int(event.get("damage") or 0)
    except (TypeError, ValueError):
        return
    loss = _sheet_loss_for(damage, hero)
    if loss <= 0:
        return
    hero.set_hp(hero.hp - loss)
    health_sync["last_pawn_health"] = victim.health


def heal_pawn_to_ratio(pawn: Any, character: "bd_dnd.Character",
                       hard: bool = False) -> int:
    """Heal the pawn toward the sheet's ratio; returns the health delta.

    RPG-side heals (rests, Second Wind, level-up) call this after the
    sheet heal: upward only, unless ``hard`` (the level-up and map_load
    hard resync, which also snaps a too-healthy pawn back down through a
    guarded native damage call). The detector is rebaselined afterwards
    so the correction itself never counts as a medikit.
    """
    if pawn is None or character is None or character.max_hp <= 0:
        return 0
    target = int(character.hp / character.max_hp * 100 + 0.5)
    target = max(0, min(100, target))
    try:
        before = pawn.health
    except Exception:
        return 0
    health_sync["syncing"] = True
    try:
        if pawn.health < target:
            pawn.heal(target - pawn.health)
        elif hard and pawn.health > target:
            factor = pawn.damage_factor
            pawn.damage_factor = 1.0
            try:
                pawn.damage(pawn.health - target)
            finally:
                pawn.damage_factor = factor
    except Exception as exc:
        bd.warn(f"delve: pawn ratio heal failed: {exc!r}")
    finally:
        health_sync["syncing"] = False
    try:
        after = pawn.health
    except Exception:
        after = before
    health_sync["last_pawn_health"] = after
    return after - before


def apply_sheet_heal(character: "bd_dnd.Character", amount: int,
                     pawn: Any = None) -> int:
    """RPG-side heal: the sheet first, then the pawn toward the ratio."""
    before = character.hp
    character.set_hp(character.hp + max(0, int(amount)))
    healed = character.hp - before
    pawn = pawn if pawn is not None else player_pawn()
    heal_pawn_to_ratio(pawn, character)
    return healed


def hard_resync_health(pawn: Any = None) -> None:
    """Snap the pawn to the sheet's ratio (level-up and map_load)."""
    if hero is None:
        return
    pawn = pawn if pawn is not None else player_pawn()
    heal_pawn_to_ratio(pawn, hero, hard=True)


def _health_sync_tick() -> None:
    """The diff detector: pawn-health GAINS heal the sheet by ratio.

    Damage is owned by the immediate actor_damaged hook; this task only
    credits gains it has not already baselined (medikits, stimpacks,
    soulspheres, pawn.heal from RPG effects).
    """
    if hero is None or health_sync["syncing"]:
        return
    pawn = player_pawn()
    if pawn is None:
        health_sync["last_pawn_health"] = None
        return
    current = pawn.health
    last = health_sync["last_pawn_health"]
    health_sync["last_pawn_health"] = current
    if last is None or current <= last:
        return
    gain = int((current - last) * hero.max_hp / 100 + 0.5)
    if gain > 0:
        hero.set_hp(hero.hp + gain)


def arm_health_sync() -> bool:
    """Hard-resync to the sheet ratio and (re)arm the map-local task.

    Called on every map_load (fresh and from_savegame) once a hero
    exists, and from found_hero. The previous task id is cancelled first
    (a map unload already killed it; the cancel is only belt and braces).
    """
    if hero is None:
        return False
    hard_resync_health()
    old = health_sync.get("task")
    if old is not None:
        try:
            bd.cancel_task(old)
        except Exception:
            pass
        health_sync["task"] = None
    try:
        task = bd.schedule(_health_sync_tick,
                           delay=content.HEALTH_SYNC_TICS,
                           repeat=content.HEALTH_SYNC_TICS, map_local=True)
    except Exception as exc:
        bd.warn(f"delve: could not arm the health sync: {exc!r}")
        return False
    health_sync["task"] = task
    return True


# --- perception trap sense (every map) -----------------------------------------

#: Trap-sense bookkeeping: map names already fired, roll count, and the
#: last result (autotest-visible).
trap_sense_state: Dict[str, Any] = {"fired": set(), "rolls": 0,
                                    "last_result": None}


def trap_sense_check(event: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    """First entry per map into a damaging sector: roll Perception.

    ``sector.damage > 0`` is nukage/lava territory; the first such entry
    each map rolls Perception vs. TRAP_SENSE_DC through the visible
    announce path, once per map per session. Success toasts the warning;
    failure stays silent (the floor keeps its secret).
    """
    if hero is None:
        return None
    try:
        if int(event.get("player_index", -1)) < 0:
            return None
    except (TypeError, ValueError):
        return None
    try:
        sector = bd.sector(int(event.get("sector")))
    except Exception:
        return None
    try:
        if sector is None or int(sector.damage) <= 0:
            return None
    except Exception:
        return None
    map_name = current_map_name()
    if map_name in trap_sense_state["fired"]:
        return None
    trap_sense_state["fired"].add(map_name)
    trap_sense_state["rolls"] += 1
    try:
        result = hero.skill_check("perception", content.TRAP_SENSE_DC)
    except Exception as exc:
        bd.warn(f"delve: trap sense roll failed: {exc!r}")
        return None
    trap_sense_state["last_result"] = result
    announce_check("Perception (trap sense)", result)
    if result.get("success"):
        try:
            toasts.toast(content.TOAST_TRAP_SENSE, kind="omen")
        except Exception:
            pass
    return result


# --- insight examine (every map) -----------------------------------------------

#: Examine bookkeeping: the armed probe task, the cached crosshair
#: target (handle identity), and the current readout (autotest-visible).
examine_state: Dict[str, Any] = {"task": None, "ref": None,
                                 "class_name": "", "name": "", "hp": 0,
                                 "max_hp": 0, "xp": None, "deadly": False,
                                 "line": ""}


def describe_examine(character: "bd_dnd.Character", class_name: str
                     ) -> Dict[str, Any]:
    """The graded half of an examine (pure): XP value and threat note,
    gated on Insight proficiency. Without proficiency the crosshair shows
    only name and HP."""
    xp = _xp_for_class(class_name)
    skills = getattr(character, "proficient_skills", ()) or ()
    if character is None or "insight" not in skills:
        return {"xp": None, "deadly": False, "value": xp}
    deadly = xp > 0 and xp >= (content.THREAT_MULTIPLIER
                               * content.DEADLY_BASE_BOUNTY
                               * max(1, character.level))
    return {"xp": xp, "deadly": bool(deadly), "value": xp}


def _clear_examine() -> None:
    examine_state.update({"ref": None, "class_name": "", "name": "",
                          "hp": 0, "max_hp": 0, "xp": None,
                          "deadly": False, "line": ""})
    try:
        bd.draw_clear(content.EXAMINE_LINE_ID)
    except Exception:
        pass


def _aim_target(pawn: Any) -> Any:
    """The live monster nearest the crosshair, or None.

    Pure geometry plus the native sight check, firing nothing: monsters
    inside EXAMINE_DISTANCE whose bearing from the pawn is within the aim
    cone (a floor half-angle, widened at close range by the apparent body
    size) and whose center pitch roughly matches the pawn's aim pass, and
    ``check_sight`` keeps walls opaque. The previous probe fired a real
    zero-damage ``bd.line_attack``: it still spawned BulletPuffs and
    bullet decals on every wall along the trace, which read as constant
    phantom gunfire (the examine runs every EXAMINE_PROBE_TICS tics).
    """
    try:
        px, py, pz = pawn.position
        aim_angle = float(pawn.angle)
        aim_pitch = float(pawn.pitch)
    except Exception:
        return None
    try:
        refs = bd.actor_refs(sphere=(px, py, content.EXAMINE_DISTANCE))
    except Exception:
        return None
    if not refs:
        return None
    try:
        rows = bd.actor_field_batch(
            refs, ("alive", "is_monster", "is_player", "x", "y", "z"))
    except Exception:
        return None
    best = None
    best_dist = 0.0
    for ref, row in zip(refs, rows):
        try:
            alive, is_monster, is_player, ax, ay, az = row
            if not alive or not is_monster or is_player:
                continue
            if ax is None or ay is None or az is None:
                continue
            dx = float(ax) - px
            dy = float(ay) - py
            dist_xy = math.hypot(dx, dy)
            if dist_xy < 1.0 or dist_xy > content.EXAMINE_DISTANCE:
                continue
            bearing = math.degrees(math.atan2(dy, dx))
            off = abs((bearing - aim_angle + 180.0) % 360.0 - 180.0)
            half = max(content.EXAMINE_AIM_HALF_ANGLE_DEG,
                       math.degrees(math.atan2(
                           content.EXAMINE_AIM_BODY_UNITS, dist_xy)))
            if off > half:
                continue
            pitch_to = math.degrees(math.atan2(
                (float(az) + content.EXAMINE_AIM_BODY_UNITS)
                - (pz + content.EXAMINE_EYE_HEIGHT), dist_xy))
            if abs(pitch_to - aim_pitch) > content.EXAMINE_AIM_PITCH_DEG:
                continue
            if not ref.check_sight(pawn):
                continue
        except Exception:
            continue
        if best is None or dist_xy < best_dist:
            best = ref
            best_dist = dist_xy
    return best


def _examine_probe_tick() -> None:
    """Acquire the crosshair monster geometrically and keep the line.

    The readout caches by handle identity, so the display item is
    only re-registered when the target changes or its HP moves; the
    threat note and XP value appear only for the Insight-proficient.
    """
    if hero is None:
        _clear_examine()
        return
    pawn = player_pawn()
    if pawn is None:
        return
    target = _aim_target(pawn)
    try:
        ok = (target is not None and target.valid and target.alive
              and target.is_monster)
    except Exception:
        ok = False
    if not ok:
        _clear_examine()
        return
    warden_tid = contract_state.get("warden_tid")
    is_warden = False
    try:
        is_warden = bool(warden_tid) and int(target.tid) == int(warden_tid)
    except Exception:
        is_warden = False
    if target == examine_state["ref"]:
        try:
            examine_state["hp"] = int(target.health)
            examine_state["max_hp"] = max(examine_state["max_hp"],
                                          examine_state["hp"])
        except Exception:
            pass
    else:
        try:
            class_name = str(target.class_name)
            hp = int(target.health)
        except Exception:
            _clear_examine()
            return
        if is_warden:
            name = (f"Warden of "
                    f"{str(contract_state.get('map') or '').upper()}"
                    f" ({class_name})")
            max_hp = int(contract_state.get("warden_max_health") or hp)
        else:
            name = class_name
            max_hp = hp
        examine_state.update({"ref": target, "class_name": class_name,
                              "name": name, "hp": hp, "max_hp": max_hp})
    graded = describe_examine(hero, examine_state["class_name"])
    examine_state["xp"] = graded["xp"]
    examine_state["deadly"] = graded["deadly"]
    line = (f"{examine_state['name']}  HP {examine_state['hp']}"
            f"/{examine_state['max_hp']}")
    if graded["xp"] is not None:
        line += f"  {graded['xp']} XP"
    if graded["deadly"]:
        line += "  deadly"
    examine_state["line"] = line
    try:
        bd.draw_text(line, id=content.EXAMINE_LINE_ID, x=0.01, y=0.92,
                     height=0.02, color="crimson", shadow=True, layer=8)
    except Exception:
        pass  # headless: the bookkeeping is the assertable half


def arm_examine_probe() -> bool:
    """(Re)arm the 7-tic map-local crosshair probe (founding/map_load)."""
    if hero is None:
        return False
    old = examine_state.get("task")
    if old is not None:
        try:
            bd.cancel_task(old)
        except Exception:
            pass
        examine_state["task"] = None
    try:
        task = bd.schedule(_examine_probe_tick,
                           delay=content.EXAMINE_PROBE_TICS,
                           repeat=content.EXAMINE_PROBE_TICS,
                           map_local=True)
    except Exception as exc:
        bd.warn(f"delve: could not arm the examine probe: {exc!r}")
        return False
    examine_state["task"] = task
    return True


# --- bash or pick any locked door (every map) ----------------------------------------


class _CardLendingDoorCheck(bd_dnd.LockedDoorCheck):
    """A LockedDoorCheck that lends every Doom key card for one activation.

    Doom locks 1-3 (card only) and 129-134 (any card or skull) all accept
    the card classes, so lending all three covers every stock Doom locked
    door without a lock-number table. The keys are reclaimed right after
    the single native ``activate`` call, exactly like the framework's
    one-key path. Every attempt is announced through
    :func:`announce_check` (visible d20).
    """

    def attempt(self, activator: Any = None, rng: Any = bd
                ) -> Dict[str, Any]:
        result = super().attempt(activator, rng=rng)
        skill, _verb = self.MODES[self.mode]
        label = ("Sleight of hand" if self.mode == "dex"
                 else "Athletics")
        announce_check(label, result)
        return result

    def _open_door(self, activator: Any) -> bool:
        if activator is None:
            return super()._open_door(activator)
        try:
            line = bd.line(self.line_index)
        except Exception as exc:
            bd.warn(f"delve: cannot resolve line "
                    f"{self.line_index}: {exc!r}")
            return False
        if line is None:
            return False  # no such line: the dispatcher retires the line
        granted: List[str] = []
        result = 0
        try:
            for key_class in content.BASH_KEY_CLASSES:
                try:
                    activator.give_inventory(key_class, 1)
                    granted.append(key_class)
                except Exception:
                    pass  # not a Doom configuration; try activating anyway
            result = line.activate(activator, clear=True)
        except Exception as exc:
            bd.warn(f"delve: door activation failed: {exc!r}")
        finally:
            for key_class in granted:
                try:
                    activator.take_inventory(key_class, 1)
                except Exception:
                    pass
        return bool(result)


def door_mode_for(character: "bd_dnd.Character") -> Tuple[str, int]:
    """Rogues pick locks (dex, DC - 2); everyone else bashes (str)."""
    if getattr(character, "class_id", None) == "Rogue":
        return "dex", content.DOOR_DC - content.ROGUE_DOOR_DC_DELTA
    return "str", content.DOOR_DC


class DoorBashRules:
    """Make every locked door in the game bashable (or pickable).

    One ``line_activation_failed`` dispatcher: when the local player fails
    to open a locked line, a per-(map, line) ``_CardLendingDoorCheck`` is
    created and run. Later uses of the same line are answered by the
    check's own framework handler (registered at creation); the dispatcher
    steps aside for lines it already knows. A line whose activation is
    refused even with the lent cards (a non-Doom configuration, an exotic
    lock) is remembered in :attr:`unbashable` and never retried, so a
    foreign door never spams checks. The first touch of a line raises a
    hint toast, once.
    """

    def __init__(self, character: "bd_dnd.Character",
                 dc: int = content.DOOR_DC, mode: str = "str") -> None:
        self.character: "bd_dnd.Character" = character
        self.dc: int = int(dc)
        self.mode: str = str(mode)
        self.checks: Dict[Tuple[str, int], _CardLendingDoorCheck] = {}
        self.unbashable: set = set()
        #: Lines whose first touch already raised the bash-hint toast.
        self._hinted: set = set()

        @bd.on("line_activation_failed")
        def _handler(event: Dict[str, Any]) -> None:
            self._dispatch(event)

        self._handler = _handler

    @staticmethod
    def _map_key() -> str:
        return current_map_name().upper()

    def bashable_door(self, line_index: int, dc: Optional[int] = None
                      ) -> Optional[_CardLendingDoorCheck]:
        """Fetch or create the check for one locked line (current map).

        Returns None when the line was already proven unbashable. The
        autotest pre-registers the MAP02 red door through this to pin its
        DC before the first use.
        """
        key = (self._map_key(), int(line_index))
        if key in self.unbashable:
            return None
        check = self.checks.get(key)
        if check is None:
            check = _CardLendingDoorCheck(
                int(line_index), self.character, mode=self.mode,
                dc=self.dc if dc is None else int(dc))
            self.checks[key] = check
        return check

    def _dispatch(self, event: Dict[str, Any], rng: Any = bd
                  ) -> Optional[Dict[str, Any]]:
        if event.get("reason") != "locked":
            return None
        activator = event.get("actor_ref")
        try:
            if activator is None or not activator.is_player:
                return None
        except Exception:
            return None
        try:
            line_index = int(event.get("line_index"))
        except (TypeError, ValueError):
            return None
        key = (self._map_key(), line_index)
        if key in self.unbashable:
            return None
        if key in self.checks:
            return None  # the line's own check handler answers this event
        check = self.bashable_door(line_index)
        if check is None:
            return None
        if key not in self._hinted:
            # First touch of a locked line: make the mechanic visible
            # before the roll speaks.
            self._hinted.add(key)
            try:
                toasts.toast(content.TOAST_BASH_HINT, kind="info")
            except Exception:
                pass
        result = check.attempt(activator, rng=rng)
        if result.get("success") and not check.opened:
            # The roll passed but the line refused the lent cards: not a
            # Doom-style door. Never roll for it again.
            self.unbashable.add(key)
        return result


# --- the announced reflex save (every map) -------------------------------------------


class ReflexSaveRule(bd_dnd.DamageSaveRule):
    """A DamageSaveRule whose every roll is a visible d20.

    The center-screen banner is suppressed: the save rolls on every
    incoming hit (35-tic cooldown), and a center message mid-combat
    blocks the view. The floating d20 readout and the check log still
    show every roll.
    """

    def apply(self, victim: Any, damage: int, damage_type: Any = None,
              rng: Any = bd) -> Dict[str, Any]:
        result = super().apply(victim, damage, damage_type=damage_type,
                               rng=rng)
        save = result.get("save")
        if save is not None and not result.get("skipped"):
            announce_check("Reflex save", save, center=False)
        return result


# --- level-toughened blows (every map) -----------------------------------------------


def level_damage_bonus(character: "bd_dnd.Character") -> int:
    """Bonus damage per hit at the character's current level (capped)."""
    return max(0, min(content.LEVEL_DAMAGE_BONUS_CAP, character.level - 1))


def level_damage_bonus_at(level: int) -> int:
    """The toughened-blows bonus a level grants (for the level-up diff)."""
    return max(0, min(content.LEVEL_DAMAGE_BONUS_CAP, int(level) - 1))


def hit_bonus_for(character: "bd_dnd.Character", distance: float) -> int:
    """Total bonus damage on one player hit: level + boons + skill perks.

    The level part (capped), the Deadly boon (stacking), Athletics CQB
    training (+2 within 96 units, requires proficiency), and Perception
    dead-eye (+1 beyond 512, requires proficiency). Distances are planar,
    measured from the event's refs by the filter.
    """
    bonus = level_damage_bonus(character) + int(boon_state.get("deadly", 0))
    try:
        skills = getattr(character, "proficient_skills", ()) or ()
        if "athletics" in skills and distance <= content.CQB_RANGE:
            bonus += content.CQB_BONUS
        if "perception" in skills and distance > content.DEADEYE_RANGE:
            bonus += content.DEADEYE_BONUS
    except Exception:
        pass
    return bonus


def skill_bonus_total(character: "bd_dnd.Character", skill: str) -> int:
    """A skill's total bonus: ability modifier + proficiency (when
    trained) + use-based mastery (bd_dnd helpers, the 32_dialogue_trees
    skill_bonus approach extended with mastery)."""
    ability = bd_dnd.SKILLS.get(str(skill))
    if character is None or ability is None:
        return 0
    bonus = int(character.abilities.mod(ability))
    try:
        if str(skill) in (getattr(character, "proficient_skills", ())
                          or ()):
            bonus += int(character.proficiency)
    except Exception:
        pass
    try:
        bonus += int(bd_dnd.mastery(character, skill))
    except Exception:
        pass
    return bonus


def wire_level_damage(character: "bd_dnd.Character") -> Any:
    """Add the hit bonus to every hit the local player lands on a monster,
    through the mutable ``actor_before_damage`` filter (level toughening,
    Deadly boons, Athletics CQB, Perception dead-eye). Returns the handler
    for tests."""

    @bd.on("actor_before_damage")
    def _on_before_damage(event: Dict[str, Any]) -> None:
        try:
            if event.get("attacker_player_index") != 0:
                return
            target = event.get("actor_ref")
            if target is None or not target.valid or not target.is_monster:
                return
            attacker = event.get("attacker_ref")
            try:
                if attacker is None or not attacker.valid:
                    attacker = None
            except Exception:
                attacker = None
            if attacker is None:
                attacker = player_pawn()
            if attacker is None:
                return
            try:
                distance = math.hypot(target.x - attacker.x,
                                      target.y - attacker.y)
            except Exception:
                distance = 0.0
            bonus = hit_bonus_for(character, distance)
            if bonus <= 0:
                return
            event["damage"] = int(event.get("damage") or 0) + bonus
        except Exception as exc:
            bd.warn(f"delve: hit bonus failed: {exc!r}")

    return _on_before_damage


# --- level-up fanfare (every map) ------------------------------------------------------

#: Autotest-visible level-up log: one entry per level reached.
levelup_log: List[Dict[str, Any]] = []


def ring_burst(ref: Any, color: Tuple[int, int, int] = content.WARDEN_TINT,
               radius: float = 24.0, seconds: float = 1.4) -> None:
    """A gold-style ring burst at an actor's feet (15's playerfx idiom)."""
    if ref is None:
        return
    try:
        bd.draw_world_ring(ref, id=content.PLAYER_RING_ID, radius=radius,
                           color=color, alpha=0.95, offset_z=2.0,
                           segments=28, duration=seconds)
    except Exception:
        pass  # headless / world mutating


def wire_level_up(character: "bd_dnd.Character") -> None:
    """Level-up fanfare: full heal (sheet and pawn), ring burst, chime,
    "LEVEL N" center message, and the toughened-blows note when the
    table ticks up. Deliberately no screen tint: full-view flashes fight
    the player's aim mid-combat."""

    def on_level_up(hero_: "bd_dnd.Character", new_level: int) -> None:
        levelup_log.append({"level": int(new_level),
                            "name": str(hero_.name)})
        hero_.set_hp(hero_.max_hp)
        pawn = player_pawn()
        if pawn is not None:
            # Level-up hard resync: both pools to the (full) sheet ratio.
            heal_pawn_to_ratio(pawn, hero_, hard=True)
            ring_burst(pawn)
        queue_boon()
        try:
            bd.play_ui_sound(content.LEVEL_UP_SOUND, volume=0.8)
        except Exception:
            pass
        bd.center_message(f"LEVEL {new_level}")
        bonus = level_damage_bonus(hero_)
        if bonus > level_damage_bonus_at(new_level - 1):
            toasts.toast(f"Your blows land harder (+{bonus} damage).",
                         kind="quest")

    character.on_level_up.append(on_level_up)


# --- kill XP popups (every map) ---------------------------------------------------------

#: Autotest-visible popup log: {"xp", "class_name", "bonus"}.
popup_log: List[Dict[str, Any]] = []
_popup_counter = 0


def xp_popup(ref: Any, amount: int, label: str = "", bonus: bool = False
             ) -> None:
    """Floating gold "+N XP" over a dying monster (transient: it keeps
    playing at the anchor after the actor dies)."""
    global _popup_counter
    popup_log.append({"xp": int(amount), "class_name": label,
                      "bonus": bool(bonus)})
    _popup_counter += 1
    if ref is None:
        return
    try:
        bd.draw_world_text(ref, id=content.XP_POPUP_BASE
                           + _popup_counter % content.XP_POPUP_SLOTS,
                           text=f"+{int(amount)} XP",
                           offset_z=30.0, color=(255, 200, 60),
                           height=0.026, outline=True, occlude=False,
                           duration=content.XP_POPUP_SECONDS)
    except Exception:
        pass  # headless / world mutating


def _xp_for_class(class_name: str) -> int:
    return {k.lower(): v for k, v in
            bd_dnd.DEFAULT_XP_TABLE.items()}.get(str(class_name).lower(), 0)


def _on_kill_xp_popup(event: Dict[str, Any]) -> None:
    """Pop "+N XP" over every player-credited tabled kill (the visible
    half of track_xp_from_kills, which runs announce=False here)."""
    if hero is None:
        return
    try:
        if event.get("attacker_player_index") != 0:
            return
    except Exception:
        return
    snapshot = event.get("actor") or {}
    if not snapshot.get("is_monster") or snapshot.get("is_player"):
        return
    class_name = str(snapshot.get("class_name") or "")
    xp = _xp_for_class(class_name)
    if not xp:
        return
    xp_popup(event.get("actor_ref"), xp, class_name)


# --- class actives (Custom Action 3, every map) -----------------------------------------

#: Autotest-visible log of class-active uses: {"id", "ok", ...}.
active_log: List[Dict[str, Any]] = []


def use_class_active(character: Optional["bd_dnd.Character"] = None,
                     pawn: Any = None, rng: Any = bd) -> Dict[str, Any]:
    """Fire one class active: spend the charge, apply the pawn effect.

    Custom Action 3 (auto-bound to C, alias ``class_active``) calls this
    for the founded hero; the autotest passes unit characters directly.
    With no charges left, says so and names the refill (the rest action).
    """
    character = character if character is not None else hero
    pawn = pawn if pawn is not None else player_pawn()
    if character is None:
        return {"ok": False, "reason": "no hero"}
    spec = content.CLASS_ACTIVES.get(getattr(character, "class_id", ""))
    if spec is None:
        return {"ok": False, "reason": "no active"}
    if pawn is None:
        return {"ok": False, "reason": "no pawn"}
    resource = spec["resource"]
    if not character.use_resource(resource):
        message = (f"{spec['name']}: no charges left; rest (V) restores "
                   f"them.")
        bd.center_message(message)
        toasts.toast(message, kind="info")
        outcome = {"ok": False, "reason": "exhausted", "id": spec["id"],
                   "message": message}
        active_log.append(outcome)
        return outcome
    outcome: Dict[str, Any] = {"ok": True, "id": spec["id"]}
    try:
        if spec["id"] == "second_wind":
            rolled = bd_dnd.roll(f"d{character.hit_die}", rng=rng)
            amount = rolled["total"] + character.level
            character._log_roll("second wind", amount,
                                detail=f"d{character.hit_die}"
                                       f"({rolled['total']})"
                                       f"+{character.level}")
            character.set_hp(character.hp + amount)
            outcome["amount"] = amount
            outcome["healed_pawn"] = heal_pawn_to_ratio(pawn, character)
            message = f"Second wind! +{amount}."
        elif spec["id"] == "uncanny_dodge":
            prior = pawn.damage_factor
            pawn.damage_factor = content.UNCANNY_DODGE_FACTOR

            def _restore(pawn_=pawn, prior_=prior):
                try:
                    if pawn_ is not None and pawn_.valid:
                        pawn_.damage_factor = prior_
                except Exception:
                    pass

            bd.schedule(_restore, delay=content.UNCANNY_DODGE_TICS,
                        map_local=False)
            outcome["factor"] = content.UNCANNY_DODGE_FACTOR
            outcome["tics"] = content.UNCANNY_DODGE_TICS
            message = "You blur; the world swings wide of you."
        elif spec["id"] == "turn_the_unholy":
            damage = content.TURN_BASE_DAMAGE + 2 * character.level
            dealt = bd.radius_damage(pawn, damage, content.TURN_RADIUS,
                                     source=pawn, damage_type="Fire",
                                     hurt_source=False)
            outcome["damage"] = damage
            outcome["dealt"] = dealt
            message = f"Turn the unholy! ({damage} fire)"
        else:
            bd.warn(f"delve: no effect branch for active {spec['id']!r}")
            message = spec["name"]
    except Exception as exc:
        bd.warn(f"delve: class active {spec['id']!r} failed: {exc!r}")
        outcome["ok"] = False
        outcome["reason"] = "error"
        active_log.append(outcome)
        return outcome
    outcome["message"] = message
    active_log.append(outcome)
    bd.center_message(message)
    toasts.toast(message, kind="quest")
    return outcome


# --- level-up boons (pick one of three) ------------------------------------------

#: Boon bookkeeping: queued picks, the taken list, and the Deadly
#: stacking damage counter (read by hit_bonus_for). Pending and taken
#: boons persist through bd.state; the autotest reads all three.
boon_state: Dict[str, Any] = {"pending": 0, "taken": [], "deadly": 0}


def _persist_boons() -> None:
    """Mirror the boon bookkeeping into bd.state (checkpoint-safe)."""
    try:
        bd.state[content.BOONS_STATE_KEY] = {
            "pending": int(boon_state["pending"]),
            "taken": list(boon_state["taken"]),
            "deadly": int(boon_state["deadly"])}
    except Exception as exc:
        bd.warn(f"delve: boon persistence failed: {exc!r}")


def boon_pending() -> int:
    """How many boon picks are queued (the chooser opens while > 0)."""
    try:
        return int(boon_state.get("pending", 0))
    except Exception:
        return 0


def queue_boon() -> None:
    """Queue one boon pick (wired to every level-up)."""
    boon_state["pending"] = boon_pending() + 1
    _persist_boons()
    try:
        toasts.toast(content.TOAST_BOON, kind="quest")
    except Exception:
        pass


def pick_boon(boon_id: str) -> bool:
    """Apply one queued boon by id (the chooser buttons AND the autotest
    drive this same function). Returns False without a queued pick."""
    if hero is None or boon_pending() <= 0:
        return False
    spec = None
    for entry in content.BOONS:
        if entry.get("id") == str(boon_id):
            spec = entry
            break
    if spec is None:
        return False
    if spec["id"] == "toughness":
        hero.max_hp += content.TOUGHNESS_HP
        hero.set_hp(hero.hp + content.TOUGHNESS_HP)
    elif spec["id"] == "deadly":
        boon_state["deadly"] = int(boon_state.get("deadly", 0)) + 1
    elif spec["id"] == "prepared":
        active = content.CLASS_ACTIVES.get(
            getattr(hero, "class_id", ""), {})
        resource = active.get("resource")
        if resource:
            hero.resource_max[resource] = \
                hero.resource_max.get(resource, 0) + 1
    boon_state["taken"].append(spec["id"])
    boon_state["pending"] = boon_pending() - 1
    _persist_boons()
    try:
        bd.center_message(f"Boon: {spec['name']}")
    except Exception:
        pass
    return True


def _reconcile_boons() -> None:
    """Restore pending/taken/deadly from bd.state after a load."""
    saved = bd.state.get(content.BOONS_STATE_KEY)
    if not isinstance(saved, dict):
        return
    try:
        boon_state["pending"] = max(0, int(saved.get("pending", 0)))
        taken = saved.get("taken")
        boon_state["taken"] = ([str(b) for b in taken]
                               if isinstance(taken, list) else [])
        boon_state["deadly"] = max(0, int(saved.get("deadly", 0)))
    except Exception as exc:
        bd.warn(f"delve: boon reconcile failed: {exc!r}")


# --- rest with teeth (every map) ---------------------------------------------------------

#: Map time of the last rest (the cooldown clock); the autotest resets it
#: between rest branches through reset_rest_cooldown().
last_rest_tic: int = -(1 << 30)
#: Autotest-visible rest outcomes, newest last.
rest_log: List[Dict[str, Any]] = []
#: Autotest-visible nightmare spawns: tids of the demons the dark made.
nightmare_log: List[int] = []


def reset_rest_cooldown() -> None:
    """Clear the rest cooldown (autotest hook; interactive never calls)."""
    global last_rest_tic
    last_rest_tic = -(1 << 30)


def sanctuary_light_of(pawn: Any) -> int:
    """Light level of the pawn's sector (0 when unresolvable)."""
    if pawn is None:
        return 0
    try:
        sector = bd.sector_at(pawn.x, pawn.y)
        return int(sector.light) if sector is not None else 0
    except Exception:
        return 0


def _heal_pawn(pawn: Any, amount: int) -> int:
    """Heal the pawn through the native path; returns the actual gain."""
    if pawn is None or amount <= 0:
        return 0
    try:
        before = pawn.health
        pawn.heal(int(amount))
        return pawn.health - before
    except Exception:
        return 0


def _ring_spots(x: float, y: float, radius: float, count: int = 8
                ) -> List[Tuple[float, float]]:
    """Eight candidate spots on a ring around (x, y), nearest first."""
    return [(x + radius * math.cos(i * math.pi / 4.0),
             y + radius * math.sin(i * math.pi / 4.0))
            for i in range(count)]


def spawn_nightmares(pawn: Any, count: int) -> List[Any]:
    """The nightmare made real: hostile Demons around the sleeper.

    Each spawn tries the fit-checked ring first (``force=False``), with a
    forced spawn only as the last resort (a missed nightmare is a free
    pass, so the dark must always answer). Each demon is tinted dark and
    titled "Nightmare"; the tids ride ``nightmare_log`` for the autotest.
    """
    spawned: List[Any] = []
    if pawn is None:
        return spawned
    next_tid = content.NIGHTMARE_TID_BASE
    for i in range(max(1, int(count))):
        ref = None
        for sx, sy in _ring_spots(pawn.x, pawn.y,
                                  content.NIGHTMARE_RING_RADIUS):
            try:
                ref = bd.spawn(content.NIGHTMARE_CLASS, sx, sy, pawn.z,
                               angle=0.0, force=False)
                break
            except Exception:
                continue
        if ref is None:
            try:
                ref = bd.spawn(content.NIGHTMARE_CLASS, pawn.x + 48.0,
                               pawn.y, pawn.z, angle=0.0, force=True)
            except Exception as exc:
                bd.warn(f"delve: nightmare spawn failed: {exc!r}")
                continue
        try:
            while bd.actor_ref(next_tid) is not None:
                next_tid += 1
            ref.tid = next_tid
            next_tid += 1
        except Exception:
            pass
        try:
            ref.tint = content.NIGHTMARE_TINT
        except Exception:
            pass
        try:
            bd.draw_world_text(ref, id=content.NIGHTMARE_LABEL_BASE + i,
                               text=content.NIGHTMARE_TITLE,
                               offset_z=12.0, color=(150, 130, 200),
                               height=0.02, outline=True)
        except Exception:
            pass
        try:
            ref.target = pawn
        except Exception:
            pass
        nightmare_log.append(ref.tid)
        spawned.append(ref)
    return spawned


def sanctuary_threshold(character: "bd_dnd.Character") -> int:
    """The sanctuary light threshold for this rester: 160, or 140 with
    Religion proficiency; DARK DELVE adds 20."""
    threshold = content.SANCTUARY_LIGHT
    try:
        if "religion" in (getattr(character, "proficient_skills", ())
                          or ()):
            threshold = content.RELIGION_SANCTUARY_LIGHT
    except Exception:
        pass
    if _modifier_id() == "dark_delve":
        threshold += content.DARK_DELVE_SANCTUARY_DELTA
    return threshold


def nightmare_ability(character: "bd_dnd.Character") -> str:
    """Religion proficiency wards the dark with WIS instead of DEX."""
    try:
        if "religion" in (getattr(character, "proficient_skills", ())
                          or ()):
            return "wis"
    except Exception:
        pass
    return "dex"


def nightmare_dc() -> int:
    """The nightmare save DC (DARK DELVE adds 2)."""
    dc = content.NIGHTMARE_DC
    if _modifier_id() == "dark_delve":
        dc += content.DARK_DELVE_NIGHTMARE_DC_DELTA
    return dc


def try_long_rest(character: "bd_dnd.Character", pawn: Any = None,
                  rng: Any = bd) -> Dict[str, Any]:
    """The delve's rest rule: light is life; the dark has teeth.

    - **Cooldown**: a second rest within REST_COOLDOWN_TICS of the last
      says so and does nothing (``{"kind": "cooldown"}``).
    - **Sanctuary** (sector light >= the threshold, 160, or 140 for the
      Religion-proficient; DARK DELVE adds 20): a true long rest, the
      sheet heals fully, resources restore, and the pawn heals back to
      the sheet's ratio.
    - **Darkness**: sleep rolls the announced nightmare save (DEX vs. DC
      12, WIS for the Religion-proficient whose faith wards the dark;
      DARK DELVE adds 2). Success is *fitful*: half the missing HP on
      the sheet, and the pawn heals back toward the ratio. Failure is
      the nightmare made real: 1-2 hostile Demons spawn around the
      sleeper and no healing happens.

    Returns an outcome dict with a ``kind`` of ``"sanctuary"``,
    ``"fitful"``, ``"nightmare"``, or ``"cooldown"``.
    """
    pawn = pawn if pawn is not None else player_pawn()
    try:
        now = bd.level_time()
    except Exception:
        now = 0
    if now - last_rest_tic < content.REST_COOLDOWN_TICS:
        toasts.toast(content.TOAST_REST_COOLDOWN, kind="info")
        outcome = {"kind": "cooldown",
                   "remaining": content.REST_COOLDOWN_TICS
                   - (now - last_rest_tic)}
        rest_log.append(outcome)
        return outcome
    _set_rest_clock(now)
    light = sanctuary_light_of(pawn)
    threshold = sanctuary_threshold(character)
    if light >= threshold:
        outcome = character.rest(short=False, rng=rng)
        outcome["kind"] = "sanctuary"
        outcome["light"] = light
        outcome["threshold"] = threshold
        outcome["pawn_healed"] = heal_pawn_to_ratio(pawn, character)
        toasts.toast(content.TOAST_SANCTUARY, kind="quest")
        rest_log.append(outcome)
        return outcome
    ability = nightmare_ability(character)
    save = character.saving_throw(ability, nightmare_dc(), rng=rng)
    announce_check(f"Nightmare save ({ability.upper()})", save)
    if save["success"]:
        missing = character.max_hp - character.hp
        healed = int(missing * content.FITFUL_HEAL_FRACTION)
        if healed > 0:
            character.set_hp(character.hp + healed)
        pawn_healed = heal_pawn_to_ratio(pawn, character)
        toasts.toast(content.TOAST_FITFUL, kind="info")
        outcome = {"kind": "fitful", "save": save, "healed": healed,
                   "pawn_healed": pawn_healed, "light": light,
                   "ability": ability}
        rest_log.append(outcome)
        return outcome
    count = 1
    try:
        count = rng.randint(1, 2)
    except Exception:
        count = 1
    spawned = spawn_nightmares(pawn, count)
    toasts.toast(content.TOAST_NIGHTMARE, kind="harm")
    outcome = {"kind": "nightmare", "save": save, "healed": 0,
               "light": light, "spawned": len(spawned)}
    rest_log.append(outcome)
    return outcome


def _set_rest_clock(now: int) -> None:
    global last_rest_tic
    last_rest_tic = int(now)


# --- the per-map contract (bd_quests spine, every map) ---------------------------------

#: The live contract bookkeeping; mirrored into bd.state under
#: content.CONTRACT_STATE_KEY so a checkpoint reload can reconcile without
#: re-rolling. Autotest-visible. "modifier" is the rolled modifier dict
#: (or {"id": "plain"}); "warden_max_health" is the crowned health, kept
#: for the examine readout.
contract_state: Dict[str, Any] = {"map": None, "census": 0,
                                  "tribute_goal": 0, "warden_tid": None,
                                  "quest_id": None, "done": False,
                                  "modifier": None, "warden_max_health": 0}


def map_salt(map_name: str) -> int:
    """A stable byte-sum salt of the map name (str hash() would vary
    between processes)."""
    return sum(bytearray(str(map_name).encode("utf-8")))


def contract_seed(map_name: str) -> int:
    """The deterministic modifier seed for a map: the persisted
    ``delve_seed`` (default 29) plus the map's byte-sum salt."""
    try:
        base = int(bd.state.get(content.SEED_STATE_KEY,
                                content.DEFAULT_DELVE_SEED))
    except Exception:
        base = content.DEFAULT_DELVE_SEED
    return base + map_salt(map_name)


def roll_modifier(map_name: str, seed: Optional[int] = None) -> Dict[str, Any]:
    """The deterministic contract modifier for a map (a fresh seeded
    stream each call, so probing it costs nothing)."""
    if seed is None:
        seed = contract_seed(map_name)
    try:
        stream = bd.rng(int(seed))
        roll = stream.int(1, 100)
    except Exception:
        roll = 100  # no stream: the plain contract
    cumulative = 0
    for entry in content.CONTRACT_MODIFIERS:
        cumulative += int(entry["weight"])
        if roll <= cumulative:
            return dict(entry)
    return dict(content.CONTRACT_MODIFIERS[-1])


def _modifier_id() -> Optional[str]:
    """The armed modifier id on the contract map (None elsewhere/plain
    is "plain")."""
    if contract_state.get("map") != current_map_name():
        return None
    modifier = contract_state.get("modifier")
    if not isinstance(modifier, dict):
        return None
    return modifier.get("id")


def delve_depth() -> int:
    """Contracts completed so far (persisted depth)."""
    try:
        return int(bd.state.get(content.DEPTH_STATE_KEY, 0))
    except Exception:
        return 0


def warden_health_mult() -> float:
    """The Warden's health multiplier: 2.5 + 0.25/depth (cap 5), doubled
    by IRON WARDEN."""
    mult = min(content.WARDEN_HEALTH_MULT_CAP,
               content.WARDEN_HEALTH_MULT
               + content.WARDEN_HEALTH_MULT_PER_DEPTH * delve_depth())
    if _modifier_id() == "iron_warden":
        mult *= 2.0
    return mult


def warden_xp_mult() -> int:
    """The Warden's XP multiplier: 5 + depth (cap 10), +2 by IRON WARDEN
    (still capped)."""
    mult = min(content.WARDEN_XP_MULT_CAP,
               content.WARDEN_XP_MULT
               + content.WARDEN_XP_MULT_PER_DEPTH * delve_depth())
    if _modifier_id() == "iron_warden":
        mult = min(content.WARDEN_XP_MULT_CAP,
                   mult + content.IRON_WARDEN_XP_BONUS)
    return mult


def tribute_goal_for_map(census: int) -> int:
    """The map's tribute goal: max(3, census // 4) + 1/depth, doubled by
    HORDE, always capped at census - 1."""
    base = (content.tribute_goal_for(census)
            + content.TRIBUTE_GOAL_PER_DEPTH * delve_depth())
    if _modifier_id() == "horde":
        base *= 2
    return max(1, min(max(0, int(census) - 1), base))


def contract_reward_xp() -> int:
    """The map's contract reward: 200 + 50/depth, shaped by the
    modifier (HORDE +100, DARK DELVE x1.5, GUILD BOUNTY x2)."""
    xp = (content.CONTRACT_XP
          + content.CONTRACT_XP_PER_DEPTH * delve_depth())
    mod = _modifier_id()
    if mod == "horde":
        xp += content.HORDE_XP_BONUS
    elif mod == "dark_delve":
        xp = int(xp * content.DARK_DELVE_XP_MULT)
    elif mod == "guild_bounty":
        xp = int(xp * content.GUILD_BOUNTY_XP_MULT)
    return xp


def apply_hound_buff(fresh: bool = False) -> None:
    """BLOODHOUND: the hound at +50% max hp this map (base otherwise).

    ``fresh`` (a new map's setup) also heals the hound to its max; the
    checkpoint reconcile only aligns the pool (the PartyState restore
    owns the hp), so a saved wound never vanishes."""
    member = party.get(content.HOUND_NAME) if party is not None else None
    if member is None:
        return
    mult = (content.BLOODHOUND_HP_MULT
            if _modifier_id() == "bloodhound" else 1.0)
    new_max = max(1, int(content.HOUND_HP * mult))
    member.max_hp = new_max
    if fresh:
        member.set_hp(new_max)
    elif member.hp > new_max:
        member.set_hp(new_max)



#: Autotest-visible completion fanfare log.
fanfare_log: List[str] = []
#: Set True when the contract visuals were re-applied after a savegame
#: load (asserted in the checkpoint round-trip).
warden_visuals_reapplied = False


def _live_hostiles() -> List[Any]:
    """All live, hostile (non-FRIENDLY), non-player monsters on the map."""
    found = []
    try:
        refs = bd.actor_refs()
    except Exception as exc:
        bd.warn(f"delve: census failed: {exc!r}")
        return found
    for ref in refs:
        try:
            if not ref.valid or not ref.alive:
                continue
            if ref.is_player or not ref.is_monster:
                continue
            if ref.get_flag("FRIENDLY"):
                continue  # the Guild Hound is a Demon, but guild property
            found.append(ref)
        except Exception:
            continue
    return found


def pick_warden(monsters: List[Any]) -> Optional[Any]:
    """The Warden: highest XP-table value in the census; ties break to
    the lowest current tid (tid-less actors sort as tid 0, stable and
    deterministic)."""
    best = None
    best_key = None
    for ref in monsters:
        try:
            xp = _xp_for_class(ref.class_name)
            tid = int(ref.tid)
        except Exception:
            continue
        key = (xp, -tid)
        if best_key is None or key > best_key:
            best_key = key
            best = ref
    return best


def _free_warden_tid() -> int:
    """A fresh tid for the crowned Warden (map monsters carry tid 0)."""
    candidate = content.WARDEN_TID_BASE
    for _ in range(64):
        try:
            if bd.actor_ref(candidate) is None:
                return candidate
        except Exception:
            return candidate
        candidate += 1
    return candidate


def crown_warden(ref: Any) -> Optional[int]:
    """Empower the Warden: fresh tid if needed, empowered health (depth
    and IRON WARDEN scale it), gold tint, gold ground ring and overhead
    title. Returns the warden tid."""
    if ref is None:
        return None
    try:
        if not int(ref.tid):
            ref.tid = _free_warden_tid()
        tid = int(ref.tid)
        crowned = max(1, int(ref.health * warden_health_mult()))
        bd.apply_actor_batch([
            ("health", ref, crowned),
            ("tint", ref, *content.WARDEN_TINT),
        ])
        contract_state["warden_max_health"] = crowned
        map_title = current_map_name().upper()
        bd.draw_world_ring(ref, id=content.WARDEN_RING_ID, radius=26.0,
                           color=content.WARDEN_TINT, alpha=0.9,
                           offset_z=2.0, segments=28)
        bd.draw_world_text(ref, id=content.WARDEN_TITLE_ID,
                           text=f"Warden of {map_title}", offset_z=26.0,
                           color=content.WARDEN_TINT, height=0.036,
                           outline=True)
        bd.log(f"delve: the Warden of {map_title} is crowned "
               f"({ref.class_name}, tid {tid})")
        return tid
    except Exception as exc:
        bd.warn(f"delve: warden crowning failed: {exc!r}")
        return None


def _persist_contract() -> None:
    """Mirror the contract bookkeeping into bd.state (checkpoint-safe)."""
    try:
        bd.state[content.CONTRACT_STATE_KEY] = {
            "map": contract_state.get("map"),
            "census": contract_state.get("census", 0),
            "tribute_goal": contract_state.get("tribute_goal", 0),
            "warden_tid": contract_state.get("warden_tid"),
            "quest_id": contract_state.get("quest_id"),
            "done": bool(contract_state.get("done")),
            "modifier": contract_state.get("modifier"),
            "warden_max_health": contract_state.get("warden_max_health",
                                                    0),
        }
    except Exception as exc:
        bd.warn(f"delve: contract persistence failed: {exc!r}")


def _contract_quest() -> Optional[Any]:
    quest_id = contract_state.get("quest_id")
    if not quest_id:
        return None
    try:
        return bd_quests.log.get(quest_id)
    except Exception:
        return None


def _on_contract_complete(quest: Any) -> None:
    """Contract completion fanfare: ring burst, chime, message (no
    screen tint, same anti-flash rule as the level-up fanfare)."""
    contract_state["done"] = True
    fanfare_log.append(str(quest.id))
    try:
        bd.state[content.DEPTH_STATE_KEY] = delve_depth() + 1
    except Exception as exc:
        bd.warn(f"delve: depth persistence failed: {exc!r}")
    _persist_contract()
    pawn = player_pawn()
    ring_burst(pawn)
    try:
        bd.play_ui_sound(content.CONTRACT_SOUND, volume=0.9)
    except Exception:
        pass
    bd.center_message(f"DELVE COMPLETE: {current_map_name().upper()}")
    toasts.toast(content.TOAST_CONTRACT_DONE, kind="quest")


def _ensure_contract_quest(map_name: str, tribute_goal: int,
                           reward_xp: Optional[int] = None) -> Any:
    """Fetch or register this map's contract quest in the shared log."""
    quest_id = content.contract_quest_id(map_name)
    quest = bd_quests.log.get(quest_id)
    if quest is None:
        quest = content.build_contract_quest(
            map_name, tribute_goal,
            content.CONTRACT_XP if reward_xp is None else int(reward_xp))
        quest.on_complete = _on_contract_complete
        bd_quests.log.add(quest)
    return quest


def setup_contract() -> None:
    """Roll this map's contract (fresh map_load, scheduled 1 tic in so
    ``bd.actor_refs`` is valid: the 15_roguelike_run monsters.py idiom).

    Census the live hostiles; on a monster-free map there is no contract
    ("no quarry here" on the strip). Otherwise crown the Warden, register
    and start the per-map quest, and persist the bookkeeping. A revisit
    to an already-contracted map reuses the quest (a completed delve
    stays paid; an abandoned one resumes its progress).
    """
    global warden_visuals_reapplied
    map_name = current_map_name()
    if not map_name:
        return
    warden_visuals_reapplied = False
    monsters = _live_hostiles()
    quest_id = content.contract_quest_id(map_name)
    existing = bd_quests.log.get(quest_id)
    if existing is not None and existing.state in (
            bd_quests.Quest.ACTIVE, bd_quests.Quest.COMPLETED):
        # A revisit inside the session: resume, do not re-roll (the
        # modifier is deterministic, so re-deriving it is not re-rolling).
        contract_state.update({
            "map": map_name, "census": len(monsters),
            "tribute_goal": (existing.objective("tribute").count
                             if existing.objective("tribute") else 0),
            "warden_tid": contract_state.get("warden_tid")
            if contract_state.get("map") == map_name else None,
            "quest_id": quest_id,
            "done": existing.state == bd_quests.Quest.COMPLETED,
            "modifier": roll_modifier(map_name)})
        apply_hound_buff(fresh=True)
        _persist_contract()
        return
    if not monsters:
        contract_state.update({"map": map_name, "census": 0,
                               "tribute_goal": 0, "warden_tid": None,
                               "quest_id": None, "done": False})
        _persist_contract()
        toasts.toast(content.TOAST_NO_QUARRY, kind="info")
        return
    # One deterministic modifier per map (rolled before the goal and
    # the reward, which it shapes); the plain contract stays silent.
    modifier = roll_modifier(map_name)
    contract_state.update({"map": map_name, "census": len(monsters),
                           "tribute_goal": 0, "warden_tid": None,
                           "quest_id": None, "done": False,
                           "modifier": modifier})
    goal = tribute_goal_for_map(len(monsters))
    quest = _ensure_contract_quest(map_name, goal, contract_reward_xp())
    if quest.state == bd_quests.Quest.INACTIVE:
        quest.start()
    warden = pick_warden(monsters)
    tid = crown_warden(warden)
    apply_hound_buff(fresh=True)
    contract_state.update({"map": map_name, "census": len(monsters),
                           "tribute_goal": goal, "warden_tid": tid,
                           "quest_id": quest_id, "done": False})
    _persist_contract()
    if modifier.get("id") not in (None, "plain"):
        try:
            bd.center_message(f"{modifier['name']}: the contract shifts.")
        except Exception:
            pass
        if modifier.get("brief"):
            toasts.toast(modifier["brief"], kind="omen")
    if session_intro_done:
        toasts.toast(content.TOAST_CONTRACT.format(map=map_name.upper()),
                     kind="quest")


def reconcile_contract() -> None:
    """Checkpoint reload path (from_savegame map_load, 1 tic in).

    The engine's ``load`` event fires before ``map_load``, so this map's
    contract quest was not yet re-registered when bd_quests restored
    itself; re-register it (fresh object) and re-apply the saved quest
    states from bd.state, then reconcile the module bookkeeping from the
    persisted contract mirror. The restored world already carries the
    empowered Warden's mutated health; only the (never serialized) tint
    and the in-memory ring/title visuals are re-applied, by tid, without
    re-rolling anything.
    """
    global warden_visuals_reapplied
    warden_visuals_reapplied = False
    map_name = current_map_name()
    saved = bd.state.get(content.CONTRACT_STATE_KEY)
    if not isinstance(saved, dict) or saved.get("map") != map_name:
        return
    contract_state.update({
        "map": map_name,
        "census": int(saved.get("census") or 0),
        "tribute_goal": int(saved.get("tribute_goal") or 0),
        "warden_tid": saved.get("warden_tid"),
        "quest_id": saved.get("quest_id"),
        "done": bool(saved.get("done")),
        "modifier": saved.get("modifier"),
        "warden_max_health": int(saved.get("warden_max_health") or 0),
    })
    apply_hound_buff(fresh=False)
    quest_id = saved.get("quest_id")
    if quest_id:
        _ensure_contract_quest(map_name,
                               int(saved.get("tribute_goal") or 0),
                               contract_reward_xp())
        try:
            # The quest registry now knows this map's quest; re-run the
            # log's tolerant, idempotent restore so its state matches the
            # save.
            saved_log = bd.state.get(bd_quests.STATE_KEY)
            if isinstance(saved_log, dict):
                bd_quests.log.restore(saved_log)
        except Exception as exc:
            bd.warn(f"delve: contract quest restore failed: {exc!r}")
    tid = contract_state.get("warden_tid")
    if tid:
        try:
            ref = bd.actor_ref(int(tid))
        except Exception:
            ref = None
        if ref is not None and ref.valid and ref.alive:
            try:
                bd.apply_actor_batch([("tint", ref, *content.WARDEN_TINT)])
                map_title = map_name.upper()
                bd.draw_world_ring(ref, id=content.WARDEN_RING_ID,
                                   radius=26.0, color=content.WARDEN_TINT,
                                   alpha=0.9, offset_z=2.0, segments=28)
                bd.draw_world_text(ref, id=content.WARDEN_TITLE_ID,
                                   text=f"Warden of {map_title}",
                                   offset_z=26.0, color=content.WARDEN_TINT,
                                   height=0.036, outline=True)
                warden_visuals_reapplied = True
            except Exception as exc:
                bd.warn(f"delve: warden visual restore failed: {exc!r}")


def _on_contract_death(event: Dict[str, Any]) -> None:
    """Count tribute kills and notice the Warden's death.

    Tribute: player-credited (``attacker_player_index == 0``) hostile
    monster deaths on the contract map, any class. The Warden objective
    completes when the warden tid dies, regardless of credit, and its 5x
    XP bounty is a world event (paid however it dies, mirroring
    15_roguelike_run's unique rule): the kill tracker pays 1x on a
    player-credited kill, so the bonus is the remainder up to 5x.
    """
    quest = _contract_quest()
    if quest is None or quest.state != bd_quests.Quest.ACTIVE:
        return
    if current_map_name() != contract_state.get("map"):
        return
    snapshot = event.get("actor") or {}
    if snapshot.get("is_player") or not snapshot.get("is_monster"):
        return
    ref = event.get("actor_ref")
    tid = 0
    try:
        if ref is not None:
            tid = int(ref.tid)
    except Exception:
        tid = 0
    if not tid:
        try:
            tid = int(snapshot.get("tid") or 0)
        except (TypeError, ValueError):
            tid = 0
    try:
        credited = event.get("attacker_player_index") == 0
    except Exception:
        credited = False
    friendly = False
    try:
        if ref is not None and ref.valid:
            friendly = bool(ref.get_flag("FRIENDLY"))
    except Exception:
        friendly = False
    warden_tid = contract_state.get("warden_tid")
    if warden_tid and tid == int(warden_tid):
        class_name = str(snapshot.get("class_name") or "")
        base_xp = _xp_for_class(class_name)
        bounty = base_xp * warden_xp_mult()
        bonus = bounty - (base_xp if credited else 0)
        if bonus > 0 and hero is not None:
            try:
                hero.award_xp(bonus)
            except Exception as exc:
                bd.warn(f"delve: warden bounty failed: {exc!r}")
            xp_popup(ref, bonus, class_name, bonus=True)
        toasts.toast(content.TOAST_WARDEN_SLAIN.format(
            map=str(contract_state.get("map") or "").upper()), kind="quest")
        _complete_objective_guarded(quest, "warden")
    if not credited or friendly:
        return
    # The Warden is also one of the map's monsters: a credited kill on it
    # counts toward the tribute as well.
    _complete_objective_guarded(quest, "tribute")


def _complete_objective_guarded(quest: Any, objective_id: str,
                                amount: int = 1) -> bool:
    """bd_quests complete_objective, guarded on state and the objective."""
    try:
        if quest.state != bd_quests.Quest.ACTIVE:
            return False
        objective = quest.objective(objective_id)
        if objective is None or objective.done:
            return False
        return bool(bd_quests.log.complete_objective(quest.id,
                                                     objective_id, amount))
    except Exception as exc:
        bd.warn(f"delve: contract progress failed: {exc!r}")
        return False


def contract_strip_segment() -> str:
    """The strip's contract tail, or "" when this map has no contract."""
    quest = _contract_quest()
    if quest is None:
        if contract_state.get("map") == current_map_name():
            return "No quarry here"
        return ""
    tribute = quest.objective("tribute")
    warden = quest.objective("warden")
    parts = []
    if tribute is not None:
        parts.append(f"tribute {tribute.progress}/{tribute.count}")
    if warden is not None:
        parts.append("Warden slain" if warden.done else "Warden alive")
    if quest.state == bd_quests.Quest.COMPLETED:
        parts.append("paid")
    return "Contract: " + " - ".join(parts) if parts else ""


# --- the founding (class cards -> real CreationWizard runs) ---------------------------

#: Interactive-only: while the founding window is visible on a live map
#: the world is engine-paused so the player can read every card
#: unmolested. The autotest and screenshot drivers tick the world on
#: schedules, so main.py disables this there (a paused world would freeze
#: their steps).
creation_pause_enabled = True
_world_paused_for_creation = False
#: True once the first-map intro beat has played this session.
session_intro_done = False
#: Autotest-visible intro record.
intro_log: List[Dict[str, Any]] = []


def pause_for_creation() -> None:
    """Engine-pause the world while the founding window is open."""
    global _world_paused_for_creation
    if not creation_pause_enabled or _world_paused_for_creation:
        return
    _world_paused_for_creation = True
    try:
        bd.execute("pause")
    except Exception:
        _world_paused_for_creation = False


def unpause_for_creation() -> None:
    """Lift the creation pause (idempotent; also manual-pause safe)."""
    global _world_paused_for_creation
    if not _world_paused_for_creation:
        return
    _world_paused_for_creation = False
    try:
        bd.execute("pause")  # toggles; see discard_creation_pause
    except Exception:
        pass


def discard_creation_pause() -> None:
    """Forget the pause bookkeeping without toggling (map changes reset
    the engine pause state on their own)."""
    global _world_paused_for_creation
    _world_paused_for_creation = False


def _binding_name(command: str, fallback: str) -> str:
    """Live display name for a console command's first key binding."""
    try:
        name = bd.input_binding(command)
    except Exception:
        name = None
    return str(name) if name else fallback


def intro_beat() -> Dict[str, Any]:
    """The first-map intro: goal center line, then the goal, key, and
    hound toasts staggered over ~3 seconds (35 tics = 1 second). Later
    maps get a single contract toast from setup_contract instead."""
    global session_intro_done
    keys_line = content.INTRO_TOAST_KEYS.format(
        wind=_binding_name("+pyaction3", "C"),
        rest=_binding_name("+pyaction2", "V"),
        sheet=_binding_name("+pyaction1", "Q"))
    map_title = current_map_name().upper() or "??"

    def _toast(text: str, kind: str) -> None:
        try:
            toasts.toast(text, kind=kind)
        except Exception:
            pass

    try:
        bd.center_message(content.INTRO_CENTER.format(map=map_title),
                          bold=True)
    except Exception:
        pass
    _toast(content.INTRO_TOAST_GOAL, "quest")
    for delay, text, kind in ((35, keys_line, "info"),
                              (70, content.INTRO_TOAST_HOUND, "info")):
        try:
            bd.schedule(lambda t=text, k=kind: _toast(t, k), delay=delay,
                        map_local=True)
        except Exception as exc:
            bd.warn(f"delve: intro toast scheduling failed: {exc!r}")
    entry = {"center": content.INTRO_CENTER.format(map=map_title),
             "toasts": [content.INTRO_TOAST_GOAL, keys_line,
                        content.INTRO_TOAST_HOUND]}
    intro_log.append(entry)
    session_intro_done = True
    return entry


def _wire_knell(companion_: "bd_dnd.Companion") -> None:
    """Companion death toll: record the name, raise a harm toast, ring
    the deep bell-ish knell (knight/death, DSKNTDTH, in doom2.wad)."""

    def on_companion_died(comp: "bd_dnd.Companion",
                          member: "bd_dnd.Character") -> None:
        companion_died_log.append(member.name)
        toasts.toast(content.knell_toast(member.name), kind="harm")
        try:
            bd.play_ui_sound(content.DEATH_KNELL_SOUND, volume=0.9)
        except Exception:
            pass  # headless / no sound device

    companion_.on_companion_died.append(on_companion_died)


def found_hero(class_name: str, hero_name: str = content.HERO_DEFAULT_NAME
               ) -> "bd_dnd.Character":
    """Found the delver: the same function the class cards call.

    Builds the sheet through a driven CreationWizard, then wires every
    hero-bound system: persistence (CharacterState, PartyState), the
    party and the Guild Hound companion, the door dispatcher (bash or
    pick by class), the announced reflex save, kill XP (announce=False;
    the popups are ours), toughened blows, level-up fanfare, the bd_quests
    XP sink, and the slim strip. Idempotent: a second call returns the
    existing hero.
    """
    global hero, hero_state, party, party_state, companion
    global door_bash, reflex_save, level_damage_handler
    if hero is not None:
        return hero
    new_hero = content.make_delver(class_name, hero_name)
    hero = new_hero
    hero_state = bd_dnd.CharacterState(hero)
    hero_state.arm_persistence()

    party = bd_dnd.Party([hero, content.make_hound_member()],
                         name=content.PARTY_NAME)
    party_state = bd_dnd.PartyState(party)
    companion = bd_dnd.Companion(content.HOUND_NAME,
                                 class_name=content.COMPANION_CLASS)
    party_state.add_companion(companion)
    _wire_knell(companion)
    companion.bind(party)
    party_state.arm_persistence()

    mode, dc = door_mode_for(hero)
    door_bash = DoorBashRules(hero, dc=dc, mode=mode)
    reflex_save = ReflexSaveRule(hero, dc=content.SAVE_DC, ability="dex",
                                 damage_type=None,
                                 cooldown_tics=content.SAVE_COOLDOWN_TICS)
    bd_dnd.track_xp_from_kills(hero, announce=False, player_index=0)
    level_damage_handler = wire_level_damage(hero)
    wire_level_up(hero)
    bd_quests.log.on_xp_reward.append(_xp_sink)

    arm_strip()
    arm_health_sync()
    arm_examine_probe()
    unpause_for_creation()
    if not session_intro_done:
        intro_beat()
    bd.log(f"delve: {hero.name} the {hero.class_id} takes the contract")
    return hero


def _xp_sink(amount: int, quest: Any) -> None:
    """bd_quests on_xp_reward: contract XP lands on the hero's sheet."""
    if hero is None:
        return
    try:
        hero.award_xp(int(amount))
    except Exception as exc:
        bd.warn(f"delve: contract xp reward failed: {exc!r}")


def _cold_restore() -> None:
    """Rebuild hero/party from bd.state when none exist (a loaded game).

    The engine's ``load`` event fires before ``map_load``; in-process
    heroes are restored by their armed CharacterState/PartyState handlers,
    so this only runs for a genuinely fresh session that loaded a
    savegame.
    """
    global hero, hero_state, party, party_state, companion
    global door_bash, reflex_save, level_damage_handler
    if hero is not None:
        return
    saved = bd.state.get(bd_dnd.STATE_KEY)
    if not isinstance(saved, dict) or not isinstance(saved.get("character"),
                                                     dict):
        return
    saved_character = saved["character"]
    class_id = str(saved_character.get("class_id") or "Fighter")
    cls = content.CLASSES_BY_ID.get(class_id, content.FIGHTER)
    new_hero = bd_dnd.Character(str(saved_character.get(
        "name", content.HERO_DEFAULT_NAME)))
    hero = new_hero
    hero_state = bd_dnd.CharacterState(hero)
    hero_state.restore(saved)
    hero.cls = cls

    def _class_level_hook(changed: "bd_dnd.Character", level: int) -> None:
        bd_dnd.apply_class_level(changed, cls, level)

    hero.on_level_up.append(_class_level_hook)
    hero_state.arm_persistence()
    saved_party = bd.state.get(bd_dnd.PARTY_STATE_KEY)
    if isinstance(saved_party, dict) and saved_party.get("party"):
        party = bd_dnd.Party([hero, content.make_hound_member()],
                             name=content.PARTY_NAME)
        party_state = bd_dnd.PartyState(party)
        companion = bd_dnd.Companion(content.HOUND_NAME,
                                     class_name=content.COMPANION_CLASS)
        party_state.add_companion(companion)
        _wire_knell(companion)
        party_state.arm_persistence()
        party_state.restore(saved_party)
        companion.bind(party, spawn_near_player=False)
    mode, dc = door_mode_for(hero)
    door_bash = DoorBashRules(hero, dc=dc, mode=mode)
    reflex_save = ReflexSaveRule(hero, dc=content.SAVE_DC, ability="dex",
                                 damage_type=None,
                                 cooldown_tics=content.SAVE_COOLDOWN_TICS)
    bd_dnd.track_xp_from_kills(hero, announce=False, player_index=0)
    level_damage_handler = wire_level_damage(hero)
    wire_level_up(hero)
    bd_quests.log.on_xp_reward.append(_xp_sink)
    arm_strip()
    arm_health_sync()
    arm_examine_probe()
    bd.log(f"delve: restored {hero.name} (level {hero.level}) from the "
           f"save")


# --- the slim strip (every map) ---------------------------------------------------------

_strip_armed = False


def refresh_strip() -> Dict[str, str]:
    """Redraw the persistent progression lines; returns the text drawn.

    Line 1 is the hero (``LV n  XP x/y (need N)  Blood s/max  Body hp
    [C] <active> xN``); line 2 is the world (``Rest: ...  Contract: ...
    [MODIFIER]  Depth n``). The crosshair examine line rides its own id,
    owned by the probe task; this returns its current text for asserts.
    Segments that do not apply are omitted: no active before founding,
    no contract on a monster-free map, no bracket on a plain contract.
    """
    if hero is None:
        return {"line1": "", "line2": "", "examine": ""}
    next_xp = hero.xp_for_next_level()
    if next_xp is None:
        xp_text = f"XP {hero.xp} (MOST HIGH)"
    else:
        xp_text = f"XP {hero.xp}/{next_xp} (need {next_xp - hero.xp})"
    segments = [f"LV {hero.level}", xp_text,
                f"Blood {hero.hp}/{hero.max_hp}"]
    pawn = player_pawn()
    if pawn is not None:
        segments.append(f"Body {pawn.health}")
    spec = content.CLASS_ACTIVES.get(getattr(hero, "class_id", ""))
    if spec is not None:
        charges = hero.resources.get(spec["resource"], 0)
        segments.append(f"[C] {spec['name']} x{charges}")
    line1 = "  ".join(segments)
    world = []
    if pawn is not None:
        world.append("Rest: sanctuary"
                     if sanctuary_light_of(pawn)
                     >= sanctuary_threshold(hero)
                     else "Rest: the dark dreams")
    contract = contract_strip_segment()
    if contract:
        world.append(contract)
    modifier = contract_state.get("modifier")
    if (contract_state.get("map") == current_map_name()
            and isinstance(modifier, dict)
            and modifier.get("id") not in (None, "plain")):
        world.append(f"[{modifier['name']}]")
    world.append(f"Depth {delve_depth()}")
    line2 = "  ".join(world)
    try:
        bd.draw_text(line1, id=content.STRIP_ID, x=0.01, y=0.94,
                     height=0.022, color="tan", shadow=True, layer=8)
        if line2:
            bd.draw_text(line2, id=content.STRIP2_ID, x=0.01, y=0.965,
                         height=0.022, color="tan", shadow=True, layer=8)
        else:
            bd.draw_clear(content.STRIP2_ID)
    except Exception:
        pass  # headless / no status bar: nothing to draw on
    return {"line1": line1, "line2": line2,
            "examine": str(examine_state.get("line") or "")}


def arm_strip() -> None:
    """Arm the strip's repeating refresh (once per session)."""
    global _strip_armed
    if _strip_armed:
        return
    _strip_armed = True
    try:
        bd.schedule(lambda: refresh_strip(), delay=content.HUD_REFRESH_TICS,
                    repeat=content.HUD_REFRESH_TICS, map_local=False)
    except Exception as exc:
        bd.warn(f"delve: could not arm the strip: {exc!r}")


# --- global event wiring (called once from main's engine_start) ---------------------------


def wire_delve_events() -> None:
    """Register the hero-independent global handlers (they self-gate).

    The contract death handler and the XP popup read module state at
    event time, so they are safe to register before founding; the load
    handler cold-restores a savegame hero in a fresh session.
    """

    @bd.on("actor_died")
    def _on_death(event: Dict[str, Any]) -> None:
        _on_contract_death(event)

    @bd.on("actor_died")
    def _on_popup(event: Dict[str, Any]) -> None:
        _on_kill_xp_popup(event)

    @bd.on("actor_damaged")
    def _on_damaged(event: Dict[str, Any]) -> None:
        _on_pawn_damaged(event)

    @bd.on("sector_entered")
    def _on_sector(event: Dict[str, Any]) -> None:
        trap_sense_check(event)

    @bd.on("load")
    def _on_load(event: Dict[str, Any]) -> None:
        try:
            _reconcile_boons()
        except Exception as exc:
            bd.warn(f"delve: boon restore failed: {exc!r}")
        try:
            _cold_restore()
        except Exception as exc:
            bd.warn(f"delve: cold restore failed: {exc!r}")
