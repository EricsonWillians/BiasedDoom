#!/usr/bin/env bash
# Exercise native lossless video and ZDEM recording through a rendered window.
# Headless mode is deliberately unsupported: it has no final framebuffer.
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: ./tools/test-video-recording.sh --iwad PATH [--exe PATH] [--timeout SEC] [--keep-temp]

Runs PNG-sequence, RGB-AVI, and configured-demo recording checks through a
private, non-headless OpenGL session. Headless mode is deliberately not
supported.
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path=""
test_timeout=75
keep_temp=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iwad) iwad_path="${2:?--iwad requires a path}"; shift 2 ;;
        --exe) engine_exe="${2:?--exe requires a path}"; shift 2 ;;
        --timeout) test_timeout="${2:?--timeout requires seconds}"; shift 2 ;;
        --keep-temp) keep_temp=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown option %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ -n "${iwad_path}" && -f "${iwad_path}" ]] || { printf 'error: a valid --iwad is required\n' >&2; exit 2; }
[[ -x "${engine_exe}" ]] || { printf 'error: executable not found: %s\n' "${engine_exe}" >&2; exit 2; }
[[ "${test_timeout}" =~ ^[1-9][0-9]*$ ]] || { printf 'error: --timeout must be a positive integer\n' >&2; exit 2; }
command -v timeout >/dev/null || { printf 'error: timeout is required\n' >&2; exit 2; }
command -v python3 >/dev/null || { printf 'error: python3 is required\n' >&2; exit 2; }
command -v setsid >/dev/null || { printf 'error: setsid is required\n' >&2; exit 2; }

# These rows are the user-facing path to the commands exercised below. Check
# both the source definition and the packaged definition so a stale PK3 cannot
# silently ship an engine whose capture UI is incomplete.
menu_source="${repo_root}/wadsrc/static/menudef.txt"
menu_package="$(dirname "${engine_exe}")/biaseddoom.pk3"
[[ -f "${menu_source}" ]] || { printf 'error: MENUDEF source not found: %s\n' "${menu_source}" >&2; exit 2; }
[[ -f "${menu_package}" ]] || { printf 'error: packaged MENUDEF not found: %s\n' "${menu_package}" >&2; exit 2; }
python3 - "${menu_source}" "${menu_package}" "${repo_root}" <<'EOF'
import os
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
)
required_controls = (
    'Control "Toggle video recording", "togglevideorecording"',
    'Control "Toggle demo recording", "toggledemorecording"',
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
    for row in required_controls:
        if row not in controls_block:
            raise SystemExit(f"{label} MENUDEF is missing recording control: {row}")

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

print("validated source and packaged MENUDEF capture fields, toggle controls, and transient-screen shortcut routes")
EOF

# Capture needs a composed hardware frame. Prefer a private X server even on
# desktop machines: it avoids background-window readback races and, coupled
# with vid_preferbackend 0 below, avoids a fresh config selecting Vulkan on an
# SDL backend that cannot create a Vulkan surface.
display_runner=()
if command -v xvfb-run >/dev/null 2>&1; then
    display_runner=(xvfb-run -a -s "-screen 0 1280x800x24")
elif [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'error: a display or xvfb-run is required for video capture\n' >&2
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
    local pid status completed=0
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
    elif [[ "${format}" == "both" ]]; then
        initial_format=0
        # Exercise both formats in one rendered session. The recorder closes
        # synchronously on stop, so this also covers a PNG-to-AVI restart
        # without paying a second map-load cost in CI. Video sampling is based
        # on wall time after a composited frame is available, not game tics.
        # Give a fresh Doom I renderer a full two seconds to settle, then
        # record PNG at half game speed. The 70-tic capture interval spans
        # roughly four wall-clock seconds at 0.5x, so the frame-count check
        # below proves cadence is not coupled to scaled game time.
        cat > "${command_file}" <<'EOF'
wait 70; i_timescale 0.5; vid_record_format 0; startvideorecording; wait 70; stopvideorecording; i_timescale 1; vid_record_format 1; startvideorecording; wait 70; stopvideorecording; echo PYTEST_VIDEO_CAPTURE_DONE; quit
EOF
    else
        cat > "${command_file}" <<'EOF'
wait 45; startvideorecording; wait 50; stopvideorecording; echo PYTEST_VIDEO_CAPTURE_DONE; quit
EOF
    fi

    # `quit` has already flushed a capture by the time its explicit marker is
    # printed. Some SDL backends can nevertheless linger during teardown, so
    # execute each run in an isolated process group and end that post-export
    # tail as soon as the marker is observed. This keeps the three independent
    # formats from paying a full watchdog timeout each.
    setsid timeout --signal=INT --kill-after=5s "${test_timeout}s" \
        "${display_runner[@]}" "${engine_exe}" \
        -stdout -nosound -nomusic -nointro -noautoload -noautoexec \
        -config "${test_root}/${mode}-${format}.ini" \
        -iwad "${iwad_path}" \
        -width 640 -height 480 +vid_fullscreen false +vid_preferbackend 0 \
        +vid_activeinbackground true +i_pauseinbackground false \
        +capture_export_dir "${output_dir}" \
        +vid_record_name VideoSmoke +vid_record_format "${initial_format}" +vid_record_fps 10 \
        +demo_record_name DemoSmoke +demo_record_map '*' -warp 1 +exec "${command_file}" \
        >"${stdout_file}" 2>&1 < /dev/null &
    pid=$!

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
        # Give `quit` a brief chance to reap its wrapper. `kill -0` remains
        # true for a zombie, so inspect ps state before signaling the private
        # group; otherwise a cleanly completed run produces a noisy, spurious
        # "Killed" notification from Bash.
        process_state=""
        for ((tick = 0; tick < 5; ++tick)); do
            process_state="$(ps -o stat= -p "${pid}" 2>/dev/null | tr -d '[:space:]' || true)"
            [[ -z "${process_state}" || "${process_state}" == Z* ]] && break
            sleep 0.1
        done
        if [[ -n "${process_state}" && "${process_state}" != Z* ]] && kill -0 "${pid}" 2>/dev/null; then
            # The marker is emitted only after stop* has synchronously closed
            # its artifact, so only a still-live backend teardown is stopped.
            kill -KILL -- "-${pid}" 2>/dev/null || true
        fi
    fi

    set +e
    wait "${pid}" 2>/dev/null
    status=$?
    set -e
    [[ "${completed}" -eq 1 ]] || fail "${mode}/${format} engine run ended before ${success_pattern} (status ${status})"
}

video_dir="${test_root}/capture output/video"
# Reserve only later members of the requested family. Neither base file exists,
# so this catches a selector that checks just VideoSmoke.png/VideoSmoke.avi and
# would otherwise overwrite or mix an incomplete prior recording.
png_family_sentinel="${video_dir}/VideoSmoke_frame999999.png"
avi_family_sentinel="${video_dir}/VideoSmoke_part002.avi"
mkdir -p "${video_dir}"
printf 'BiasedDoom stale PNG family sentinel\n' > "${png_family_sentinel}"
printf 'BiasedDoom stale AVI family sentinel\n' > "${avi_family_sentinel}"
[[ ! -e "${video_dir}/VideoSmoke.png" && ! -e "${video_dir}/VideoSmoke.avi" ]] || fail 'capture family sentinels unexpectedly created a base file'
run_engine video both "${video_dir}" 'PYTEST_VIDEO_CAPTURE_DONE'
video_stdout="${last_stdout}"
grep -Fq 'PYTEST_VIDEO_CAPTURE_DONE' "${video_stdout}" || fail 'video driver did not complete'
grep -Fq 'Lossless PNG recording armed' "${video_stdout}" || fail 'video driver did not start PNG recording'
grep -Fq 'Lossless RGB AVI recording armed' "${video_stdout}" || fail 'video driver did not start AVI recording'

png_dir="${video_dir}"
avi_dir="${video_dir}"

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

python3 - "${png_dir}" "${avi_dir}" "${demo_dir}" "${png_family_sentinel}" "${avi_family_sentinel}" <<'EOF'
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

png_dir, avi_dir, demo_dir, png_sentinel, avi_sentinel = sys.argv[1:]


def assert_sentinel(path, expected):
    if not os.path.isfile(path) or open(path, "rb").read() != expected:
        raise SystemExit(f"stale capture-family sentinel was changed: {path}")


assert_sentinel(png_sentinel, b"BiasedDoom stale PNG family sentinel\n")
assert_sentinel(avi_sentinel, b"BiasedDoom stale AVI family sentinel\n")

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
if len(frames) < 30:
    raise SystemExit(
        f"expected at least 30 PNG frames at 10 FPS over the half-speed wall-clock interval, found {len(frames)}"
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
frame_count = struct.unpack_from("<I", data, avih_start + 16)[0]
if frame_count == 0:
    raise SystemExit("AVI avih reports no frames")
if frame_count < 2:
    raise SystemExit(f"AVI avih reports only {frame_count} frame")

strl_type = strl_end = None
for fourcc, _start, body_start, body_end in hdrl_chunks:
    if fourcc == b"LIST" and data[body_start:body_start + 4] == b"strl":
        strl_type, strl_end = body_start, body_end
        break
if strl_type is None:
    raise SystemExit("AVI is missing a stream list")
strl_chunks = list(chunks(strl_type + 4, strl_end))
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

movi_type, movi_end = list_body(b"movi")
idx1_chunks = [chunk for chunk in root_chunks if chunk[0] == b"idx1"]
if len(idx1_chunks) != 1:
    raise SystemExit("AVI is missing its idx1 index")
_fourcc, _start, idx1_start, idx1_end = idx1_chunks[0]
if (idx1_end - idx1_start) % 16:
    raise SystemExit("AVI idx1 length is not a whole number of entries")

indexed_frames = []
for entry_offset in range(idx1_start, idx1_end, 16):
    chunk_id, flags, relative_offset, indexed_size = struct.unpack_from("<4sIII", data, entry_offset)
    if chunk_id != b"00db" or flags != 0x10:
        raise SystemExit(f"unexpected AVI idx1 entry {chunk_id!r} flags={flags:#x}")
    # AVI idx1 offsets are relative to the `movi` list type. The first `00db`
    # therefore begins at relative offset four, not zero. Verify every entry
    # resolves precisely to its referenced media chunk.
    chunk_offset = movi_type + relative_offset
    if chunk_offset + 8 > movi_end or data[chunk_offset:chunk_offset + 4] != b"00db":
        raise SystemExit(f"AVI idx1 offset {relative_offset} does not address a 00db chunk")
    actual_size = struct.unpack_from("<I", data, chunk_offset + 4)[0]
    if actual_size != indexed_size or actual_size != image_size or chunk_offset + 8 + actual_size > movi_end:
        raise SystemExit("AVI idx1 size does not match its media chunk")
    indexed_frames.append((chunk_offset, actual_size))

if len(indexed_frames) != frame_count:
    raise SystemExit(f"AVI avih has {frame_count} frames but idx1 has {len(indexed_frames)}")
if indexed_frames[0][0] - movi_type != 4:
    raise SystemExit("AVI first idx1 frame is not offset four bytes from movi")

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
    decoder_note = "ffmpeg decoded"
else:
    decoder_note = "native BI_RGB decoder validated (ffmpeg unavailable)"

demos = sorted(glob.glob(os.path.join(demo_dir, "DemoSmoke*.lmp")))
if not demos:
    raise SystemExit("no demo output found")
data = open(demos[0], "rb").read()
if not data.startswith(b"FORM") or b"ZDEM" not in data or b"ZDHD" not in data or b"BODY" not in data:
    raise SystemExit("demo is not a complete ZDEM IFF stream")

print(
    f"validated {len(frames)} PNG frames, {os.path.basename(avi_path)} "
    f"({frame_count} exact idx1 frames; {decoder_note}), and {os.path.basename(demos[0])}"
)
EOF

printf 'PASS: lossless PNG, lossless RGB AVI, and configured demo recording verified\n'
