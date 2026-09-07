"""Pyre & Rime — elemental combat rites on Doom II MAP01.

A combat-rites mini-scenario built on the engine-shipped ``bd_rpg``
rules framework and dressed in the ``bd_horror`` presentation pack
(theme / toasts / atmosphere). You carry an elemental focus — Pyre,
Rime, or Rot — cycled with F; your native weapon hits are retyped and
affinity-scaled through the ``actor_before_damage`` mutable filter, and
G hurls an elemental burst at the nearest horror. The horde ahead burns
eagerly (imps are weak to Pyre, x2) and the pinkies are rot-proof
(immune to Rot, x0 — immunity beats soak). Two orange Cinder Thralls
ignite you when their blows connect; the pale Rime-Bound chills you —
and carries a legendary relic. Behind you, the Warding Idol (a
Soulsphere prop) grants a stoneskin ward whose effect is a scripted
``actor_before_damage`` cancel — and drinks the light from its alcove
(a sector blackout) as its price. A slow fluorescent corpse-light
flickers over the horde pen. The ImGui HUD shows the three sigils, the
bearer's afflictions, the souls bar, and the kill/loot litany.

Module map (all four are listed in the PYTHON manifest):

- ``content.py`` — pure data and factories: elements, elites, loot
  tables, relic names, prose. No engine calls at import.
- ``systems.py`` — behavior: element registry, affinity tables, loot
  rules, the focus/burst rites, damage filters, elite afflictions and
  death rattles, the Warding Idol, sector-index light programs.
- ``ui.py`` — the bd_horror-themed ImGui combat HUD. Inert headless.
- ``main.py`` (this file) — bootstrap, world setup, and the autotest
  driver.

Autotest: ``BD_EXAMPLE_AUTOTEST=1`` drives the rules headlessly and
deterministically — resolver branches with scripted rng doubles
(hit/miss/crit/dice), affinity exactness (Pyre weakness doubles the
final), immunity beating soak, the soak min-1 rule, status
refresh/stack/independent semantics with tick counts, the idol's
stoneskin ward cancelling damage *and* drinking the light (blackout
active on the shrine sector, omen toast in the history ring),
elite-applied burning and rime-chill (plus the negative case), loot
drops with exact killer attribution, elite death rattles, then a
checkpoint round-trip asserting the loot RNG stream rewinds exactly, a
TID-tracked status timer survives, the player's affinity is re-applied,
the elite marker is re-applied, and the Rime-Bound's legendary relic
drops with its omen toast. Because the fluorescent horde-pen light draws
from the script RNG on every step and its post-load task phase cannot
reproduce the saved stream position (probe-verified), the autotest
quiesces it just before the save. The run ends via ``-scripttest``'s
own PASS/FAIL accounting (no explicit quit).

``BD_EXAMPLE_SCREENSHOT=1`` schedules a screenshot a few seconds in, for
documentation captures.
"""

import math
import os
import sys

import biaseddoom as bd
import bd_rpg


def _load_sibling(path, module_name):
    """Import a sibling module once, through sys.modules.

    Mirrors how hello_world imports pyscripts/helper.py, but idempotent:
    the engine also executes every PYTHON manifest line as a script, so a
    sibling may already be registered under its module name — importing
    it again would re-execute the module and double-register callbacks.
    """
    module = sys.modules.get(module_name)
    if module is None:
        module = bd.import_script(path, module_name=module_name)
    return module


content = _load_sibling("pyscripts/content.py", "pyre_content")
systems = _load_sibling("pyscripts/systems.py", "pyre_systems")
ui = _load_sibling("pyscripts/ui.py", "pyre_ui")

AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"
SCREENSHOT = os.environ.get("BD_EXAMPLE_SCREENSHOT") == "1"

status = systems.status
state = bd_rpg.RpgState()

player_pawn = systems.player_pawn

ui_ctx = {"panel_visible": [True], "panel_drawn_ok": False}
loot_sequence = None            # rolls drawn after the checkpoint save
tick_counter = []               # custom status tick hits


class _Roller:
    """Scripted rng test double: yields clamped values in order."""

    def __init__(self, ints, floats=()):
        self._ints = list(ints)
        self._floats = list(floats) or [1.0]
        self._i = 0
        self._f = 0

    def randint(self, lo, hi):
        if not self._ints:
            return lo
        value = self._ints[self._i % len(self._ints)]
        self._i += 1
        return max(lo, min(hi, value))

    def random(self):
        value = self._floats[self._f % len(self._floats)]
        self._f += 1
        return max(0.0, min(0.999999, value))


# --- setup ----------------------------------------------------------------------


@bd.on("engine_start")
def setup(event):
    bd.imgui.set_master_visible(True)
    bd.imgui.set_nav_enabled(True)
    bd.log(f"bd_rpg shipped from: {bd_rpg.__file__}")

    systems.init_rites()     # elements, affinities, loot, XP, aliases
    systems.arm_events()     # filters, elite touch/rattle, idol, ui_command
    state.arm_persistence()
    if not AUTOTEST:
        from bd_horror import toasts
        toasts.toast(content.TOAST_INTRO, kind="omen")


# --- world setup ------------------------------------------------------------------


def _spawn(class_name, x, y, z, angle, tid, tint=None, harmless=False):
    ref = bd.spawn(class_name, x, y, z, angle=angle, tid=tid, force=True)
    try:
        if tint is not None:
            ref.tint = tint
        if harmless:
            ref.damage_multiply = 0.0
    except Exception:
        pass
    return ref


def _bind_elite(ref, elite):
    """Tint an elite and pin its affix marker in actor_data.

    The handle is pinned — actor_data is purged with the last live
    handle, and the affix marker must survive the run.
    """
    try:
        ref.tint = elite["tint"]
        bd.actor_data(ref).setdefault("bd_rpg", {})["elite"] = \
            elite["affix"]
        systems._pinned.append(ref)
    except Exception:
        pass


@bd.on("map_load")
def on_map(event):
    if event.get("from_savegame"):
        # The checkpoint restored the world; actor_data payloads (elite
        # markers) and tints are not serialized, so re-apply them here —
        # the standard post-load rebind recipe — and pin the fresh
        # handles so the engine does not purge the markers with a
        # handle GC.
        for elite in content.ELITES:
            ref = bd.actor_ref(elite["tid"])
            if ref is not None:
                _bind_elite(ref, elite)
        if AUTOTEST:
            bd.schedule(autotest_post_load, delay=10)
            bd.schedule(autotest_post_load_rites, delay=20)
        return

    pawn = player_pawn()
    if pawn is None:
        return
    angle = math.radians(pawn.angle)
    fx, fy = math.cos(angle), math.sin(angle)
    sx, sy = -fy, fx
    face = (pawn.angle + 180.0) % 360.0

    # The horde: pyre-weak imps in front, rot-proof pinkies flanking.
    for i, tid in enumerate(content.HORDE_IMP_TIDS):
        side = (i - 1.5) * 72.0
        _spawn("DoomImp", pawn.x + fx * 320.0 + sx * side,
               pawn.y + fy * 320.0 + sy * side, pawn.z, face, tid,
               harmless=AUTOTEST)
    for i, tid in enumerate(content.PINKY_TIDS):
        side = (i - 0.5) * 160.0
        _spawn("Demon", pawn.x + fx * 224.0 + sx * side,
               pawn.y + fy * 224.0 + sy * side, pawn.z, face, tid,
               harmless=AUTOTEST)
    # The elite roster: two Cinder Thralls and the pale Rime-Bound.
    for i, elite in enumerate(content.ELITES):
        side = (i - 1.0) * 96.0
        ref = _spawn(content.ELITE_CLASS, pawn.x + fx * 448.0 + sx * side,
                     pawn.y + fy * 448.0 + sy * side, pawn.z, face,
                     elite["tid"], harmless=AUTOTEST)
        _bind_elite(ref, elite)
    # TID-tagged imp for the status persistence recipe.
    _spawn("DoomImp", pawn.x + fx * 320.0 + sx * 220.0,
           pawn.y + fy * 320.0 + sy * 220.0, pawn.z, face,
           content.STATUS_IMP_TID, harmless=AUTOTEST)

    # The Warding Idol: a Soulsphere prop behind the player. Its pickup
    # (item_picked) grants the stoneskin ward — and drinks the light.
    _spawn(content.IDOL_CLASS, pawn.x - fx * 128.0, pawn.y - fy * 128.0,
           pawn.z, face, 0)
    # Probe the shrine alcove and the horde pen (MAP01's arena sectors
    # are untagged, so the light programs bind by sector index).
    shrine = bd.sector_at(pawn.x - fx * 128.0, pawn.y - fy * 128.0)
    systems.set_shrine_sector(
        int(shrine.index) if shrine is not None else None)
    horde = bd.sector_at(pawn.x + fx * 320.0, pawn.y + fy * 320.0)
    systems.horde_sector_index = \
        int(horde.index) if horde is not None else None
    systems.arm_horde_light()

    if AUTOTEST:
        # Resolver dummies behind the player, harmless for determinism.
        for i, tid in enumerate(content.DUMMY_TIDS):
            class_name = "Demon" if tid == 9455 else "DoomImp"
            _spawn(class_name, pawn.x - fx * 300.0 + sx * (i - 2.5) * 64.0,
                   pawn.y - fy * 300.0 + sy * (i - 2.5) * 64.0, pawn.z,
                   face, tid, harmless=True)
        _spawn("DoomImp", pawn.x - fx * 420.0, pawn.y - fy * 420.0, pawn.z,
               face, content.STATUS_DUMMY_TID, harmless=True)
        bd.schedule(autotest_resolver_math, delay=10)
        bd.schedule(autotest_affinities, delay=20)
        bd.schedule(autotest_status_begin, delay=30)
        bd.schedule(autotest_horde_light, delay=35)
        bd.schedule(autotest_status_refresh, delay=50)
        bd.schedule(autotest_status_results, delay=70)
        bd.schedule(autotest_stoneskin, delay=80)
        bd.schedule(autotest_idol_blackout, delay=85)
        bd.schedule(autotest_elite_burning, delay=90)
        bd.schedule(autotest_loot_drop, delay=100)
        bd.schedule(autotest_loot_assert, delay=110)
        bd.schedule(autotest_pre_save, delay=120)
        bd.schedule(autotest_save, delay=130)
        bd.schedule(autotest_rng_draws, delay=145)
        bd.schedule(autotest_load, delay=160)
    if SCREENSHOT:
        bd.schedule(screenshot_flavor, delay=bd.TICRATE)
        bd.schedule(lambda: bd.execute("screenshot /tmp/pyre_rime"),
                    delay=2 * bd.TICRATE)
        bd.schedule(lambda: bd.execute("quit"), delay=5 * bd.TICRATE)


# --- autotest steps ----------------------------------------------------------------


def autotest_resolver_math():
    # Hit branch: d20 roll 15 + accuracy 5 = 20 vs 10 + defense 2 = 12.
    target = bd.actor_ref(content.DUMMY_TIDS[0])
    before = target.health
    result = bd_rpg.resolve_attack(
        player_pawn(), target,
        {"damage": 10, "type": "pyre", "accuracy": 5, "defense": 2,
         "crit_chance": 0.0},
        rng=_Roller([15]))
    bd.assert_true(result["hit"] and not result["critical"],
                   "resolver: d20 hit branch hits")
    bd.assert_true(result["raw"] == 10 and result["multiplier"] == 2.0,
                   "resolver: raw roll and pyre-weak multiplier reported")
    bd.assert_true(result["final"] == 20 and target.health == before - 20,
                   "resolver: pyre weakness doubles the final damage")

    # Miss branch: roll 2 + 5 = 7 < 12 -> no damage at all.
    target = bd.actor_ref(content.DUMMY_TIDS[1])
    before = target.health
    result = bd_rpg.resolve_attack(
        player_pawn(), target,
        {"damage": 10, "type": "pyre", "accuracy": 5, "defense": 2},
        rng=_Roller([2]))
    bd.assert_true(not result["hit"] and result["final"] == 0
                   and target.health == before,
                   "resolver: miss branch deals no damage")

    # Crit branch: scripted float under crit_chance doubles the raw roll.
    target = bd.actor_ref(content.DUMMY_TIDS[2])
    before = target.health
    result = bd_rpg.resolve_attack(
        player_pawn(), target,
        {"damage": 10, "type": "physical", "crit_chance": 0.5,
         "crit_mult": 2.0},
        rng=_Roller([], floats=[0.1]))
    bd.assert_true(result["hit"] and result["critical"]
                   and result["raw"] == 20 and result["final"] == 20
                   and target.health == before - 20,
                   "resolver: crit branch multiplies the raw roll")

    # Dice notation: 2d6+3 with scripted dice 4 and 2 -> raw 9.
    target = bd.actor_ref(content.DUMMY_TIDS[3])
    before = target.health
    result = bd_rpg.resolve_attack(
        player_pawn(), target, {"damage": "2d6+3", "type": "physical"},
        rng=_Roller([4, 2]))
    bd.assert_true(result["raw"] == 9 and result["final"] == 9
                   and target.health == before - 9,
                   "resolver: NdM+K dice notation rolls through rng")

    # Soak: flat 5 soak absorbs min(5, dmg-1) — at least 1 gets through.
    target = bd.actor_ref(content.DUMMY_TIDS[4])
    bd_rpg.set_soak(target, 5)
    before = target.health
    result = bd_rpg.resolve_attack(
        player_pawn(), target, {"damage": 10, "type": "physical"},
        rng=_Roller([]))
    bd.assert_true(result["soak"] == 5 and result["final"] == 5
                   and target.health == before - 5,
                   "resolver: soak absorbs min(soak, dmg-1)")

    # Immunity: multiplier 0 zeroes the hit BEFORE soak is consulted.
    target = bd.actor_ref(content.DUMMY_TIDS[5])  # a pinky: rot-immune
    bd_rpg.set_soak(target, 5)
    before = target.health
    result = bd_rpg.resolve_attack(
        player_pawn(), target, {"damage": 10, "type": "rot"},
        rng=_Roller([]))
    bd.assert_true(result["hit"] and result["multiplier"] == 0.0
                   and result["soak"] == 0 and result["final"] == 0
                   and target.health == before,
                   "resolver: immune defender takes 0 before soak")


def autotest_affinities():
    imp = bd.actor_ref(content.DUMMY_TIDS[0])
    pinky = bd.actor_ref(content.DUMMY_TIDS[5])
    bd.assert_true(bd_rpg.affinity_of(imp, "pyre") == 2.0,
                   "affinity: class weakness applies to live actors")
    bd.assert_true(bd_rpg.affinity_of(pinky, "rot") == 0.0,
                   "affinity: class immunity applies to live actors")
    bd.assert_true(bd_rpg.affinity_of(imp, "rime") == 1.0,
                   "affinity: unlisted types default to neutral")
    bd_rpg.set_affinity(imp, "pyre", 0.5)
    bd.assert_true(bd_rpg.affinity_of(imp, "pyre") == 0.5,
                   "affinity: per-actor entry overrides the class default")
    entry = bd_rpg.damage_types.get("rot")
    bd.assert_true(entry is not None
                   and entry["color"] == content.ELEMENT_COLORS["rot"],
                   "damage type registry: custom rot type registered")
    bd.assert_true(len(bd_rpg.damage_types.all()) == 8 + 3,
                   "damage type registry: 8 builtins + pyre/rime/rot")


def autotest_status_begin():
    ref = bd.actor_ref(content.STATUS_DUMMY_TID)
    bd.assert_true(ref is not None and ref.valid,
                   "status: test dummy available")
    if ref is None:
        return
    # refresh: applied now, re-applied at t+20 — the timer resets.
    status.apply(ref, "freshen", 100)
    # stack: two applications combine into one instance with 3 stacks.
    status.apply(ref, "stacky", 200, stacks=1, rule="stack")
    status.apply(ref, "stacky", 200, stacks=2, rule="stack")
    # independent: two applications run as two instances.
    status.apply(ref, "indy", 300, rule="independent")
    status.apply(ref, "indy", 300, rule="independent")
    # custom ticking effect: interval 10, duration 35 -> exactly 3 ticks.
    status.apply(ref, "ticker", 35, interval=10,
                 tick=lambda r, inst: tick_counter.append(1))
    bd.assert_true(status.has(ref, "freshen")
                   and status.stacks(ref, "stacky") == 3
                   and status.instances(ref, "stacky") == 1
                   and status.instances(ref, "indy") == 2,
                   "status: refresh/stack/independent applied")


def autotest_horde_light():
    """The slow fluorescent corpse-light is bound over the horde pen."""
    bd.assert_true(systems.horde_sector_index is not None,
                   "horde pen sector probed at map_load")
    programs = list(systems.lights.programs) if systems.lights else []
    bound = [p for p in programs if p.kind == "fluorescent" and p._sectors]
    bd.assert_true(len(bound) == 1
                   and bound[0].params.get("indices")
                   == [systems.horde_sector_index],
                   "fluorescent program active on the horde pen sector")
    if bound and bound[0].originals:
        index = systems.horde_sector_index
        bd.assert_true(bound[0].originals.get(index) == 160,
                       "horde pen original light captured (160)")


def autotest_status_refresh():
    ref = bd.actor_ref(content.STATUS_DUMMY_TID)
    if ref is not None and ref.valid:
        status.apply(ref, "freshen", 100)  # refresh: timer resets to 100


def autotest_status_results():
    ref = bd.actor_ref(content.STATUS_DUMMY_TID)
    bd.assert_true(ref is not None and ref.valid, "status: dummy still live")
    if ref is None:
        return
    remaining = status.remaining(ref, "freshen")
    bd.assert_true(60 <= remaining <= 90,
                   f"status: refresh reset the timer (remaining "
                   f"{remaining}, would be ~60 without the refresh)")
    bd.assert_true(status.stacks(ref, "stacky") == 3,
                   "status: stacked effect kept its stacks")
    bd.assert_true(status.instances(ref, "indy") == 2,
                   "status: independent instances run side by side")
    bd.assert_true(len(tick_counter) == 3,
                   f"status: custom tick fired 3 times "
                   f"({len(tick_counter)} actual)")
    bd.assert_true(not status.has(ref, "ticker"),
                   "status: expired effect removed itself")
    summary = status.list(ref)
    bd.assert_true(summary.get("indy", {}).get("duration") == 300,
                   "status: list() reports durations for UI progress")


def _scripted_pawn_damage(amount, source=None):
    pawn = player_pawn()
    return pawn.damage(amount, source=source)


def autotest_stoneskin():
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "stoneskin: pawn available")
    if pawn is None:
        return
    # Drive the real pickup wiring with a synthetic item_picked event.
    systems.on_item_picked({"class_name": "Soulsphere"})
    bd.assert_true(status.has(pawn, "stoneskin"),
                   "stoneskin: the idol pickup granted the ward")
    health = pawn.health
    _scripted_pawn_damage(10)
    bd.assert_true(pawn.health == health,
                   "stoneskin: actor_before_damage cancel negates the hit")
    status.dispel(pawn, "stoneskin")
    _scripted_pawn_damage(5)
    bd.assert_true(pawn.health == health - 5,
                   "stoneskin: damage lands again after dispel")
    pawn.heal(5)


def autotest_idol_blackout():
    """The idol's price: its pickup drank the light from the shrine's
    sector (a blackout program) and raised the omen toast."""
    from bd_horror import toasts
    index = systems.shrine_sector_index
    bd.assert_true(index is not None, "shrine sector probed at map_load")
    if index is None:
        return
    bd.assert_true(systems.idol_blackouts == [index],
                   "idol blackout triggered on the shrine's sector")
    bd.assert_true(int(bd.sector(index).light) == 0,
                   "the idol drank the light (sector light 0)")
    programs = list(systems.lights.programs) if systems.lights else []
    blackouts = [p for p in programs if p.kind == "blackout"]
    bd.assert_true(len(blackouts) == 1
                   and blackouts[0].params.get("indices") == [index],
                   "blackout program active on the shrine sector")
    bd.assert_true(any(t["kind"] == "omen"
                       and t["text"] == content.TOAST_IDOL
                       for t in toasts.history),
                   "idol pickup raised the 'drinks the light' omen toast")


def autotest_elite_burning():
    pawn = player_pawn()
    plain = bd.actor_ref(content.HORDE_IMP_TIDS[0])
    thrall = bd.actor_ref(content.ELITE_TIDS[0])
    rime_bound = bd.actor_ref(content.ELITE_TIDS[2])
    bd.assert_true(pawn is not None and plain is not None
                   and thrall is not None and rime_bound is not None,
                   "elite touch: pawn and attackers available")
    if pawn is None or plain is None or thrall is None \
            or rime_bound is None:
        return
    # Damage sourced to a monster is scaled by the source's
    # damage_multiply, and the autotest horde is harmless (multiply 0) —
    # un-mute the scripted attackers for the duration of the hits so the
    # resultant damage (and its actor_damaged event) is real.
    plain.damage_multiply = 1.0
    thrall.damage_multiply = 1.0
    rime_bound.damage_multiply = 1.0
    try:
        _scripted_pawn_damage(1, source=plain)
        bd.assert_true(not status.has(pawn, "burning"),
                       "elite touch: a plain imp hit does not ignite")
        _scripted_pawn_damage(1, source=thrall)
        bd.assert_true(status.has(pawn, "burning"),
                       "elite touch: a Cinder Thrall hit ignites the "
                       "player")
        _scripted_pawn_damage(1, source=rime_bound)
        bd.assert_true(status.has(pawn, "slowed"),
                       "elite touch: the Rime-Bound's grip chills")
    finally:
        plain.damage_multiply = 0.0
        thrall.damage_multiply = 0.0
        rime_bound.damage_multiply = 0.0
    status.dispel(pawn, "burning")
    status.dispel(pawn, "slowed")
    pawn.heal(3)


def autotest_loot_drop():
    pawn = player_pawn()
    thrall = bd.actor_ref(content.ELITE_TIDS[0])
    bd.assert_true(pawn is not None and thrall is not None
                   and thrall.valid,
                   "loot: elite available")
    if pawn is None or thrall is None:
        return
    thrall.damage(1000, source=pawn)


def autotest_loot_assert():
    bd.assert_true(len(systems.drops) == 1,
                   "loot: the elite death dropped exactly one item")
    if systems.drops:
        class_name, rarity = systems.drops[0]
        bd.assert_true(rarity == "rare",
                       "loot: drop carried the rare rarity tier")
        bd.assert_true(class_name in content.DISPLAY_NAMES,
                       f"loot: dropped class {class_name} is in the table")
    bd.assert_true(bd_rpg.kill_xp(0) == 20,
                   "kill XP: the elite kill credited its tabled XP")
    bd.assert_true(systems.rattle_log == [content.ELITE_TIDS[0]],
                   "the Cinder Thrall's death rattle fired")
    # Environmental/source-less kills must NOT drop: kill the second
    # thrall with no source and confirm no second drop (its rattle still
    # sounds — the rattle is about the death, not the credit).
    thrall = bd.actor_ref(content.ELITE_TIDS[1])
    if thrall is not None and thrall.valid:
        thrall.damage(1000)  # no source -> attacker_player_index is None
    bd.assert_true(len(systems.drops) == 1,
                   "loot: source-less kills drop nothing "
                   "(exact attribution)")
    bd.assert_true(systems.rattle_log == list(content.ELITE_TIDS[:2]),
                   "the second thrall's death rattle fired")


def autotest_pre_save():
    pawn = player_pawn()
    target = bd.actor_ref(content.STATUS_IMP_TID)
    if pawn is not None:
        bd_rpg.set_affinity(pawn, "pyre", 0.5)  # persisted by RpgState
        pawn.heal(100)
    if target is not None and target.valid:
        # Long poison on a TID-tagged actor: its timer rides the checkpoint.
        status.apply(target, "poisoned", 700)
    # Quiesce the RNG-drawing light programs (the fluorescent horde light
    # and any in-flight idol blackout) before the checkpoint: their task
    # phase re-anchors at load time and would desync the exact script-RNG
    # stream assertion (probe-verified). The interactive path leaves them
    # running.
    systems.quiesce_lights()
    bd.assert_true(not systems.lights.programs,
                   "light programs quiesced before the checkpoint")


def autotest_save():
    bd.save_checkpoint(content.CHECKPOINT_NAME,
                       description="Pyre & Rime autotest")


def autotest_rng_draws():
    # Drawn after the checkpoint was written: these consume script-RNG
    # stream positions the load will rewind.
    global loot_sequence
    loot_sequence = [systems.elite_loot.roll()["class_name"]
                     for _ in range(3)]


def autotest_load():
    bd.load_checkpoint(content.CHECKPOINT_NAME)


def autotest_post_load():
    pawn = player_pawn()
    redrawn = [systems.elite_loot.roll()["class_name"] for _ in range(3)]
    bd.assert_true(loot_sequence is not None and redrawn == loot_sequence,
                   "loot RNG stream resumes exactly after checkpoint load")
    ref = bd.actor_ref(content.STATUS_IMP_TID)
    bd.assert_true(ref is not None and ref.valid,
                   "persistence: TID-tagged imp re-resolved after load")
    if ref is not None and ref.valid:
        remaining = status.remaining(ref, "poisoned")
        bd.assert_true(0 < remaining < 700,
                       f"persistence: poisoned timer survived the "
                       f"round-trip (remaining {remaining})")
    bd.assert_true(pawn is not None
                   and bd_rpg.affinity_of(pawn, "pyre") == 0.5,
                   "persistence: player affinity re-applied after load")
    saved = bd.state.get(bd_rpg.STATE_KEY)
    bd.assert_true(isinstance(saved, dict)
                   and "status_timers" in saved,
                   "persistence: bd_rpg state present in bd.state")
    rime_bound = bd.actor_ref(content.ELITE_TIDS[2])  # alive: survived
    bd.assert_true(rime_bound is not None and rime_bound.valid,
                   "persistence: the Rime-Bound survived the checkpoint")
    if rime_bound is not None and rime_bound.valid:
        pack = bd.actor_data(rime_bound).get("bd_rpg") or {}
        bd.assert_true(pack.get("elite") == "rime",
                       "persistence: Rime-Bound marker re-applied on "
                       "map_load")
        # Slay the Rime-Bound (credited to the player): the legendary
        # relic drops. Assertions land in autotest_post_load_rites.
        if pawn is not None:
            rime_bound.damage(1000, source=pawn)


def autotest_post_load_rites():
    """The Rime-Bound's death, post-load: legendary relic, omen toast,
    death rattle."""
    from bd_horror import toasts
    bd.assert_true(len(systems.drops) == 2
                   and systems.drops[-1][1] == "legendary",
                   "the Rime-Bound dropped a legendary relic")
    if len(systems.drops) == 2:
        class_name = systems.drops[-1][0]
        relic = content.RELIC_NAMES.get(class_name)
        bd.assert_true(relic is not None,
                       f"the legendary drop is a named relic "
                       f"({class_name})")
        bd.assert_true(any(t["kind"] == "omen" and relic in t["text"]
                           for t in toasts.history),
                       "legendary drop raised the relic omen toast")
    bd.assert_true(content.ELITE_TIDS[2] in systems.rattle_log,
                   "the Rime-Bound's death rattle fired")
    bd.assert_true(bd_rpg.kill_xp(0) == 40,
                   "kill XP: both credited elite kills counted "
                   "(20 + 20 across the checkpoint)")
    bd.log("PYRE & RIME AUTOTEST assertions complete")


# --- screenshot helper ---------------------------------------------------------------


def screenshot_flavor():
    """Set up a lively HUD: two statuses on the bearer, a thrall kill
    with its rattle and spoils, a burst line for the litany."""
    pawn = player_pawn()
    if pawn is None:
        return
    status.apply(pawn, "burning", 175)
    status.apply(pawn, "slowed", 280)
    thrall = bd.actor_ref(content.ELITE_TIDS[0])
    if thrall is not None and thrall.valid:
        thrall.damage(1000, source=pawn)
    systems.elemental_burst()


# --- imgui overlay ------------------------------------------------------------------


@bd.on("imgui_frame")
def draw_ui(event):
    # Headless runs never fire imgui_frame, so this whole HUD is a no-op
    # there by construction; the autotest never depends on it.
    ui.draw_frame(ui_ctx)
