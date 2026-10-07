"""Read-only native companion combat regression.

The map owns the target and the engine owns every combat decision.  Python
only queues the public addcompanion command and observes the real damage
event, proving a companion—not Player 1—acquired and damaged the hostile.
"""

import biaseddoom as bd


TARGET_TID = 32000
BLOCKER_TID = 32001
FINAL_TIC = 260

companion_actor = None
companion_join_tic = None
target_seen = False
companion_damage_tic = None
blocker_damage_tic = None
wrong_source_damage = False
safety_map = False


@bd.on("map_load")
def map_loaded(event):
    global safety_map
    safety_map = bd.current_map().upper() == "BOTSFE"
    bd.assert_true(bd.current_map().upper() in ("BOTFIGHT", "BOTSFE"),
                   "fixture requires a companion combat map")
    bd.log(f"BOT_COMBAT map={bd.current_map()}")


@bd.on("actor_damaged", tid=TARGET_TID)
def target_damaged(event):
    global companion_damage_tic, wrong_source_damage
    source = event["source_ref"]
    if source is not None and companion_actor is not None and source == companion_actor:
        if companion_damage_tic is None:
            companion_damage_tic = bd.level_time()
            bd.log("BOT_COMBAT companion_damage "
                   f"tic={companion_damage_tic} damage={event['damage']}")
    else:
        wrong_source_damage = True
        bd.log(f"BOT_COMBAT unexpected_damage_source source={source}")


@bd.on("actor_damaged", tid=BLOCKER_TID)
def blocker_damaged(event):
    global blocker_damage_tic
    if blocker_damage_tic is None:
        blocker_damage_tic = bd.level_time()
        bd.log("BOT_COMBAT blocker_damage "
               f"tic={blocker_damage_tic} source={event['source_ref']} damage={event['damage']}")


@bd.on("pre_tick")
def observe_native_combat(event):
    global companion_actor, companion_join_tic, target_seen

    now = bd.level_time()
    leader = bd.player(0)
    bd.assert_true(leader is not None and leader.actor is not None, "Player 1 pawn exists")
    if leader is None or leader.actor is None:
        return

    if now == 1:
        bd.execute("addcompanion")

    if not target_seen:
        target = bd.actor_ref(TARGET_TID)
        bd.assert_true(target is not None and target.valid and target.is_monster,
                       "passive hostile target exists")
        if target is not None:
            target_seen = True
            bd.log(f"BOT_COMBAT target_ready health={target.health} position={target.position}")

    if safety_map:
        blocker = bd.actor_ref(BLOCKER_TID)
        bd.assert_true(blocker is not None and blocker.valid and blocker.is_monster,
                       "solid friendly lane blocker exists")

    if companion_actor is None:
        for player in bd.player_refs():
            if player.index != 0 and player.actor is not None:
                companion_actor = player.actor
                companion_join_tic = now
                bd.log("BOT_COMBAT companion_joined "
                       f"tic={now} slot={player.index} position={companion_actor.position}")
                break

    if now == FINAL_TIC:
        bd.log("BOT_COMBAT final "
               f"companion_join_tic={companion_join_tic} "
               f"companion_damage_tic={companion_damage_tic} "
               f"wrong_source_damage={wrong_source_damage}")
        bd.assert_true(target_seen, "target was observed")
        bd.assert_true(companion_actor is not None and companion_join_tic is not None,
                       "native addcompanion created a companion")
        if safety_map:
            bd.assert_true(companion_damage_tic is None,
                           "a solid friendly actor prevents hitscan damage behind it")
            bd.assert_true(blocker_damage_tic is None,
                           "a companion never shoots the friendly lane blocker")
            bd.log("BOT_COMBAT SAFETY PASS")
        else:
            bd.assert_true(not wrong_source_damage,
                           "only the companion may damage the passive target")
            bd.assert_true(companion_damage_tic is not None,
                           "companion acquired and damaged the visible hostile")
            bd.log("BOT_COMBAT PASS")
