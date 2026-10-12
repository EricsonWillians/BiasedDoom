# Experimental Cameras

BiasedDoom has two local-only presentation cameras. Both are intended for
single-player screenshots and experimentation; they never move an actor,
change a savegame, or participate in demo or network input.

Open **Options → Display Options → Appearance → Experimental cameras** to
select and tune a mode. The same page exposes camera-specific field of view,
free-flight speed, and the top-down framing controls.

- **Photo Mode / Free Camera** (`P` by default, or `togglephotomode`) detaches
  the view from the player. Use the normal movement keys to fly, `PgUp` and
  `Ins` to ascend and descend, and the usual look controls to aim. Player
  input is consumed while the mode is active; the game continues to run. The
  HUD is hidden by default for clean captures and can be restored with
  `cam_photo_showhud`.
- **Top-Down** (`O` by default, or `toggletopdowncamera`) follows the player
  from a high, downward-looking third-person view. Its camera heading stays
  stable while the player turns, and a GTA-style aim marker orbits the visible
  player in the exact horizontal direction used by weapons. The engine's trace
  provides target color and depth feedback but cannot move that marker. The
  collision-safe camera boom, height, marker radius, aim guide, and field of
  view can be tuned with `cam_topdown_height`, `cam_topdown_distance`,
  `cam_topdown_aim_radius`, `cam_topdown_aim_guide`, and
  `cam_topdown_fov`.

`cam_experimental_mode` selects **Off** (0), **Photo Mode / Free Camera** (1),
or **Top-Down** (2). These commands and the menu options are unavailable in
netgames and during demo playback or recording to keep input deterministic.
