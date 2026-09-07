"""The Sunken Crypt — systems: rules wiring, the bash dispatcher, rests.

Everything here is *behavior*: the MAP02 light-program arming, the generic
locked-door bash dispatcher (any locked line, any map), level-toughened
blows through the ``actor_before_damage`` filter, the Second Wind heal,
the sanctuary/nightmare rest rule that mends the pawn as well as the
sheet, the persistent progression strip, and the level-up and
companion-toll feedback hooks. No engine events are registered at import
time — ``main.py`` calls these functions from its own handlers, so this
module is inert when the engine executes it standalone from the PYTHON
manifest (the smoke test loads it on MAP01 with no scenario wired up).
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

import biaseddoom as bd
import bd_dnd
import bd_horror
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


def is_crypt_map() -> bool:
    """True on MAP02, the map that carries the probed set pieces."""
    try:
        name = bd.current_map()
    except Exception:
        return False
    return str(name or "").upper() == content.CRYPT_MAP


# --- torch and darkness (MAP02 set piece) -------------------------------------------


def arm_crypt_lights(horror: "bd_horror.HorrorState") -> List[Any]:
    """Arm the crypt's light programs on its tagged sectors.

    Tallow candles in the entrance chamber (tag 13), grave-candles along
    the drowned passage beyond the red door (tag 7), and a fluorescent
    corpse-light in the flooded hall (tag 12, sector 47). Programs bind by
    tag, so they rebind themselves on every map transition; returns the
    created programs for assertions. Only call this on the crypt map.
    """
    return [
        horror.lights.candle(content.CANDLE_TAGS,
                             amplitude=content.CANDLE_AMPLITUDE,
                             period=content.CANDLE_PERIOD),
        horror.lights.candle(content.PASSAGE_TAGS,
                             amplitude=content.PASSAGE_AMPLITUDE,
                             period=content.PASSAGE_PERIOD),
        horror.lights.fluorescent(content.CORPSE_LIGHT_TAGS,
                                  dropout_chance=content.CORPSE_LIGHT_DROPOUT,
                                  period=content.CORPSE_LIGHT_PERIOD),
    ]


def crypt_light_report(horror: "bd_horror.HorrorState") -> Dict[str, Any]:
    """Assertion-friendly snapshot of which programs are actually bound."""
    report = {"candle": False, "fluorescent": False, "programs": []}
    for program in horror.lights.programs:
        bound = [int(sector.index) for sector in program._sectors]
        report["programs"].append({"kind": program.kind,
                                   "tags": list(program.tags),
                                   "sectors": bound,
                                   "originals": dict(program.originals)})
        if bound and program.kind in ("candle", "fluorescent"):
            report[program.kind] = True
    return report


# --- bash any locked door (every map) ----------------------------------------------


class _CardLendingDoorCheck(bd_dnd.LockedDoorCheck):
    """A LockedDoorCheck that lends every Doom key card for one activation.

    Doom locks 1-3 (card only) and 129-134 (any card or skull) all accept
    the card classes, so lending all three covers every stock Doom locked
    door without a lock-number table. The keys are reclaimed right after
    the single native ``activate`` call, exactly like the framework's
    one-key path.
    """

    def _open_door(self, activator: Any) -> bool:
        if activator is None:
            return super()._open_door(activator)
        try:
            line = bd.line(self.line_index)
        except Exception as exc:
            bd.warn(f"sunken crypt: cannot resolve line "
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
            bd.warn(f"sunken crypt: door activation failed: {exc!r}")
        finally:
            for key_class in granted:
                try:
                    activator.take_inventory(key_class, 1)
                except Exception:
                    pass
        return bool(result)


class DoorBashRules:
    """Make every locked door in the game bashable with Athletics.

    One ``line_activation_failed`` dispatcher: when the local player fails
    to open a locked line, a per-(map, line) ``_CardLendingDoorCheck`` is
    created and run. Later uses of the same line are answered by the
    check's own framework handler (registered at creation); the dispatcher
    steps aside for lines it already knows. A line whose activation is
    refused even with the lent cards (a non-Doom configuration, an exotic
    lock) is remembered in :attr:`unbashable` and never retried, so a
    foreign door never spams checks.
    """

    def __init__(self, character: "bd_dnd.Character",
                 dc: int = content.DOOR_DC) -> None:
        self.character: "bd_dnd.Character" = character
        self.dc: int = int(dc)
        self.checks: Dict[Tuple[str, int], _CardLendingDoorCheck] = {}
        self.unbashable: set = set()

        @bd.on("line_activation_failed")
        def _handler(event: Dict[str, Any]) -> None:
            self._dispatch(event)

        self._handler = _handler

    @staticmethod
    def _map_key() -> str:
        try:
            return str(bd.current_map() or "").upper()
        except Exception:
            return ""

    def bashable_door(self, line_index: int, dc: Optional[int] = None
                      ) -> Optional[_CardLendingDoorCheck]:
        """Fetch or create the bash check for one locked line (current map).

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
                int(line_index), self.character, mode="str",
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
        result = check.attempt(activator, rng=rng)
        if result.get("success") and not check.opened:
            # The roll passed but the line refused the lent cards: not a
            # Doom-style door. Never roll for it again.
            self.unbashable.add(key)
        return result


# --- level-toughened blows (every map) -----------------------------------------------


def level_damage_bonus(character: "bd_dnd.Character") -> int:
    """Bonus damage per hit at the character's current level (capped)."""
    return max(0, min(content.LEVEL_DAMAGE_BONUS_CAP, character.level - 1))


def wire_level_damage(character: "bd_dnd.Character") -> Any:
    """Add the level damage bonus to every hit the local player lands on a
    monster, through the mutable ``actor_before_damage`` filter. Returns
    the handler for tests."""

    @bd.on("actor_before_damage")
    def _on_before_damage(event: Dict[str, Any]) -> None:
        try:
            if event.get("attacker_player_index") != 0:
                return
            target = event.get("actor_ref")
            if target is None or not target.valid or not target.is_monster:
                return
            bonus = level_damage_bonus(character)
            if bonus <= 0:
                return
            event["damage"] = int(event.get("damage") or 0) + bonus
        except Exception as exc:
            bd.warn(f"sunken crypt: level damage bonus failed: {exc!r}")

    return _on_before_damage


# --- second wind (every map) ----------------------------------------------------------


def use_second_wind(character: "bd_dnd.Character", pawn: Any = None,
                    rng: Any = bd) -> Dict[str, Any]:
    """Spend the second_wind charge: heal the sheet and the pawn.

    The heal is the hit die plus level (rolled so it shows in the sheet's
    Omens log). The pawn heals through the native ``Actor.heal`` path,
    which clamps to its real maximum health on its own. Note for modders:
    for player pawns that path keys on ``player->health`` — wound or heal
    pawns through ``Actor.damage``/``Actor.heal``, never through direct
    ``pawn.health`` writes, which only touch ``mo->health`` and desync the
    mirrors (probe-verified).
    """
    pawn = pawn if pawn is not None else player_pawn()
    if not character.use_resource(content.SECOND_WIND_RESOURCE):
        toasts.toast(content.TOAST_NO_WIND, kind="info")
        return {"ok": False, "reason": "exhausted"}
    rolled = bd_dnd.roll(f"d{character.hit_die}", rng=rng)
    amount = rolled["total"] + character.level
    character._log_roll("second wind", amount,
                        detail=f"d{character.hit_die}({rolled['total']})"
                               f"+{character.level}")
    healed_sheet = 0
    before_sheet = character.hp
    character.set_hp(min(character.max_hp, character.hp + amount))
    healed_sheet = character.hp - before_sheet
    healed_pawn = 0
    if pawn is not None:
        try:
            before = pawn.health
            pawn.heal(amount)
            healed_pawn = pawn.health - before
        except Exception as exc:
            bd.warn(f"sunken crypt: second wind heal failed: {exc!r}")
    toasts.toast(content.TOAST_SECOND_WIND, kind="quest")
    return {"ok": True, "amount": amount, "healed_sheet": healed_sheet,
            "healed_pawn": healed_pawn}


# --- sanctuary rests -----------------------------------------------------------------


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


def try_long_rest(character: "bd_dnd.Character", pawn: Any = None,
                  rng: Any = bd) -> Dict[str, Any]:
    """The delve's rest rule: light is life, and rest mends the body too.

    - **Sanctuary** (sector light >= 160): a true long rest — the sheet
      heals fully, resources restore, and the pawn heals to its maximum.
    - **Darkness**: sleep is a nightmare — a DEX save vs. DC 12. On a
      failure the dark eats one ``resolve`` charge (no healing) and a
      harm toast is raised; on a success the rest is *fitful* — half the
      missing HP, on the sheet and on the pawn, and no resources.

    Returns an outcome dict with a ``kind`` of ``"sanctuary"``,
    ``"nightmare"``, or ``"fitful"``; ``pawn_healed`` reports the actual
    engine-side gain.
    """
    pawn = pawn if pawn is not None else player_pawn()
    light = sanctuary_light_of(pawn)
    if light >= content.SANCTUARY_LIGHT:
        outcome = character.rest(short=False, rng=rng)
        outcome["kind"] = "sanctuary"
        outcome["light"] = light
        outcome["pawn_healed"] = _heal_pawn(pawn, 1000)
        toasts.toast(content.TOAST_SANCTUARY, kind="quest")
        return outcome
    save = character.saving_throw("dex", content.NIGHTMARE_DC, rng=rng)
    if save["success"]:
        missing = character.max_hp - character.hp
        healed = int(missing * content.FITFUL_HEAL_FRACTION)
        if healed > 0:
            character.set_hp(character.hp + healed)
        pawn_healed = 0
        if pawn is not None:
            try:
                pawn_missing = 100 - pawn.health  # baseline; heal() clamps
                pawn_healed = _heal_pawn(
                    pawn, int(pawn_missing * content.FITFUL_HEAL_FRACTION))
            except Exception:
                pawn_healed = 0
        toasts.toast(content.TOAST_FITFUL, kind="info")
        return {"kind": "fitful", "save": save, "healed": healed,
                "pawn_healed": pawn_healed, "light": light}
    resource = content.NIGHTMARE_RESOURCE
    lost = character.use_resource(resource)
    toasts.toast(content.TOAST_NIGHTMARE, kind="harm")
    return {"kind": "nightmare", "save": save, "healed": 0,
            "pawn_healed": 0,
            "resource": resource if lost else None, "resource_lost": lost,
            "light": light}


# --- the progression strip --------------------------------------------------------------

#: Display-list id for the strip; below bd_horror's 888000 vignette base
#: and the toast/announce range at 999000+.
PROGRESS_HUD_ID = 887100


def refresh_progress_strip(character: "bd_dnd.Character",
                           pawn: Any = None) -> str:
    """Redraw the persistent progression line; returns the text drawn.

    A single ``bd.draw_text`` with a stable id: the display list replaces
    the item in place, so a slow repeating task keeps it current without
    per-frame cost. Headless runs simply record nothing visible.
    """
    pawn = pawn if pawn is not None else player_pawn()
    next_xp = character.xp_for_next_level()
    if next_xp is None:
        xp_text = f"XP {character.xp} (MOST HIGH)"
    else:
        xp_text = f"XP {character.xp}/{next_xp}"
    text = (f"{character.name}  Lv {character.level}  {xp_text}  "
            f"Blood {character.hp}/{character.max_hp}  "
            f"+{level_damage_bonus(character)} dmg")
    try:
        bd.draw_text(text, id=PROGRESS_HUD_ID, x=0.01, y=0.965,
                     height=0.022, color="tan", shadow=True, layer=8)
    except Exception:
        pass  # headless / no status bar: nothing to draw on
    return text


# --- feedback hooks ---------------------------------------------------------------------


def wire_level_up(character: "bd_dnd.Character") -> None:
    """Level-up feedback: an ember flash, a quest toast, a chime, and the
    damage-bonus note when the toughened-blows table ticks up."""

    def on_level_up(hero: "bd_dnd.Character", new_level: int) -> None:
        try:
            ember = toasts.PALETTE["ember"]
            bd.screen_flash(int(ember[0] * 255), int(ember[1] * 255),
                            int(ember[2] * 255), 0.22)
        except Exception:
            pass  # headless: no renderer to flash
        toasts.toast(content.TOAST_LEVEL_UP, kind="quest")
        bonus = level_damage_bonus(hero)
        if bonus > level_damage_bonus_at(new_level - 1):
            toasts.toast(f"Your blows land harder (+{bonus} damage).",
                         kind="quest")
        bd.center_message(f"{hero.name} reaches level {new_level}!")
        try:
            bd.play_ui_sound(content.LEVEL_UP_SOUND, volume=0.8)
        except Exception:
            pass

    character.on_level_up.append(on_level_up)


def level_damage_bonus_at(level: int) -> int:
    """The toughened-blows bonus a level grants (for the level-up diff)."""
    return max(0, min(content.LEVEL_DAMAGE_BONUS_CAP, int(level) - 1))


def wire_knell(companion: "bd_dnd.Companion", log: List[str]) -> None:
    """Companion death toll: record the name, raise a harm toast, and ring
    the deep bell-ish knell (knight/death — DSKNTDTH, verified in
    doom2.wad). ``log`` is the autotest-visible member-name list."""

    def on_companion_died(comp: "bd_dnd.Companion",
                          member: "bd_dnd.Character") -> None:
        log.append(member.name)
        bd.center_message(f"{member.name}'s companion has fallen!")
        toasts.toast(content.knell_toast(member.name), kind="harm")
        try:
            bd.play_ui_sound(content.DEATH_KNELL_SOUND, volume=0.9)
        except Exception:
            pass  # headless / no sound device

    companion.on_companion_died.append(on_companion_died)


# --- checkpoint quiescence --------------------------------------------------------------


def quiesce_lights(horror: "bd_horror.HorrorState") -> None:
    """Stop every light program before a checkpoint.

    The candle/fluorescent programs draw from the deterministic script RNG
    on every step, and their task phase after a savegame load is anchored
    to the load time, not to the saved stream position — so a checkpoint
    taken while they run cannot resume the exact RNG stream (probe-
    verified). The autotest calls this just before ``bd.save_checkpoint``
    and re-arms the lights after the post-load stream assertion. The
    interactive path never calls it: there the programs ride
    ``HorrorState`` persistence normally.
    """
    horror.lights.clear()
