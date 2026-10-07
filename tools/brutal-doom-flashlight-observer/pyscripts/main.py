"""Read-only assertion that Brutal Doom's flashlight ACS action took effect.

The outer shell test invokes ``ToggleFlashlight`` before simulation begins.
Checking the current Brutal Doom flashlight ownership token means a silent ACS
no-op cannot make the rollback stress test pass merely because the engine
survived an unrelated map load. This fixture never modifies actors, inventory,
or input.
"""

import biaseddoom as bd


CHECK_TIC = 16
checked = False


@bd.on("pre_tick")
def assert_flashlight_is_active(event):
    global checked

    if checked or bd.level_time() < CHECK_TIC:
        return

    player = bd.player(0)
    bd.assert_true(player is not None and player.actor is not None,
                   "flashlight observer requires Player 1")
    if player is None or player.actor is None:
        return

    pawn = player.actor
    flashlight_token_count = int(pawn.inventory_count("BDFlashlightToken"))
    bd.assert_true(flashlight_token_count >= 1,
                   "ToggleFlashlight granted Brutal Doom's flashlight token")
    bd.log("BRUTAL_FLASHLIGHT_OBSERVER active "
           f"token={flashlight_token_count} tic={bd.level_time()}")
    checked = True
