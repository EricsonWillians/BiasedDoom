# Experimental Cameras

BiasedDoom has two local-only experimental camera modes. Photo Mode is a
presentation camera that never moves an actor. Top-Down is a local
single-player gameplay view: it remaps local input to its visible map cursor
and is unavailable in demos and netgames.

Open **Options → Display Options → Appearance → Experimental cameras** to
select and tune a mode.

- **Photo Mode / Free Camera** (`P` by default, or `togglephotomode`) detaches
  the view from the player. Use the normal movement keys to fly, `PgUp` and
  `Ins` to ascend and descend, and the usual look controls to aim. Player
  input is consumed while the mode is active; the game continues to run. The
  HUD is hidden by default for clean captures and can be restored with
  `cam_photo_showhud`.
- **Top-Down** (`O` by default, or `toggletopdowncamera`) is a north-up,
  full-screen textured Automap presentation. It deliberately uses the
  automap's native coordinate transform for floors, lines, and the player
  marker, so the displayed player position is the same authoritative position
  used by the Automap. It works on every renderer and keeps W/A/S/D
  screen-relative. **Pointer aim (mouse / stick)** is enabled by default:
  mouse, right-stick, gyro, and keyboard-look input move a real cursor across
  the map. The player turns and fires toward that cursor, so the reticle,
  weapon direction, and input stay in agreement. It is a planar mode: aim is
  level with the map rather than using first-person vertical pitch. Turn it
  off to restore ordinary first-person look and autoaim.
  Choose the exact-position **Arrow** or the actor's animated **Player sprite** with
  `cam_topdown_player_marker`. Zoom, colors, and line style stay under the
  standard Automap options, which the presentation honors; normal Automap
  rotation is intentionally ignored so this view stays north-up. Top-Down is
  intentionally more than the normal Automap in full-screen mode: it always
  draws non-hidden monsters and pickups as their live animated, rotated
  sprites (including their translation, alpha, and render style), plus
  actionable special boundaries including doors. This is independent of the
  normal Automap's **Show things as sprites** preference; Top-Down retains
  the user's Automap colors and zoom. Explicit mapper `ML_DONTDRAW` and actor
  `NOTONAUTOMAP` opt-outs retain their normal Automap treatment. Its open-arm
  **pointer reticle** is drawn at the real map cursor, not at a ray impact.
  An optional cursor guide runs from the player to that target. Configure its
  visibility, color, and thickness through `cam_topdown_crosshair`,
  `cam_topdown_crosshair_color`, and `cam_topdown_crosshair_thickness`; use
  `cam_topdown_aim_guide` for the guide. `cam_topdown_stick_aim_speed` and
  `cam_topdown_gyro_aim_speed` tune controller cursor response.
  Use the independent `cam_topdown_player_scale`,
  `cam_topdown_monster_scale`, and `cam_topdown_thing_scale` controls to size
  those three marker groups without altering gameplay actor scale.

Top-Down also has a temporary framing mode: **Top-Down Free Pan** (`I` by
default, or `toggletopdownpan`). While it is enabled, W/A/S/D moves the
north-up map camera in any planar direction instead of moving the player, so
you can choose how much of the level to show. Pointer aim remains active.
Toggle it off to hold the chosen framing; it does not snap back to the player.
Use `resettopdowncamera` (available in **Customize Controls → Other
Controls**) when you want to resume following the player. The ordinary
Automap follow preference is never changed. Configure Free Pan through
`cam_topdown_freepan` and `cam_topdown_pan_speed`, or in the Experimental
cameras menu. The shortcut can be changed in **Customize Controls → Other
Controls**.

`cam_experimental_mode` selects **Off** (0), **Photo Mode / Free Camera** (1),
or **Top-Down** (2). These commands and the menu options are unavailable in
netgames and during demo playback or recording to keep input deterministic.
