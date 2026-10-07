"""Runtime regression for exact profile roster authority.

The driver passes a six-name legacy ``-bots`` list, then this fixture deploys
two ordinary profiles through the public ``addcompanion`` command. That is the
same profile/reconciliation path used after two reviewed random-draft deploys:
the random feature only chooses and saves each profile's identity/style/skill.
The live roster must settle at exactly two companions, never combine them with
the stale startup list.
"""

import biaseddoom as bd


FINAL_TIC = 160
requests_sent = 0
largest_roster = 0
initial_mask = None
initial_count = None


def live_companions():
    return [
        player
        for player in bd.player_refs()
        if player.index != 0 and player.actor is not None
    ]


@bd.on("map_load")
def map_loaded(event):
    bd.assert_true(bd.current_map().upper() == "SQUADAUTH", "fixture requires SQUADAUTH")
    bd.log(f"BOT_PROFILE_AUTHORITY map={bd.current_map()}")


@bd.on("pre_tick")
def deploy_two_profiles_against_a_legacy_startup_list(event):
    global requests_sent, largest_roster, initial_mask, initial_count

    now = bd.level_time()
    if now == 1:
        initial_mask = int(bd.get_cvar("bot_companion_enabled_mask"))
        initial_count = int(bd.get_cvar("bot_companion_count"))
        bd.assert_true(initial_mask in (0, 0b11),
                       "fixture starts either with no profiles or the requested two-profile CVar squad")
        bd.assert_true(initial_count in (0, 2),
                       "fixture starts either with no compatibility count or the requested count of two")

    if initial_mask == 0 and now == 1:
        # Public deployment uses the same profile mask and reconciliation
        # machinery as DeployDraft. Keeping this fixture menu-independent
        # makes it deterministic in headless CI while still executing the
        # engine's real runtime lifecycle. Queue both commands before the
        # first one promotes this local run to cooperative play: Python rightly
        # forbids a second script-side mutation after that transition.
        bd.execute("addcompanion; addcompanion")
        requests_sent += 2

    largest_roster = max(largest_roster, len(live_companions()))

    if now == FINAL_TIC:
        live = live_companions()
        mask = int(bd.get_cvar("bot_companion_enabled_mask"))
        count = int(bd.get_cvar("bot_companion_count"))
        bd.log(
            "BOT_PROFILE_AUTHORITY final "
            f"initial_mask={initial_mask} initial_count={initial_count} "
            f"requests={requests_sent} mask={mask} count={count} "
            f"live={len(live)} max_live={largest_roster} "
            f"slots={[player.index for player in live]}"
        )
        bd.assert_true(initial_mask == 0 or requests_sent == 0,
                       "an already configured two-profile squad is not deployed a second time")
        bd.assert_true(initial_mask == 0 or initial_count == 2,
                       "the clean CVar path preserved its requested profile count")
        bd.assert_true(initial_mask != 0 or requests_sent == 2,
                       "the legacy-startup path deployed exactly two companion profiles")
        bd.assert_true(mask == 0b11 and count == 2,
                       "two profile deployments retain an exact two-member mask/count")
        bd.assert_true(len(live) == 2,
                       "legacy startup bots were not combined with the two-profile squad")
        bd.assert_true(largest_roster == 2,
                       "the live roster never transiently exceeded the configured squad")
        bd.log("BOT_PROFILE_AUTHORITY PASS")
