"""Ashvale Crossing - thin bootstrap, the headless autotest, the screenshot pose.

Deliberately small: classes/content live in :mod:`ashvale_content`, rules and
engine wiring in :mod:`ashvale_systems`, and every window in
:mod:`ashvale_ui`. What remains here is the deterministic autotest
(``BD_EXAMPLE_AUTOTEST=1``) and the documentation screenshot pose
(``BD_EXAMPLE_SCREENSHOT=1``).

The autotest drives the real engine paths headlessly with scripted RNG
doubles and asserts, in order: creation validation errors and a full
Mercenary founding; every class preset round-tripping through
``content.apply_preset`` into a valid founding; unit-level class progression
through level 4 with the ASI queue and the level-2 resource raises; the
spawned hub (four friendly tinted NPCs in talk range, the cache, no yard
before the quest, the initial marker bookkeeping); nearest-NPC targeting;
the real ``bind e talk -> pyui talk -> ui_command`` path into a
disposition-carrying session; the quest handout (quest active, Sera +10,
five AMBUSH zombies up, the Sera "!" swapped for the yard beacon); both
persuasion branches through ``session.rng`` (the rumor starts watch_grows
only on success); the Dobb fetch-accept dialogue (quest active, cache
beacon up); the shop (buy/sell math, stock counts, a scheduled restock);
the healer (restore, fee, refusal when broke); the trainer (scripted
mastery advancement); the quest flow (pawn-sourced kills complete the yard,
kill XP and quest XP land, the level-2 class feature raises Second Wind to
two charges, Sera shifts +20); Sera's post-yard trust choice starting
watch_grows; the cache walk completing the fetch; recruitment (Korr retired
from the hub, one companion actor at his slot, watch_grows completed);
companion combat (the follower marks and targets a monster the player
hurt); the hero damage filter on a synthetic event; the Mercenary class
active (heal on sheet and pawn, charge spent, the no-charge message); a
checkpoint round-trip (RNG stream, hero, dispositions, shop stock, party,
companion rebind, NPC TIDs, the retired set, quest states, the HUD strip
task, cleared markers); and the synthetic Custom Action path (action 1
press starts a talk session with the documented event payload, action 2
press/release flips the hero sheet both ways).
The run ends via ``-scripttest``'s own PASS/FAIL accounting.

Manifest entry 4 of 4 (runs after its siblings have self-registered).
"""

import os

import biaseddoom as bd
import bd_dialogue
import bd_dnd
import bd_quests
from bd_quests import Quest

try:
    import ashvale_content as content
    import ashvale_systems as systems
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="ashvale_content")
    systems = bd.import_script("pyscripts/systems.py",
                               module_name="ashvale_systems")

AUTOTEST = os.environ.get("BD_EXAMPLE_AUTOTEST") == "1"
SCREENSHOT = os.environ.get("BD_EXAMPLE_SCREENSHOT") == "1"

# The interactive founding pause (world freeze while the creation wizard is
# open) is driven by rendered frames; the scheduled drivers above tick the
# world, so a pause would freeze their steps. Keep both paths alive.
if AUTOTEST or SCREENSHOT:
    systems.creation_pause_enabled = False

# Snapshots captured before the checkpoint save (post-load asserts compare).
_pre_save = {}


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


def _choice_index(session, text):
    """Visible index of the choice with exactly this text (or -1)."""
    for index, (choice, _enabled, _ann) in enumerate(session.choices()):
        if choice.text == text:
            return index
    return -1


def _teleport_to(npc_id, offset=None):
    """Teleport the pawn beside a registered NPC (returns the NPC ref).

    The side matters: each NPC got a fit-probed slot, and teleporting onto
    the wrong shoulder lands the pawn closer to the neighbor (the hall is
    tight). Sera stands east of her spot's approach, everyone else west.
    """
    pawn = systems.player_pawn()
    ref = systems.manager.actor_for(npc_id)
    if pawn is None or ref is None:
        return None
    if offset is None:
        offset = 48.0 if npc_id == "sera" else -48.0
    pawn.set_position(ref.x + offset, ref.y, ref.z, check=False)
    return ref


# --- run-mode scheduling ----------------------------------------------------------------


@bd.on("map_load")
def schedule_run_modes(event):
    if AUTOTEST and event.get("from_savegame"):
        bd.schedule(autotest_post_load, delay=10)
        bd.schedule(autotest_custom_action_talk_press, delay=45)
        bd.schedule(autotest_custom_action_talk_asserts, delay=57)
        bd.schedule(autotest_custom_action_talk_release, delay=69)
        bd.schedule(autotest_custom_action_sheet_press, delay=81)
        bd.schedule(autotest_custom_action_sheet_asserts, delay=93)
        bd.schedule(autotest_custom_action_sheet_restore, delay=105)
        bd.schedule(autotest_custom_action_sheet_back, delay=117)
        bd.schedule(autotest_finish, delay=135)
        return
    if not event.get("from_savegame"):
        if AUTOTEST:
            pawn = systems.player_pawn()
            if pawn is not None:
                pawn.damage_factor = 0.0  # nothing may kill the test driver
            bd.schedule(autotest_creation, delay=10)
            bd.schedule(autotest_presets, delay=18)
            bd.schedule(autotest_class_progression, delay=25)
            bd.schedule(autotest_spawns, delay=40)
            bd.schedule(autotest_nearest_talk, delay=55)
            bd.schedule(autotest_session_asserts, delay=75)
            bd.schedule(autotest_quest_handout, delay=95)
            bd.schedule(autotest_markers_after_accept, delay=112)
            bd.schedule(autotest_persuasion, delay=125)
            bd.schedule(autotest_watch_marker_reset, delay=140)
            bd.schedule(autotest_fetch_accept, delay=150)
            bd.schedule(autotest_fetch_marker, delay=160)
            bd.schedule(autotest_shop, delay=175)
            bd.schedule(autotest_healer, delay=220)
            bd.schedule(autotest_trainer, delay=250)
            bd.schedule(autotest_quest_flow, delay=280)
            bd.schedule(autotest_sera_watch_choice, delay=300)
            bd.schedule(autotest_markers_after_watch, delay=308)
            bd.schedule(_walk_onto_cache, delay=315)
            bd.schedule(autotest_fetch_cache_asserts, delay=330)
            bd.schedule(autotest_recruit, delay=345)
            bd.schedule(autotest_companion_combat_mark, delay=362)
            bd.schedule(autotest_companion_combat_assert, delay=378)
            bd.schedule(autotest_class_active_no_charge, delay=395)
            bd.schedule(autotest_class_active_no_charge_asserts, delay=407)
            bd.schedule(autotest_class_active_press, delay=419)
            bd.schedule(autotest_class_active_asserts, delay=431)
            bd.schedule(autotest_pre_save, delay=448)
            bd.schedule(autotest_load, delay=475)
        if SCREENSHOT:
            screenshot_wizard_pose()
            bd.schedule(screenshot_pose, delay=bd.TICRATE,
                        map_local=False)
            bd.schedule(lambda: bd.execute("screenshot /tmp/ashvale"),
                        delay=2 * bd.TICRATE + 20, map_local=False)
            bd.schedule(lambda: bd.execute("quit"), delay=4 * bd.TICRATE,
                        map_local=False)


def screenshot_pose():
    """Pose the documentation shot: creation wizard + a live conversation."""
    _teleport_to("sera")
    bd.execute("talk")


def screenshot_wizard_pose():
    """Drive the shared wizard into a photogenic state before the first
    frame: a chosen class renders the briefing panel and the preset row,
    and the applied preset shows the annotated scores. Runs at map_load so
    the founding window's first-use autosize fits the full content."""
    if systems.hero is not None or systems.wizard is None:
        return
    try:
        systems.wizard.choose_class(content.MERCENARY)
        content.apply_preset(systems.wizard, content.CLASS_PRESETS[
            content.MERCENARY.name][0])
    except Exception:
        pass


# --- autotest steps ---------------------------------------------------------------------


def autotest_creation():
    """Wizard validation errors, then a full Mercenary founding."""
    bd.assert_true("bd_npcs" in str(systems.manager.__class__.__module__),
                   "NPCManager is a bd_npcs class")
    bd.assert_true(systems.hero is None, "no hero before creation")

    bare = bd_dnd.CreationWizard()
    bd.assert_true(raises_value_error(bare.finish),
                   "finish without a name raises ValueError")
    bare.set_name("X")
    bd.assert_true(raises_value_error(bare.finish),
                   "finish without a class raises ValueError")
    bare.choose_class(content.MERCENARY)
    bd.assert_true(raises_value_error(bare.finish),
                   "finish without a score method raises ValueError")
    bare.use_standard_array()
    for ability in bd_dnd.ABILITIES:
        bare.set_score(ability, 15)
    bd.assert_true(raises_value_error(bare.finish),
                   "the standard array multiset is enforced")
    bare2 = bd_dnd.CreationWizard()
    bare2.set_name("Y")
    bare2.choose_class(content.MERCENARY)
    bare2.use_standard_array()
    for ability, value in content.SUGGESTED_ARRAYS["Mercenary"].items():
        bare2.set_score(ability, value)
    bd.assert_true(raises_value_error(lambda: bare2.assign_skill("stealth")),
                   "a non-class skill is rejected")
    bare2.assign_skill("athletics")
    bare2.assign_skill("intimidation")
    bare2.assign_skill("perception")
    scout_wiz = bd_dnd.CreationWizard()
    scout_wiz.set_name("Q")
    scout_wiz.choose_class(content.SCOUT)
    scout_wiz.use_standard_array()
    for ability, value in content.SUGGESTED_ARRAYS["Scout"].items():
        scout_wiz.set_score(ability, value)
    for skill in ("stealth", "sleight_of_hand", "acrobatics"):
        scout_wiz.assign_skill(skill)
    bd.assert_true(raises_value_error(
        lambda: scout_wiz.assign_skill("perception")),
        "the class skill quota (3 of 4 for the Scout) is enforced")
    buy = bd_dnd.CreationWizard()
    buy.set_name("Z")
    buy.choose_class(content.MERCENARY)
    buy.use_point_buy()
    for ability in bd_dnd.ABILITIES:
        buy.set_score(ability, 15)
    bd.assert_true(raises_value_error(buy.finish),
                   "point buy overspending raises ValueError")
    s1, s2 = bd.rng(42), bd.rng(42)
    bd.assert_true([s1.int(1, 20) for _ in range(8)]
                   == [s2.int(1, 20) for _ in range(8)],
                   "twin rng streams produce identical sequences")

    # The real founding, through the same wizard the UI drives.
    pawn = systems.player_pawn()
    clips_before = pawn.inventory_count("Clip") if pawn is not None else 0
    shells_before = pawn.inventory_count("Shell") if pawn is not None else 0
    wiz = systems.wizard
    wiz.choose_class(content.MERCENARY)
    wiz.set_name(content.HERO_DEFAULT_NAME)
    wiz.use_standard_array()
    for ability, value in content.SUGGESTED_ARRAYS["Mercenary"].items():
        wiz.set_score(ability, value)
    for skill in ("athletics", "intimidation", "perception"):
        wiz.assign_skill(skill)
    hero = systems.finish_creation(wiz)
    bd.assert_true(hero is systems.hero, "finish_creation stores the hero")
    bd.assert_true(systems.hero_damage_filter is not None,
                   "hero progression is wired (kill XP + damage filter)")
    bd.assert_true(hero.name == content.HERO_DEFAULT_NAME
                   and hero.class_id == "Mercenary",
                   "the hero is a bound Mercenary")
    bd.assert_true(hero.hit_die == 10 and hero.max_hp == 12,
                   "d10 hit die, max hp 12 (10 + con 14)")
    bd.assert_true(hero.resources.get("second_wind") == 1
                   and hero.resource_max.get("second_wind") == 1,
                   "the level-1 Second Wind feature granted its charge")
    bd.assert_true(hero.proficient_skills
                   == {"athletics", "intimidation", "perception"},
                   "the three class skills are proficient")
    pawn = systems.player_pawn()
    if pawn is not None:
        # Ammo persists in inventory (the pistol start carries 50 Clip);
        # armor is consumed by the native pickup path and worn instead, so
        # only the ammo counts are assertable here.
        bd.assert_true(pawn.inventory_count("Clip") == clips_before + 2
                       and pawn.inventory_count("Shell") == shells_before + 4,
                       "Mercenary starting ammo was handed out")


def autotest_presets():
    """Every class preset applies through the shared helper and finishes.

    The UI's preset buttons call ``content.apply_preset`` on the shared
    wizard; this drives the same helper over fresh wizards, so a broken
    preset (a bad multiset, a non-class skill, an over-quota pick) fails
    here before a player can click it.
    """
    bd.assert_true(set(content.CLASS_PRESETS)
                   == {cls.name for cls in content.CLASS_LIST},
                   "every class ships presets")
    for cls in content.CLASS_LIST:
        presets = content.CLASS_PRESETS[cls.name]
        bd.assert_true(len(presets) >= 2,
                       f"{cls.name} offers a real choice of presets")
        briefing = content.class_briefing(cls)
        bd.assert_true(briefing["hit_die"] == f"d{cls.hit_die}"
                       and briefing["features"],
                       f"{cls.name} briefing matches the live class")
        seen_ids = set()
        for preset in presets:
            bd.assert_true(preset["id"] not in seen_ids,
                           f"{cls.name}/{preset['id']}: unique preset id")
            seen_ids.add(preset["id"])
            bd.assert_true(sorted(preset["scores"].values())
                           == sorted(content.STANDARD_ARRAY),
                           f"{cls.name}/{preset['id']}: standard array used "
                           "exactly once")
            bd.assert_true(all(skill in cls.class_skills
                               for skill in preset["skills"]),
                           f"{cls.name}/{preset['id']}: preset skills are "
                           "class skills")
            wiz = bd_dnd.CreationWizard()
            wiz.set_name("P")
            wiz.choose_class(cls)
            content.apply_preset(wiz, preset)
            bd.assert_true(wiz.method == "standard_array",
                           f"{cls.name}/{preset['id']}: preset sets the "
                           "standard array method")
            built = wiz.finish()  # raises ValueError on an invalid preset
            bd.assert_true(built.abilities.serialize()
                           == dict(preset["scores"]),
                           f"{cls.name}/{preset['id']}: preset scores landed")
            bd.assert_true(set(preset["skills"])
                           == set(built.proficient_skills),
                           f"{cls.name}/{preset['id']}: preset skills landed")
            bd.assert_true(built.class_id == cls.name,
                           f"{cls.name}/{preset['id']}: the class bound")


def autotest_class_progression():
    """Unit-level progression: features at 1 and 2, ASI queue at 4."""
    wiz = bd_dnd.CreationWizard()
    wiz.choose_class(content.MERCENARY)
    wiz.set_name("Unit")
    wiz.use_point_buy()
    for ability, value in content.SUGGESTED_ARRAYS["Mercenary"].items():
        wiz.set_score(ability, value)
    for skill in ("athletics", "intimidation", "perception"):
        wiz.assign_skill(skill)
    unit = wiz.finish()
    bd.assert_true(unit.level == 1 and unit.xp == 0
                   and unit.resource_max.get("second_wind") == 1,
                   "level-1 class package applied at creation")
    bd.assert_true(unit.award_xp(299) == [] and unit.level == 1,
                   "299 XP does not level")
    events = unit.award_xp(1)  # 300 exactly: level 2
    bd.assert_true(unit.level == 2 and len(events) == 1,
                   "300 XP reaches level 2")
    bd.assert_true(unit.resource_max.get("second_wind") == 2,
                   "the level-2 Press On feature raised Second Wind to "
                   "two charges")
    unit.award_xp(1800)  # 2100 total: level 3
    bd.assert_true(unit.level == 3, "2100 XP reaches level 3")
    unit.award_xp(600)  # 2700 total: level 4, ASI queued
    bd.assert_true(unit.level == 4, "2700 XP reaches level 4")
    pending = getattr(unit, "pending_asi", None)
    bd.assert_true(isinstance(pending, list) and len(pending) == 1
                   and pending[0].get("points") == 2
                   and pending[0].get("spent") == 0,
                   "level 4 queues two pending ASI points")
    bd.assert_true(unit.proficiency == 2, "proficiency stays +2 at level 4")
    # The Scout's level-1 feature is a pure mod note; the blur charge is
    # the level-2 feature, so the active exists only from level 2.
    scout = bd_dnd.Character("S", bd_dnd.AbilityScores(dex=15), hit_die=8)
    bd_dnd.bind_class(scout, content.SCOUT)
    bd.assert_true(getattr(scout, "mod_notes", {}).get("skirmisher_damage")
                   == 1 and scout.hit_die == 8,
                   "the Scout's Skirmisher note applies at level 1")
    bd.assert_true(scout.resource_max.get("uncanny_step", 0) == 0,
                   "the Scout has no blur charge at level 1")
    bd_dnd.apply_class_level(scout, content.SCOUT, 2)
    bd.assert_true(scout.resource_max.get("uncanny_step") == 1,
                   "Uncanny Step grants the blur charge at level 2")
    # The Lightkeeper's level-2 feature raises the light pool.
    keeper = bd_dnd.Character("L", bd_dnd.AbilityScores(wis=15), hit_die=8)
    bd_dnd.bind_class(keeper, content.LIGHTKEEPER)
    bd.assert_true(keeper.resource_max.get("light") == 3,
                   "the Lightkeeper starts with three light charges")
    bd_dnd.apply_class_level(keeper, content.LIGHTKEEPER, 2)
    bd.assert_true(keeper.resource_max.get("light") == 4,
                   "Warding Flame raises the light pool to four")


def autotest_spawns():
    """The hub: four friendly tinted NPCs in range, cache, empty yard."""
    pawn = systems.player_pawn()
    bd.assert_true(pawn is not None, "player pawn available")
    for npc_id in ("sera", "dobb", "wren", "korr"):
        ref = systems.manager.actor_for(npc_id)
        bd.assert_true(ref is not None and ref.valid and ref.alive,
                       f"{npc_id} is live")
        if ref is None:
            continue
        bd.assert_true(ref.get_flag("FRIENDLY"),
                       f"{npc_id} is friendly")
        bd.assert_true(ref.tid == content.NPC_TID_BASE
                       + ("sera", "dobb", "wren", "korr").index(npc_id),
                       f"{npc_id} carries its stable tid_base TID")
        if pawn is not None:
            bd.assert_true(pawn.distance_to(ref) <= content.TALK_RANGE,
                           f"{npc_id} spawns within talk range")
        if npc_id == "sera":
            bd.assert_true(tuple(ref.tint) == (0xC0, 0x70, 0x20),
                           "sera's tint applied as (r, g, b)")
        ref.damage_factor = 0.0  # the fixture cast is invulnerable
    cache = bd.actor_refs(content.GAME_CONTENT["cache_class"])
    bd.assert_true(len(cache) >= 1, "the buried cache spawned in the west hall")
    bd.assert_true(bd.actor_ref(content.YARD_TIDS[0]) is None,
                   "the yard is empty before the quest is accepted")
    bd.assert_true(systems.currency_ready
                   and systems.currency_class in content.CURRENCY_CLASSES,
                   "the currency spawn probe resolved a class")
    clear = bd_quests.log.get(content.QUEST_CLEAR_YARD)
    bd.assert_true(clear is not None and clear.state == Quest.INACTIVE,
                   "clear_yard registered and inactive")
    fetch = bd_quests.log.get(content.QUEST_FETCH_CACHE)
    bd.assert_true(fetch is not None and fetch.state == Quest.INACTIVE,
                   "fetch_cache registered and inactive (no auto-start)")
    watch = bd_quests.log.get(content.QUEST_WATCH_GROWS)
    bd.assert_true(watch is not None and watch.state == Quest.INACTIVE,
                   "watch_grows registered and inactive")
    bd.assert_true(bd_quests.log.get("prove_worth") is None,
                   "the old hidden shadow quest is gone")
    # The opening markers: gold "!" over both quest givers, no beacons yet.
    bd.assert_true(systems.marker_state.get(content.MARKER_SERA_ID) == "sera",
                   "the Sera marker shows while clear_yard waits")
    bd.assert_true(systems.marker_state.get(content.MARKER_DOBB_ID) == "dobb",
                   "the Dobb marker shows while fetch_cache waits")
    bd.assert_true(content.MARKER_KORR_ID not in systems.marker_state
                   and content.MARKER_YARD_BEACON_ID not in systems.marker_state
                   and content.MARKER_CACHE_BEACON_ID not in systems.marker_state,
                   "no Korr marker and no beacons before any quest runs")


def autotest_nearest_talk():
    """Nearest targeting, then the real console path into a session."""
    _teleport_to("dobb")
    definition, _handle = systems.manager.nearest(systems.player_pawn())
    bd.assert_true(definition is not None and definition.id == "dobb",
                   "nearest picks Dobb from his spot")
    _teleport_to("sera")
    definition, _handle = systems.manager.nearest(systems.player_pawn())
    bd.assert_true(definition is not None and definition.id == "sera",
                   "nearest picks Sera from hers")
    bd.execute("talk")  # alias talk -> pyui talk -> ui_command (later tic)


def autotest_session_asserts():
    """The talk command opened a disposition-carrying session with Sera."""
    active = bd_dialogue.active_session()
    bd.assert_true(active is not None and active.active,
                   "talk command started a session")
    bd.assert_true(systems.current_session() is active,
                   "the example session is the active one (UI reads it)")
    if active is None:
        return
    bd.assert_true(active.active_node is not None
                   and active.active_node.id == "start",
                   "session opens at the start node")
    ctx = active.context()
    bd.assert_true(ctx.get("npc_id") == "sera"
                   and ctx.get("dispositions") is systems.manager.dispositions
                   and ctx.get("disposition") == 0
                   and ctx.get("standing") == "neutral",
                   "the session ctx carries the disposition keys")
    bd.assert_true(ctx.get("character") is systems.hero,
                   "the hero is the session character")
    bd.assert_true(ctx.get("quest_log") is bd_quests.log,
                   "the shared quest log rides the ctx")
    entries = active.choices()
    rumor = next((ann for _c, _en, ann in entries
                  if _c.text == "What really happened at the bridge?"), None)
    bd.assert_true(len(entries) == 4 and rumor == "(DC 12 Persuasion)",
                   "four choices; the rumor shows its DC annotation")
    work = _choice_index(active, "I need work.")
    bd.assert_true(work >= 0, "the quest handout is visible pre-accept")


def autotest_quest_handout():
    """Accepting the yard job: quest active, Sera +10, five zombies up."""
    active = bd_dialogue.active_session()
    bd.assert_true(active is not None and active.active,
                   "session live for the handout")
    if active is None:
        return
    active.choose(_choice_index(active, "I need work."))
    clear = bd_quests.log.get(content.QUEST_CLEAR_YARD)
    bd.assert_true(clear is not None and clear.state == Quest.ACTIVE,
                   "accepting starts clear_yard")
    bd.assert_true(active.active_node is not None
                   and active.active_node.id == "errand",
                   "the handout routes to the errand node")
    bd.assert_true(bd.player_log() == content.ACCEPT_LOG_TEXT,
                   "the handout choice wrote the native player log")
    bd.assert_true(systems.manager.dispositions.get("sera")
                   == content.ACCEPT_DISPOSITION_DELTA,
                   "accepting shifted Sera's disposition by +10")
    alive = 0
    for tid in content.YARD_TIDS:
        ref = bd.actor_ref(tid)
        if ref is not None and ref.valid and ref.alive:
            alive += 1
    bd.assert_true(alive == 5, "five AMBUSH yard zombies spawned")
    active.choose(0)  # "It will be done." ends the conversation
    bd.assert_true(not active.active and bd_dialogue.active_session() is None,
                   "the errand exit ends the session")


def autotest_markers_after_accept():
    """After the handout: Sera's "!" is gone, the yard beacon stands."""
    bd.assert_true(content.MARKER_SERA_ID not in systems.marker_state,
                   "the Sera marker cleared once clear_yard runs")
    bd.assert_true(systems.marker_state.get(content.MARKER_YARD_BEACON_ID)
                   == "yard_beacon",
                   "the yard beacon shows while clear_yard runs")
    bd.assert_true(systems.marker_state.get(content.MARKER_DOBB_ID) == "dobb",
                   "the Dobb marker still shows (fetch not yet accepted)")


def autotest_persuasion():
    """Both rumor branches through session.rng: only success starts
    watch_grows (pointing at Korr) and writes the rumor log line."""
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the persuasion branches")
    if session is None:
        return
    rumor_text = "What really happened at the bridge?"
    session.rng = _Roller([12])  # 12 + 0 (cha 10) + 0 = 12 >= 12
    result = session.choose(_choice_index(session, rumor_text))
    bd.assert_true(isinstance(result, dict) and result["success"]
                   and result["total"] == 12,
                   "scripted roller drives the success branch (12 vs DC 12)")
    bd.assert_true(session.active_node.id == "rumor_ok",
                   "success routes to the rumor_ok node")
    watch = bd_quests.log.get(content.QUEST_WATCH_GROWS)
    bd.assert_true(watch is not None and watch.state == Quest.ACTIVE,
                   "the rumor effect started watch_grows")
    bd.assert_true(bd.player_log() == content.RUMOR_LOG_TEXT,
                   "the rumor effect wrote the player log")
    session.end()

    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session restarted for the failure branch")
    if session is None:
        return
    session.rng = _Roller([4])  # 4 + 0 + 0 = 4 < 12
    result = session.choose(_choice_index(session, rumor_text))
    bd.assert_true(isinstance(result, dict) and not result["success"]
                   and result["total"] == 4,
                   "scripted roller drives the failure branch (4 vs DC 12)")
    bd.assert_true(session.active_node.id == "rumor_no",
                   "failure routes to the refusal node")
    watch = bd_quests.log.get(content.QUEST_WATCH_GROWS)
    bd.assert_true(watch is not None and watch.state == Quest.ACTIVE,
                   "a failed rumor does not disturb watch_grows")
    session.end()


def autotest_watch_marker_reset():
    """Korr's "!" rides watch_grows; then reset the quest for the Sera path.

    The reset is a plain attribute write: quest starts are one-way by
    design, and the autotest still owes the post-yard Sera choice an
    INACTIVE watch_grows to fire on. No hooks are bypassed (start only
    flips state); the objective is untouched.
    """
    bd.assert_true(systems.marker_state.get(content.MARKER_KORR_ID) == "korr",
                   "the Korr marker shows while watch_grows is open")
    watch = bd_quests.log.get(content.QUEST_WATCH_GROWS)
    bd.assert_true(watch is not None and watch.state == Quest.ACTIVE,
                   "watch_grows active before the reset")
    if watch is not None:
        watch.state = Quest.INACTIVE


def autotest_fetch_accept():
    """Dobb's errand: the real dialogue choice starts fetch_cache."""
    _teleport_to("dobb")
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session with Dobb open for the errand")
    if session is None:
        return
    index = _choice_index(session, "Anything you need moved?")
    bd.assert_true(index >= 0, "the cache errand is visible while inactive")
    if index < 0:
        session.end()
        return
    session.choose(index)
    fetch = bd_quests.log.get(content.QUEST_FETCH_CACHE)
    bd.assert_true(fetch is not None and fetch.state == Quest.ACTIVE,
                   "accepting the errand started fetch_cache")
    bd.assert_true(session.active_node is not None
                   and session.active_node.id == "cache_where",
                   "the errand routes to the cache_where node")
    bd.assert_true(bd.player_log() == content.FETCH_LOG_TEXT,
                   "the errand choice wrote the native player log")
    session.choose(0)  # "Consider it moved." ends the conversation
    bd.assert_true(not session.active, "the errand exit ends the session")


def autotest_fetch_marker():
    """The cache beacon stands and Dobb's "!" is gone while fetch runs."""
    bd.assert_true(systems.marker_state.get(content.MARKER_CACHE_BEACON_ID)
                   == "cache_beacon",
                   "the cache beacon shows while fetch_cache runs")
    bd.assert_true(content.MARKER_DOBB_ID not in systems.marker_state,
                   "the Dobb marker cleared once fetch_cache runs")


def autotest_shop():
    """Buy/sell math, stock counts, and the scheduled restock.

    The consumable buys (ClipBox/Stimpack/GreenArmor) exercise the
    AshvaleShop delivery workaround: the native pickup path folds ClipBox
    into the Clip ammo count, heals with the stim, and wears the armor, so
    none of them persist under their own class name.
    """
    _teleport_to("dobb")
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session with Dobb open")
    if session is None:
        return
    bd.assert_true(session.context().get("npc_id") == "dobb",
                   "the session ctx names dobb")
    session.choose(_choice_index(session, "Show me your wares."))
    bd.assert_true(systems.shop_open, "the trade choice opened the shop")
    bd.assert_true(session.active_node.id == "trading",
                   "the trade choice routes to the trading node")

    pawn = systems.player_pawn()
    shop = systems.dobb_shop
    currency = systems.currency_class
    bd.assert_true(pawn is not None and shop is not None, "shop fixture up")
    if pawn is None or shop is None:
        return
    pawn.give_inventory(currency, 200)
    start_currency = shop.currency(pawn)
    bd.assert_true(start_currency == 200, "the test purse holds 200")

    stock_names = [entry["class_name"] for entry in shop.stock]
    shell_index = stock_names.index("Shell")
    shells_before = pawn.inventory_count("Shell")
    clips_before = pawn.inventory_count("Clip")
    tasks_before = bd.task_count()
    result = shop.buy(pawn, shell_index)
    bd.assert_true(result["ok"], "buying a shell succeeds")
    bd.assert_true(shop.count_of(shell_index) == 11,
                   "the shell stock decremented (12 -> 11)")
    bd.assert_true(shop.currency(pawn) == start_currency - 8,
                   "the shell price was paid")
    bd.assert_true(pawn.inventory_count("Shell") == shells_before + 1,
                   "the shell landed in inventory")
    bd.assert_true(bd.task_count() == tasks_before + 1,
                   "a restock task was scheduled for the shell")
    shop._restock(shell_index)  # the scheduled task calls exactly this
    bd.assert_true(shop.count_of(shell_index) == 12,
                   "the restock task restores the count to max")

    clip_index = stock_names.index("ClipBox")
    result = shop.buy(pawn, clip_index)
    bd.assert_true(result["ok"] and shop.count_of(clip_index) == 5,
                   "buying a ClipBox succeeds (worked-around delivery)")
    bd.assert_true(pawn.inventory_count("Clip") > clips_before,
                   "the ClipBox folded into the parent Clip ammo count")
    stim_index = stock_names.index("Stimpack")
    result = shop.buy(pawn, stim_index)
    bd.assert_true(result["ok"] and shop.count_of(stim_index) == 7,
                   "buying a stimpack succeeds (health-capped delivery)")
    armor_index = stock_names.index("GreenArmor")
    result = shop.buy(pawn, armor_index)
    bd.assert_true(result["ok"] and shop.count_of(armor_index) == 0,
                   "the one-off armor sells and is worn (no restock)")
    bd.assert_true(shop.currency(pawn) == start_currency - 8 - 15 - 12 - 90,
                   "every purchase was paid for")

    sold = shop.sell(pawn, "Shell")
    bd.assert_true(sold["ok"] and sold["price"] == 4,
                   "selling a shell pays half price (4)")
    refused = shop.sell(pawn, "ClipBox")
    bd.assert_true(not refused["ok"],
                   "no ClipBox item exists to sell (it folds into Clip ammo)")
    refused = shop.sell(pawn, "Stimpack")
    bd.assert_true(not refused["ok"],
                   "the shop refuses to buy what it does not list")
    bd.assert_true(shop.currency(pawn)
                   == start_currency - 8 - 15 - 12 - 90 + 4,
                   "the purse matches every transaction")
    session.end()
    bd.assert_true(not systems.shop_open,
                   "ending the conversation closes the shop")


def autotest_healer():
    """Wren: heal, fee, charge-restoring breather, and the broke refusal."""
    _teleport_to("wren")
    pawn = systems.player_pawn()
    bd.assert_true(pawn is not None, "healer: pawn available")
    if pawn is None:
        return
    pawn.damage_factor = 1.0  # the wound must be real
    health_before = pawn.health
    pawn.damage(25)
    # The starting GreenArmor absorbs part of the hit (probe-verified: 25
    # keeps 17), so the assertion is "a real wound landed", not the raw 25.
    bd.assert_true(pawn.health < health_before,
                   "the test wound landed (armor-absorbed)")
    # Spend a class-active charge so the breather restore has room to show.
    hero = systems.hero
    spent_resource = None
    if hero is not None and hero.resources.get("second_wind", 0) > 0:
        hero.use_resource("second_wind")
        spent_resource = "second_wind"
        bd.assert_true(hero.resources["second_wind"] == 0,
                       "the second_wind charge is spent for the test")
    currency = systems.currency_class
    pawn.give_inventory(currency, 50)
    purse = systems.dobb_shop.currency(pawn)
    result = systems.run_healer()
    bd.assert_true(result.get("ok"), "the healer takes the patient")
    bd.assert_true(pawn.health == health_before,
                   "the healer restored the pawn to full")
    bd.assert_true(systems.dobb_shop.currency(pawn) == purse - 15,
                   "the 15-coin fee was paid")
    if spent_resource is not None:
        bd.assert_true(
            hero.resources[spent_resource] == hero.resource_max[spent_resource],
            "Wren's care restores the class-active charges")
        bd.assert_true("breathing" in str(result.get("message", "")),
                       "the breather message names the restore")
    pawn.take_inventory(currency, systems.dobb_shop.currency(pawn))
    if spent_resource is not None:
        hero.use_resource(spent_resource)
    result = systems.run_healer()
    bd.assert_true(not result.get("ok") and "afford" in result.get("message", ""),
                   "a broke patient is refused politely")
    if spent_resource is not None:
        bd.assert_true(hero.resources[spent_resource] == 0,
                       "a refused heal restores no charges")
    pawn.give_inventory(currency, 200)
    pawn.damage_factor = 0.0  # test driver is invulnerable again


def autotest_trainer():
    """Wren's TrainerService: scripted mastery advancement."""
    _teleport_to("wren")
    hero = systems.hero
    bd.assert_true(hero is not None, "trainer: hero available")
    if hero is None:
        return
    bd.assert_true(bd_dnd.mastery(hero, "medicine") == 0,
                   "no medicine mastery yet")
    pawn = systems.player_pawn()
    purse = systems.dobb_shop.currency(pawn)
    result = systems.run_service("wren", 1, {"rng": _Roller([95])})
    bd.assert_true(result.get("ok") and "improved" in result.get("message", ""),
                   "a 95 on the d100 check improves the skill")
    bd.assert_true(bd_dnd.mastery(hero, "medicine") == 1
                   and bd_dnd.skill_bonus(hero, "medicine") == 1,
                   "mastery tier 1 and its flat skill bonus")
    bd.assert_true(systems.dobb_shop.currency(pawn) == purse - 20,
                   "the 20-coin training fee was paid")
    result = systems.run_service("wren", 1, {"rng": _Roller([50])})
    bd.assert_true(result.get("ok") and bd_dnd.mastery(hero, "medicine") == 1,
                   "a failed advancement roll keeps the tier")


def autotest_quest_flow():
    """Kill the yard through the player pawn: quest, kill XP, level, disposition."""
    pawn = systems.player_pawn()
    bd.assert_true(pawn is not None, "quest flow: pawn available")
    if pawn is None:
        return
    pawn.set_position(*content.YARD_CENTER, check=False)
    for tid in content.YARD_TIDS:
        ref = bd.actor_ref(tid)
        if ref is not None and ref.valid and ref.alive:
            # source=pawn: the pawn is the Die() source, so exact kill
            # credit applies to the tracker's killer="player" policy AND
            # to track_xp_from_kills' player_index=0 policy.
            ref.damage(1000, source=pawn)
    clear = bd_quests.log.get(content.QUEST_CLEAR_YARD)
    bd.assert_true(clear is not None and clear.state == Quest.COMPLETED,
                   "five pawn-sourced kills completed clear_yard")
    obj = clear.objective("kill_zombies")
    bd.assert_true(obj is not None and obj.done and obj.progress == 5,
                   "the kill objective tracked 5/5")
    hero = systems.hero
    # Every yard kill paid its tabled 25 XP (5 x 25 = 125) through
    # track_xp_from_kills, and clear_yard's own 300 XP reward landed on the
    # fifth kill: 125 + 300 = 425, past the 300 threshold for level 2, so
    # the Press On feature already applied.
    bd.assert_true(hero.level == 2 and hero.xp == 425,
                   "kill XP plus the quest reward leveled the hero to 2 "
                   "(125 + 300 = 425)")
    bd.assert_true(hero.resource_max.get("second_wind") == 2,
                   "Press On raised Second Wind to two charges per rest")
    bd.assert_true(systems.manager.dispositions.get("sera") == 30,
                   "the quest reward shifted Sera by +20 (10 -> 30)")


def autotest_sera_watch_choice():
    """Post-yard, Sera's trust pointer starts watch_grows (the second of
    the quest's two starts)."""
    _teleport_to("sera")
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session with Sera open for the trust pointer")
    if session is None:
        return
    index = _choice_index(session, "Who else can I trust here?")
    bd.assert_true(index >= 0,
                   "the trust choice is visible once the yard is clean")
    if index < 0:
        session.end()
        return
    session.choose(index)
    watch = bd_quests.log.get(content.QUEST_WATCH_GROWS)
    bd.assert_true(watch is not None and watch.state == Quest.ACTIVE,
                   "the trust choice started watch_grows")
    bd.assert_true(session.active_node is not None
                   and session.active_node.id == "trust_korr",
                   "the trust choice routes to the Korr pointer node")
    session.choose(0)  # "I will find him." ends the conversation
    bd.assert_true(not session.active, "the trust pointer exit ends the session")


def autotest_markers_after_watch():
    """watch_grows open again: Korr carries the "!", the yard beacon is down."""
    bd.assert_true(systems.marker_state.get(content.MARKER_KORR_ID) == "korr",
                   "the Korr marker shows again for watch_grows")
    bd.assert_true(content.MARKER_YARD_BEACON_ID not in systems.marker_state,
                   "the yard beacon cleared once clear_yard completed")


def autotest_fetch_cache_asserts():
    """Walking the cache line completed the fetch quest (and paid shells)."""
    fetch = bd_quests.log.get(content.QUEST_FETCH_CACHE)
    bd.assert_true(fetch is not None and fetch.state == Quest.COMPLETED,
                   "the cache pickup completed fetch_cache")
    obj = fetch.objective("recover_cache")
    bd.assert_true(obj is not None and obj.done,
                   "the pickup objective credited the crate")
    hero = systems.hero
    bd.assert_true(hero is not None and hero.xp == 575,
                   "the fetch XP landed (425 -> 575)")
    bd.assert_true(content.MARKER_CACHE_BEACON_ID not in systems.marker_state,
                   "the cache beacon cleared once fetch_cache completed")


def _walk_onto_cache():
    pawn = systems.player_pawn()
    if pawn is None:
        return
    # set_position is a raw SetOrigin (no touch check): nudge into real
    # movement so the next tic's P_TryMove overlaps the cache and fires the
    # native pickup path.
    pawn.set_position(*content.CACHE_POS, check=False)
    pawn.set_velocity(2.0, 0.0, 0.0)


def autotest_recruit():
    """Recruit Korr through the gated dialogue effect: the hub NPC retires
    and the companion spawns at his slot."""
    korr_slot = None
    ref_before = systems.manager.actor_for("korr")
    if ref_before is not None and ref_before.valid:
        korr_slot = (ref_before.x, ref_before.y, ref_before.z)
    bd.assert_true(korr_slot is not None, "the hub Korr is live pre-recruit")
    _teleport_to("korr")
    session = systems.start_talk()
    bd.assert_true(session is not None and session.active,
                   "session with Korr open")
    if session is None:
        return
    entries = session.choices()
    join = next((i for i, (c, en, _a) in enumerate(entries)
                 if c.text == "Fight beside me."), -1)
    bd.assert_true(join >= 0 and entries[join][1],
                   "the recruitment choice is visible and enabled "
                   "(yard complete gate)")
    session.choose(join)
    bd.assert_true(systems.korr_recruited, "the effect recruited Korr")
    bd.assert_true(session.active_node.id == "joined",
                   "recruitment routes to the joined node")
    bd.assert_true(systems.manager.is_retired("korr"),
                   "the hub Korr is retired")
    bd.assert_true(systems.manager.actor_for("korr") is None,
                   "the manager no longer tracks a hub Korr")
    party = systems.party
    bd.assert_true(party is not None and len(party) == 2,
                   "the party holds the hero and Korr")
    korr = party.get(content.KORR_NAME) if party is not None else None
    companion = systems.korr_companion
    ref = companion.actor() if companion is not None else None
    bd.assert_true(korr is not None and ref is not None and ref.valid
                   and ref.alive, "Korr's companion actor is in the world")
    if ref is not None and korr_slot is not None:
        dx, dy = ref.x - korr_slot[0], ref.y - korr_slot[1]
        bd.assert_true((dx * dx + dy * dy) ** 0.5 <= 80.0,
                       "the companion spawned at the retired NPC slot")
    demons = [r for r in bd.actor_refs("Demon") if r.valid and r.alive]
    bd.assert_true(len(demons) == 1,
                   "exactly one Demon follower exists (no hub duplicate)")
    watch = bd_quests.log.get(content.QUEST_WATCH_GROWS)
    bd.assert_true(watch is not None and watch.state == Quest.COMPLETED,
                   "recruitment completed watch_grows")
    obj = watch.objective("recruit_korr") if watch is not None else None
    bd.assert_true(obj is not None and obj.done,
                   "the recruit objective credited Korr signing on")
    hero = systems.hero
    bd.assert_true(hero is not None and hero.xp == 725,
                   "the watch_grows XP landed (575 -> 725)")
    if ref is not None and korr is not None:
        bd.assert_true(ref.health == korr.hp,
                       "companion health initialized from Korr's hp")
        bd.assert_true(ref.get_flag("FRIENDLY")
                       and not ref.get_flag("COUNTKILL"),
                       "the companion keeps friendly semantics")
        ref.damage_factor = 0.0  # fixture stays alive for the round-trip
    try:
        from bd_horror import toasts
        bd.assert_true(any(t.get("text") == content.TOAST_RECRUIT
                           for t in toasts.history),
                       "the recruitment toast was raised")
    except Exception:
        bd.assert_true(False, "bd_horror toasts available for the toast check")
    session.choose(0)  # the retired NPC handle is stale: ends the conversation


#: TID for the hostile test zombie of the companion-combat stage.
ZOMBIE_TEST_TID = 9210


def autotest_companion_combat_mark():
    """Spawn a hostile, let the player hurt it, and drive the damage filter.

    The companion's mark list updates off the actor_damaged event; the hero
    damage filter is driven directly with synthetic event dicts (the engine
    never sees them, so the run stays isolation-safe).
    """
    pawn = systems.player_pawn()
    companion = systems.korr_companion
    bd.assert_true(pawn is not None and companion is not None,
                   "companion combat: pawn and companion available")
    bd.assert_true(content.MARKER_KORR_ID not in systems.marker_state,
                   "the Korr marker cleared once he signed on")
    if pawn is None or companion is None:
        return
    zombie = None
    try:
        zombie = bd.spawn(content.GAME_CONTENT["yard_monster"],
                          pawn.x + 256.0, pawn.y, pawn.z, angle=270.0,
                          tid=ZOMBIE_TEST_TID, force=True)
        zombie.set_flag("STANDSTILL", True)
        zombie.speed = 0.0  # planted: the companion cannot reach it mid-test
    except Exception as exc:
        bd.assert_true(False, f"test zombie spawned: {exc!r}")
        return
    # The player hurts it: the companion marks it (asserted next stage).
    zombie.damage(3, source=pawn)
    # The hero damage filter, driven directly with a synthetic event shaped
    # like actor_before_damage: a player hit on a live monster gains the
    # level bonus (level 2: min(8, 1) = 1; no Skirmisher note on a
    # Mercenary).
    event = {"attacker_player_index": 0, "actor_ref": zombie, "damage": 10,
             "damage_type": "None"}
    systems.hero_damage_filter(event)
    expected = 10 + min(8, max(0, systems.hero.level - 1))
    bd.assert_true(event["damage"] == expected,
                   "the hero damage filter added the level bonus")
    neutral = {"attacker_player_index": None, "actor_ref": zombie,
               "damage": 10}
    systems.hero_damage_filter(neutral)
    bd.assert_true(neutral["damage"] == 10,
                   "the filter ignores hits not landed by the player")
    bystander = {"attacker_player_index": 0, "actor_ref": pawn, "damage": 10}
    systems.hero_damage_filter(bystander)
    bd.assert_true(bystander["damage"] == 10,
                   "the filter ignores non-monster victims")


def autotest_companion_combat_assert():
    """~15 tics later: the companion fights the mark; the kill pays XP."""
    companion = systems.korr_companion
    zombie = bd.actor_ref(ZOMBIE_TEST_TID)
    if companion is None or zombie is None:
        bd.assert_true(False, "the test zombie survived to the assert")
        return
    cref = companion.actor()
    targeted = False
    try:
        targeted = (cref is not None and cref.target is not None
                    and cref.target == zombie)
    except Exception:
        targeted = False
    marked = False
    try:
        marked = any(h is not None and h.valid and h == zombie
                     for h, _expiry in companion._attackers)
    except Exception:
        marked = False
    bd.assert_true(targeted or marked,
                   "the companion fights the monster the player hurt")
    pawn = systems.player_pawn()
    hero = systems.hero
    if pawn is None or hero is None:
        return
    xp_before = hero.xp
    zombie.damage(999, source=pawn)
    bd.assert_true(hero.xp == xp_before + 25,
                   "a pawn-sourced ZombieMan kill paid its tabled XP (25)")
    try:
        zombie.destroy()
    except Exception:
        pass


#: Scripted roll for the class-active heal: d10 face 5, plus the level.
_ACTIVE_ROLL = 5


def autotest_class_active_no_charge():
    """Custom Action 3 with an empty pool: the no-charge path only talks."""
    pawn = systems.player_pawn()
    hero = systems.hero
    bd.assert_true(pawn is not None and hero is not None,
                   "class active: pawn and hero available")
    if pawn is None or hero is None:
        return
    # A real wound on the pawn (the established damage_factor toggle) and
    # a wounded sheet, so the heal that eventually lands is measurable.
    pawn.damage_factor = 1.0
    pawn.damage(30)
    pawn.damage_factor = 0.0
    _pre_save["pawn_wounded_hp"] = pawn.health
    hero.set_hp(5)
    hero.resources["second_wind"] = 0  # spend down the pool by hand
    bd.set_custom_action(3, True)


def autotest_class_active_no_charge_asserts():
    """The empty pool named the resource and the rest, and nothing healed."""
    bd.assert_true((3, True) in systems.action_log,
                   "the class-active press fired {'action': 3, pressed}")
    hero = systems.hero
    pawn = systems.player_pawn()
    bd.assert_true(hero is not None and hero.hp == 5,
                   "no charge, no sheet heal")
    bd.assert_true(pawn is not None
                   and pawn.health == _pre_save.get("pawn_wounded_hp"),
                   "no charge, no pawn heal")
    message = systems.last_active_message
    bd.assert_true("second_wind" in message and "rest" in message,
                   "the no-charge path names the resource and the rest")
    bd.set_custom_action(3, False)
    # Refill for the real heal; script the die so the amount is exact.
    if hero is not None:
        hero.resources["second_wind"] = \
            hero.resource_max.get("second_wind", 2)
    systems.class_active_rng = _Roller([_ACTIVE_ROLL])


def autotest_class_active_press():
    """Custom Action 3 with a charge: Second Wind fires on the next scan."""
    bd.set_custom_action(3, True)


def autotest_class_active_asserts():
    """The Mercenary heal landed on sheet and pawn; the charge is spent."""
    hero = systems.hero
    pawn = systems.player_pawn()
    amount = _ACTIVE_ROLL + (hero.level if hero is not None else 0)
    if hero is not None:
        bd.assert_true(hero.hp == 5 + amount,
                       "Second Wind healed the sheet by die + level")
        bd.assert_true(hero.resources.get("second_wind") == 1,
                       "the heal spent one of the two charges")
    if pawn is not None:
        bd.assert_true(pawn.health
                       == _pre_save.get("pawn_wounded_hp", -99) + amount,
                       "Second Wind healed the pawn by the same amount")
    bd.assert_true("Second Wind" in systems.last_active_message,
                   "the use feedback named the active")
    systems.class_active_rng = None
    bd.set_custom_action(3, False)


def autotest_pre_save():
    """Snapshot everything, spend a resource, and write the checkpoint."""
    active = bd_dialogue.active_session()
    bd.assert_true(active is None, "no conversation open at save time")
    hero = systems.hero
    bd.assert_true(hero is not None and systems.party is not None,
                   "hero and party exist before the save")
    bd.assert_true(hero.use_resource("second_wind"),
                   "the second wind charge is spent before the save")
    _pre_save["hero"] = hero.serialize()
    _pre_save["sera"] = systems.manager.dispositions.get("sera")
    _pre_save["stock"] = systems.dobb_shop.stock_snapshot()
    # The retired Korr carries no TID anymore; only the three hub NPCs do.
    _pre_save["tids"] = {
        npc_id: systems.manager.actor_for(npc_id).tid
        for npc_id in ("sera", "dobb", "wren")}
    _pre_save["companion_tid"] = systems.korr_companion.tid
    _pre_save["korr_hp"] = systems.party.get(content.KORR_NAME).hp
    bd.save_checkpoint(content.CHECKPOINT_NAME,
                       description="Ashvale Crossing autotest")


def autotest_load():
    """Draw post-save stream positions, disturb state, and reload."""
    _pre_save["rng_pair"] = (bd.randint(1, 1000000), bd.randint(1, 1000000))
    systems.manager.dispositions.shift("sera", -5)  # undone by the restore
    bd.load_checkpoint(content.CHECKPOINT_NAME)


def autotest_post_load():
    """The checkpoint round-trip: stream, hero, standings, stock, party."""
    pair = _pre_save.get("rng_pair")
    redrawn = (bd.randint(1, 1000000), bd.randint(1, 1000000))
    bd.assert_true(pair is not None and redrawn == pair,
                   "script RNG stream resumes exactly after checkpoint load")
    hero = systems.hero
    snap = _pre_save.get("hero") or {}
    bd.assert_true(hero is not None, "the hero exists after the load")
    if hero is None:
        return
    bd.assert_true(hero.level == snap.get("level") == 2
                   and hero.xp == snap.get("xp") == 750,
                   "level and XP round-tripped")
    bd.assert_true(hero.max_hp == snap.get("max_hp") == 20,
                   "max hp round-tripped (12 + 8 on the level-up)")
    bd.assert_true(hero.abilities.score("str") == 15
                   and hero.class_id == "Mercenary",
                   "ability scores and the class binding round-tripped")
    bd.assert_true(hero.proficient_skills
                   == set(snap.get("proficient_skills", ())),
                   "proficiencies round-tripped")
    bd.assert_true(hero.resources.get("second_wind") == 0
                   and hero.resource_max.get("second_wind") == 2,
                   "the spent charge stays spent; Press On's second max "
                   "charge round-tripped")
    bd.assert_true(getattr(hero, "cls", None) is content.MERCENARY,
                   "the live class object is still attached")
    bd.assert_true(systems.manager.dispositions.get("sera")
                   == _pre_save.get("sera") == 30,
                   "Sera's disposition round-tripped (the -5 was undone)")
    bd.assert_true(systems.dobb_shop.stock_snapshot()
                   == _pre_save.get("stock"),
                   "the shop stock counts round-tripped")
    party = systems.party
    bd.assert_true(party is not None and len(party) == 2,
                   "the party survived the checkpoint")
    if party is not None:
        korr = party.get(content.KORR_NAME)
        bd.assert_true(korr is not None and korr.hp == _pre_save.get("korr_hp"),
                       "Korr's hp round-tripped")
    companion = systems.korr_companion
    bd.assert_true(companion is not None and companion.bound
                   and not companion.dead,
                   "the companion survived the checkpoint")
    if companion is not None and party is not None:
        ref = companion.actor()
        korr = party.get(content.KORR_NAME)
        bd.assert_true(ref is not None and ref.valid and ref.alive,
                       "the companion re-bound to its actor after the load")
        if ref is not None and korr is not None:
            bd.assert_true(ref.health == korr.hp,
                           "companion health still mirrors Korr's hp")
        bd.assert_true(companion.tid == _pre_save.get("companion_tid"),
                       "the companion kept its saved TID (adopted, respawned)")
    for npc_id, tid in (_pre_save.get("tids") or {}).items():
        ref = systems.manager.actor_for(npc_id)
        bd.assert_true(ref is not None and ref.valid and ref.alive
                       and ref.tid == tid,
                       f"{npc_id} re-bound to its saved TID")
    # The retired set round-tripped: spawn_all must not respawn a hub Korr,
    # and the companion remains the one Demon in the world.
    bd.assert_true(systems.manager.is_retired("korr"),
                   "Korr's retirement round-tripped")
    bd.assert_true(systems.manager.actor_for("korr") is None,
                   "spawn_all did not respawn a hub Korr after the load")
    demons = [r for r in bd.actor_refs("Demon") if r.valid and r.alive]
    bd.assert_true(len(demons) == 1,
                   "still exactly one Demon after the load (the companion)")
    for quest_id, state in ((content.QUEST_CLEAR_YARD, Quest.COMPLETED),
                            (content.QUEST_FETCH_CACHE, Quest.COMPLETED),
                            (content.QUEST_WATCH_GROWS, Quest.COMPLETED)):
        quest = bd_quests.log.get(quest_id)
        bd.assert_true(quest is not None and quest.state == state,
                       f"{quest_id} round-tripped as {state}")
    bd.assert_true(bd_quests.log.get("prove_worth") is None,
                   "the deleted shadow quest never comes back")
    bd.assert_true(systems._hud_task is not None,
                   "the HUD strip task is alive after the load")
    bd.assert_true(not systems.marker_state,
                   "every marker cleared (all quests complete post-load)")
    for key in (bd_dnd.STATE_KEY, bd_dnd.PARTY_STATE_KEY, "bd_npcs",
                bd_quests.STATE_KEY, content.SHOP_STATE_KEY):
        bd.assert_true(isinstance(bd.state.get(key), dict),
                       f"bd.state carries '{key}' after the load")


def autotest_finish():
    bd.log("ASHVALE CROSSING AUTOTEST assertions complete")


# --- custom actions (synthetic presses) ---------------------------------------------------


def autotest_custom_action_talk_press():
    """Synthetic Custom Action 1 press: the talk path without the console.

    The world is not creation-paused here (the autotest disabled the
    pause), so the press surfaces on the next gametic's scan."""
    _teleport_to("sera")
    bd.assert_true(bd_dialogue.active_session() is None,
                   "no session open before the custom action press")
    bd.set_custom_action(1, True)


def autotest_custom_action_talk_asserts():
    """The press fired the documented payload and opened a session."""
    active = bd_dialogue.active_session()
    bd.assert_true((1, True) in systems.action_log,
                   "the custom_action event carried "
                   "{'action': 1, 'pressed': True}")
    bd.assert_true(active is not None and active.active,
                   "custom action 1 press started a session")
    if active is not None:
        bd.assert_true(active.active_node is not None
                       and active.active_node.id == "start",
                       "the custom-action session opens at the start node")
        active.end()
    bd.set_custom_action(1, False)


def autotest_custom_action_talk_release():
    """The release edge fired too, without starting another session."""
    bd.assert_true((1, False) in systems.action_log,
                   "the release edge fired with "
                   "{'action': 1, 'pressed': False}")
    bd.assert_true(bd_dialogue.active_session() is None,
                   "the release edge did not start a session")


def autotest_custom_action_sheet_press():
    """Synthetic Custom Action 2 press flips the hero sheet."""
    bd.assert_true(systems.hero_sheet is not None
                   and systems.hero_sheet.visible,
                   "the hero sheet is visible before the toggle press")
    bd.set_custom_action(2, True)


def autotest_custom_action_sheet_asserts():
    bd.assert_true((2, True) in systems.action_log,
                   "the sheet press fired {'action': 2, 'pressed': True}")
    bd.assert_true(systems.hero_sheet is not None
                   and not systems.hero_sheet.visible,
                   "custom action 2 hid the sheet")
    bd.set_custom_action(2, False)


def autotest_custom_action_sheet_restore():
    bd.assert_true((2, False) in systems.action_log,
                   "the sheet release fired {'action': 2, 'pressed': False}")
    bd.assert_true(systems.hero_sheet is not None
                   and not systems.hero_sheet.visible,
                   "the release edge left the sheet hidden")
    bd.set_custom_action(2, True)


def autotest_custom_action_sheet_back():
    bd.assert_true(systems.hero_sheet is not None
                   and systems.hero_sheet.visible,
                   "the second press showed the sheet again")
    bd.set_custom_action(2, False)


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


_sys.modules.setdefault("ashvale_main", _LiveAlias("ashvale_main", globals()))
del _sys, _types
