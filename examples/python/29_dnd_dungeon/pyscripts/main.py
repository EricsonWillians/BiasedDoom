"""The Sunken Crypt — a D&D delve across the whole WAD.

A grave-dark dungeon-crawl rules layer built on the engine-shipped
``bd_dnd`` rules framework and the ``bd_horror`` presentation pack
(theme / toasts / light programs — deliberately NOT the Dread meter,
whose heartbeat loop and whisper stings were retired from this example).
You are Morrow, a grave-hardened fighter; The Hollow Warden and its bound
crypt hound descend with you.

The rules ride every map of the game: kill XP from the full
Doom/Heretic/Hexen bestiary, level-toughened blows (+damage per level
through the ``actor_before_damage`` filter), light-ruled rests that mend
the pawn as well as the sheet, an active Second Wind heal on Custom
Action 3, a DEX reflex save that refunds half of incoming hits, and any
locked door in the game bashable with Athletics (systems.DoorBashRules).
MAP02 keeps the probed set pieces that make it the crypt proper: the
line-111 red door (the bash's reference fixture), the entrance dart trap
(sector 7, tag 13), and the candle/corpse-light programs on tags 13/7/12.

Module map (all four are listed in the PYTHON manifest):

- ``content.py``: pure data and factories: the delvers (Morrow, built
  by a driven ``CreationWizard`` bound to the Fighter class), the crypt's
  probed map constants, progression tuning, rest rules, prose.
- ``systems.py`` — behavior: light-program arming, the door-bash
  dispatcher, level damage, Second Wind, sanctuary/nightmare rests, the
  progression strip, level-up and death-knell feedback, checkpoint
  quiescence.
- ``ui.py`` — the bd_horror-themed ImGui reliquary (character sheet,
  party roster, toasts). Inert headless.
- ``main.py`` (this file) — bootstrap, event wiring, and the autotest
  driver.

Autotest: ``BD_EXAMPLE_AUTOTEST=1`` drives the rules engine headlessly
and deterministically: dice math and determinism, the classes layer
(wizard validation, the wizard-built Fighter sheet reproducing Morrow's
stats, the Grave-Hardened resolve grant and Second Wind charge, ASI
queueing at level 4), the modifier and proficiency tables,
scripted-roller checks for success/failure/crit branches, XP thresholds
and level-up effects, rests and resources, kill XP with exact player
credit (the monsters are killed through the player pawn; synthetic-event
cases prove monster/environmental kills do not count), the extended
bestiary XP table, level-toughened blows (synthetic before-damage
events), the *real* door-bash path on line 111 (an unreachable DC first,
then a trivial one) plus the generic bash dispatcher (per-line check
creation, refusal memory), controlled trap springs (full vs. halved
damage), the sanctuary/nightmare rest branches under forced light levels
(healing asserted on sheet and pawn alike), a Second Wind press through
Custom Action 3, then a checkpoint round-trip asserting the script RNG
stream, the whole CharacterState, the PartyState (companions included),
the ``toggle_sheet`` console alias, and the synthetic Custom Action
presses (sheet toggle round-trip, then a controlled sanctuary rest
through Custom Action 2 with the outcome read back). Because the
candle/fluorescent light programs draw from the script RNG on every step
and their post-load task phase cannot reproduce the saved stream position
(probe-verified), the autotest quiesces them just before the save and
re-arms them after the stream assertion; the interactive path lets them
ride HorrorState persistence normally. The run ends via ``-scripttest``'s
own PASS/FAIL accounting (no explicit quit).

``BD_EXAMPLE_SCREENSHOT=1`` schedules a screenshot a few seconds in, for
documentation captures.
"""

import os
import sys

import biaseddoom as bd
import bd_dnd
import bd_horror
from bd_dnd.sheet import bind_sheet_toggle
from bd_horror import toasts


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


content = _load_sibling("pyscripts/content.py", "crypt_content")
systems = _load_sibling("pyscripts/systems.py", "crypt_systems")
ui = _load_sibling("pyscripts/ui.py", "crypt_ui")

AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"
SCREENSHOT = os.environ.get("BD_EXAMPLE_SCREENSHOT") == "1"

# --- the delvers and the crypt ------------------------------------------------

character = content.make_hero_via_wizard()
state = bd_dnd.CharacterState(character)
horror = bd_horror.HorrorState()
sheet = ui.ReliquarySheet(
    character, state,
    title=f"{content.HERO_NAME} {content.HERO_EPITHET} - Fighter 1")

door_bash = None        # DoorBashRules dispatcher: any locked door, any map
door_check = None       # the MAP02 line-111 check (autotest pre-registers it)
entrance_trap = None    # interactive-only TrapZone in the entrance (MAP02)
dialogue_gate = None    # DialogueSkillGate demo (unit-tested headlessly)
reflex_save = None      # interactive-only DamageSaveRule (any hit, DEX)
level_damage_handler = None  # wire_level_damage's actor_before_damage hook
party = None            # the hero plus the Hollow Warden
party_state = None      # PartyState persistence for the party
companion = None        # the Warden's bound crypt hound in the world
companion_died_log = [] # on_companion_died member names, in order

autowarp_done = False
sheet_drawn_ok = False
strip_armed = False     # the progression strip's repeating refresh task
door_drive_until = -1   # pre_tick +use pulses while level_time < this
rng_pair = None         # (a1, a2) drawn after the checkpoint save
expected_snapshot = None  # character.serialize() captured before reload

player_pawn = systems.player_pawn


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


# --- custom actions ------------------------------------------------------------


#: Every custom_action payload the example saw, as (action, pressed)
#: pairs. The autotest asserts on the exact transitions.
action_log = []

#: The outcome dict of the last rest Custom Action 2 (or the
#: `crypt_rest` alias) ran; the autotest reads it back.
last_rest_outcome = None

#: The outcome dict of the last Second Wind (Custom Action 3 or the
#: `second_wind` alias); the autotest reads it back.
last_wind_outcome = None


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
        bd.warn(f"sunken crypt: could not bind +pyaction{n}: {exc!r}")


# --- scenario setup ------------------------------------------------------------


@bd.on("engine_start")
def setup_dungeon(event):
    global door_bash, entrance_trap, dialogue_gate, reflex_save
    global level_damage_handler
    bd.imgui.set_master_visible(True)
    bd.log(f"bd_dnd shipped from: {bd_dnd.__file__}")
    bd.log(f"bd_horror shipped from: {bd_horror.theme.__file__}")

    state.arm_persistence()
    horror.arm_persistence()  # light programs only; dread never starts

    # Morrow's resolve and second_wind pools are class-granted: the level-1
    # Grave-Hardened and Second Wind features seed them when the
    # CreationWizard binds the Fighter.

    # Bash any locked door in the game with Athletics: the dispatcher
    # lazily builds a card-lending check per locked line the player uses.
    door_bash = systems.DoorBashRules(character, dc=content.DOOR_DC)

    # Levels toughen the blows: +1 damage per level past the first (capped)
    # on every monster the local player hits.
    level_damage_handler = systems.wire_level_damage(character)

    # A gate demo: MAP02 has no Strife NPCs, so this never fires
    # interactively; the autotest exercises it with a synthetic event.
    dialogue_gate = bd_dnd.DialogueSkillGate(
        character, log_number=1, skill="persuasion", dc=12,
        on_success=lambda c, r: bd.center_message("They believe you."),
        on_fail=lambda c, r: bd.center_message("They see through you."))

    if not AUTOTEST:
        # Dart trap in the entrance chamber, a MAP02 set piece: created
        # disarmed so no other map's tag 13 can spring it; on_map arms it
        # on the crypt map and disarms it elsewhere. sector_entered fires
        # for the spawn sector at t=0, so it greets the player immediately
        # and then respects its cooldown on re-entry.
        entrance_trap = bd_dnd.TrapZone(
            content.TRAP_TAG, character, dc=content.TRAP_DC,
            damage=content.TRAP_DAMAGE, once=False,
            cooldown_tics=content.TRAP_COOLDOWN_TICS)
        entrance_trap.disarm()
        # Reflexes: a DEX save vs. any incoming damage refunds half on a
        # success (the framework center-messages the dodge). Works on
        # every map: fireballs, floors, and fists alike. DoomImpBall
        # declares no DamageType (verified in
        # wadsrc/static/zscript/actors/doom/doomimp.zs), so imp fireballs
        # report damage_type "None" — damage_type=None (no filter) is
        # what catches them here; a real mod would filter, e.g.
        # damage_type="Fire" for explicitly fire-typed mod damage.
        reflex_save = bd_dnd.DamageSaveRule(
            character, dc=content.SAVE_DC, ability="dex", damage_type=None,
            cooldown_tics=content.SAVE_COOLDOWN_TICS)

    # Exact kill credit: only kills by the local player (player_index=0)
    # award XP — missile kills credit the shooter; monster infighting,
    # crushers, and source-less scripted kills award nothing. Pass
    # player_index=None for the legacy any-death policy.
    bd_dnd.track_xp_from_kills(character, player_index=0)

    # Console aliases through the pyui/ui_command bridge: `toggle_sheet`
    # flips the reliquary, `crypt_rest` attempts a sanctuary rest,
    # `second_wind` spends the heal charge. The keys live on Custom
    # Actions 1 (sheet), 2 (rest), and 3 (second wind), auto-bound below
    # and rebindable in Options -> Customize Controls, Custom Actions.
    bind_sheet_toggle(sheet)
    bd.execute('alias crypt_rest "pyui crypt_rest"')
    bd.execute('alias second_wind "pyui second_wind"')
    ensure_custom_action_binding(1, "q")
    ensure_custom_action_binding(2, "v")
    ensure_custom_action_binding(3, "c")

    systems.wire_level_up(character)
    ui.ctx.update(sheet=sheet, sheet_drawn_ok=False)


@bd.on("map_load")
def on_map(event):
    global strip_armed
    pawn = player_pawn()
    if pawn is None:
        return
    # The persistent progression strip: one repeating task for the whole
    # session (map_local=False, it redraws the same display-list id).
    if not strip_armed:
        strip_armed = True
        try:
            bd.schedule(lambda: systems.refresh_progress_strip(character),
                        delay=content.HUD_REFRESH_TICS,
                        repeat=content.HUD_REFRESH_TICS, map_local=False)
        except Exception as exc:
            bd.warn(f"sunken crypt: could not arm the progress strip: "
                    f"{exc!r}")

    if event.get("from_savegame"):
        # The checkpoint restored the world and the bd_dnd/bd_horror load
        # handlers restored character, party, and light-program state. Off
        # the crypt map no programs may bind, so drop them instead.
        if not systems.is_crypt_map():
            horror.lights.clear()
        if entrance_trap is not None:
            entrance_trap.armed = systems.is_crypt_map()
        # The autotest deliberately does NOT re-arm the RNG-drawing light
        # programs here — their task phase would desync the exact-stream
        # assertion; autotest_post_load re-arms them after it.
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

    # The probed set pieces (candle/corpse-light programs, the entrance
    # dart trap, the fixture monsters) live on the crypt map alone; the
    # generic rules layer runs everywhere.
    on_crypt = systems.is_crypt_map()
    horror.lights.clear()  # drop any stale programs before (re)arming
    if on_crypt:
        systems.arm_crypt_lights(horror)
    if entrance_trap is not None:
        entrance_trap.armed = on_crypt
    if not AUTOTEST and on_crypt:
        toasts.toast(content.TOAST_INTRO, kind="omen")

    if on_crypt:
        # Worth XP: 25 + 25 + 50 = 100.
        for class_name, tid, side in content.MONSTER_SPAWNS:
            try:
                bd.spawn(class_name, content.START_POS[0] + side,
                         content.AWAY_POS[1], content.START_POS[2],
                         angle=90.0, tid=tid, force=True)
            except Exception as exc:
                bd.warn(f"sunken crypt spawn {class_name} failed: {exc!r}")

    if not AUTOTEST:
        ensure_interactive_party()
    if AUTOTEST:
        pawn.damage_factor = 0.0  # nothing may kill the test driver
        bd.schedule(autotest_dice, delay=10)
        bd.schedule(autotest_class_creation, delay=15)
        bd.schedule(autotest_abilities, delay=20)
        bd.schedule(autotest_lights, delay=25)
        bd.schedule(autotest_checks, delay=30)
        bd.schedule(autotest_xp_levels, delay=40)
        bd.schedule(autotest_resources_rest, delay=50)
        bd.schedule(autotest_kill_xp, delay=60)
        bd.schedule(autotest_exact_credit, delay=65)
        bd.schedule(autotest_xp_levelup, delay=70)
        bd.schedule(autotest_door_generic, delay=77)
        bd.schedule(autotest_level_damage, delay=78)
        bd.schedule(autotest_door_holds, delay=80)
        bd.schedule(autotest_door_bash, delay=130)
        bd.schedule(autotest_door_asserts, delay=170)
        bd.schedule(autotest_trap_fail_arm, delay=190)
        bd.schedule(autotest_trap_fail_spring, delay=205)
        bd.schedule(autotest_trap_fail_asserts, delay=220)
        bd.schedule(autotest_trap_save_arm, delay=235)
        bd.schedule(autotest_trap_save_spring, delay=250)
        bd.schedule(autotest_trap_save_asserts, delay=265)
        bd.schedule(autotest_dialogue_gate, delay=275)
        bd.schedule(autotest_damage_save, delay=280)
        bd.schedule(autotest_mid_asserts, delay=290)
        bd.schedule(autotest_party, delay=293)
        bd.schedule(autotest_companion_damage, delay=305)
        bd.schedule(autotest_companion_teleport, delay=315)
        bd.schedule(autotest_companion_teleport_check, delay=340)
        bd.schedule(autotest_companion_kill, delay=350)
        bd.schedule(autotest_companion_revive, delay=360)
        bd.schedule(autotest_companion_revive_check, delay=375)
        bd.schedule(autotest_nightmare, delay=378)
        bd.schedule(autotest_fitful_and_sanctuary, delay=381)
        bd.schedule(autotest_second_wind_press, delay=382)
        bd.schedule(autotest_second_wind_asserts, delay=384)
        bd.schedule(autotest_pre_save, delay=386)
        bd.schedule(autotest_save, delay=388)
        bd.schedule(autotest_rng_draws, delay=400)
        bd.schedule(autotest_load, delay=415)
    if SCREENSHOT:
        bd.schedule(screenshot_flavor, delay=bd.TICRATE)
        bd.schedule(lambda: bd.execute("screenshot /tmp/sunken_crypt"),
                    delay=2 * bd.TICRATE + 10)
        bd.schedule(lambda: bd.execute("quit"), delay=4 * bd.TICRATE)


@bd.on("pre_tick")
def drive_door_use(event):
    """Face the sealed door and pulse BT_USE (1 tic on / 3 off).

    ``actor.angle`` is set directly — ``set_input(yaw=...)`` writes a yaw
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


def _run_crypt_rest():
    """The crypt_rest body shared by the console alias and Custom Action 2.

    Records the outcome dict so the autotest can read back which rest
    branch ran."""
    global last_rest_outcome
    outcome = systems.try_long_rest(character)
    last_rest_outcome = outcome
    if outcome["kind"] == "sanctuary":
        bd.center_message("You rest in the light.")
    elif outcome["kind"] == "fitful":
        bd.center_message("You sleep badly, and wake worse.")
    else:
        bd.center_message("The dark dreams with you.")
    return outcome


def _run_second_wind():
    """The second_wind body shared by the console alias and Custom Action
    3. Records the outcome dict so the autotest can read it back."""
    global last_wind_outcome
    outcome = systems.use_second_wind(character)
    last_wind_outcome = outcome
    if outcome.get("ok"):
        bd.center_message(
            f"Second wind! +{outcome['amount']} ({outcome['healed_pawn']} "
            f"to the body)")
    return outcome


@bd.on("ui_command")
def on_ui_command(event):
    command = event.get("command")
    if command == "second_wind":
        if AUTOTEST:
            return  # the autotest drives the Custom Action 3 path
        _run_second_wind()
        return
    if command != "crypt_rest":
        return
    if AUTOTEST:
        return  # the scripted rest assertions drive try_long_rest directly
    _run_crypt_rest()


@bd.on("custom_action")
def on_custom_action(event):
    """Action 1 toggles the reliquary, action 2 attempts a rest, action 3
    spends the Second Wind; all run the same bodies as the
    `toggle_sheet` / `crypt_rest` / `second_wind` aliases."""
    try:
        action = int(event.get("action") or 0)
        pressed = bool(event.get("pressed"))
        action_log.append((action, pressed))
        if not pressed:
            return
        if action == 1:
            sheet.toggle()
        elif action == 2:
            _run_crypt_rest()
        elif action == 3:
            _run_second_wind()
    except Exception as exc:
        bd.warn(f"sunken crypt: custom action failed: {exc!r}")


# --- party / companion helpers -------------------------------------------------


def ensure_interactive_party():
    """Interactive mode: party + the Warden's crypt hound, bound once.

    The hound shadows the player, fights their recent attackers through
    its native demon AI, and its health *is* the Warden's RPG hp — hurt
    it and the PartyReliquary blood bar drops; rest the Warden and the
    hound heals.
    """
    global party, party_state, companion
    if party is not None:
        return
    party = content.make_party(character)
    party_state = bd_dnd.PartyState(party)
    companion = content.make_companion()
    party_state.add_companion(companion)
    systems.wire_knell(companion, companion_died_log)
    companion.bind(party)
    party_state.arm_persistence()
    ui.ctx["party_sheet"] = ui.PartyReliquary(party, party_state,
                                              title=content.PARTY_NAME)


# --- autotest steps ------------------------------------------------------------


def autotest_dice():
    result = bd_dnd.roll("2d6+3", rng=_Roller([4, 2]))
    bd.assert_true(result["rolls"] == [4, 2] and result["modifier"] == 3
                   and result["total"] == 9 and result["notation"] == "2d6+3",
                   "roll('2d6+3') structure and total")
    bd.assert_true(bd_dnd.roll("d20", rng=_Roller([11]))["rolls"] == [11],
                   "bare 'd20' means 1d20")
    spaced = bd_dnd.roll("1 d8 - 2", rng=_Roller([5]))
    bd.assert_true(spaced["rolls"] == [5] and spaced["modifier"] == -2
                   and spaced["total"] == 3,
                   "spaced '1 d8 - 2' parses with a negative modifier")
    bd.assert_true(bd_dnd.roll("2D6", rng=_Roller([1, 6]))["total"] == 7,
                   "notation is case-insensitive")
    bd.assert_true(raises_value_error(lambda: bd_dnd.roll("2x6")),
                   "malformed notation raises ValueError")
    bd.assert_true(raises_value_error(lambda: bd_dnd.roll("")),
                   "empty notation raises ValueError")
    bd.assert_true(raises_value_error(lambda: bd_dnd.roll("0d6")),
                   "zero dice raises ValueError")
    # d20: advantage keeps the higher, disadvantage the lower, both cancel.
    adv = bd_dnd.d20(mod=3, advantage=True, rng=_Roller([18, 6]))
    bd.assert_true(adv["kept"] == 18 and adv["discarded"] == 6
                   and adv["total"] == 21 and adv["roll"] == 18,
                   "advantage keeps the higher die")
    dis = bd_dnd.d20(mod=3, disadvantage=True, rng=_Roller([18, 6]))
    bd.assert_true(dis["kept"] == 6 and dis["discarded"] == 18
                   and dis["total"] == 9,
                   "disadvantage keeps the lower die")
    both = bd_dnd.d20(mod=0, advantage=True, disadvantage=True,
                      rng=_Roller([13, 20]))
    bd.assert_true(both["kept"] == 13 and "discarded" not in both,
                   "advantage + disadvantage cancel into a straight roll")
    bd.assert_true(bd_dnd.d20(rng=_Roller([20]))["critical"] == "hit",
                   "natural 20 is a critical hit")
    bd.assert_true(bd_dnd.d20(rng=_Roller([1]))["critical"] == "miss",
                   "natural 1 is a critical miss")
    bd.assert_true(bd_dnd.d20(rng=_Roller([10]))["critical"] is None,
                   "ordinary rolls are not critical")
    # Determinism: identical seeds produce identical sequences.
    s1, s2 = bd.rng(42), bd.rng(42)
    bd.assert_true([s1.int(1, 20) for _ in range(8)]
                   == [s2.int(1, 20) for _ in range(8)],
                   "twin rng streams produce identical sequences")


def autotest_class_creation():
    """The classes layer: wizard validation, the bound Fighter, ASI queue.

    Unit checks on the shipped hero plus one fresh wizard-built character
    (the main character only supplies the class/resource assertions).
    """
    # The wizard path rejects an unskilled finish: one class skill picked
    # where the Fighter demands both. Point buy keeps the check dice-free.
    unskilled = bd_dnd.CreationWizard()
    unskilled.choose_class(content.FIGHTER)
    unskilled.set_name("Unfinished")
    unskilled.use_point_buy()
    for ability in bd_dnd.ABILITIES:
        unskilled.set_score(ability, 10)
    unskilled.assign_skill("athletics")
    bd.assert_true(raises_value_error(unskilled.finish),
                   "the wizard rejects a finish with too few class skills")
    # The shipped hero is a wizard-built Fighter reproducing the exact
    # probed sheet; the level-1 Grave-Hardened feature seeded the resolve
    # pool when bind_class ran inside finish().
    bd.assert_true(character.class_id == "Fighter"
                   and character.cls is content.FIGHTER,
                   "the hero is a wizard-bound Fighter")
    bd.assert_true(character.abilities.score("str") == 16
                   and character.abilities.score("con") == 14
                   and character.hit_die == 10
                   and character.proficient_skills
                   == {"athletics", "perception"}
                   and character.proficient_saves == {"str", "con"},
                   "the wizard-built hero reproduces Morrow's exact sheet")
    bd.assert_true(character.resource_max.get(content.NIGHTMARE_RESOURCE)
                   == content.NIGHTMARE_RESOURCE_CHARGES
                   and character.resources.get(content.NIGHTMARE_RESOURCE)
                   == content.NIGHTMARE_RESOURCE_CHARGES,
                   "the Grave-Hardened feature granted the resolve charges")
    bd.assert_true(character.resource_max.get(content.SECOND_WIND_RESOURCE)
                   == content.SECOND_WIND_CHARGES
                   and character.resources.get(content.SECOND_WIND_RESOURCE)
                   == content.SECOND_WIND_CHARGES,
                   "the Second Wind feature granted its charge")
    # XP to level 4 queues a pending ASI entry (two points, unspent).
    unit = content.make_hero_via_wizard()
    unit.award_xp(2700)  # 300/900/2700 thresholds: exactly level 4
    pending = getattr(unit, "pending_asi", None)
    bd.assert_true(unit.level == 4 and isinstance(pending, list)
                   and len(pending) == 1 and pending[0].get("points") == 2
                   and pending[0].get("spent") == 0,
                   "level 4 queues two pending ASI points")


def autotest_abilities():
    expected = {1: -5, 3: -4, 8: -1, 9: -1, 10: 0, 11: 0, 12: 1,
                15: 2, 20: 5}
    for score, mod in expected.items():
        bd.assert_true(bd_dnd.modifier(score) == mod,
                       f"modifier({score}) == {mod}")
    bd.assert_true(character.abilities.mod("str") == 3
                   and character.abilities.mod("dex") == 1
                   and character.abilities.mod("con") == 2
                   and character.abilities.mod("cha") == -1,
                   "fighter ability modifiers")
    prof = {1: 2, 4: 2, 5: 3, 8: 3, 9: 4, 12: 4, 13: 5, 16: 5, 17: 6}
    for level, bonus in prof.items():
        bd.assert_true(bd_dnd.proficiency_bonus(level) == bonus,
                       f"proficiency_bonus({level}) == {bonus}")
    bd.assert_true(bd_dnd.SKILLS["athletics"] == "str"
                   and bd_dnd.SKILLS["sleight_of_hand"] == "dex"
                   and bd_dnd.SKILLS["perception"] == "wis"
                   and bd_dnd.SKILLS["persuasion"] == "cha",
                   "skill -> ability table")


def autotest_lights():
    """Torch and darkness: the crypt's light programs are bound and live."""
    report = systems.crypt_light_report(horror)
    bd.assert_true(report["candle"] and report["fluorescent"],
                   "candle and fluorescent programs are bound to sectors")
    by_tag = {}
    for entry in report["programs"]:
        for tag in entry["tags"]:
            by_tag[tag] = entry
    bd.assert_true(13 in by_tag and by_tag[13]["kind"] == "candle"
                   and by_tag[13]["sectors"] == [7]
                   and by_tag[13]["originals"].get(7) == 144,
                   "candle program active on the entrance chamber "
                   "(tag 13, sector 7, original light 144)")
    bd.assert_true(7 in by_tag and by_tag[7]["kind"] == "candle"
                   and by_tag[7]["sectors"] == [40, 41, 42, 43],
                   "candle program active on the drowned passage (tag 7)")
    bd.assert_true(12 in by_tag and by_tag[12]["kind"] == "fluorescent"
                   and by_tag[12]["sectors"] == [47],
                   "corpse-light program active on the flooded hall "
                   "(tag 12, sector 47)")


def autotest_checks():
    hero = bd_dnd.Character(
        "Test", bd_dnd.AbilityScores(str=16, wis=14),
        proficient_skills=("athletics",), proficient_saves=("str",))
    # Success branch: 14 + 3 (str) + 2 (prof) = 19 vs DC 15.
    result = hero.skill_check("athletics", 15, rng=_Roller([14]))
    bd.assert_true(result["success"] and result["total"] == 19
                   and result["ability"] == "str"
                   and result["ability_mod"] == 3 and result["prof"] == 2
                   and result["critical"] is None,
                   "skill check success branch math")
    # Failure branch, no proficiency: 4 + 2 (wis 14) = 6 vs DC 15.
    result = hero.skill_check("perception", 15, rng=_Roller([4]))
    bd.assert_true(not result["success"] and result["total"] == 6
                   and result["prof"] == 0,
                   "skill check failure branch, unproficient")
    # Crit branch: natural 20 reports critical hit (and success).
    result = hero.skill_check("athletics", 30, rng=_Roller([20]))
    bd.assert_true(result["critical"] == "hit" and result["total"] == 25
                   and not result["success"],
                   "critical hit reported even when the total misses the DC")
    # Ability check: no proficiency, ever.
    result = hero.ability_check("str", 10, rng=_Roller([5]))
    bd.assert_true(result["prof"] == 0 and result["total"] == 8
                   and not result["success"],
                   "ability checks never add proficiency")
    # Saving throw: proficiency only for proficient saves.
    result = hero.saving_throw("str", 10, rng=_Roller([5]))
    bd.assert_true(result["prof"] == 2 and result["total"] == 10
                   and result["success"],
                   "proficient save adds proficiency")
    result = hero.saving_throw("dex", 10, rng=_Roller([5]))
    bd.assert_true(result["prof"] == 0 and result["total"] == 5,
                   "unproficient save does not")
    bd.assert_true(raises_value_error(
        lambda: hero.skill_check("card_counting", 10)),
        "unknown skill raises ValueError")
    bd.assert_true(len(hero.roll_log) == 6,
                   "every check appends to the roll log")


def autotest_xp_levels():
    hero = bd_dnd.Character("Test2", bd_dnd.AbilityScores(con=14),
                            hit_die=10)
    bd.assert_true(hero.max_hp == 12,
                   "level-1 HP = hit die (10) + con mod (2)")
    fired = []
    hero.on_level_up.append(lambda c, lvl: fired.append(lvl))
    events = hero.award_xp(299)
    bd.assert_true(events == [] and hero.level == 1,
                   "299 XP does not level")
    events = hero.award_xp(1)  # 300 exactly: level 2
    bd.assert_true(hero.level == 2 and len(events) == 1
                   and events[0]["level"] == 2 and fired == [2],
                   "300 XP reaches level 2 and fires on_level_up")
    bd.assert_true(events[0]["hp_gained"] == 8 and hero.max_hp == 20
                   and hero.hp == 20,
                   "level-up: +hit die avg (6) + con mod (2), hp heals too")
    bd.assert_true(hero.proficiency == 2, "proficiency still +2 at level 2")
    events = hero.award_xp(1000000)  # blow through every threshold
    bd.assert_true(hero.level == bd_dnd.Character.MAX_LEVEL
                   and hero.proficiency == 6,
                   "XP flood caps at level 20 with +6 proficiency")
    bd.assert_true(hero.xp_for_next_level() is None,
                   "no next-level threshold at the cap")


def autotest_resources_rest():
    hero = bd_dnd.Character("Test3")
    hero.grant_resource("rage", 2)
    bd.assert_true(hero.use_resource("rage") and hero.use_resource("rage"),
                   "spend both rage charges")
    bd.assert_true(not hero.use_resource("rage"),
                   "exhausted resource refuses")
    hero.restore_resources()
    bd.assert_true(hero.resources["rage"] == 2, "restore refills the pool")
    hero.hp = 4
    outcome = hero.rest(short=True)
    bd.assert_true(outcome["healed"] == 2 and hero.hp == 6,
                   "short rest heals 25% of max hp (10 -> +2)")
    hero.hp = hero.max_hp - 1
    outcome = hero.rest(short=True)
    bd.assert_true(outcome["healed"] == 1 and hero.hp == hero.max_hp,
                   "short rest caps at max hp")
    hero.use_resource("rage")
    hero.hp = 1
    outcome = hero.rest(short=False)
    bd.assert_true(outcome["healed"] == hero.max_hp - 1
                   and hero.hp == hero.max_hp
                   and hero.resources["rage"] == 2,
                   "long rest fully heals and restores resources")


def autotest_kill_xp():
    bd.assert_true(character.xp == 0, "no XP before the kills")
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "player pawn available")
    if pawn is None:
        return
    for tid in content.MONSTER_TIDS:
        ref = bd.actor_ref(tid)
        if ref is not None and ref.valid:
            # source=pawn: the player pawn is the Die() source, so
            # attacker_player_index == 0 and exact credit applies.
            ref.damage(1000, source=pawn)


def autotest_exact_credit():
    """Exact kill credit, unit-tested with synthetic actor_died events
    (same style as the DialogueSkillGate test): real monster-kills-monster
    scenarios are unreliable to stage deterministically, so the crediting
    function is called directly with the payload shapes the engine emits."""
    hero = bd_dnd.Character("CreditTest")
    handler = bd_dnd.track_xp_from_kills(hero, announce=False, player_index=0)
    handler({"actor": {"class_name": "ZombieMan"}, "actor_ref": None,
             "attacker_player_index": 0})
    bd.assert_true(hero.xp == 25, "a player-sourced kill credits XP")
    handler({"actor": {"class_name": "ZombieMan"}, "actor_ref": None,
             "attacker_player_index": None})
    bd.assert_true(hero.xp == 25,
                   "a source-less death (crusher, script) does not credit")
    handler({"actor": {"class_name": "ZombieMan"}, "actor_ref": None,
             "attacker_player_index": 1})
    bd.assert_true(hero.xp == 25, "another player's kill does not credit")
    handler({"actor": {"class_name": "ZombieMan"}, "actor_ref": None})
    bd.assert_true(hero.xp == 25,
                   "events lacking attacker keys do not credit")
    # Legacy approximate policy: player_index=None credits any death.
    legacy = bd_dnd.Character("CreditTestLegacy")
    legacy_handler = bd_dnd.track_xp_from_kills(legacy, announce=False,
                                                player_index=None)
    legacy_handler({"actor": {"class_name": "ZombieMan"},
                    "actor_ref": None, "attacker_player_index": None})
    bd.assert_true(legacy.xp == 25,
                   "player_index=None restores any-death crediting")
    # The shipped table covers the full Doom II roster plus the common
    # Heretic/Hexen bestiary, so kills pay out on every map of any game.
    table = {k.lower(): v for k, v in bd_dnd.DEFAULT_XP_TABLE.items()}
    for class_name in ("revenant", "mancubus", "arachnotron",
                       "painelemental", "archvile", "lostsoul",
                       "chaingunguy", "spectre", "spidermastermind",
                       "cyberdemon", "hereticimp", "wizard", "beast",
                       "ironlich", "minotaur", "sorcerer1", "ettin",
                       "firedemon", "centaur", "serpent", "bishop",
                       "wraith", "heresiarch", "korax"):
        bd.assert_true(table.get(class_name, 0) > 0,
                       f"the XP table covers {class_name}")


def autotest_door_generic():
    """The bash dispatcher covers any locked line, not just the MAP02 door.

    Synthetic events drive the dispatcher directly (same style as the
    DialogueSkillGate test): a failed roll creates the per-line check and
    leaves it armed; a passed roll whose activation is refused (the line
    cannot resolve) retires the line into the unbashable memory.
    """
    pawn = player_pawn()
    bd.assert_true(pawn is not None and door_bash is not None,
                   "door generic: fixtures available")
    if pawn is None or door_bash is None:
        return
    result = door_bash._dispatch({"reason": "locked", "line_index": 5,
                                  "actor_ref": pawn},
                                 rng=_Roller([1]))  # 1+3+2 = 6 < DC 15
    check = door_bash.checks.get(("MAP02", 5))
    bd.assert_true(result is not None and not result["success"],
                   "the dispatcher rolled the lazily-created check")
    bd.assert_true(check is not None and check.attempts == 1
                   and not check.opened
                   and ("MAP02", 5) not in door_bash.unbashable,
                   "a failed bash stays armed for retries")
    result = door_bash._dispatch({"reason": "locked", "line_index": 99999,
                                  "actor_ref": pawn},
                                 rng=_Roller([20]))  # 20+3+2 >= DC 15
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


def autotest_level_damage():
    """Level-toughened blows: +1 per level past the first, monsters only.

    The character is level 2 here (autotest_xp_levelup ran), so the bonus
    is exactly +1. Synthetic before-damage events, same style as the
    exact-credit test."""
    pawn = player_pawn()
    bd.assert_true(pawn is not None and level_damage_handler is not None,
                   "level damage: fixtures available")
    if pawn is None or level_damage_handler is None:
        return
    bd.assert_true(systems.level_damage_bonus(character) == 1,
                   "level 2 grants +1 damage")
    target = None
    try:
        target = bd.spawn("ZombieMan", *content.AWAY_POS, force=True)
    except Exception as exc:
        bd.warn(f"sunken crypt: level-damage target spawn failed: {exc!r}")
    if target is not None:
        event = {"actor_ref": target, "attacker_player_index": 0,
                 "damage": 10}
        level_damage_handler(event)
        bd.assert_true(event["damage"] == 11,
                       "a player hit on a monster gains the level bonus")
        event = {"actor_ref": target, "attacker_player_index": None,
                 "damage": 10}
        level_damage_handler(event)
        bd.assert_true(event["damage"] == 10,
                       "a source-less hit is untouched")
    event = {"actor_ref": pawn, "attacker_player_index": 0, "damage": 10}
    level_damage_handler(event)
    bd.assert_true(event["damage"] == 10,
                   "hits on the player pawn are untouched")
    if target is not None:
        try:
            target.destroy()
        except Exception:
            pass


def autotest_xp_levelup():
    bd.assert_true(character.xp == 100,
                   "kill XP: 25 + 25 + 50 from the monster table")
    events = character.award_xp(250)  # 350 total: level 2 (300), not 3 (900)
    bd.assert_true(character.level == 2 and len(events) == 1,
                   "350 XP reaches exactly level 2")
    bd.assert_true(character.max_hp == 20 and character.hp == 20,
                   "main character level-up applied (+8 HP)")
    bd.assert_true(any(t["kind"] == "quest"
                       and t["text"] == content.TOAST_LEVEL_UP
                       for t in toasts.history),
                   "level-up raised the ember quest toast")


def autotest_door_holds():
    """Phase A: an unreachable DC 30 bash always fails; door stays shut."""
    global door_check, door_drive_until
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "player pawn available")
    if pawn is None:
        return
    # Pre-register the reference fixture through the dispatcher (the same
    # object interactive play would create lazily on the first use).
    door_check = door_bash.bashable_door(content.DOOR_LINE)
    bd.assert_true(door_check is not None,
                   "the line-111 bash check registered")
    door_check.dc = 30  # best possible total is 20+3+2 = 25
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
    door_check.dc = 5  # worst possible total is 1+3+2 = 6
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


def autotest_trap_fail_arm():
    """Arm a controlled guaranteed-fail trap, then leave the entrance."""
    pawn = player_pawn()
    if pawn is None:
        return
    # max dex save total: 20 + 1 = 21 < 30 -> always fails
    autotest_trap_fail_arm.trap = bd_dnd.TrapZone(
        content.TRAP_TAG, character, dc=30, damage="2d6",
        save_ability="dex", once=True)
    pawn.damage_factor = 1.0  # traps must really hurt
    pawn.set_position(*content.AWAY_POS, check=False)


def autotest_trap_fail_spring():
    """Re-enter sector 7; the trap springs on the next tick's sector
    detection (sector_entered is not synchronous with set_position)."""
    pawn = player_pawn()
    trap = getattr(autotest_trap_fail_arm, "trap", None)
    if pawn is None or trap is None:
        return
    autotest_trap_fail_arm.health_before = pawn.health
    pawn.set_position(*content.START_POS, check=False)


def autotest_trap_fail_asserts():
    pawn = player_pawn()
    trap = getattr(autotest_trap_fail_arm, "trap", None)
    bd.assert_true(pawn is not None and trap is not None,
                   "fail-trap fixture available")
    if pawn is None or trap is None:
        return
    outcome = trap.last_outcome
    bd.assert_true(outcome is not None and not outcome["save"]["success"],
                   "fail-trap sprang on sector entry and the save failed")
    if outcome is not None:
        bd.assert_true(outcome["damage_rolled"] == outcome["damage_applied"],
                       "failed save applies full damage")
        bd.assert_true(
            pawn.health == autotest_trap_fail_arm.health_before
            - outcome["damage_dealt"]
            and outcome["damage_dealt"] == outcome["damage_applied"],
            "full trap damage landed on the actor")
    bd.assert_true(character.hp == character.max_hp,
                   "trap damage does not touch the RPG hp pool")


def autotest_trap_save_arm():
    """Arm a controlled guaranteed-save trap, then leave the entrance."""
    pawn = player_pawn()
    if pawn is None:
        return
    # min dex save total: 1 + 1 = 2 >= 1 -> always saves
    autotest_trap_save_arm.trap = bd_dnd.TrapZone(
        content.TRAP_TAG, character, dc=1, damage="2d6",
        save_ability="dex", once=True)
    pawn.set_position(*content.AWAY_POS, check=False)


def autotest_trap_save_spring():
    pawn = player_pawn()
    trap = getattr(autotest_trap_save_arm, "trap", None)
    if pawn is None or trap is None:
        return
    autotest_trap_save_arm.health_before = pawn.health
    pawn.set_position(*content.START_POS, check=False)


def autotest_trap_save_asserts():
    pawn = player_pawn()
    trap = getattr(autotest_trap_save_arm, "trap", None)
    bd.assert_true(pawn is not None and trap is not None,
                   "save-trap fixture available")
    if pawn is None or trap is None:
        return
    outcome = trap.last_outcome
    bd.assert_true(outcome is not None and outcome["save"]["success"],
                   "save-trap sprang and the save succeeded")
    if outcome is not None:
        expected = max(1, outcome["damage_rolled"] // 2)
        bd.assert_true(outcome["damage_applied"] == expected,
                       "successful save halves the damage (minimum 1)")
        bd.assert_true(
            pawn.health == autotest_trap_save_arm.health_before - expected,
            "halved trap damage landed on the actor")
    pawn.damage_factor = 0.0  # test driver is invulnerable again
    # LockedDoorCheck event filtering, with synthetic events: wrong
    # reason / wrong line / non-player actor are all ignored.
    before = door_check.attempts
    door_check._on_activation_failed({"reason": "unknown_special",
                                      "line_index": content.DOOR_LINE,
                                      "actor_ref": pawn})
    door_check._on_activation_failed({"reason": "locked",
                                      "line_index": content.DOOR_LINE + 1,
                                      "actor_ref": pawn})
    door_check._on_activation_failed({"reason": "locked",
                                      "line_index": content.DOOR_LINE,
                                      "actor_ref": None})
    bd.assert_true(door_check.attempts == before,
                   "door check ignores unrelated failure events")


def autotest_dialogue_gate():
    """DialogueSkillGate: MAP02 has no Strife NPCs, so the real
    conversation_reply path cannot fire here; the gate is unit-tested
    through its public handler with a synthetic event (documented in the
    module docstring)."""
    outcomes = []
    gate = bd_dnd.DialogueSkillGate(
        character, log_number=7, skill="persuasion", dc=12,
        on_success=lambda c, r: outcomes.append("success"),
        on_fail=lambda c, r: outcomes.append("fail"))
    gate._on_reply({"log_number": 99, "reply_index": 0})
    bd.assert_true(gate.attempts == 0,
                   "gate ignores replies with other log numbers")
    # cha 8 -> -1, no proficiency: 16-1 = 15 >= 12 succeeds; 9-1 = 8 fails.
    gate.check(rng=_Roller([16]))
    gate.check(rng=_Roller([9]))
    bd.assert_true(outcomes == ["success", "fail"],
                   "gate fires on_success / on_fail in order")
    gate.disarm()
    gate._on_reply({"log_number": 7, "reply_index": 0})
    bd.assert_true(gate.attempts == 2, "disarmed gate is a no-op")


def autotest_damage_save():
    """DamageSaveRule: a scripted hit on the player is refunded on a save.

    The rule is retroactive (the engine cannot cancel damage post-hoc),
    so the assertion is on the heal refund. dc=1: the worst possible DEX
    total is 1 + 1 = 2 >= 1, so the save always succeeds (natural 20s
    included; negate_on_critical would refund *more*, so negation is
    disabled here to keep the refund deterministic).
    """
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "player pawn available")
    if pawn is None:
        return
    rule = bd_dnd.DamageSaveRule(character, dc=1, ability="dex",
                                 damage_type=None, negate_on_critical=False)
    pawn.damage_factor = 1.0
    health_before = pawn.health
    pawn.damage(10)  # source-less scripted hit, damage_type "None"
    bd.assert_true(pawn.health == health_before - 5,
                   "successful damage save refunds half (10 -> 5 kept)")
    result = rule.last_result
    bd.assert_true(result is not None and result["save"]["success"]
                   and result["refunded"] == 5 and not result["negated"]
                   and result["skipped"] is None,
                   "damage save recorded its outcome in last_result")
    # Cooldown: a follow-up hit inside the window is skipped entirely.
    rule.cooldown_tics = 10000
    health_before = pawn.health
    pawn.damage(10)
    bd.assert_true(pawn.health == health_before - 10,
                   "cooldown suppresses the follow-up save (full damage)")
    bd.assert_true(rule.last_result is not None
                   and rule.last_result.get("skipped") == "cooldown",
                   "cooldown skip reported in last_result")
    # Natural 20 with negation enabled refunds the whole hit (direct call
    # with a scripted roller, like TrapZone.spring tests).
    crit_rule = bd_dnd.DamageSaveRule(character, dc=30, ability="dex")
    outcome = crit_rule.apply(pawn, 8, damage_type="Fire", rng=_Roller([20]))
    bd.assert_true(outcome["negated"] and outcome["refunded"] == 8
                   and outcome["save"]["critical"] == "hit",
                   "natural 20 negates the hit entirely")
    # Damage-type filter: a "Fire"-only rule ignores "Ice" damage.
    fire_only = bd_dnd.DamageSaveRule(character, dc=1, ability="dex",
                                      damage_type="Fire")
    bd.assert_true(fire_only._on_actor_damaged(
        {"actor_ref": pawn, "damage": 10, "damage_type": "Ice"}) is None,
        "damage_type filter ignores non-matching damage")
    bd.assert_true(fire_only.last_result is None,
                   "filtered damage never reaches the save")
    pawn.damage_factor = 0.0  # test driver is invulnerable again


def autotest_party():
    """Party: roster, active cursor, shared/solo XP; PartyState armed for
    the checkpoint round-trip (post-load assertions in autotest_post_load).

    CharacterState and PartyState are mutually exclusive in real mods;
    this example deliberately exercises both (the double restore of the
    shared hero is idempotent because both snapshots are taken at the
    same save) to cover both APIs in one run.
    """
    global party, party_state, companion
    party = content.make_party(character)
    warden = party.get(content.WARDEN_NAME)
    bd.assert_true(len(party) == 2 and party.active is character,
                   "party roster with the hero active by default")
    bd.assert_true([m.name for m in party]
                   == [content.HERO_NAME, content.WARDEN_NAME],
                   "party iterates in join order")
    bd.assert_true(party.get(content.WARDEN_NAME) is warden
                   and party.get("Nobody") is None,
                   "party member lookup by name")
    # Shared award: 101 across 2 members -> 50 each, remainder 1 to active.
    events = party.award_xp(101, share=True)
    bd.assert_true(character.xp == 350 + 51 and warden.xp == 50,
                   "shared XP splits evenly, remainder to the active member")
    bd.assert_true(events == {content.HERO_NAME: [],
                              content.WARDEN_NAME: []},
                   "no level-ups from the small shared award")
    # Solo award to the active member: the Warden reaches level 2 (300 XP).
    party.set_active(content.WARDEN_NAME)
    events = party.award_xp(250, share=False)
    bd.assert_true(warden.xp == 300 and warden.level == 2
                   and character.xp == 401,
                   "share=False awards the full amount to the active member")
    bd.assert_true(len(events[content.WARDEN_NAME]) == 1
                   and events[content.WARDEN_NAME][0]["level"] == 2,
                   "level-up events fire per member")
    party_state = bd_dnd.PartyState(party)
    party_state.arm_persistence()

    # Companion: the Warden's crypt hound. damage_factor 0.0 while bound
    # so no roaming MAP02 monster can kill the fixture mid-test (the same
    # guard the test-driver pawn uses); the damage/kill steps re-enable
    # it for their own controlled hits.
    companion = content.make_companion()
    party_state.add_companion(companion)
    systems.wire_knell(companion, companion_died_log)
    bd.assert_true(companion.bind(party), "companion spawned on bind")
    ref = companion.actor()
    bd.assert_true(ref is not None and ref.valid and ref.alive
                   and companion.tid, "companion actor live with a TID")
    if ref is not None:
        bd.assert_true(ref.get_flag("FRIENDLY"),
                       "companion is flagged FRIENDLY")
        bd.assert_true(not ref.get_flag("COUNTKILL"),
                       "companion does not count toward kills")
        bd.assert_true(ref.health == warden.hp,
                       "companion health initialized from the member's hp")
        try:
            master = ref.master
            bd.assert_true(master is not None and master.valid
                           and master.is_player,
                           "companion's master is the player pawn")
        except Exception:
            bd.assert_true(False, "companion master readable")
        ref.damage_factor = 0.0
    bd.assert_true(raises_value_error(
        lambda: bd_dnd.Companion("Nobody").bind(party)),
        "binding an unknown member name raises ValueError")


def autotest_companion_damage():
    """Damage sync, both directions: actor hits lower member.hp;
    member-side set_hp heals the actor through on_hp_changed."""
    warden = party.get(content.WARDEN_NAME)
    ref = companion.actor()
    bd.assert_true(warden is not None and ref is not None and ref.valid
                   and ref.alive, "companion fixture available")
    if warden is None or ref is None:
        return
    ref.damage_factor = 1.0
    hp_before = warden.hp
    dealt = ref.damage(3)  # source-less scripted hit on the companion
    ref.damage_factor = 0.0
    bd.assert_true(dealt == 3 and warden.hp == hp_before - 3,
                   "companion damage lowers the member's RPG hp by 3")
    bd.assert_true(ref.health == warden.hp,
                   "actor health and member hp stay in sync after damage")
    warden.set_hp(warden.max_hp)  # RPG-side heal: the hook heals the actor
    bd.assert_true(ref.health == warden.hp == warden.max_hp,
                   "member-side set_hp heals the companion actor")


def autotest_companion_teleport():
    """Jump the player across the map; the companion must catch up."""
    pawn = player_pawn()
    ref = companion.actor()
    bd.assert_true(pawn is not None and ref is not None and ref.valid,
                   "teleport fixture available")
    if pawn is None or ref is None:
        return
    bd.assert_true(ref.distance_to(pawn) < companion.teleport_distance,
                   "companion near the player before the jump")
    # ~744 units away — beyond teleport_distance (512), far beyond what
    # the 8 u/tic follow thrust could cover before the check.
    pawn.set_position(*content.DOOR_APPROACH, check=False)


def autotest_companion_teleport_check():
    pawn = player_pawn()
    ref = companion.actor()
    bd.assert_true(pawn is not None and ref is not None and ref.valid
                   and ref.alive, "teleport check fixture available")
    if pawn is None or ref is None:
        return
    bd.assert_true(ref.distance_to(pawn) < 256.0,
                   "companion teleported to catch up within 25 tics")


def autotest_companion_kill():
    """Fatal damage: the actor dies, the member drops to hp 0, and the
    deep knell tolls (harm toast + knight/death, DSKNTDTH)."""
    warden = party.get(content.WARDEN_NAME)
    ref = companion.actor()
    bd.assert_true(warden is not None and ref is not None and ref.valid,
                   "kill fixture available")
    if warden is None or ref is None:
        return
    autotest_companion_kill.old_tid = companion.tid
    ref.damage_factor = 1.0
    ref.damage(10000)
    bd.assert_true(companion.dead, "companion marked dead after fatal hit")
    bd.assert_true(warden.hp == 0,
                   "companion death incapacitates the member (hp 0)")
    bd.assert_true(companion_died_log == [content.WARDEN_NAME],
                   "on_companion_died fired with the member")
    bd.assert_true(any(t["kind"] == "harm"
                       and t["text"] == content.knell_toast(
                           content.WARDEN_NAME)
                       for t in toasts.history),
                   "companion death raised the harm knell toast")


def autotest_companion_revive():
    warden = party.get(content.WARDEN_NAME)
    bd.assert_true(companion.revive(), "revive() respawned the companion")
    bd.assert_true(not companion.dead
                   and warden.hp == max(1, (warden.max_hp + 1) // 2),
                   "revive restores half of max hp (rounded up)")
    ref = companion.actor()
    if ref is not None and ref.valid:
        ref.damage_factor = 0.0  # fixture is invulnerable again


def autotest_companion_revive_check():
    warden = party.get(content.WARDEN_NAME)
    ref = companion.actor()
    bd.assert_true(ref is not None and ref.valid and ref.alive,
                   "revived companion alive in the world")
    if ref is None or warden is None:
        return
    bd.assert_true(companion.tid != autotest_companion_kill.old_tid,
                   "revive allocated a fresh TID (no corpse shadowing)")
    autotest_companion_revive_check.revive_tid = companion.tid
    bd.assert_true(ref.health == warden.hp,
                   "revived companion health matches the member's hp")
    bd.assert_true(ref.get_flag("FRIENDLY") and not ref.get_flag("COUNTKILL"),
                   "revived companion keeps its friendly semantics")


def autotest_nightmare():
    """Resting in darkness runs the nightmare: scripted-fail DEX save ->
    a resolve charge is eaten and the harm toast is raised."""
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "nightmare: pawn available")
    if pawn is None:
        return
    pawn.set_position(*content.AWAY_POS, check=False)  # untagged sector 0
    sector = bd.sector_at(pawn.x, pawn.y)
    sector.light = 40  # forced darkness (no light program binds this tag)
    character.set_hp(10)  # max_hp 20: 10 missing
    character.resources[content.NIGHTMARE_RESOURCE] = 1
    # Scripted roller: 3 + dex mod 1 = 4 < DC 12 -> the nightmare wins.
    outcome = systems.try_long_rest(character, rng=_Roller([3]))
    bd.assert_true(outcome["kind"] == "nightmare"
                   and outcome["resource_lost"]
                   and outcome["resource"] == content.NIGHTMARE_RESOURCE,
                   "nightmare branch: failed DEX save loses a resource")
    bd.assert_true(character.resources[content.NIGHTMARE_RESOURCE] == 0,
                   "the dark ate the resolve charge")
    bd.assert_true(character.hp == 10,
                   "a nightmare heals nothing")
    bd.assert_true(any(t["kind"] == "harm"
                       and t["text"] == content.TOAST_NIGHTMARE
                       for t in toasts.history),
                   "nightmare raised the 'dark dreams with you' harm toast")


def autotest_fitful_and_sanctuary():
    """Nightmare success grants half benefit; sanctuary light (>= 160)
    grants the full long rest — on the sheet AND on the pawn."""
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "sanctuary: pawn available")
    if pawn is None:
        return
    sector = bd.sector_at(pawn.x, pawn.y)
    bd.assert_true(int(sector.light) < content.SANCTUARY_LIGHT,
                   "still in darkness for the fitful branch")
    pawn.damage_factor = 1.0
    health_before = pawn.health
    pawn.damage(20)  # a real wound for the fitful/sanctuary heal asserts
    pawn.damage_factor = 0.0
    pawn_wound = health_before - pawn.health
    bd.assert_true(pawn_wound > 0, "the pawn carries a real wound")
    # Scripted roller: 15 + 1 = 16 >= DC 12 -> the sleeper wakes.
    outcome = systems.try_long_rest(character, rng=_Roller([15]))
    bd.assert_true(outcome["kind"] == "fitful" and outcome["healed"] == 5
                   and character.hp == 15,
                   "fitful branch: half the missing HP, no resources")
    bd.assert_true(outcome.get("pawn_healed", 0) > 0
                   and pawn.health > health_before - pawn_wound,
                   "a fitful rest mends the pawn too (half the missing)")
    bd.assert_true(character.resources[content.NIGHTMARE_RESOURCE] == 0,
                   "a survived nightmare does not restore resources")
    bd.assert_true(any(t["kind"] == "info"
                       and t["text"] == content.TOAST_FITFUL
                       for t in toasts.history),
                   "fitful rest raised its toast")
    # Sanctuary: force the light high and the full long rest lands.
    pawn_wounded_health = pawn.health
    sector.light = 200
    outcome = systems.try_long_rest(character)
    bd.assert_true(outcome["kind"] == "sanctuary"
                   and character.hp == character.max_hp
                   and character.resources[content.NIGHTMARE_RESOURCE]
                   == content.NIGHTMARE_RESOURCE_CHARGES,
                   "sanctuary branch: full heal and resources restored")
    bd.assert_true(outcome.get("pawn_healed", 0) > 0
                   and pawn.health > pawn_wounded_health,
                   "a sanctuary rest heals the pawn to full")
    bd.assert_true(character.resources[content.SECOND_WIND_RESOURCE]
                   == content.SECOND_WIND_CHARGES,
                   "the rest refilled the second wind charge")
    bd.assert_true(any(t["kind"] == "quest"
                       and t["text"] == content.TOAST_SANCTUARY
                       for t in toasts.history),
                   "sanctuary rest raised its toast")


def autotest_second_wind_press():
    """Synthetic Custom Action 3 press: the active heal, body and sheet."""
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "second wind: pawn available")
    if pawn is None:
        return
    # The sanctuary rest just refilled the charge and healed everything;
    # wound both pools so the heal has room to land. The wound must come
    # from the real damage path: a direct `pawn.health = ...` write only
    # touches mo->health and desyncs player->health, which the native
    # P_GiveBody heal keys on (probe-verified).
    character.set_hp(10)
    pawn.damage_factor = 1.0
    pawn.damage(40)
    pawn.damage_factor = 0.0
    autotest_second_wind_press.wounded_health = pawn.health
    bd.assert_true(pawn.health < 100, "the pawn carries the wound")
    bd.set_custom_action(3, True)


def autotest_second_wind_asserts():
    pawn = player_pawn()
    bd.assert_true((3, True) in action_log,
                   "the second wind press fired {'action': 3, pressed: True}")
    outcome = last_wind_outcome or {}
    bd.assert_true(outcome.get("ok"), "the second wind spent its charge")
    amount = outcome.get("amount", 0)
    bd.assert_true(3 <= amount <= 12,
                   "the heal rolled d10 + level 2 (3..12)")
    bd.assert_true(character.hp == min(character.max_hp, 10 + amount),
                   "the sheet's Blood pool healed by the roll (clamped)")
    wounded = getattr(autotest_second_wind_press, "wounded_health", None)
    if pawn is not None and wounded is not None:
        bd.assert_true(pawn.health == wounded + amount,
                       f"the pawn healed by the same roll "
                       f"(wounded={wounded}, health={pawn.health}, "
                       f"amount={amount})")
        bd.assert_true(outcome.get("healed_pawn") == amount,
                       "the outcome reports the pawn-side heal")
    bd.assert_true(character.resources.get(content.SECOND_WIND_RESOURCE) == 0,
                   "the charge is spent until the next rest")
    labels = [entry["label"] for entry in character.roll_log]
    bd.assert_true(any("second wind" in label for label in labels),
                   "the heal shows up in the Omens roll log")
    # Exhausted: a second press refuses politely.
    bd.set_custom_action(3, False)
    _run_second_wind()
    bd.assert_true(last_wind_outcome is not None
                   and not last_wind_outcome.get("ok"),
                   "an empty second wind refuses")


def autotest_pre_save():
    """Quiesce the RNG-drawing light programs before the checkpoint.

    See systems.quiesce_lights: light programs re-anchor their task phase
    at load time, which would desync the exact script-RNG stream the
    round-trip asserts. Stopped here, re-armed in autotest_post_load.
    """
    systems.quiesce_lights(horror)
    bd.assert_true(not horror.lights.programs,
                   "light programs quiesced before the checkpoint")


def autotest_mid_asserts():
    bd.assert_true(character.level == 2 and character.xp == 350,
                   "character is level 2 with 350 XP before the save")
    labels = [entry["label"] for entry in character.roll_log]
    bd.assert_true(any("athletics" in label for label in labels)
                   and any("dex save" in label for label in labels)
                   and any("persuasion" in label for label in labels),
                   "roll log recorded the door, trap, and dialogue rolls")
    if not bd.headless():  # imgui_frame never fires under -headless
        bd.assert_true(sheet_drawn_ok,
                       "character sheet drew inside imgui_frame")


def autotest_save():
    # Spend the second wind and wound the RPG pool so the round-trip has
    # nontrivial state to prove; then advance the script RNG stream so the
    # checkpoint captures a nontrivial stream position.
    character.grant_resource("second_wind", 1)
    bd.assert_true(character.use_resource("second_wind"),
                   "second wind spent before the save")
    character.hp = 7
    for _ in range(3):
        bd.randint(1, 1000000)
    bd.save_checkpoint(content.CHECKPOINT_NAME,
                       description="Sunken Crypt autotest")


def autotest_rng_draws():
    # Drawn after the checkpoint was written: these consume stream
    # positions the load will rewind.
    global rng_pair, expected_snapshot
    rng_pair = (bd.randint(1, 1000000), bd.randint(1, 1000000))
    expected_snapshot = character.serialize()


def autotest_load():
    bd.load_checkpoint(content.CHECKPOINT_NAME)


def autotest_post_load():
    redrawn = (bd.randint(1, 1000000), bd.randint(1, 1000000))
    bd.assert_true(rng_pair is not None and redrawn == rng_pair,
                   "script RNG stream resumes exactly after checkpoint load")
    snap = expected_snapshot or {}
    bd.assert_true(character.level == snap.get("level")
                   and character.xp == snap.get("xp")
                   and character.max_hp == snap.get("max_hp")
                   and character.hp == snap.get("hp") == 7,
                   "level/XP/HP round-tripped through the checkpoint")
    bd.assert_true(character.abilities.score("str") == 16
                   and character.abilities.score("con") == 14,
                   "ability scores round-tripped")
    bd.assert_true(character.proficient_skills
                   == set(snap.get("proficient_skills", ()))
                   and character.proficient_saves
                   == set(snap.get("proficient_saves", ())),
                   "proficiencies round-tripped")
    bd.assert_true(character.resources.get("second_wind") == 0
                   and character.resource_max.get("second_wind") == 1,
                   "resource pools round-tripped (spent charge stays spent)")
    bd.assert_true(character.resources.get(content.NIGHTMARE_RESOURCE)
                   == content.NIGHTMARE_RESOURCE_CHARGES,
                   "the resolve pool round-tripped")
    saved = bd.state.get(bd_dnd.STATE_KEY)
    bd.assert_true(isinstance(saved, dict) and "character" in saved,
                   "bd_dnd state present in bd.state after load")
    saved_horror = bd.state.get(bd_horror.STATE_KEY)
    bd.assert_true(isinstance(saved_horror, dict) and "dread" in saved_horror,
                   "bd_horror state present in bd.state after load")
    # Party round-trip: both members' XP/levels and the active cursor.
    bd.assert_true(party is not None and len(party) == 2,
                   "party survived the checkpoint")
    if party is not None:
        warden = party.get(content.WARDEN_NAME)
        bd.assert_true(warden is not None and warden.xp == 300
                       and warden.level == 2,
                       "party member XP/level round-tripped")
        bd.assert_true(character.xp == 401 and character.level == 2,
                       "active member XP round-tripped through PartyState")
        bd.assert_true(party.active is warden,
                       "active-member cursor round-tripped")
        saved_party = bd.state.get(bd_dnd.PARTY_STATE_KEY)
        bd.assert_true(isinstance(saved_party, dict)
                       and "party" in saved_party,
                       "bd_dnd party state present in bd.state after load")
        saved_companions = saved_party.get("companions") \
            if isinstance(saved_party, dict) else None
        bd.assert_true(isinstance(saved_companions, list)
                       and len(saved_companions) == 1
                       and saved_companions[0].get("member_name")
                       == content.WARDEN_NAME
                       and saved_companions[0].get("tid") == companion.tid,
                       "companion descriptor round-tripped with the party")
    # Companion re-bind: the checkpoint restored the world with the
    # follower actor and its TID; map_load adopted it (no respawn — the
    # TID is unchanged) and the Warden's half-hp state survived exactly.
    bd.assert_true(companion is not None and companion.bound
                   and not companion.dead,
                   "companion survived the checkpoint")
    if companion is not None:
        ref = companion.actor()
        warden = party.get(content.WARDEN_NAME) if party is not None else None
        bd.assert_true(ref is not None and ref.valid and ref.alive,
                       "companion re-bound to its actor after the load")
        if ref is not None and warden is not None:
            bd.assert_true(warden.hp == max(1, (warden.max_hp + 1) // 2) == 5
                           and ref.health == warden.hp,
                           "companion and member hp round-tripped")
            bd.assert_true(ref.get_flag("FRIENDLY")
                           and not ref.get_flag("COUNTKILL"),
                           "re-bound companion keeps its flags")
            bd.assert_true(companion.tid
                           == autotest_companion_revive_check.revive_tid,
                           "the saved TID was adopted, not respawned")
    bd.assert_true(bd.sector(content.DOOR_TRACK_SECTOR).ceiling_height > 48.0,
                   "the bashed door is still open after the reload")
    if not bd.headless():  # imgui_frame never fires under -headless
        bd.assert_true(sheet_drawn_ok,
                       "character sheet drew inside imgui_frame")
    # Torch-and-darkness resurrection: with the RNG stream proven, re-arm
    # the crypt's lights and verify they bind again.
    horror.lights.clear()
    systems.arm_crypt_lights(horror)
    report = systems.crypt_light_report(horror)
    bd.assert_true(report["candle"] and report["fluorescent"],
                   "light programs re-armed after the checkpoint reload")
    bd.log("SUNKEN CRYPT AUTOTEST assertions complete")


# --- sheet toggle (pyui / ui_command) ---------------------------------------------


def autotest_sheet_toggle_off():
    # bind_sheet_toggle registered `alias toggle_sheet "pyui toggle_sheet"`
    # at engine_start (before any map load); running the alias fires a
    # ui_command event whose handler flips sheet.visible.
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
    # The alias round-trip above left the sheet visible again.
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
    sanctuary: force the light, wound Morrow, then press."""
    pawn = player_pawn()
    bd.assert_true(pawn is not None, "rest: pawn available")
    if pawn is None:
        return
    sector = bd.sector_at(pawn.x, pawn.y)
    bd.assert_true(sector is not None, "rest: sector resolvable")
    if sector is None:
        return
    sector.light = 200  # forced sanctuary light (the fixture sector is untagged)
    character.set_hp(10)  # max_hp 20: a sanctuary rest must heal 10
    bd.set_custom_action(2, True)


def autotest_custom_action_rest_assert():
    bd.assert_true((2, True) in action_log,
                   "the rest press fired {'action': 2, 'pressed': True}")
    outcome = last_rest_outcome or {}
    bd.assert_true(outcome.get("kind") == "sanctuary",
                   "custom action 2 ran the rest path (sanctuary branch)")
    bd.assert_true(character.hp == character.max_hp,
                   "the custom-action rest healed Morrow fully")
    bd.set_custom_action(2, False)


def autotest_custom_action_rest_release():
    bd.assert_true((2, False) in action_log,
                   "the rest release fired {'action': 2, 'pressed': False}")


# --- screenshot helper ---------------------------------------------------------


def screenshot_flavor():
    # Populate the roll log so the reliquary's Omens show their colors,
    # wound the blood bar, and refresh the progression strip.
    character.skill_check("athletics", 15)          # the door bash
    character.saving_throw("dex", 13)               # the dart trap
    character.skill_check("perception", 12)
    character.skill_check("persuasion", 16)
    character.saving_throw("str", 20)
    character.skill_check("athletics", 28)          # a likely failure
    character.set_hp(max(1, character.max_hp * 2 // 5))
    character.use_resource(content.NIGHTMARE_RESOURCE)
    systems.refresh_progress_strip(character)


# --- imgui overlay -------------------------------------------------------------


@bd.on("imgui_frame")
def draw_ui(event):
    global autowarp_done, sheet_drawn_ok
    # Headless runs (-scripttest) launch without +map: queue a warp on the
    # first rendered frame when nothing loaded a level yet (same pattern as
    # 26_imgui_overlays / 27_quest_journal / 28_vtm_chronicle). MAP02 is
    # only the documentation default — the crypt's probed set pieces live
    # there, but the rules layer runs on any map the player loads.
    if not autowarp_done:
        autowarp_done = True
        try:
            if not bd.current_map():
                bd.execute("map map02")
        except RuntimeError:
            pass
    ui.draw_frame()
    sheet_drawn_ok = sheet_drawn_ok or bool(ui.ctx.get("sheet_drawn_ok"))
