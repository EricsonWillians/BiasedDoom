#!/usr/bin/env python3
"""Offline contracts for bounded, leader-directed companion descents.

Stock Doom II MAP01 begins with a safe 48-unit ledge down to the exterior.
That is larger than a monster-style MaxDropOffHeight, but it must not turn
combat strafing, pickups, portals, stale route data, or arbitrary players into
permission to walk off a cliff. The runtime fixtures in test-bot-map01-drop.sh
and test-bot-map01-hostiles.sh cover the isolated and stock-combat paths;
these small source and math contracts protect the specific safety policy
without needing an IWAD on every edit.
"""

from __future__ import annotations

import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAX_PLANNED_DESCENT = 64.0


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    """Extract a C++ function body without depending on surrounding comments."""

    signature_start = text.index(signature)
    opening = text.index("{", signature_start)
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


def can_traverse_drop(upper_floor: float, lower_floor: float,
                      normal_limit: float, planned: bool) -> bool:
    """Model BotCanTraverseDrop's intentionally narrow numerical rule."""

    drop = upper_floor - lower_floor
    return drop <= normal_limit or (planned and drop <= MAX_PLANNED_DESCENT)


def planned_descent(*, position: tuple[float, float], probe: tuple[float, float],
                    goal: tuple[float, float] | None, upper_floor: float,
                    lower_floor: float, planned_floor: float, normal_limit: float,
                    step_height: float, radius: float, portal_drop: bool) -> bool:
    """Model IsPlannedDescent's floor/portal/alignment gates."""

    if goal is None or portal_drop:
        return False
    if not can_traverse_drop(upper_floor, lower_floor, normal_limit, True):
        return False
    if abs(planned_floor - lower_floor) > step_height:
        return False

    approach = (probe[0] - position[0], probe[1] - position[1])
    toward_goal = (goal[0] - position[0], goal[1] - position[1])
    approach_length = math.hypot(*approach)
    goal_length = math.hypot(*toward_goal)
    if approach_length <= 1e-6 or goal_length <= max(radius, approach_length):
        return False
    alignment = ((approach[0] * toward_goal[0] + approach[1] * toward_goal[1]) /
                 (approach_length * goal_length))
    return alignment >= 0.5


def test_bounded_drop_math() -> None:
    # MAP01's floor 56 -> floor 8 start descent is deliberately larger than a
    # normal 24-unit monster drop but inside the one controlled 64-unit band.
    assert not can_traverse_drop(56.0, 8.0, 24.0, False)
    assert can_traverse_drop(56.0, 8.0, 24.0, True)

    # A planned route cannot widen this into a generic cliff policy.
    assert not can_traverse_drop(56.0, -9.0, 24.0, True)  # 65 units
    assert not can_traverse_drop(320.0, 8.0, 24.0, True)


def test_descent_proof_requires_real_safe_goal_geometry() -> None:
    # A representative MAP01 start/leader geometry: move north-east-ish from
    # the co-op platform toward a human who is already on the floor-8 exterior.
    safe_map01_descent = dict(
        position=(-32.0, 784.0),
        probe=(-40.0, 800.0),
        goal=(-96.0, 920.0),
        upper_floor=56.0,
        lower_floor=8.0,
        planned_floor=8.0,
        normal_limit=24.0,
        step_height=24.0,
        radius=16.0,
        portal_drop=False,
    )
    assert planned_descent(**safe_map01_descent)

    # There must be no incidental acceptance for a raw combat direction,
    # portal support, stale elevation proof, or a lateral/backward sidestep.
    assert not planned_descent(**{**safe_map01_descent, "goal": None})
    assert not planned_descent(**{**safe_map01_descent, "portal_drop": True})
    assert not planned_descent(**{**safe_map01_descent, "planned_floor": 56.0})
    assert not planned_descent(**{**safe_map01_descent, "probe": (-32.0, 768.0)})
    assert not planned_descent(**{**safe_map01_descent, "probe": (-16.0, 784.0)})


def test_source_keeps_the_numeric_bound_and_full_proof() -> None:
    header = source("src/playsim/bots/b_bot.h")
    move = source("src/playsim/bots/b_move.cpp")
    func = source("src/playsim/bots/b_func.cpp")

    assert "constexpr double BOT_MAX_PLANNED_DESCENT = 64.0;" in header
    drop_helper = function_body(header, "inline bool BotCanTraverseDrop")
    assert "drop <= normalDropLimit" in drop_helper
    assert "allowPlannedDescent && drop <= BOT_MAX_PLANNED_DESCENT" in drop_helper

    proof = function_body(move, "bool IsPlannedDescent")
    for fragment in (
        "goal == nullptr",
        "probe.dropoffisportal",
        "BotCanTraverseDrop(probe.floorz, probe.dropoffz",
        "fabs(plannedFloor - probe.dropoffz) > actor->MaxStepHeight",
        "goalLength <= max(actor->radius, approachLength)",
        "return alignment >= 0.5;",
    ):
        assert fragment in proof, fragment

    clean_ahead = function_body(move, "bool FCajunMaster::CleanAhead")
    assert "tm.floorz - tm.dropoffz > thing->MaxDropOffHeight" in clean_ahead
    assert "!IsPlannedDescent(thing, x, y, tm, plannedDescentGoal, plannedDescentFloor)" in clean_ahead

    # Direct visibility must return the actual first relaxed support transition,
    # not blindly reuse the mate's base-sector floor or a cosmetic Z value.
    reachable = function_body(func, "bool DBot::Reachable")
    assert "double *plannedLandingFloor, bool *usesPlannedDescent" in func
    for fragment in (
        "double firstPlannedLandingFloor = 0.0;",
        "bool usedPlannedDescent = false;",
        "if ((!openingDropIsNormal || !supportDropIsNormal) && !usedPlannedDescent)",
        "firstPlannedLandingFloor = floorheight;",
        "*plannedLandingFloor = usedPlannedDescent ? firstPlannedLandingFloor : targetFloor;",
        "*usesPlannedDescent = usedPlannedDescent;",
    ):
        assert fragment in reachable, fragment


def test_leader_only_permission_and_route_cache_discriminator() -> None:
    move = source("src/playsim/bots/b_move.cpp")
    roam = function_body(move, "void DBot::Roam")

    # A destination which happens to be a player can be a hostile opponent or
    # a non-follow target. Only the selected cooperative mate may enable the
    # controlled descent policy. A visible hostile must not strand a companion
    # above a verified leader route, but deathmatch still never receives this
    # traversal exception.
    assert ("const bool allowPlannedDescent = dest == mate && mate != nullptr "
            "&& !deathmatch;" in roam)
    assert "dest->player" not in roam

    route = function_body(move, "bool DBot::FindRouteWaypoint")
    # A cache result built while leader descent was permitted must never be
    # reused by a later ordinary item/combat request for the same target sector.
    assert route.count("route_waypoint_allows_planned_descent == allowPlannedDescent") == 2
    assert "route_waypoint_allows_planned_descent = allowPlannedDescent;" in route
    assert "bool &usesPlannedDescent" in move
    assert "usesPlannedDescent = false;" in route
    assert "usesPlannedDescent = route_waypoint_uses_planned_descent;" in route
    assert "route_waypoint_uses_planned_descent = !firstOpeningDropIsNormal ||" in route

    # A manual door/lift may postpone clearance, but never waive portal width
    # or the safe bounded landing predicate.
    assert "const bool landingSafe =" in route
    assert "if (!portalWideEnough || !landingSafe || (!clearNow && !canOperate)" in route


def test_only_a_verified_leader_route_reaches_the_final_guard() -> None:
    move = source("src/playsim/bots/b_move.cpp")
    think = source("src/playsim/bots/b_think.cpp")
    func = source("src/playsim/bots/b_func.cpp")

    validate = function_body(move, "void DBot::ValidateMovementCommand")
    assert "planned_descent_active ? &planned_descent_goal : nullptr" in validate
    assert "planned_descent_floor" in validate

    think_body = function_body(think, "void DBot::Think ()")
    # The permission is transient; a new think pass starts with no descent
    # grant and must reconstruct it from a fresh direct reachability proof.
    assert "planned_descent_active = false;" in think_body

    follow = function_body(think, "void DBot::ThinkForMove")
    for fragment in (
        "Reachable(mate, true, &mateFloor, &mateUsesPlannedDescent)",
        "if (mateUsesPlannedDescent)",
        "planned_descent_goal = mate->Pos().XY();",
        "planned_descent_floor = mateFloor;",
        "planned_descent_active = true;",
    ):
        assert fragment in follow, fragment

    # Projectile safety traces leave the optional CleanAhead proof arguments
    # unset, so an aim trace cannot accidentally inherit a movement exception.
    fake_fire = function_body(func, "double FCajunMaster::FakeFire")
    assert "CleanAhead (th, th->X(), th->Y(), cmd)" in fake_fire
    assert "plannedDescent" not in fake_fire


def test_planned_descent_keeps_movement_priority_and_bearing() -> None:
    """A visible hostile must not recreate MAP01's upper-ledge stall.

    Missile avoidance still wins, but ordinary combat yields after a fresh,
    bounded leader-descent proof. Dofire may change Angle while it evaluates a
    shot, so the follower bearing has to be restored before the final movement
    validator projects the forward command.
    """

    think = source("src/playsim/bots/b_think.cpp")
    follow = function_body(think, "void DBot::ThinkForMove")

    combat_guard = "else if (!mustFollowPlannedDescent && enemy &&"
    follow_guard = "else if (mate && (mustFollowPlannedDescent ||"
    assert combat_guard in follow
    assert follow_guard in follow
    combat_guard_at = follow.index(combat_guard)
    follow_guard_at = follow.index(follow_guard)
    assert "P_CheckSight(player->mo, enemy, 0)" in follow[combat_guard_at:follow_guard_at]
    assert combat_guard_at < follow_guard_at

    descent_fire = "if (mustFollowPlannedDescent && enemy && Check_LOS"
    descent_fire_at = follow.index(descent_fire, follow_guard_at)
    dofire_at = follow.index("Dofire(cmd);", descent_fire_at)
    restore_pitch_at = follow.index("Pitch(mate);", dofire_at)
    restore_bearing_at = follow.index("Angle = player->mo->AngleTo(mate);", restore_pitch_at)
    assert descent_fire_at < dofire_at < restore_pitch_at < restore_bearing_at


def test_map01_fixture_requires_a_native_elevated_platform_descent() -> None:
    """Keep the live MAP01 check from passing via a lower spawn or recovery."""

    fixture = source("tools/bot-map01-drop/pyscripts/main.py")
    runner = source("tools/test-bot-map01-drop.sh")

    assert "EXPECTED_START_FLOOR = 56.0" in fixture
    assert "NATIVE_DESCENT_DEADLINE = 2 * bd.TICRATE" in fixture
    assert "starts_on_platform = (pawn.alive" in fixture
    assert "abs(pawn.floor_z - EXPECTED_START_FLOOR) <= 0.01" in fixture
    assert "BOT_MAP01_DROP companion_start" in fixture
    assert "start_platform_verified and companion is not None" in fixture
    assert "initial_companion_actor = pawn" in fixture
    assert "pawn == initial_companion_actor" in fixture
    assert "native_before_catchup = descended_at_tic < NATIVE_DESCENT_DEADLINE" in fixture
    assert "native_before_catchup={native_before_catchup}" in fixture

    # The only setup mutations must precede the public co-op join command.
    # After this point the fixture only observes native companion behavior.
    join_at = fixture.index('bd.execute("addcompanion")')
    after_join = fixture[join_at:]
    assert ".set_position(" not in after_join
    assert ".destroy()" not in after_join

    # Recovery is deliberately delayed by the same interval the runtime
    # fixture enforces, so a recorded early floor-8 crossing cannot be the
    # catch-up teleport fallback.
    think = source("src/playsim/bots/b_think.cpp")
    assert "static constexpr int CompanionCatchupTics = 2 * TICRATE;" in think
    follow = function_body(think, "void DBot::ThinkForMove")
    assert "if (t_follow < CompanionCatchupTics)" in follow
    assert "if (t_follow >= CompanionCatchupTics)" in follow
    assert "if (TryCatchUpToMate())" in follow
    assert follow.index("if (t_follow >= CompanionCatchupTics)") < follow.index(
        "if (TryCatchUpToMate())")

    # Fresh companions start the counter at zero, so the first crossing cannot
    # be attributed to recovery before the fixture's deadline.
    bot = source("src/playsim/bots/b_bot.cpp")
    clear = function_body(bot, "void DBot::Clear")
    assert "t_follow = 0;" in clear

    for marker in (
        "BOT_MAP01_DROP companion_start",
        "elevated_platform=True",
        "original_pawn=True",
        "native_before_catchup=True",
    ):
        assert marker in runner


def test_hostile_map01_fixture_preserves_stock_combat_context() -> None:
    """Keep the populated MAP01 test on native console input and live actors."""

    fixture = source("tools/bot-map01-hostiles/pyscripts/main.py")
    runner = source("tools/test-bot-map01-hostiles.sh")

    assert "MIN_NEARBY_HOSTILES = 2" in fixture
    assert "NATIVE_DESCENT_DEADLINE = 2 * bd.TICRATE" in fixture
    assert 'bd.execute("addcompanion; wait 30; +forward; wait 20; -forward")' in fixture
    for forbidden in (".set_position(", ".set_velocity(", ".destroy(", ".set_input("):
        assert forbidden not in fixture
        assert forbidden in runner
    for marker in (
        "BOT_MAP01_HOSTILES hostiles_retained",
        "BOT_MAP01_HOSTILES leader_descended",
        "BOT_MAP01_HOSTILES companion_start",
        "BOT_MAP01_HOSTILES companion_descended",
        "native_before_catchup={native_before_catchup}",
    ):
        assert marker in fixture, marker
    assert "allowed_native_input" in runner


def main() -> None:
    test_bounded_drop_math()
    test_descent_proof_requires_real_safe_goal_geometry()
    test_source_keeps_the_numeric_bound_and_full_proof()
    test_leader_only_permission_and_route_cache_discriminator()
    test_only_a_verified_leader_route_reaches_the_final_guard()
    test_planned_descent_keeps_movement_priority_and_bearing()
    test_map01_fixture_requires_a_native_elevated_platform_descent()
    test_hostile_map01_fixture_preserves_stock_combat_context()
    print("PASS: planned companion descent policy contracts")


if __name__ == "__main__":
    main()
