"""The Sunken Crypt — systems: rules wiring, directors, and rest logic.

Everything here is *behavior*: light-program arming, the dread-spiking
trap subclass, the sanctuary/nightmare rest rule, and the level-up and
companion-toll feedback hooks. No engine events are registered at import
time — ``main.py`` calls these functions from its own handlers, so this
module is inert when the engine executes it standalone from the PYTHON
manifest (the smoke test loads it on MAP01 with no scenario wired up).
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional

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


# --- torch and darkness -----------------------------------------------------------


def arm_crypt_lights(horror: "bd_horror.HorrorState") -> List[Any]:
    """Arm the crypt's light programs on its tagged sectors.

    Tallow candles in the entrance chamber (tag 13), grave-candles along
    the drowned passage beyond the red door (tag 7), and a fluorescent
    corpse-light in the flooded hall (tag 12, sector 47). Programs bind by
    tag, so they rebind themselves on every map transition; returns the
    created programs for assertions.
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


# --- trap dread --------------------------------------------------------------------

#: One entry per sprung trap: {"sector_index", "amount", "dread_after"}.
trap_dread_log: List[Dict[str, Any]] = []


def spike_dread(dread: "bd_horror.Dread", amount: float,
                sector_index: int = -1) -> float:
    """Add ``amount`` to the dread meter; returns the new level.

    ``bd_horror.Dread`` deliberately exposes no additive API — only
    ``set_level`` and its damage-spike wiring — so the spike is applied
    with set_level arithmetic, clamped to the 0-100 meter. Upward
    threshold crossings (25/50/75/100) fire normally, whispers included.
    """
    new_level = min(dread.MAX_LEVEL, dread.level + float(amount))
    dread.set_level(new_level)
    trap_dread_log.append({"sector_index": int(sector_index),
                           "amount": float(amount),
                           "dread_after": dread.level})
    return dread.level


class DreadTrapZone(bd_dnd.TrapZone):
    """A TrapZone whose springs spike the dread meter.

    Identical rules to ``bd_dnd.TrapZone`` (same save, same damage, same
    outcome dict); each spring additionally raises dread by
    ``dread_spike`` — the crypt notices pain.
    """

    def __init__(self, *args: Any, dread: Any = None,
                 dread_spike: float = content.TRAP_DREAD_SPIKE,
                 **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.dread: Any = dread
        self.dread_spike: float = float(dread_spike)

    def spring(self, victim: Any, sector_index: int = -1, rng: Any = bd
               ) -> Dict[str, Any]:
        outcome = super().spring(victim, sector_index=sector_index, rng=rng)
        if self.dread is not None:
            spike_dread(self.dread, self.dread_spike, sector_index)
        return outcome


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


def try_long_rest(character: "bd_dnd.Character", pawn: Any = None,
                  rng: Any = bd) -> Dict[str, Any]:
    """The crypt's rest rule: light is life.

    - **Sanctuary** (sector light >= 160): a true long rest — full heal
      and resources restored.
    - **Darkness**: sleep is a nightmare — a DEX save vs. DC 12. On a
      failure the dark eats one ``resolve`` charge (no healing) and a
      harm toast is raised; on a success the rest is *fitful* — half the
      missing HP, no resources.

    Returns an outcome dict with a ``kind`` of ``"sanctuary"``,
    ``"nightmare"``, or ``"fitful"``.
    """
    pawn = pawn if pawn is not None else player_pawn()
    light = sanctuary_light_of(pawn)
    if light >= content.SANCTUARY_LIGHT:
        outcome = character.rest(short=False, rng=rng)
        outcome["kind"] = "sanctuary"
        outcome["light"] = light
        toasts.toast(content.TOAST_SANCTUARY, kind="quest")
        return outcome
    save = character.saving_throw("dex", content.NIGHTMARE_DC, rng=rng)
    if save["success"]:
        missing = character.max_hp - character.hp
        healed = int(missing * content.FITFUL_HEAL_FRACTION)
        if healed > 0:
            character.set_hp(character.hp + healed)
        toasts.toast(content.TOAST_FITFUL, kind="info")
        return {"kind": "fitful", "save": save, "healed": healed,
                "light": light}
    resource = content.NIGHTMARE_RESOURCE
    lost = character.use_resource(resource)
    toasts.toast(content.TOAST_NIGHTMARE, kind="harm")
    return {"kind": "nightmare", "save": save, "healed": 0,
            "resource": resource if lost else None, "resource_lost": lost,
            "light": light}


# --- feedback hooks ---------------------------------------------------------------------


def wire_level_up(character: "bd_dnd.Character") -> None:
    """Level-up feedback: an ember flash, a quest toast, a chime."""

    def on_level_up(hero: "bd_dnd.Character", new_level: int) -> None:
        try:
            ember = toasts.PALETTE["ember"]
            bd.screen_flash(int(ember[0] * 255), int(ember[1] * 255),
                            int(ember[2] * 255), 0.22)
        except Exception:
            pass  # headless: no renderer to flash
        toasts.toast(content.TOAST_LEVEL_UP, kind="quest")
        bd.center_message(f"{hero.name} reaches level {new_level}!")
        try:
            bd.play_ui_sound(content.LEVEL_UP_SOUND, volume=0.8)
        except Exception:
            pass

    character.on_level_up.append(on_level_up)


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


def quiesce_horror(horror: "bd_horror.HorrorState") -> None:
    """Stop the dread tick and every light program before a checkpoint.

    The candle/fluorescent programs draw from the deterministic script RNG
    on every step, and their task phase after a savegame load is anchored
    to the load time, not to the saved stream position — so a checkpoint
    taken while they run cannot resume the exact RNG stream (probe-
    verified). The autotest calls this just before ``bd.save_checkpoint``
    and re-arms the lights after the post-load stream assertion. The
    interactive path never calls it: there the programs ride
    ``HorrorState`` persistence normally.
    """
    horror.dread.stop()
    horror.stalker.stop()
    horror.lights.clear()
