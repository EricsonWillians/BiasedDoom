"""Read-only runtime regression for companion leader-priority routing.

Player 1 is moved before the public ``addcompanion`` command turns the test
into co-op.  Afterwards the fixture observes only normal bot movement: the
fresh companion must take the east-then-north route around a wall, rather than
randomly selecting an item/player or staring at the leader through geometry.
"""

import math

import biaseddoom as bd


LEADER_X = 96.0
LEADER_Y = 192.0
FINAL_TIC = 180
FORMATION_DISTANCE = 240.0
# A stock player or bot cannot legitimately cover this much horizontal ground
# in one tic. Keep the bound deliberately generous so it catches only a
# teleport-style recovery, not ordinary movement, a diagonal, or tick jitter.
MAX_COMPANION_STEP = 96.0

command_queued = False
initial_companion = None
saw_middle_room = False
saw_north_turn = False
last_companion_position = None
last_companion_tic = None
max_companion_step = 0.0


def companion_player():
    for candidate in bd.player_refs():
        if candidate.index != 0 and candidate.actor is not None:
            return candidate
    return None


@bd.on("map_load")
def map_loaded(event):
    bd.assert_true(bd.current_map().upper() == "FOLLOWRT", "fixture requires FOLLOWRT")
    bd.log(f"BOT_FOLLOW_ROUTE map={bd.current_map()}")


@bd.on("pre_tick")
def observe_native_companion_route(event):
    global command_queued, initial_companion, saw_middle_room, saw_north_turn
    global last_companion_position, last_companion_tic, max_companion_step

    now = bd.level_time()
    leader = bd.player(0)
    bd.assert_true(leader is not None and leader.actor is not None, "leader pawn exists")
    if leader is None or leader.actor is None:
        return

    if now == 1:
        # The one permitted map mutation happens while this is still a local
        # game. The route itself is then entirely native co-op bot behavior.
        moved = leader.actor.set_position(LEADER_X, LEADER_Y, 0.0, check=True, fog=False)
        bd.assert_true(bool(moved), "leader moved into the north route room")
        bd.log(f"BOT_FOLLOW_ROUTE leader_ready position={leader.actor.position}")

    if now == 2 and not command_queued:
        bd.execute("addcompanion")
        command_queued = True

    companion = companion_player()
    if initial_companion is not None:
        bd.assert_true(companion is not None and companion.actor is not None,
                       "original companion remains present on every route tic")
    if companion is not None and companion.actor is not None:
        pawn = companion.actor
        if initial_companion is None:
            initial_companion = pawn
            bd.log(f"BOT_FOLLOW_ROUTE companion_start tic={now} position={pawn.position}")
            bd.assert_true(pawn.x < 0.0 and abs(pawn.y) < 64.0,
                           "companion began in the west room")

        if last_companion_position is not None and last_companion_tic is not None:
            elapsed_tics = max(1, now - last_companion_tic)
            step = math.hypot(pawn.x - last_companion_position[0],
                              pawn.y - last_companion_position[1])
            max_companion_step = max(max_companion_step, step / elapsed_tics)
            bd.assert_true(step <= MAX_COMPANION_STEP * elapsed_tics,
                           "companion crossed the route using normal movement, not a teleport")
        last_companion_position = (pawn.x, pawn.y)
        last_companion_tic = now

        # Sector 1 lies east of the first portal; sector 2 starts north of the
        # second. Sampling both makes a direct-through-wall path or an emergency
        # teleport unable to masquerade as normal routing.
        if pawn.x > 8.0 and pawn.y < 64.0:
            saw_middle_room = True
        if pawn.x >= 64.0 and pawn.y > 80.0:
            saw_north_turn = True

    if now == FINAL_TIC:
        bd.assert_true(initial_companion is not None,
                       "companion joined the route fixture")
        bd.assert_true(companion is not None and companion.actor is not None,
                       "companion remains live")
        if companion is None or companion.actor is None:
            return

        pawn = companion.actor
        distance = math.hypot(pawn.x - leader.actor.x, pawn.y - leader.actor.y)
        same_pawn = pawn == initial_companion
        bd.log("BOT_FOLLOW_ROUTE final "
               f"position={pawn.position} distance={distance:.2f} "
               f"middle={saw_middle_room} north_turn={saw_north_turn} "
               f"same_pawn={same_pawn} max_step={max_companion_step:.2f}")
        bd.assert_true(same_pawn, "original companion completed the route")
        bd.assert_true(saw_middle_room and saw_north_turn,
                       "companion took the physical east-then-north route")
        bd.assert_true(distance <= FORMATION_DISTANCE,
                       "companion regrouped within the normal formation band")
        bd.log("BOT_FOLLOW_ROUTE PASS")
