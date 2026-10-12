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
- **Top-Down** (`O` by default, or `toggletopdowncamera`) is a genuine
  orthographic, overhead map view on the hardware renderer. It renders from
  above map geometry and selects floor-facing surfaces, so low ceilings do not
  push the camera down or cover the scene; sky planes are excluded to prevent
  ceiling portals from filling a sector. A small configurable visual tilt keeps
  walls, doors, and switches readable without changing the classic horizontal
  gameplay aim. The camera heading stays stable; W/A/S/D are screen-relative
  while mouse/stick yaw still turns and aims the player. Pitch is locked level
  for classic 1990s horizontal weapon aiming. The centered player arrow and
  GTA-style aim marker share that exact horizontal firing direction. Tune map
  scale, visual tilt, readable wall/switch outlines, marker radius, aim guide,
  and field of view with `cam_topdown_height`, `cam_topdown_pitch`,
  `cam_topdown_wall_overlay`, `cam_topdown_aim_radius`,
  `cam_topdown_aim_guide`, and `cam_topdown_fov`.

`cam_experimental_mode` selects **Off** (0), **Photo Mode / Free Camera** (1),
or **Top-Down** (2). These commands and the menu options are unavailable in
netgames and during demo playback or recording to keep input deterministic.
