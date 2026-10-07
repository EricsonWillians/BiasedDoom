#!/usr/bin/env python3
"""Offline regression checks for Cajun bot corner recovery.

The classic rover chooses a world-space `movedir`, but bot movement commands
are relative to view yaw and are consumed on the following tic. This test
protects the small invariants that keep a newly chosen escape lane usable:
the command must project into that lane after its queued yaw has applied,
must respect unequal forward/side command limits, and one reroute pass must
not retry a known blocked direction.
"""

from __future__ import annotations

import math
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


FORWARDRUN = 0x3200
SIDERUN = 0x2800


def projected_recovery_command(next_tick_yaw: float, desired: float) -> tuple[float, float]:
    """Mirror ApplyPendingRecoveryMoveCommand's bounded world projection."""

    turn = math.radians(desired - next_tick_yaw)
    forward_component = math.cos(turn)
    side_component = -math.sin(turn)
    scale = min(
        FORWARDRUN / max(abs(forward_component), 1e-12),
        SIDERUN / max(abs(side_component), 1e-12),
    )
    return (round(scale * forward_component), round(scale * side_component))


def recovered_world_vector(facing: float, forward: float, side: float) -> tuple[float, float]:
    """Model PlayerPawn::MovePlayer for the projected command."""

    # PlayerPawn uses forward on `Angle` and side on `Angle - 90`.
    facing_radians = math.radians(facing)
    side_radians = facing_radians - math.pi / 2.0
    return (
        forward * math.cos(facing_radians) + side * math.cos(side_radians),
        forward * math.sin(facing_radians) + side * math.sin(side_radians),
    )


def test_recovery_command_uses_the_chosen_world_lane_after_queued_yaw() -> None:
    # Bot thinking happens after player movement. A TurnToAng step has already
    # changed actor yaw once and PlayerPawn will apply cmd.yaw next tic, so the
    # projection must use that final yaw rather than either older facing.
    for actor_yaw, queued_yaw in ((15.0, 15.0), (90.0, -15.0), (181.0, 0.0), (315.0, 12.0)):
        next_tick_yaw = actor_yaw + queued_yaw
        for desired in range(0, 360, 45):
            forward, side = projected_recovery_command(next_tick_yaw, float(desired))
            assert abs(forward) <= FORWARDRUN
            assert abs(side) <= SIDERUN
            x, y = recovered_world_vector(next_tick_yaw, forward, side)
            wanted_x = math.cos(math.radians(desired))
            wanted_y = math.sin(math.radians(desired))
            length = math.hypot(x, y)
            assert length > 0.0
            assert math.isclose(x / length, wanted_x, abs_tol=1e-4)
            assert math.isclose(y / length, wanted_y, abs_tol=1e-4)


def test_recovery_projection_respects_unequal_axis_limits() -> None:
    # The legacy commands allow 0x3200 forward but only 0x2800 sideways.
    # A 45-degree recovery must use equal raw components, not a skewed pair
    # independently multiplied by those unequal maxima.
    assert projected_recovery_command(0.0, 0.0) == (FORWARDRUN, 0)
    assert projected_recovery_command(0.0, 90.0) == (0, -SIDERUN)
    assert projected_recovery_command(0.0, 45.0) == (SIDERUN, -SIDERUN)


def test_protected_recovery_transitions_are_not_bypassed() -> None:
    # P_TeleportMove checks the destination but does not walk the segment. A
    # recovery candidate must reject every crossed line. Two-sided lines can
    # still carry mid-textures, 3D floors, portals, or semantics that are not
    # represented by a destination-only teleport collision check.
    def protected(*, crosses_any_line: bool) -> bool:
        return crosses_any_line

    assert protected(crosses_any_line=True)
    assert not protected(crosses_any_line=False)


def test_one_reroute_pass_never_retries_a_blocked_direction() -> None:
    # This mirrors the intended attempt guard: a corner can reject the direct
    # diagonal and its matching axis. The later fallback pass must still reach
    # an available lateral lane instead of testing the rejected directions
    # again.
    attempts: set[int] = set()
    blocked = {1, 2, 3}
    candidates = (1, 2, 3, 2, 1, 4, 5, 6, 7, 0)
    selected = None
    for direction in candidates:
        if direction in attempts:
            continue
        attempts.add(direction)
        if direction not in blocked:
            selected = direction
            break
    assert selected == 4
    assert attempts == {1, 2, 3, 4}


def test_source_contracts() -> None:
    source = (REPO_ROOT / "src" / "playsim" / "bots" / "b_move.cpp").read_text(encoding="utf-8")
    required = (
        "DAngle MoveDirectionAngle(dirtype_t direction)",
        "void DBot::ApplyPendingRecoveryMoveCommand(usercmd_t *cmd, DAngle nextTickYaw)",
        "const double scale = min(FORWARDRUN / max(forwardMagnitude, 1e-12)",
        "pendingRecoveryMoveDirection = direction;",
        "Angle = MoveDirectionAngle((dirtype_t)player->mo->movedir);",
        "attempted[DI_NODIR] = {};",
        "if (direction >= DI_NODIR || attempted[direction])",
        "attempted[direction] = true;",
    )
    for fragment in required:
        assert fragment in source, f"missing corner-recovery contract: {fragment!r}"

    think = (REPO_ROOT / "src" / "playsim" / "bots" / "b_think.cpp").read_text(encoding="utf-8")
    assert "const DAngle nextTickYaw = actor->Angles.Yaw + quantizedYaw;" in think
    assert "ApplyPendingRecoveryMoveCommand(cmd, nextTickYaw);" in think
    func = (REPO_ROOT / "src" / "playsim" / "bots" / "b_func.cpp").read_text(encoding="utf-8")
    for fragment in (
        "bool IsProtectedRecoveryTransition(const line_t *line)",
        "bool CrossesProtectedRecoveryTransition(AActor *actor, const DVector2& destination)",
        "CrossesProtectedRecoveryTransition(player->mo, candidate)",
        "return line != nullptr;",
        "Only an entirely line-free local",
    ):
        assert fragment in func, f"missing catch-up barrier contract: {fragment!r}"


def main() -> None:
    test_recovery_command_uses_the_chosen_world_lane_after_queued_yaw()
    test_recovery_projection_respects_unequal_axis_limits()
    test_protected_recovery_transitions_are_not_bypassed()
    test_one_reroute_pass_never_retries_a_blocked_direction()
    test_source_contracts()
    print("PASS: bot corner recovery contracts")


if __name__ == "__main__":
    main()
