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
frame is presented. It records the active presentation viewport at its native
size, without chroma subsampling or lossy compression. This avoids an extra
render-thread resize when a window is letterboxed or uses high-DPI output.

Two formats are available:

| Format | Best use | Output |
|---|---|---|
| PNG frames (lossless master) | Editing, archival, or a high-quality encode in another tool | `Name_frame000001.png`, `Name_frame000002.png`, …, `Name.ffconcat`, and `Name_audio.wav` |
| RGB AVI (lossless, huge) | A directly playable uncompressed reference file | `Name.avi`, with a 48 kHz stereo PCM game-audio track |

PNG frames are the recommended master format. They retain exact RGB pixels and
are resilient to an interrupted take because already written frames remain
valid. Each normally stopped PNG take also receives an `ffconcat` sidecar next
to the frames. It records the actual duration between captured frames, so a
take that skipped samples under load remains real-time rather than becoming a
fast-forward video. Use that sidecar instead of assigning the selected capture
rate to every PNG in an editor or encoder. For example, this produces a
variable-frame-rate delivery MP4 while preserving the recorder's timestamps:

```text
ffmpeg -f concat -safe 0 -i "Name.ffconcat" -i "Name_audio.wav" -fps_mode vfr -c:v libx264 -crf 18 -pix_fmt yuv420p -c:a aac -b:a 192k "Name.mp4"
```

The example is a delivery encode, not a lossless intermediate: choose an
appropriate lossless codec/container if the encoded result is also meant to be
a master. Keep the `.ffconcat` file with its PNG family when moving a capture;
its entries use frame basenames so the family remains portable as a directory.
On older FFmpeg releases, use `-vsync vfr` in place of `-fps_mode vfr`.

Raw RGB AVI is also lossless but grows very quickly; the engine starts a
numbered sibling part before a file approaches conservative AVI size limits.
AVI normally uses the selected fixed frame rate. If a new GPU readback is not
ready for a scheduled slot, the writer holds the most recently completed frame
for that slot instead of changing the stream rate. This preserves the complete
elapsed take and aligned PCM audio even when capture skips native samples. The
held slots are written only by the background writer from its one completed
image: they do not accumulate GPU readbacks or raw frames on the game thread.
Raw AVI remains exceptionally bandwidth-heavy, so the normal per-take and
free-space budgets still apply. If a real output-space limit is reached, the
recorder finalizes the complete AVI portion it has already written; use PNG or
a lower resolution/rate for especially long captures. The AVI timeline begins
with the first completed composited frame, not with the start command, so
normal cold GPU readback latency creates no leading duplicate images; embedded
audio begins at that same first-frame point.

**PNG compression** is separate from screenshot compression. It defaults to
**Fast lossless** (level 1), which is usually the best capture-throughput
tradeoff. Level 0 minimizes encoder work, level 5 is balanced, and level 9
makes smaller files at a substantially higher CPU cost. All four choices are
lossless.

The selected rate controls a best-effort output cadence from 24 through 240
FPS, independent of the game's speed setting. AVI writing and capture output
coordination run on one bounded background thread. PNG recording can use a
small automatic pool of persistent encoders—up to three, and never more than
the three-frame raw-data limit—when the machine has enough hardware threads.
There is no worker-count option to tune: smaller or unknown machines retain
the single-encoder path, while larger machines gain compression throughput
without creating threads per frame. The coordinator reserves output space,
commits PNG names and the `.ffconcat` sidecar in capture order, and drains the
fixed worker set before finalization.

On normal desktop hardware capture, the render path bulk-copies native RGBA
rows out of the GPU; RGB packing and vertical orientation then happen only on
those bounded background encoders. Only a small, fixed amount of raw frame
data is allowed to wait for them. If rendering, compression, or storage cannot
keep up, the engine skips new samples rather than backfilling a duplicate-frame
backlog that could stall the game or consume memory. The PNG sidecar retains
elapsed capture time for the samples that were saved. AVI holds the latest
completed image for every missed selected-rate slot on its writer thread,
preserving the requested fixed-rate duration without retaining a backlog of raw
capture frames. AVI output remains bounded by the same free-space reserve and
per-take output budget.

Stopping a take returns control immediately while the background writer drains
every frame that has already crossed the bounded hand-off queue and any ready
GPU tail. This preserves the captured tail without making the input or render
thread wait for disk I/O. If the last GPU sample cannot safely be handed to the
bounded writer, finalization gives it a short grace period, then saves all
completed frames and audio rather than leaving the game (or a queued restart)
stuck forever; the final notice explicitly identifies that rare unavailable
last GPU sample. A start or toggle
issued during finalization is queued automatically and arms a fresh take after
the final stopped message; a second toggle cancels the pending restart. For the
cleanest motion, use a frame rate your hardware can sustain, close performance
overlays, and record at the resolution you intend to deliver.

On capable desktop OpenGL drivers, continuous recording uses up to three
pixel-buffer-object readbacks, capped at 128 MiB across both live and
not-yet-retired buffers. The renderer
polls their fences with zero wait time and skips a sample if the oldest
transfer is not ready. If the same oldest transfer remains deferred across
several polls, one recovery probe waits at most one millisecond; this lets
software and remote OpenGL drivers complete a valid PBO transfer without
restoring the old unbounded stall. A full ring is back-pressure, never a
request to drain the GPU: recording does not call `glFinish` merely because
capture work is still in flight. Stop, resize, and immediate restart mark old
requests as discarded and reclaim them only after later zero-wait fence polls,
keeping the same bounded ring without freezing the render thread or allowing
stale frames into a new take. The same discard path is used when the bounded
CPU writer is full: the recorder has already chosen to skip that sample, so it
does not leave an unusable PBO holding presentation behind a GPU fence. The
discarded-only path releases its capture pacing handles immediately and returns
to the renderer's normal synchronization instead of spending the recovery
budget freezing presentation for work that no longer belongs to the take. The
capture command is explicitly flushed before the normal presentation swap so
the DMA transfer can overlap the frame limiter rather than waiting for an
implementation-specific swap flush. A separate
three-frame end-of-frame fence queue bounds ordinary OpenGL command submission
while that legacy finish path is deferred. Its fences are zero-time polls: when
the GPU is behind, capture keeps the bounded queue rather than draining
rendering work. A saturated queue receives only tightly bounded recovery
probes; if it still cannot retire, the recorder detaches, preserves completed
output, and the engine zero-polls only the final capture tail. It never queues
additional frames or waits indefinitely. Discarded PBOs remain behind that
same bounded gate while they retire; if a discard-only GPU tail still cannot
recover within the finite probe budget, the renderer drops its GL handles
(whose deletion is deferred safely by the driver) and resumes normal
presentation without turning that abandoned work into a failure for the next
take. Older OpenGL paths retain the established synchronous screenshot fallback
for correctness. If a context/API transition removes any required asynchronous
readback operation (fence, wait, map, unmap, or deletion), the renderer first
releases its logical ownership of every old PBO slot, then uses that synchronous
fallback; stale active or discarded capture work cannot leak into the next take.

Vulkan uses one reusable, frame-delayed RGBA8 presentation target and a
CPU-visible staging buffer. The staging buffer is capped at 96 MiB; its
matching presentation target can use another 96 MiB at the maximum supported
capture size, so capture-specific Vulkan memory remains bounded at roughly
192 MiB. It renders the final-present shader directly into that target,
avoiding the former capture-only R16F intermediate and full-image blit; it
invalidates non-coherent mappings before reading them. After a stop, restart,
or output-size change it skips one sample until the normal deferred-deletion
boundary has retired the old target, rather than temporarily keeping two large
capture pairs resident. Its map/copy work
and the renderer's normal end-of-frame
synchronization remain part of the Vulkan render path, so it is not a promise
of completely non-blocking capture on every backend. Other backends retain
their established synchronous behavior.

To protect the machine, recording verifies the export volume before writing,
keeps a 4 GiB free-space reserve, never budgets more than half of the space
available when the take begins, and limits one take to 32 GiB. If a limit is
reached, already completed PNG frames are preserved and the current AVI part
is finalized when possible. Buffered flush and close failures also invalidate
and remove the affected PNG or AVI part instead of reporting it as a completed
artifact. Raw RGB AVI is exceptionally bandwidth heavy: at 4K/60 it is roughly
1.4 GiB per second. Use PNG frames or a lower resolution/rate for longer takes.

Video capture includes synchronized 48 kHz stereo PCM game audio. PNG takes
write one `Name_audio.wav` sidecar spanning the full take; each AVI part embeds
an aligned PCM audio stream. The recorder taps the engine's decoded sound and
music/custom-stream data before driver-specific output processing. It includes
game effects and music, but deliberately excludes operating-system audio,
external applications, voice chat, output-device HRTF, and output-side EFX or
reverb. A take with no game sound may therefore be validly silent.

The mix is recorder-side rather than an operating-system loopback. Effects
already playing at the start are sampled from their current voice state;
already queued music begins at the next decoded stream callback. Device-side
filters and later changes to a voice's gain, pan, or pitch are intentionally
approximated rather than copied from the output device.

Before an AVI part or final sidecar is mixed, the recorder waits for stream
callbacks that had already begun decoding for that take. This short
producer-watermark barrier keeps a final music block from being omitted during
a stop or a large-file AVI rollover, while later callbacks cannot hold the
writer open indefinitely.

Audio history is bounded and released after finalization. If an audio source
cannot be retained safely, finalization reports failure rather than silently
writing a shortened sound track. Video capture is unavailable with the
null/headless video driver because that driver has no composited frame to
export.

If the window, output viewport, or output resolution changes during a take,
capture continues in a new numbered part instead of mixing image sizes in one
stream. AVI uses a sibling such as `Name_part002.avi`; PNG uses matching
`Name_part002_frame…png` images and its own `Name_part002.ffconcat` timing
sidecar, while the one `Name_audio.wav` sidecar spans the entire PNG take.
Stop a take before switching renderer or resolution when one contiguous file
or image sequence matters.

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
Fresh installations bind it to **F12**. Existing configurations with the old
stock `spynext` binding are migrated to that direct F12 shortcut. Other F12
choices are preserved: when their double-tap slot is free (including the
common existing F12 screenshot binding), **double-tap F12** toggles video
recording. The Recording Shortcuts menu offers explicit actions to make F12
or double-tap F12 the video shortcut if you prefer a different arrangement.
**Spy next** remains configurable in Other Controls.

The screen confirms the shortcut visually. While a take is active, a red
**REC** indicator shows its wall-clock elapsed time and capture format. When a
stop is requested, it changes to **FINALIZING VIDEO** while the background
writer finishes its remaining work. A green **VIDEO SAVED** or red **VIDEO
FAILED** message then remains briefly on screen; failures include a concise
reason. The console retains the full output path and diagnostics.

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
  master. Keep the generated `.ffconcat` sidecar with the frames and import it
  as variable-frame-rate media; the pixels include the active renderer,
  lighting, fog, and post-processing settings.
- Use 60 FPS for most gameplay footage; 120–240 FPS is available when the
  renderer and storage can sustain it. A skipped-frame notice means the writer
  is saturated; lower the rate, PNG compression level, or output resolution
  instead of waiting for a backlog to recover. Raw RGB output is
  bandwidth-heavy, especially at high resolutions.
- Use a demo for repeatable performance shots or to preserve a run; play it
  back and record the preferred render/preset configuration separately when
  you need both a compact source and polished footage.
- Keep the export folder on a writable, fast local disk. Errors while opening,
  writing, or finalizing a capture stop the affected output rather than
  overwriting an earlier capture.

On normal engine shutdown, an active video capture is stopped and finalized
before graphics resources are released. Always stop a take normally before
terminating the process when possible so the final AVI index is written.
