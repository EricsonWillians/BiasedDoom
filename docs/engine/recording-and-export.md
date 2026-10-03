# Recording and Export

BiasedDoom has two complementary capture tools: lossless video-frame recording
for finished footage, and native ZDEM demo recording for replayable gameplay
takes. Both use one configurable export folder and can be started from the
menu, the console, or a player-defined shortcut.

## Quick start

Open **Options → System → Recording & Export**.

1. Enter a folder in **Default export folder**, or choose **Paste export
   folder** to use the system clipboard. The engine creates missing folder
   levels when it starts an export.
2. Choose a video base name, lossless format, and frame rate.
3. Start or stop a video capture from the same screen. Choose **Configure
   recording shortcuts** to bind the video and demo toggles without searching
   through the full control list.

Leave the export-folder field blank to use the engine's per-user
`screenshots/captures` folder. **Copy resolved export folder** and **Open export
folder** make it easy to verify the exact destination.

The text fields accept ordinary paths, and the Paste command is useful for
long paths from a file manager. A base name without a directory is written in
the configured export folder. Existing captures are never overwritten: the
engine reserves the complete output family and adds a numeric suffix when
necessary—even if only later PNG frames or a numbered AVI part from an
interrupted older take remain.

## Lossless video capture

Video capture reads the final composited screen after the 3D view, HUD,
automap, console, and post-processing are complete, immediately before the
frame is presented. It records the active output resolution without scaling,
chroma subsampling, or lossy compression.

Two formats are available:

| Format | Best use | Output |
|---|---|---|
| PNG frames (lossless master) | Editing, archival, or a high-quality encode in another tool | `Name_frame000001.png`, `Name_frame000002.png`, … |
| RGB AVI (lossless, huge) | A directly playable uncompressed reference file | `Name.avi` |

PNG frames are the recommended master format. They retain exact RGB pixels and
are resilient to an interrupted take because already written frames remain
valid. Assemble them later at the selected frame rate in the editor or encoder
of your choice. Raw RGB AVI is also lossless but grows very quickly; the engine
starts a numbered sibling part before a file approaches conservative AVI size
limits.

The selected rate controls output timing from 24 through 240 FPS, independent
of the game's speed setting. If rendering cannot keep up, the recorder repeats
the most recent completed frame to retain the intended duration rather than
silently changing the output frame rate. For the cleanest motion, use a frame
rate your hardware can sustain, close performance overlays, and record at the
resolution you intend to deliver.

Video capture is visual-only. It does not record or remux game audio. It is
also unavailable with the null/headless video driver because that driver has no
composited frame to export.

If the window/output resolution changes during a take, capture continues in a
new numbered part instead of mixing image sizes in one stream. Stop a take
before switching renderer or resolution when one contiguous file or image
sequence matters.

### Video commands

```text
startvideorecording [base name]
stopvideorecording
togglevideorecording [base name]
```

Without an optional base name, the command uses **Video base name** from the
Recording & Export menu. A typed extension is replaced with the extension for
the selected format, so format and filename always agree.

The player-configurable **Toggle video recording** control starts and stops
the same lossless capture. It lives both in **Recording & Export → Configure
recording shortcuts** and under **Customize Controls → Other Controls**.

## Demo recording

A demo is not a movie. It is a compact ZDEM stream of the new single-player
run's commands and deterministic game state, intended for later playback by a
compatible engine/game-data setup. It preserves a much smaller, replayable take
than lossless video capture but does not contain final rendered pixels.

In **Recording & Export**, set **Demo base name** and **Demo map**. Leave the
map as `*` to record a fresh run of the map currently being played. Starting a
demo recording queues a fresh single-player game; it is unavailable in a
netgame. The `.lmp` output goes to the same configured export folder as video.

```text
recorddemo [base name] [map]
stopdemorecording
toggledemorecording
```

`recorddemo` uses the configured base name and map when no arguments are
given. Supplying a base name and map is useful in launch scripts. The toggle
control starts a fresh configured take, stops an active take, and cancels a
queued take if pressed again before recording begins. The engine resets game
speed to normal before any demo starts, because ZDEM input takes must play back
at the standard tic rate.

## Quality and workflow guidance

- Use PNG frames at the intended output resolution for a lossless editing
  master. The pixels include the active renderer, lighting, fog, and
  post-processing settings.
- Use 60 FPS for most gameplay footage; 120–240 FPS is available when the
  renderer and storage can sustain it. Raw RGB output is bandwidth-heavy,
  especially at high resolutions.
- Use a demo for repeatable performance shots or to preserve a run; play it
  back and record the preferred render/preset configuration separately when
  you need both a compact source and polished footage.
- Keep the export folder on a writable, fast local disk. Errors while opening,
  writing, or finalizing a capture stop the affected output rather than
  overwriting an earlier capture.

On normal engine shutdown, an active video capture is stopped and finalized
before graphics resources are released. Always stop a take normally before
terminating the process when possible so the final AVI index is written.
