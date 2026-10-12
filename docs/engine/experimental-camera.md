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
  keeps W/A/S/D screen-relative while mouse/stick yaw still turns and aims the
  player. Pitch is locked level for classic horizontal weapon aiming. Choose
  the exact-position **Arrow** or the actor's animated **Player sprite** with
  `cam_topdown_player_marker`. Zoom, colors, and line style stay under the
  standard Automap options, which the presentation honors; normal Automap
  rotation is intentionally ignored so this view stays north-up.

`cam_experimental_mode` selects **Off** (0), **Photo Mode / Free Camera** (1),
or **Top-Down** (2). These commands and the menu options are unavailable in
netgames and during demo playback or recording to keep input deterministic.
