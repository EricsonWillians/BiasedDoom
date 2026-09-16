"""The Delve, a map-agnostic D&D rules layer in the roguelike idiom.

Built on the engine-shipped ``bd_dnd`` rules framework, the ``bd_horror``
presentation pack (toasts and the sheet skin only), and the ``bd_quests``
journal pack. It owes its shape to 15_roguelike_run: no probed fixtures,
no map gating, one clear loop that works identically on every map, and
everything announced loudly.

The loop: found a delver (Fighter / Rogue / Cleric class cards driving a
real ``bd_dnd.CreationWizard``; interactive play opens a world-paused
founding window, headless and test runs call the same ``systems.found_hero``).
Every fresh map then deals a contract: a blood tribute (put down N of the
map's monsters, player-credited kills only) and a crowned Warden (the
census monster with the highest XP-table value, empowered with doubled
health, gold tint, a gold ring and title, worth 5x XP). Kills pay XP with
floating gold popups; levels heal fully and toughen blows; every check is
a visible d20 (door bashes and lockpicks, the reflex save, the nightmare
rest save); rests are safe only in the light and spawn real nightmares in
the dark; the Guild Hound fights at your side.

Module map (all four are listed in the PYTHON manifest):

- ``content.py``: pure data and factories: the three classes with their
  level-1/level-2 active features, preset standard arrays, the contract
  factory, tuning constants, the MAP02 autotest door fixture, prose.
- ``systems.py``: behavior: the founding, the contract (census, crown,
  quest, tribute/warden death handling), announce_check, the door
  dispatcher, the reflex rule, class actives, rest-with-teeth, the strip,
  the cold restore. Registers no events at import.
- ``ui.py``: the founding window and the slim reliquary sheet.
- ``main.py`` (this file): bootstrap, event wiring, the autotest driver.

Autotest: ``BD_EXAMPLE_AUTOTEST=1`` drives the rules engine headlessly
and deterministically on MAP02 (its line 111 is a real locked door, the
only probed fixture): founding through the same function the cards call
(Fighter for the run, Rogue and Cleric unit-founded for feature/active
coverage), the classes layer (validation, presets, level-2 charge
features), dice/checks/XP/rests units, the contract (census, the
deterministic Warden pick, tribute credited-only counting, the Warden
kill and its 5x bounty, completion XP and fanfare), visible rolls in
``check_log`` (the real line-111 bash drive, the reflex save, both rest
branches; the dark-failure branch asserts the nightmare demons exist,
are hostile, and are not FRIENDLY), class actives (Second Wind heal
through a real Custom Action 3 press plus the direct path, the Rogue
blur with the damage_factor toggle, the Cleric burst), the companion
(damage sync, teleport catch-up, the stuck-teleport across a door, kill
knell, revive), then a checkpoint round-trip (RNG stream, CharacterState,
PartyState, companion rebind, contract counter + warden tid + quest
states). The run ends via ``-scripttest``'s own PASS/FAIL accounting.

``BD_EXAMPLE_SCREENSHOT=1`` founds the Fighter and schedules a screenshot
a few seconds in, for documentation captures.
"""

import os
import sys

import biaseddoom as bd
import bd_dnd
import bd_horror
import bd_quests
from bd_dnd.sheet import bind_sheet_toggle
from bd_horror import toasts


def _load_sibling(path, module_name):
    """Import a sibling module once, through sys.modules.

    Mirrors how hello_world imports pyscripts/helper.py, but idempotent:
    the engine also executes every PYTHON manifest line as a script, so a
    sibling may already be registered under its module name; importing
    it again would re-execute the module and double-register callbacks.
    """
    module = sys.modules.get(module_name)
    if module is None:
        module = bd.import_script(path, module_name=module_name)
    return module


content = _load_sibling("pyscripts/content.py", "crypt_content")
systems = _load_sibling("pyscripts/systems.py", "crypt_systems")
ui = _load_sibling("pyscripts/ui.py", "crypt_ui")

AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"
SCREENSHOT = os.environ.get("BD_EXAMPLE_SCREENSHOT") == "1"

player_pawn = systems.player_pawn

sheet = None            # the reliquary sheet, created at founding
sheet_drawn_ok = False
first_map_seen = False  # the founding window opens on the first map only
autowarp_done = False

door_drive_until = -1   # pre_tick +use pulses while level_time < this
door_check = None       # the MAP02 line-111 check (autotest pre-registers)
rng_pair = None         # (a1, a2) drawn after the checkpoint save
expected_snapshot = None  # hero.serialize() captured before reload

#: Every custom_action payload the example saw, as (action, pressed)
#: pairs. The autotest asserts on the exact transitions.
action_log = []
#: The outcome dict of the last rest Custom Action 2 ran.
last_rest_outcome = None
#: The outcome dict of the last class-active Custom Action 3 ran.
last_active_outcome = None


class _Roller:
    """Scripted rng test double: yields clamped values in order."""

    def __init__(self, values):
        self._values = list(values)
        self._index = 0

    def randint(self, lo, hi):
        value = self._values[self._index % len(self._values)]
        self._index += 1
        return max(lo, min(hi, value))


def raises_value_error(fn):
    """True when fn() raises ValueError (bd.assert_true never raises)."""
    try:
        fn()
    except ValueError:
        return True
    except Exception:
        return False
    return False


# --- custom actions and aliases -------------------------------------------------


def ensure_custom_action_binding(n, default_key):
    """Bind ``default_key`` to ``+pyactionN`` when the player has not.

    Custom Actions are ordinary engine buttons, so a binding set through
    Options -> Customize Controls, Custom Actions always wins; this only
    fills in a stock-unbound default so the example is playable out of the
    box. Safe from ``engine_start`` (console commands queue pre-map).
    """
    try:
        if bd.input_binding(f"+pyaction{n}") is None:
            bd.execute(f"bind {default_key} +pyaction{n}")
    except Exception as exc:
        bd.warn(f"delve: could not bind +pyaction{n}: {exc!r}")


def _run_delve_rest():
    """The delve_rest body shared by the alias and Custom Action 2."""
    global last_rest_outcome
    if systems.hero is None:
        return None
    outcome = systems.try_long_rest(systems.hero)
    last_rest_outcome = outcome
    kind = outcome.get("kind")
    if kind == "sanctuary":
        bd.center_message("You rest in the light.")
    elif kind == "fitful":
        bd.center_message("You sleep badly, and wake worse.")
    elif kind == "nightmare":
        bd.center_message("The dark dreams with teeth.")
    else:
        bd.center_message("You cannot rest yet.")
    return outcome


def _run_class_active():
    """The class_active body shared by the alias and Custom Action 3."""
    global last_active_outcome
    outcome = systems.use_class_active()
    last_active_outcome = outcome
    return outcome


@bd.on("ui_command")
def on_ui_command(event):
    command = event.get("command")
    if AUTOTEST:
        return  # the autotest drives the Custom Action paths directly
    if command == "delve_rest":
        _run_delve_rest()
    elif command == "class_active":
        _run_class_active()


@bd.on("custom_action")
def on_custom_action(event):
    """Action 1 toggles the sheet, action 2 rests, action 3 fires the
    class active; all run the same bodies as the console aliases."""
    try:
        action = int(event.get("action") or 0)
        pressed = bool(event.get("pressed"))
        action_log.append((action, pressed))
        if not pressed:
            return
        if action == 1:
            if sheet is not None:
                sheet.toggle()
        elif action == 2:
            _run_delve_rest()
        elif action == 3:
            _run_class_active()
    except Exception as exc:
        bd.warn(f"delve: custom action failed: {exc!r}")


# --- founding -------------------------------------------------------------------


def ensure_sheet():
    """Create (once) the reliquary sheet and its toggle binding.

    Lives here so the sheet exists even when no frame ever renders: the
    Custom Action 1 toggle and the autotest drive it headlessly, where
    ``imgui_frame`` never fires. Called after every founding path (the
    window's card click goes through systems.found_hero too).
    """
    global sheet
    if sheet is None and systems.hero is not None:
        hero = systems.hero
        sheet = ui.ReliquarySheet(
            hero, systems.hero_state,
            title=f"{hero.name} - {hero.class_id} {hero.level}")
        bind_sheet_toggle(sheet)
        ui.ctx.update(sheet=sheet, sheet_drawn_ok=False)
    return sheet


# --- scenario setup ---------------------------------------------------------------


@bd.on("engine_start")
def setup_delve(event):
    bd.imgui.set_master_visible(True)
    bd.log(f"bd_dnd shipped from: {bd_dnd.__file__}")
    bd.log(f"bd_horror shipped from: {bd_horror.theme.__file__}")
    bd.log(f"bd_quests shipped from: {bd_quests.__file__}")

    systems.wire_delve_events()
    if AUTOTEST or SCREENSHOT:
        systems.creation_pause_enabled = False

    # Console aliases through the pyui/ui_command bridge; the keys live on
    # Custom Actions 1 (sheet), 2 (rest), and 3 (class active), auto-bound
    # below and rebindable in Options -> Customize Controls, Custom Actions.
    bd.execute('alias delve_rest "pyui delve_rest"')
    bd.execute('alias class_active "pyui class_active"')
    ensure_custom_action_binding(1, "q")
    ensure_custom_action_binding(2, "v")
    ensure_custom_action_binding(3, "c")


@bd.on("map_load")
def on_map(event):
    global first_map_seen
    pawn = player_pawn()
    systems.discard_creation_pause()  # map changes reset the engine pause

    if event.get("from_savegame"):
        # The checkpoint restored the world; the bd_dnd/bd_quests load
        # handlers restored hero, party, and quest state. The contract's
        # own bookkeeping reconciles one tic in, and the (never
        # serialized) Warden tint and ring/title visuals are re-applied
        # by tid without re-rolling. The health sync hard-resyncs the
        # pawn to the sheet's ratio (map_load is a resync point).
        if systems.hero is not None:
            systems.arm_health_sync()
            systems.arm_examine_probe()
        bd.schedule(systems.reconcile_contract, delay=1)
        if AUTOTEST:
            bd.schedule(autotest_post_load, delay=10)
            bd.schedule(autotest_sheet_toggle_off, delay=20)
            bd.schedule(autotest_sheet_toggle_on, delay=32)
            bd.schedule(autotest_sheet_toggle_assert, delay=44)
            bd.schedule(autotest_custom_action_sheet_press, delay=56)
            bd.schedule(autotest_custom_action_sheet_assert, delay=68)
            bd.schedule(autotest_custom_action_sheet_restore, delay=80)
            bd.schedule(autotest_custom_action_sheet_back, delay=92)
            bd.schedule(autotest_custom_action_rest_press, delay=105)
            bd.schedule(autotest_custom_action_rest_assert, delay=118)
            bd.schedule(autotest_custom_action_rest_release, delay=130)
        return

    # Fresh map: the contract rolls one tic in (the actor_refs-valid
    # idiom), whatever map this is.
    bd.schedule(systems.setup_contract, delay=1)

    if systems.hero is None:
        if AUTOTEST:
            bd.schedule(autotest_found_hero, delay=4)
            bd.schedule(autotest_unit_founding, delay=8)
            bd.schedule(autotest_contract, delay=14)
            bd.schedule(autotest_dice, delay=24)
            bd.schedule(autotest_abilities, delay=34)
            bd.schedule(autotest_checks, delay=44)
            bd.schedule(autotest_xp_levels, delay=54)
            bd.schedule(autotest_resources_rest, delay=64)
            bd.schedule(autotest_kill_credit, delay=74)
            bd.schedule(autotest_levelup, delay=84)
            bd.schedule(autotest_door_generic, delay=94)
            bd.schedule(autotest_skills_damage, delay=99)
            bd.schedule(autotest_door_holds, delay=104)
            bd.schedule(autotest_door_bash, delay=154)
            bd.schedule(autotest_door_asserts, delay=194)
            bd.schedule(autotest_reflex_save, delay=204)
            bd.schedule(autotest_health_sync_damage, delay=210)
            bd.schedule(autotest_health_sync_gain, delay=224)
            bd.schedule(autotest_rest_sanctuary, delay=230)
            bd.schedule(autotest_rest_cooldown, delay=236)
            bd.schedule(autotest_rest_fitful, delay=242)
            bd.schedule(autotest_religion_sanctuary, delay=248)
            bd.schedule(autotest_religion_nightmare, delay=254)
            bd.schedule(autotest_trap_sense, delay=260)
            bd.schedule(autotest_examine, delay=266)
            bd.schedule(autotest_rest_nightmare, delay=272)
            bd.schedule(autotest_active_second_wind_press, delay=282)
            bd.schedule(autotest_active_second_wind_asserts, delay=286)
            bd.schedule(autotest_active_blur, delay=292)
            bd.schedule(autotest_active_turn, delay=296)
            bd.schedule(autotest_blur_restored, delay=476)
            bd.schedule(autotest_companion_damage, delay=486)
            bd.schedule(autotest_companion_stuck_setup, delay=491)
            bd.schedule(autotest_companion_stuck_assert, delay=651)
            bd.schedule(autotest_companion_teleport, delay=656)
            bd.schedule(autotest_companion_teleport_check, delay=681)
            bd.schedule(autotest_companion_kill, delay=686)
            bd.schedule(autotest_companion_revive, delay=696)
            bd.schedule(autotest_companion_revive_check, delay=711)
            bd.schedule(autotest_contract_warden, delay=721)
            bd.schedule(autotest_contract_tribute, delay=726)
            bd.schedule(autotest_boons, delay=731)
            bd.schedule(autotest_mid_asserts, delay=736)
            bd.schedule(autotest_pre_save, delay=741)
            bd.schedule(autotest_save, delay=746)
            bd.schedule(autotest_rng_draws, delay=758)
            bd.schedule(autotest_load, delay=774)
            if pawn is not None:
                pawn.damage_factor = 0.0  # nothing may kill the test driver
        elif SCREENSHOT:
            systems.found_hero("Fighter")
            ensure_sheet()
            bd.schedule(screenshot_flavor, delay=bd.TICRATE)
            bd.schedule(lambda: bd.execute("screenshot /tmp/the_delve"),
                        delay=2 * bd.TICRATE + 10)
            bd.schedule(lambda: bd.execute("quit"), delay=4 * bd.TICRATE)
        elif bd.headless():
            # No window can show headless: the default delver steps in
            # (the universality smokes exercise the full loop this way).
            systems.found_hero("Fighter")
            ensure_sheet()
        elif not first_map_seen:
            # Interactive first map: the world-paused founding window.
            ui.open_founding()
            systems.pause_for_creation()
    if systems.hero is not None:
        systems.arm_health_sync()
        systems.arm_examine_probe()
    ensure_sheet()
    first_map_seen = True


@bd.on("pre_tick")
def drive_door_use(event):
    """Face the sealed door and pulse BT_USE (1 tic on / 3 off).

    AUTOTEST-only door drive for the MAP02 line-111 fixture.
    ``actor.angle`` is set directly; ``set_input(yaw=...)`` writes a yaw
    *delta*, not an absolute facing (probe-verified).
    """
    if not AUTOTEST or door_drive_until < 0:
        return
    if bd.level_time() >= door_drive_until:
        return
    pawn = player_pawn()
    if pawn is None:
        return
    pawn.angle = content.DOOR_FACE_ANGLE
    buttons = bd.BT_USE if (bd.level_time() % 4) == 1 else 0
    bd.player(0).set_input(buttons=buttons)


# --- autotest steps ------------------------------------------------------------


def autotest_found_hero():
    """Found the run's Fighter through the same function the cards call."""
    hero = systems.found_hero("Fighter", "Delver")
    ensure_sheet()
    bd.assert_true(hero is systems.hero and hero.name == "Delver",
                   "the founding produced the hero")
    bd.assert_true(hero.class_id == "Fighter" and hero.cls is content.FIGHTER
                   and hero.hit_die == 10,
                   "the hero is a wizard-bound Fighter")
    bd.assert_true(hero.abilities.score("str") == 15
                   and hero.abilities.score("con") == 14
                   and hero.abilities.score("dex") == 13,
                   "the Fighter preset standard array landed")
    bd.assert_true(hero.proficient_skills == {"athletics", "perception"}
                   and hero.proficient_saves == {"str", "con"},
                   "the Fighter's skills and saves landed")
    bd.assert_true(hero.resources.get("second_wind") == 1
                   and hero.resource_max.get("second_wind") == 1,
                   "the level-1 feature granted the Second Wind charge")
    bd.assert_true(systems.party is not None and len(systems.party) == 2
                   and systems.party.get(content.HOUND_NAME) is not None,
                   "the party is hero plus Guild Hound")
    companion = systems.companion
    bd.assert_true(companion is not None and companion.bound,
                   "the Guild Hound is bound")
    ref = companion.actor() if companion is not None else None
    bd.assert_true(ref is not None and ref.valid and ref.alive
                   and ref.get_flag("FRIENDLY"),
                   "the Guild Hound is live and friendly")
    if ref is not None and ref.valid:
        # The test fixture is invulnerable outside its own controlled
        # hits: no roaming monster (or friendly-fire burst) may kill it
        # mid-test. Companion stages re-enable damage_factor for theirs.
        ref.damage_factor = 0.0
    bd.assert_true(systems.door_bash is not None
                   and systems.door_bash.mode == "str"
                   and systems.door_bash.dc == content.DOOR_DC,
                   "the door dispatcher runs in Athletics mode")
    bd.assert_true(systems.reflex_save is not None
                   and systems.reflex_save.armed,
                   "the announced reflex save is armed")
    pawn = player_pawn()
    if pawn is not None:
        pawn.damage_factor = 0.0  # nothing may kill the test driver
    # The live reflex save would refund scripted wounds at stream-chosen
    # moments and disturb the ratio bookkeeping; the autotest disarms it
    # now and re-arms it for its own stage (handlers never unregister).
    systems.reflex_save.disarm()


def autotest_unit_founding():
    """Rogue and Cleric unit founding: presets, features, door modes."""
    rogue = content.make_delver("Rogue", "Unit Rogue")
    bd.assert_true(rogue.class_id == "Rogue" and rogue.hit_die == 8
                   and rogue.proficient_saves == {"dex", "int"}
                   and rogue.proficient_skills
                   == {"sleight_of_hand", "perception"},
                   "the Rogue preset landed")
    bd.assert_true(rogue.resources.get("uncanny_dodge") == 1,
                   "the Rogue's active charge seeded")
    cleric = content.make_delver("Cleric", "Unit Cleric")
    bd.assert_true(cleric.class_id == "Cleric" and cleric.hit_die == 8
                   and cleric.proficient_saves == {"wis", "cha"}
                   and cleric.proficient_skills == {"religion", "insight"},
                   "the Cleric preset landed")
    bd.assert_true(cleric.resources.get("turn_the_unholy") == 1,
                   "the Cleric's active charge seeded")
    # The level-2 features raise the charge maximum (and refill it).
    rogue.use_resource("uncanny_dodge")
    rogue.award_xp(300)  # exactly level 2
    bd.assert_true(rogue.level == 2
                   and rogue.resource_max.get("uncanny_dodge") == 2
                   and rogue.resources.get("uncanny_dodge") == 2,
                   "the Rogue level-2 feature raised the charge max")
    autotest_unit_founding.rogue = rogue
    autotest_unit_founding.cleric = cleric
    bd.assert_true(systems.door_mode_for(rogue) ==
                   ("dex", content.DOOR_DC - content.ROGUE_DOOR_DC_DELTA),
                   "Rogues pick doors (dex, DC - 2)")
    bd.assert_true(systems.door_mode_for(systems.hero) ==
                   ("str", content.DOOR_DC),
                   "Fighters bash doors (str)")
    # The pick check really rolls sleight_of_hand at the reduced DC (the
    # pawn as activator; the line does not exist, so the refusal stays
    # quiet through the card-lending path).
    pick = systems._CardLendingDoorCheck(99998, rogue, mode="dex",
                                         dc=content.DOOR_DC
                                         - content.ROGUE_DOOR_DC_DELTA)
    result = pick.attempt(player_pawn(), rng=_Roller([16]))  # 16+2+2 >= 13
    bd.assert_true(result["skill"] == "sleight_of_hand"
                   and result["ability"] == "dex"
                   and result["dc"] == content.DOOR_DC
                   - content.ROGUE_DOOR_DC_DELTA
                   and result["success"] and not pick.opened,
                   "the Rogue pick rolls sleight_of_hand at DC - 2")
    # Wizard validation still bites: too few skills rejects the finish.
    bad = bd_dnd.CreationWizard()
    bad.choose_class(content.FIGHTER)
    bad.set_name("Unfinished")
    bad.use_standard_array()
    for ability in bd_dnd.ABILITIES:
        bad.set_score(ability, 10)
    bad.assign_skill("athletics")
    bd.assert_true(raises_value_error(bad.finish),
                   "the wizard rejects a finish with too few class skills")


def autotest_contract():
    """The per-map contract: census, the deterministic Warden, the quest."""
    state = systems.contract_state
    bd.assert_true(state.get("map") == "MAP02" and state.get("census", 0) > 0,
                   "the census found MAP02's monsters")
    goal = content.tribute_goal_for(state["census"])
    bd.assert_true(state.get("tribute_goal") == goal and goal >= 3,
                   "the tribute goal is max(3, census // 4)")
    tid = state.get("warden_tid")
    bd.assert_true(tid, "a Warden was crowned with a real tid")
    ref = bd.actor_ref(tid) if tid else None
    bd.assert_true(ref is not None and ref.valid and ref.alive,
                   "the Warden resolves")
    if ref is not None and ref.valid:
        # MAP02's most dangerous tabled monster is its Demon (100 XP).
        bd.assert_true(ref.class_name == "Demon",
                       "the deterministic Warden pick is MAP02's Demon")
        bd.assert_true(ref.health
                       == int(150 * systems.warden_health_mult()),
                       "the Warden's health is empowered (depth-scaled)")
        bd.assert_true(tuple(ref.tint) == tuple(content.WARDEN_TINT),
                       "the Warden wears the gold tint")
        expected = systems.pick_warden(systems._live_hostiles())
        bd.assert_true(expected == ref,
                       "re-running the census pick returns the same Warden")
    quest = bd_quests.log.get(content.contract_quest_id("MAP02"))
    bd.assert_true(quest is not None
                   and quest.state == bd_quests.Quest.ACTIVE
                   and quest.parallel,
                   "the contract quest started (parallel objectives)")
    if quest is not None:
        tribute = quest.objective("tribute")
        warden = quest.objective("warden")
        bd.assert_true(tribute is not None and tribute.count == goal
                       and not tribute.done and warden is not None
                       and not warden.done,
                       "both contract objectives are open")
    saved = bd.state.get(content.CONTRACT_STATE_KEY)
    bd.assert_true(isinstance(saved, dict)
                   and saved.get("map") == "MAP02"
                   and saved.get("warden_tid") == tid
                   and saved.get("tribute_goal") == goal,
                   "the contract mirror persists in bd.state")
    strip = systems.refresh_strip()
    bd.assert_true("Contract: tribute 0/" in strip["line2"]
                   and "Warden alive" in strip["line2"],
                   "the strip carries the contract segment")
    # The deterministic modifier: the same seed function arms it.
    expected_modifier = systems.roll_modifier("MAP02")
    modifier = systems.contract_state.get("modifier")
    bd.assert_true(isinstance(modifier, dict)
                   and modifier.get("id") == expected_modifier.get("id"),
                   "the armed modifier matches the deterministic roll")
    bd.assert_true(systems.delve_depth() == 0,
                   "no contracts completed before this one (depth 0)")
    bd.assert_true("Depth 0" in strip["line2"],
                   "the strip carries the depth")


def autotest_dice():
    result = bd_dnd.roll("2d6+3", rng=_Roller([4, 2]))
    bd.assert_true(result["rolls"] == [4, 2] and result["modifier"] == 3
                   and result["total"] == 9 and result["notation"] == "2d6+3",
                   "roll('2d6+3') structure and total")
    adv = bd_dnd.d20(mod=3, advantage=True, rng=_Roller([18, 6]))
    bd.assert_true(adv["kept"] == 18 and adv["discarded"] == 6
                   and adv["total"] == 21,
                   "advantage keeps the higher die")
    dis = bd_dnd.d20(mod=3, disadvantage=True, rng=_Roller([18, 6]))
    bd.assert_true(dis["kept"] == 6 and dis["discarded"] == 18,
                   "disadvantage keeps the lower die")
    bd.assert_true(bd_dnd.d20(rng=_Roller([20]))["critical"] == "hit",
                   "natural 20 is a critical hit")
    bd.assert_true(bd_dnd.d20(rng=_Roller([1]))["critical"] == "miss",
                   "natural 1 is a critical miss")
    bd.assert_true(raises_value_error(lambda: bd_dnd.roll("2x6")),
                   "malformed notation raises ValueError")
    s1, s2 = bd.rng(42), bd.rng(42)
    bd.assert_true([s1.int(1, 20) for _ in range(8)]
                   == [s2.int(1, 20) for _ in range(8)],
                   "twin rng streams produce identical sequences")


def autotest_abilities():
    expected = {1: -5, 3: -4, 8: -1, 9: -1, 10: 0, 11: 0, 12: 1,
                15: 2, 20: 5}
    for score, mod in expected.items():
        bd.assert_true(bd_dnd.modifier(score) == mod,
                       f"modifier({score}) == {mod}")
    hero = systems.hero
    bd.assert_true(hero.abilities.mod("str") == 2
                   and hero.abilities.mod("dex") == 1
                   and hero.abilities.mod("con") == 2
                   and hero.abilities.mod("cha") == -1,
                   "delver ability modifiers")
    prof = {1: 2, 4: 2, 5: 3, 8: 3, 9: 4, 12: 4, 13: 5, 16: 5, 17: 6}
    for level, bonus in prof.items():
        bd.assert_true(bd_dnd.proficiency_bonus(level) == bonus,
                       f"proficiency_bonus({level}) == {bonus}")
    bd.assert_true(bd_dnd.SKILLS["athletics"] == "str"
                   and bd_dnd.SKILLS["sleight_of_hand"] == "dex"
                   and bd_dnd.SKILLS["religion"] == "int"
                   and bd_dnd.SKILLS["insight"] == "wis",
                   "skill -> ability table")


def autotest_checks():
    hero = bd_dnd.Character(
        "Test", bd_dnd.AbilityScores(str=16, wis=14),
        proficient_skills=("athletics",), proficient_saves=("str",))
    result = hero.skill_check("athletics", 15, rng=_Roller([14]))
    bd.assert_true(result["success"] and result["total"] == 19
                   and result["ability_mod"] == 3 and result["prof"] == 2,
                   "skill check success branch math")
    result = hero.skill_check("perception", 15, rng=_Roller([4]))
    bd.assert_true(not result["success"] and result["total"] == 6
                   and result["prof"] == 0,
                   "skill check failure branch, unproficient")
    result = hero.skill_check("athletics", 30, rng=_Roller([20]))
    bd.assert_true(result["critical"] == "hit" and not result["success"],
                   "critical hit reported even when the total misses")
    result = hero.saving_throw("str", 10, rng=_Roller([5]))
    bd.assert_true(result["prof"] == 2 and result["success"],
                   "proficient save adds proficiency")
    bd.assert_true(raises_value_error(
        lambda: hero.skill_check("card_counting", 10)),
        "unknown skill raises ValueError")
    bd.assert_true(len(hero.roll_log) == 4,
                   "every check appends to the roll log")


def autotest_xp_levels():
    hero = bd_dnd.Character("Test2", bd_dnd.AbilityScores(con=14),
                            hit_die=10)
    fired = []
    hero.on_level_up.append(lambda c, lvl: fired.append(lvl))
    events = hero.award_xp(299)
    bd.assert_true(events == [] and hero.level == 1,
                   "299 XP does not level")
    events = hero.award_xp(1)  # 300 exactly: level 2
    bd.assert_true(hero.level == 2 and len(events) == 1 and fired == [2],
                   "300 XP reaches level 2 and fires on_level_up")
    bd.assert_true(hero.max_hp == 20 and hero.hp == 20,
                   "level-up heals by the hit die average plus con mod")
    events = hero.award_xp(1000000)
    bd.assert_true(hero.level == bd_dnd.Character.MAX_LEVEL
                   and hero.proficiency == 6,
                   "XP flood caps at level 20 with +6 proficiency")


def autotest_resources_rest():
    hero = bd_dnd.Character("Test3")
    hero.grant_resource("rage", 2)
    bd.assert_true(hero.use_resource("rage") and hero.use_resource("rage")
                   and not hero.use_resource("rage"),
                   "resource pools spend and refuse")
    hero.restore_resources()
    hero.hp = 4
    outcome = hero.rest(short=True)
    bd.assert_true(outcome["healed"] == 2 and hero.hp == 6,
                   "short rest heals 25% of max hp")
    hero.hp = 1
    outcome = hero.rest(short=False)
    bd.assert_true(outcome["healed"] == hero.max_hp - 1
                   and hero.resources["rage"] == 2,
                   "long rest fully heals and restores resources")


def autotest_kill_credit():
    """Tribute counts player-credited kills only; XP pops over the kill."""
    hero = systems.hero
    quest = bd_quests.log.get(content.contract_quest_id("MAP02"))
    warden_tid = systems.contract_state.get("warden_tid")
    bd.assert_true(hero is not None and quest is not None,
                   "kill credit: fixtures available")
    if hero is None or quest is None:
        return
    victims = []
    for ref in systems._live_hostiles():
        try:
            if int(ref.tid) == int(warden_tid or 0):
                continue
            if systems._xp_for_class(ref.class_name) <= 0:
                continue
            victims.append(ref)
        except Exception:
            continue
    bd.assert_true(len(victims) >= 2, "two tabled census victims available")
    if len(victims) < 2:
        return
    pawn = player_pawn()
    first, second = victims[0], victims[1]
    xp_first = systems._xp_for_class(first.class_name)
    popups_before = len(systems.popup_log)
    first.damage(1000, source=pawn)
    tribute = quest.objective("tribute")
    bd.assert_true(hero.xp == xp_first,
                   "a player-sourced kill pays its XP")
    bd.assert_true(tribute.progress == 1,
                   "a player-sourced kill increments the tribute")
    bd.assert_true(len(systems.popup_log) == popups_before + 1
                   and systems.popup_log[-1]["xp"] == xp_first,
                   "the kill popped its floating XP")
    second.damage(1000)  # source-less: a scripted, uncredited death
    bd.assert_true(hero.xp == xp_first and tribute.progress == 1,
                   "an uncredited death pays nothing and counts nothing")
    bd.assert_true(len(systems.popup_log) == popups_before + 1,
                   "an uncredited death pops nothing")


def autotest_levelup():
    """Level-up fanfare: full heal, the LEVEL beat, the +1 charge feature."""
    hero = systems.hero
    bd.assert_true(hero is not None and hero.level == 1 and hero.xp == 25,
                   "level-up: fixtures available")
    if hero is None:
        return
    pawn = player_pawn()
    # Wound the body first: the hook drains the sheet by ratio (30 * 20
    # / 100 = 6), then the level-up hard-resyncs both pools to full.
    pawn.damage_factor = 1.0
    pawn.damage(30)
    pawn.damage_factor = 0.0
    bd.assert_true(hero.hp == hero.max_hp - int(30 * hero.max_hp / 100
                                              + 0.5),
                   "the pawn wound drained the sheet by ratio")
    hero.set_hp(8)
    events = hero.award_xp(275)  # 300 exactly: level 2
    bd.assert_true(hero.level == 2 and len(events) == 1,
                   "the award reached exactly level 2")
    bd.assert_true(hero.hp == hero.max_hp == 20,
                   "the level-up fanfare healed the sheet fully")
    bd.assert_true(pawn.health == 100,
                   "the level-up hard resync healed the body to full")
    bd.assert_true(systems.levelup_log
                   and systems.levelup_log[-1]["level"] == 2,
                   "the LEVEL 2 beat is on the books")
    bd.assert_true(hero.resource_max.get("second_wind") == 2
                   and hero.resources.get("second_wind") == 2,
                   "the level-2 feature granted the second charge")
    strip = systems.refresh_strip()
    bd.assert_true("[C] Second Wind x2" in strip["line1"],
                   "the strip shows the active and its charges")
    bd.assert_true("Body 100" in strip["line1"],
                   "the strip shows the body's health")


def autotest_door_generic():
    """The dispatcher covers any locked line, not just the MAP02 fixture.

    Synthetic events drive it directly: a failed roll creates the per-line
    check and leaves it armed (and toasts the first-touch hint); a passed
    roll whose activation is refused retires the line into the unbashable
    memory."""
    pawn = player_pawn()
    door_bash = systems.door_bash
    bd.assert_true(pawn is not None and door_bash is not None,
                   "door generic: fixtures available")
    if pawn is None or door_bash is None:
        return
    result = door_bash._dispatch({"reason": "locked", "line_index": 5,
                                  "actor_ref": pawn},
                                 rng=_Roller([1]))  # 1+2+2 = 5 < DC 15
    check = door_bash.checks.get(("MAP02", 5))
    bd.assert_true(result is not None and not result["success"],
                   "the dispatcher rolled the lazily-created check")
    bd.assert_true(check is not None and check.attempts == 1
                   and not check.opened
                   and ("MAP02", 5) not in door_bash.unbashable,
                   "a failed bash stays armed for retries")
    bd.assert_true(any(t["text"] == content.TOAST_BASH_HINT
                       for t in toasts.history),
                   "the first touch of a locked line raised the bash hint")
    result = door_bash._dispatch({"reason": "locked", "line_index": 99999,
                                  "actor_ref": pawn},
                                 rng=_Roller([20]))  # 20+2+2 >= DC 15
    bd.assert_true(result is not None and result["success"],
                   "the second dispatch passed the roll")
    bd.assert_true(("MAP02", 99999) in door_bash.unbashable,
                   "a refused activation retires the line (no spam)")
    before = door_bash.checks[("MAP02", 99999)].attempts
    bd.assert_true(door_bash._dispatch({"reason": "locked",
                                        "line_index": 99999,
                                        "actor_ref": pawn}) is None
                   and door_bash.checks[("MAP02", 99999)].attempts == before,
                   "a retired line never rolls again")
    bd.assert_true(door_bash._dispatch({"reason": "unknown_special",
                                        "line_index": 7,
                                        "actor_ref": pawn}) is None
                   and ("MAP02", 7) not in door_bash.checks,
                   "non-locked failures never create checks")


def autotest_skills_damage():
    """CQB and dead-eye ride the real filter at planted distances.

    The near target (64u) gains Athletics CQB (+2, requires
    proficiency), the far target (700u) gains Perception dead-eye (+1,
    requires proficiency), the mid target (300u) gains only the level
    bonus. Proficiency gating is proven with the Cleric unit (neither
    skill trained)."""
    hero = systems.hero
    handler = systems.level_damage_handler
    pawn = player_pawn()
    bd.assert_true(hero is not None and pawn is not None
                   and handler is not None,
                   "skills damage: fixtures available")
    if hero is None or pawn is None or handler is None:
        return
    base = systems.level_damage_bonus(hero)
    bd.assert_true(base == 1, "level 2 grants +1 level damage")
    cleric = getattr(autotest_unit_founding, "cleric", None)
    bd.assert_true(cleric is not None
                   and systems.hit_bonus_for(cleric, 64.0)
                   == systems.level_damage_bonus(cleric)
                   and systems.hit_bonus_for(cleric, 700.0)
                   == systems.level_damage_bonus(cleric),
                   "untrained skills grant no CQB or dead-eye")
    bd.assert_true(systems.hit_bonus_for(hero, 64.0) == base
                   + content.CQB_BONUS
                   and systems.hit_bonus_for(hero, 300.0) == base
                   and systems.hit_bonus_for(hero, 700.0) == base
                   + content.DEADEYE_BONUS,
                   "hit_bonus_for distances map to the right perks")
    planted = []
    for dx in (64.0, 300.0, 700.0):
        try:
            ref = bd.spawn("ZombieMan", pawn.x + dx, pawn.y, pawn.z,
                           force=True)
            planted.append((ref, dx))
        except Exception as exc:
            bd.warn(f"delve: skills target spawn failed: {exc!r}")
    for ref, dx in planted:
        event = {"actor_ref": ref, "attacker_ref": pawn,
                 "attacker_player_index": 0, "damage": 10}
        handler(event)
        bd.assert_true(event["damage"] == 10
                       + systems.hit_bonus_for(hero, dx),
                       f"the filter applied the {int(dx)}u bonus")
    for ref, _dx in planted:
        try:
            ref.destroy()
        except Exception:
            pass


def autotest_door_holds():
    """Phase A: an unreachable DC 30 bash always fails; door stays shut.

    The MAP02 line-111 red door is the autotest's real locked-door
    fixture (content.DOOR_LINE, clearly commented there); the rules layer
    itself keys on no map."""
    global door_check, door_drive_until
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "player pawn available")
    if pawn is None:
        return
    # Pre-register the fixture through the dispatcher (the same object
    # interactive play would create lazily on the first use).
    door_check = systems.door_bash.bashable_door(content.DOOR_LINE)
    bd.assert_true(door_check is not None,
                   "the line-111 check registered")
    door_check.dc = 30  # best possible total is 20+2+2 = 24
    pawn.set_position(*content.DOOR_APPROACH)
    pawn.angle = content.DOOR_FACE_ANGLE
    door_drive_until = bd.level_time() + 40


def autotest_door_bash():
    """Phase B: a trivial DC 5 bash always succeeds; the door opens."""
    global door_drive_until
    bd.assert_true(door_check.attempts >= 1,
                   "the real +use path reached the door check")
    bd.assert_true(door_check.last_result is not None
                   and not door_check.last_result["success"],
                   "DC 30 bash failed")
    bd.assert_true(not door_check.opened
                   and bd.sector(content.DOOR_TRACK_SECTOR).ceiling_height
                   == 48.0,
                   "the door holds fast on a failed bash")
    pawn = player_pawn()
    if pawn is None:
        return
    door_check.dc = 5  # worst possible total is 1+2+2 = 5
    pawn.set_position(*content.DOOR_APPROACH)
    pawn.angle = content.DOOR_FACE_ANGLE
    door_drive_until = bd.level_time() + 40


def autotest_door_asserts():
    global door_drive_until
    door_drive_until = -1
    bd.assert_true(door_check.opened, "DC 5 bash opened the door")
    bd.assert_true(door_check.last_result is not None
                   and door_check.last_result["success"]
                   and door_check.last_result["skill"] == "athletics",
                   "the bash was an athletics check")
    bd.assert_true(bd.sector(content.DOOR_TRACK_SECTOR).ceiling_height
                   > 48.0,
                   "the door track sector is rising/open")
    pawn = player_pawn()
    if pawn is not None:
        bd.assert_true(pawn.inventory_count("RedCard") == 0,
                       "the lent red key was reclaimed after activation")
    labels = [entry["label"] for entry in systems.check_log]
    bd.assert_true(labels.count("Athletics") >= 2,
                   "both real bash rolls were announced on the visible d20")


def autotest_reflex_save():
    """The announced reflex save: a scripted hit is refunded on a save,
    the roll lands in check_log, and the cooldown gates re-rolls."""
    pawn = player_pawn()
    rule = systems.reflex_save
    bd.assert_true(pawn is not None and rule is not None,
                   "reflex save: fixtures available")
    if pawn is None or rule is None:
        return
    # Re-arm for this stage (the autotest disarmed the live rule at
    # founding), and pin the DC to 1 for a guaranteed save (the worst
    # possible DEX total is 1 + 1 = 2 >= 1); the live 12 would make the
    # branch depend on the stream. Disarmed again at the end.
    rule.armed = True
    rule.cooldown_tics = content.SAVE_COOLDOWN_TICS
    rule.dc = 1
    pawn.damage_factor = 1.0
    health_before = pawn.health
    pawn.damage(10)  # source-less scripted hit, damage_type "None"
    bd.assert_true(pawn.health == health_before - 5,
                   "successful reflex save refunds half (10 -> 5 kept)")
    result = rule.last_result
    bd.assert_true(result is not None and result["save"]["success"]
                   and result["refunded"] == 5 and not result["negated"],
                   "the refund outcome is on the books")
    checks = [entry for entry in systems.check_log
              if entry["label"] == "Reflex save"]
    bd.assert_true(checks and checks[-1]["success"],
                   "the reflex roll was announced on the visible d20")
    rule.cooldown_tics = 10000
    health_before = pawn.health
    pawn.damage(10)
    bd.assert_true(pawn.health == health_before - 10,
                   "cooldown suppresses the follow-up save (full damage)")
    # Natural 20 negates the whole hit (direct call, scripted roller).
    crit_rule = systems.ReflexSaveRule(systems.hero, dc=30, ability="dex")
    outcome = crit_rule.apply(pawn, 8, damage_type="Fire", rng=_Roller([20]))
    bd.assert_true(outcome["negated"] and outcome["refunded"] == 8
                   and outcome["save"]["critical"] == "hit",
                   "natural 20 negates the hit entirely")
    crit_rule.disarm()
    # The engine cannot unregister event handlers: the autotest retires
    # the live rule here so later scripted wounds land exactly.
    rule.dc = content.SAVE_DC
    rule.disarm()
    pawn.damage_factor = 0.0


def autotest_health_sync_damage():
    """Health unification, damage direction: the pawn wound removes sheet
    hp by ratio (actor_damaged hook), immediately."""
    hero = systems.hero
    pawn = player_pawn()
    bd.assert_true(hero is not None and pawn is not None,
                   "health sync: fixtures available")
    if hero is None or pawn is None:
        return
    pawn.set_position(1104.0, 1696.0, 0.0, check=False)
    hero.set_hp(hero.max_hp)  # a known pool: 20
    autotest_health_sync_damage.h0 = hero.hp
    pawn.damage_factor = 1.0
    if pawn.health < 100:
        pawn.heal(100)  # normalize after the reflex stage's wounds
    pawn.damage(10)
    pawn.damage_factor = 0.0
    bd.assert_true(pawn.health == 90, "the pawn took the wound")
    bd.assert_true(hero.hp == autotest_health_sync_damage.h0 - 2,
                   "the sheet lost the ratio share (10 * 20 / 100 = 2)")
    # A pawn-side gain the diff detector has not baselined: heal 10.
    pawn.heal(10)


def autotest_health_sync_gain():
    """The diff detector credited the medikit-style gain to the sheet by
    ratio, and the RPG-side heal moved the pawn to the ratio."""
    hero = systems.hero
    pawn = player_pawn()
    bd.assert_true(hero is not None and pawn is not None,
                   "health sync gain: fixtures available")
    if hero is None or pawn is None:
        return
    h0 = autotest_health_sync_damage.h0
    bd.assert_true(pawn.health == 100, "the pawn healed to full")
    bd.assert_true(hero.hp == h0,
                   "the sheet gained the ratio share back (10 * 20 / 100)")
    # Wound again (the hook drains the sheet by ratio), then the RPG-side
    # heal lifts the body back UP toward the sheet's new ratio.
    pawn.damage_factor = 1.0
    pawn.damage(30)
    pawn.damage_factor = 0.0
    bd.assert_true(hero.hp == h0 - int(30 * hero.max_hp / 100 + 0.5),
                   "the second wound drained the sheet by ratio")
    healed = systems.apply_sheet_heal(hero, 4, pawn=pawn)
    bd.assert_true(healed == 4, "the RPG heal raised the sheet by 4")
    target = min(100, int(hero.hp / hero.max_hp * 100 + 0.5))
    bd.assert_true(pawn.health == target,
                   "the body healed UP toward the sheet's ratio")


def autotest_trap_sense():
    """Perception trap sense: the first damaging-sector entry per map
    rolls an announced Perception check."""
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "trap sense: pawn available")
    if pawn is None:
        return
    before = systems.trap_sense_state["rolls"]
    damaging = None
    try:
        for sector in bd.sectors():
            try:
                if int(sector.damage) > 0:
                    damaging = sector
                    break
            except Exception:
                continue
    except Exception:
        damaging = None
    # The check is driven with a synthetic sector_entered event over a
    # temporarily damaging sector, same as the framework's own unit
    # style. MAP02 carries no damaging sectors, so the scan above is
    # expected to find none here; the synthetic drive keeps the stage
    # deterministic on any map.
    autotest_trap_sense.found_real = damaging is not None
    sector = bd.sector(0)
    old_damage = int(sector.damage)
    sector.damage = 10
    systems.trap_sense_check({"sector": 0, "tags": [], "player_index": 0,
                              "actor_ref": pawn})
    sector.damage = old_damage
    bd.assert_true(systems.trap_sense_state["rolls"] == before + 1,
                   "the trap sense rolled on the damaging sector")
    checks = [entry for entry in systems.check_log
              if entry["label"] == "Perception (trap sense)"]
    bd.assert_true(checks and checks[-1]["dc"] == content.TRAP_SENSE_DC,
                   "the trap sense was announced on the visible d20")


def autotest_religion_sanctuary():
    """Religion proficiency: the sanctuary threshold drops to 140."""
    cleric = getattr(autotest_unit_founding, "cleric", None)
    pawn = player_pawn()
    bd.assert_true(cleric is not None and pawn is not None,
                   "religion sanctuary: fixtures available")
    if cleric is None or pawn is None:
        return
    systems.reset_rest_cooldown()
    threshold = systems.sanctuary_threshold(cleric)
    bd.assert_true(threshold
                   == content.RELIGION_SANCTUARY_LIGHT
                   + (content.DARK_DELVE_SANCTUARY_DELTA
                      if systems._modifier_id() == "dark_delve" else 0),
                   "the religion threshold is 140 (plus any DARK DELVE)")
    sector = bd.sector_at(pawn.x, pawn.y)
    sector.light = threshold  # exactly on the threshold: sanctuary
    outcome = systems.try_long_rest(cleric_unit_ref(), rng=_Roller([1]))
    bd.assert_true(outcome["kind"] == "sanctuary"
                   and outcome.get("threshold") == threshold,
                   "religion proficiency lowered the sanctuary threshold")
    systems.reset_rest_cooldown()


def autotest_religion_nightmare():
    """Religion proficiency: the nightmare save rolls WIS, not DEX."""
    cleric = getattr(autotest_unit_founding, "cleric", None)
    pawn = player_pawn()
    bd.assert_true(cleric is not None and pawn is not None,
                   "religion nightmare: fixtures available")
    if cleric is None or pawn is None:
        return
    systems.reset_rest_cooldown()
    bd.assert_true(systems.nightmare_ability(cleric) == "wis",
                   "faith wards the dark with WIS")
    bd.assert_true(systems.nightmare_ability(systems.hero) == "dex",
                   "the untrained hero rolls DEX")
    sector = bd.sector_at(pawn.x, pawn.y)
    sector.light = 40  # forced darkness
    # Cleric WIS 15 -> mod +2: 15 + 2 = 17 clears even a DARK DELVE DC.
    outcome = systems.try_long_rest(cleric, rng=_Roller([15]))
    bd.assert_true(outcome["kind"] == "fitful"
                   and outcome.get("ability") == "wis",
                   "the religion nightmare save rolled WIS")
    checks = [entry for entry in systems.check_log
              if entry["label"] == "Nightmare save (WIS)"]
    bd.assert_true(checks and checks[-1]["success"],
                   "the WIS nightmare save was announced by name")
    systems.reset_rest_cooldown()


def cleric_unit_ref():
    return getattr(autotest_unit_founding, "cleric", None)


def autotest_examine():
    """Insight examine: the crosshair probe captures name and HP; the XP
    value and threat note stay Insight-gated."""
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "examine: pawn available")
    if pawn is None:
        return
    target = None
    try:
        target = bd.spawn("ZombieMan", pawn.x + 150.0, pawn.y, pawn.z,
                          force=True)  # same sector: clean trace line
    except Exception as exc:
        bd.warn(f"delve: examine target spawn failed: {exc!r}")
    bd.assert_true(target is not None, "examine: target planted")
    if target is None:
        return
    pawn.angle = 0.0  # face east, dead at the planted zombie
    systems._examine_probe_tick()  # the same body the 7-tic task runs
    bd.assert_true(systems.examine_state["class_name"].lower()
                   == "zombieman",
                   "the probe captured the crosshair monster's class")
    bd.assert_true(systems.examine_state["hp"] == 20
                   and systems.examine_state["max_hp"] == 20,
                   "the probe captured its HP")
    bd.assert_true(systems.examine_state["xp"] is None,
                   "no Insight: no XP value for the Fighter")
    bd.assert_true("zombieman  hp 20/20"
                   in systems.examine_state["line"].lower(),
                   "the examine line shows name and HP")
    cleric = cleric_unit_ref()
    graded = systems.describe_examine(cleric, "Demon")
    bd.assert_true(graded["xp"] == 100 and graded["deadly"],
                   "Insight shows the XP value and flags the deadly "
                   "(100 >= 4 * 25 * level 1)")
    graded = systems.describe_examine(cleric, "ZombieMan")
    bd.assert_true(graded["xp"] == 25 and not graded["deadly"],
                   "Insight reads small game as small")
    try:
        target.destroy()
    except Exception:
        pass
    systems._examine_probe_tick()  # clear the line for later asserts
    bd.assert_true(systems.examine_state["line"] == "",
                   "the examine line clears when the crosshair empties")


def autotest_rest_sanctuary():
    """Sanctuary: light >= 160 grants the full rest, sheet and pawn."""
    hero = systems.hero
    pawn = player_pawn()
    bd.assert_true(hero is not None and pawn is not None,
                   "sanctuary: fixtures available")
    if hero is None or pawn is None:
        return
    pawn.set_position(1104.0, 1696.0, 0.0, check=False)  # untagged sector 0
    sector = bd.sector_at(pawn.x, pawn.y)
    sector.light = 200  # forced sanctuary light
    hero.set_hp(10)
    pawn.damage_factor = 1.0
    pawn.damage(30)
    pawn.damage_factor = 0.0
    bd.assert_true(pawn.health < 100, "the pawn carries a real wound")
    bd.assert_true(hero.hp == 10 - int(30 * hero.max_hp / 100 + 0.5),
                   "the wound drained the sheet by ratio")
    outcome = systems.try_long_rest(hero)
    bd.assert_true(outcome["kind"] == "sanctuary"
                   and hero.hp == hero.max_hp
                   and hero.resources.get("second_wind") == 2,
                   "sanctuary branch: full heal and charges restored")
    bd.assert_true(pawn.health == 100,
                   "sanctuary rest heals the body to the full ratio")
    bd.assert_true(any(t["text"] == content.TOAST_SANCTUARY
                       for t in toasts.history),
                   "sanctuary rest raised its toast")
    bd.assert_true("Rest: sanctuary" in systems.refresh_strip()["line2"],
                   "the strip forecasts sanctuary under forced light")


def autotest_rest_cooldown():
    """A second rest inside the cooldown says so and does nothing."""
    outcome = systems.try_long_rest(systems.hero)
    bd.assert_true(outcome["kind"] == "cooldown"
                   and outcome.get("remaining", 0) > 0,
                   "the cooldown refuses the second rest")
    bd.assert_true(any(t["text"] == content.TOAST_REST_COOLDOWN
                       for t in toasts.history),
                   "the cooldown says so")
    systems.reset_rest_cooldown()  # test hook: the branches need fresh rests


def autotest_rest_fitful():
    """Dark, save made: the fitful rest grants half the missing HP."""
    hero = systems.hero
    pawn = player_pawn()
    sector = bd.sector_at(pawn.x, pawn.y)
    sector.light = 40  # forced darkness
    hero.set_hp(10)  # max_hp 20: 10 missing
    pawn.damage_factor = 1.0
    pawn.damage(20)  # sheet drains by ratio: 20 * 20 / 100 = 4
    pawn.damage_factor = 0.0
    bd.assert_true(hero.hp == 10 - 4,
                   "the wound drained the sheet by ratio (4)")
    bd.assert_true(pawn.health == 80, "the pawn carries the wound")
    # Scripted roller: 15 + dex mod 1 = 16 >= DC 12 -> the sleeper wakes.
    outcome = systems.try_long_rest(hero, rng=_Roller([15]))
    bd.assert_true(outcome["kind"] == "fitful" and outcome["healed"] == 7
                   and hero.hp == 13,
                   "fitful branch: half the missing HP (14 / 2 = 7)")
    # The sheet sits at 65% while the body sits at 80%: the RPG side
    # heals the body upward only, so nothing moves.
    bd.assert_true(outcome.get("pawn_healed") == 0
                   and pawn.health == 80,
                   "the fitful heal leaves the healthier body alone")
    checks = [entry for entry in systems.check_log
              if entry["label"] == "Nightmare save (DEX)"]
    bd.assert_true(checks and checks[-1]["success"],
                   "the nightmare save was announced (DEX, success)")
    bd.assert_true("Rest: the dark dreams"
                   in systems.refresh_strip()["line2"],
                   "the strip forecasts the dark dreams under darkness")
    systems.reset_rest_cooldown()


def autotest_rest_nightmare():
    """Dark, save failed: THE NIGHTMARE MADE REAL spawns hostile demons."""
    hero = systems.hero
    pawn = player_pawn()
    sector = bd.sector_at(pawn.x, pawn.y)
    sector.light = 40  # still dark (the religion stages may have moved it)
    systems.reset_rest_cooldown()
    quest = bd_quests.log.get(content.contract_quest_id("MAP02"))
    tribute_before = (quest.objective("tribute").progress
                      if quest is not None else -1)
    hero.set_hp(10)
    # Scripted roller: 1 + 1 = 2 < DC 12 (fail), then 2 demons (1..2).
    outcome = systems.try_long_rest(hero, rng=_Roller([1, 2]))
    bd.assert_true(outcome["kind"] == "nightmare"
                   and not outcome["save"]["success"]
                   and outcome["healed"] == 0,
                   "nightmare branch: failed save heals nothing")
    bd.assert_true(outcome.get("spawned") == 2,
                   "the nightmare made real spawned two demons")
    bd.assert_true(len(systems.nightmare_log) == 2,
                   "the nightmare tids are on the books")
    for tid in systems.nightmare_log:
        ref = bd.actor_ref(tid)
        bd.assert_true(ref is not None and ref.valid and ref.alive
                       and ref.is_monster,
                       "a nightmare demon exists in the world")
        if ref is not None and ref.valid:
            bd.assert_true(not ref.get_flag("FRIENDLY"),
                           "the nightmare is hostile (not FRIENDLY)")
            bd.assert_true(tuple(ref.tint)
                           == tuple(content.NIGHTMARE_TINT),
                           "the nightmare wears the dark tint")
            ref.destroy()  # cleanup: no death event, no tribute credit
    if quest is not None:
        bd.assert_true(quest.objective("tribute").progress == tribute_before,
                       "the nightmare cleanup touched no tribute count")
    bd.assert_true(any(t["text"] == content.TOAST_NIGHTMARE
                       for t in toasts.history),
                   "the nightmare raised its toast")
    checks = [entry for entry in systems.check_log
              if entry["label"] == "Nightmare save (DEX)"]
    bd.assert_true(checks and not checks[-1]["success"],
                   "the nightmare save was announced (DEX, failure)")
    systems.reset_rest_cooldown()


def autotest_active_second_wind_press():
    """Second Wind through a real Custom Action 3 press."""
    hero = systems.hero
    pawn = player_pawn()
    bd.assert_true(hero is not None and pawn is not None,
                   "second wind: fixtures available")
    if hero is None or pawn is None:
        return
    hero.set_hp(10)
    pawn.damage_factor = 1.0
    pawn.damage(40)  # body 60; the sheet drains by ratio to 2
    pawn.damage_factor = 0.0
    bd.assert_true(hero.hp == 10 - int(40 * hero.max_hp / 100 + 0.5),
                   "the wound drained the sheet by ratio")
    autotest_active_second_wind_press.wounded_health = pawn.health
    autotest_active_second_wind_press.sheet_before = hero.hp
    bd.assert_true(hero.resources.get("second_wind") == 2,
                   "two charges after the sanctuary rest")
    bd.set_custom_action(3, True)


def autotest_active_second_wind_asserts():
    hero = systems.hero
    pawn = player_pawn()
    bd.assert_true((3, True) in action_log,
                   "the class-active press fired {'action': 3, pressed: True}")
    outcome = last_active_outcome or {}
    bd.assert_true(outcome.get("ok") and outcome.get("id") == "second_wind",
                   "the press spent a Second Wind charge")
    amount = outcome.get("amount", 0)
    bd.assert_true(3 <= amount <= 12,
                   "the heal rolled d10 + level 2 (3..12)")
    sheet_before = getattr(autotest_active_second_wind_press,
                           "sheet_before", None)
    if sheet_before is None:
        sheet_before = 2  # 10 - 8 (the ratio drain), see the press stage
    expected_sheet = min(hero.max_hp, sheet_before + amount)
    bd.assert_true(hero.hp == expected_sheet,
                   "the sheet healed by the roll (clamped)")
    wounded = getattr(autotest_active_second_wind_press,
                      "wounded_health", None)
    if pawn is not None and wounded is not None:
        # The body heals toward the sheet's new ratio, not by the roll.
        target = min(100, int(hero.hp / hero.max_hp * 100 + 0.5))
        bd.assert_true(pawn.health == max(wounded, target),
                       "the body healed to the sheet's ratio")
    bd.assert_true(hero.resources.get("second_wind") == 1,
                   "one charge remains")
    bd.set_custom_action(3, False)
    # The direct path: second charge spends, the third refuses politely.
    hero.set_hp(5)
    second = systems.use_class_active()
    bd.assert_true(second.get("ok") and hero.resources.get("second_wind") == 0,
                   "the direct path spent the last charge")
    third = systems.use_class_active()
    bd.assert_true(not third.get("ok")
                   and third.get("reason") == "exhausted"
                   and "rest" in third.get("message", "").lower(),
                   "an empty active names the refill (the rest action)")


def autotest_active_blur():
    """Uncanny Dodge (unit Rogue): 175 tics at damage_factor 0.35."""
    rogue = getattr(autotest_unit_founding, "rogue", None)
    pawn = player_pawn()
    bd.assert_true(rogue is not None and pawn is not None,
                   "blur: fixtures available")
    if rogue is None or pawn is None:
        return
    pawn.damage_factor = 1.0
    health_before = pawn.health
    outcome = systems.use_class_active(rogue)
    bd.assert_true(outcome.get("ok") and outcome.get("id") == "uncanny_dodge"
                   and pawn.damage_factor == content.UNCANNY_DODGE_FACTOR,
                   "the blur set the damage factor")
    dealt = pawn.damage(20)
    bd.assert_true(dealt == int(20 * content.UNCANNY_DODGE_FACTOR)
                   and pawn.health == health_before - dealt,
                   "the blur shrinks the hit to about a third")


def autotest_active_turn():
    """Turn the Unholy (unit Cleric): the burst burns what surrounds."""
    cleric = getattr(autotest_unit_founding, "cleric", None)
    pawn = player_pawn()
    bd.assert_true(cleric is not None and pawn is not None,
                   "turn: fixtures available")
    if cleric is None or pawn is None:
        return
    target = None
    try:
        # Point-blank (probe-verified: GZDoom's radius falloff subtracts
        # the target's radius, so len clamps to 0 here and the burst
        # deals its full damage).
        target = bd.spawn("ZombieMan", pawn.x + 20.0, pawn.y, pawn.z,
                          force=True)
    except Exception as exc:
        bd.warn(f"delve: turn target spawn failed: {exc!r}")
    bd.assert_true(target is not None, "turn: target spawned")
    if target is None:
        return
    outcome = systems.use_class_active(cleric)
    expected = content.TURN_BASE_DAMAGE + 2 * cleric.level
    bd.assert_true(outcome.get("ok") and outcome.get("damage") == expected,
                   "the burst rolled its damage")
    # The pawn is the burst's source, so the full hit bonus (level +1,
    # Athletics CQB +2 at 20u) rides the actor_before_damage filter onto
    # every monster hit, this one included.
    expected += systems.hit_bonus_for(systems.hero, 20.0)
    bd.assert_true(target.valid and target.health == 20 - expected,
                   "the burst burned the surround (level and CQB bonuses "
                   "included)")
    try:
        target.destroy()  # survives wounded; no death event, no credit
    except Exception:
        pass


def autotest_blur_restored():
    pawn = player_pawn()
    bd.assert_true(pawn is not None
                   and pawn.damage_factor == 1.0,
                   "the blur expired back to the prior damage factor")
    if pawn is not None:
        pawn.damage_factor = 0.0


def autotest_companion_damage():
    """Damage sync, both directions: actor hits lower member.hp;
    member-side set_hp heals the actor through on_hp_changed."""
    member = systems.party.get(content.HOUND_NAME)
    ref = systems.companion.actor()
    bd.assert_true(member is not None and ref is not None and ref.valid
                   and ref.alive, "companion fixture available")
    if member is None or ref is None:
        return
    ref.damage_factor = 1.0
    hp_before = member.hp
    dealt = ref.damage(3)  # source-less scripted hit on the hound
    ref.damage_factor = 0.0
    bd.assert_true(dealt == 3 and member.hp == hp_before - 3,
                   "companion damage lowers the member's RPG hp by 3")
    bd.assert_true(ref.health == member.hp,
                   "actor health and member hp stay in sync after damage")
    member.set_hp(member.max_hp)  # RPG-side heal: the hook heals the actor
    bd.assert_true(ref.health == member.hp == member.max_hp,
                   "member-side set_hp heals the companion actor")


def autotest_companion_stuck_setup():
    """Park the hound across geometry it cannot traverse: the pawn waits
    on the corridor floor past the bashed red door (820, 1236, floor 48,
    a probed fit), the hound goes into the light-96 water hall below
    (500, 1100, floor -88). The 136-unit wall between them is not
    climbable, and 348 units of distance is inside teleport_distance, so
    only the framework's stuck-teleport (STUCK_TELEPORT_TICS of no
    progress) can close the gap."""
    pawn = player_pawn()
    ref = systems.companion.actor()
    bd.assert_true(pawn is not None and ref is not None and ref.valid
                   and ref.alive, "stuck fixture available")
    if pawn is None or ref is None:
        return
    # Surgical cleanup (source-less: no tribute, no XP): the west hall's
    # shotgunners would shred the parked hound, and the corridor's
    # stragglers would pick a fight with it instead of letting it follow.
    # The crowned Warden is off limits: the contract needs it alive.
    warden_tid = systems.contract_state.get("warden_tid")
    for hostile in systems._live_hostiles():
        try:
            if int(hostile.tid) == int(warden_tid or 0):
                continue
            if hostile.distance_to(pawn) < 300.0:
                hostile.damage(10000)
                continue
            hx, hy = hostile.x, hostile.y
            if ((hx - 500.0) ** 2 + (hy - 1100.0) ** 2) ** 0.5 < 600.0:
                hostile.damage(10000)
        except Exception:
            pass
    pawn.set_position(820.0, 1236.0, 48.0, check=False)
    try:
        ref.target = None  # no grudges: the follow loop owns the test
    except Exception:
        pass
    ref.set_position(500.0, 1100.0, -88.0, check=False)
    parked = ref.distance_to(pawn)
    autotest_companion_stuck_setup.parked_distance = parked
    bd.assert_true(200.0 < parked < systems.companion.teleport_distance,
                   "the hound is parked 200+ units out across the drop, "
                   "within teleport distance")


def autotest_companion_stuck_assert():
    pawn = player_pawn()
    ref = systems.companion.actor()
    bd.assert_true(pawn is not None and ref is not None and ref.valid
                   and ref.alive, "stuck assert fixture available")
    if pawn is None or ref is None:
        return
    bd.assert_true(ref.distance_to(pawn) <= systems.companion.follow_distance,
                   "the stuck hound teleported up from the water hall "
                   "within ~10 follow ticks")


def autotest_companion_teleport():
    """Jump the player across the map; the hound must catch up."""
    pawn = player_pawn()
    ref = systems.companion.actor()
    bd.assert_true(pawn is not None and ref is not None and ref.valid,
                   "teleport fixture available")
    if pawn is None or ref is None:
        return
    bd.assert_true(ref.distance_to(pawn) < systems.companion.teleport_distance,
                   "hound near the player before the jump")
    # ~800 units away: beyond teleport_distance (512), far beyond what
    # the follow thrust could cover before the check.
    pawn.set_position(1104.0, 1984.0, 0.0, check=False)  # the start room


def autotest_companion_teleport_check():
    pawn = player_pawn()
    ref = systems.companion.actor()
    bd.assert_true(pawn is not None and ref is not None and ref.valid
                   and ref.alive, "teleport check fixture available")
    if pawn is None or ref is None:
        return
    bd.assert_true(ref.distance_to(pawn) < 256.0,
                   "hound teleported to catch up within 25 tics")


def autotest_companion_kill():
    """Fatal damage: the hound dies, the member drops to hp 0, and the
    guild knell tolls (harm toast + knight/death, DSKNTDTH)."""
    member = systems.party.get(content.HOUND_NAME)
    ref = systems.companion.actor()
    bd.assert_true(member is not None and ref is not None and ref.valid,
                   "kill fixture available")
    if member is None or ref is None:
        return
    autotest_companion_kill.old_tid = systems.companion.tid
    ref.damage_factor = 1.0
    ref.damage(10000)
    bd.assert_true(systems.companion.dead,
                   "hound marked dead after the fatal hit")
    bd.assert_true(member.hp == 0,
                   "hound death incapacitates the member (hp 0)")
    bd.assert_true(systems.companion_died_log == [content.HOUND_NAME],
                   "on_companion_died fired with the member")
    bd.assert_true(any(t["kind"] == "harm"
                       and t["text"] == content.knell_toast(
                           content.HOUND_NAME)
                       for t in toasts.history),
                   "hound death raised the harm knell toast")


def autotest_companion_revive():
    member = systems.party.get(content.HOUND_NAME)
    bd.assert_true(systems.companion.revive(),
                   "revive() respawned the hound")
    bd.assert_true(not systems.companion.dead
                   and member.hp == max(1, (member.max_hp + 1) // 2),
                   "revive restores half of max hp (rounded up)")
    ref = systems.companion.actor()
    if ref is not None and ref.valid:
        ref.damage_factor = 0.0  # fixture is invulnerable again


def autotest_companion_revive_check():
    member = systems.party.get(content.HOUND_NAME)
    ref = systems.companion.actor()
    bd.assert_true(ref is not None and ref.valid and ref.alive,
                   "revived hound alive in the world")
    if ref is None or member is None:
        return
    bd.assert_true(systems.companion.tid != autotest_companion_kill.old_tid,
                   "revive allocated a fresh TID (no corpse shadowing)")
    autotest_companion_revive_check.revive_tid = systems.companion.tid
    bd.assert_true(ref.health == member.hp,
                   "revived hound health matches the member's hp")
    bd.assert_true(ref.get_flag("FRIENDLY") and not ref.get_flag("COUNTKILL"),
                   "revived hound keeps its friendly semantics")


def autotest_contract_warden():
    """Slay the Warden: the objective completes on the tid's death, the
    5x bounty pays out (track 1x + contract bonus), the tribute counts it
    too."""
    hero = systems.hero
    quest = bd_quests.log.get(content.contract_quest_id("MAP02"))
    tid = systems.contract_state.get("warden_tid")
    ref = bd.actor_ref(tid) if tid else None
    bd.assert_true(hero is not None and quest is not None
                   and ref is not None and ref.valid and ref.alive,
                   "warden kill: fixtures available")
    if hero is None or quest is None or ref is None or not ref.valid:
        return
    pawn = player_pawn()
    base_xp = systems._xp_for_class(ref.class_name)
    mult = systems.warden_xp_mult()
    xp_before = hero.xp
    progress_before = quest.objective("tribute").progress
    ref.damage(999, source=pawn)
    bd.assert_true(quest.objective("warden").done,
                   "the warden objective completed on the tid's death")
    bd.assert_true(hero.xp == xp_before + base_xp * mult,
                   f"the warden paid {mult}x XP (track 1x plus the "
                   f"contract bonus)")
    bd.assert_true(quest.objective("tribute").progress == progress_before + 1,
                   "the warden's death counts toward the tribute too")
    bd.assert_true(any(entry.get("bonus") and entry["xp"]
                       == base_xp * (mult - 1)
                       for entry in systems.popup_log),
                   "the bounty popped its bonus XP")
    bd.assert_true(quest.state == bd_quests.Quest.ACTIVE,
                   "the quest stays active until the tribute is full")
    autotest_contract_warden.xp_after = hero.xp


def autotest_contract_tribute():
    """Fill the tribute: the contract completes with XP and fanfare."""
    hero = systems.hero
    quest = bd_quests.log.get(content.contract_quest_id("MAP02"))
    bd.assert_true(hero is not None and quest is not None,
                   "tribute fill: fixtures available")
    if hero is None or quest is None:
        return
    pawn = player_pawn()
    tribute = quest.objective("tribute")
    remaining = tribute.count - tribute.progress
    victims = []
    for ref in systems._live_hostiles():
        try:
            if systems._xp_for_class(ref.class_name) <= 0:
                continue
            victims.append(ref)
        except Exception:
            continue
    victims.sort(key=lambda r: (r.class_name, r.x, r.y))
    victims = victims[:remaining]
    bd.assert_true(len(victims) == remaining,
                   "enough live monsters remain to fill the tribute")
    expected_xp = hero.xp + sum(systems._xp_for_class(ref.class_name)
                                for ref in victims)
    # The completion reward as baked at setup (depth and modifier shaped).
    expected_xp += int(quest.rewards.get("xp") or 0)
    for ref in victims:
        ref.damage(999, source=pawn)
    bd.assert_true(quest.state == bd_quests.Quest.COMPLETED,
                   "the full tribute completed the contract")
    bd.assert_true(systems.contract_state.get("done"),
                   "the contract bookkeeping shows it paid")
    bd.assert_true(content.contract_quest_id("MAP02")
                   in systems.fanfare_log,
                   "the completion fanfare fired")
    bd.assert_true(hero.xp == expected_xp,
                   "the tribute kills and the contract XP all landed")
    bd.assert_true(any(t["text"] == content.TOAST_CONTRACT_DONE
                       for t in toasts.history),
                   "the paid-off toast fired")
    strip = systems.refresh_strip()
    bd.assert_true("Warden slain" in strip["line2"],
                   "the strip shows the slain Warden")


def autotest_boons():
    """Level-up boons: three level-ups queue three picks; Toughness and
    Deadly apply through the same function the chooser buttons call;
    one pick stays pending for the checkpoint."""
    hero = systems.hero
    handler = systems.level_damage_handler
    pawn = player_pawn()
    bd.assert_true(hero is not None and handler is not None
                   and pawn is not None, "boons: fixtures available")
    if hero is None or pawn is None:
        return
    pending_before = systems.boon_pending()
    max_before = hero.max_hp
    hero.award_xp(14000 - hero.xp)  # exactly level 6 (three level-ups)
    bd.assert_true(hero.level == 6,
                   "the award carried the hero to level 6")
    bd.assert_true(systems.boon_pending() == pending_before + 3,
                   "each level-up queued a boon pick")
    max_hp_after_levels = hero.max_hp
    bd.assert_true(systems.pick_boon("toughness"),
                   "Toughness picked through the shared pick function")
    bd.assert_true(hero.max_hp == max_hp_after_levels
                   + content.TOUGHNESS_HP,
                   "Toughness raised the sheet max hp")
    bd.assert_true(systems.pick_boon("deadly"),
                   "Deadly picked through the shared pick function")
    bd.assert_true(systems.boon_state.get("deadly") == 1,
                   "the Deadly counter ticks up")
    target = None
    try:
        target = bd.spawn("ZombieMan", pawn.x + 300.0, pawn.y, pawn.z,
                          force=True)
    except Exception as exc:
        bd.warn(f"delve: boon target spawn failed: {exc!r}")
    if target is not None:
        event = {"actor_ref": target, "attacker_ref": pawn,
                 "attacker_player_index": 0, "damage": 10}
        handler(event)
        bd.assert_true(event["damage"] == 10
                       + systems.hit_bonus_for(hero, 300.0)
                       == 10 + systems.level_damage_bonus(hero) + 1,
                       "the Deadly boon stacks through the level filter")
        try:
            target.destroy()
        except Exception:
            pass
    bd.assert_true(systems.boon_pending() == pending_before + 1,
                   "one pick stays pending for the checkpoint")
    bd.assert_true(systems.boon_state.get("taken")
                   == ["toughness", "deadly"],
                   "the taken list records both picks")


def autotest_mid_asserts():
    hero = systems.hero
    bd.assert_true(hero is not None and hero.level == 6,
                   "the run's XP carried the hero to level 6")
    labels = [entry["label"] for entry in systems.check_log]
    bd.assert_true(labels.count("Athletics") >= 2
                   and "Reflex save" in labels
                   and labels.count("Nightmare save (DEX)") >= 2
                   and "Nightmare save (WIS)" in labels
                   and "Perception (trap sense)" in labels,
                   "check_log recorded the door, reflex, rest, religion, "
                   "and trap-sense rolls")
    bd.assert_true(len(systems.intro_log) == 1
                   and "MAP02" in systems.intro_log[0]["center"]
                   and len(systems.intro_log[0]["toasts"]) == 3,
                   "the first-map intro beat fired once with all pieces")
    keys_line = content.INTRO_TOAST_KEYS.format(
        wind=systems._binding_name("+pyaction3", "C"),
        rest=systems._binding_name("+pyaction2", "V"),
        sheet=systems._binding_name("+pyaction1", "Q"))
    bd.assert_true(systems.intro_log[0]["toasts"][1] == keys_line,
                   "the intro stated the live key bindings (the toast "
                   "ring rolls; the intro record is the truth)")
    strip = systems.refresh_strip()
    bd.assert_true("LV 6" in strip["line1"]
                   and "(need " in strip["line1"]
                   and "Blood " in strip["line1"]
                   and "Body " in strip["line1"]
                   and "[C] Second Wind" in strip["line1"],
                   "strip line 1 carries the hero's truth")
    modifier = systems.contract_state.get("modifier")
    if isinstance(modifier, dict) and modifier.get("id") not in (
            None, "plain"):
        bd.assert_true(f"[{modifier['name']}]" in strip["line2"],
                       "strip line 2 brackets the armed modifier")
    bd.assert_true("Depth 1" in strip["line2"],
                   "strip line 2 carries the completed-contract depth")
    if not bd.headless():  # imgui_frame never fires under -headless
        bd.assert_true(sheet_drawn_ok,
                       "the sheet drew inside imgui_frame")


def autotest_pre_save():
    autotest_pre_save.boon_pending_snapshot = systems.boon_pending()
    autotest_pre_save.boon_taken_snapshot = list(
        systems.boon_state.get("taken") or [])
    # Top the pool up and spend a charge so the round-trip has nontrivial
    # state to prove; then advance the script RNG stream so the checkpoint
    # captures a nontrivial stream position.
    hero = systems.hero
    hero.grant_resource("second_wind", 2)
    bd.assert_true(hero.use_resource("second_wind"),
                   "a charge spent before the save")
    bd.assert_true(hero.resources.get("second_wind") == 1,
                   "one charge remains for the round-trip")
    hero.hp = 7
    for _ in range(3):
        bd.randint(1, 1000000)


def autotest_save():
    bd.save_checkpoint(content.CHECKPOINT_NAME,
                       description="The Delve autotest")


def autotest_rng_draws():
    # Drawn after the checkpoint was written: these consume stream
    # positions the load will rewind.
    global rng_pair, expected_snapshot
    rng_pair = (bd.randint(1, 1000000), bd.randint(1, 1000000))
    expected_snapshot = systems.hero.serialize()


def autotest_load():
    bd.load_checkpoint(content.CHECKPOINT_NAME)


def autotest_post_load():
    hero = systems.hero
    redrawn = (bd.randint(1, 1000000), bd.randint(1, 1000000))
    bd.assert_true(rng_pair is not None and redrawn == rng_pair,
                   "script RNG stream resumes exactly after checkpoint load")
    snap = expected_snapshot or {}
    bd.assert_true(hero.level == snap.get("level") == 6
                   and hero.xp == snap.get("xp")
                   and hero.max_hp == snap.get("max_hp")
                   and hero.hp == snap.get("hp") == 7,
                   "level/XP/HP round-tripped through the checkpoint")
    pawn = player_pawn()
    if pawn is not None:
        bd.assert_true(pawn.health
                       == min(100, int(hero.hp / hero.max_hp * 100 + 0.5)),
                       "the map_load hard resync snapped the body to the "
                       "sheet's ratio")
    bd.assert_true(hero.abilities.score("str") == 15
                   and hero.abilities.score("con") == 14,
                   "ability scores round-tripped")
    bd.assert_true(hero.proficient_skills
                   == set(snap.get("proficient_skills", ()))
                   and hero.proficient_saves
                   == set(snap.get("proficient_saves", ())),
                   "proficiencies round-tripped")
    bd.assert_true(hero.resources.get("second_wind") == 1
                   and hero.resource_max.get("second_wind") == 2,
                   "resource pools round-tripped (spent charge stays spent)")
    saved = bd.state.get(bd_dnd.STATE_KEY)
    bd.assert_true(isinstance(saved, dict) and "character" in saved,
                   "bd_dnd state present in bd.state after load")
    saved_party = bd.state.get(bd_dnd.PARTY_STATE_KEY)
    bd.assert_true(isinstance(saved_party, dict)
                   and "party" in saved_party,
                   "bd_dnd party state present in bd.state after load")
    # The hound re-binds by TID: the checkpoint restored the world with
    # the follower actor, and map_load adopted it (no respawn).
    companion = systems.companion
    bd.assert_true(companion is not None and companion.bound
                   and not companion.dead,
                   "the hound survived the checkpoint")
    if companion is not None:
        ref = companion.actor()
        member = systems.party.get(content.HOUND_NAME)
        bd.assert_true(ref is not None and ref.valid and ref.alive
                       and ref.get_flag("FRIENDLY"),
                       "the hound re-bound to its actor after the load")
        if ref is not None and member is not None:
            bd.assert_true(ref.health == member.hp,
                           "hound and member hp round-tripped")
            bd.assert_true(companion.tid
                           == autotest_companion_revive_check.revive_tid,
                           "the saved TID was adopted, not respawned")
    # The contract round-tripped: the quest state restored (the reconcile
    # re-registered this map's quest after the load event), the mirror is
    # intact, and the module state matches.
    quest = bd_quests.log.get(content.contract_quest_id("MAP02"))
    bd.assert_true(quest is not None
                   and quest.state == bd_quests.Quest.COMPLETED,
                   "the completed contract quest round-tripped")
    if quest is not None:
        tribute = quest.objective("tribute")
        warden = quest.objective("warden")
        bd.assert_true(tribute.done and tribute.progress == tribute.count
                       and warden.done,
                       "both contract objectives round-tripped done")
    saved_contract = bd.state.get(content.CONTRACT_STATE_KEY)
    bd.assert_true(isinstance(saved_contract, dict)
                   and saved_contract.get("map") == "MAP02"
                   and saved_contract.get("warden_tid")
                   == systems.contract_state.get("warden_tid")
                   and saved_contract.get("done"),
                   "the contract mirror round-tripped")
    bd.assert_true(systems.contract_state.get("map") == "MAP02"
                   and systems.contract_state.get("done"),
                   "the contract module state reconciled from bd.state")
    bd.assert_true(not systems.warden_visuals_reapplied,
                   "the warden is dead: no visuals to re-apply (and no "
                   "re-roll happened)")
    saved_quests = bd.state.get(bd_quests.STATE_KEY)
    bd.assert_true(isinstance(saved_quests, dict)
                   and any(q.get("id") == content.contract_quest_id("MAP02")
                           and q.get("state") == "completed"
                           for q in saved_quests.get("quests", [])),
                   "bd_quests state carries the completed contract")
    bd.assert_true(systems.delve_depth() == 1,
                   "the completed contract persisted the depth")
    # Boons round-tripped: one pending pick and both taken boons, the
    # Deadly counter included; the pending Prepared pick then applies
    # through the same shared function.
    bd.assert_true(systems.boon_pending()
                   == autotest_pre_save.boon_pending_snapshot,
                   "the pending boons round-tripped")
    bd.assert_true(systems.boon_state.get("taken")
                   == ["toughness", "deadly"]
                   and systems.boon_state.get("deadly") == 1,
                   "the taken boons and the Deadly counter round-tripped")
    max_before = hero.resource_max.get("second_wind")
    bd.assert_true(systems.pick_boon("prepared"),
                   "the pending pick applied after the load")
    bd.assert_true(hero.resource_max.get("second_wind")
                   == max_before + 1,
                   "Prepared raised the charge maximum after the load")
    if not bd.headless():  # imgui_frame never fires under -headless
        bd.assert_true(sheet_drawn_ok,
                       "the sheet drew inside imgui_frame")
    bd.log("THE DELVE AUTOTEST assertions complete")


# --- sheet toggle (pyui / ui_command) ---------------------------------------------


def autotest_sheet_toggle_off():
    # bind_sheet_toggle registered `alias toggle_sheet "pyui toggle_sheet"`
    # at founding; running the alias fires a ui_command event whose handler
    # flips sheet.visible.
    bd.assert_true(sheet.visible, "sheet visible before the console toggle")
    bd.execute("toggle_sheet")


def autotest_sheet_toggle_on():
    bd.assert_true(not sheet.visible,
                   "toggle_sheet hid the sheet via ui_command")
    bd.execute("toggle_sheet")


def autotest_sheet_toggle_assert():
    bd.assert_true(sheet.visible,
                   "toggle_sheet showed the sheet again (idempotent binding)")


# --- custom actions (synthetic presses) -----------------------------------------------


def autotest_custom_action_sheet_press():
    """Synthetic Custom Action 1 press flips the reliquary sheet."""
    bd.assert_true(sheet.visible, "sheet visible before the custom action press")
    bd.set_custom_action(1, True)


def autotest_custom_action_sheet_assert():
    bd.assert_true((1, True) in action_log,
                   "the sheet press fired {'action': 1, 'pressed': True}")
    bd.assert_true(not sheet.visible,
                   "custom action 1 hid the sheet")
    bd.set_custom_action(1, False)


def autotest_custom_action_sheet_restore():
    bd.assert_true((1, False) in action_log,
                   "the sheet release fired {'action': 1, 'pressed': False}")
    bd.assert_true(not sheet.visible,
                   "the release edge left the sheet hidden")
    bd.set_custom_action(1, True)


def autotest_custom_action_sheet_back():
    bd.assert_true(sheet.visible,
                   "the second press showed the sheet again")
    bd.set_custom_action(1, False)


def autotest_custom_action_rest_press():
    """Synthetic Custom Action 2 press runs the rest path in a controlled
    sanctuary: force the light, clear the cooldown, then press."""
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "rest: pawn available")
    if pawn is None:
        return
    sector = bd.sector_at(pawn.x, pawn.y)
    bd.assert_true(sector is not None, "rest: sector resolvable")
    if sector is None:
        return
    sector.light = 200  # forced sanctuary light
    systems.reset_rest_cooldown()  # the pre-save rests are inside 30s
    systems.hero.set_hp(10)
    bd.set_custom_action(2, True)


def autotest_custom_action_rest_assert():
    bd.assert_true((2, True) in action_log,
                   "the rest press fired {'action': 2, 'pressed': True}")
    outcome = last_rest_outcome or {}
    bd.assert_true(outcome.get("kind") == "sanctuary",
                   "custom action 2 ran the rest path (sanctuary branch)")
    bd.assert_true(systems.hero.hp == systems.hero.max_hp,
                   "the custom-action rest healed the hero fully")
    bd.set_custom_action(2, False)


def autotest_custom_action_rest_release():
    bd.assert_true((2, False) in action_log,
                   "the rest release fired {'action': 2, 'pressed': False}")


# --- screenshot helper ---------------------------------------------------------


def screenshot_flavor():
    # Populate the roll log so the Delver window's sections have content,
    # wound the blood bar, put a monster in the crosshair for the
    # examine line, and refresh the strip.
    hero = systems.hero
    hero.skill_check("athletics", 15)          # a door bash
    hero.saving_throw("dex", 13)               # the reflex save
    hero.skill_check("perception", 12)
    hero.set_hp(max(1, hero.max_hp * 2 // 5))
    pawn = player_pawn()
    if pawn is not None:
        try:
            pawn.set_position(pawn.x, pawn.y, pawn.z, check=False)
            pawn.damage_factor = 1.0
            pawn.damage(30)
            pawn.damage_factor = 0.0
            bd.spawn("DoomImp", pawn.x + 220.0, pawn.y, pawn.z,
                     angle=270.0, force=True)
            pawn.angle = 0.0
            systems._examine_probe_tick()
        except Exception as exc:
            bd.warn(f"delve: screenshot pose failed: {exc!r}")
    systems.refresh_strip()


# --- imgui overlay -------------------------------------------------------------


@bd.on("imgui_frame")
def draw_ui(event):
    global autowarp_done, sheet_drawn_ok
    # Headless runs (-scripttest) launch without +map: queue a warp on the
    # first rendered frame when nothing loaded a level yet (same pattern as
    # 26_imgui_overlays / 27_quest_journal). Any map works for the Delve.
    if not autowarp_done:
        autowarp_done = True
        try:
            if not bd.current_map():
                bd.execute("map map02")
        except RuntimeError:
            pass
    ensure_sheet()
    ui.draw_frame()
    sheet_drawn_ok = sheet_drawn_ok or bool(ui.ctx.get("sheet_drawn_ok"))
