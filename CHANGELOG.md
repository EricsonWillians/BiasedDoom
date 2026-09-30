# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

## [4.15.17] - 2026-09-30

### Added

- **Rendering Presets** submenu (Postprocess → Rendering Presets): the single
  home for the preset system — graphics/lighting/fog selectors, all three
  searchable browsers, the layer-linking toggle, and a **Reset to Vanilla
  Doom** button (`resetrenderpresets` console command) that restores the
  stock look in one click (Vanilla+ / Classic Balanced / Disabled).
- The graphics preset browser now shows each preset's paired lighting and
  fog presets on every row (e.g. `VHS Horror → Horror Contrast / Cinematic
  Layers`), so the layer relationship is visible while browsing.
- `tools/test-preset-link.sh`: headless test for the preset layer semantics
  (linking, diverge-to-Custom, unlinked independence, vanilla reset).
- 35 append-only, playable rendering looks (IDs 66–100), taking the curated
  collection to **100 graphics**, **75 lighting**, and **53 fog** presets.
  They include Absolution colour variants plus distinct visual genres such as
  Chrome Basilica, Phosphor Terminal, Porcelain Citadel, Lantern Festival,
  Glasshouse Rain, Mercury Mirror, Cinder Opera, Opaline Reef, Ultraviolet
  Archive, Saffron Sandstorm, and Polar Signal Station. The paired lighting
  and fog layers are exposed in the same searchable menu, and the existing
  newer presets now use materially different image pipelines instead of
  feeling like simple palette swaps.

- The Doom 64-inspired "Absolution" preset family (suggested by Brett
  Saltzer), a full three-layer recreation of the notorious lights-off Doom
  64 look: fog preset 18 "Absolution" (deep indigo override fog with murky
  thick-fog depth walls and a near-black gradient floor), lighting preset 40
  "Absolution" (pitch-black ambient floor, strong light diminishing,
  saturated colored light pools, GI ambient deliberately off because its
  sector-bleed feed washes the sky veil out to grey on outdoor levels), and
  graphics preset 65 "Absolution (Doom 64)" (auto-pairs the two and adds
  only soft bloom and a gentle vignette — every tonemap mode, the low
  postfx-quality path, and lowered exposure were measured to lift the dark
  sky veil toward grey, so the darkness lives entirely in the lighting and
  fog layers).
- `CONTRIBUTORS.md`: recognizes community members who shape the project
  through feedback and insights rather than direct commits or pull
  requests; first entry credits Brett Saltzer.
- Gamepad layout presets: Options > Joystick Options > "Gamepad Layout" (or
  the `gamepadlayout` CCMD / `joy_padlayout` cvar) switches between Classic
  (no vertical aiming, the new classic-first default bindings: fire on RT,
  use on LT, run on pad B, center view on R3, right-stick Y unbound),
  Classic + Move (right-stick Y walks, Doom 64 style), Modern (right-stick
  freelook + jump), and Custom. Presets rewrite only the bindings they
  manage and one-shot couple the aiming model: classic presets lock
  freelook off (`freelook false`, `sv_freelook 1`) and enable
  `bd_classic_autoaim`; Modern restores freelook (`freelook true`,
  `sv_freelook 2`) and disables it.
- `bd_classic_autoaim` cvar (default false): when set, weapon
  `+WEAPON.NOAUTOAIM` flags no longer disable autoaim. Mods such as Brutal
  Doom flag their entire arsenal NOAUTOAIM to force manual aiming, which
  silently breaks the classic no-freelook play style; this opt-in restores
  vanilla autoaim under such mods without per-mod code. Set automatically
  by the gamepad layout presets; documented in
  [docs/engine/gamepad.md](docs/engine/gamepad.md).
- Gyro look for controllers that expose a gyroscope through SDL
  (DualSense/DualShock 4 on Linux and macOS SDL builds), off by default,
  with `joy_gyro_look`, `joy_gyro_sensitivity_yaw/_pitch`, and
  `joy_gyro_invert_yaw/_pitch` cvars; pitch follows the usual freelook
  rules so the classic experience is untouched.
- Curated sound-driven weapon haptics for Doom (per-class rumble feels:
  pistol trigger tick, shotgun thump, super-shotgun full-motor boom,
  chaingun chatter, rocket whoosh, plasma crackle, BFG long sweep,
  chainsaw grind), plus damage feedback in `P_DamageMobj` that scales rumble
  intensity and duration with the actual damage taken; the ZScript
  `PlayerHurtMakeRumble` hook remains as an override point.
- DirectInput backend (Windows) detects Sony DualShock 4 and DualSense pads
  by VID/PID and applies a standard gamepad mapping (named axes/buttons,
  trigger axes, d-pad) instead of a raw generic layout.
- `docs/engine/gamepad.md`: full gamepad guide — platform matrix, default
  layout, presets and aiming-model coupling, the NOAUTOAIM-mod section,
  gyro, haptics, and per-device tuning.
- `tools/test-gamepad.sh` and `tools/test-vertical-autoaim.sh`: headless
  engine tests for the gamepad configuration (defaults, presets, coupling,
  haptics content) and for classic vertical autoaim, the latter with a
  `--mod` mode that proves the `bd_classic_autoaim` override under Brutal
  Doom's NOAUTOAIM arsenal (and an `AIM_NO_OVERRIDE=1` sensitivity mode).
- `tools/test-sky-fog.sh`, `tools/test-fog-presets.sh`, and
  `tools/analyze_sky_fog.py`: headless tests asserting sky-fog horizon
  continuity and per-preset sky/geometry seam coherence.

### Changed

- Preset layering is now governed by one explicit switch, **Link Preset
  Layers** (`bd_preset_locked`, default **On**, replacing the old hidden
  explicit-choice heuristic): linked, a graphics preset always drives the
  lighting and fog selectors to its paired values, and manually picking a
  lighting/fog preset drops the graphics selector to Custom; unlinked, the
  three layers are fully independent and graphics presets never touch the
  other selectors. The `bd_autopaired_*` tracking cvars are no longer
  consulted (kept registered for config compatibility).

- The sky fog veil is now the analytic limit of the geometry fog model
  (`FSkyFogParams`, transmittance `T(e)=exp2(σ/(k·ln2·sin e))`), computed
  from `GetFogDensity` for all fog sources, with per-vertex alpha and tint
  on the fog dome mirroring the `getFogColor()` gradient; all 17 fog
  presets were retuned around strength (horizon match) and height falloff
  (zenith clearing), and `bd_fog_sky_horizon` is deprecated (kept
  registered for config compatibility).
- Classic vertical autoaim is vanilla-faithful again: with freelook
  disallowed (or for monster shooters), `P_AimLineAttack` no longer stops
  at a fixed 35° cone — a miss widens the slope search (35° → 50° → 65° →
  80°) the way vanilla Doom keeps reaching higher and lower targets, so
  ledges and flying monsters stay hittable without manual aiming.
- With freelook disallowed, the view pitch is pinned to the horizon every
  tic in `P_PlayerThink` (vanilla Doom has no vertical looking), so mod
  scripts that tilt the view for recoil can no longer leave the camera
  stuck off-level and drag the autoaim cone off-center.
- The Procedural Game main-menu entry is now a localized, title-cased
  string (`$PGMNU_TITLE`) rendered through the exact same text pipeline as
  its sibling entries, so it matches them under custom color palettes and
  languages (suggested by Brett Saltzer).

### Fixed

- Hardened hostile asset handling throughout the engine: malformed ZIP
  SHRINK/IMPLODE streams, HOG archives, KVX voxels, PNG chunks, raw page
  patches, binary `SWITCHES`/`ANIMATED` lumps, and IVF movie frames now fail
  safely instead of risking out-of-bounds access, oversized allocation, or
  unbounded parsing.
- Fixed GLES model normals by reconstructing packed 10:10:10 normals in the
  shader on platforms that cannot consume the desktop packed attribute type.
- Precalculated IQM model-animation frames now serialize and restore instead
  of being discarded from savegames.
- Hardened embedded Python/ImGui allocation and UTF-8 error paths, clear held
  ImGui input when the overlay is disabled, and warn Python mods about
  duplicate actor TIDs.
- `bd_npcs` now persists restored dead NPC state, preventing a savegame or
  hub restore from duplicating a corpse; the framework contract suite covers
  the regression.

## [4.15.16] - 2026-09-28

### Added

- New console options, exposed under System > Console in the options menu:
  `con_font` selects the scrollback font (new console font, classic CONFONT,
  or small font; the input line intentionally keeps the new console font
  because its cell metrics are hardcoded to it), and `con_timestamps`
  prefixes scrollback lines with a local `[HH:MM:SS]` timestamp at insert
  time (console display only; the logfile is unaffected). The menu also
  surfaces the existing `con_scale`, `con_alpha`, and `con_buffersize`
  settings next to an "Open Console" entry.
- `tools/test-console.sh`: headless engine smoke test that asserts the live
  default backquote binding for `toggleconsole`, exercises the new console
  cvars, and pixel-compares baseline/open/closed console screenshots
  captured through a Python driver PK3.
- Game-speed control: the Gameplay options menu now has a "Game speed"
  slider (0.1x-4x) backed by `i_timescale`, which is no longer a virtual
  cvar, so the stored value is reported truthfully by console queries,
  `bd.get_timescale()`, and the FPS counter's scale compensation. The
  setting stays session-only (not archived) and is now also rejected while
  recording or playing a demo (demos record per-tic commands and would
  play back at the wrong pace), alongside the existing netgame guard.

### Changed

- Mouse and joystick sensitivity sliders gained finer granularity and wider
  ranges: mouse sensitivity 0.05-16 in 0.01 steps (was 0.1-8 in 0.05),
  mouse turn/mouselook/forward/strafe speeds and joystick turn/look speeds
  0-4 in 0.05 steps (was 0-2.5 in 0.1), joystick device sensitivity 0-4
  in 0.05 steps (was 0-2 in 0.1), and per-axis scale steps of 0.05.
- The `map_load` Python event now carries `from_hub` (true when the map was
  entered by reopening a hub snapshot) alongside `from_savegame`, so mods and
  frameworks can tell hub restores apart from savegame restores.
- The engine releases the Python GIL only around its idle frame wait, letting
  Python-created background threads run while the engine is idle; every
  `biaseddoom` engine API remains engine-callback-thread-only, with a
  thread-safe queue as the documented handoff pattern.
- `bd.import_script` gives unnamed helpers unique generated module names, and
  an explicit `module_name` can no longer hijack an occupied module,
  `biaseddoom`, shipped `bd_*` frameworks, or guarded standard-library
  modules.
- `bd_npcs.NPCManager.spawn_all(from_savegame, from_hub)` now also adopts
  hub-restored actors by TID; NPCs without a `tid_base` receive a stable
  auto-allocated nonzero TID that persists for save/hub rebinds, and an
  occupied configured TID warns and advances instead of duplicating the
  actor.
- `bd_horror` light programs treat a hub restore like a savegame restore,
  keeping captured original light levels instead of re-sampling them.

### Fixed

- `bd.state` JSON now round-trips `NaN`/`Infinity` via Python's JSON dialect
  instead of failing the whole state save; the deterministic RNG state moved
  to the reserved key `__biaseddoom_rng_state_v1__` (legacy `__rng_state__`
  saves still load), and `py_reload` aborts before interpreter shutdown when
  non-empty state fails serialization, warning that tasks and callbacks do
  not survive reload.
- `bd_dialogue` now ends active sessions on `map_unload` (reason
  `"map_change"`), and `NPCManager.begin_talk` recovers from a stale or
  inactive session instead of soft-locking all conversation.
- Fixed a net-stream desync/crash when a packet carries a legitimate
  zero-payload command (`DEM_PAUSE`, `DEM_SUICIDE`, `DEM_DOAUTOSAVE`, and
  the other no-argument commands): the command skipper now advances zero
  bytes and reports success for them instead of treating them as malformed
  and consuming the rest of the stream, which corrupted every later command
  in the same packet and could crash the packet walk.
- Hardened the legacy model loaders against malformed content: DMD chunk
  walks validate chunk lengths, require a `DMC_INFO` chunk, and check all
  counts, frame sizes, and source offsets before allocating; MD2 validates
  counts, frame size, and offsets and clamps out-of-range vertex-normal
  indices instead of reading past the normal table; MD3 validates the magic,
  frame/surface counts, and per-surface offsets and clamps corrupt vertex
  indices; IQM caps the animation TRS allocation and clamps frame indices;
  OBJ rejects faces with invalid vertex references; UE1 rejects missing
  counterpart lumps, invalid counts, and undersized animation data and
  clamps polygon vertex/surface-skin indices; model-type magic sniffing now
  respects the available buffer length; voxels no longer take element-0
  addresses of empty vertex/index arrays; and MODELDEF frame indices and
  zero-length animation loop spans are rejected instead of dividing by
  zero.
- Hardened the PCX, TGA, IMGZ, and QOI texture decoders against truncated
  or corrupt input: PCX validates the header geometry and bytes-per-scan-
  line minimum, clamps negative source lengths, and stops at the end of the
  source and the end of each scan line in all four readers; TGA and IMGZ
  cap dimensions and reject negative color-map offsets; IMGZ validates the
  uncompressed image size and RLE stream bounds; QOI caps dimensions and
  requires multi-byte opcodes to fit inside the chunk area.
- Custom audio streams are now owned solely by their creators: the OpenAL
  renderer orphans (rather than deletes) them on teardown, so entry points
  no-op safely after renderer destruction and owners keep exclusive delete
  responsibility. Reopening the audio device in place now restarts streams
  that were still playing and re-runs the channel evict/restore cycle, and
  passes the same context attributes (sample rate, source counts, HRTF,
  output limiter) used at context creation instead of resetting them.
  SNDSEQ `volumerand` clamps a zero-width range so it can no longer divide
  by zero at run time, and sound start times are guarded against
  zero-length samples.
- Hardened save, demo, and config parsing: a corrupted save can no longer
  carry a zero ticrate into a division; demo FORM/chunk lengths are
  validated so a mangled demo cannot walk out of or backwards through the
  buffer, and demo player indices are range-checked; compact cvar and user
  info strings must be NUL-terminated inside their buffers and malformed
  data consumes the rest of the stream so walks terminate; `exec` files
  are limited to 32 levels of nesting to stop recursive self-exec stack
  exhaustion; statistics parser strings are copied with guaranteed NUL
  termination; save-slot removal/load/save validate menu-supplied indices;
  IWADINFO `Config` names are truncated to 32 characters before reaching
  fixed-size config section buffers; and a second signal during shutdown
  now uses async-signal-safe output and `_exit` instead of buffered
  `Printf`/`exit`.
- Graphics preset auto-pairing is restored after a restart: when the stored
  lighting/fog selectors exactly match the previous graphics preset's
  pairing entry, the auto-pair trackers are reseeded on the first
  application, while explicit user selections stay protected.
- Keys bound to the screenshot command no longer fire a screenshot and
  swallow the typed character while the console, chat, or a menu text-entry
  field owns the keyboard (SDL, Win32, and Cocoa); the preset search text
  field also guards against an input event arriving without a pending
  editor.
- Strife dialogue `ItemCheckNode` jumps are bounds-checked against the
  loaded dialogue list; the dynamic XLAT parser now uses a heap-grown stack
  with explicit overflow diagnostics instead of a fixed stack; and ACS
  diagnostic messages guard against null activators/players when reporting
  script request failures.
- Hardened malformed-content handling across the engine: VOC audio lumps,
  PK3 archive entries, and network packets validate sizes before reading;
  the save compressor falls back to stored (uncompressed) output when
  compression fails; GL node path and cache lookups are hardened; the
  sector-light bleed rebuild is rate-limited; Vulkan push-constant usage is
  checked against the device limit; dynamic light culling is radius-aware;
  Vulkan screenshots capture custom post-process shaders; bloom radius
  scales as a single value; ImGui atlas rebuilds are bounded; and screenshot
  key binding matching is case-insensitive.
- Hardened the glTF loader against malformed mod content: accessor buffer
  ranges, buffer-view indices, child/joint node indices, interleaved
  strides, cyclic node hierarchies, and animation frame allocations are
  now validated before memory is read or written.
- Fixed `bd_rpg` actor tracking to dedupe handles by actor identity,
  preventing status effects from processing multiple times per actor.
- Fixed `bd_vtm` temporary attribute restores to survive map transitions,
  preventing discipline effects from becoming permanent.
- Fixed `bd_npcs` shops to prove delivery with inventory-count deltas,
  refund failed deliveries, and keep restock timers alive across map
  changes.
- Corrected the installed Linux executable RPATH so `cmake --install` finds
  `libzmusic` in sibling `lib`/`lib64` directories.
- Made the local release AppImage smoke test work in display-less Ubuntu
  22.04 containers.
- Hardened the network driver and lobby against unauthenticated datagrams:
  runt packets (< 5 bytes) can no longer underflow the CRC/uncompress/memcpy
  length arithmetic, unknown senders can no longer index the client table
  with -1, host-supplied client indices and limits are clamped to
  `MAXPLAYERS`, the lobby password compare is length-bounded, and lobby
  stream views are sized by the actual datagram instead of the maximum
  packet size.
- Hardened network command execution against crafted packets: per-player
  numbers, quitter lists, host-handoff targets, controller grants, weapon
  slot numbers, and kick authority are validated before indexing engine
  arrays; commands whose executors conditionally consumed fewer bytes than
  the skipper (`DEM_SINFCHANGED(XOR)`, `DEM_RUNSCRIPT` family with no pawn,
  `DEM_SAVEGAME` outside a level) now always consume their full payload so
  the stream cannot desynchronize; `DEM_STOP`, `DEM_DROPPLAYER`, and
  `DEM_ADVANCEINTER` are inert no-ops instead of fatal unknown commands;
  and the packet walk is bounded by the actual datagram with failures
  reported as missing sequences.
- Hardened savegame deserialization against corrupted saves: player records
  without a pawn are rejected instead of dereferenced, the player count is
  clamped before allocation, ACS world/global array indices are bounds-
  checked (previously an out-of-bounds write), subsector cached geometry
  requires a valid string, base64 memory fields null-check the decoded
  value and no longer overrun the destination, stored zip entries cannot
  claim more data than was allocated, hub snapshot decompression is size-
  capped and checked, unmorphed travellers require a valid alternative,
  `SavegameManager.SetFileInfo` validates its slot index, RNG state indices
  are clamped, save-supplied player class and next-skill indices are
  clamped to valid ranges, short RapidJSON arrays and parse failures are
  rejected, and the `load` command no longer accepts save names starting
  with `..`.
- Hardened the ACS module loader and interpreter against crafted BEHAVIOR:
  chunk extents are validated centrally, branch/jump/call targets and
  script entry addresses are bounds-checked, the instruction pointer is
  validated each fetch, `CASEGOTOSORTED` and `PUSHBYTES` are bounded,
  encrypted/escaped string tables validate every offset before writing,
  map-variable store indices are clamped at all six sites, string-chunk
  lookups validate their tables, and `goto Label+offset` in DECORATE/
  ZScript state blocks must land inside the class's states. The DAP
  debugger no longer aborts the engine on malformed client-supplied
  variable names.
- Hardened resource parsers against malformed lumps: FON1/FON2/BMF font
  loaders bound their RLE and character walks, Build .ART tile ranges and
  pixel data are validated, TEXTUREx patch records must fit in the lump,
  GL subsector seg ranges are extent-checked, SSI/GRP/PAK/RFF/MVL
  container counts are validated before allocation, the ZIP64 extra-field
  walk is bounded, DDS pitch and dimensions are clamped, Hex font glyph
  lookups reject out-of-range codepoints, and Doom patch post data is
  clamped to the lump. Also fixed the Hexen startup NOTCH texture never
  being written, `$musicvolume`'s dB suffix converting a stale value, and
  negative sound-sequence types reading before the translation table.
- Fixed fullscreen blend overlays (damage/item flashes) permanently
  breaking after the first window resize (the reserved vertex quad was
  re-copied from the wrong offset), glTF models with only non-indexed
  primitives never uploading vertex data, MD3/IQM models with more than
  32 surfaces corrupting the texture precache hitlist, Doom-style GLDEFS
  skyboxes with unresolvable faces crashing on first view, `FindFModel`
  indexing `Models[-1]` when a MODELDEF model file is missing, and script-
  set model generator indices reading past the frame table. glTF node
  chains are depth-capped at load and UBO bone truncation now logs a
  one-time warning.
- Python runtime robustness: gameplay event dispatch is depth-capped so a
  damage handler that deals damage can no longer exhaust the native stack;
  world teardown keeps mutations blocked until tasks and handles are fully
  invalidated, and world references can no longer alias the next map;
  actor handle deallocation is re-entrancy safe; `-scripttest` disables the
  wall-clock tick budget so CI autotests are deterministic; `bd.state`
  serialization is capped at 16 MiB and fails the save cleanly beyond it;
  ImGui style setters reject unmapped indices and non-finite floats; and
  actor `tid` assignment rejects values outside 32 bits.
- Python gameplay mutations (`bd.spawn` and friends) are now also rejected
  while a world is being torn down and while the throwaway base map of a
  savegame load is being set up, closing a window where
  `actor_spawned`/`actor_destroyed` handlers or `__del__` finalizers could
  spawn actors into a half-destroyed level; the block lifts before
  `map_load` handlers fire on the new world.
- Hardened DEHACKED patch parsing: `Pointer` headers without a closing
  parenthesis no longer dereference null, `Text` chunk string sizes are
  validated against the patch, sprite-name replacements can no longer
  overflow the 4-character field, and sprite-table indices are range-
  checked instead of growing the table by an unbounded amount.
- ZScript compile-time constant folding no longer invokes undefined
  behavior: add/subtract/multiply/divide/modulo folds are computed in 64
  bits and only folded when the result fits a 32-bit register, so
  `INT_MIN / -1`, `INT_MIN % -1`, and overflowing arithmetic keep their
  runtime expressions instead of crashing or miscompiling the mod.
- The ZScript VM now bounds-checks runtime-indexed constant-pool loads
  (`LK_R` family), virtual-call table indices, and call return counts even
  in release builds, turning latent release-mode out-of-bounds reads into
  clean script aborts; FraggleScript execution has a statement budget so a
  loop without `wait()` can no longer hang the game; and script-encoded
  state-label offsets are validated against the label storage before
  decoding.
- Network robustness: server-variable changes for unknown cvars can no
  longer free an uninitialized pointer, a full input event queue now drops
  new events with a one-time warning instead of silently overwriting
  unconsumed input, and mods generating more network events per tic than a
  packet holds get excess events dropped with a warning instead of an
  engine abort.
- ACS loader: the enhanced-format claimed code size and the chunk
  directory base are validated against the lump before any chunk walk.
- Resource loading: WAD directory lump offsets/sizes are clamped to the
  file, unordered lump positions can no longer underflow the LZSS size
  derivation, truncated zip end-of-central-directory records are rejected,
  and zip local headers are validated before their lengths are trusted.
- Fixed VPX I440 video frames reading past their chroma planes (I440 has
  half the luma's chroma rows).
- Fixed an out-of-range `wipetype` CVAR crashing the engine on the next
  screen wipe (unknown wipe types now fall back to the melt wipe), and
  SBARINFO `drawinventorybar` rejecting negative or absurd slot counts
  instead of attempting multi-gigabyte allocations.
- Hardened the audio paths: crafted VOC lumps can no longer overflow the
  decoded-length accumulator into a small allocation (which was followed
  by gigabyte-sized writes), streamed sound decoding is capped at 256 MiB
  so a hostile stream cannot wrap the output buffer, reverb environment
  names are freed with the matching allocator, savegame-restored sound
  sequence offsets are clamped to the script buffer, and the custom
  rolloff curve can no longer be indexed one past its end.
- Fixed sector light bleeding painting vertical sector-colored bands
  across the sky outdoors: the sky dome and skybox are renderer-generated
  geometry, not sector surfaces, and are now excluded from the bleed
  effect. The bleed map also no longer samples arbitrary sectors outside
  the map bounds (out-of-map texels replicate the nearest real edge
  sector, fixing wrong colors around outdoor map borders), and it now
  rebuilds when a sector's sky texture assignment changes at run time.
- Hardened the post-processing pipeline against hostile console values:
  `gl_dither_bpc` is clamped so it can no longer invoke undefined
  bit-shifts, `gl_exposure_min` is floored above zero so it cannot cause a
  division by zero that propagates NaN through bloom, `gl_exposure_speed`
  is clamped to [0,1], and NaN blur amounts can no longer reach the
  Gaussian sample computation. On GLES, enabling a custom "screen"
  postprocess shader no longer makes the final present pass render into a
  pipeline texture instead of the backbuffer.
- Fixed negative fog densities from scripts or a negative `gl_distfog`
  bypassing the MAPINFO parse clamps and reaching the renderer's fog
  state, shadow-map light rows beyond the configured limit leaking stale
  lights into the shadow shader (the list now covers all 1024 rows and
  unused rows are zeroed), and the Strife binary dialogue reader forcing
  NUL termination on fixed-width text fields that do not guarantee it.
- Hardened localization: short CSV rows and headers without an identifier
  column are rejected instead of being read out of bounds, a trailing
  backslash in a language string can no longer consume the terminator,
  and cyclic `$$` string references now resolve iteratively with a depth
  cap instead of recursing until stack exhaustion.
- Fixed physics NaN sources reachable from specials: zero-height stair
  steps are rejected, waggle floor speed no longer divides by a hostile
  time value or overflows its multiply, pillar speeds no longer divide by
  zero distances, and `A_Face` against an exactly overlapping target no
  longer computes a 0/0 or out-of-domain arcsine pitch.

## [4.15.15] - 2026-09-26

### Fixed

- Restored real dependency bundling for the Linux AppImage: SDL2, X11/Wayland
  platform libraries, VP8/VP9, BZip2, the C++ runtime, OpenAL, GTK, and
  CPython extension dependencies are packaged from Ubuntu 22.04/Jammy instead
  of being silently resolved from the user's host. Release and CI packaging now fail if a non-system dependency
  resolves outside the AppDir, and smoke-test startup in clean Ubuntu 22.04
  and 20.04 containers.
- Fixed the documented local Linux release fallback to require an Ubuntu
  22.04/Jammy host, require embedded Python, and include `libpython`, the
  private standard library, framework packages, and CPython license in the
  portable tarball.
- Removed broken AppImage update metadata whose `.zsync` file was never
  published under the expected release asset name.

## [4.15.14] - 2026-09-23

### Changed

- **Graphics/lighting/fog preset integration**: the three preset families are
  now strictly layered. Graphics presets own only the image pipeline (postfx,
  bloom, CRT/VHS, colorgrade, tonemap, atmosphere, exposure, quality toggles)
  and auto-pair a matching named lighting and fog preset by moving the
  selectors themselves, so menus always show the look that is actually active.
  A lighting or fog preset selected explicitly is never overridden by graphics
  presets again — previously every graphics preset silently stomped the
  lighting/fog CVars while the selectors kept displaying the old names.
- **Graphics preset audit**: all 64 presets now pick a deliberate tonemap
  (ACES, Uncharted2, Lottes Filmic, Reinhard, Palette, or the Gothic / Gothic
  Noir / Silent Hill / Graveyard / Moonlit / Bleach Bypass family) and tune
  exposure adaptation per preset, making the differences between presets clear
  and usable for modern gaming as well as horror/stylized looks.

### Added

- New fog preset **Analog Sepia** and lighting preset **Analog Fluorescent**
  (append-only IDs), preserving the classic Analog Horror look under the new
  layered model.
- Full preset reference in `docs/engine/rendering-presets.md`: every graphics,
  lighting, and fog preset explained, with tonemap/pairing tables and
  hand-combination recipes.

### Fixed

- Screenshots are no longer silently swallowed when a menu is open: keys bound
  to `screenshot` bypass GUI capture on SDL/Win32/Cocoa and pass through the
  menu responder to the binding system.
- Screenshot requests dropped during level transitions now print feedback
  instead of failing silently.
- Vulkan: eliminated the one-frame double present pass (double
  gamma/atmosphere/CRT) that followed every screenshot.
- `screenshot <path>` creates missing destination directories instead of
  failing with "Could not open"; `LevelLocals.MakeScreenShot` is guarded in
  headless mode.

## [4.15.13] - 2026-09-17

### Added

- **Live render-backend switching**: changing `vid_preferbackend`
  (OpenGL/Vulkan/GLES) in Video Options takes effect at runtime —
  backend-owned texture, material, postprocess, 2D-shape, and level-geometry
  resources are rebuilt at the frame boundary without restarting the engine.
- **Sector-edge light bleed**: optional per-pixel smoothing of floor/ceiling
  sector-light transitions (`bd_sectorlight_bleed`, `bd_sectorlight_distance`,
  `bd_sectorlight_strength`), generated as a low-resolution world-space light
  map on OpenGL, Vulkan, and GLES-capable paths, with portal/sky/fog-boundary
  and 3D-floor-control-sector exclusions and live backend-switch re-uploads.
- **Bloom pipeline overhaul**: radius-matched Gaussian weights, mirrored edge
  sampling, energy-preserving mip transfers, and exposed threshold, soft-knee,
  and intensity controls (`bd_bloom_threshold`, `bd_bloom_knee`,
  `bd_bloom_intensity`).
- **Searchable preset browsers**: Graphics, Lighting, and Fog preset menus now
  include full searchable picker submenus with active-preset highlighting.
  Preset libraries expanded to 64 graphics, 38 lighting, and 16 fog presets
  (append-only IDs remain INI-compatible).

- **Python API v2 additions**: `line_activation_failed` now carries
  failure reason codes (`reason`/`reason_code`, with ZScript parity via
  `WorldLineActivationFailed` and `WorldEvent.ActivationFailReason`);
  new events `item_dropped`, `weapon_changed`, `sector_entered`,
  `sector_exited`, `conversation_started`, and `conversation_reply`;
  engine-seeded deterministic `bd.random()`/`randrange()`/`randint()`/
  `choice()` persisted in savegames; query push-down filters for
  `bd.actor_refs` plus `bd.sector_at`/`bd.actors_in_sector`; vectorized
  `bd.actor_field_batch` reads; read-only multiplayer/demo observer mode
  (`bd.session_read_only()`); `bd.assert_true`/`bd.warn` with structured
  `-pyerrorlog` JSON records; and `-scripttest <tics> [ff]`
  fast-forward for CI-style script tests.
- **Dear ImGui overlay**: vendored Dear ImGui 1.92.8
  (`libraries/imgui/`, MIT) with an engine overlay layer
  (`src/common/imgui/`) that renders ImGui draw lists through
  `F2DDrawer` on all render backends, GUI input capture on all
  platforms, the `py_imgui` CVar and `py_imgui_demo` CCMD, and the
  `bd.imgui` Python submodule (~60 functions) driven by the new
  `imgui_frame` event. Gated by the `BIASEDDOOM_ENABLE_IMGUI` CMake
  option.
- **Engine-shipped Python framework packages** under `src/python/lib/`,
  staged beside the embedded stdlib at build time: `bd_quests`
  (data-driven quests/objectives with event auto-wiring, savegame
  persistence, and an ImGui journal UI), `bd_vtm` (VtM-inspired
  chronicle rules: blood pool, hunger/frenzy, humanity, disciplines,
  feeding, masquerade, factions, ImGui vitae HUD), and `bd_dnd`
  (d20/5e-inspired rules: dice with advantage, ability/skill checks, XP
  and leveling, locked-door bashes, trap zones, dialogue skill gates,
  ImGui character sheet).
- Added four new gameplay examples (`26_imgui_overlays` through
  `29_dnd_dungeon`) demonstrating the ImGui overlay and each framework
  package.
- **Exact kill attribution**: `actor_died` now carries
  `attacker_ref`/`attacker_class`/`attacker_player_index` (ZScript sees
  the same via `WorldEvent.DamageSource` on `WorldThingDied`);
  `bd_quests.track_kills` and `bd_dnd.track_xp_from_kills` credit
  player kills exactly by default.
- **`ui_command` event + `pyui` CCMD**: console aliases and key binds
  can drive script UIs; one-line helpers `bd_quests.bind_journal_toggle`,
  `bd_vtm.bind_hud_toggle`, and `bd_dnd.bind_sheet_toggle`.
- **ImGui docking + richer images**: the vendored ImGui now tracks the
  docking branch (`v1.92.8-docking`) with `dock_space_over_viewport()`/
  `dock_space()`/`set_next_window_dock_id()` (multi-viewport
  deliberately disabled), and `bd.imgui.image()` gained UV sub-rects,
  tint, borders, sprite-name fallback, live Actor sprites, and
  `image_size()`.
- **Real dialogue fixture**: new example `30_conversation_quests` — a
  talkative NPC driven by engine-native ZSDF dialogue whose reply
  completes a `bd_quests` objective, exercising
  `conversation_started`/`conversation_reply` under `-scripttest`.
- **`bd_dnd` v1.2**: `DamageSaveRule` (saving throws wired to
  `actor_damaged` with retroactive half/negate refunds), `Party` rosters
  with shared XP + `PartyState` persistence + `PartySheet` UI, and
  `Companion` world-bound follower actors with real combat targeting.
- **Headless video driver**: `-headless` (or `BIASEDDOOM_HEADLESS=1`)
  boots the engine with no display, no GL/Vulkan, and no window
  (`src/common/rendering/nullvideo/`); `-scripttest` passes without an
  X server — CI runners no longer need xvfb.
- **Golden screenshot comparator**: `tools/compare_screenshots.py`
  (stdlib-only) for pixel-tolerant golden-image regression tests in CI.
- **Mutable pre-damage filter**: the `actor_before_damage` event fires
  before armor/damage factors with a mutable event dict — scripts rewrite
  `damage`, retype `damage_type`, or `cancel` the hit outright
  (offline-only, zero cost when unregistered).
- **Per-actor script storage**: `bd.actor_data(ref)` /
  `bd.actor_data_drop(ref)` give every actor a persistent dict, purged
  automatically on destruction or map change via the handle registry's
  invalidation path.
- **`bd_rpg` framework package**: genre-agnostic elemental combat —
  damage-type registry, per-actor/class affinities, `resolve_attack`
  pipeline (hit/crit/dice/affinity/soak), `StatusEngine` timed effects
  (burning/poisoned/slowed/stunned/regenerating), weighted loot tables
  with rarity feedback, kill-XP glue, savegame persistence.
- **`bd_dialogue` framework package**: script-authored branching dialogue
  trees with condition/skill-check/faction-gated choices, native
  player-log integration, and an ImGui presentation layer with live NPC
  portraits and keyboard navigation (`bd.imgui.set_nav_enabled`).
- New examples `31_elemental_combat` and `32_dialogue_trees`
  demonstrating both packs with full `-scripttest` autotests.
- **`bd_horror` framework package**: a shared dark-UX layer —
  `theme.py` (full ImGui horror skin with palette and themed window/
  bar/portrait helpers), `toasts.py` (diegetic notifications), and
  `atmosphere.py` (a `Dread` meter with heartbeat/vignette/whisper
  effects, `LightProgram` flicker/blackout programs via writable
  `Sector.light`, a `StalkerDirector`, and savegame persistence).
- **Showcase re-architecture**: the six RPG examples (27–32) were
  rebuilt on a uniform four-module layout (`main`/`content`/`systems`/
  `ui`) and re-themed as a coherent horror set — *Whispers in the
  Walls*, *The Last Feeding*, *The Sunken Crypt*, *The Confessor*,
  *Pyre & Rime*, *The Interrogation* — with expanded autotest coverage
  that all passes headless.
- **`bd_npcs` `NPCManager.retire(npc_id)`**: takes a world NPC off duty
  (the recruit who joins the party as a follower): the actor leaves the
  world (or is released for adoption), the manager stops tracking,
  prompting, and respawning it, the retired set round-trips through
  savegames, and the disposition standing survives.
- **RPG examples gameplay overhaul**: the RPG set (27, 29, 30, 31, 32,
  and the 33 capstone) gained in-world guidance and real goals: quest
  giver "!" marks, objective beacons and rings (`bd.draw_world_*`
  display list), persistent HUD strips with live key bindings, intro
  onboarding, per-hit elemental feedback and a win condition in
  *Pyre & Rime*, and an *Ashvale Crossing* campaign that now pays XP for
  real Doom kills, gives every class a working active ability on Custom
  Action 3, rebuilds its quest chain (no more hidden shadow quest or
  silent fetch), and recruits Korr as a companion who spawns at his own
  slot and actually fights. `29_dnd_dungeon` was rebuilt as *The
  Delve*, a map-agnostic D&D rules layer in the roguelike example's
  mold: a one-click class founding (Fighter/Rogue/Cleric, each with a
  working class active), a per-map delve contract on any map (a crowned
  Warden unique plus a blood-tribute kill count), visible d20 roll
  popups for bashes/saves/rests, and rests that risk a real nightmare
  spawn in the dark. It now also unifies the health pool (the sheet's
  hp and the pawn's health are one pool kept at the same ratio, armor
  still reducing damage pre-sync), makes the skills Doom-coherent
  (Athletics CQB damage, Perception trap sense and dead-eye, Religion
  light-warded rests with WIS nightmare saves, Insight crosshair
  examine with threat notes), adds the full Delver ImGui window and a
  two-line HUD strip with an examine line, and grows the contract with
  depth scaling, deterministic per-map modifiers, and level-up boons
  (a paused pick-one-of-three chooser).

### Fixed

- Preset selection lifecycle is deterministic and category-independent:
  graphics/lighting/fog selectors no longer dirty each other, startup archive
  replay no longer stomps saved per-feature tweaks, and advanced graphics
  features (SSAO, FXAA, and shadow-map settings) reset to a neutral baseline
  before each graphics preset applies.

- `save_checkpoint`/savegame thumbnails no longer crash in `-headless`
  mode (`PutSavePic` writes the placeholder PNG when no renderer exists).
- Removed the gameplay screen tints from the RPG examples (full-view
  `screen_flash`/`screen_fade` effects fight the player's aim
  mid-combat): level-up and contract fanfares in `29_dnd_dungeon`,
  focus cycling and elite death-rattle fades in `31_elemental_combat`,
  the frenzy flash and storm pulses in `28_vtm_chronicle`, and the
  level-up frame and affix damage feedback in `15_roguelike_run`.
  Feedback stays in the world (rings, titles, sounds) and the HUD.
  `bd_rpg.LootRules` gained `screen_feedback=False` for opting out of
  the rarity flash (31 uses it).
- `bd_dnd` `Companion` followers no longer spawn or teleport into walls
  (spawning and catch-up teleports try a fit-checked candidate ring
  around the player, and `bind(party, anchor=...)` can pin the first
  spawn to a probed slot), and companions now fight proactively:
  they engage monsters that hurt the player, monsters the player hurts,
  monsters that hurt them, and, while combat is recent, the nearest
  visible hostile near the player, instead of only reacting to damage
  that already landed on the player.
- `29_dnd_dungeon`'s autotest no longer leaks its scripted
  `DamageSaveRule` instances into the later rest tests (they are
  disarmed after use); a stray natural 20 could previously refund the
  scripted wound and fail the fitful/sanctuary assertion.
- `29_dnd_dungeon`'s Insight crosshair examine no longer fires a real
  hitscan: the old zero-damage `bd.line_attack` probe still spawned
  BulletPuffs and bullet decals on every wall along its trace every 7
  tics, which read as constant phantom gunfire. Acquisition is now pure
  geometry (a `bd.actor_refs` sphere query, one `bd.actor_field_batch`
  read, a bearing/pitch cone, `check_sight` for walls), firing nothing.
- `29_dnd_dungeon`'s reflex save no longer spams the center-screen
  banner: it rolls on every incoming hit, so `announce_check` gained a
  `center` flag and the save announces with `center=False`, keeping the
  floating d20 readout and the check log.

## [4.15.12] - 2026-09-01

### Added

- Added the Python **display list**, a persistent canvas API for embedded
  scripts: screen-space `draw_text`/`draw_rect`/`draw_line`/
  `draw_texture`/`draw_circle`/`draw_frame` and actor-anchored world-space
  `draw_world_text`/`draw_world_bar`/`draw_world_ring`/
  `draw_world_texture`/`draw_world_line`, with normalized-height text,
  outline/shadow, distance fade, line-of-sight occlusion, health-tracked
  or manual-fraction bars, and id-based redraw/replace plus
  `draw_clear`/`draw_clear_all`. Rendered from the status bar/HUD pass on
  all hardware paths.
- Added true per-actor RGB **tints** to the Python batch API (`tint` op in
  `apply_actor_batch` and the `Actor.tint` property) via dedicated
  `TRANSLATION_PythonText`/`TRANSLATION_PythonActor` translation types —
  script-tinted monsters no longer fight the fixed translation tables.
- Added `item_picked` and `secret_found` Python events: pickups report
  class name, amount, and player exactly once per world pickup (hooked
  from `Inventory.Touch`), and secrets report only after the secret
  actually counts.
- Added thirteen new gameplay examples (`13_bullet_time` through
  `25_pickup_magnet`), including a fully modular endless horde mode
  (`24_wave_defense`): map-native spawn pools with self-pruning closet
  detection, Diablo-2-style Doom-lore affixes with real mechanics
  (volatile, brood, leeching, barbed, hoarding), named uniques, weapon
  ladder and arsenal-aware loot, wave mutators, twin bosses, and
  distance-adaptive overhead health bars with a compass needle.
- Added `tools/play-python-example.py`, an interactive curses launcher for
  the example suite (with a headless test mode), and extended the example
  build/test scripts accordingly.
- The engine boot splash (`BOOTLOGO`) now uses the current BiasedDoom
  banner art.

### Changed

- Examples `01`–`12` were revised against the matured API (guarded
  world-mutation calls, ui-toolkit chrome, display-list visuals), and the
  scripting documentation and `biaseddoom.pyi` type stub cover the new
  surface.

## [4.15.11] - 2026-08-24

### Fixed

- Fixed a crash (SIGSEGV) when loading a map that has no player start:
  `FLevelLocals::FinishTravel` dereferenced `Players[i]->mo`
  unconditionally, but the actor is never spawned without a player start,
  so the engine died on a null pointer dereference during the initial
  level load. Maps without a player start now load with an empty camera
  instead of taking down the process.

### Documentation

- The README now recommends [Heresy Editor](https://github.com/EricsonWillians/heresy-editor)
  as the companion map editor, with a dedicated "Mapping With Heresy
  Editor" section covering the `biaseddoom` port profile, Test in Game,
  the project workflow, and its modern authoring tools; the documentation
  index links it from the front page and the task table.

## [4.15.10] - 2026-08-14

### Added

- glTF PBR materials now render with the hardware PBR shader (GGX/Cook-Torrance specular) instead of plain diffuse: `metallicRoughnessTexture` is split into its metallic (B) and roughness (G) channels with `metallicFactor`/`roughnessFactor` baked in, normal maps and ambient occlusion maps (with `occlusionStrength` baked) are bound as material layers, and emissive maps render as fullbright brightmaps (suppressed entirely when `emissiveFactor` is zero; the factor's magnitude is not baked in). Materials without any PBR content keep the standard Doom shading, and factor-only materials (no textures at all) get solid-value PBR layers, so e.g. a chrome bumper with `metallicFactor 1` / `roughnessFactor 0` picks up dynamic-light reflections. Two caveats, both shared with TEXTURES-defined PBR materials: there is no environment map, so fully metallic surfaces go dark under ambient-only sector light and need dynamic lights to shine; and `normalScale` is not applied. Set `GLTF_NO_PBR=1` in the environment to force standard shading for comparison or troubleshooting.

### Changed

- **Breaking (visual)**: glTF models now face the actor's angle, matching MD3 behavior — the asset's front (glTF +Z) previously rendered 90° counterclockwise off the thing's facing because the loader passed glTF coordinates through raw. The same raw passthrough also rendered every glTF model **mirrored** (decal text read backwards): the conversion now swaps the X/Z axes, which has the same handedness as the MD3 `(x,z,y)` swap, so chirality matches the asset as authored. The conversion is applied at the scene roots, covering static, baked, skinned and animated models uniformly. glTF things placed in existing maps will appear rotated 90° (and un-mirrored); adjust their angles (or add `AngleOffset -90` to their MODELDEF) to restore the old look. Model-facing vertical/horizontal placement can be fine-tuned with the existing MODELDEF `Offset x y z` (Doom axes and units, applied around the thing's position and angle) — e.g. `Offset 0 0 24` lifts a model whose origin sits below its base so it rests on the floor.

### Fixed

- glTF multi-mesh models rendered with scrambled geometry (stretched triangle fans, patchwork panels, out-of-place parts): the vertex buffer stored globally-offset indices while the render state also shifted the vertex attribute pointers by each mesh's vertex offset, so every mesh after the first read vertices belonging to other meshes (or past the buffer). Indices are now mesh-local and the non-indexed path passes a local start, matching the MD3 addressing convention the render state expects.
- glTF nodes authored with a `matrix` property keep their raw matrix instead of being decomposed to TRS and rebuilt (lossy for rotation × non-uniform scale), and the broken translation-only `TRSFromMatrix` stub is gone.
- Fixed a crash at level start (precache) with glTF models having more than 32 meshes: `FGLTFModel::AddSkins` iterated all of the model's meshes over the caller's 32-entry (`MD3_MAX_SURFACES`) surface-skin array, reading out of bounds and writing the precache hitlist at a garbage index. Large scene-scale models (thousands of primitives) now precache correctly.
- glTF loading/rendering quality pass: GLB files with embedded textures (bufferView, in-memory vector, and base64 `data:` URI images) now load their textures instead of falling back to flat colors — images are decoded eagerly into memory-backed textures since the lump-backed image classes cannot re-read embedded data. Normals are now baked with the inverse-transpose of the node matrix, so non-uniform node scale no longer skews shading. The mesh→primitive mapping used by the transform bake is recorded during loading, so a primitive that fails to load no longer misaligns the bake of every mesh after it.
- glTF materials now honor `alphaMode`: `BLEND` meshes (car glass, baked shadow-catcher planes, stickers) render translucent in a second pass after the opaque geometry instead of as solid surfaces, and `MASK` meshes alpha-test at their material `alphaCutoff`. Blended meshes do not write depth, so overlapping translucent surfaces composite instead of clipping each other. `BLEND` meshes are also alpha-tested at their `alphaCutoff` (0.5 when unspecified): without PBR shading, the low-alpha clearcoat/refraction overlay shells common in asset-pack models otherwise fog the whole model — such shells now drop out, while genuinely translucent surfaces can keep their full gradient by setting a low `alphaCutoff` in the asset.
- glTF `baseColorFactor` is now multiplied into the base color texture at render time (via cached tinted texture copies) instead of being ignored whenever a texture was present. Real-world PBR exports are authored this way — e.g. a near-black car paint with factor 0.09 over a gray texture rendered light gray before, and baked shadow planes with a near-zero factor rendered as bright white decals.
- glTF node transforms (TRS or matrix) are now baked into the vertex data of unskinned meshes at load: the renderer uploads raw vertex positions and only applied node transforms through skinning, so real-world assets with transformed nodes (most Sketchfab/Blender exports) rendered at wrong sizes with misplaced parts. Skinned meshes are untouched, and meshes instanced by multiple nodes bake once.
- glTF models now resolve relative texture URIs against the directory of the `.gltf` file itself (per the glTF spec), instead of always assuming a flat `models/` prefix; models in subdirectories like `models/statue/statue.gltf` with a sibling `statue.png` now load their textures (falling back to the old `models/` behavior when the file is not found).
- `A_ChangeModel` no longer rejects an empty modeldef name: the ZScript `''` literal is now treated as "no override", so the actor's own MODELDEF is used — matching what the `GLTFModel` mixin's default `InitGLTFModel(path)` always intended.

### Known issues

- glTF animation playback only applies to skinned meshes (armatures): animation is sampled through skins, so plain node TRS animations on unskinned models currently render in bind pose.
- ZScript mixins do not cross compilation units, so `mixin GLTFModel` only works inside the engine pk3; mod ZScript should call the underlying `Actor` natives (`A_ChangeModel`, `GLTF_PlayAnimation`, `GLTF_UpdateModel`, ...) directly.

## [4.15.9] - 2026-08-09

### Added

- Added a `line_activation_failed` Python event that fires when a line with a nonzero special is activated but the special fails (for example `ACS_Execute` with no backing script), carrying `line_index`, `special`, `args`, `activation_type`, and `actor_ref`. Failed triggers are no longer silent.
- Added a `-scripttest <tics>` command-line mode for CI: the level runs for the given number of tics, then the engine prints `SCRIPT TEST: PASS/FAIL` with the Python error count and exits 0, 1 (errors), or 2 (no level loaded).
- Added a `-pyerrorlog <file>` command-line option appending every reported Python error (including dedup repeat counts) as a JSON line with timestamp, map, context, and traceback, for editors and CI tooling.
- Added a `dumppystub [path]` console command that regenerates the actor-constant block of the `biaseddoom.pyi` type stub from the live class registry (or writes a full skeleton when the file is missing) and warns about any public API missing from the stub.
- Added a static project website under `website/` (standard-library-only generator, restrained retro style): guides, events reference, actor registry, console debugging, VSCode setup, Heresy Editor integration, and the API/roadmap analysis, with the API reference generated from `biaseddoom.pyi` so docs cannot drift. Deploys to GitHub Pages via the `pages.yml` workflow.

- Added a `copyconsole [N]` console command that copies the last `N` console lines, or the entire scrollback when `N` is omitted, to the OS clipboard with color codes stripped, printing a confirmation with the copied line count.
- Added mouse text selection to the console scrollback: drag with the left button to highlight a range (works across wrapped lines), Ctrl+C copies the selection, Ctrl+A selects and copies the entire scrollback with a visible full highlight, and clicking once, pressing Escape, or typing clears the selection. The selection tracks the underlying text while scrolling and is discarded when the console reformats (font or width change, or `clear`).
- Added an actor class registry to the Python API: `bd.actors` now doubles as a discoverable namespace of class constants (`bd.actors.DOOM_IMP` → `"DoomImp"`) covering every non-abstract actor class including mod- and script-defined ones, with `names()`/`constants()`/`dir()` listing, `children_of()` ancestry queries, category helpers (`monsters()`, `projectiles()`, `weapons()`, `items()`, `players()`), `random(kind)` class selection, and `spawn_random(x, y, z, kind=...)` one-call random spawns. `bd.actors(...)` remains callable for live actor queries.
- Added `docs/scripting/biaseddoom.pyi`, a full type stub of the embedded Python module (function signatures, `Actor`/`Line`/`Sector`/`Player` handle members, the `bd.actors` registry, and all built-in actor class constants). Dropping it into a `typings/` folder at the VSCode workspace root enables completions and inline docs via Pylance's default stub path.

### Changed

- In the console, Ctrl+A now selects and copies the entire scrollback (visibly highlighted) instead of moving the cursor to the start of the input line. Ctrl+C copies the highlighted selection when one exists, then the input line when it doesn't.

### Fixed

- Repeated identical Python errors are now deduplicated: a traceback that repeats every tick is printed once and summarized ("repeated N times") instead of flooding the console and logfile. Buffered Python `print()` output is flushed before each traceback so messages and errors appear in the order they happened.

## [4.15.8] - 2026-08-03

### Added

- Added engine-wide support for huge maps with coordinates and sector heights through ±262144 (`MAX_MAP_COORD`), replacing the old ±32768 ceiling of 16.16 fixed point: double-precision node building and traversal, double-precision `node_t` partition lines and bounding boxes, double-precision subsector and blockmap lookups, and UDMF coordinate validation against the new range. UDMF `heightfloor`/`heightceiling` values are accepted through the full range, so floors and ceilings can sit far beyond the classic ±32767 limit; legacy node formats that cannot represent oversized maps are rejected and rebuilt automatically.
- Added `HW_SKY_EXTENT` sky geometry scaled to the full coordinate range: sky walls, portal stencil caps, and horizon portal grids now cover very tall sectors, and the hardware far plane reaches 262144 units.
- Added automatic visibility scaling on very large maps: fog density, global visibility, thick-fog distance, and the sky fog veil scale with the map's bounding-box diagonal instead of crushing distant geometry to black with constants tuned for ~2000-unit maps. Classic-size maps are unaffected.
- Added a physically-based fog shading rewrite: exponential optical-depth transmittance, optional spatial turbulence with bounded floating-point error at extreme coordinates, banding dither, and a smooth minimum-visibility floor without contour discontinuities. The sky dome renders its own atmospheric horizon layer with per-vertex alpha and a true pole so translucent passes never double-blend.
- The sky dome zenith now renders a per-wedge color gradient sampled from the sky texture's own edge bands, replacing the flat average-color cap that produced a featureless disc over tall sky sectors.

### Changed

- Procedural map generation allows sizes up to 160 (from 80) with a widened coordinate safety band, taking advantage of the extended coordinate range.
- The sky is no longer darkened by stale sector light levels in any light mode; sky at infinity always renders at full brightness.
- Fog menu options clarify turbulence-free and exponential-height-falloff modes.

### Fixed

- Fixed uninitialized lightmap coordinates on sky dome vertices, which could sample garbage light data.
- Fixed red/blue channel inversion on the sky zenith cap gradient.
- Fixed a software-renderer visplane hash that overflowed fixed point on plane heights beyond ±32767.

## [4.15.7] - 2026-07-20

### Added

- Added an opt-in embedded CPython API v2 alongside ACS and ZScript, with trusted same-container manifests, synchronous filtered lifecycle/gameplay callbacks, scheduling, live actor/player/sector/line handles, native mutation and attack helpers, ACS execution, typed public ZScript calls, savegame state, whole-tic budgets, multiplayer/demo guards, twelve focused examples, and integration tooling.
- Added deterministic cross-platform IWAD discovery with `-findiwads`, `DOOMWADPATH`, explicit recursive paths, content validation, normalized de-duplication, short-name launch selection, modern/legacy Steam library and app-manifest parsing, external/renamed library support, Flatpak/Snap/macOS/Windows roots, and Linux `~/.steam/debian-installation` coverage.
- Added `biaseddoom-audio-probe`, automatic audio diagnostic logs, `snd_status` decoder reporting, detailed endpoint listings, and root-level troubleshooting documentation.

### Changed

- Official Windows builds now statically bundle pinned OpenAL Soft and libsndfile with OGG, FLAC, Opus, and MPEG support. CI, native Windows, MinGW, and release packaging validate the static dependency closure and reject loose audio/codec DLL regressions.
- Windows and MinGW helpers now package required runtimes, detect stale incompatible MinGW thread-model archives, and run the Windows audio/codec regression probe under Wine when available.
- Native release packages include the embedded Python standard library and license when supported; MinGW explicitly retains Python stubs while preserving ACS and ZScript.
- Python examples use a portable `pyscripts/` resource directory, and their packager derives content roots from each manifest instead of requiring a case-conflicting folder name.

### Fixed

- OpenAL initialization now falls back to silent output with bounded automatic retries when no endpoint is temporarily available. Active disconnects reopen the configured or default device in place so buffers, sources, effects, and music streams survive monitor, GPU, USB, and default-device changes.
- Audio failures now record exact ALC errors, driver overrides, backend/device/extension/source details, and decoder availability in `%LOCALAPPDATA%\biaseddoom\biaseddoom-audio.log` (or a packaged-directory fallback). `-audiodiagnostics` saves the report even when initialization succeeds.
- Mod-provided sounds that fail decoding now name both the logical sound and WAD/PK3 resource path, and an empty-decode path no longer leaks its decoder.
- Steam discovery now follows actual library/app-manifest metadata instead of relying on fragile hard-coded install folders, while rejecting unsafe `installdir` traversal and duplicate candidates.
- Native Windows and macOS vcpkg resolution now pins the helper ports required by CPython 3.12.13 while preserving the established dependency baseline. Bundled-codec discovery prefers vcpkg config packages, avoiding case-insensitive `mpg123` module collisions, and MinGW packaging uses a GCC 13 runner with the C++20 library required by OpenAL Soft 1.25.1. Example resources no longer collide as `PYTHON` and `python` on case-insensitive filesystems.

## [4.15.6] - 2026-07-18

### Added

- Procedural maps now reserve deterministic, theme-aware shallow water, blood, nukage, and lava architecture spanning central, trench, paired, and irregular pools; whole flooded rooms with dry islands; and straight, staggered, or bending multi-cell watercourses. Natural banks, 80-unit dry circulation bands, and 64-unit causeways make liquids part of traversal and combat composition rather than pairs of decorative pits, while nukage and lava retain their classic UDMF damage behavior.
- Procedural levels now select a seed-stable soundtrack from a random map that actually exists in the active IWAD, including the correct reduced roster for Doom shareware and the full Ultimate Doom/Doom II rosters.
- Fixed-seed feature and compatibility matrices now prove every pool/river, reveal-family, reveal-cue, and elevated-position family across themes, including harmless/hazardous liquid mixes and Doom/Ultimate Doom texture availability.

### Changed

- Key traps and switch-opened opportunity spaces now vary among freestanding pavilions, framed wall alcoves, and chambers behind perimeter false walls, with prominent, subtle, and room-matched hidden opening cues selected deterministically from feasible geometry.
- Elevated ranged-monster positions now vary among square stair platforms, chamfered turrets, and wall-backed balconies with straight, offset, or dogleg 16-unit stair approaches.
- Procedural room composition now uses explicit connector, small, medium, and major spatial classes plus compact, axial, and compound footprint families. Seed-stable uneven grid cadence, L/T/cross/stepped growth, nonuniform wall slopes, broad same-room openings, longer foldback loops, and raised cross-room sightlines replace the previous field of similarly sized near-square modules.
- The secret budget now scales more aggressively with size and detail, and every switch-opened opportunity cache is an engine-counted secret while key-triggered ambush chambers remain ordinary encounters.

### Fixed

- Successful ACS termination notices now use the existing developer-level script trace channel instead of normal console notifications. Mods that synchronously query short ACS functions such as `GetCrosshair` no longer flood ordinary gameplay output, while developer-level tracing and all script warnings and errors remain available.
- Fixed repeated `AL_INVALID_ENUM` console errors on OpenAL implementations without `AL_EXT_source_distance_model`. Per-source Doppler is now applied only when the extension exists, which also restores FluidSynth, TiMidity++, OPL, GUS, WildMIDI, ADL, and OPN streaming on affected systems instead of leaving the external Microsoft GS Wavetable synth as the only working MIDI output.
- Procedural soundtrack selection now reapplies the chosen IWAD music after `PROCMAP` generation, fixing the previous lifecycle ordering where the selection was logged after initial music setup but no track became active.
- Fluid planning now uses safe spare cells in composed landmark rooms instead of collapsing ordinary-size maps to small local pools; progression cells remain excluded, and natural banks retain their validated dry clearance around nearby walls and features.
- Framed opportunity-alcove piers are inset from chamfered shells, preventing a rare intersecting loop that made the node builder synthesize a dummy subsector.
- The full cross-platform CI matrix now installs its explicitly selected legacy compiler, bootstraps the repository-pinned vcpkg toolchain, and exercises glTF support, preventing native configurations from failing when `g++-9` or `fastgltf` is absent on a clean runner. The restored submodule metadata also removes checkout warnings, and release-tag pushes no longer duplicate the branch CI matrix.

## [4.15.5] - 2026-07-15

### Added

- Procedural savegames now archive the exact generated UDMF together with seed, theme, difficulty, size, layout, verticality, detail, and outdoor metadata, preserving the original base map across generator revisions.
- Added deterministic Layout Shape, Verticality, Architecture Detail, and Outdoor Spaces menu controls, each backed by an independent generation-effect and runtime/node regression.
- All five procedural themes now own architectural silhouettes, ceiling/elevation behavior, courtyard and interactive-feature cadence, colored lighting, landmark materials, trim, and prop rhythms; Corrupted Tech gains dedicated four-phase hybrid surfaces.
- Procedural routes now use broad multi-level terraces connected by full-width 8-unit stair sectors; structural regression checks require a 96-unit vertical range and size-scaled stair coverage.
- Procedural progression now audits every composed connection by lock stage and rejects any cross-stage opening that is not the single planned keyed gate.
- Structural validation removes keyed door sectors and proves that ordinary doors and open portals cannot reconnect either side of a key gate.
- Added Industrial, Gothic, and Corrupted Tech procedural themes with distinct material transitions and decoration vocabularies.
- Added size-scaled deep-branch survival caches, a guaranteed main-route recovery cadence, and substantially denser role-aware decoration.
- Added size-80 extreme-map generation with guarded UDMF coordinate limits and regression coverage for real runtime loading.
- Added a fixed-seed, all-theme size-80 regression for seed `1771465796`, including serialized passage-clearance and real node-builder checks.
- Added a five-seed maximum-size stress matrix with developer-level BSP diagnostics; renderer-dangerous coincident lines, open/branched sector boundaries, zero-area loops, and synthetic hole subsectors are now regression failures.
- Procedural secrets now use the engine's real `SECRET_MASK`, receive staged backpack, invisibility, berserk, soulsphere, computer-map, light-amplification, invulnerability, and Doom II megasphere rewards, and have dedicated structural/runtime regressions.
- Ordinary doors now select stock 64×72, 128×96, 128×112, and 64/128×128 profiles by theme and IWAD, including Doom II `SPCDOOR` variants.

### Changed

- Screenshot requests are captured after final 2D composition and before presentation, so full-screen automap, HUD, and console layers are included consistently by OpenGL, GLES, and Vulkan.
- Ordinary procedural room floors now follow a `0 → 32 → 64 → 96` terrace rhythm with deterministic branch offsets and a bounded 64-unit inter-room transition, replacing shallow per-room height jitter.
- Key-triggered ambushes and switch-opened opportunity caches now vary silhouette, floor/ceiling treatment, lighting, reveal-door prominence, and actor/reward layout; some cache switches are placed in a nearby room within the same lock stage.
- Procedural surface families now use broader IWAD-safe palettes and progression/role clusters. Continuous chamber walls retain one material, while connector, jamb, platform, and reveal accents change only at visible geometry seams.
- The procedural size slider now reaches 80, optional-branch density is higher, major fights receive more recovery, and ordinary rooms attempt one to three decorations instead of being mostly bare.
- Sizes above 40 now reflow excess horizontal growth into height and center the emitted bounds, retaining extreme capacity without placing starts against the UDMF coordinate edge.
- Industrial and Gothic now use dedicated four-phase wall, floor, and ceiling tables; every theme has a more varied semantic prop rhythm.
- Same-room joins now use explicit 224–256-unit hall portals instead of consuming whole coarse-cell edges, keeping huge four-way junctions topologically well-defined.
- Direct recovery now has a deterministic floor of one substantial pickup per four authored monsters, so high-difficulty huge maps scale their survival economy with actual encounter pressure.
- Door openings now inherit the selected stock texture's native width and height, with explicit lowered approach sectors forming real lintels on both sides instead of fitting one motif to every tall room.
- The macOS deployment target and application metadata now require macOS 10.15, matching the C++17 filesystem support required by the enabled glTF stack.

### Fixed

- Hardware sky fog now uses a non-overlapping, continuously interpolated 32-strip hemisphere, eliminating concentric rings and translucent fan wedges when looking into the sky.
- GLES mapped-buffer subupdates no longer write through a missing CPU shadow allocation, preventing a crash when the new sky-fog gradient is uploaded through the compatibility renderer.
- Saving an in-memory procedural map no longer dereferences the invalid `-1` map-lump container and crashes. Loading stages the archived TEXTMAP before world restoration instead of depending on ambient procedural CVars.
- Automap screenshots no longer capture the hidden 3D view underneath the map overlay.
- Door thresholds that must remain level are normalized before emission; every other non-level room connection receives a traversable staircase instead of an impassable ledge.
- Room composition and UDMF emission can no longer turn a progression-stage boundary into a normal unlocked door or opening.
- Corridor support textures no longer begin in the middle of a flat chamber wall; 8-unit depth returns provide a natural architectural transition.
- Remote opportunity switches are assigned only to rooms proven to contain a full panel wall, preventing large-map generation failures in highly connected one-cell rooms.
- Solid decorations now reserve 112-unit approaches around passages, doors, lifts, and full stair routes, plus a tighter exclusion around shallow landmark tiers, instead of checking actor overlap alone.
- Maximum-width four-way joins no longer emit coincident solid lines or zero-area pinwheel boundaries that produced black floor/ceiling holes after GL-node construction.
- Exit and key chambers with sufficient physical space now always receive their authored landmark platform, including single-cell exits on huge seeds.
- Dramatic terraces on maximum Exploratory graphs now use a graph-distance fallback when cyclic local relaxation cannot converge, guaranteeing every adjacent rise remains within the eight-tread staircase bound.
- Every mission graph now reserves a one-door optional leaf before landmark expansion; loops and room merging cannot consume it, so compact seeds still contain a genuine hidden reward rather than a through-route secret flag.
- Compact maps may place perches and safely bypassable lifts on terrace cells when no level feature cell remains; each feature replaces the cell with its own validated platform or stair geometry instead of aborting generation.
- Mandatory theme landmarks use a collision- and passage-checked wall-bay fallback, keeping Hell finale markers and dense Gothic dressing present without blocking doors, stairs, or gameplay actors.
- Door art no longer extends into adjoining wall shoulders: each moving face matches its native 64- or 128-unit texture width, and every 16-unit slab retains a nonzero recessed approach on both sides.
- Secret supplies avoid landmark and combat-feature footprints, and the expanded reward-slot layout prevents multiple survival pickups from occupying the same coordinate.
- Linux-hosted Windows packaging now prefers the POSIX MinGW-w64 thread model, preventing glTF's simdjson dependency from being compiled without `std::thread`, `std::mutex`, and `std::condition_variable` support.
- Tagged release packaging no longer treats an absent optional runtime file or resource directory as a fatal native Windows or macOS packaging error.
- Release checksums are now generated portably on Linux, Windows, and macOS and record relocatable artifact basenames instead of runner-local absolute paths.
- MinGW cross-builds now target the Vista-or-newer Windows SDK surface across every bundled C and C++ library, exposing the synchronization and common-file-dialog interfaces used by ZMusic and ZWidget on older MinGW-w64 toolchains.
- Native Windows release staging now uses Git Bash's POSIX workspace path for resource discovery and workspace-relative action outputs for artifact upload, reliably including the generated PK3 resources without confusing Windows path translation.

## [4.15.4] - 2026-07-13

### Added

- Procedural map sizes now use a 1–20 slider, extending deterministic generation from compact missions through colossal maps.
- Procedural landmarks can include switch-operated supply reveals, key-triggered ambush closets, raised ranged perches, broad stair tiers, and optional reward lifts with permanent bypasses.
- Player-facing mugshot controls now provide 0.25x–4x scaling, horizontal/vertical positioning, and one-action reset for stock ZScript and legacy SBARINFO status bars.
- Autoaim now has explicit off support and independently tunable horizontal and vertical assistance.

### Changed

- Procedural ordinary rooms begin at broader multi-cell targets, while hubs, arenas, key rooms, and finales grow with map size and combat difficulty.
- Procedural encounters use safer room-aware pressure, stronger major-fight support, and larger finale floor areas at every higher difficulty step.
- Player skin selection now survives gameplay-mod player replacements and remains visible on the actual actor in first-person state changes and third-person views.
- Procedural texture phases are centered per segment so opposite walls, doorway shoulders, chamfers, and accent surfaces align symmetrically.
- Exit landmarks, keyed-door borders, outdoor spaces, and room silhouettes now have clearer visual language and greater variation.

### Fixed

- Raised procedural areas now include traversable stairs or lift/bypass routes instead of leaving required spaces unreachable.
- Cyberdemons require a finale of at least eight merged cells, and Spider Masterminds are no longer selected for generated finales.
- High-resolution mugshots no longer need global texture edits to fit classic status-bar slots.
- Tight procedural rooms no longer receive heavyweight bosses or disproportionate encounter caps.

## [4.15.3] - 2026-07-10

### Added

- Six fog presets with quality, height-falloff, turbulence, and sky-horizon controls.
- Adaptive third-person shot-impact crosshairs with depth cueing, target colors, and viewport clamping.
- A persistent Procedural Game setup and launch menu with deterministic seeds, theme, difficulty, and size controls.
- Mission-graph-first procedural levels with staged keys, lock-safe loops, hubs, arenas, outdoor landmarks, secrets, and guaranteed weapon progression.
- A detailed procedural-generation implementation and evaluation paper under `docs/engine/`.
- **Release Packaging**: Added a tagged GitHub release pipeline that publishes a Linux AppImage, Windows x64 packages, macOS packaging, and SHA256 checksums.
- **Windows Build Helper**: Added `tools/build-windows.ps1` to bootstrap vcpkg, build with Visual Studio 2022, and create a shareable Windows zip.
- **Windows MinGW Cross Build**: Added `tools/build-windows-mingw.sh`, a MinGW-w64 toolchain, CI coverage, and release packaging for a Linux-built Windows x64 `.exe` zip.
- **glTF Support**: Integrated glTF 2.0 model loading through `fastgltf`, including `.gltf`/`.glb`, initial skeletal animation, and material rendering fixes.

### Changed

- Main list menus now scroll with wheel, arrows, Page Up/Down, Home, and End, keeping oversized mod menus fully accessible.
- Procedural rooms now vary their cell composition, footprint, chamfers, floor elevation, clear height, surfaces, accents, lighting, landmarks, and decoration.
- Procedural encounters use gentler per-room caps, later heavy-monster tiers, stronger major-fight ammunition, and more consistent recovery support.
- Procedural menu entries are restored after gameplay mods replace the engine main menu.
- **Project Rename**: Renamed NeoDoom to BiasedDoom, including the `biaseddoom` executable, CMake variables, build scripts, and documentation.
- **SBARINFO Support**: Added custom mugshot scaling and positioning support.

### Fixed

- Tall procedural door textures no longer tile vertically; narrow door motifs are centered instead of asymmetrically cropped.
- Closed map geometry, wall winding, texture alignment, functional keyed doors, IWAD-specific actor compatibility, and deterministic map loading now have expanded regression coverage.
