"""Live stock-Doom-II MAP01 companion regression with hostiles preserved.

This fixture deliberately does not reposition, damage, destroy, or otherwise
mutate an actor. It schedules normal console input after MAP01 is live: native
``addcompanion`` followed by Player 1's normal ``+forward`` action. Player 1
consequently walks off the real 56 -> 8 unit opening while MAP01's nearby stock
monsters remain in place. The assertions below observe native movement only.
"""

import biaseddoom as bd


EXPECTED_START_FLOOR = 56.0
EXPECTED_LOWER_FLOOR = 8.0
START_X = -96.0
START_Y = 784.0
LOWER_EXTERIOR_Y = 840.0
NEARBY_HOSTILE_RADIUS = 320.0
MIN_NEARBY_HOSTILES = 2
# Native companion recovery is intentionally delayed by this amount.  An
# earlier lower-floor transition therefore has to be normal bot navigation,
# rather than the bounded catch-up fallback.
NATIVE_DESCENT_DEADLINE = 2 * bd.TICRATE
FINAL_TIC = 280


leader_start_actor = None
leader_start_seen = False
leader_forward_input_seen = False
leader_crossed_at_tic = None
companion_start_seen = False
companion_start_tic = None
initial_companion_actor = None
companion_descended_at_tic = None
nearby_hostile_refs = []
nearby_hostiles_at_start = 0
nearby_hostiles_at_join = None
nearby_hostiles_snapshotted = False


def companion_player():
    """Return the one companion introduced by the native command fixture."""

    for candidate in bd.player_refs():
        if candidate.index != 0 and candidate.actor is not None:
            return candidate
    return None


def nearby_stock_hostiles():
    """Return live MAP01 monsters adjacent to its untouched P1 start."""

    nearby = []
    radius_squared = NEARBY_HOSTILE_RADIUS * NEARBY_HOSTILE_RADIUS
    for actor in bd.actor_refs():
        if not actor.valid or not actor.alive or not actor.is_monster:
            continue
        dx = actor.x - START_X
        dy = actor.y - START_Y
        if dx * dx + dy * dy <= radius_squared:
            nearby.append(actor)
    return nearby


def retained_hostile_count():
    """Count the original nearby monsters, not a later replacement spawn."""

    return sum(1 for actor in nearby_hostile_refs if actor.valid and actor.alive)


@bd.on("map_load")
def map_loaded(event):
    bd.assert_true(bd.current_map().upper() == "MAP01", "fixture requires Doom II MAP01")
    bd.log(f"BOT_MAP01_HOSTILES map={bd.current_map()}")


@bd.on("pre_tick")
def drive_and_observe_map01(event):
    """Observe a real player-led descent with the map's monsters untouched."""

    global leader_start_actor, leader_start_seen, leader_forward_input_seen
    global leader_crossed_at_tic, companion_start_seen, companion_start_tic
    global initial_companion_actor, nearby_hostile_refs, nearby_hostiles_at_start
    global nearby_hostiles_at_join, companion_descended_at_tic, nearby_hostiles_snapshotted

    now = bd.level_time()
    leader = bd.player(0)
    bd.assert_true(leader is not None and leader.actor is not None,
                   "MAP01 leader pawn exists")
    if leader is None or leader.actor is None:
        return
    leader_pawn = leader.actor

    if not leader_start_seen:
        leader_start_actor = leader_pawn
        leader_start_seen = True
        starts_on_platform = (leader_pawn.alive and
                              abs(leader_pawn.floor_z - EXPECTED_START_FLOOR) <= 0.01)
        bd.log("BOT_MAP01_HOSTILES leader_start "
               f"tic={now} position={leader_pawn.position} floor={leader_pawn.floor_z} "
               f"angle={leader_pawn.angle} elevated_platform={starts_on_platform}")
        bd.assert_true(starts_on_platform,
                       "Player 1 began on MAP01's elevated floor-56 platform")

    if now == 5:
        # This is the same native command path an interactive host uses. The
        # waits leave the companion on the stock elevated start before Player
        # 1 walks the ledge, while keeping every actor mutation inside the
        # engine's usual command and input systems.
        bd.execute("addcompanion; wait 30; +forward; wait 20; -forward")

    # Some stock MAP01 actors enter the thinker list after map_load. Capture
    # the baseline after that initialization rather than treating a partial
    # tic-zero list as proof that monsters were removed.
    if now == 10 and not nearby_hostiles_snapshotted:
        nearby_hostile_refs = nearby_stock_hostiles()
        nearby_hostiles_at_start = len(nearby_hostile_refs)
        nearby_hostiles_snapshotted = True
        bd.log("BOT_MAP01_HOSTILES hostiles_retained "
               f"initial={nearby_hostiles_at_start} radius={NEARBY_HOSTILE_RADIUS}")
        bd.assert_true(nearby_hostiles_at_start >= MIN_NEARBY_HOSTILES,
                       "nearby stock MAP01 hostiles were retained for the regression")

    # drive.cfg holds the real +forward action.  Seeing the native command in
    # the local player's tic command establishes that the descent did not use
    # a position/velocity write in this fixture.
    if abs(leader.forward_move) > 0.0:
        leader_forward_input_seen = True

    lower_exterior = (leader_pawn.alive and
                      abs(leader_pawn.floor_z - EXPECTED_LOWER_FLOOR) <= 0.01 and
                      leader_pawn.y > LOWER_EXTERIOR_Y)
    if lower_exterior and leader_crossed_at_tic is None:
        same_leader_pawn = leader_pawn == leader_start_actor
        leader_crossed_at_tic = now
        bd.log("BOT_MAP01_HOSTILES leader_descended "
               f"tic={now} position={leader_pawn.position} floor={leader_pawn.floor_z} "
               f"original_pawn={same_leader_pawn} "
               f"native_forward={leader_forward_input_seen}")
        bd.assert_true(same_leader_pawn,
                       "the original Player 1 pawn walked across MAP01's first ledge")
        bd.assert_true(leader_forward_input_seen,
                       "Player 1 descent used the native +forward command")

    companion = companion_player()
    if companion is not None and companion.actor is not None and not companion_start_seen:
        pawn = companion.actor
        starts_on_platform = (pawn.alive and
                              abs(pawn.floor_z - EXPECTED_START_FLOOR) <= 0.01)
        companion_start_seen = True
        companion_start_tic = now
        initial_companion_actor = pawn
        bd.log("BOT_MAP01_HOSTILES companion_start "
               f"tic={now} slot={companion.index} position={pawn.position} "
               f"floor={pawn.floor_z} elevated_platform={starts_on_platform}")
        bd.assert_true(starts_on_platform,
                       "fresh companion began on MAP01's elevated floor-56 platform")

    if companion_start_seen and nearby_hostiles_snapshotted and nearby_hostiles_at_join is None:
        nearby_hostiles_at_join = retained_hostile_count()
        bd.log("BOT_MAP01_HOSTILES hostiles_at_join "
               f"count={nearby_hostiles_at_join}")
        bd.assert_true(nearby_hostiles_at_join >= MIN_NEARBY_HOSTILES,
                       "nearby stock hostiles still existed when the companion joined")

    if (companion_start_seen and companion is not None and companion.actor is not None and
            companion_descended_at_tic is None):
        pawn = companion.actor
        companion_lower_exterior = (pawn.alive and
                                    abs(pawn.floor_z - EXPECTED_LOWER_FLOOR) <= 0.01 and
                                    pawn.y > LOWER_EXTERIOR_Y)
        if companion_lower_exterior:
            same_companion_pawn = pawn == initial_companion_actor
            companion_descended_at_tic = now
            native_before_catchup = companion_descended_at_tic < NATIVE_DESCENT_DEADLINE
            bd.log("BOT_MAP01_HOSTILES companion_descended "
                   f"tic={now} position={pawn.position} floor={pawn.floor_z} "
                   f"original_pawn={same_companion_pawn} "
                   f"native_before_catchup={native_before_catchup}")
            bd.assert_true(same_companion_pawn,
                           "the original companion pawn made the MAP01 descent")
            bd.assert_true(native_before_catchup,
                           "companion reached MAP01 exterior before catch-up recovery")

    if now == FINAL_TIC:
        bd.log("BOT_MAP01_HOSTILES final "
               f"leader_crossed_at_tic={leader_crossed_at_tic} "
               f"companion_start_tic={companion_start_tic} "
               f"companion_descended_at_tic={companion_descended_at_tic} "
               f"hostiles_initial={nearby_hostiles_at_start} "
               f"hostiles_at_join={nearby_hostiles_at_join}")
        bd.assert_true(leader_start_seen, "Player 1 start platform was observed")
        bd.assert_true(leader_forward_input_seen,
                       "the fixture observed normal Player 1 forward input")
        bd.assert_true(leader_crossed_at_tic is not None,
                       "Player 1 naturally crossed the MAP01 opening")
        bd.assert_true(companion_start_seen,
                       "the native addcompanion command deployed a companion")
        bd.assert_true(companion_start_tic is not None and
                       leader_crossed_at_tic is not None and
                       companion_start_tic <= leader_crossed_at_tic,
                       "companion joined before or during Player 1's natural ledge crossing")
        bd.assert_true(companion_descended_at_tic is not None,
                       "companion followed Player 1 through MAP01's safe opening")
        bd.assert_true(initial_companion_actor is not None and
                       companion is not None and companion.actor == initial_companion_actor,
                       "the original companion pawn remained live through the route")
        bd.assert_true(nearby_hostiles_at_start >= MIN_NEARBY_HOSTILES,
                       "the live test retained nearby MAP01 hostiles")
