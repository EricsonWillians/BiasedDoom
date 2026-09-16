"""Player-centered effects: aura rings around the player's feet.

Rings use the same bd.draw_world_ring primitive as the monster auras,
anchored to the player body with a duration so they auto-expire (mostly a
third-person/multiplayer cue, first person barely sees its own floor).
Re-registering the shared canvas id refreshes effects on rapid procs.

Deliberately no screen tints: full-view flashes, fades, and frame
overlays fight the player's aim mid-combat, so all first-person feedback
here stays in the world (rings) or in the HUD (text color).
"""

import biaseddoom as bd

import rogue_config as config
import rogue_runstate as runstate
import rogue_hud as hud


def ring(color, radius, duration, alpha=0.95):
    """Flash a colored ground ring around the player's feet (mostly a
    third-person/multiplayer cue; first person barely sees its own floor)."""
    body = runstate.player_body()
    if body is None:
        return
    hud.safe_draw(bd.draw_world_ring, body, id=config.PLAYER_RING,
                  radius=radius, color=color, alpha=alpha, offset_z=2.0,
                  segments=28, duration=duration)


def level_up_fx():
    """Gold aura ring + pickup chime on level-up (the announce itself is
    handled by the caller)."""
    ring(config.UNIQUE_COLOR, 24.0, 1.4)
    bd.play_ui_sound("misc/pkup", volume=0.8)


def heal_proc_fx():
    """Brief green ring pulse when the vampire mutator feeds a kill to
    you."""
    ring(bd.ui.theme.good, 16.0, 0.5, alpha=0.8)
