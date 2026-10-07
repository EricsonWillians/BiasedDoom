#!/usr/bin/env python3
"""Offline contracts for the map-agnostic Cajun bot navigation pass.

The game integration suite exercises engine startup, but it cannot cheaply
cover every stock, custom, and generated map shape. These focused contracts
guard the engine-side rules that must remain true on all of them: routing is
based on loaded level geometry rather than a generator format, only known
manual movement links are used, movement is clearance/hazard checked after
all behavior modes have contributed input, and combat has a real firing-lane
and blast-radius safety gate.
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    """Return one C++ function body without relying on nearby comments.

    Keeping assertions scoped to the function which implements a rule makes
    this regression test resilient to comments elsewhere in the source while
    still detecting a behavior-changing edit.
    """

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


def test_reachability_uses_real_portal_geometry() -> None:
    text = source("src/playsim/bots/b_func.cpp")
    body = function_body(text, "bool DBot::Reachable")
    assert "PT_ADDLINES)" in body
    assert "PT_ADDTHINGS" not in body
    assert "const DVector2 hit = it.InterceptPoint(in);" in body
    assert "player->mo->MaxStepHeight" in body
    assert "IsDangerous(player->mo, s, hit, floorheight)" in body
    assert "targetFloor <= last_z + player->mo->MaxStepHeight" in body
    assert "P_LineOpening(opening, player->mo, line, hit);" in body
    assert "opening.range >= player->mo->Height" in body
    # Direct ray reachability must reject the same too-narrow boundary that
    # sector-route planning rejects. Otherwise a visible target can suppress
    # route search while the pawn repeatedly collides with the opening.
    assert "const double portalWidth = (line->v2->fPos() - line->v1->fPos()).Length();" in body
    assert "if (portalWidth < player->mo->radius * 2.0 + 8.0)" in body
    assert "BotCanTraverseDrop(last_z, opening.lowfloor" in body
    assert "BotCanTraverseDrop(last_z, floorheight" in body
    assert "BotCanTraverseDrop(last_z, targetFloor" in body
    # The endpoint needs to resolve from the target's actual floor support,
    # not the last support crossed by the bot. That keeps a leader on a 3D
    # bridge/platform from being mistaken for one standing on the base floor.
    assert "rtarget->floorz, targetSupport" in body


def test_route_probes_actual_3d_support_and_same_sector_hazards() -> None:
    """Keep 3D-floor probes from regressing to base-plane heuristics."""

    func = source("src/playsim/bots/b_func.cpp")
    move = source("src/playsim/bots/b_move.cpp")
    p_map = source("src/playsim/p_map.cpp")

    # Both direct reachability and graph edges query the floor/ceiling around
    # the bot's virtual support height, then evaluate the line opening at that
    # same height. This is needed for stacked 3D platforms and overhangs.
    for text in (func, move):
        for fragment in (
            "bool ProbeBotSupport(AActor *actor, sector_t *sector",
            "NextLowestFloorAt(sector, position.X, position.Y",
            "NextHighestCeilingAt(sector, position.X, position.Y",
            "bool IsDangerousBotSupport(AActor *actor, const FCheckPosition &probe)",
            "void GetBotLineOpeningAtSupport(FLineOpening &opening, AActor *actor",
            "bool ProbePointPastLine(AActor *actor, const line_t *line",
        ):
            assert fragment in text, fragment

    reachable = function_body(func, "bool DBot::Reachable")
    assert "FCheckPosition startSupport;" in reachable
    assert "double last_z = startSupport.floorz;" in reachable
    assert "FCheckPosition nextSupport;" in reachable
    assert "GetBotLineOpeningAtSupport(opening, player->mo, line, hit, last_z);" in reachable
    assert "!IsDangerousBotSupport(player->mo, nextSupport)" in reachable

    route = function_body(move, "bool DBot::FindRouteWaypoint")
    assert "TArray<double> supportFloor;" in route
    assert "const double currentFloor = supportFloor[currentIndex];" in route
    assert "supportFloor[next->sectornum] = nextFloor;" in route
    assert "GetBotLineOpeningAtSupport(opening, player->mo, line, portal, currentFloor);" in route
    assert "IsDangerousBotSupport(player->mo, nextSupport)" in route

    # The final movement guard must reject a dangerous 3D support transition
    # even when tm.sector is the exact same base sector as the source.
    try_move = function_body(p_map, "bool P_TryMove(AActor *thing, const DVector2 &pos,")
    for fragment in (
        "const auto supportIsDangerous",
        "sector_t *oldBaseSector",
        "sector_t *newSupportSector",
        "const bool wasDangerous = supportIsDangerous",
        "const bool entersDanger = supportIsDangerous",
        "if (entersDanger && !wasDangerous)",
    ):
        assert fragment in try_move, fragment
    assert "tm.sector != thing->Sector" not in try_move


def test_route_guidance_is_bounded_and_conservative() -> None:
    text = source("src/playsim/bots/b_move.cpp")
    body = function_body(text, "bool DBot::FindRouteWaypoint")
    assert "constexpr unsigned int BotRouteMinExpansions = 512;" in text
    assert "constexpr unsigned int BotRouteMaxExpansions = 4096;" in text
    required = (
        "parent.Resize(sectorCount);",
        "const unsigned int routeExpansionBudget = clamp(sectorCount,",
        "expansions++ < routeExpansionBudget",
        "getNextSector(line, current)",
        "line->flags & (ML_BLOCKING | ML_BLOCKEVERYTHING | ML_BLOCK_PLAYERS | ML_SECRET)",
        "Level->BotInfo.IsDangerous(player->mo, next, portal, nextFloor)",
        "Level->PointInSector(candidate) == next",
        "P_LineOpening(opening, player->mo, line, portal);",
        "const bool portalWideEnough = portalWidth >= player->mo->radius * 2.0 + 8.0;",
        "const bool landingSafe =",
        "CanPlanNavigationLine(player->mo, line, current)",
        "route_waypoint_valid = true;",
        "t_route = BotRouteRefreshTics;",
    )
    for fragment in required:
        assert fragment in body, fragment

    # The guide uses the generic loaded map's sector/line topology. That makes
    # it valid for IWAD, PWAD, hub, and procedural maps alike; it cannot depend
    # on a procgen-only graph or manifest.
    assert "Level->sectors" in body
    assert "procgen" not in body.casefold()

    roam = function_body(text, "void DBot::Roam (usercmd_t *cmd)")
    for fragment in (
        "const bool allowPlannedDescent = dest == mate && mate != nullptr && !deathmatch;",
        "const bool direct = Reachable(dest, allowPlannedDescent, &targetFloor,",
        "const bool routed = !direct && FindRouteWaypoint(dest, waypoint, waypointFloor,",
        "const DVector2 goal = routed ? waypoint : dest->Pos().XY();",
        "absangle(Angle, MoveDirectionAngle((dirtype_t)player->mo->movedir)) > DAngle::fromDeg(67.5)",
        "if (!direct && t_active > 0)",
        "if (t_stuck >= 3)",
        "NewChaseDir(cmd, routed ? &goal : nullptr);",
    ):
        assert fragment in roam, fragment


def test_route_state_is_serialized_but_map_scoped() -> None:
    bot = source("src/playsim/bots/b_bot.cpp")
    for fragment in (
        "IMPLEMENT_POINTER(route_target)",
        '("routetarget", route_target)',
        '("routewaypoint", route_waypoint)',
        '("routewaypointfloor", route_waypoint_floor)',
        '("routewaypointsector", route_waypoint_sector)',
        '("routetargetsector", route_target_sector)',
        '("routewaypointvalid", route_waypoint_valid)',
        '("routewaypointallowsplanneddescent", route_waypoint_allows_planned_descent)',
        '("routewaypointusesplanneddescent", route_waypoint_uses_planned_descent)',
        '("route", t_route)',
        '("stuck", t_stuck)',
        '("blockeddirectionticks", t_blocked_direction)',
        '("blockeddirection", blocked_move_direction)',
    ):
        assert fragment in bot, fragment

    route = function_body(source("src/playsim/bots/b_move.cpp"),
                          "bool DBot::FindRouteWaypoint")
    # These guards make cache reuse safe for ordinary same-map target changes.
    # A level transition may retain the same travelling target pointer and
    # reuse the same numeric sector IDs, so it needs the explicit reset below.
    assert "route_waypoint_valid && route_target == target && t_route > 0" in route
    assert "route_target_sector == targetSector" in route
    assert "route_waypoint_sector != startSector" in route
    assert "route_waypoint_allows_planned_descent == allowPlannedDescent" in route
    assert "route_waypoint_uses_planned_descent" in route
    assert "route_waypoint_valid = false;" in route
    assert "route_waypoint_sector == startSector" in route


def test_map_travel_invalidates_transient_navigation_state() -> None:
    """A freshly relinked companion must never consume an old-map route."""

    header = source("src/playsim/bots/b_bot.h")
    bot = source("src/playsim/bots/b_bot.cpp")
    level = source("src/g_level.cpp")
    reset = function_body(bot, "void DBot::ResetNavigationAfterTravel()")
    finish_travel = function_body(level, "int FLevelLocals::FinishTravel()")

    assert "void ResetNavigationAfterTravel();" in header
    # The reset intentionally preserves player/skill/profile identity while
    # clearing every cached map coordinate, sector relation, and old-map wait.
    for fragment in (
        "route_target = nullptr;",
        "route_waypoint = { 0, 0 };",
        "route_waypoint_floor = 0.0;",
        "route_waypoint_sector = -1;",
        "route_target_sector = -1;",
        "route_waypoint_valid = false;",
        "route_waypoint_allows_planned_descent = false;",
        "route_waypoint_uses_planned_descent = false;",
        "planned_descent_active = false;",
        "t_active = 0;",
        "t_route = 0;",
        "t_stuck = 0;",
        "t_follow = 0;",
        "pendingRecoveryMoveDirection = -1;",
        "player->mo->movedir = DI_NODIR;",
        "player->mo->movecount = -1;",
    ):
        assert fragment in reset, fragment
    assert "Clear();" not in reset

    # Run the reset after generic traveller relinking/cleanup, but before the
    # Travelled callbacks can run scripts or another tick sees stale state.
    reset_at = finish_travel.index("Players[i]->Bot->ResetNavigationAfterTravel();")
    assert finish_travel.index("ClientSideThinkers.CleanUpTravellers(savegamerestore);") < reset_at
    assert reset_at < finish_travel.index("// Some ZScript will be called here")


def test_manual_mover_policy_is_explicit_and_key_aware() -> None:
    text = source("src/playsim/bots/b_move.cpp")
    whitelist = function_body(text, "bool IsNavigationMoverSpecial(int special)")
    lift_whitelist = function_body(text, "bool IsNavigationLiftSpecial(int special)")
    allowed = {
        "Door_Open",
        "Door_Raise",
        "Door_LockedRaise",
        "Door_Animated",
        "Door_WaitRaise",
        "Door_CloseWaitOpen",
        "Generic_Door",
        "Generic_Lift",
        "Plat_DownWaitUpStay",
        "Plat_DownByValue",
        "Plat_UpWaitDownStay",
        "Plat_UpByValue",
        "Plat_UpNearestWaitDownStay",
        "Plat_DownWaitUpStayLip",
        "Plat_UpByValueStayTx",
    }
    actual_doors = {
        line.strip().removeprefix("case ").removesuffix(":").strip()
        for line in whitelist.splitlines()
        if line.strip().startswith("case ")
    }
    actual_lifts = {
        line.strip().removeprefix("case ").removesuffix(":").strip()
        for line in lift_whitelist.splitlines()
        if line.strip().startswith("case ")
    }
    assert actual_doors | actual_lifts == allowed
    assert "IsNavigationLiftSpecial(special)" in whitelist
    for unsafe_action in ("Teleport", "Exit_", "ACS_", "SPAC_Cross", "SPAC_Push"):
        assert unsafe_action not in whitelist
        assert unsafe_action not in lift_whitelist

    keys = function_body(text, "bool HasRequiredKey(AActor *actor, const line_t *line)")
    for fragment in (
        "int lock = line->locknumber;",
        "line->special == Door_LockedRaise",
        "line->special == Door_Animated",
        "lock = line->args[3];",
        "line->special == Generic_Door",
        "lock = line->args[4];",
        "P_CheckKeys(actor, lock, false, true)",
    ):
        assert fragment in keys, fragment

    plan = function_body(text, "bool CanPlanNavigationLine(AActor *actor, const line_t *line, const sector_t *from)")
    plan_from_side = function_body(text, "bool CanPlanNavigationLineFromSide(AActor *actor, const line_t *line, int side)")
    use_at_yaw = function_body(text, "bool CanUseNavigationLineAtYaw(AActor *actor, line_t *line, int expectedSide, DAngle yaw)")
    use = function_body(text, "bool CanUseNavigationLine(AActor *actor, line_t *line, sector_t *from, const DVector2 &aimPoint)")
    assert "CanPlanNavigationLineFromSide(actor, line, LineSideForSector(line, from))" in plan
    assert "SPAC_Use | SPAC_UseThrough" in plan_from_side
    assert "SPAC_UseBack" in plan_from_side
    assert "if (side == 1)" in plan_from_side
    assert "SPAC_UseBack alone" in plan_from_side
    assert "ControlsAdjacentNavigationCrossing(line)" in plan_from_side
    assert "CanPlanNavigationLineFromSide(actor, line, expectedSide)" in use_at_yaw
    assert "P_TestActivateLine(line, actor, expectedSide, activation," in use_at_yaw
    assert "const DAngle savedYaw = actor->Angles.Yaw;" in use_at_yaw
    assert "actor->Angles.Yaw = yaw;" in use_at_yaw
    assert "DAngle aimYaw = actor->Angles.Yaw;" in use
    assert "aimYaw = (aimPoint - actor->Pos().XY()).Angle();" in use
    assert "return CanUseNavigationLineAtYaw(actor, line, LineSideForSector(line, from), aimYaw);" in use

    local_target = function_body(text, "bool ControlsAdjacentNavigationCrossing(const line_t *line)")
    # A tagged movement special can operate a remote door/lift. It is never a
    # route edge unless it is explicitly the local form that controls this
    # physical crossing. Generic_Door's local-door bit is the one exception.
    assert "line->args[0] == 0 || (line->args[2] & 128) != 0" in local_target
    assert "return line->args[0] == 0;" in local_target


def test_doors_wait_for_the_actual_next_tic_facing_ray() -> None:
    move = source("src/playsim/bots/b_move.cpp")
    think = source("src/playsim/bots/b_think.cpp")
    bot = source("src/playsim/bots/b_bot.cpp")
    header = source("src/playsim/bots/b_bot.h")
    body = function_body(move, "bool DBot::Move (usercmd_t *cmd)")
    validate = function_body(move, "void DBot::ValidateNavigationUseCommand(usercmd_t *cmd, DAngle nextTickYaw)")
    think_body = function_body(think, "void DBot::Think ()")
    clear_body = function_body(bot, "void DBot::Clear ()")
    reset_body = function_body(bot, "void DBot::ResetNavigationAfterTravel()")

    # Move can choose only a conservative, key-aware manual mover, but it must
    # not pulse use yet: TurnToAng has not created the yaw P_CheckUse will see.
    can_use_at = body.index("if (CanUseNavigationLine(player->mo, ld, player->mo->Sector, interactionPoint))")
    pending_at = body.index("pending_navigation_use_line = ld;")
    assert can_use_at < pending_at
    assert "cmd->buttons |= BT_USE;" not in body
    assert "t_active = BotDoorRetryTics;" not in body
    assert "P_TestActivateLine (ld, player->mo, 0, SPAC_Push)" not in body
    assert "P_ActivateLine" not in body
    assert "ClosestPointOnLineSegment(player->mo->Pos().XY(), ld)" in body

    # The deferred command records the side selected by the collision probe,
    # consumes both transient values after one tic, then proves the exact
    # portal-aware use ray can reach that line from that side before assigning
    # the edge-triggered pulse and the normal geometry wait. This prevents an
    # L-turn from using while still aimed down its old corridor or through an
    # unrelated special.
    assert (validate.index("line_t *line = pending_navigation_use_line;") <
            validate.index("pending_navigation_use_line = nullptr;"))
    assert "const int expectedSide = pending_navigation_use_side;" in validate
    assert "pending_navigation_use_side = -1;" in validate
    assert "int pending_navigation_use_side = -1;" in header
    assert "pending_navigation_use_side = -1;" in clear_body
    assert "pending_navigation_use_side = -1;" in reset_body
    assert "pending_navigation_use_side = -1;" in think_body
    assert "t_active > 0 || (cmd->buttons & BT_USE) != 0" in validate
    yaw_test = "CanUseNavigationLineAtYaw(player->mo, line, expectedSide, nextTickYaw)"
    assert yaw_test in validate
    assert validate.index(yaw_test) < validate.index("cmd->buttons |= BT_USE;")
    assert "t_active = BotDoorRetryTics;" in validate
    assert (think_body.index("ValidateMovementCommand(cmd, nextTickYaw);") <
            think_body.index("ValidateNavigationUseCommand(cmd, nextTickYaw);"))

    # The dry proof mirrors the *traversal* part of P_UseLines. It must use
    # the same portal-transition origin/range and FPathTraverse relocation,
    # reject actor hooks and earlier use specials without invoking them, and
    # reject walls which would stop P_UseTraverse before the selected line.
    ray = function_body(move, "bool NavigationUseRayReachesLine(AActor *actor, line_t *line, int expectedSide,")
    for fragment in (
        "actor->GetPortalTransition(actor->Height / 2).XY()",
        "actor->FloatVar(NAME_UseRange)",
        "PT_ADDLINES | PT_ADDTHINGS",
        "it.PortalRelocate(in, PT_ADDLINES | PT_ADDTHINGS, &xpos)",
        "intercepted->flags5 & MF5_USESPECIAL",
        "intercepted->special == UsePuzzleItem",
        "IFVIRTUALPTR(intercepted, AActor, Used)",
        "if (traversed == line)",
        "P_PointOnLineSide(xpos.XY(), traversed) != expectedSide",
        "SPAC_Use | SPAC_UseThrough | SPAC_UseBack",
        "ML_BLOCKEVERYTHING | ML_BLOCKUSE",
        "P_LineOpening(open, nullptr, traversed, it.InterceptPoint(in));",
        "COMPATF_USEBLOCKING",
    ):
        assert fragment in ray, fragment
    assert "P_ActivateLine" not in ray
    assert "P_TestActivateLine" not in ray

    # The selected side is recorded alongside the line at the original
    # collision point; a later state/sector change cannot silently flip it.
    assert "int selectedInteractionSide = -1;" in body
    assert "selectedInteractionSide = interactionSide;" in body
    assert "pending_navigation_use_side = selectedInteractionSide;" in body


def test_door_use_requires_a_safe_reason_or_a_verified_low_side_lift() -> None:
    """Unsafe space cannot trigger use, while an ordinary low-side lift can."""

    move = source("src/playsim/bots/b_move.cpp")
    header = source("src/playsim/bots/b_bot.h")
    clean = function_body(move, "bool FCajunMaster::CleanAhead")
    move_body = function_body(move, "bool DBot::Move (usercmd_t *cmd)")

    assert "enum class EBotMoveBlockReason" in header
    assert "EBotMoveBlockReason *blockReason = nullptr" in header
    assert "EBotMoveBlockReason *blockReason" in move
    assert "setBlockReason(EBotMoveBlockReason::None);" in clean
    geometry_failure = clean.index("if (!SafeCheckPosition(thing, x, y, tm))")
    assert "setBlockReason(EBotMoveBlockReason::Geometry);" in clean[geometry_failure:]
    assert clean.count("setBlockReason(EBotMoveBlockReason::VerticalStep);") == 2
    assert clean.count("setBlockReason(EBotMoveBlockReason::Unsafe);") >= 3

    assert "EBotMoveBlockReason blockReason;" in move_body
    assert "planned_descent_floor, &blockReason" in move_body
    assert "blockReason == EBotMoveBlockReason::Geometry" in move_body
    assert "blockReason == EBotMoveBlockReason::VerticalStep" in move_body
    assert "!IsNavigationLiftSpecial(ld->special)" in move_body
    safety_gate = move_body.index("if (!canOperateTransition || !spechit.Size())")
    use_scan = move_body.index("while (spechit.Pop")
    assert safety_gate < use_scan


def test_all_movement_paths_receive_clearance_and_hazard_checks() -> None:
    move = source("src/playsim/bots/b_move.cpp")
    think = source("src/playsim/bots/b_think.cpp")
    p_map = source("src/playsim/p_map.cpp")
    assert "void DBot::ValidateMovementCommand(usercmd_t *cmd, DAngle nextTickYaw)" in move
    assert "CleanAhead(player->mo" in move
    assert "IsDangerous(thing, landingSector, DVector2(x, y), tm.floorz)" in move
    assert "thing->floorz + thing->MaxStepHeight" in move
    assert "cmd->buttons |= BT_JUMP;" not in move
    assert "const DAngle nextTickYaw = actor->Angles.Yaw + quantizedYaw;" in think
    assert "ValidateMovementCommand(cmd, nextTickYaw);" in think
    assert "ValidateAttackCommand(cmd, nextTickYaw, nextTickPitch);" in think
    assert "tm.pos.XY(), tm.floorz" in p_map

    # Think validates the final command after all behavior paths (follow,
    # combat, roam, recovery) have contributed input, before ThinkForMove
    # records that a movement command was actually requested.
    think_body = function_body(think, "void DBot::Think ()")
    assert think_body.index("ApplyPendingRecoveryMoveCommand") < think_body.index("ValidateMovementCommand")
    assert "movement_requested = cmd->forwardmove != 0 || cmd->sidemove != 0;" in think


def test_stuck_recovery_and_door_waits_do_not_fight_each_other() -> None:
    think = function_body(source("src/playsim/bots/b_think.cpp"), "void DBot::Think ()")
    roam = function_body(source("src/playsim/bots/b_move.cpp"), "void DBot::Roam (usercmd_t *cmd)")
    assert "if (movement_requested)" in think
    assert "(player->mo->Pos().XY() - old).LengthSquared() < 4.0" in think
    assert "++t_stuck;" in think
    assert "movement_requested = false;" in think
    assert "if (!direct && t_active > 0)" in roam
    assert "route_waypoint_valid = false;" in roam
    assert "t_route = 0;" in roam


def test_companion_regroup_is_a_leader_priority_intent() -> None:
    """A cooperative bot must route back to its leader before loot or pursuit."""

    think = source("src/playsim/bots/b_think.cpp")
    body = function_body(think, "void DBot::ThinkForMove (usercmd_t *cmd)")
    pickup = function_body(think, "void DBot::WhatToGet (AActor *item)")

    for fragment in (
        "static constexpr double CompanionSoftLeashDistance = 320.0;",
        "static constexpr double CompanionHardLeashDistance = 512.0;",
        "static constexpr double CompanionLocalPickupDistance = 320.0;",
        "const bool mustRegroupWithMate = !deathmatch && mate != nullptr &&",
        "mateDistance > CompanionSoftLeashDistance || t_stuck >= 3",
    ):
        assert fragment in think, fragment

    # Missile evasion remains the only higher-priority behavior.  Regrouping
    # must occur before the raw combat branch so a visible monster cannot pull
    # a companion through the next room.
    regroup_at = body.index("else if (mustRegroupWithMate)")
    combat_at = body.index("else if (!mustFollowPlannedDescent && enemy")
    assert regroup_at < combat_at
    regroup = body[regroup_at:combat_at]
    assert "dest = mate;" in regroup
    assert "Roam(cmd);" in regroup
    assert "TryCatchUpToMate()" in regroup
    assert "mateDistance >= CompanionHardLeashDistance" in regroup
    # The blocked-movement ticker feeds back into the same route path instead
    # of leaving direct follow/combat to press the same wall forever.
    assert "t_stuck >= 3" in body[:regroup_at]

    # Optional rewards are confined to the leader's local district and cannot
    # replace a follower that has already fallen beyond its soft leash.
    assert "item->Distance2D(mate) > CompanionLocalPickupDistance" in pickup
    assert "player->mo->Distance2D(mate) > CompanionSoftLeashDistance" in pickup
    assert pickup.index("CompanionLocalPickupDistance") < pickup.index("if (!Reachable(item))")


def test_travel_tolerates_a_transient_companion_without_a_pawn() -> None:
    """Map exits must not dereference a companion awaiting/rejecting a spawn."""

    level = source("src/g_level.cpp")
    bots = source("src/playsim/bots/b_game.cpp")
    start_travel = function_body(level, "void FLevelLocals::StartTravel()")
    add_traveller = function_body(level, "void FLevelLocals::AddToTravellingList(DThinker* th)")
    add_bot = function_body(bots, "bool FCajunMaster::DoAddBot")
    reject = function_body(bots, "void FCajunMaster::RejectUnplaceableCompanion")

    # The safe general-purpose list helper already accepts null; StartTravel
    # must also avoid reading the null player pawn while iterating live slots.
    assert "if (th == nullptr ||" in add_traveller
    guard = "PlayerInGame(i) && Players[i]->mo != nullptr && Players[i]->health > 0"
    assert guard in start_travel
    assert start_travel.index(guard) < start_travel.index("AddToTravellingList(Players[i]->mo)")

    # Both creation and failed placement explicitly recognize the short-lived
    # no-pawn state, so the travel guard is a narrow safety net rather than a
    # way to retain an invalid companion indefinitely.
    assert "!playeringame[bnum] || players[bnum].mo == nullptr" in add_bot
    assert "ClearPlayer(player, true);" in reject


def test_item_and_combat_selection_stay_local_and_safe() -> None:
    func = source("src/playsim/bots/b_func.cpp")
    think = source("src/playsim/bots/b_think.cpp")
    assert "mo->Distance2D(players[i].mo) <= 1024.0" in func
    assert "P_CheckSight(players[i].mo, mo, SF_SEEPASTBLOCKEVERYTHING)" in func
    assert "static constexpr double BotPickupMaxDistance = 1024.0;" in think
    pickup = function_body(think, "void DBot::WhatToGet (AActor *item)")
    # BotTick is a cheap caller-side filter, but the policy belongs here too:
    # the periodic inventory iterator invokes WhatToGet directly.
    assert "item->Distance2D(player->mo) > BotPickupMaxDistance" in pickup
    assert "P_CheckSight(player->mo, item, SF_SEEPASTBLOCKEVERYTHING)" in pickup
    assert pickup.index("BotPickupMaxDistance") < pickup.index("if (!Reachable(item))")
    assert "if (!Reachable(item))" in pickup
    assert "WhatToGet(item);" in think
    assert "bool DBot::CanFireAt(AActor *target, const BotInfoData &weaponInfo," in func
    assert "ML_BLOCKEVERYTHING | ML_BLOCKHITSCAN" in func
    assert "trace.Actor == target" in func
    assert "blastRadius" in func
    assert "mo->player ? !mo->IsTeammate" not in func

    set_enemy = function_body(think, "void DBot::Set_enemy ()")
    assert "P_CheckSight (player->mo, enemy)" in set_enemy
    assert "if (deathmatch || !oldenemy)" in set_enemy

    assert "DAngle firingYaw, DAngle firingPitch) const" in func
    fire = function_body(func, "bool DBot::CanFireAt(AActor *target, const BotInfoData &weaponInfo,")
    # The final command's exact ray remains mandatory for projectiles,
    # explosive/BFG weapons, and unknown mod weapons. Known hitscan weapons
    # may use the much narrower native-autoaim lane only when that exact ray
    # is clear of every actor; this lets a companion acquire a hostile while
    # turning without creating a shoot-through-teammates exception.
    for fragment in (
        "const DVector3 firingDirection(",
        "pitchCos * firingYaw.Cos()",
        "-firingPitch.Sin()",
        "const bool exactTargetHit = Trace(",
        "if (!exactTargetHit)",
        "if (explosive || bfg || trace.HitType == TRACE_HitActor ||",
        "!CanUseKnownHitscanAimLane(source, target, weaponInfo, firingYaw,",
        "if (!explosive && !bfg)",
        "const double blastRadius = bfg ? 256.0 : 160.0;",
        "source->IsTeammate(friendActor)",
        "const DVector2 impactXY = trace.HitPos.XY();",
        "friendActor->Pos().XY() - impactXY",
        "const DVector2 closest = startXY + shot * fraction;",
        "friendActor->radius + 24.0",
        "source->Pos().XY() - impactXY",
    ):
        assert fragment in fire, fragment

    helper = function_body(func, "bool CanUseKnownHitscanAimLane(AActor *source, AActor *target,")
    for fragment in (
        "BIF_BOT_FALLBACK",
        "weaponInfo.projectileType != nullptr",
        "BIF_BOT_EXPLOSIVE | BIF_BOT_BFG | BIF_BOT_NO_FIRE",
        "WIF_NOAUTOAIM",
        "const DAngle horizontalAutoAim",
        "const DAngle offsets[] = { nullAngle, horizontalAutoAim, -horizontalAutoAim };",
        "P_AimLineAttack(source, firingYaw + offset,",
        "FTraceResults autoAimTrace = {};",
        "autoAimTrace.HitType == TRACE_HitActor && autoAimTrace.Actor == target",
        "HasTeammateInHitscanLane(source, start, targetCenter, horizontalAutoAim)",
    ):
        assert fragment in helper, fragment
    lane = function_body(func, "bool HasTeammateInHitscanLane(AActor *source, const DVector3 &start,")
    assert "source->IsTeammate(friendActor)" in lane
    assert "CompanionHitscanLaneMargin" in lane

    dofire = function_body(func, "void DBot::Dofire (usercmd_t *cmd)")
    assert "if (weaponInfo.flags & BIF_BOT_EXPLOSIVE)" in dofire
    assert "else\n\t\t{\n\t\t\t// Generic projectile weapons" in dofire
    assert "if (deathmatch)" in dofire
    assert "ValidateAttackCommand" not in dofire
    validate_attack = function_body(func, "void DBot::ValidateAttackCommand(usercmd_t *cmd, DAngle nextTickYaw,")
    assert "cmd->buttons &= ~BT_ATTACK;" in validate_attack
    assert "CanFireAt(enemy, weaponInfo, nextTickYaw, nextTickPitch)" in validate_attack
    assert "nextTickYaw, nextTickPitch" in validate_attack
    find_enemy = function_body(func, "AActor *DBot::Find_enemy ()")
    assert "P_BlockmapSearch(player->mo, 20, FindVisibleBotEnemyInBlock, nullptr)" in find_enemy
    visible_search = function_body(func, "AActor *FindVisibleBotEnemyInBlock(AActor *source, int blockIndex, void *)")
    assert "source->IsOkayToAttack(candidate)" in visible_search

    tick = function_body(func, "void FCajunMaster::BotTick(AActor *mo)")
    for fragment in (
        "const double closing = toBot.X * velocity.X + toBot.Y * velocity.Y;",
        "const bool incoming = (mo->flags3 & MF3_WARNBOT)",
        "if (bot->missile == mo && !incoming)",
        "eta + 0.25 < threatEta(current)",
    ):
        assert fragment in tick, fragment
    think_for_move = function_body(think, "void DBot::ThinkForMove (usercmd_t *cmd)")
    assert "!missile->Vel.X && !missile->Vel.Y" in think_for_move


def main() -> None:
    test_reachability_uses_real_portal_geometry()
    test_route_probes_actual_3d_support_and_same_sector_hazards()
    test_route_guidance_is_bounded_and_conservative()
    test_route_state_is_serialized_but_map_scoped()
    test_map_travel_invalidates_transient_navigation_state()
    test_manual_mover_policy_is_explicit_and_key_aware()
    test_doors_wait_for_the_actual_next_tic_facing_ray()
    test_door_use_requires_a_safe_reason_or_a_verified_low_side_lift()
    test_all_movement_paths_receive_clearance_and_hazard_checks()
    test_stuck_recovery_and_door_waits_do_not_fight_each_other()
    test_companion_regroup_is_a_leader_priority_intent()
    test_travel_tolerates_a_transient_companion_without_a_pawn()
    test_item_and_combat_selection_stay_local_and_safe()
    print("PASS: bot navigation and behavior contracts")


if __name__ == "__main__":
    main()
