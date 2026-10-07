#!/usr/bin/env bash
# Exercise native lossless video and ZDEM recording through a rendered window.
# Headless mode is deliberately unsupported: it has no final framebuffer.
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: ./tools/test-video-recording.sh --iwad PATH [--exe PATH] [--timeout SEC] [--keep-temp]
                                      [--backend opengl|vulkan] [--require-backend]

Runs PNG-sequence, RGB-AVI, and configured-demo recording checks through a
private, non-headless hardware session. The portable default is OpenGL.
`--backend vulkan` adds decoded-pixel validation for PNG and AVI captures;
it prints a skip when a Vulkan device/surface is unavailable. Pair it with
`--require-backend` in a Vulkan-capable CI job to make that absence a failure.
Headless mode is deliberately not supported.
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path=""
test_timeout=75
keep_temp=0
requested_backend="opengl"
require_backend=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iwad) iwad_path="${2:?--iwad requires a path}"; shift 2 ;;
        --exe) engine_exe="${2:?--exe requires a path}"; shift 2 ;;
        --timeout) test_timeout="${2:?--timeout requires seconds}"; shift 2 ;;
        --keep-temp) keep_temp=1; shift ;;
        --backend) requested_backend="${2:?--backend requires opengl or vulkan}"; shift 2 ;;
        --require-backend) require_backend=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown option %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ -n "${iwad_path}" && -f "${iwad_path}" ]] || { printf 'error: a valid --iwad is required\n' >&2; exit 2; }
[[ -x "${engine_exe}" ]] || { printf 'error: executable not found: %s\n' "${engine_exe}" >&2; exit 2; }
runtime_dir="$(dirname "${engine_exe}")"
# The executable can start without its runtime siblings, but it cannot
# identify an IWAD without IWADINFO from game_support.pk3. Fail here rather
# than timing out while the engine waits at the IWAD picker before this test's
# configuration file can execute.
for runtime_package in game_support.pk3 brightmaps.pk3 lights.pk3 game_widescreen_gfx.pk3; do
    [[ -f "${runtime_dir}/${runtime_package}" ]] || {
        printf 'error: engine runtime package not found: %s\n' "${runtime_dir}/${runtime_package}" >&2
        printf 'hint: build the matching *_pk3 targets before running this capture regression\n' >&2
        exit 2
    }
done
[[ "${test_timeout}" =~ ^[1-9][0-9]*$ ]] || { printf 'error: --timeout must be a positive integer\n' >&2; exit 2; }
command -v timeout >/dev/null || { printf 'error: timeout is required\n' >&2; exit 2; }
command -v python3 >/dev/null || { printf 'error: python3 is required\n' >&2; exit 2; }

# The Xvfb/llvmpipe OpenGL combination used by several generic Linux workers
# can present a black drawable even for an unchanged synchronous screenshot.
# Its structural/cadence checks remain valuable, but it cannot distinguish a
# recorder defect from that host presentation limitation. A successfully
# created Vulkan device does have a reliable final-present readback here, so
# make decoded-pixel proof an explicit backend-qualified mode instead of
# turning ordinary non-Vulkan CI red.
backend_preference=0
validate_pixels=0
case "${requested_backend}" in
    opengl)
        ;;
    vulkan)
        backend_preference=1
        validate_pixels=1
        ;;
    *)
        printf 'error: --backend must be opengl or vulkan\n' >&2
        exit 2
        ;;
esac
if [[ "${require_backend}" -eq 1 && "${requested_backend}" != "vulkan" ]]; then
    printf 'error: --require-backend is only meaningful with --backend vulkan\n' >&2
    exit 2
fi

# These rows are the user-facing path to the commands exercised below. Check
# both the source definition and the packaged definition so a stale PK3 cannot
# silently ship an engine whose capture UI is incomplete.
menu_source="${repo_root}/wadsrc/static/menudef.txt"
menu_package="$(dirname "${engine_exe}")/biaseddoom.pk3"
[[ -f "${menu_source}" ]] || { printf 'error: MENUDEF source not found: %s\n' "${menu_source}" >&2; exit 2; }
[[ -f "${menu_package}" ]] || { printf 'error: packaged MENUDEF not found: %s\n' "${menu_package}" >&2; exit 2; }
python3 - "${menu_source}" "${menu_package}" "${repo_root}" <<'EOF'
import os
import re
import sys
import zipfile

source_path, package_path, root = sys.argv[1:]
source = open(source_path, encoding="utf-8").read()
with zipfile.ZipFile(package_path) as package:
    names = [name for name in package.namelist() if name.lower().endswith("menudef.txt")]
    if len(names) != 1:
        raise SystemExit(f"expected exactly one packaged MENUDEF, found {names!r}")
    packaged = package.read(names[0]).decode("utf-8")

required_capture_rows = (
    'TextField "Default export folder", "capture_export_dir"',
    'Command "Paste export folder", "pastecapturepath"',
    'Option "PNG compression", "vid_record_png_level", "VideoCapturePngCompression"',
)
required_controls = (
    'Control "Toggle video recording", "togglevideorecording"',
    'Control "Toggle demo recording", "toggledemorecording"',
	'Command "Make F12 toggle video recording", "bind F12 togglevideorecording"',
	'Command "Make double-tap F12 toggle video recording", "doublebind F12 togglevideorecording"',
)
for label, text in (("source", source), ("package", packaged)):
    capture_start = text.find('OptionMenu "CaptureOptions" protected')
    controls_start = text.find('OptionMenu "CaptureControlsMenu" protected')
    if capture_start < 0 or controls_start < 0:
        raise SystemExit(f"{label} MENUDEF is missing the capture option menus")
    capture_block = text[capture_start:controls_start]
    controls_block = text[controls_start:text.find('//', controls_start)]
    for row in required_capture_rows:
        if row not in capture_block:
            raise SystemExit(f"{label} MENUDEF is missing capture row: {row}")
    if 'OptionValue "VideoCapturePngCompression"' not in text:
        raise SystemExit(f"{label} MENUDEF is missing the recording PNG-compression values")
    for row in required_controls:
        if row not in controls_block:
            raise SystemExit(f"{label} MENUDEF is missing recording control: {row}")

commonbinds = open(os.path.join(root, "wadsrc/static/engine/commonbinds.txt"), encoding="utf-8").read()
if "f12 togglevideorecording" not in commonbinds:
    raise SystemExit("fresh configurations are missing the F12 recording-toggle default")
config_source = open(os.path.join(root, "src/gameconfigfile.cpp"), encoding="utf-8").read()
for fragment in (
    '"RecordingControlMigration"',
    '"F12VideoToggleV2"',
    'stricmp(f12Binding, "spynext") == 0',
    'Bindings.SetBind(KEY_F12, "togglevideorecording")',
	'DoubleBindings.SetBind(KEY_F12, "togglevideorecording")',
	'bool BindingStartsCommand',
):
    if fragment not in config_source:
        raise SystemExit(f"existing-config F12 migration is missing {fragment!r}")

# Keep the versioned migration behavior explicit. Existing bindings replace
# defaults wholesale, so a static fresh-config check alone cannot prove that a
# prior F12 screenshot configuration gains a usable recording shortcut.
def starts_command(binding, command):
    if binding is None or not binding.lower().startswith(command.lower()):
        return False
    if len(binding) == len(command):
        return True
    return binding[len(command)] == ";" or binding[len(command)].isspace()

def migrate_f12(normal, double):
    if starts_command(normal, "togglevideorecording") or starts_command(double, "togglevideorecording"):
        return normal, double
    if normal is None or normal.lower() == "spynext":
        return "togglevideorecording", double
    if double is None:
        return normal, "togglevideorecording"
    return normal, double

if migrate_f12(None, None) != ("togglevideorecording", None):
    raise SystemExit("unbound F12 should receive the direct recording toggle")
if migrate_f12("spynext", None) != ("togglevideorecording", None):
    raise SystemExit("stock F12 spynext should migrate to the direct recording toggle")
if migrate_f12("screenshot", None) != ("screenshot", "togglevideorecording"):
    raise SystemExit("existing F12 screenshot should retain single-tap and gain double-tap recording")
if migrate_f12("custom_action", None) != ("custom_action", "togglevideorecording"):
    raise SystemExit("custom F12 should retain single-tap and gain double-tap recording")
if migrate_f12("custom_action", "custom_double") != ("custom_action", "custom_double"):
    raise SystemExit("fully custom F12 bindings must not be overwritten")
if migrate_f12("togglevideorecording MyTake", "custom_double") != ("togglevideorecording MyTake", "custom_double"):
    raise SystemExit("an existing named video binding must remain untouched")

bind_source = open(os.path.join(root, "src/common/console/c_bind.cpp"), encoding="utf-8").read()
for fragment in (
	"bind[commandLength] == ';'",
	"isspace(static_cast<unsigned char>(bind[commandLength]))",
):
	if fragment not in bind_source:
		raise SystemExit(f"capture command delimiter handling is missing {fragment!r}")

for input_path in (
	"src/common/platform/posix/sdl/i_input.cpp",
	"src/common/platform/win32/i_keyboard.cpp",
):
	input_source = open(os.path.join(root, input_path), encoding="utf-8").read()
	if "menuactive == MENU_WaitKey" not in input_source:
		raise SystemExit(f"capture shortcut cannot be rebound from controls menu: {input_path}")

menu_responder = open(os.path.join(root, "src/common/menu/menu.cpp"), encoding="utf-8").read()
if "Controller buttons are normally translated into menu navigation" not in menu_responder:
	raise SystemExit("controller capture shortcuts are swallowed by menu navigation")

# This check is intentionally source-level in addition to the rendered smoke
# below. Keep the hard safety contract visible: bounded background work,
# free-space protection, and released high-resolution allocations. The rendered
# sustained-AVI run below owns the user-visible duration contract; it must not
# be replaced by a source-level requirement to truncate a slow take.
recording_source = open(os.path.join(root, "src/m_misc.cpp"), encoding="utf-8").read()
recording_header = open(os.path.join(root, "src/m_misc.h"), encoding="utf-8").read()
display_source = open(os.path.join(root, "src/d_main.cpp"), encoding="utf-8").read()
video_header = open(os.path.join(root, "src/common/rendering/v_video.h"), encoding="utf-8").read()
opengl_source = open(os.path.join(root, "src/common/rendering/gl/gl_framebuffer.cpp"), encoding="utf-8").read()
opengl_header = open(os.path.join(root, "src/common/rendering/gl/gl_framebuffer.h"), encoding="utf-8").read()
vulkan_source = open(os.path.join(root, "src/common/rendering/vulkan/system/vk_renderdevice.cpp"), encoding="utf-8").read()
vulkan_header = open(os.path.join(root, "src/common/rendering/vulkan/system/vk_renderdevice.h"), encoding="utf-8").read()
png_header = open(os.path.join(root, "src/common/textures/m_png.h"), encoding="utf-8").read()
png_source = open(os.path.join(root, "src/common/textures/m_png.cpp"), encoding="utf-8").read()
file_writer_header = open(os.path.join(root, "src/common/filesystem/include/fs_files.h"), encoding="utf-8").read()
audio_header_path = os.path.join(root, "src/common/audio/sound/video_capture_audio.h")
audio_source_path = os.path.join(root, "src/common/audio/sound/video_capture_audio.cpp")
if not os.path.isfile(audio_header_path) or not os.path.isfile(audio_source_path):
    raise SystemExit("video recording audio implementation is missing")
audio_header = open(audio_header_path, encoding="utf-8").read()
audio_source = open(audio_source_path, encoding="utf-8").read()
openal_sound_source = open(os.path.join(root, "src/common/audio/sound/oalsound.cpp"), encoding="utf-8").read()
if "FVideoRecordingAudioTap VideoAudioTap" in audio_source:
    raise SystemExit("video audio tap must not use a cross-translation-unit static lifetime")
for fragment in (
    "FVideoRecordingAudioTap &GetVideoRecordingAudioTap()",
    "static FVideoRecordingAudioTap *tap = new FVideoRecordingAudioTap",
    "GetVideoRecordingAudioTap().Start",
    "GetVideoRecordingAudioTap().Discard",
):
    if fragment not in audio_source:
        raise SystemExit(f"video audio shutdown-lifetime contract is missing {fragment!r}")
required_recording_contract = (
    "VIDEO_QUEUE_MAX_FRAMES = 3",
    "VIDEO_QUEUE_MAX_BYTES",
    "AVI_INDEX_MAX_BYTES",
    "VIDEO_DISK_RESERVE_BYTES",
    "VIDEO_SESSION_MAX_BYTES",
    "VIDEO_MAX_FREE_SPACE_FRACTION = 2",
    "VIDEO_FREE_SPACE_CHECK_INTERVAL",
    "readbackRowBytes > VIDEO_MAX_SINGLE_FRAME_BYTES / rows",
    "QueryCaptureFreeSpace",
    "mNextOutputSpaceProbeAtBytes",
    "std::thread mWriterThread",
    "void WriterMain()",
    "void ScheduleNextCapture(uint64_t captureStarted)",
    "JoinWriterIfFinished()",
    "mRestartPending",
    "void Toggle(const char *requestedName)",
    "mBgrFrame.Reset()",
    "mAviIndex.Reset()",
    "FPNGEncoder mPngEncoder",
    "mPngEncoder.Reset()",
    "mOutstandingFrames",
    "CUSTOM_CVAR(Int, vid_record_png_level, 1",
    "mPngCompressionLevel = vid_record_png_level",
    "uint64_t CaptureTimeNS = 0",
    "AppendPngTimelineFrame",
    "FinishPngTimeline",
    "AbortPngTimeline",
    "mPngTimelineFailed",
    "SequenceStemForPart",
    '"ffconcat version 1.0\\n"',
    "mAvihMicrosecondsPerFramePosition",
    "mAvihMaxBytesPerSecondPosition",
    "mStrhScalePosition",
    "mStrhRatePosition",
	"the video writer encountered an unexpected error",
    "screen->GetVideoCaptureBuffer",
    "screen->GetVideoCaptureDimensions",
    "screen->ResetVideoCapture",
    "VideoBytesPerPixel",
    "frame.BottomUp",
    "frame.StorageBytes",
)
for fragment in required_recording_contract:
    if fragment not in recording_source:
        raise SystemExit(f"video recorder safety contract is missing {fragment!r}")
for forbidden in ("VIDEO_CATCHUP_SECONDS", "while (mTotalFrames + 1 < wantedFrames"):
    if forbidden in recording_source:
        raise SystemExit(f"video recorder reintroduced unbounded catch-up: {forbidden!r}")

# Queue admission happens on the render thread. deque growth can fail under
# memory pressure, so accounting must remain unchanged until the frame is
# actually resident in the queue; otherwise one failed allocation can poison
# the bounded-slot counters or escape through the game loop.
queue_start = recording_source.find("\t\tbool QueueFrame(FQueuedVideoFrame &&frame)")
queue_end = recording_source.find("void NoteDroppedFrame()", queue_start)
if queue_start < 0 or queue_end < 0:
    raise SystemExit("could not locate video queue admission")
queue_body = recording_source[queue_start:queue_end]
enqueue = queue_body.find("mQueue.emplace_back(std::move(frame));")
account_bytes = queue_body.find("mQueuedBytes += bytes;")
account_frames = queue_body.find("++mOutstandingFrames;")
if enqueue < 0 or account_bytes < 0 or account_frames < 0:
    raise SystemExit("video queue admission is missing its enqueue/accounting operations")
if "try" not in queue_body[:enqueue] or "catch (...)" not in queue_body[enqueue:account_bytes]:
    raise SystemExit("video queue admission does not contain allocation failure")
if not (enqueue < account_bytes < account_frames):
    raise SystemExit("video queue accounting must follow successful frame admission")

# PNG is the only format allowed to use more than one encoder. Its coordinator
# has a deliberately narrow contract: no more raw frames than the existing
# three-frame admission limit, globally ordered filenames/timeline entries even
# when workers finish out of order, and no orphaned later images after a write
# failure. These source-level checks cover scheduling cases that a fast CI disk
# may not naturally provoke during the rendered smoke run below.
parallel_start = recording_source.find("\t\tEVideoWriterResult WritePngFramesParallel()")
parallel_end = recording_source.find("\n\t\tvoid WriterMain()", parallel_start)
ensure_start = recording_source.find("\t\tbool EnsurePngOutputForFrame(")
ensure_end = recording_source.find("\n\t\tEVideoWriterResult WritePngFramesParallel()", ensure_start)
if parallel_start < 0 or parallel_end < 0 or ensure_start < 0 or ensure_end < 0:
    raise SystemExit("could not locate bounded parallel PNG coordinator")
parallel_body = recording_source[parallel_start:parallel_end]
ensure_body = recording_source[ensure_start:ensure_end]
for fragment in (
    "VIDEO_PNG_MAX_ENCODER_WORKERS = VIDEO_QUEUE_MAX_FRAMES",
    "VideoPngEncoderWorkerCount()",
    "std::thread::hardware_concurrency()",
    "FPngWorkerState",
    "FPNGEncoder encoder;",
    "ReservePngOutputSpace",
    "ReleasePngOutputReservation",
    "mPngReservedOutputBytes",
    "std::array<FPngEncodeTask, VIDEO_PNG_MAX_ENCODER_WORKERS>",
    "std::array<FPngEncodeResult, VIDEO_PNG_MAX_ENCODER_WORKERS>",
    "state.PendingCount",
    "state.CompletedCount",
    "state.Completed[state.CompletedCount++] = std::move(completed);",
    "completed.FrameNumber == nextCommitNumber",
    "task.FrameNumber = nextFrameNumber",
    "state.StopWorkers = true",
    "if (worker.joinable()) worker.join();",
    "AbortFrameAdmissionForWriterFailure",
    "mWriterAborting = true",
    "if (completed.Succeeded) RemoveFile",
    "ReleaseFrameSlot(completed.StorageBytes)",
    "sizeChangeBarrier",
    "state.EncodingFrames == 0 && state.CompletedCount == 0",
    "const uint64_t failureCutoff = failedFrame != 0 ? failedFrame : nextFrameNumber;",
    "takeCompletedAtOrAfter",
    "while (!terminalFailure || nextCommitNumber < firstFailedFrame)",
    "pipelineDrained && !validPrefixCompletion",
):
    if fragment not in recording_source:
        raise SystemExit(f"parallel PNG recorder contract is missing {fragment!r}")
if "vid_record_png_workers" in recording_source:
    raise SystemExit("parallel PNG worker count must remain automatic, not a user CVar")
if "std::deque<FPngEncodeTask>" in recording_source or "std::deque<FPngEncodeResult>" in recording_source:
    raise SystemExit("parallel PNG hand-off slots must not allocate after dispatch")
if "state.Pending.emplace_back" in parallel_body or "state.Completed.emplace_back" in parallel_body:
    raise SystemExit("parallel PNG hand-off slots must remain fixed-capacity")
if "nextFrameNumber" in ensure_body:
    raise SystemExit("PNG part transition must not reset the global task/file ordinal")
if "++mPart;" not in ensure_body or "FinishPngTimeline(changeTimeNS)" not in ensure_body:
    raise SystemExit("PNG resolution transition must finalize the old timing sidecar before a new part")
worker_start = parallel_body.find("auto workerMain")
worker_end = parallel_body.find("\n\t\t\ttry\n\t\t\t{", worker_start)
if worker_start < 0 or worker_end < 0:
    raise SystemExit("could not isolate PNG encoder worker implementation")
worker_body = parallel_body[worker_start:worker_end]
if "AppendPngTimelineFrame" in worker_body or "mEstimatedOutputBytes" in worker_body:
    raise SystemExit("PNG workers must not write ordered timeline or output-budget state")
publish_result = worker_body.find("state.Completed[state.CompletedCount++] = std::move(completed)")
release_encoder = worker_body.find("--state.EncodingFrames")
if publish_result < 0 or release_encoder < 0 or publish_result > release_encoder:
    raise SystemExit("PNG worker must publish completion before releasing its encoder slot")
if "state.CompletedCount < state.Completed.size()" not in worker_body:
    raise SystemExit("PNG worker must wait for a fixed completion slot rather than dropping a result")
if "auto workerMain = [this, &state, &workerWake]() noexcept" not in worker_body:
    raise SystemExit("PNG worker must retain an outer no-escape exception boundary")
for fragment in (
    "const char *StaticError = nullptr;",
    "bool OutputOpened = false;",
    "auto markWorkerFailure",
    "auto abandonPendingTasks",
    "state.WorkerFailed = true;",
    "completed.StaticError = \"could not write a PNG frame\";",
    "const bool removeOutput = task.OutputOpened;",
    "copyWorkerFailureReason",
    "const uint64_t firstAbandonedFrame = abandonPendingTasks();",
    "if (state.WorkerFailed) return true;",
):
    if fragment not in recording_source:
        raise SystemExit(f"PNG worker exception handoff is missing {fragment!r}")
for unsafe_error_assignment in (
    'completed.Error = "could not write a PNG frame";',
    'completed.Error = "PNG recording stopped before encoding a queued frame";',
    'result.Error = "could not write a PNG frame";',
):
    if unsafe_error_assignment in recording_source:
        raise SystemExit(
            "PNG worker error recovery must not allocate a dynamic error string: "
            f"{unsafe_error_assignment!r}"
        )
if "std::unique_ptr<FileWriter> file" not in recording_source or "file.reset();" not in recording_source:
    raise SystemExit("PNG output must close owned files on encoder exceptions")
serial_start = recording_source.find("\t\tEVideoWriterResult WriteQueuedFramesSerial()")
serial_end = recording_source.find("\n\t\tbool EnsurePngOutputForFrame", serial_start)
if serial_start < 0 or serial_end < 0:
    raise SystemExit("could not locate serial video writer containment")
serial_body = recording_source[serial_start:serial_end]
if "try\n\t\t\t\t{\n\t\t\t\t\twritten = WriteFrame(frame);" not in serial_body or "AbortFrameAdmissionForWriterFailure();" not in serial_body:
    raise SystemExit("serial writer must contain frame exceptions and release its bounded queue")

# An unexpected coordinator exception can occur after a result has moved out
# of Completed or after workers have started. It must retain local ownership of
# that result, stop/join every worker before vector destruction, and sweep both
# pending and completed fixed slots exactly once before WriterMain reports a
# normal failure instead of terminating the process.
for fragment in (
    "bool reservationReleased = false;",
    "bool frameReleased = false;",
    "auto releaseTakenCompletion",
    "releaseTakenCompletion(true);",
    "The workers are still joined below before this function can unwind.",
    "auto abandonPendingTasks",
    "while (state.PendingCount != 0)",
    "ReleasePngOutputReservation(abandoned[index].ReservationBytes);",
    "ReleaseFrameSlot(abandoned[index].Frame.StorageBytes);",
    "abandonPendingTasks();",
):
    if fragment not in parallel_body:
        raise SystemExit(f"parallel PNG exception cleanup is missing {fragment!r}")
writer_main_start = recording_source.find("\t\tvoid WriterMain()")
writer_main_end = recording_source.find("\n\t\tvoid ResetAfterStop()", writer_main_start)
if writer_main_start < 0 or writer_main_end < 0:
    raise SystemExit("could not locate writer-thread finalization containment")
writer_main_body = recording_source[writer_main_start:writer_main_end]
for fragment in (
    "catch (...)",
    "AbortFrameAdmissionForWriterFailure();",
    "DiscardAviPart();",
    "DiscardPngTimeline();",
    "PublishWriterResult(result, mWriterFailureReason);",
):
    if fragment not in writer_main_body:
        raise SystemExit(f"writer-thread finalization containment is missing {fragment!r}")
if writer_main_body.count("if (mFormat == VIDEO_RECORDING_RGB_AVI && mFile != nullptr && result != VIDEO_WRITER_FAILURE)") != 1:
    raise SystemExit("writer-thread AVI finalization must have one unambiguous branch")

# A normal stop may observe the worker after it published a success but before
# the coordinator commits it. It must not delete that tail image. A failure
# keeps the lower ordinal prefix, while results at/after the cutoff are
# released/deleted promptly so they cannot spin the coordinator or leak an
# admission slot.
normal_stop = parallel_body.find("!terminalFailure && stopRequested && !queuedFrames && pipelineDrained && !validPrefixCompletion")
publish_before_stop = parallel_body.find("state.Completed[state.CompletedCount++] = std::move(completed)")
terminal_cleanup = parallel_body.find("while (takeCompletedAtOrAfter(firstFailedFrame, discarded))")
terminal_wait = parallel_body.find("completed.FrameNumber < firstFailedFrame && completed.FrameNumber == nextCommitNumber")
if normal_stop < 0 or publish_before_stop < 0 or terminal_cleanup < 0 or terminal_wait < 0:
    raise SystemExit("parallel PNG stop/failure drain contract is incomplete")
if "terminalFailure && pipelineDrained && !uncommittedCompletion" in parallel_body:
    raise SystemExit("parallel PNG terminal drain still treats failed/later completions as ordered work")

# Audio is captured from engine PCM, not an operating-system loopback device.
# Keep that contract explicit: PNG writes a real WAV sidecar, AVI carries a
# native PCM stream, and the OpenAL renderer timestamps both effects and
# streams while the recording epoch is active.
for fragment in (
    "I_StartVideoRecordingAudio",
    "I_StopVideoRecordingAudio",
    "I_DiscardVideoRecordingAudio",
    "I_WriteVideoRecordingAudioWav",
    "I_RenderVideoRecordingAudio",
    "FinishPngAudio",
    "WriteAviAudioForPart",
    'WriteFourCC("auds")',
    'WriteFourCC("01wb")',
    "mAviPartAudioFrames",
):
    if fragment not in recording_source:
        raise SystemExit(f"video recorder audio integration is missing {fragment!r}")
for fragment in (
    "VIDEO_AUDIO_RATE = 48000",
    "VIDEO_AUDIO_CHANNELS = 2",
    "VIDEO_AUDIO_MAX_STREAM_BYTES",
    "I_RecordVideoRecordingAudioEffectStart",
    "I_RecordVideoRecordingAudioStream",
):
    if fragment not in audio_header and fragment not in audio_source:
        raise SystemExit(f"video recording audio tap is missing {fragment!r}")
# Stream conversion intentionally happens outside the mixer mutex. A per-take
# reservation watermark makes callbacks already in flight at a stop or AVI
# rollover visible to that final snapshot without allowing stale callbacks into
# a later take.
for fragment in (
    "StreamReservationNext",
    "StreamPublicationsInFlight",
    "ReserveStreamPublicationLocked",
    "ResolveStreamPublicationLocked",
    "StreamPublicationWake.wait",
    "Active may now be false because Stop() occurred",
):
    if fragment not in audio_source:
        raise SystemExit(f"video recording stream-finalization barrier is missing {fragment!r}")
for fragment in (
    "I_RecordVideoRecordingAudioEffectStart",
    "I_RecordVideoRecordingAudioStream",
    "VideoRecordingAudioCaptureWasActive",
    "TrimVideoRecordingAudioCache(VIDEO_RECORDING_AUDIO_IDLE_CACHE_BYTES)",
):
    if fragment not in openal_sound_source:
        raise SystemExit(f"OpenAL video audio integration is missing {fragment!r}")

# A normal stop is intentionally asynchronous, but it must drain every frame
# the render thread has already handed to the bounded writer queue. It must not
# join a slow disk writer from the input/render path. Engine teardown is the
# one place where joining is required to avoid leaving a process-owned thread
# behind.
stop_start = recording_source.find("\t\tvoid Stop()")
finish_start = recording_source.find("\t\tvoid Finish()", stop_start)
is_active_start = recording_source.find("\t\tbool IsActive()", finish_start)
if stop_start < 0 or finish_start < 0 or is_active_start < 0:
    raise SystemExit("could not locate video stop/finalize implementation")
stop_body = recording_source[stop_start:finish_start]
finish_body = recording_source[finish_start:is_active_start]
if "RequestWriterStopAfterCaptureDrain()" not in stop_body or "mStopCaptureDrainPending" not in stop_body:
    raise SystemExit("interactive video stop must enter the asynchronous capture-drain phase")
if "RequestWriterStop(" in stop_body:
    raise SystemExit("interactive video stop must delegate writer shutdown until final GPU work is drained")
if "CompleteInteractiveStop()" not in stop_body or "JoinWriter();" in stop_body:
    raise SystemExit("interactive video stop must not block on the writer thread")
if "DrainStoppingReadback()" not in finish_body or "RequestWriterStopAfterCaptureDrain()" not in finish_body or "JoinWriter();" not in finish_body:
    raise SystemExit("engine teardown must drain and finalize the bounded writer queue")
if "CompleteStopAndRestartIfRequested()" not in recording_source:
    raise SystemExit("video stop completion can lose a queued restart")
if "VIDEO_STOP_READBACK_DRAIN_NS" in recording_source:
    raise SystemExit("video stop drain reintroduced its obsolete tail-timeout contract")

# Native hardware RGBA data should move straight into the bounded queue; the
# PNG/AVI writer owns its later channel packing and orientation conversion.
capture_start = recording_source.find("\t\tvoid CaptureFrame()")
capture_end = recording_source.find("\n\tprivate:", capture_start)
if capture_start < 0 or capture_end < 0:
    raise SystemExit("could not locate video capture implementation")
capture_body = recording_source[capture_start:capture_end]
queue_start = recording_source.find("\t\tEQueuedCaptureResult QueueCapturedFrame", capture_end)
queue_end = recording_source.find("\n\t\tvoid CompleteInteractiveStop()", queue_start)
if queue_start < 0 or queue_end < 0:
    raise SystemExit("could not locate video queue hand-off implementation")
queue_body = recording_source[queue_start:queue_end]
for fragment in ("frame.Pixels = std::move(screenshot);", "frame.BottomUp = bottomUp;"):
    if fragment not in queue_body:
        raise SystemExit(f"native capture frame no longer reaches the writer unchanged: {fragment!r}")
for fragment in ("int compressionLevel = -1", "bool useConfiguredGamma = false"):
    if fragment not in png_header:
        raise SystemExit(f"background PNG encoder cannot snapshot {fragment!r}")
if "SS_RGBA" not in png_header or "case SS_RGBA:" not in png_source:
    raise SystemExit("background PNG encoder cannot accept native RGBA recording rows")
for fragment in (
    "class FPNGEncoder",
    "void Reset();",
    "M_CreatePNGWithEncoder",
):
    if fragment not in png_header:
        raise SystemExit(f"continuous PNG encoder API is missing {fragment!r}")
for fragment in (
    "FPNGEncoder::SaveBitmap",
    "FPNGEncoder::Reset()",
    "deflateReset(&state.Stream)",
):
    if fragment not in png_source:
        raise SystemExit(f"continuous PNG encoder implementation is missing {fragment!r}")
if "M_CreatePNGWithEncoder(file" not in recording_source or "&mPngEncoder" not in recording_source:
    raise SystemExit("video writer does not use its persistent PNG encoder")
# The writer can discover a full/failing filesystem only while flushing IDAT
# data. Each early failure must leave zlib in a known reusable state, or end it
# before returning. Keeping a successfully reset stream is valid (and avoids a
# per-frame allocator churn), so do not require an unconditional deflateEnd.
png_write_failures = list(re.finditer(
    r"if\s*\(\s*!WriteIDAT\s*\(file,\s*buffer\.data\(\),\s*sz\)\s*\)",
    png_source,
))
if len(png_write_failures) != 2:
    raise SystemExit("PNG encoder must handle both IDAT write-failure paths")
for failure in png_write_failures:
    recovery_end = png_source.find("return false;", failure.start())
    if recovery_end < 0:
        raise SystemExit("PNG IDAT write failure does not return to the recorder")
    recovery = png_source[failure.start():recovery_end]
    for fragment in ("deflateReset(&stream)", "deflateEnd(&stream)", "state.Initialized = false"):
        if fragment not in recovery:
            raise SystemExit(f"PNG IDAT write-failure recovery is missing {fragment!r}")
if "bool CloseChecked()" not in file_writer_header or "file->CloseChecked()" not in recording_source:
    raise SystemExit("video recorder cannot detect buffered close failures")
for fragment in (
    "GetVideoCaptureDimensions",
    "GetVideoCaptureBuffer",
    "HasPendingVideoCapture",
    "ResetVideoCapture",
    "AbandonPendingVideoCaptureReadbacks",
    "CanRenderNextFrame",
    "ConsumeVideoCaptureBackpressureFailure",
    "bool issueNext = true",
):
    if fragment not in video_header:
        raise SystemExit(f"video capture backend API is missing {fragment!r}")

drain_start = recording_source.find("\t\tvoid DrainStoppingReadback()")
drain_end = recording_source.find("void PublishRecordingStatus", drain_start)
if drain_start < 0 or drain_end < 0:
    raise SystemExit("could not locate asynchronous stop-drain implementation")
drain_body = recording_source[drain_start:drain_end]
for fragment in (
    "HasPendingVideoCapture()",
    "bottomUp, false",
    "CanQueueFrame(maximumReadbackBytes)",
    "VIDEO_STOP_READBACK_DRAIN_MAX_NS",
    "mStopCaptureDrainStartedNS",
    "mStopReadbackAbandoned = true",
    "preserving all completed frames",
):
    if fragment not in drain_body:
        raise SystemExit(f"video stop drain is missing its nonblocking tail contract: {fragment!r}")
for forbidden in ("GetScreenshotBuffer", "JoinWriter();", "VIDEO_STOP_READBACK_DRAIN_NS"):
    if forbidden in drain_body:
        raise SystemExit(f"video stop drain reintroduced a blocking or timeout path: {forbidden!r}")

# Desktop OpenGL uses a bounded PBO ring plus a separately bounded frame-fence
# queue. A saturated fence queue admits no new rendering work until a finite
# recovery poll retires an old fence; a persistent failure detaches the take
# rather than allowing the driver command backlog to grow. Neither normal
# capture nor stop/reset may turn that protection into an unbounded glFinish.
required_opengl_contract = (
    "VideoReadbackSlotCount = 3",
	"VideoFramePacingFenceCount = 3",
    "GL_PIXEL_PACK_BUFFER",
    "MaxVideoReadbackBytes = 128ull * 1024ull * 1024ull",
    "glClientWaitSync(oldest->Fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0)",
	"VideoReadbackRecoveryWaitNS = 1000000ull",
	"VideoReadbackRecoveryPollCount = 3",
	"VideoFramePacingRecoveryWaitNS = 1000000ull",
	"VideoFramePacingMaxSaturationPolls = 120",
    "GL_RGBA, GL_UNSIGNED_BYTE",
    "width = slot.SourceWidth;",
    "height = slot.SourceHeight;",
    "bottomUp = true;",
    "candidate.AllocatedBytes != sourceBytes",
    "resetStorage",
    "uint64_t residentBytes = 0;",
    "candidate.AllocatedBytes > MaxVideoReadbackBytes - residentBytes",
    "sourceBytes > MaxVideoReadbackBytes - residentBytes",
    "HasPendingVideoReadback()",
    "HasPendingVideoCapture() const",
    "GetVideoCaptureDimensions(int &width, int &height) const",
    "ResetVideoCapture()",
    "AbandonPendingVideoCaptureReadbacks()",
    "RetireDiscardedVideoReadbacks()",
	"ThrottleVideoFramePacing()",
	"CanRenderNextFrame()",
	"ConsumeVideoCaptureBackpressureFailure()",
	"NoteVideoFramePacingFailure()",
	"mVideoFramePacingFenceCreationFailed",
	"mVideoFramePacingFailureReported",
    "QueueVideoFramePacingFence()",
    "bool issueNext",
)
for fragment in required_opengl_contract:
    if fragment not in opengl_source and fragment not in opengl_header:
        raise SystemExit(f"OpenGL video-readback contract is missing {fragment!r}")

# Vulkan intentionally uses one reusable frame-delayed RGBA8 target/staging
# pair. It must not recreate the old capture-specific wait or use the old
# R16F-pipeline-image-plus-blit route inside the delayed issue function.
required_vulkan_contract = (
    "MAX_VIDEO_READBACK_STAGING_BYTES = 96ull * 1024ull * 1024ull",
    "Presentation(width, height, PixelFormat::Rgba8)",
    "Presentation.TransferSource = true;",
    "VideoCaptureReadbackStaging",
    "std::unique_ptr<FVideoReadbackSlot> mVideoReadback",
    "bool mVideoReadbackRetirementPending = false;",
    "bool mVideoReadbackIssueFailed = false;",
    "TArray<uint8_t> VulkanRenderDevice::GetVideoCaptureBuffer",
    "bool VulkanRenderDevice::HasPendingVideoCapture() const",
    "void VulkanRenderDevice::ResetVideoCapture()",
    "void VulkanRenderDevice::AbandonPendingVideoCaptureReadbacks()",
    "color_type = SS_RGBA;",
    "bottomUp = true;",
    "Staging->Invalidate(0, (size_t)rgbaBytes);",
    "bool issueNext",
)
for fragment in required_vulkan_contract:
    if fragment not in vulkan_source and fragment not in vulkan_header:
        raise SystemExit(f"Vulkan video-readback contract is missing {fragment!r}")
issue_start = vulkan_source.find("bool VulkanRenderDevice::IssueVideoReadback")
issue_end = vulkan_source.find("TArray<uint8_t> VulkanRenderDevice::ConsumeVideoReadback", issue_start)
if issue_start < 0 or issue_end < 0:
    raise SystemExit("could not locate Vulkan delayed video-readback implementation")
vulkan_issue = vulkan_source[issue_start:issue_end]
for fragment in (
    "DrawPresentTexture(box, true, true, &mVideoReadback->Presentation)",
    "copyImageToBuffer(captureImage->Image->image",
    "mVideoReadback->Staging->buffer",
	"catch (const CVulkanError &)",
	"catch (const std::bad_alloc &)",
	"ReleaseVideoReadback();",
):
    if fragment not in vulkan_issue:
        raise SystemExit(f"Vulkan direct RGBA8 video readback is missing {fragment!r}")
for forbidden in (
    "WaitForCommands(",
    "BlitCurrentToImage(",
    "GetCurrentPipelineImage(",
    "SetCurrentPipelineImage(",
    "CopyScreenToBuffer(",
):
    if forbidden in vulkan_issue:
        raise SystemExit(
            f"Vulkan video capture reintroduced an intermediate/blit or mid-frame stall: {forbidden!r}"
        )

# A Vulkan size/reset transition hands the old image and staging buffer to the
# command manager's deferred-delete list. Require the one-sample retirement
# hold so a new target cannot be allocated in the same command lifetime and
# double the largest capture allocation.
for fragment in (
    "mVideoReadbackRetirementPending = true;",
    "if (mVideoReadbackRetirementPending)",
    "mVideoReadbackRetirementPending = false;",
    "mVideoReadbackIssueFailed = true;",
    "pending = !mVideoReadbackIssueFailed;",
    "WaitForCommands(true);",
):
    if fragment not in vulkan_source:
        raise SystemExit(f"Vulkan capture retirement bound is missing {fragment!r}")

# Deferred deletion is still a real GPU lifetime hold, but an allocation
# failure must not masquerade as that normal one-sample delay. Both entry
# paths through GetVideoCaptureBuffer must return a non-pending empty frame for
# the latched failure; CaptureFrame will then take its ordinary stop/drain path
# while DrawDeleteList owns the old resources until WaitForCommands retires it.
vulkan_capture_start = vulkan_source.find("TArray<uint8_t> VulkanRenderDevice::GetVideoCaptureBuffer")
vulkan_capture_end = vulkan_source.find("bool VulkanRenderDevice::HasPendingVideoCapture() const", vulkan_capture_start)
vulkan_reset_start = vulkan_source.find("void VulkanRenderDevice::ResetVideoCapture()")
vulkan_reset_end = vulkan_source.find("bool VulkanRenderDevice::IssueVideoReadback", vulkan_reset_start)
if min(vulkan_capture_start, vulkan_capture_end, vulkan_reset_start, vulkan_reset_end) < 0:
    raise SystemExit("could not locate Vulkan capture failure-latch implementation")
vulkan_capture_body = vulkan_source[vulkan_capture_start:vulkan_capture_end]
vulkan_reset_body = vulkan_source[vulkan_reset_start:vulkan_reset_end]
if vulkan_capture_body.count("pending = !mVideoReadbackIssueFailed;") != 2:
    raise SystemExit("Vulkan deferred-retirement paths must both expose an allocation failure to the recorder")
if "mVideoReadbackIssueFailed = false;" not in vulkan_reset_body:
    raise SystemExit("Vulkan capture reset must clear the allocation-failure latch for a later take")

# A normal recording intentionally leaves capture GPU work outstanding. The
# ordinary OpenGL swap completion point remains for non-capture frames only;
# active capture instead uses a separate bounded end-of-frame fence queue, so
# it neither turns PBO saturation into a glFinish nor lets unrelated GL work
# accumulate without a latency bound.
swap_start = opengl_source.find("void OpenGLFrameBuffer::Swap()")
swap_end = opengl_source.find("//===========================================================================", swap_start + 1)
if swap_start < 0 or swap_end < 0:
    raise SystemExit("could not locate OpenGL swap implementation")
swap_body = opengl_source[swap_start:swap_end]
for fragment in (
    "const bool pendingVideoReadback = HasPendingVideoReadback();",
	"bool useVideoFramePacing = pendingVideoReadback",
	"ThrottleVideoFramePacing();",
	"useVideoFramePacing = HasPendingVideoReadback()",
	"const bool waitForGpuAtSwap = !HasPendingVideoReadback() || !useVideoFramePacing;",
	"QueueVideoFramePacingFence();",
    "if (swapbefore && waitForGpuAtSwap) glFinish();",
    "if (!swapbefore && waitForGpuAtSwap) glFinish();",
):
    if fragment not in swap_body:
        raise SystemExit(f"OpenGL capture no-wait swap contract is missing {fragment!r}")
for forbidden in (
    "IsVideoReadbackSaturated",
    "mVideoReadbackRetirementPending",
    "forceVideoCompletion",
    "mVideoReadbackFinishDeferrals",
):
    if forbidden in swap_body:
        raise SystemExit(f"OpenGL capture reintroduced a readback-triggered glFinish: {forbidden!r}")

pacing_start = opengl_source.find("void OpenGLFrameBuffer::ThrottleVideoFramePacing()")
pacing_end = opengl_source.find("void OpenGLFrameBuffer::QueueVideoFramePacingFence()", pacing_start)
if pacing_start < 0 or pacing_end < 0:
	raise SystemExit("could not locate OpenGL frame-pacing fence throttle")
pacing_body = opengl_source[pacing_start:pacing_end]
for fragment in (
	"if (entry.Fence == nullptr)",
	"glClientWaitSync(oldest->Fence, GL_SYNC_FLUSH_COMMANDS_BIT,",
	"VideoFramePacingRecoveryWaitNS",
	"mVideoFramePacingSaturated = true",
	"frame-admission gate",
):
	if fragment not in pacing_body:
		raise SystemExit(f"OpenGL frame-pacing fence throttle is missing {fragment!r}")
if "glFinish(" in pacing_body:
	raise SystemExit("OpenGL frame-pacing fence throttle must not use glFinish")
if "GL_TIMEOUT_IGNORED" in pacing_body:
	raise SystemExit("OpenGL frame-pacing fence throttle must never wait indefinitely")

admission_start = opengl_source.find("bool OpenGLFrameBuffer::CanRenderNextFrame()")
admission_end = opengl_source.find("void OpenGLFrameBuffer::ThrottleVideoFramePacing()", admission_start)
if admission_start < 0 or admission_end < 0:
	raise SystemExit("could not locate OpenGL frame-admission implementation")
admission_body = opengl_source[admission_start:admission_end]
for fragment in (
	"VideoFramePacingMaxSaturationPolls",
	"VideoFramePacingRecoveryWaitNS",
	"mVideoFramePacingFenceCreationFailed",
	"mVideoFramePacingFailureReported",
	"NoteVideoFramePacingFailure()",
	"return false;",
):
	if fragment not in admission_body:
		raise SystemExit(f"OpenGL frame-admission gate is missing {fragment!r}")
if "glFinish(" in admission_body or "GL_TIMEOUT_IGNORED" in admission_body:
	raise SystemExit("OpenGL frame-admission gate must retain bounded waits")
if "if (mVideoFramePacingFailureReported)" not in admission_body:
	raise SystemExit("OpenGL admission gate must avoid repeated bounded waits after it already failed recording")

# The admission gate must inspect a full ring *before* its optimistic return.
# If saturation were first detected in Swap(), a fourth rendered frame could be
# submitted without a pacing fence after all three tracked frames. That is a
# finite-but-real driver backlog and was the gap fixed by the pre-render check.
preflight_start = admission_body.find("if (!mVideoFramePacingSaturated &&")
fast_path = admission_body.find("if (!mVideoFramePacingSaturated)", preflight_start + 1)
if preflight_start < 0 or fast_path < 0 or preflight_start >= fast_path:
	raise SystemExit("OpenGL admission gate does not preflight fence saturation before rendering")
preflight_body = admission_body[preflight_start:fast_path]
for fragment in (
	"RetireVideoFramePacingFences();",
	"bool full = true;",
	"mVideoFramePacingSaturated = true;",
):
	if fragment not in preflight_body:
		raise SystemExit(f"OpenGL pre-render fence admission is missing {fragment!r}")

queue_start = opengl_source.find("void OpenGLFrameBuffer::QueueVideoFramePacingFence()")
queue_end = opengl_source.find("bool OpenGLFrameBuffer::IssueVideoReadback", queue_start)
if queue_start < 0 or queue_end < 0:
	raise SystemExit("could not locate OpenGL frame-pacing enqueue")
queue_body = opengl_source[queue_start:queue_end]
for fragment in ("glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)", "mVideoFramePacingFenceCreationFailed = true", "NoteVideoFramePacingFailure()"):
	if fragment not in queue_body:
		raise SystemExit(f"OpenGL null-fence safety path is missing {fragment!r}")

display_gate = display_source.find("screen->ConsumeVideoCaptureBackpressureFailure()")
display_begin = display_source.find("screen->BeginFrame()")
if display_gate < 0 or display_begin < 0 or display_gate >= display_begin:
	raise SystemExit("capture back-pressure gate must run before BeginFrame")
for fragment in ("screen->CanRenderNextFrame()", "M_FailVideoRecording(", "GPU capture backlog did not recover"):
	if fragment not in display_source[display_gate:display_begin]:
		raise SystemExit(f"display-side capture back-pressure handling is missing {fragment!r}")
if "M_PollStoppingVideoRecording();" not in display_source[display_gate:display_begin]:
	raise SystemExit("capture-gated display path must continue polling recorder finalization")
if "void M_PollStoppingVideoRecording()" not in recording_header or "void M_PollStoppingVideoRecording()" not in recording_source:
	raise SystemExit("capture-gated display path has no tail-only recorder poll")
poll_stopping_start = recording_source.find("\t\tvoid PollStopping()")
poll_stopping_end = recording_source.find("\n\tprivate:", poll_stopping_start)
if poll_stopping_start < 0 or poll_stopping_end < 0:
	raise SystemExit("capture-gated display path has no recorder stopping poll implementation")
poll_stopping_body = recording_source[poll_stopping_start:poll_stopping_end]
for fragment in ("mStopCaptureDrainPending", "DrainStoppingReadback()", "StopRequested()", "JoinWriterIfFinished()"):
	if fragment not in poll_stopping_body:
		raise SystemExit(f"recorder stopping poll is missing {fragment!r}")
if "CaptureFrame(" in poll_stopping_body or "GetVideoCaptureBuffer" in poll_stopping_body:
	raise SystemExit("capture-gated display path can issue a replacement readback instead of tail-polling")
pending_screenshot_start = recording_source.find("void M_ProcessPendingScreenShot()")
pending_screenshot_end = recording_source.find("\nCCMD(startvideorecording)", pending_screenshot_start)
if pending_screenshot_start < 0 or pending_screenshot_end < 0 or \
		"VideoRecorder.CaptureFrame();" not in recording_source[pending_screenshot_start:pending_screenshot_end]:
	raise SystemExit("ordinary screenshot processing must retain normal video capture servicing")
if "void M_FailVideoRecording(const char *reason)" not in recording_header or "VideoRecorder.Fail(reason);" not in recording_source:
	raise SystemExit("recorder cannot publish a backend back-pressure failure")

reset_start = opengl_source.find("void OpenGLFrameBuffer::ResetVideoCapture()")
reset_end = opengl_source.find("void OpenGLFrameBuffer::RetireDiscardedVideoReadbacks()", reset_start)
if reset_start < 0 or reset_end < 0:
    raise SystemExit("could not locate OpenGL video-capture reset implementation")
reset_body = opengl_source[reset_start:reset_end]
if "slot.Discard = true;" not in reset_body:
    raise SystemExit("OpenGL reset must discard, not synchronously delete, pending PBOs")
if "glFinish(" in reset_body:
    raise SystemExit("OpenGL reset must not synchronously finish pending PBOs")

# CPU-writer congestion must discard only samples that cannot be admitted, not
# tear down the bounded PBO/staging allocation on every dropped frame. This is
# the critical escape path for the OpenGL frame-pacing gate: a full CPU queue
# must not leave an obsolete readback looking like active footage forever.
if "mReadbackAbandonedForBackpressure" not in capture_body:
    raise SystemExit("recorder has no latch for writer-backpressure readback abandonment")
if capture_body.count("screen->AbandonPendingVideoCaptureReadbacks();") < 2:
    raise SystemExit("recorder must abandon unusable GPU readbacks on both queue-admission paths")
admission_start = capture_body.find("if (!CanQueueFrame(maximumReadbackBytes))")
admission_end = capture_body.find("mReadbackAbandonedForBackpressure = false;", admission_start)
if admission_start < 0 or admission_end < 0:
    raise SystemExit("could not locate recorder preflight backpressure handling")
admission_body = capture_body[admission_start:admission_end]
for fragment in (
    "if (!mReadbackAbandonedForBackpressure)",
    "screen->AbandonPendingVideoCaptureReadbacks();",
    "mReadbackAbandonedForBackpressure = true;",
    "ScheduleNextCapture(now);",
    "NoteDroppedFrame();",
):
    if fragment not in admission_body:
        raise SystemExit(f"recorder backpressure abandonment is missing {fragment!r}")
if admission_body.find("screen->AbandonPendingVideoCaptureReadbacks();") > admission_body.find("ScheduleNextCapture(now);"):
    raise SystemExit("recorder must abandon a stale GPU readback before rescheduling a dropped sample")

abandon_start = opengl_source.find("void OpenGLFrameBuffer::AbandonPendingVideoCaptureReadbacks()")
abandon_end = opengl_source.find("void OpenGLFrameBuffer::RetireDiscardedVideoReadbacks()", abandon_start)
recycle_start = opengl_source.find("void OpenGLFrameBuffer::RecycleVideoReadback")
recycle_end = opengl_source.find("void OpenGLFrameBuffer::ResetVideoCapture()", recycle_start)
if min(abandon_start, abandon_end, recycle_start, recycle_end) < 0:
    raise SystemExit("could not locate OpenGL backpressure-readback retirement")
abandon_body = opengl_source[abandon_start:abandon_end]
recycle_body = opengl_source[recycle_start:recycle_end]
for fragment in ("slot.Pending && !slot.Discard", "slot.Discard = true;"):
    if fragment not in abandon_body:
        raise SystemExit(f"OpenGL pending-readback abandon is missing {fragment!r}")
if "ReleaseVideoReadback" in abandon_body or "glDeleteBuffers" in abandon_body:
    raise SystemExit("OpenGL backpressure abandon must retain reusable idle PBO storage")
for fragment in ("slot.Pending = false;", "slot.Discard = false;", "slot.Fence = nullptr;"):
    if fragment not in recycle_body:
        raise SystemExit(f"OpenGL discarded PBO recycle is missing {fragment!r}")
if "glDeleteBuffers" in recycle_body:
    raise SystemExit("OpenGL discarded PBO recycle must retain its allocation")

vulkan_abandon_start = vulkan_source.find("void VulkanRenderDevice::AbandonPendingVideoCaptureReadbacks()")
vulkan_abandon_end = vulkan_source.find("bool VulkanRenderDevice::IssueVideoReadback", vulkan_abandon_start)
if vulkan_abandon_start < 0 or vulkan_abandon_end < 0:
    raise SystemExit("could not locate Vulkan pending-readback abandonment")
vulkan_abandon_body = vulkan_source[vulkan_abandon_start:vulkan_abandon_end]
for fragment in ("mVideoReadback && mVideoReadback->Pending", "mVideoReadback->CaptureTimeNS = 0;", "mVideoReadback->Pending = false;"):
    if fragment not in vulkan_abandon_body:
        raise SystemExit(f"Vulkan pending-readback abandon is missing {fragment!r}")
if "ReleaseVideoReadback" in vulkan_abandon_body:
    raise SystemExit("Vulkan backpressure abandon must retain reusable staging storage")

retire_start = opengl_source.find("void OpenGLFrameBuffer::RetireDiscardedVideoReadbacks()")
retire_end = opengl_source.find("void OpenGLFrameBuffer::ReleaseVideoFramePacingFences()", retire_start)
if retire_start < 0 or retire_end < 0:
    raise SystemExit("could not locate OpenGL discarded-readback retirement")
retire_body = opengl_source[retire_start:retire_end]
for fragment in (
    "slot.Pending || !slot.Discard",
    "glClientWaitSync(slot.Fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0)",
    "RecycleVideoReadback(slot);",
    "ReleaseVideoReadback(slot);",
):
    if fragment not in retire_body:
        raise SystemExit(f"OpenGL discarded PBO reclaim is missing {fragment!r}")
if "glFinish(" in retire_body:
    raise SystemExit("OpenGL discarded PBO reclaim must remain a zero-wait poll")

consume_start = opengl_source.find("TArray<uint8_t> OpenGLFrameBuffer::ConsumeVideoReadback")
consume_end = opengl_source.find("void OpenGLFrameBuffer::GetVideoCaptureDimensions", consume_start)
if consume_start < 0 or consume_end < 0:
    raise SystemExit("could not locate OpenGL PBO consume path")
consume_body = opengl_source[consume_start:consume_end]
map_call = re.search(r"glMapBufferRange\s*\([^;]+\);", consume_body, re.S)
if map_call is None or "GL_MAP_READ_BIT" not in map_call.group(0):
    raise SystemExit("OpenGL PBO consume path must use a readable map")
if "GL_MAP_UNSYNCHRONIZED_BIT" in map_call.group(0):
    raise SystemExit("OpenGL PBO read map must not use the write-only UNSYNCHRONIZED flag")

issue_start = opengl_source.find("bool OpenGLFrameBuffer::IssueVideoReadback")
issue_end = opengl_source.find("TArray<uint8_t> OpenGLFrameBuffer::ConsumeVideoReadback", issue_start)
if issue_start < 0 or issue_end < 0:
    raise SystemExit("could not locate OpenGL video-readback issue path")
opengl_issue = opengl_source[issue_start:issue_end]
if not re.search(r"if\s*\(\s*slot\s*==\s*nullptr\s*\)\s*\{\s*return false;", opengl_issue):
    raise SystemExit("OpenGL PBO saturation must skip a sample instead of blocking")

# A PBO allocation can fail after glGenBuffers succeeds. Its GL error must be
# scoped to that allocation, and the slot must be released before its byte
# count is committed; otherwise every later request incorrectly reuses an
# unallocated buffer and the recorder spins on empty readbacks.
allocation_start = opengl_issue.find("if (slot->AllocatedBytes != sourceBytes)")
allocation_end = opengl_issue.find("glReadPixels", allocation_start)
if allocation_start < 0 or allocation_end < 0:
    raise SystemExit("could not locate OpenGL PBO allocation path")
allocation_body = opengl_issue[allocation_start:allocation_end]
for fragment in (
    "ClearVideoReadbackErrors();",
    "glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)sourceBytes, nullptr, GL_STREAM_READ);",
    "VideoReadbackHasError()",
    "ReleaseVideoReadback(*slot);",
    "return false;",
    "slot->AllocatedBytes = sourceBytes;",
):
    if fragment not in allocation_body:
        raise SystemExit(f"OpenGL PBO allocation-failure recovery is missing {fragment!r}")
buffer_data_index = allocation_body.find("glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)sourceBytes, nullptr, GL_STREAM_READ);")
error_index = allocation_body.find("VideoReadbackHasError()", buffer_data_index)
release_index = allocation_body.find("ReleaseVideoReadback(*slot);", error_index)
commit_index = allocation_body.find("slot->AllocatedBytes = sourceBytes;")
if min(buffer_data_index, error_index, release_index, commit_index) < 0 or not (
    buffer_data_index < error_index < release_index < commit_index
):
    raise SystemExit("OpenGL PBO allocation must release on error before committing allocation bytes")

# The capture-specific PBO functions must never synchronously finish the GPU.
# Frame pacing has its own admission gate and may mention glFinish in comments,
# so inspect executable function bodies rather than a broad source slice.
for function_start, function_end, label in (
	(reset_start, reset_end, "reset"),
	(retire_start, retire_end, "discarded-readback retirement"),
	(issue_start, issue_end, "PBO issue"),
	(consume_start, consume_end, "PBO consume"),
):
	if "glFinish(" in opengl_source[function_start:function_end]:
		raise SystemExit(f"OpenGL {label} must not synchronously finish pending GPU work")
# WriterMain and its PNG workers are background threads. Console output owns
# mutable main-thread containers, so diagnostics must be handed back to
# CompleteStop rather than calling Printf directly from an encoder thread.
for label, body in (
    ("parallel PNG coordinator", parallel_body),
    ("serial writer", serial_body),
    ("writer main", writer_main_body),
):
    if "Printf(" in body or "DPrintf(" in body:
        raise SystemExit(f"{label} must not write to the console from a video writer thread")
for fragment in (
    "bool mWriterPngPoolFallback = false;",
    "mWriterPngPoolFallback = true;",
    "pngPoolFallback = mWriterPngPoolFallback;",
    "PNG encoder pool was unavailable; used bounded serial encoding.",
):
    if fragment not in recording_source:
        raise SystemExit(f"writer-to-main diagnostic handoff is missing {fragment!r}")

# Stopping a capture or dropping an unusable CPU-writer tail marks old PBOs
# Discard. Once no active-take PBO remains, those slots contain no footage that
# needs preserving: the admission gate must drop their GL handles immediately,
# before its 120-poll recovery window. The one-shot active-take failure still
# has to be reported first so this fast path cannot swallow it.
swap_start = opengl_source.find("void OpenGLFrameBuffer::Swap()")
swap_end = opengl_source.find("\nvoid OpenGLFrameBuffer::SetVSync", swap_start)
can_render_start = opengl_source.find("bool OpenGLFrameBuffer::CanRenderNextFrame()")
can_render_end = opengl_source.find("\nvoid OpenGLFrameBuffer::ThrottleVideoFramePacing", can_render_start)
if swap_start < 0 or swap_end < 0 or can_render_start < 0 or can_render_end < 0:
    raise SystemExit("could not locate OpenGL capture pacing gates")
swap_body = opengl_source[swap_start:swap_end]
can_render_body = opengl_source[can_render_start:can_render_end]
for fragment in (
    "const bool pendingVideoReadback = HasPendingVideoReadback();",
	"bool useVideoFramePacing = pendingVideoReadback",
	"ThrottleVideoFramePacing();",
	"useVideoFramePacing = HasPendingVideoReadback()",
	"const bool waitForGpuAtSwap = !HasPendingVideoReadback() || !useVideoFramePacing;",
):
    if fragment not in swap_body:
        raise SystemExit(f"OpenGL stale-readback bounded-pacing contract is missing {fragment!r}")
for fragment in (
    "if (!HasPendingVideoReadback())",
    "const auto abandonDiscardOnlyReadbacks = [this]()",
    "AbandonDiscardedVideoReadbacks();",
    "ResetVideoCapture();",
    "AbandonVideoReadbacks();",
    "VideoFramePacingMaxSaturationPolls",
):
    if fragment not in can_render_body:
        raise SystemExit(f"OpenGL discard-only recovery contract is missing {fragment!r}")
failure_pending_index = can_render_body.find("if (mVideoFramePacingFailurePending)")
discard_only_index = can_render_body.find("if (!HasPendingVideoCapture())")
saturation_preflight_index = can_render_body.find("if (!mVideoFramePacingSaturated)")
if min(failure_pending_index, discard_only_index, saturation_preflight_index) < 0 or not (
    failure_pending_index < discard_only_index < saturation_preflight_index
):
    raise SystemExit(
        "OpenGL discard-only PBO teardown must follow the active-take failure handoff "
        "and precede frame-pacing saturation recovery"
    )
discard_only_body = can_render_body[discard_only_index:saturation_preflight_index]
for fragment in (
    "AbandonDiscardedVideoReadbacks();",
    "ReleaseVideoFramePacingFences();",
    "return true;",
):
    if fragment not in discard_only_body:
        raise SystemExit(f"OpenGL immediate discard-only teardown is missing {fragment!r}")
for forbidden in ("VideoFramePacingRecoveryWaitNS", "VideoFramePacingMaxSaturationPolls", "glClientWaitSync("):
    if forbidden in discard_only_body:
        raise SystemExit(f"OpenGL discard-only teardown must not enter bounded fence recovery: {forbidden!r}")
async_support_start = opengl_source.find("static bool HasAsyncVideoReadbackSupport()")
async_support_end = opengl_source.find("//==========================================================================", async_support_start)
if async_support_start < 0 or async_support_end < 0:
    raise SystemExit("could not locate the shared OpenGL async-readback capability predicate")
async_support_body = opengl_source[async_support_start:async_support_end]
for fragment in (
    "glFenceSync != nullptr",
    "glClientWaitSync != nullptr",
    "glMapBufferRange != nullptr",
    "glDeleteSync != nullptr",
    "glUnmapBuffer != nullptr",
):
    if fragment not in async_support_body:
        raise SystemExit(f"OpenGL async-readback capability predicate is missing {fragment!r}")
capture_start = opengl_source.find("TArray<uint8_t> OpenGLFrameBuffer::GetVideoCaptureBuffer")
capture_end = opengl_source.find("bool OpenGLFrameBuffer::HasPendingVideoCapture()", capture_start)
if capture_start < 0 or capture_end < 0:
    raise SystemExit("could not locate OpenGL capture fallback implementation")
if "if (!HasAsyncVideoReadbackSupport())" not in opengl_source[capture_start:capture_end]:
    raise SystemExit("OpenGL screenshot fallback must use the shared async-readback capability predicate")
api_loss_index = can_render_body.find("if (!HasAsyncVideoReadbackSupport())")
unsaturated_index = can_render_body.find("if (!mVideoFramePacingSaturated)")
if api_loss_index < 0 or unsaturated_index < 0 or api_loss_index > unsaturated_index:
    raise SystemExit("OpenGL API-loss cleanup must precede the unsaturated admission fast path")
abandon_start = opengl_source.find("void OpenGLFrameBuffer::AbandonDiscardedVideoReadbacks()")
abandon_end = opengl_source.find("void OpenGLFrameBuffer::AbandonVideoReadbacks()", abandon_start)
all_abandon_end = opengl_source.find("void OpenGLFrameBuffer::ReleaseVideoFramePacingFences()", abandon_end)
release_pacing_start = all_abandon_end
release_pacing_end = opengl_source.find("void OpenGLFrameBuffer::RetireVideoFramePacingFences()", release_pacing_start)
if min(abandon_start, abandon_end, all_abandon_end, release_pacing_end) < 0:
    raise SystemExit("could not locate OpenGL discard-only recovery helpers")
abandon_body = opengl_source[abandon_start:abandon_end]
all_abandon_body = opengl_source[abandon_end:all_abandon_end]
release_pacing_body = opengl_source[release_pacing_start:release_pacing_end]
for fragment in ("if (slot.Discard)", "ReleaseVideoReadback(slot);"):
    if fragment not in abandon_body:
        raise SystemExit(f"discard-only PBO abandonment is missing {fragment!r}")
if "ReleaseVideoReadback(slot);" not in all_abandon_body:
    raise SystemExit("OpenGL API-loss recovery must release every old PBO slot")
for fragment in ("if (glDeleteSync != nullptr)", "mVideoFramePacingFailurePending = false;"):
    if fragment not in release_pacing_body:
        raise SystemExit(f"OpenGL pacing teardown is missing {fragment!r}")
release_readback_start = opengl_source.find("void OpenGLFrameBuffer::ReleaseVideoReadback")
release_readback_end = opengl_source.find("void OpenGLFrameBuffer::ResetVideoCapture()", release_readback_start)
release_readback_body = opengl_source[release_readback_start:release_readback_end]
for fragment in ("if (glDeleteSync != nullptr)", "if (glDeleteBuffers != nullptr)"):
    if fragment not in release_readback_body:
        raise SystemExit(f"OpenGL API-loss PBO teardown is missing {fragment!r}")

# A shortcut must give the player an immediate visible state, not only console
# text. Keep the lifecycle hand-off explicit: a running wall-clock indicator,
# a distinct asynchronous-finalization state, and short terminal success or
# failure notices rendered above the normal HUD/menu stack.
required_status_contract = (
    "bool M_GetVideoRecordingStatus(FVideoRecordingStatus &status)",
    "VRS_Recording",
    "VRS_Stopping",
    "VRS_Finalized",
    "VRS_Failed",
    "PublishStoppingStatus",
    "PublishTerminalStatus(VRS_Finalized",
    "PublishTerminalStatus(VRS_Failed",
    "DrawVideoRecordingStatus()",
    "FINALIZING VIDEO",
    "VIDEO SAVED",
    "VIDEO FAILED",
    "REC %s%s%s",
)
for fragment in required_status_contract:
    if fragment not in recording_source and fragment not in recording_header and fragment not in display_source:
        raise SystemExit(f"video recording status UX is missing {fragment!r}")

# Title/demo, cutscene, and intermission handlers sit outside the regular
# gameplay responder. Keep their capture-key route in the regression gate so a
# menu-defined shortcut (including a double-tap binding) cannot be swallowed
# or turn into a screen advance in one of those transient states.
shortcut_routes = {
    "src/g_game.cpp": ("C_IsCaptureKey(ev->data1)", "C_DoKey(ev, &Bindings, &DoubleBindings);"),
    "src/common/cutscenes/screenjob.cpp": ("C_IsCaptureKey(ev->data1)", "C_DoKey(ev, &Bindings, &DoubleBindings);"),
    "src/intermission/intermission.cpp": ("C_IsCaptureKey(ev->KeyScan)",),
}
for relative, required in shortcut_routes.items():
    path = os.path.join(root, relative)
    text = open(path, encoding="utf-8").read()
    for fragment in required:
        if fragment not in text:
            raise SystemExit(f"capture shortcut route is missing {fragment!r} from {relative}")

print("validated capture UI, bounded-writer safety contract, and transient-screen shortcut routes")
EOF

# Capture needs a composed hardware frame. Prefer a private X server even on
# desktop machines: it avoids background-window readback races. The default
# asks for OpenGL; the opt-in Vulkan visual mode below verifies that a real
# Vulkan device was created before it interprets rendered pixels. Merely
# finding xvfb-run is not sufficient: a container can have it installed while
# the X socket cannot be created, in which case SDL silently selects its
# non-presenting offscreen driver and can hang before the game initializes.
display_runner=()
usable_display=0
if command -v xvfb-run >/dev/null 2>&1 && command -v xdpyinfo >/dev/null 2>&1; then
    if xvfb-run -a -s "-screen 0 1280x800x24" sh -c 'xdpyinfo -display "$DISPLAY" >/dev/null 2>&1' >/dev/null 2>&1; then
        display_runner=(xvfb-run -a -s "-screen 0 1280x800x24")
        usable_display=1
    fi
fi
if [[ "${usable_display}" -eq 0 && -n "${DISPLAY:-}" ]] && command -v xdpyinfo >/dev/null 2>&1; then
    if xdpyinfo -display "${DISPLAY}" >/dev/null 2>&1; then
        usable_display=1
    fi
fi
if [[ "${usable_display}" -eq 0 && -n "${WAYLAND_DISPLAY:-}" ]]; then
    # SDL validates the Wayland connection during startup. Keep this fallback
    # for a native Wayland desktop, then explicitly reject an offscreen driver
    # from the engine log below.
    usable_display=1
fi
if [[ "${usable_display}" -eq 0 ]]; then
    printf 'error: a working display is required for rendered video capture (xvfb-run could not create an X display)\n' >&2
    exit 2
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-video-recording.XXXXXX")"
cleanup() {
    if [[ "${keep_temp}" -eq 0 ]]; then
        rm -rf "${test_root}"
    else
        printf 'keeping test artifacts in %s\n' "${test_root}"
    fi
}
trap cleanup EXIT

last_stdout=""
last_actual_backend=""
fail() {
    printf 'FAIL: %s\n' "$1" >&2
    [[ -z "${last_stdout}" ]] || tail -n 80 "${last_stdout}" >&2 || true
    exit 1
}

run_engine() {
    local mode="$1"
    local format="$2"
    local output_dir="$3"
    local success_pattern="$4"
    local replay_demo="${5:-}"
    local stdout_file="${test_root}/${mode}-${format}.stdout"
    local command_file="${test_root}/${mode}-${format}.cfg"
    local initial_format="${format}"
    local pid status completed=0 process_state="" runner_group="" script_group="" hitch_pid=""
    last_stdout="${stdout_file}"
    mkdir -p "${output_dir}"
    if [[ "${mode}" == "demo" ]]; then
        cat > "${command_file}" <<'EOF'
wait 45; i_timescale 0.5; recorddemo; wait 50; stopdemorecording; wait 12; echo PYTEST_VIDEO_DEMO_DONE; quit
EOF
    elif [[ "${mode}" == "playback" ]]; then
        [[ -n "${replay_demo}" && -f "${replay_demo}" ]] || fail 'demo playback was not given a recorded demo'
        # The internally-created path may contain spaces, so quote it for the
        # console parser. The marker lands well before this short smoke demo
        # completes, proving G_DoPlayDemo reached an active playback map.
        printf 'wait 45; playdemo "%s"; wait 12; echo PYTEST_VIDEO_DEMO_PLAYBACK_REACHED; quit\n' "${replay_demo}" > "${command_file}"
    elif [[ "${mode}" == "music" ]]; then
        # Effects are muted and no console sound is played. A non-silent WAV
        # therefore proves that the recorder receives the active map music
        # stream, rather than only predecoded OpenAL effect buffers.
        cat > "${command_file}" <<'EOF'
wait 100; snd_mastervolume 1; snd_sfxvolume 0; snd_musicvolume 1; vid_record_format 0; startvideorecording; wait 140; stopvideorecording; wait 280; echo PYTEST_VIDEO_MUSIC_DONE; quit
EOF
    elif [[ "${mode}" == "sustained" ]]; then
        # This deliberately runs longer than the old three-frame AVI hold
        # allowance at the normal 60 FPS setting. A real capture backend may
        # skip samples under pressure, but it must still finalize the whole
        # requested wall-clock interval rather than silently returning the
        # first contiguous second or two as an AVI prefix.
        #
        # Seven seconds keeps the raw lossless payload reasonable even when
        # the rendered presentation viewport is larger than the logical
        # 640x480 window. The test injects one 600-ms process hitch after the
        # start marker. The 350-tic tail lets the asynchronous writer finish
        # before the explicit completion marker and quit.
        cat > "${command_file}" <<'EOF'
wait 70; snd_mastervolume 1; snd_sfxvolume 1; snd_musicvolume 1; vid_record_format 1; vid_record_fps 60; startvideorecording SustainedAVI; echo PYTEST_VIDEO_SUSTAINED_STARTED; wait 245; stopvideorecording; wait 350; echo PYTEST_VIDEO_SUSTAINED_DONE; quit
EOF
    elif [[ "${format}" == "both" ]]; then
        initial_format=0
        # Exercise both formats in one rendered session. The second start is
        # intentionally issued immediately after the first asynchronous stop:
        # it must either arm at once or queue safely until the prior writer
        # finishes. Video sampling is based on wall time after a composited
        # frame is available, not game tics.
        # Give a fresh Doom I renderer a full two seconds to settle, then
        # record PNG at half game speed. The 70-tic capture interval spans
        # roughly four wall-clock seconds at 0.5x, so the frame-count check
        # below proves cadence is not coupled to scaled game time. Keep the
        # immediate stop/start below, but leave an extra two seconds in the
        # AVI request window: a deliberately non-blocking restart may wait for
        # the preceding PNG writer and its bounded final-GPU-readback drain.
        # This preserves the restart coverage and the strict AVI-duration
        # assertion rather than turning a temporarily busy writer into a flaky
        # short-capture result.
        cat > "${command_file}" <<'EOF'
wait 70; i_timescale 0.5; snd_mastervolume 1; snd_sfxvolume 1; snd_musicvolume 1; vid_record_format 0; startvideorecording; wait 10; playsound weapons/pistol; echo PYTEST_VIDEO_PNG_SOUND_TRIGGERED; wait 25; playsound weapons/pistol; wait 35; stopvideorecording; i_timescale 1; vid_record_format 1; startvideorecording; wait 10; playsound weapons/pistol; echo PYTEST_VIDEO_AVI_SOUND_TRIGGERED; wait 20; playsound weapons/pistol; wait 20; playsound weapons/pistol; wait 20; playsound weapons/pistol; wait 110; stopvideorecording; wait 70; echo PYTEST_VIDEO_CAPTURE_DONE; quit
EOF
    else
        cat > "${command_file}" <<'EOF'
wait 45; startvideorecording; wait 50; stopvideorecording; wait 70; echo PYTEST_VIDEO_CAPTURE_DONE; quit
EOF
    fi

    # `quit` has already flushed a capture by the time its explicit marker is
    # printed. Some SDL backends can nevertheless linger during teardown, so
    # end that post-export tail as soon as the marker is observed. Do not wrap
    # the job in `setsid`: a fully detached session can make an automation host
    # believe this test shell has finished before its later validators run.
    # GNU timeout already supervises its child in a separate process group when
    # not invoked with --foreground, and forwards a normal interrupt to it.
    # The guarded group kill below is only an emergency fallback for a backend
    # that ignores that interrupt.
    # OpenAL Soft's null backend gives this rendered test a deterministic
    # engine mixer without requiring host audio hardware. Unlike -nosound, it
    # still instantiates sound effects and therefore verifies the recorder's
    # PCM tap and container tracks.
    env ALSOFT_DRIVERS=null \
    timeout --signal=INT --kill-after=5s "${test_timeout}s" \
        "${display_runner[@]}" stdbuf -oL -eL "${engine_exe}" \
        -stdout -nointro -noautoload -noautoexec \
        -config "${test_root}/${mode}-${format}.ini" \
        -iwad "${iwad_path}" \
        -width 640 -height 480 +vid_fullscreen false +vid_preferbackend "${backend_preference}" \
        +vid_activeinbackground true +i_pauseinbackground false \
        +capture_export_dir "${output_dir}" \
        +vid_record_name VideoSmoke +vid_record_format "${initial_format}" +vid_record_fps 10 \
        +demo_record_name DemoSmoke +demo_record_map '*' -warp 1 +exec "${command_file}" \
        >"${stdout_file}" 2>&1 < /dev/null &
    pid=$!

    if [[ "${mode}" == "sustained" ]]; then
        # Inject one short scheduling/render hitch after the engine has armed
        # the take. This makes the duration regression deterministic on both a
        # fast local GPU and a slow CI renderer: the old three-slot AVI limiter
        # converted this ordinary wall-clock gap into a silently truncated
        # prefix. GNU timeout owns a private process group here; validate that
        # relationship before pausing it, so this can never stop the test shell
        # itself. The helper stays in the shell's group and resumes the engine
        # after 600 ms.
        (
            for ((hitch_tick = 0; hitch_tick < 300; ++hitch_tick)); do
                if grep -Fq 'PYTEST_VIDEO_SUSTAINED_STARTED' "${stdout_file}"; then
                    sleep 1
                    local_group="$(ps -o pgid= -p "${pid}" 2>/dev/null | tr -d '[:space:]' || true)"
                    local_script_group="$(ps -o pgid= -p "$$" 2>/dev/null | tr -d '[:space:]' || true)"
                    if [[ "${local_group}" == "${pid}" && -n "${local_group}" && "${local_group}" != "${local_script_group}" ]] && \
                        kill -STOP -- "-${local_group}" 2>/dev/null; then
                        sleep 0.6
                        if kill -CONT -- "-${local_group}" 2>/dev/null; then
                            printf 'sustained AVI hitch injected\n' > "${test_root}/sustained-hitch.txt"
                        fi
                    fi
                    exit 0
                fi
                if ! kill -0 "${pid}" 2>/dev/null; then
                    exit 0
                fi
                sleep 0.05
            done
        ) &
        hitch_pid=$!
    fi

    for ((tick = 0; tick < test_timeout * 10; ++tick)); do
        if grep -Fq "${success_pattern}" "${stdout_file}"; then
            completed=1
            break
        fi
        if ! kill -0 "${pid}" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done

    if kill -0 "${pid}" 2>/dev/null; then
        # `quit` normally owns a short renderer/audio teardown, including the
        # recorder's bounded final-GPU-readback grace period. Let it finish
        # that normal path before signalling the timeout wrapper. `kill -0`
        # remains true for a zombie, so inspect ps state rather than treating
        # its existence as a live engine process.
        for ((tick = 0; tick < 50; ++tick)); do
            process_state="$(ps -o stat= -p "${pid}" 2>/dev/null | tr -d '[:space:]' || true)"
            [[ -z "${process_state}" || "${process_state}" == Z* ]] && break
            sleep 0.1
        done
        if [[ -n "${process_state}" && "${process_state}" != Z* ]] && kill -0 "${pid}" 2>/dev/null; then
            kill -INT "${pid}" 2>/dev/null || true
            for ((tick = 0; tick < 50; ++tick)); do
                process_state="$(ps -o stat= -p "${pid}" 2>/dev/null | tr -d '[:space:]' || true)"
                [[ -z "${process_state}" || "${process_state}" == Z* ]] && break
                sleep 0.1
            done
            if [[ -n "${process_state}" && "${process_state}" != Z* ]] && kill -0 "${pid}" 2>/dev/null; then
                # Default GNU timeout places its monitored command in a private
                # process group. Verify that relationship before the emergency
                # kill so a future timeout implementation cannot terminate this
                # test shell or leave a detached engine process behind.
                runner_group="$(ps -o pgid= -p "${pid}" 2>/dev/null | tr -d '[:space:]' || true)"
                script_group="$(ps -o pgid= -p "$$" 2>/dev/null | tr -d '[:space:]' || true)"
                if [[ "${runner_group}" == "${pid}" && -n "${runner_group}" && "${runner_group}" != "${script_group}" ]]; then
                    kill -KILL -- "-${runner_group}" 2>/dev/null || true
                else
                    # A direct timeout kill can orphan an engine child on an
                    # implementation that does not own a private group. Send a
                    # second forwarded signal and let its global timeout retain
                    # ownership if that unusual platform refuses it.
                    kill -TERM "${pid}" 2>/dev/null || true
                fi
            fi
        fi
    fi

    set +e
    wait "${pid}" 2>/dev/null
    status=$?
    set -e
    if [[ -n "${hitch_pid}" ]]; then
        wait "${hitch_pid}" 2>/dev/null || true
    fi
    if grep -Fq 'Using video driver offscreen' "${stdout_file}"; then
        fail "${mode}/${format} selected SDL's non-presenting offscreen driver; a working display is required"
    fi
    [[ "${completed}" -eq 1 ]] || fail "${mode}/${format} engine run ended before ${success_pattern} (status ${status})"
    if grep -Fq 'Vulkan device:' "${stdout_file}"; then
        last_actual_backend="vulkan"
    else
        last_actual_backend="other"
    fi
}

video_dir="${test_root}/capture output/video"
# Reserve later media members and the PNG timing sidecar for the requested
# family. This catches a selector that checks only VideoSmoke.png/VideoSmoke.avi
# and would otherwise overwrite or mix an incomplete prior recording.
png_family_sentinel="${video_dir}/VideoSmoke_frame999999.png"
png_timeline_sentinel="${video_dir}/VideoSmoke.ffconcat"
png_audio_sentinel="${video_dir}/VideoSmoke_audio.wav"
avi_family_sentinel="${video_dir}/VideoSmoke_part002.avi"
mkdir -p "${video_dir}"
printf 'BiasedDoom stale PNG family sentinel\n' > "${png_family_sentinel}"
printf 'BiasedDoom stale PNG timeline sentinel\n' > "${png_timeline_sentinel}"
printf 'BiasedDoom stale PNG audio sentinel\n' > "${png_audio_sentinel}"
printf 'BiasedDoom stale AVI family sentinel\n' > "${avi_family_sentinel}"
[[ ! -e "${video_dir}/VideoSmoke.png" && ! -e "${video_dir}/VideoSmoke.avi" ]] || fail 'capture family sentinels unexpectedly created a base file'
run_engine video both "${video_dir}" 'PYTEST_VIDEO_CAPTURE_DONE'
video_stdout="${last_stdout}"
if [[ "${validate_pixels}" -eq 1 && "${last_actual_backend}" != "vulkan" ]]; then
    unavailable_note='Vulkan visual capture was requested, but no Vulkan device was created (the platform fell back or has no usable Vulkan surface)'
    if [[ "${require_backend}" -eq 1 ]]; then
        fail "${unavailable_note}"
    fi
    printf 'SKIP: %s; preserving the portable OpenGL structural/audio regression result.\n' "${unavailable_note}"
    validate_pixels=0
fi
grep -Fq 'PYTEST_VIDEO_CAPTURE_DONE' "${video_stdout}" || fail 'video driver did not complete'
grep -Fq 'Lossless PNG recording armed' "${video_stdout}" || fail 'video driver did not start PNG recording'
grep -Fq 'Lossless RGB AVI recording armed' "${video_stdout}" || fail 'video driver did not start AVI recording'
grep -Fq 'PYTEST_VIDEO_PNG_SOUND_TRIGGERED' "${video_stdout}" || fail 'video driver did not trigger the PNG audio probe'
grep -Fq 'PYTEST_VIDEO_AVI_SOUND_TRIGGERED' "${video_stdout}" || fail 'video driver did not trigger the AVI audio probe'
if grep -Fq "'weapons/pistol' is not a sound" "${video_stdout}"; then
    fail 'video audio probe did not resolve the known IWAD pistol sound'
fi

# A short smoke capture can accidentally pass even if the writer gives up on
# its first sustained cadence gap. Exercise the user-facing default 60 FPS
# interval separately and validate its final AVI duration before moving on to
# the unrelated music/demo coverage below.
sustained_dir="${test_root}/capture output/sustained avi"
run_engine sustained 1 "${sustained_dir}" 'PYTEST_VIDEO_SUSTAINED_DONE'
sustained_stdout="${last_stdout}"
grep -Fq 'PYTEST_VIDEO_SUSTAINED_STARTED' "${sustained_stdout}" || fail 'sustained AVI driver did not start recording'
grep -Fq 'Lossless RGB AVI recording armed at 60 FPS' "${sustained_stdout}" || fail 'sustained AVI driver did not select 60 FPS'
[[ -f "${test_root}/sustained-hitch.txt" ]] || fail 'sustained AVI driver did not receive the deterministic render hitch'
if grep -Fq 'AVI capture finalized a playable selected-rate prefix' "${sustained_stdout}"; then
    fail 'sustained AVI finalized a cadence-limited prefix instead of the requested interval'
fi
python3 - "${sustained_dir}" <<'EOF'
import glob
import os
import struct
import sys

capture_dir = sys.argv[1]
paths = sorted(glob.glob(os.path.join(capture_dir, "SustainedAVI*.avi")))
if paths != [os.path.join(capture_dir, "SustainedAVI.avi")]:
    raise SystemExit(f"expected exactly one sustained AVI output, found {paths}")

data = open(paths[0], "rb").read()
if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"AVI ":
    raise SystemExit("sustained output is not a RIFF/AVI file")
riff_end = struct.unpack_from("<I", data, 4)[0] + 8
if riff_end != len(data):
    raise SystemExit(f"sustained AVI RIFF size mismatch ({riff_end} header bytes, {len(data)} actual bytes)")

def chunks(begin, end):
    cursor = begin
    while cursor < end:
        if cursor + 8 > end:
            raise SystemExit("truncated sustained AVI chunk header")
        fourcc = data[cursor:cursor + 4]
        size = struct.unpack_from("<I", data, cursor + 4)[0]
        body_start = cursor + 8
        body_end = body_start + size
        if body_end > end:
            raise SystemExit(f"truncated sustained AVI chunk {fourcc!r}")
        yield fourcc, cursor, body_start, body_end
        cursor = body_end + (size & 1)
    if cursor != end:
        raise SystemExit("malformed sustained AVI chunk padding")

root = list(chunks(12, riff_end))
hdrl = next((chunk for chunk in root if chunk[0] == b"LIST" and
             data[chunk[2]:chunk[2] + 4] == b"hdrl"), None)
idx1 = next((chunk for chunk in root if chunk[0] == b"idx1"), None)
if hdrl is None or idx1 is None or (idx1[3] - idx1[2]) % 16:
    raise SystemExit("sustained AVI is missing a valid header or idx1 index")

hdrl_chunks = list(chunks(hdrl[2] + 4, hdrl[3]))
avih = next((chunk for chunk in hdrl_chunks if chunk[0] == b"avih" and chunk[3] - chunk[2] == 56), None)
if avih is None:
    raise SystemExit("sustained AVI is missing avih")
frame_count = struct.unpack_from("<I", data, avih[2] + 16)[0]

video_header = None
audio_header = None
for chunk in hdrl_chunks:
    if chunk[0] != b"LIST" or data[chunk[2]:chunk[2] + 4] != b"strl":
        continue
    stream_chunks = list(chunks(chunk[2] + 4, chunk[3]))
    header = next((entry for entry in stream_chunks if entry[0] == b"strh" and entry[3] - entry[2] == 56), None)
    if header is None:
        continue
    stream_type = data[header[2]:header[2] + 4]
    if stream_type == b"vids":
        video_header = header
    elif stream_type == b"auds":
        audio_header = header
if video_header is None or audio_header is None:
    raise SystemExit("sustained AVI is missing video or PCM audio stream headers")

video_scale, video_rate, _video_start, video_length = struct.unpack_from("<IIII", data, video_header[2] + 20)
if video_scale != 1 or video_rate != 60 or video_length != frame_count:
    raise SystemExit(
        "sustained AVI did not retain its requested 60 FPS stream "
        f"(scale={video_scale}, rate={video_rate}, length={video_length}, avih={frame_count})"
    )
duration = frame_count * video_scale / video_rate
if not 6.5 <= duration <= 9.0:
    raise SystemExit(
        f"sustained AVI duration {duration:.3f}s does not cover the seven-second capture window and injected hitch"
    )

indexed_video_frames = sum(
    1 for offset in range(idx1[2], idx1[3], 16)
    if data[offset:offset + 4] == b"00db"
)
if indexed_video_frames != frame_count:
    raise SystemExit(
        f"sustained AVI header/index disagree ({frame_count} header frames, {indexed_video_frames} indexed frames)"
    )

audio_scale, audio_rate, _audio_start, audio_length = struct.unpack_from("<IIII", data, audio_header[2] + 20)
audio_duration = audio_length * audio_scale / audio_rate if audio_rate else 0.0
if abs(audio_duration - duration) > 1.0 / video_rate:
    raise SystemExit(
        f"sustained AVI audio/video durations diverge ({audio_duration:.6f}s audio, {duration:.6f}s video)"
    )
print(f"validated sustained 60-FPS AVI: {frame_count} indexed frames, {duration:.3f}s A/V duration")
EOF

png_dir="${video_dir}"
avi_dir="${video_dir}"

music_dir="${test_root}/capture output/music"
run_engine music 0 "${music_dir}" 'PYTEST_VIDEO_MUSIC_DONE'
music_stdout="${last_stdout}"
grep -Fq 'PYTEST_VIDEO_MUSIC_DONE' "${music_stdout}" || fail 'music-only video driver did not complete'
grep -Fq 'Lossless PNG recording armed' "${music_stdout}" || fail 'music-only video driver did not start PNG recording'
grep -Fq 'Video recording audio:' "${music_stdout}" || fail 'music-only video driver did not write a WAV sidecar'

demo_dir="${test_root}/capture output/demo"
run_engine demo 0 "${demo_dir}" 'PYTEST_VIDEO_DEMO_DONE'
demo_stdout="${last_stdout}"
grep -Fq 'PYTEST_VIDEO_DEMO_DONE' "${demo_stdout}" || fail 'demo driver did not complete'
grep -Fq 'Demo ' "${demo_stdout}" || fail 'demo recorder did not report a saved file'
grep -Fq 'Resetting game speed to normal before demo recording.' "${demo_stdout}" || fail 'demo recorder did not normalize i_timescale before recording'

demo_path="${demo_dir}/DemoSmoke.lmp"
[[ -f "${demo_path}" ]] || fail 'demo recorder did not create the expected replay input'
playback_dir="${test_root}/demo playback"
run_engine playback 0 "${playback_dir}" 'PYTEST_VIDEO_DEMO_PLAYBACK_REACHED' "${demo_path}"
playback_stdout="${last_stdout}"
grep -Fq 'PYTEST_VIDEO_DEMO_PLAYBACK_REACHED' "${playback_stdout}" || fail 'demo playback driver did not reach its marker'
grep -Fq "Playing demo ${demo_path}" "${playback_stdout}" || fail 'generated demo did not enter playback'
grep -Eq '^(E[1-4]M[1-9]|MAP[0-9][0-9]) - ' "${playback_stdout}" || fail 'generated demo did not load its recorded map'
if grep -Eq 'Unable to (open|read) demo|Cannot play non-|Demo is mangled|Not a .* demo file|Demo has no |Could not decompress demo|Demo requires a newer version' "${playback_stdout}"; then
    fail 'generated demo hit a replay format or read error'
fi

python3 - "${png_dir}" "${avi_dir}" "${music_dir}" "${demo_dir}" "${png_family_sentinel}" "${png_timeline_sentinel}" "${png_audio_sentinel}" "${avi_family_sentinel}" "${validate_pixels}" <<'EOF'
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

png_dir, avi_dir, music_dir, demo_dir, png_sentinel, png_timeline_sentinel, png_audio_sentinel, avi_sentinel, validate_pixels = sys.argv[1:]
validate_pixels = validate_pixels == "1"


def assert_sentinel(path, expected):
    if not os.path.isfile(path) or open(path, "rb").read() != expected:
        raise SystemExit(f"stale capture-family sentinel was changed: {path}")


assert_sentinel(png_sentinel, b"BiasedDoom stale PNG family sentinel\n")
assert_sentinel(png_timeline_sentinel, b"BiasedDoom stale PNG timeline sentinel\n")
assert_sentinel(png_audio_sentinel, b"BiasedDoom stale PNG audio sentinel\n")
assert_sentinel(avi_sentinel, b"BiasedDoom stale AVI family sentinel\n")


def decode_png_rgb(path):
    """Return an 8-bit RGB PNG without relying on optional image packages."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"invalid PNG signature: {path}")

    cursor = 8
    ihdr = None
    compressed = bytearray()
    saw_iend = False
    while cursor + 12 <= len(data):
        size = struct.unpack_from(">I", data, cursor)[0]
        chunk_type = data[cursor + 4:cursor + 8]
        body_start = cursor + 8
        body_end = body_start + size
        if body_end + 4 > len(data):
            raise SystemExit(f"truncated PNG chunk while decoding pixels: {path}")
        body = data[body_start:body_end]
        if chunk_type == b"IHDR":
            if ihdr is not None or size != 13:
                raise SystemExit(f"invalid PNG IHDR while decoding pixels: {path}")
            ihdr = struct.unpack(">IIBBBBB", body)
        elif chunk_type == b"IDAT":
            compressed.extend(body)
        elif chunk_type == b"IEND":
            if size != 0:
                raise SystemExit(f"invalid PNG IEND while decoding pixels: {path}")
            saw_iend = True
            break
        cursor = body_end + 4

    if ihdr is None or not saw_iend:
        raise SystemExit(f"incomplete PNG while decoding pixels: {path}")
    width, height, bit_depth, color_type, compression, filtering, interlace = ihdr
    channels = {2: 3, 6: 4}.get(color_type)
    if (width <= 0 or height <= 0 or bit_depth != 8 or channels is None or
            compression != 0 or filtering != 0 or interlace != 0):
        raise SystemExit(
            "PNG visual validation supports only non-interlaced 8-bit RGB/RGBA output "
            f"(got {width}x{height}, depth={bit_depth}, color={color_type}): {path}"
        )
    try:
        filtered = zlib.decompress(compressed)
    except zlib.error as error:
        raise SystemExit(f"could not inflate PNG pixels: {path}: {error}") from error

    row_bytes = width * channels
    expected = height * (row_bytes + 1)
    if len(filtered) != expected:
        raise SystemExit(f"PNG scanline payload has {len(filtered)} bytes, expected {expected}: {path}")

    def paeth(left, above, upper_left):
        estimate = left + above - upper_left
        left_distance = abs(estimate - left)
        above_distance = abs(estimate - above)
        upper_left_distance = abs(estimate - upper_left)
        if left_distance <= above_distance and left_distance <= upper_left_distance:
            return left
        if above_distance <= upper_left_distance:
            return above
        return upper_left

    rgb = bytearray(width * height * 3)
    previous = bytearray(row_bytes)
    source_offset = 0
    destination_offset = 0
    for _row in range(height):
        filter_type = filtered[source_offset]
        source_offset += 1
        source = filtered[source_offset:source_offset + row_bytes]
        source_offset += row_bytes
        current = bytearray(row_bytes)
        for index, value in enumerate(source):
            left = current[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_type == 0:
                reconstructed = value
            elif filter_type == 1:
                reconstructed = value + left
            elif filter_type == 2:
                reconstructed = value + above
            elif filter_type == 3:
                reconstructed = value + ((left + above) >> 1)
            elif filter_type == 4:
                reconstructed = value + paeth(left, above, upper_left)
            else:
                raise SystemExit(f"unsupported PNG filter {filter_type} while decoding pixels: {path}")
            current[index] = reconstructed & 0xff

        if channels == 3:
            rgb[destination_offset:destination_offset + row_bytes] = current
            destination_offset += row_bytes
        else:
            for pixel in range(width):
                source_pixel = pixel * 4
                rgb[destination_offset:destination_offset + 3] = current[source_pixel:source_pixel + 3]
                destination_offset += 3
        previous = current

    return width, height, bytes(rgb)


def assert_nontrivial_rgb(label, rgb, width, height):
    """Reject an all-black/solid capture without assuming a particular map palette."""
    expected = width * height * 3
    if width <= 0 or height <= 0 or len(rgb) != expected:
        raise SystemExit(f"{label} has an invalid decoded RGB layout ({width}x{height}, {len(rgb)} bytes)")

    visible_pixels = 0
    low = 255
    high = 0
    color_bins = set()
    for offset in range(0, len(rgb), 3):
        red, green, blue = rgb[offset:offset + 3]
        maximum = max(red, green, blue)
        minimum = min(red, green, blue)
        if maximum > 8:
            visible_pixels += 1
        low = min(low, minimum)
        high = max(high, maximum)
        color_bins.add((red >> 4, green >> 4, blue >> 4))

    # A real map/status-bar capture has broad tonal and color variation. These
    # deliberately modest thresholds reject blank readbacks and a fabricated
    # solid color without incorrectly demanding a bright or specific scene.
    required_visible = max(256, width * height // 2000)
    if visible_pixels < required_visible or high - low < 16 or len(color_bins) < 8:
        raise SystemExit(
            f"{label} lacks nontrivial rendered pixels "
            f"(visible={visible_pixels}/{width * height}, range={high - low}, bins={len(color_bins)})"
        )


png_pattern = re.compile(r"^(VideoSmoke(?:_\d+)?)_frame(\d{6})\.png$")
png_groups = {}
base_family_pngs = []
for path in glob.glob(os.path.join(png_dir, "VideoSmoke*_frame*.png")):
    match = png_pattern.fullmatch(os.path.basename(path))
    if match is None:
        raise SystemExit(f"unexpected VideoSmoke PNG family member: {path}")
    family = match.group(1)
    if family == "VideoSmoke":
        base_family_pngs.append(os.path.abspath(path))
    else:
        png_groups.setdefault(family, []).append(path)

if sorted(base_family_pngs) != [os.path.abspath(png_sentinel)]:
    raise SystemExit("the requested PNG family was used instead of being reserved")
if len(png_groups) != 1:
    raise SystemExit(f"expected one alternate PNG family, found {sorted(png_groups)}")
png_family, frames = next(iter(png_groups.items()))
frames.sort()
if len(frames) < 24:
    raise SystemExit(
        f"expected at least 24 PNG frames at 10 FPS over the half-speed wall-clock interval, found {len(frames)}"
    )
for path in frames:
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise SystemExit(f"invalid PNG frame: {path}")
    width, height = struct.unpack(">II", data[16:24])
    if width <= 0 or height <= 0:
        raise SystemExit(f"invalid PNG dimensions: {path}")
    cursor = 8
    gamma = None
    saw_iend = False
    while cursor + 12 <= len(data):
        size = struct.unpack_from(">I", data, cursor)[0]
        chunk_type = data[cursor + 4:cursor + 8]
        chunk_start = cursor + 8
        chunk_end = chunk_start + size
        if chunk_end + 4 > len(data):
            raise SystemExit(f"truncated PNG chunk in {path}")
        if chunk_type == b"gAMA":
            if size != 4:
                raise SystemExit(f"invalid PNG gAMA chunk in {path}")
            gamma = struct.unpack_from(">I", data, chunk_start)[0]
            stored_crc = struct.unpack_from(">I", data, chunk_end)[0]
            actual_crc = zlib.crc32(data[cursor + 4:chunk_end]) & 0xffffffff
            if stored_crc != actual_crc:
                raise SystemExit(f"PNG gAMA CRC mismatch in {path}")
        if chunk_type == b"IEND":
            saw_iend = True
            break
        cursor = chunk_end + 4
    if not saw_iend or gamma is None or gamma == 0:
        raise SystemExit(f"PNG frame is missing valid gamma metadata: {path}")

# Generic Xvfb/llvmpipe OpenGL can return a black final drawable even for the
# engine's unchanged synchronous screenshot path, so this expensive visual
# assertion is enabled only after an actual Vulkan device was confirmed above.
# Check start, middle, and tail rather than one convenient still: all three
# must contain a real rendered scene and not merely valid PNG structure.
if validate_pixels:
    for frame_index in sorted({0, len(frames) // 2, len(frames) - 1}):
        visual_width, visual_height, visual_rgb = decode_png_rgb(frames[frame_index])
        assert_nontrivial_rgb(
            f"PNG frame {os.path.basename(frames[frame_index])}",
            visual_rgb,
            visual_width,
            visual_height,
        )

# ffconcat is the PNG sequence's timing contract. It must list every frame in
# capture order with a positive duration, then repeat the final still image so
# FFmpeg's concat demuxer applies that final duration as well.
timeline_pattern = re.compile(r"^(VideoSmoke(?:_\d+)?)\.ffconcat$")
alternate_timelines = []
base_timelines = []
for path in glob.glob(os.path.join(png_dir, "VideoSmoke*.ffconcat")):
    match = timeline_pattern.fullmatch(os.path.basename(path))
    if match is None:
        raise SystemExit(f"unexpected VideoSmoke timing sidecar: {path}")
    family = match.group(1)
    if family == "VideoSmoke":
        base_timelines.append(os.path.abspath(path))
    else:
        alternate_timelines.append((family, path))
if sorted(base_timelines) != [os.path.abspath(png_timeline_sentinel)]:
    raise SystemExit("the requested PNG timing sidecar was changed instead of being reserved")
if len(alternate_timelines) != 1 or alternate_timelines[0][0] != png_family:
    raise SystemExit(f"expected one {png_family} PNG timing sidecar, found {alternate_timelines}")
timeline_path = alternate_timelines[0][1]
timeline_lines = open(timeline_path, encoding="utf-8").read().splitlines()
if not timeline_lines or timeline_lines[0] != "ffconcat version 1.0":
    raise SystemExit("PNG timing sidecar is not an ffconcat 1.0 file")
timeline_entries = []
line_index = 1
while line_index < len(timeline_lines):
    match = re.fullmatch(r"file '([^']+)'", timeline_lines[line_index])
    if match is None:
        raise SystemExit(f"malformed ffconcat file entry: {timeline_lines[line_index]!r}")
    filename = match.group(1)
    duration = None
    line_index += 1
    if line_index < len(timeline_lines) and timeline_lines[line_index].startswith("duration "):
        try:
            duration = float(timeline_lines[line_index][len("duration "):])
        except ValueError as error:
            raise SystemExit(f"malformed ffconcat duration: {timeline_lines[line_index]!r}") from error
        if duration <= 0.0:
            raise SystemExit(f"non-positive ffconcat duration for {filename!r}")
        line_index += 1
    timeline_entries.append((filename, duration))
if len(timeline_entries) != len(frames) + 1:
    raise SystemExit(
        f"ffconcat has {len(timeline_entries)} file entries for {len(frames)} PNG frames"
    )
expected_frame_names = [os.path.basename(path) for path in frames]
if [entry[0] for entry in timeline_entries[:-1]] != expected_frame_names:
    raise SystemExit("ffconcat frame order does not match the PNG sequence")
if timeline_entries[-1] != (expected_frame_names[-1], None):
    raise SystemExit("ffconcat does not repeat the final PNG without a second duration")
if any(entry[1] is None for entry in timeline_entries[:-1]):
    raise SystemExit("ffconcat is missing a duration for a captured PNG frame")
timeline_duration = sum(entry[1] for entry in timeline_entries[:-1])
if timeline_duration <= 0.0:
    raise SystemExit("ffconcat did not describe a positive capture duration")
if timeline_duration <= 3.0:
    raise SystemExit(
        f"ffconcat lost the half-speed wall-clock interval under bounded capture loss: {timeline_duration:.3f}s"
    )

# PNG recordings carry the same engine PCM timeline as a standalone WAV. The
# base family is deliberately occupied above, so prove selector coverage for
# the sidecar as well as the frames/timing file before validating its exact
# native PCM layout and audible probe samples.
wav_pattern = re.compile(r"^(VideoSmoke(?:_\d+)?)_audio\.wav$")
alternate_wavs = []
base_wavs = []
for path in glob.glob(os.path.join(png_dir, "VideoSmoke*_audio.wav")):
    match = wav_pattern.fullmatch(os.path.basename(path))
    if match is None:
        raise SystemExit(f"unexpected VideoSmoke WAV sidecar: {path}")
    family = match.group(1)
    if family == "VideoSmoke":
        base_wavs.append(os.path.abspath(path))
    else:
        alternate_wavs.append((family, path))
if sorted(base_wavs) != [os.path.abspath(png_audio_sentinel)]:
    raise SystemExit("the requested PNG audio sidecar was changed instead of being reserved")
if len(alternate_wavs) != 1 or alternate_wavs[0][0] != png_family:
    raise SystemExit(f"expected one {png_family} WAV sidecar, found {alternate_wavs}")
wav_path = alternate_wavs[0][1]


def parse_pcm_wav(path):
    wav_data = open(path, "rb").read()
    if len(wav_data) < 12 or wav_data[:4] != b"RIFF" or wav_data[8:12] != b"WAVE":
        raise SystemExit(f"missing RIFF/WAVE header: {path}")
    if struct.unpack_from("<I", wav_data, 4)[0] + 8 != len(wav_data):
        raise SystemExit(f"WAV RIFF size mismatch: {path}")
    cursor = 12
    fmt = None
    pcm_data = None
    while cursor < len(wav_data):
        if cursor + 8 > len(wav_data):
            raise SystemExit(f"truncated WAV chunk header: {path}")
        fourcc = wav_data[cursor:cursor + 4]
        size = struct.unpack_from("<I", wav_data, cursor + 4)[0]
        body_start = cursor + 8
        body_end = body_start + size
        if body_end > len(wav_data):
            raise SystemExit(f"truncated {fourcc!r} WAV chunk: {path}")
        if fourcc == b"fmt ":
            if fmt is not None or size != 16:
                raise SystemExit(f"WAV has an invalid PCM fmt chunk: {path}")
            fmt = struct.unpack_from("<HHIIHH", wav_data, body_start)
        elif fourcc == b"data":
            if pcm_data is not None:
                raise SystemExit(f"WAV has more than one data chunk: {path}")
            pcm_data = wav_data[body_start:body_end]
        cursor = body_end + (size & 1)
    if cursor != len(wav_data) or fmt is None or pcm_data is None:
        raise SystemExit(f"WAV is missing a complete fmt/data layout: {path}")
    audio_format, channels, sample_rate, byte_rate, block_align, bits_per_sample = fmt
    if (audio_format, channels, sample_rate, byte_rate, block_align, bits_per_sample) != (1, 2, 48000, 192000, 4, 16):
        raise SystemExit(
            "WAV is not native 48 kHz stereo 16-bit PCM "
            f"(fmt={(audio_format, channels, sample_rate, byte_rate, block_align, bits_per_sample)})"
        )
    if len(pcm_data) == 0 or len(pcm_data) % block_align:
        raise SystemExit(f"WAV PCM data has an invalid block alignment: {path}")
    if not any(pcm_data):
        raise SystemExit(f"WAV PCM data is silent despite the known pistol sound probe: {path}")
    return len(pcm_data) // block_align, pcm_data


wav_frames, wav_pcm = parse_pcm_wav(wav_path)
wav_duration = wav_frames / 48000.0
if wav_duration <= 3.0:
    raise SystemExit(f"WAV sidecar lost the half-speed capture interval: {wav_duration:.3f}s")
if abs(wav_duration - timeline_duration) > 1.0:
    raise SystemExit(
        "WAV sidecar duration does not remain synchronized with its PNG timing sidecar "
        f"({wav_duration:.3f}s vs {timeline_duration:.3f}s)"
    )

# Effects are muted in this isolated take, so its non-silent PCM proves the
# OpenAL stream tap includes active map music rather than merely a triggered
# sound effect. Keep it in its own output family so the collision test above
# remains focused on the immediate PNG/AVI restart path.
music_wavs = sorted(glob.glob(os.path.join(music_dir, "VideoSmoke*_audio.wav")))
if len(music_wavs) != 1:
    raise SystemExit(f"expected exactly one music-only WAV sidecar, found {music_wavs}")
music_frames, music_pcm = parse_pcm_wav(music_wavs[0])
music_duration = music_frames / 48000.0
if music_duration <= 3.0:
    raise SystemExit(f"music-only WAV sidecar is too short: {music_duration:.3f}s")
if not any(music_pcm):
    raise SystemExit("music-only WAV sidecar is silent with effects muted")

avi_base_pattern = re.compile(r"^(VideoSmoke(?:_\d+)?)\.avi$")
avi_part_pattern = re.compile(r"^(VideoSmoke(?:_\d+)?)_part\d+\.avi$")
alternate_avis = []
base_family_avi_parts = []
for path in glob.glob(os.path.join(avi_dir, "VideoSmoke*.avi")):
    basename = os.path.basename(path)
    base_match = avi_base_pattern.fullmatch(basename)
    part_match = avi_part_pattern.fullmatch(basename)
    if base_match is not None:
        family = base_match.group(1)
        if family == "VideoSmoke":
            raise SystemExit("the requested AVI base family was used instead of being reserved")
        alternate_avis.append((family, path))
    elif part_match is not None and part_match.group(1) == "VideoSmoke":
        base_family_avi_parts.append(os.path.abspath(path))
    elif part_match is None:
        raise SystemExit(f"unexpected VideoSmoke AVI family member: {path}")

if sorted(base_family_avi_parts) != [os.path.abspath(avi_sentinel)]:
    raise SystemExit("the requested AVI family part was changed instead of being reserved")
if len(alternate_avis) != 1:
    raise SystemExit(f"expected one alternate AVI output, found {alternate_avis}")
avi_family, avi_path = alternate_avis[0]
if avi_family != png_family:
    raise SystemExit(f"PNG and AVI selected different alternate families: {png_family}, {avi_family}")
data = open(avi_path, "rb").read()
if data[:4] != b"RIFF" or data[8:12] != b"AVI ":
    raise SystemExit("missing RIFF/AVI header")
riff_size = struct.unpack_from("<I", data, 4)[0]
riff_end = riff_size + 8
if riff_end != len(data):
    raise SystemExit(f"AVI RIFF size mismatch: header says {riff_end}, file has {len(data)}")


def chunks(begin, end):
    """Yield (fourcc, chunk_start, body_start, body_end) from a RIFF scope."""
    cursor = begin
    while cursor < end:
        if cursor + 8 > end:
            raise SystemExit("truncated RIFF chunk header")
        fourcc = data[cursor:cursor + 4]
        size = struct.unpack_from("<I", data, cursor + 4)[0]
        body_start = cursor + 8
        body_end = body_start + size
        if body_end > end:
            raise SystemExit(f"truncated {fourcc!r} RIFF chunk")
        yield fourcc, cursor, body_start, body_end
        cursor = body_end + (size & 1)
    if cursor != end:
        raise SystemExit("malformed RIFF chunk padding")


root_chunks = list(chunks(12, riff_end))


def list_body(name):
    for fourcc, _start, body_start, body_end in root_chunks:
        if fourcc == b"LIST" and body_start + 4 <= body_end and data[body_start:body_start + 4] == name:
            return body_start, body_end
    raise SystemExit(f"AVI is missing LIST {name!r}")


hdrl_type, hdrl_end = list_body(b"hdrl")
hdrl_chunks = list(chunks(hdrl_type + 4, hdrl_end))
avih_chunks = [chunk for chunk in hdrl_chunks if chunk[0] == b"avih"]
if len(avih_chunks) != 1 or avih_chunks[0][3] - avih_chunks[0][2] != 56:
    raise SystemExit("AVI is missing a valid avih header")
_fourcc, _start, avih_start, _end = avih_chunks[0]
microseconds_per_frame, max_bytes_per_second = struct.unpack_from("<II", data, avih_start)
frame_count = struct.unpack_from("<I", data, avih_start + 16)[0]
stream_count = struct.unpack_from("<I", data, avih_start + 24)[0]
if microseconds_per_frame == 0:
    raise SystemExit("AVI avih reports a zero frame duration")
if max_bytes_per_second == 0:
    raise SystemExit("AVI avih reports a zero maximum byte rate")
if frame_count < 2:
    raise SystemExit(f"AVI avih reports only {frame_count} frame")
if stream_count != 2:
    raise SystemExit(f"AVI avih must declare one video and one PCM audio stream, found {stream_count}")

# Locate the video stream by type rather than assuming it is the only strl.
# AVI audio support may add a PCM stream alongside it, but the cadence contract
# belongs to the `vids` header and its 00db index members.
video_strl_chunks = None
for fourcc, _start, body_start, body_end in hdrl_chunks:
    if fourcc != b"LIST" or data[body_start:body_start + 4] != b"strl":
        continue
    candidate = list(chunks(body_start + 4, body_end))
    headers = [chunk for chunk in candidate if chunk[0] == b"strh"]
    if len(headers) == 1 and headers[0][3] - headers[0][2] == 56 and data[headers[0][2]:headers[0][2] + 4] == b"vids":
        if video_strl_chunks is not None:
            raise SystemExit("AVI has more than one video stream")
        video_strl_chunks = candidate
if video_strl_chunks is None:
    raise SystemExit("AVI is missing a video stream list")
audio_strl_chunks = None
for fourcc, _start, body_start, body_end in hdrl_chunks:
    if fourcc != b"LIST" or data[body_start:body_start + 4] != b"strl":
        continue
    candidate = list(chunks(body_start + 4, body_end))
    headers = [chunk for chunk in candidate if chunk[0] == b"strh"]
    if len(headers) == 1 and headers[0][3] - headers[0][2] == 56 and data[headers[0][2]:headers[0][2] + 4] == b"auds":
        if audio_strl_chunks is not None:
            raise SystemExit("AVI has more than one audio stream")
        audio_strl_chunks = candidate
if audio_strl_chunks is None:
    raise SystemExit("AVI is missing a PCM audio stream list")
strl_chunks = video_strl_chunks
strh_chunks = [chunk for chunk in strl_chunks if chunk[0] == b"strh"]
_fourcc, _start, strh_start, _end = strh_chunks[0]
stream_scale, stream_rate, stream_start, stream_length = struct.unpack_from("<IIII", data, strh_start + 20)
if stream_scale != 1 or stream_rate == 0:
    raise SystemExit(
        "AVI stream timing must retain a nonzero selected FPS "
        f"(scale={stream_scale}, rate={stream_rate}, avih={microseconds_per_frame})"
    )
if stream_rate != 10:
    raise SystemExit(f"AVI stream rate changed from the configured 10 FPS (rate={stream_rate})")
expected_microseconds_per_frame = (1_000_000 + stream_rate // 2) // stream_rate
if microseconds_per_frame != expected_microseconds_per_frame:
    raise SystemExit(
        "AVI avih timing does not match its selected stream FPS "
        f"(avih={microseconds_per_frame}, rate={stream_rate})"
    )
if stream_start != 0 or stream_length != frame_count:
    raise SystemExit("AVI stream length/start does not match avih frame count")
strf_chunks = [chunk for chunk in strl_chunks if chunk[0] == b"strf"]
if len(strf_chunks) != 1 or strf_chunks[0][3] - strf_chunks[0][2] != 40:
    raise SystemExit("AVI is missing a valid BITMAPINFOHEADER")
_fourcc, _start, strf_start, _end = strf_chunks[0]
header_size, width, signed_height, planes, bit_count, compression, image_size = struct.unpack_from("<IiiHHII", data, strf_start)
if header_size != 40 or width <= 0 or signed_height == 0 or planes != 1 or bit_count != 24 or compression != 0:
    raise SystemExit("AVI stream is not uncompressed 24-bit RGB")
height = abs(signed_height)
stride = (width * 3 + 3) & ~3
if image_size != stride * height:
    raise SystemExit("AVI BITMAPINFOHEADER has an invalid image size")
expected_max_bytes_per_second = min(0xffffffff, image_size * stream_rate // stream_scale)
audio_bytes_per_second = 48000 * 2 * 2
expected_max_bytes_per_second = min(0xffffffff, expected_max_bytes_per_second + audio_bytes_per_second)
if max_bytes_per_second != expected_max_bytes_per_second:
    raise SystemExit(
        "AVI avih maximum byte rate does not include its selected fixed video rate and PCM audio rate"
    )
avi_duration = frame_count * stream_scale / stream_rate
if avi_duration <= 0.0:
    raise SystemExit("AVI finalized timing does not describe a positive duration")
if avi_duration <= 1.5:
    raise SystemExit(f"AVI lost its normal-speed capture interval: {avi_duration:.3f}s")

# AVISTREAMHEADER's audio scale and sample size are one stereo PCM frame, so
# dwLength can be compared directly with indexed 01wb payload bytes below.
audio_strh_chunks = [chunk for chunk in audio_strl_chunks if chunk[0] == b"strh"]
if len(audio_strh_chunks) != 1 or audio_strh_chunks[0][3] - audio_strh_chunks[0][2] != 56:
    raise SystemExit("AVI is missing a valid audio AVISTREAMHEADER")
_fourcc, _start, audio_strh_start, _end = audio_strh_chunks[0]
if data[audio_strh_start:audio_strh_start + 4] != b"auds":
    raise SystemExit("AVI audio stream does not have the auds type")
audio_scale, audio_rate, audio_start, audio_length = struct.unpack_from("<IIII", data, audio_strh_start + 20)
audio_sample_size = struct.unpack_from("<I", data, audio_strh_start + 44)[0]
if (audio_scale, audio_rate, audio_start, audio_sample_size) != (4, 192000, 0, 4):
    raise SystemExit(
        "AVI audio stream timing/sample layout is not 48 kHz stereo PCM "
        f"(scale={audio_scale}, rate={audio_rate}, start={audio_start}, sample_size={audio_sample_size})"
    )
audio_strf_chunks = [chunk for chunk in audio_strl_chunks if chunk[0] == b"strf"]
if len(audio_strf_chunks) != 1 or audio_strf_chunks[0][3] - audio_strf_chunks[0][2] != 16:
    raise SystemExit("AVI is missing a valid PCM WAVEFORMATEX")
_fourcc, _start, audio_strf_start, _end = audio_strf_chunks[0]
audio_format = struct.unpack_from("<HHIIHH", data, audio_strf_start)
if audio_format != (1, 2, 48000, 192000, 4, 16):
    raise SystemExit(f"AVI audio strf is not native 48 kHz stereo PCM: {audio_format}")

movi_type, movi_end = list_body(b"movi")
idx1_chunks = [chunk for chunk in root_chunks if chunk[0] == b"idx1"]
if len(idx1_chunks) != 1:
    raise SystemExit("AVI is missing its idx1 index")
_fourcc, _start, idx1_start, idx1_end = idx1_chunks[0]
if (idx1_end - idx1_start) % 16:
    raise SystemExit("AVI idx1 length is not a whole number of entries")

indexed_frames = []
indexed_audio = []
indexed_audio_bytes = 0
indexed_audio_non_silent = False
for entry_offset in range(idx1_start, idx1_end, 16):
    chunk_id, flags, relative_offset, indexed_size = struct.unpack_from("<4sIII", data, entry_offset)
    # AVI idx1 offsets are relative to the `movi` list type. The first `00db`
    # therefore begins at relative offset four, not zero. Verify every entry
    # resolves precisely to its referenced media chunk.
    chunk_offset = movi_type + relative_offset
    if chunk_offset + 8 > movi_end or data[chunk_offset:chunk_offset + 4] != chunk_id:
        raise SystemExit(f"AVI idx1 offset {relative_offset} does not address its media chunk")
    actual_size = struct.unpack_from("<I", data, chunk_offset + 4)[0]
    if actual_size != indexed_size or chunk_offset + 8 + actual_size > movi_end:
        raise SystemExit("AVI idx1 size does not match its media chunk")
    if chunk_id == b"00db":
        if flags != 0x10:
            raise SystemExit(f"AVI video idx1 entry is not a key frame (flags={flags:#x})")
        if actual_size != image_size:
            raise SystemExit("AVI video idx1 size does not match the DIB image size")
        indexed_frames.append((chunk_offset, actual_size))
    elif chunk_id == b"01wb":
        if flags != 0:
            raise SystemExit(f"AVI audio idx1 entry has unexpected flags={flags:#x}")
        if actual_size == 0 or actual_size % 4:
            raise SystemExit("AVI audio idx1 payload is not whole 16-bit stereo PCM frames")
        payload = data[chunk_offset + 8:chunk_offset + 8 + actual_size]
        indexed_audio_non_silent = indexed_audio_non_silent or any(payload)
        indexed_audio.append((chunk_offset, actual_size))
        indexed_audio_bytes += actual_size
    else:
        raise SystemExit(f"unexpected AVI idx1 media chunk {chunk_id!r}")

if len(indexed_frames) != frame_count:
    raise SystemExit(f"AVI avih has {frame_count} frames but idx1 has {len(indexed_frames)}")
if indexed_frames[0][0] - movi_type != 4:
    raise SystemExit("AVI first idx1 frame is not offset four bytes from movi")
if not indexed_audio:
    raise SystemExit("AVI has no indexed PCM audio chunks")
if indexed_audio_bytes != audio_length * 4:
    raise SystemExit(
        "AVI audio stream length does not match its indexed PCM payload "
        f"({audio_length} frames vs {indexed_audio_bytes} bytes)"
    )
if not indexed_audio_non_silent:
    raise SystemExit("AVI PCM audio is silent despite the known pistol sound probe")
avi_audio_duration = audio_length * audio_scale / audio_rate
if abs(avi_audio_duration - avi_duration) > max(1.0 / stream_rate, 1.0 / 48000.0):
    raise SystemExit(
        "AVI audio and video durations are not synchronized "
        f"({avi_audio_duration:.6f}s vs {avi_duration:.6f}s)"
    )

# Decode the first BI_RGB frame ourselves. AVI stores positive-height DIBs
# bottom-up in BGR order; a valid top-down RGB result proves the index, stride,
# channel order, and image bounds work together rather than merely finding
# marker strings in the file.
frame_offset, frame_size = indexed_frames[0]
raw_bgr = data[frame_offset + 8:frame_offset + 8 + frame_size]
decoded_rgb = bytearray(width * height * 3)
for stored_row in range(height):
    output_row = height - stored_row - 1 if signed_height > 0 else stored_row
    source = stored_row * stride
    destination = output_row * width * 3
    for x in range(width):
        b, g, r = raw_bgr[source + x * 3:source + x * 3 + 3]
        decoded_rgb[destination + x * 3:destination + x * 3 + 3] = bytes((r, g, b))

if validate_pixels:
    assert_nontrivial_rgb("AVI first decoded RGB frame", bytes(decoded_rgb), width, height)

# When available, use an independent decoder as the final compatibility gate.
# The structural/raw decoder above keeps this repository test portable when a
# minimal CI image lacks ffmpeg.
ffmpeg = shutil.which("ffmpeg")
if ffmpeg:
    decoded = subprocess.run(
        [ffmpeg, "-v", "error", "-i", avi_path, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if decoded.returncode != 0:
        raise SystemExit(f"ffmpeg could not decode the AVI: {decoded.stderr.decode(errors='replace')}")
    if decoded.stdout != bytes(decoded_rgb):
        raise SystemExit("ffmpeg RGB decode differs from the indexed BI_RGB frame")
    decoded_audio = subprocess.run(
        [ffmpeg, "-v", "error", "-i", avi_path, "-map", "0:a:0", "-f", "s16le", "-ac", "2", "-ar", "48000", "-"],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if decoded_audio.returncode != 0:
        raise SystemExit(f"ffmpeg could not decode the AVI audio: {decoded_audio.stderr.decode(errors='replace')}")
    if len(decoded_audio.stdout) != indexed_audio_bytes or not any(decoded_audio.stdout):
        raise SystemExit("ffmpeg AVI audio decode is absent, truncated, or silent")
    decoder_note = "ffmpeg decoded video and PCM audio"
else:
    decoder_note = "native BI_RGB/PCM index validation (ffmpeg unavailable)"

demos = sorted(glob.glob(os.path.join(demo_dir, "DemoSmoke*.lmp")))
if not demos:
    raise SystemExit("no demo output found")
data = open(demos[0], "rb").read()
if not data.startswith(b"FORM") or b"ZDEM" not in data or b"ZDHD" not in data or b"BODY" not in data:
    raise SystemExit("demo is not a complete ZDEM IFF stream")

visual_note = "; Vulkan decoded-pixel proof passed for sampled PNG frames and AVI" if validate_pixels else ""
print(
    f"validated {len(frames)} PNG frames with {os.path.basename(timeline_path)} "
    f"and {os.path.basename(wav_path)} ({timeline_duration:.3f}s video, {wav_duration:.3f}s non-silent 48 kHz PCM), "
    f"{os.path.basename(avi_path)} ({frame_count} exact video idx1 frames; {len(indexed_audio)} PCM idx1 chunks; "
    f"{avi_duration:.3f}s fixed selected-rate A/V timing; {decoder_note}), "
    f"and {os.path.basename(demos[0])}{visual_note}"
)
EOF

printf 'PASS: timed lossless PNG/WAV audio, real-time-paced RGB AVI/PCM audio, and configured demo recording verified\n'
