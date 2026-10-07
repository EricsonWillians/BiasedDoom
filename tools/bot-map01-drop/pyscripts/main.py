"""Headless regression fixture for Doom II MAP01's first companion drop.

The initial co-op starts are on the 56-unit platform.  The open exterior just
north of it is floor 8, so a companion must make a real, safe 48-unit descent
to follow its leader.  The human is repositioned *before* the companion is
added: once the local run becomes cooperative, Python gameplay mutations are
correctly disabled, and the fixture deliberately does not bypass that guard.
"""

import biaseddoom as bd


LEADER_X = -96.0
LEADER_Y = 920.0
LEADER_Z = 8.0
EXPECTED_LOWER_FLOOR = 8.0
EXPECTED_START_FLOOR = 56.0
# Native recovery may attempt a collision-checked catch-up only after this
# interval. A successful crossing before it is therefore a real bot movement
# descent, not a later recovery teleport beside the leader.
NATIVE_DESCENT_DEADLINE = 2 * bd.TICRATE
FINAL_TIC = 280

companion_requested = False
join_reported = False
start_platform_verified = False
initial_companion_actor = None
descended_at_tic = None
descended_position = None


def companion_player():
    """Return the sole fresh-test companion, if it has joined yet."""

    for candidate in bd.player_refs():
        if candidate.index != 0 and candidate.actor is not None:
            return candidate
    return None


@bd.on("map_load")
def map_loaded(event):
    bd.assert_true(bd.current_map().upper() == "MAP01", "fixture requires Doom II MAP01")
    bd.log(f"BOT_MAP01_DROP map={bd.current_map()}")


@bd.on("pre_tick")
def exercise_first_descent(event):
    """Set up the leader, then observe only normal companion movement."""

    global companion_requested, join_reported, start_platform_verified, initial_companion_actor
    global descended_at_tic, descended_position
    now = bd.level_time()

    if now == 1:
        # This is a navigation-focused fixture, not a combat evaluation. Clear
        # stock hostile monsters while this is still a local, writable session
        # so a chance sighting cannot legitimately switch the new companion to
        # combat before it demonstrates the intended follow descent. Nothing
        # mutates after addcompanion promotes the run to co-op.
        removed_hostiles = 0
        for actor in bd.actor_refs():
            if actor.valid and actor.is_monster:
                actor.destroy()
                removed_hostiles += 1
        bd.log(f"BOT_MAP01_DROP hostiles_removed={removed_hostiles}")

        leader = bd.player(0)
        bd.assert_true(leader is not None and leader.actor is not None,
                       "MAP01 leader pawn exists before companion deployment")
        if leader is None or leader.actor is None:
            return

        # This location is in MAP01 sector 0, the safe lower area beyond the
        # initial platform edge.  The checked relocation happens while the run
        # is still local single-player; all subsequent movement is native bot
        # behavior in the normal co-op session.
        moved = leader.actor.set_position(LEADER_X, LEADER_Y, LEADER_Z,
                                          check=True, fog=False)
        bd.assert_true(bool(moved), "leader reached MAP01 lower sector")
        bd.assert_true(abs(leader.actor.floor_z - EXPECTED_LOWER_FLOOR) <= 0.01,
                       "leader is standing on MAP01 floor 8")
        bd.log("BOT_MAP01_DROP leader_below "
               f"position={leader.actor.position} floor={leader.actor.floor_z}")

    if now == 2 and not companion_requested:
        # Queue the public co-op command while the run is still local.  It
        # promotes the session in the same supported way as the menu.  Do not
        # alter player input or actor state after this point.
        bd.execute("addcompanion")
        companion_requested = True

    companion = companion_player()
    # Observe the new pawn at its first visible tic, before it can be credited
    # with the lower exterior. This rejects a bad co-op spawn configuration
    # rather than letting an accidental floor-8 spawn masquerade as a descent.
    if (companion is not None and companion.actor is not None and
            not start_platform_verified):
        pawn = companion.actor
        starts_on_platform = (pawn.alive and
                              abs(pawn.floor_z - EXPECTED_START_FLOOR) <= 0.01)
        bd.log("BOT_MAP01_DROP companion_start "
               f"tic={now} slot={companion.index} position={pawn.position} "
               f"floor={pawn.floor_z} elevated_platform={starts_on_platform}")
        bd.assert_true(starts_on_platform,
                       "fresh companion began on MAP01's elevated floor-56 platform")
        start_platform_verified = starts_on_platform
        initial_companion_actor = pawn

    if now >= 20 and not join_reported:
        bd.assert_true(companion is not None, "fresh companion joined MAP01")
        if companion is not None:
            bd.log("BOT_MAP01_DROP companion_joined "
                   f"slot={companion.index} position={companion.actor.position} "
                   f"floor={companion.actor.floor_z}")
        join_reported = True

    # A companion is free to resume ordinary item/route behavior after it has
    # reached the leader's exterior. Capture the real successful transition
    # instead of requiring it to stand still on floor 8 until FINAL_TIC.
    if (start_platform_verified and companion is not None and companion.actor is not None and
            descended_at_tic is None):
        pawn = companion.actor
        safe_lower_exterior = (pawn.alive and
                               abs(pawn.floor_z - EXPECTED_LOWER_FLOOR) <= 0.01 and
                               pawn.y > 840.0)
        if safe_lower_exterior:
            same_pawn = pawn == initial_companion_actor
            bd.assert_true(same_pawn,
                           "the original companion pawn made the MAP01 descent")
            descended_at_tic = now
            descended_position = pawn.position
            native_before_catchup = descended_at_tic < NATIVE_DESCENT_DEADLINE
            bd.log("BOT_MAP01_DROP descended "
                   f"tic={descended_at_tic} position={descended_position} "
                   f"floor={pawn.floor_z} original_pawn={same_pawn} "
                   f"native_before_catchup={native_before_catchup}")
            bd.assert_true(native_before_catchup,
                           "companion reached the exterior by native movement before recovery")

    if now == FINAL_TIC:
        bd.assert_true(companion is not None and companion.actor is not None,
                       "companion remains live through MAP01 descent")
        if companion is None or companion.actor is None:
            return

        pawn = companion.actor
        bd.log("BOT_MAP01_DROP final "
               f"slot={companion.index} position={pawn.position} floor={pawn.floor_z} "
               f"alive={pawn.alive} start_platform_verified={start_platform_verified} "
               f"descended_at_tic={descended_at_tic}")
        bd.assert_true(pawn.alive, "companion remains alive after MAP01 navigation")
        bd.assert_true(start_platform_verified,
                       "companion started on MAP01's elevated platform before descent")
        bd.assert_true(pawn == initial_companion_actor,
                       "the original companion pawn remains after MAP01 descent")
        bd.assert_true(descended_at_tic is not None,
                       "companion crossed MAP01's safe 48-unit opening to its leader")
