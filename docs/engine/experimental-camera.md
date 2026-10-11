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
- **Top-Down (GTA2-style)** (`O` by default, or `toggletopdowncamera`) follows
  the player from a high, downward-looking third-person view. It leaves normal
  player controls active and can be tuned with `cam_topdown_height`,
  `cam_topdown_distance`, and `cam_topdown_pitch`.

`cam_experimental_mode` selects **Off** (0), **Photo Mode / Free Camera** (1),
or **Top-Down** (2). These commands and the menu options are unavailable in
netgames and during demo playback or recording to keep input deterministic.
