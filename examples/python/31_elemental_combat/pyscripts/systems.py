"""Pyre & Rime — systems: rites wiring, directors, and rules.

Everything behavioral lives here: the custom element registry, the
affinity tables, the loot rules with their relic toasts, the element
focus and burst rites, the native damage filters, the elite afflictions
and death rattles, the Warding Idol's blackout, and the sector-index
light programs. No engine events are registered at import time —
``arm_events``/``init_rites`` are called from ``main.py``'s
``engine_start`` handler, so this module is inert when the engine
executes it standalone from the PYTHON manifest (the smoke test loads it
on MAP01 with no rite wired up).

Sector-index light programs
---------------------------

``bd_horror.LightProgram`` binds sectors by *tag* — but MAP01's arena
(the horde pen, the shrine alcove) is entirely untagged, and the engine
offers no way to add tags at runtime. :class:`SectorLightProgram`
overrides only the resolution step to bind by sector *index* instead
(probed at map_load), keeping every other behavior — flicker stepping,
original capture/restore, write-on-change — from the pack. Sector
indices are stable for a given map across a save/load, so the programs
also rebind correctly on map transitions. They are example-local and do
not ride ``HorrorState``'s descriptor persistence (which rebuilds
tag-based programs); the autotest quiesces them before its checkpoint
anyway, because RNG-drawing programs re-anchor their task phase at load
time and would desync the exact-stream assertion (probe-verified).
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional

import biaseddoom as bd
import bd_rpg
import bd_horror
from bd_horror import toasts

try:
    import pyre_content as content
except ImportError:  # standalone manifest load: self-register the sibling
    content = bd.import_script("pyscripts/content.py",
                               module_name="pyre_content")

status = bd_rpg.status

# --- mutable rite state ---------------------------------------------------------

focus = [content.ELEMENTS[0]]   # list, so closures can assign through it
combat_log: List[str] = []      # the litany: kill/loot/XP lines for the HUD
drops: List[tuple] = []         # (class_name, rarity) from on_drop
rattle_log: List[int] = []      # elite TIDs whose death rattle fired
idol_blackouts: List[int] = []  # sector indices the idol has darkened
shrine_sector_index: Optional[int] = None  # probed at map_load
horde_sector_index: Optional[int] = None   # probed at map_load
_pinned: List[Any] = []         # retained handles: actor_data dies with the
                                # last live Python handle, so elites (whose
                                # affix markers live in actor_data) are pinned

elite_loot: Optional[bd_rpg.LootTable] = None
relic_loot: Optional[bd_rpg.LootTable] = None
loot_rules: Optional[bd_rpg.LootRules] = None

#: The rite's light programs (sector-index bound; see module docstring).
lights = None  # SectorLightManager, created in init_rites()


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


def log_line(text: str) -> None:
    """Append a line to the litany (kept to the last 12)."""
    combat_log.append(str(text))
    del combat_log[:-12]


# --- sector-index light programs ----------------------------------------------------


class SectorLightProgram(bd_horror.LightProgram):
    """A LightProgram that binds by sector index, not tag.

    MAP01's play space is untagged; the indices to bind are carried in
    ``params["indices"]`` (plain JSON-able data, like every other
    program parameter). Everything else — stepping, original capture and
    restore, write-on-change, blackout completion — is inherited.
    """

    def _resolve(self) -> List[Any]:
        sectors: List[Any] = []
        for index in self.params.get("indices") or ():
            try:
                sectors.append(bd.sector(int(index)))
            except Exception as exc:
                bd.warn(f"pyre & rime: sector {index} lookup failed: "
                        f"{exc!r}")
        return sectors


class SectorLightManager(bd_horror.LightManager):
    """LightManager factories that take sector indices instead of tags."""

    def fluorescent_sectors(self, indices: Any, base: Optional[int] = None,
                            dropout_chance: float = 0.06,
                            period: int = 2) -> SectorLightProgram:
        if isinstance(indices, int):
            indices = [indices]
        return self._add(SectorLightProgram("fluorescent", [], {
            "base": None if base is None else int(base),
            "dropout_chance": max(0.0, min(1.0, float(dropout_chance))),
            "period": max(1, int(period)),
            "indices": [int(i) for i in indices],
        }))

    def blackout_sector(self, index: int,
                        duration_tics: int = 70) -> SectorLightProgram:
        """Force one sector to light 0, then restore after the duration."""
        return self._add(SectorLightProgram("blackout", [], {
            "duration_tics": max(1, int(duration_tics)),
            "indices": [int(index)],
        }))


def arm_horde_light() -> Optional[SectorLightProgram]:
    """The slow fluorescent corpse-light over the horde pen."""
    if lights is None or horde_sector_index is None:
        return None
    return lights.fluorescent_sectors(
        [horde_sector_index],
        dropout_chance=content.HORDE_LIGHT_DROPOUT,
        period=content.HORDE_LIGHT_PERIOD)


def quiesce_lights() -> None:
    """Stop and forget every light program (pre-checkpoint; see the
    module docstring for the RNG-phase rationale)."""
    if lights is not None:
        lights.clear()


# --- elements, affinities, loot ----------------------------------------------------


def init_rites() -> None:
    """Register elements, affinities, loot rules, kill XP, and aliases.

    Called once from main's ``engine_start``. LootRule registration arms
    an ``actor_died`` handler, so it must happen at engine start, never
    at import.
    """
    global elite_loot, relic_loot, loot_rules, lights

    # The three elements, registered like the pre-registered eight.
    for element in content.ELEMENTS:
        bd_rpg.damage_types.register(
            element, color=content.ELEMENT_COLORS[element])

    # Class-level affinity defaults (script-side constants, never
    # persisted): imps burn, pinkies shrug off rot entirely.
    for class_name, element, mult in content.CLASS_AFFINITIES:
        bd_rpg.set_class_affinity(class_name, element, mult)

    elite_loot = content.make_rare_loot()
    relic_loot = content.make_relic_loot()
    loot_rules = bd_rpg.LootRules()
    # Legendary first: the Rime-Bound's relic outranks the common spoils
    # (rules are checked in registration order; the first match wins).
    loot_rules.register(rime_bound_predicate, relic_loot,
                        rarity="legendary")
    loot_rules.register(elite_predicate, elite_loot, rarity="rare")
    loot_rules.on_drop.append(on_drop)

    # Kill XP: a class->XP table for awards plus a listener for the log.
    bd_rpg.track_kill_xp(content.KILL_XP_TABLE)
    bd_rpg.track_kill_xp(lambda amount, idx: log_line(f"+{amount} XP"))

    # Console aliases + key binds through the pyui/ui_command bridge.
    bd.execute('alias cycle_element "pyui cycle_element"')
    bd.execute("bind f cycle_element")
    bd.execute('alias elemental_burst "pyui elemental_burst"')
    bd.execute("bind g elemental_burst")

    lights = SectorLightManager()


def elite_predicate(event: Dict[str, Any]) -> bool:
    """True for deaths of the elite roster (Cinder Thralls, Rime-Bound)."""
    snapshot = event.get("actor") or {}
    return (snapshot.get("class_name") == content.ELITE_CLASS
            and snapshot.get("tid") in content.ELITE_TIDS)


def rime_bound_predicate(event: Dict[str, Any]) -> bool:
    """True for the death of the Rime-Bound, bearer of the relic."""
    snapshot = event.get("actor") or {}
    return (snapshot.get("class_name") == content.ELITE_CLASS
            and content.ELITE_BY_TID.get(snapshot.get("tid"), {})
            .get("affix") == "rime")


def on_drop(event: Dict[str, Any], entry: Dict[str, Any], rarity: str,
            spawned_ref: Any) -> None:
    """Drop feedback: record it, sing it in the litany, toast the relics."""
    class_name = entry["class_name"]
    drops.append((class_name, rarity))
    display = content.display_name(class_name)
    if rarity == "legendary":
        log_line(f"A relic surfaces: {display}")
        toasts.toast(content.toast_relic(display), kind="omen")
    else:
        log_line(f"Spoils [{rarity}]: {display}")
        toasts.toast(f"Spoils: {display}", kind="loot")


# --- the focus and burst rites ---------------------------------------------------


def cycle_focus() -> None:
    """F: turn the elemental focus one sigil widdershins."""
    index = (content.ELEMENTS.index(focus[0]) + 1) % len(content.ELEMENTS)
    focus[0] = content.ELEMENTS[index]
    entry = bd_rpg.damage_types.get(focus[0]) or {}
    r, g, b = entry.get("color", (255, 255, 255))
    bd.hud_text(f"Focus: {focus[0].upper()}", id=731, y=0.18,
                color="gold", hold=1.5)
    bd.screen_flash(r, g, b, 0.15)


def nearest_living_monster(pawn: Any) -> Any:
    """Nearest living horde monster to the pawn, or None."""
    best = None
    best_dist = None
    for class_name in ("DoomImp", "Demon"):
        try:
            refs = bd.actor_refs(class_name)
        except Exception:
            continue
        for ref in refs:
            try:
                if not ref.alive:
                    continue
                dist = ref.distance_to(pawn)
            except Exception:
                continue
            if best is None or dist < best_dist:
                best, best_dist = ref, dist
    return best


def elemental_burst() -> None:
    """G: hurl the focused element at the nearest living horror."""
    pawn = player_pawn()
    target = nearest_living_monster(pawn) if pawn is not None else None
    if pawn is None or target is None:
        return
    result = bd_rpg.resolve_attack(
        pawn, target,
        {"damage": "2d6+3", "type": focus[0], "crit_chance": 0.1,
         "crit_mult": 2.0})
    if result["final"] > 0:
        line = f"Burst {result['type']}: {result['final']} dmg"
        if result["critical"]:
            line += " CRIT"
        if result["killed"]:
            line += " (slain)"
    else:
        line = f"Burst {result['type']}: immune!" if result["hit"] else \
            f"Burst {result['type']}: miss"
    log_line(line)


# --- the Warding Idol ------------------------------------------------------------


def set_shrine_sector(index: Optional[int]) -> None:
    """Remember which sector the Warding Idol stands in (map_load)."""
    global shrine_sector_index
    shrine_sector_index = None if index is None else int(index)


def on_idol_pickup(pawn: Any) -> None:
    """The idol's blessing and its price: stoneskin for the bearer, a
    blackout for the shrine's sector, and the omen toast."""
    status.apply(pawn, "stoneskin", content.IDOL_WARD_TICS)
    bd.center_message("WARDING IDOL — your skin turns to granite")
    log_line("The idol wards you: damage negated for 10s")
    if lights is not None and shrine_sector_index is not None:
        lights.blackout_sector(shrine_sector_index,
                               duration_tics=content.IDOL_BLACKOUT_TICS)
        idol_blackouts.append(shrine_sector_index)
    toasts.toast(content.TOAST_IDOL, kind="omen")


# --- elite afflictions and death rattles --------------------------------------------


def elite_touch(event: Dict[str, Any]) -> None:
    """Elite blows carry their affix: Thralls ignite, the Rime-Bound chills."""
    ref = event.get("actor_ref")
    source = event.get("source_ref")
    if ref is None or source is None:
        return
    try:
        if not ref.is_player:
            return
        pack = bd.actor_data(source).get("bd_rpg") or {}
    except Exception:
        return
    affix = pack.get("elite")
    if affix == "pyre":
        status.apply(ref, "burning", content.BURNING_TICS)
        log_line("A Cinder Thrall sets you ablaze!")
    elif affix == "rime":
        status.apply(ref, "slowed", content.CHILLED_TICS)
        log_line("The Rime-Bound's grip chills your marrow")


def elite_death_rattle(event: Dict[str, Any]) -> None:
    """Elite deaths rattle: a wet stock sound and a brief bruise fade."""
    snapshot = event.get("actor") or {}
    tid = snapshot.get("tid")
    if (snapshot.get("class_name") != content.ELITE_CLASS
            or tid not in content.ELITE_TIDS):
        return
    rattle_log.append(int(tid))
    name = content.ELITE_BY_TID.get(tid, {}).get("name", "An elite")
    log_line(f"{name} breathes its last rattle")
    try:
        bd.play_ui_sound(content.RATTLE_SOUND, volume=0.7)
    except Exception:
        pass  # headless / no sound device
    r, g, b, alpha, seconds = content.RATTLE_FADE
    try:
        bd.screen_fade(r, g, b, alpha, seconds=seconds)
    except Exception:
        pass  # headless: no renderer to fade


# --- native damage-filter integration ---------------------------------------------


def elemental_focus_filter(event: Dict[str, Any]) -> None:
    """Retype + affinity-scale the player's native weapon hits.

    Hits that already carry a registered RPG damage type (resolver output)
    are left alone — the resolver computes, this filter only translates
    vanilla hits into elemental ones.
    """
    if event.get("cancel"):
        return
    if event.get("attacker_player_index") != 0:
        return
    target = event.get("actor_ref")
    if target is None:
        return
    try:
        if target.is_player:
            return
    except Exception:
        return
    dtype = str(event.get("damage_type") or "").lower()
    if bd_rpg.damage_types.get(dtype) is not None:
        return  # already elemental (e.g. resolve_attack output)
    event["damage_type"] = focus[0]
    try:
        mult = bd_rpg.affinity_of(target, focus[0])
    except Exception:
        mult = 1.0
    if mult != 1.0:
        try:
            event["damage"] = int(int(event.get("damage", 0)) * mult)
        except (TypeError, ValueError):
            pass


def stoneskin_filter(event: Dict[str, Any]) -> None:
    """Stoneskin: while the ward runs, the bearer is unbreakable stone."""
    if event.get("cancel"):
        return
    ref = event.get("actor_ref")
    if ref is None:
        return
    try:
        if not ref.is_player:
            return
    except Exception:
        return
    if status.has(ref, "stoneskin"):
        event["cancel"] = True
        log_line("The idol's ward absorbs the blow")


def on_item_picked(event: Dict[str, Any]) -> None:
    """The Warding Idol grants its ward when picked up.

    Module-level (not a closure) so the autotest can drive the real
    pickup wiring with a synthetic ``item_picked`` event.
    """
    if str(event.get("class_name") or "").lower() != "soulsphere":
        return
    pawn = player_pawn()
    if pawn is None:
        return
    on_idol_pickup(pawn)


# --- event registration ---------------------------------------------------------


def arm_events() -> None:
    """Register every event handler. Called once from engine_start."""

    @bd.on("ui_command")
    def _on_ui_command(event: Dict[str, Any]) -> None:
        command = event.get("command")
        try:
            if command == "cycle_element":
                cycle_focus()
            elif command == "elemental_burst":
                elemental_burst()
        except Exception as exc:
            bd.warn(f"pyre & rime: ui_command {command!r} failed: {exc!r}")

    @bd.on("actor_before_damage")
    def _on_before_damage_focus(event: Dict[str, Any]) -> None:
        elemental_focus_filter(event)

    @bd.on("actor_before_damage")
    def _on_before_damage_stoneskin(event: Dict[str, Any]) -> None:
        stoneskin_filter(event)

    @bd.on("actor_damaged")
    def _on_actor_damaged_elite(event: Dict[str, Any]) -> None:
        elite_touch(event)

    @bd.on("actor_died")
    def _on_actor_died_rattle(event: Dict[str, Any]) -> None:
        elite_death_rattle(event)

    @bd.on("item_picked")
    def _on_item_picked(event: Dict[str, Any]) -> None:
        on_item_picked(event)
