"""Read-only companion doorway passability fixture.

The command sequence is queued while the game is still local. It adds one
companion at Player 2's one-player-wide corridor start, enables the engine's
time freeze (which pauses companion thought but keeps Player 1 movement live),
then holds Player 1 forward into and through that stationary companion. The
terminal wall is the independent map-collision control.
"""

import biaseddoom as bd


FINAL_TIC = 125
START_FORWARD_TIC = 28
TERMINAL_WALL_X = 384.0
# PlayerPawn's normal 16-unit radius stops its center at roughly x=368 against
# the terminal x=384 wall. Leave a one-unit tolerance for fixed-point motion;
# a no-clip pawn would continue far beyond this bound while the held input is
# still active.
PLAYER_RADIUS_MARGIN = 15.0
PASS_MARGIN = 20.0

command_queued = False
companion_seen = None
frozen_position = None
passed_companion = False
max_player_x = float("-inf")


def companion_player():
    for player in bd.player_refs():
        if player.index != 0 and player.actor is not None:
            return player
    return None


@bd.on("map_load")
def map_loaded(event):
    bd.assert_true(bd.current_map().upper() == "DOORPASS", "fixture requires DOORPASS")
    bd.log(f"BOT_DOORPASS map={bd.current_map()}")


@bd.on("pre_tick")
def observe_doorway_passability(event):
    global command_queued, companion_seen, frozen_position, passed_companion, max_player_x

    now = bd.level_time()
    leader = bd.player(0)
    bd.assert_true(leader is not None and leader.actor is not None, "Player 1 exists")
    if leader is None or leader.actor is None:
        return

    # Everything after this call uses normal queued console input; Python never
    # writes an actor once addcompanion turns the session cooperative.
    if now == 1 and not command_queued:
        bd.execute("addcompanion; wait 12; freeze; wait 12; +forward; wait 80; -forward")
        command_queued = True

    companion = companion_player()
    if companion is not None and companion.actor is not None and companion_seen is None:
        companion_seen = companion.actor
        bd.log("BOT_DOORPASS companion_joined "
               f"tic={now} position={companion.actor.position}")

    if now == START_FORWARD_TIC:
        bd.assert_true(companion_seen is not None and companion is not None and companion.actor is not None,
                       "companion joined before doorway traversal")
        if companion is not None and companion.actor is not None:
            frozen_position = companion.actor.position
            bd.log("BOT_DOORPASS frozen_companion "
                   f"tic={now} position={frozen_position}")

    max_player_x = max(max_player_x, leader.actor.x)
    if frozen_position is not None and leader.actor.x > frozen_position[0] + PASS_MARGIN:
        passed_companion = True

    if now == FINAL_TIC:
        bd.assert_true(frozen_position is not None, "stationary companion position was sampled")
        bd.assert_true(companion is not None and companion.actor is not None,
                       "companion remains live")
        if companion is None or companion.actor is None or frozen_position is None:
            return

        dx = abs(companion.actor.x - frozen_position[0])
        dy = abs(companion.actor.y - frozen_position[1])
        bd.log("BOT_DOORPASS final "
               f"player={leader.actor.position} companion={companion.actor.position} "
               f"frozen={frozen_position} passed={passed_companion} "
               f"max_player_x={max_player_x} companion_drift=({dx},{dy})")
        bd.assert_true(dx <= 0.01 and dy <= 0.01,
                       "time-frozen companion remained stationary in the doorway")
        bd.assert_true(passed_companion,
                       "Player 1 crossed the stationary companion in the narrow doorway")
        bd.assert_true(max_player_x < TERMINAL_WALL_X - PLAYER_RADIUS_MARGIN,
                       "terminal wall still blocks Player 1 after companion pass-through")
        bd.log("BOT_DOORPASS PASS")
