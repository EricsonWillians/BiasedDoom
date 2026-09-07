"""The Last Feeding — bootstrap, event wiring, and the autotest driver.

A Vampire: the Masquerade mini-chronicle on Doom II MAP01, built on the
shipped ``bd_vtm``, ``bd_quests``, and ``bd_horror`` packages.
Architecture:

- ``content.py`` — pure data (mortal herd, Sabbat packs, disciplines,
  factions, quests, prose, tunables). No engine calls at import time.
- ``systems.py`` — the rules engine: stock bd_vtm systems plus the hunt
  layer (cowering mortals, hunger-driven dread, darkness-aware feeding,
  the breach ambush, the frenzy vignette storm).
- ``ui.py`` — the bd_horror-skinned vitae HUD and discipline panel.
- ``main.py`` (this file) — imports the siblings, registers every engine
  event, and drives the deterministic autotest.

The PYTHON manifest lists all four modules; the runtime execs each one,
and this file imports the other three as siblings via ``bd.import_script``
(the hello_world pattern). The libraries are definition-only at import,
so their manifest execution is inert and all wiring happens here, once.

The HUD toggles with the H key or the ``toggle_hud`` console command,
wired manually through the ``alias``/``pyui``/``ui_command`` bridge (the
same pattern ``bd_vtm.bind_hud_toggle`` uses, aimed at our custom themed
window).

Autotest: ``BD_EXAMPLE_AUTOTEST=1`` drives the rules engine headlessly
and deterministically — blood spend/gain math, hunger accrual, the
hunger-driven dread binding, scripted-roller degeneration checks,
twin-seed frenzy determinism, discipline blood denial/cooldowns
(including potence), feeding (drain, kill -> humanity loss, witnessed ->
masquerade violation), the darkness-halved witness radius, cowering
displacement, faction gating, the breach ambush, the frenzy storm, then
a checkpoint round-trip asserting that the script RNG stream and the
whole VtMState resume exactly. The run ends via ``-scripttest``'s own
PASS/FAIL accounting.

``BD_EXAMPLE_SCREENSHOT=1`` schedules a screenshot a few seconds in, for
documentation captures.
"""

import math
import os

import biaseddoom as bd
import bd_quests
from bd_quests import Quest
import bd_vtm
from bd_horror import toasts

content = bd.import_script("pyscripts/content.py", module_name="lf_content")
systems = bd.import_script("pyscripts/systems.py", module_name="lf_systems")
ui = bd.import_script("pyscripts/ui.py", module_name="lf_ui")

AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"
SCREENSHOT = os.environ.get("BD_EXAMPLE_SCREENSHOT") == "1"

# --- module state ------------------------------------------------------------

chron = None        # systems.Chronicle, built at engine_start
hud = None          # ui.VitaeHud
panel = None        # ui.DisciplinePanel
autowarp_done = False
hud_drawn_ok = False
panel_drawn_ok = False
rng_pair = None          # (a1, a2) drawn after the checkpoint save
expected_snapshot = None # dict of VtMState scalars captured before the reload
cower_probe = None       # recorded mortal/pawn positions for the cower test


# --- engine wiring --------------------------------------------------------------


@bd.on("engine_start")
def on_engine_start(event):
    global chron, hud, panel
    bd.imgui.set_master_visible(True)
    bd.log(f"bd_vtm shipped from: {bd_vtm.__file__}")
    chron = systems.build_chronicle(content)
    hud = ui.VitaeHud(chron, title="Vitae")
    panel = ui.DisciplinePanel(chron, content, title="Disciplines")
    # Console/key toggle, wired manually through the pyui bridge: running
    # `toggle_hud` (or pressing H) fires a ui_command event that flips the
    # HUD's visibility.
    bd.execute('alias toggle_hud "pyui toggle_hud"')
    bd.execute("bind h toggle_hud")


@bd.on("ui_command")
def on_ui_command(event):
    if event.get("command") == "toggle_hud" and hud is not None:
        hud.toggle()


@bd.on("map_load")
def on_map(event):
    systems.setup_map(chron, content, event)

    if event.get("from_savegame"):
        # The checkpoint restored the world and the bd_vtm/bd_quests load
        # handlers restored chronicle and quests; only post-load steps.
        if AUTOTEST:
            bd.schedule(autotest_post_load, delay=12)
            bd.schedule(autotest_hud_toggle_off, delay=20)
            bd.schedule(autotest_hud_toggle_on, delay=32)
            bd.schedule(autotest_hud_toggle_assert, delay=44)
        return

    pawn = systems.player_pawn()
    if pawn is None:
        return

    if AUTOTEST:
        pawn.damage_factor = 0.0  # nothing may kill the test driver
        bd.schedule(autotest_blood_math, delay=10)
        bd.schedule(autotest_hunger_accrual, delay=20)
        bd.schedule(autotest_hunger_dread, delay=26)
        bd.schedule(autotest_humanity_and_frenzy, delay=34)
        bd.schedule(autotest_disciplines, delay=44)
        bd.schedule(autotest_feeding, delay=54)
        bd.schedule(autotest_witnessed_feeding, delay=64)
        bd.schedule(autotest_faction_gate_closed, delay=74)
        bd.schedule(autotest_kill_pack, delay=84)
        bd.schedule(autotest_darkness_feed, delay=92)
        bd.schedule(autotest_mid_asserts, delay=104)
        bd.schedule(autotest_cower_begin, delay=112)
        bd.schedule(autotest_breach_ambush, delay=120)
        bd.schedule(autotest_cower_assert, delay=148)
        bd.schedule(autotest_frenzy_storm, delay=152)
        bd.schedule(autotest_save, delay=158)
        bd.schedule(autotest_rng_draws, delay=172)
        bd.schedule(autotest_storm_ended, delay=226)
        bd.schedule(autotest_load, delay=234)
    if SCREENSHOT:
        bd.schedule(screenshot_flavor, delay=30)
        bd.schedule(screenshot_toast, delay=45)
        bd.schedule(lambda: bd.execute("screenshot /tmp/last_feeding"),
                    delay=88)
        bd.schedule(lambda: bd.execute("quit"), delay=175)


# --- autotest: the stock bd_vtm rules -------------------------------------------------


def autotest_blood_math():
    pool = chron.state.blood
    fired = []
    pool.on_blood_changed.append(lambda p: fired.append(p.current))
    bd.assert_true(pool.max == 10 and pool.current == 10,
                   "13th generation starts at 10/10 blood")
    bd.assert_true(pool.spend(3) and pool.current == 7, "spend(3) -> 7")
    bd.assert_true(not pool.spend(8) and pool.current == 7,
                   "overspending is denied")
    bd.assert_true(pool.spend(7) and pool.current == 0, "spend to empty")
    bd.assert_true(pool.gain(4) == 4 and pool.current == 4, "gain(4)")
    bd.assert_true(pool.gain(99) == 6 and pool.current == 10,
                   "gain clamps at max and reports the real gain")
    bd.assert_true(fired == [7, 0, 4, 10], "on_blood_changed sequence")
    bd.assert_true(chron.state.hunger.level == 0, "full pool means no hunger")


def autotest_hunger_accrual():
    pool = chron.state.blood
    pool.spend(9)  # 1/10 = 10% of the pool left
    bd.assert_true(chron.state.hunger.level == 4, "hunger 4 at <=20% blood")
    pool.spend(1)  # empty
    bd.assert_true(chron.state.hunger.level == 5, "hunger 5 on empty pool")
    pool.gain(10)
    bd.assert_true(chron.state.hunger.level == 0, "feeding resets hunger")


def autotest_hunger_dread():
    # The hunt binding: every blood change re-derives hunger, and the
    # bd_horror dread meter follows it one step of DREAD_PER_HUNGER each.
    dread = chron.horror.dread
    bd.assert_true(dread.level == 0.0, "dread calm while the pool is full")
    chron.state.blood.spend(9)
    bd.assert_true(dread.level == 4 * content.DREAD_PER_HUNGER,
                   "dread tracks hunger 4")
    chron.state.blood.spend(1)
    bd.assert_true(dread.level == 5 * content.DREAD_PER_HUNGER,
                   "dread maxes out on an empty pool")
    chron.state.blood.gain(10)
    bd.assert_true(dread.level == 0.0, "dread drains when the hunger does")


def autotest_humanity_and_frenzy():
    # Degeneration rule with a scripted roller: loss only when roll > rating.
    changes = []
    applied = bd_vtm.Humanity(7, rng=systems.Roller([8]))
    applied.on_humanity_changed.append(
        lambda h, delta, reason: changes.append((delta, reason)))
    bd.assert_true(applied.lose(1, "test sin") and applied.rating == 6,
                   "degeneration applies when the roll beats the rating")
    bd.assert_true(changes == [(-1, "test sin")],
                   "on_humanity_changed fired with delta and reason")
    resisted = bd_vtm.Humanity(7, rng=systems.Roller([3]))
    bd.assert_true(not resisted.lose(1) and resisted.rating == 7,
                   "degeneration resisted when the roll ties/undercuts")
    saint = bd_vtm.Humanity(10, rng=systems.Roller([10]))
    bd.assert_true(not saint.lose(1) and saint.rating == 10,
                   "rating 10 always resists (no roll beats it)")
    bd.assert_true(bd_vtm.Humanity(3).frenzy_bonus == 1
                   and bd_vtm.Humanity(7).frenzy_bonus == 0,
                   "frenzy_bonus kicks in at humanity <= 3")
    # Frenzy gate: below hunger 4 (plus bonus) no roll is even made.
    bd.assert_true(not bd_vtm.Hunger(3).check_frenzy(rng=bd.rng(5)),
                   "hunger 3 cannot frenzy")
    # Determinism: identical seeds produce identical rolls and outcomes.
    s1, s2 = bd.rng(2024), bd.rng(2024)
    bd.assert_true([s1.int(1, 10) for _ in range(8)]
                   == [s2.int(1, 10) for _ in range(8)],
                   "twin rng streams produce identical sequences")
    bd.assert_true(bd_vtm.Hunger(5).check_frenzy(rng=bd.rng(99))
                   == bd_vtm.Hunger(5).check_frenzy(rng=bd.rng(99)),
                   "frenzy outcome identical across identical seeds")
    # Statistical sanity of the engine stream: ~50% frenzy at effective 5.
    stream = bd.rng(777)
    rolls = [stream.int(1, 10) for _ in range(400)]
    bd.assert_true(min(rolls) >= 1 and max(rolls) <= 10,
                   "rolls stay inside [1, 10]")
    frenzies = sum(1 for roll in rolls if roll <= 5)
    bd.assert_true(120 < frenzies < 280,
                   "frenzy rate at hunger 5 is statistically sane")


def autotest_disciplines():
    pawn = systems.player_pawn()
    bd.assert_true(pawn is not None, "player pawn available")
    if pawn is None:
        return
    state = chron.state
    celerity = state.disciplines.get("celerity")
    state.blood.spend(state.blood.current)  # empty the pool
    bd.assert_true(not celerity.use(pawn),
                   "discipline denied on an empty blood pool")
    state.blood.gain(10)
    original_speed = pawn.speed
    bd.assert_true(celerity.use(pawn), "celerity fires with blood in pool")
    bd.assert_true(abs(pawn.speed - original_speed * 1.25) < 0.001,
                   "celerity level 1 boosts speed by 25%")
    bd.assert_true(not celerity.use(pawn), "celerity is on cooldown")
    bd.assert_true(celerity.ready_in() > 0, "ready_in reports the cooldown")
    potence = state.disciplines.get("potence")
    bd.assert_true(potence is not None, "potence registered")
    original_mult = pawn.damage_multiply
    bd.assert_true(potence.use(pawn), "potence fires with blood in pool")
    bd.assert_true(abs(pawn.damage_multiply - original_mult * 2.0) < 0.001,
                   "potence level 1 doubles damage dealt")
    dominate = state.disciplines.get("dominate")
    bd.assert_true(not dominate.use(pawn, None), "dominate requires a target")
    shovelhead = bd.actor_ref(content.SABBAT_TIDS[0])
    bd.assert_true(shovelhead is not None and shovelhead.valid,
                   "sabbat pack member resolvable by TID")
    if shovelhead is not None and shovelhead.valid:
        bd.assert_true(dominate.use(pawn, shovelhead),
                       "dominate pacifies a living monster")
        bd.assert_true(shovelhead.damage_multiply == 0.0,
                       "pacified target deals no damage")


def autotest_feeding():
    pawn = systems.player_pawn()
    victim = bd.actor_ref(content.MORTAL_TIDS[0])
    bd.assert_true(pawn is not None and victim is not None and victim.valid,
                   "predator and victim handles available")
    if pawn is None or victim is None or not victim.valid:
        return
    before = chron.state.blood.current
    result = bd_vtm.feed(pawn, victim, chron.state.blood, drain=5,
                         blood_gain=1)
    bd.assert_true(result["drained"] > 0 and not result["killed"],
                   "a sip drains the victim without killing")
    bd.assert_true(chron.state.blood.current == before + 1,
                   "feeding refills the blood pool")
    bd.assert_true(not result["witnessed"],
                   "no masquerade tracker means no witness report")
    # Draining dry kills, and the kill costs humanity; the scripted roller
    # guarantees the degeneration check fails (roll 10 > rating 7).
    test_humanity = bd_vtm.Humanity(7, rng=systems.Roller([10]))
    losses = []
    test_humanity.on_humanity_changed.append(
        lambda h, delta, reason: losses.append((delta, reason)))
    result = bd_vtm.feed(pawn, victim, chron.state.blood,
                         humanity=test_humanity, drain=50)
    bd.assert_true(result["killed"], "draining dry kills the victim")
    bd.assert_true(test_humanity.rating == 6
                   and losses == [(-1, "drained a victim dry")],
                   "killing a vessel costs humanity")


def autotest_witnessed_feeding():
    pawn = systems.player_pawn()
    victim = bd.actor_ref(content.MORTAL_TIDS[1])
    bd.assert_true(pawn is not None and victim is not None and victim.valid,
                   "second victim available")
    if pawn is None or victim is None or not victim.valid:
        return
    before = chron.state.masquerade.level
    result = bd_vtm.feed(pawn, victim, chron.state.blood,
                         masquerade=chron.state.masquerade, drain=5)
    bd.assert_true(result["witnessed"],
                   "feeding in the open is witnessed by bystanders")
    bd.assert_true(chron.state.masquerade.level == before + 1,
                   "witnessed feeding is a masquerade violation")


def autotest_faction_gate_closed():
    quest = chron.log.get("street_cred")
    bd.assert_true(quest is not None and quest.state == Quest.INACTIVE,
                   "gated quest starts inactive")
    bd.assert_true(chron.gated_quest is not None
                   and not chron.gated_quest.can_start(),
                   "gate closed at Anarchs standing 0")
    bd.assert_true(not chron.gated_quest.start(), "gated start is blocked")
    bd.assert_true(quest.state == Quest.INACTIVE,
                   "quest remains inactive after a blocked start")
    bd.assert_true(chron.state.factions.standing("Camarilla", "Sabbat") == -2,
                   "disposition matrix: Camarilla hostile to Sabbat")
    bd.assert_true(chron.state.factions.standing("Anarchs", "Camarilla") == 0,
                   "disposition default is neutral")


def autotest_kill_pack():
    # Kills are sourced to the player pawn: bd_quests.track_kills defaults
    # to killer="player" exact credit, so source-less scripted kills would
    # not count (that is the intended attribution semantics).
    pawn = systems.player_pawn()
    for tid in content.SABBAT_TIDS:
        ref = bd.actor_ref(tid)
        if ref is not None and ref.valid:
            ref.damage(1000, source=pawn)


# --- autotest: the hunt layer ---------------------------------------------------------


def autotest_darkness_feed():
    """The witness radius halves in darkness: same feed, two outcomes."""
    pawn = systems.player_pawn()
    victim = bd.actor_ref(content.MORTAL_TIDS[1])
    witness = bd.actor_ref(content.MORTAL_TIDS[2])
    bd.assert_true(pawn is not None and victim is not None and victim.valid
                   and witness is not None and witness.valid,
                   "darkness feed cast available")
    if pawn is None or victim is None or not victim.valid:
        return
    # Clear the stage: anything else alive near the concourse counts as a
    # witness to bd_vtm.feed — stock monsters, and even dropped clips from
    # the dead pack (inanimate objects pass the engine's sight check).
    # destroy() removes without a death event.
    for ref in bd.actor_refs(sphere=(pawn.x, pawn.y, 600.0)):
        try:
            if (ref.alive and not ref.is_player
                    and ref.tid not in content.MORTAL_TIDS):
                ref.destroy()
        except Exception:
            continue
    # Place the witness between the dark and lit radii, sightline open.
    wx, wy = -96.0, 1072.0
    sector = bd.sector_at(wx, wy)
    witness.set_position(wx, wy, float(sector.floor_height),
                         check=False, fog=False)
    bd.assert_true(witness.check_sight(pawn) and pawn.check_sight(witness),
                   "witness has an open sightline to the predator")
    dist_2d = math.hypot(witness.x - pawn.x, witness.y - pawn.y)
    bd.assert_true(abs(dist_2d - 288.0) < 1.0,
                   "witness sits between the dark and lit radii")
    own = bd.sector_at(pawn.x, pawn.y)
    original_light = int(own.light)
    own.light = 40
    bd.assert_true(systems.witness_radius_for(pawn, content)
                   == content.WITNESS_RADIUS_LIT * content.WITNESS_DARK_FACTOR,
                   "darkness halves the witness radius")
    dark = systems.feed(chron, pawn, victim, content, drain=3)
    bd.assert_true(not dark["witnessed"],
                   "feeding in the dark goes unseen at 288 units")
    bd.assert_true(dark["radius"] == 256.0,
                   "the feed used the halved radius")
    own.light = original_light
    bd.assert_true(systems.witness_radius_for(pawn, content)
                   == content.WITNESS_RADIUS_LIT,
                   "light restores the full witness radius")
    lit = systems.feed(chron, pawn, victim, content, drain=3)
    bd.assert_true(lit["witnessed"],
                   "the same feed is witnessed once the lights are on")
    bd.assert_true(lit["radius"] == 512.0, "the feed used the full radius")


def autotest_mid_asserts():
    bd.assert_true(chron.log.get("first_night").state == Quest.COMPLETED,
                   "first_night completed after the pack is dead")
    bd.assert_true(chron.state.factions.reputation("Anarchs") == 1,
                   "completing the first night grants Anarchs standing")
    bd.assert_true(chron.log.get("street_cred").state == Quest.ACTIVE,
                   "the gate opened once standing sufficed")


def autotest_cower_begin():
    global cower_probe
    pawn = systems.player_pawn()
    mortal = bd.actor_ref(content.MORTAL_TIDS[2])
    if pawn is None or mortal is None or not mortal.valid:
        return
    # Close on Father Abram: inside the cower radius, in plain sight.
    spot = bd.sector_at(-96.0, 976.0)
    pawn.set_position(-96.0, 976.0, float(spot.floor_height),
                      check=False, fog=False)
    cower_probe = {"x": mortal.x, "y": mortal.y,
                   "px": pawn.x, "py": pawn.y}


def autotest_breach_ambush():
    # Five violations in one step force the breach crossing; the Sabbat
    # answer with a pack spawned around the player.
    before = len(chron.ambush_log)
    level = chron.state.masquerade.violation("autotest breach", amount=5)
    bd.assert_true(level == bd_vtm.Masquerade.MAX_LEVEL,
                   "forced violations cap the masquerade track")
    bd.assert_true(len(chron.ambush_log) == before + 1,
                   "the breach spawned an answering pack")
    pack = chron.ambush_log[-1]["tids"] if chron.ambush_log else []
    bd.assert_true(len(pack) == len(content.AMBUSH_TIDS),
                   "the full ambush pack materialized")
    for tid in pack:
        ref = bd.actor_ref(tid)
        bd.assert_true(ref is not None and ref.valid and ref.alive,
                       f"answering shovelhead {tid} lives")
    bd.assert_true(any(entry["kind"] == "harm" and "Masquerade" in entry["text"]
                       for entry in toasts.history),
                   "breach harm toast recorded")


def autotest_cower_assert():
    mortal = bd.actor_ref(content.MORTAL_TIDS[2])
    bd.assert_true(cower_probe is not None and mortal is not None
                   and mortal.valid and mortal.alive,
                   "cowering test subject still available")
    if cower_probe is None or mortal is None or not mortal.valid:
        return
    bd.assert_true(len(chron.cower_log) > 0,
                   "the cower tick fired while the predator was close")
    # Displacement must carry the mortal away from the recorded predator
    # position (due north in this staged geometry).
    away = (mortal.x - cower_probe["px"], mortal.y - cower_probe["py"])
    norm = math.hypot(*away) or 1.0
    moved = (mortal.x - cower_probe["x"], mortal.y - cower_probe["y"])
    dot = (moved[0] * away[0] + moved[1] * away[1]) / norm
    bd.assert_true(dot > 8.0,
                   "the mortal scrambled away from the predator")
    pawn = systems.player_pawn()
    if pawn is not None:
        bd.assert_true(mortal.distance_to(pawn) > 100.0,
                       "the mortal opened the distance")


def autotest_frenzy_storm():
    systems.start_frenzy_storm(chron, content)
    bd.assert_true(chron.storm_active(),
                   "the frenzy vignette storm spins up")


def autotest_storm_ended():
    bd.assert_true(not chron.storm_active(),
                   "the frenzy storm spends itself")
    bd.assert_true(chron.storm["pulses"] >= 5,
                   "the storm pulsed on schedule")


# --- autotest: the checkpoint round-trip --------------------------------------------


def autotest_save():
    # Advance the script RNG stream so the checkpoint captures a nontrivial
    # stream position; the save itself happens at the next tic boundary.
    for _ in range(3):
        bd.randint(1, 1000000)
    bd.save_checkpoint(content.CHECKPOINT_NAME,
                       description="The Last Feeding autotest")


def autotest_rng_draws():
    # Drawn after the checkpoint was written: these consume stream positions
    # the load will rewind.
    global rng_pair, expected_snapshot
    rng_pair = (bd.randint(1, 1000000), bd.randint(1, 1000000))
    expected_snapshot = {
        "blood": chron.state.blood.current,
        "hunger": chron.state.hunger.level,
        "humanity": chron.state.humanity.rating,
        "masquerade": chron.state.masquerade.level,
        "anarchs": chron.state.factions.reputation("Anarchs"),
    }


def autotest_load():
    bd.load_checkpoint(content.CHECKPOINT_NAME)


def autotest_post_load():
    redrawn = (bd.randint(1, 1000000), bd.randint(1, 1000000))
    bd.assert_true(rng_pair is not None and redrawn == rng_pair,
                   "script RNG stream resumes exactly after checkpoint load")
    snap = expected_snapshot or {}
    bd.assert_true(chron.state.blood.current == snap.get("blood"),
                   "blood pool round-tripped through the checkpoint")
    bd.assert_true(chron.state.hunger.level == snap.get("hunger"),
                   "hunger round-tripped through the checkpoint")
    bd.assert_true(chron.state.humanity.rating == snap.get("humanity"),
                   "humanity round-tripped through the checkpoint")
    bd.assert_true(chron.state.masquerade.level == snap.get("masquerade"),
                   "masquerade round-tripped through the checkpoint")
    bd.assert_true(chron.state.factions.reputation("Anarchs")
                   == snap.get("anarchs"),
                   "faction reputation round-tripped through the checkpoint")
    saved = bd.state.get(bd_vtm.STATE_KEY)
    bd.assert_true(isinstance(saved, dict) and "blood" in saved,
                   "bd_vtm state present in bd.state after load")
    bd.assert_true(isinstance(bd.state.get("bd_horror"), dict),
                   "bd_horror state present in bd.state after load")
    bd.assert_true(chron.log.get("first_night").state == Quest.COMPLETED,
                   "quest state preserved across the round-trip")
    bd.assert_true(chron.log.get("street_cred").state == Quest.ACTIVE,
                   "gated quest state preserved across the round-trip")
    for tid in content.AMBUSH_TIDS:
        ref = bd.actor_ref(tid)
        bd.assert_true(ref is not None and ref.valid and ref.alive,
                       f"ambush pack member {tid} survived the round-trip")
    # imgui_frame never fires under -headless; draw smoke checks are
    # rendering-dependent and legitimately skipped there.
    if not bd.headless():
        bd.assert_true(hud_drawn_ok, "vitae HUD drew inside imgui_frame")
        bd.assert_true(panel_drawn_ok,
                       "discipline panel drew inside imgui_frame")
    bd.log("THE LAST FEEDING AUTOTEST assertions complete")


# --- autotest: the HUD toggle (pyui / ui_command) ---------------------------------------


def autotest_hud_toggle_off():
    bd.assert_true(hud.visible, "HUD visible before the console toggle")
    bd.execute("toggle_hud")


def autotest_hud_toggle_on():
    bd.assert_true(not hud.visible, "toggle_hud hid the HUD via ui_command")
    bd.execute("toggle_hud")


def autotest_hud_toggle_assert():
    bd.assert_true(hud.visible,
                   "toggle_hud showed the HUD again (idempotent binding)")


# --- screenshot drive -----------------------------------------------------------------


def screenshot_flavor():
    # Spend blood, fire celerity (cooldown sweep), and feed once so the
    # HUD and panel show off every row.
    pawn = systems.player_pawn()
    if pawn is None:
        return
    chron.state.blood.spend(5)
    chron.state.disciplines.get("celerity").use(pawn)
    chron.feed_nearest(pawn, content)


def screenshot_toast():
    toasts.toast("the Masquerade frays - someone was watching", kind="harm")


# --- the overlay ------------------------------------------------------------------------


@bd.on("imgui_frame")
def draw_ui(event):
    global autowarp_done, hud_drawn_ok, panel_drawn_ok
    # Headless runs (-scripttest) launch without +map: queue a warp on the
    # first rendered frame when nothing loaded a level yet.
    if not autowarp_done:
        autowarp_done = True
        try:
            if not bd.current_map():
                bd.execute("map map01")
        except RuntimeError:
            pass
    if hud is not None and hud.visible:
        hud.draw()
        hud_drawn_ok = True
    if panel is not None and panel.visible:
        panel.draw()
        panel_drawn_ok = True
    toasts.draw_toasts()  # no-op when the queue is empty
