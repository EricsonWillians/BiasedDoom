# Experimental Cameras

BiasedDoom has two local-only presentation cameras. Both are intended for
single-player screenshots and experimentation; they never move an actor,
change a savegame, or participate in demo or network input.

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
  used by the Automap. It works on every renderer, follows the player, and
  keeps W/A/S/D screen-relative while mouse, right-stick, and gyro look still
  turn and aim the player. **Direct mouse / stick aim** is enabled by default:
  standard hitscan and player-missile helpers use the raw local aim ray rather
  than pulling it onto an auto-aim target, so the reticle, shot, mouse, and
  controller look input agree. Turn it off to restore ordinary autoaim.
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
  **impact reticle** is placed at the result of the same auto-aim and line
  trace used for the firing ray, rather than at a guessed screen position. An
  optional aim ray makes that relation visible; both use the Automap crosshair
  color. `cam_topdown_crosshair` and `cam_topdown_aim_guide` control them.
  Use the independent `cam_topdown_player_scale`,
  `cam_topdown_monster_scale`, and `cam_topdown_thing_scale` controls to size
  those three marker groups without altering gameplay actor scale.

Top-Down also has a temporary framing mode: **Top-Down Free Pan** (`I` by
default, or `toggletopdownpan`). While it is enabled, W/A/S/D moves the
north-up map camera in any planar direction instead of moving the player, so
you can choose how much of the level to show. Mouse/stick yaw continues to
aim the player. Toggle it off to recenter and resume following the player;
the ordinary Automap follow preference is never changed. Configure it through
`cam_topdown_freepan` and `cam_topdown_pan_speed`, or in the Experimental
cameras menu. The shortcut can be changed in **Customize Controls → Other
Controls**.

`cam_experimental_mode` selects **Off** (0), **Photo Mode / Free Camera** (1),
or **Top-Down** (2). These commands and the menu options are unavailable in
netgames and during demo playback or recording to keep input deterministic.
