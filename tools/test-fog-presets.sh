#!/usr/bin/env bash
# Fog preset horizon coherence test: for every fog preset, verifies that the
# physical sky-fog veil keeps the horizon seamless on an open map.
#
# Runs the engine under xvfb with a generated UDMF open-plain map (sky
# ceiling, MAPINFO fog) and a Python driver that applies each fog preset
# 2..17 in turn, capturing one screenshot with the preset active and one
# control shot with the sky veil disabled (bd_fog_sky_strength 0).
#
# Per preset, tools/analyze_sky_fog.py measures the horizon seam (mean color
# delta between tight bands just above and below the horizon) and asserts:
#   1. seam with the preset active < 22 (veil keeps the horizon coherent),
#   2. seam with the veil off is at least 8 units larger (the veil is what
#      keeps it coherent — the metric can still see the bug).
# A metrics table is printed for review; --keep-temp retains the full
# screenshot contact sheet.
set -euo pipefail

usage() {
    cat <<'USAGE'
Verify horizon coherence for every fog preset in the real engine.

Usage:
  ./tools/test-fog-presets.sh --iwad PATH [options]

Options:
  --iwad PATH       IWAD to run (required, e.g. doom2.wad)
  --exe PATH        executable for the test (default: build/biaseddoom)
  --timeout SEC     maximum seconds for the engine process (default: 300)
  --keep-temp       retain the driver PK3, config, logs, and screenshots
  -h, --help        show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path=""
test_timeout=300
keep_temp=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iwad)
            [[ $# -ge 2 ]] || { printf 'error: --iwad requires a path\n' >&2; exit 2; }
            iwad_path="$2"
            shift 2
            ;;
        --exe)
            [[ $# -ge 2 ]] || { printf 'error: --exe requires a path\n' >&2; exit 2; }
            engine_exe="$2"
            shift 2
            ;;
        --timeout)
            [[ $# -ge 2 ]] || { printf 'error: --timeout requires seconds\n' >&2; exit 2; }
            test_timeout="$2"
            shift 2
            ;;
        --keep-temp)
            keep_temp=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'error: unknown option %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ -n "${iwad_path}" ]] || { printf 'error: --iwad is required\n' >&2; usage >&2; exit 2; }
[[ -f "${iwad_path}" ]] || { printf 'error: IWAD not found: %s\n' "${iwad_path}" >&2; exit 2; }
[[ -x "${engine_exe}" ]] || { printf 'error: executable not found: %s\n' "${engine_exe}" >&2; exit 2; }
[[ "${test_timeout}" =~ ^[1-9][0-9]*$ ]] || {
    printf 'error: --timeout must be a positive integer\n' >&2; exit 2
}
command -v timeout >/dev/null 2>&1 || { printf 'error: GNU timeout is required\n' >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { printf 'error: python3 is required\n' >&2; exit 2; }

# Private X server: on a shared desktop the window's focus/visibility races
# the Vulkan swapchain readback and screenshots can silently drop.
display_runner=()
if command -v xvfb-run >/dev/null 2>&1; then
    display_runner=(xvfb-run -a -s "-screen 0 1280x800x24")
elif [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'error: no display available and xvfb-run is missing\n' >&2
    exit 2
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-fogpresets-test.XXXXXX")"
cleanup() {
    if [[ "${keep_temp}" -eq 0 ]]; then
        rm -rf "${test_root}"
    else
        printf 'keeping test artifacts in %s\n' "${test_root}"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Generate the driver PK3 (open-plain fog map + Python driver)
# ---------------------------------------------------------------------------

driver_src="${test_root}/driver"
mkdir -p "${driver_src}/pyscripts" "${driver_src}/maps"

cat > "${driver_src}/MAPINFO" <<'EOF'
map TESTFOG "Sky Fog Test"
{
    sky1 = "SKY1"
    fade = "c8 c8 be"
    fogdensity = 200
}
EOF

cat > "${driver_src}/PYTHON" <<'EOF'
# One UTF-8 VFS path per line. Paths are resolved inside this PK3 only.
pyscripts/main.py
EOF

cat > "${driver_src}/pyscripts/main.py" <<'EOF'
"""Fog preset horizon coherence driver.

Applies each fog preset 2..17 on the open TESTFOG plain, capturing one
screenshot per preset with the sky veil as tuned and one control with
bd_fog_sky_strength 0.
"""

import os

import biaseddoom as bd

OUT_DIR = os.environ.get("FOGPRESET_TEST_OUT", "/tmp")
PRESETS = list(range(2, 18))


def shot(name):
    bd.execute(f"screenshot {os.path.join(OUT_DIR, name)}.png")


def step_setup():
    # Fullscreen world view (no status bar) so the horizon sits exactly at
    # the frame's vertical center for the seam metric.
    bd.execute("screenblocks 11")
    bd.execute("bd_aerial_strength 0")


def make_on_step(preset):
    def step():
        bd.execute(f"bd_fog_preset {preset}")
        bd.execute(f"echo PRESET_{preset}_ON")
    return step


def make_on_shot(preset):
    def step():
        shot(f"preset_{preset:02d}_on")
    return step


def make_off_step(preset):
    def step():
        bd.execute("bd_fog_sky_strength 0")
    return step


def make_off_shot(preset):
    def step():
        shot(f"preset_{preset:02d}_off")
    return step


def step_done():
    bd.execute("echo PYTEST FOGPRESETS_SHOTS_DONE; wait 5; quit")


@bd.on("map_load")
def on_map_load(event):
    bd.schedule(step_setup, delay=5)
    tic = 40
    for preset in PRESETS:
        bd.schedule(make_on_step(preset), delay=tic)
        bd.schedule(make_on_shot(preset), delay=tic + 30)
        bd.schedule(make_off_step(preset), delay=tic + 40)
        bd.schedule(make_off_shot(preset), delay=tic + 70)
        tic += 90
    bd.schedule(step_done, delay=tic + 10)
EOF

# UDMF test map: an 8192x8192 plain (sky ceiling) inside a low outer ring
# (ceiling 128, also sky), so the horizon sits at the frame's vertical center.
# Linedef winding keeps every front side facing the map interior.
python3 - "${driver_src}" <<'PYEOF'
import os
import struct
import sys

src = sys.argv[1]

textmap = """namespace = "zdoom";

thing
{
    x = 0.0;
    y = 0.0;
    angle = 0;
    type = 1;
    skill1 = true;
    skill2 = true;
    skill3 = true;
    skill4 = true;
    skill5 = true;
    skill6 = true;
    skill7 = true;
    skill8 = true;
    single = true;
    coop = true;
    dm = true;
}

vertex { x = -4096.0; y = -4096.0; }  // 0..3: inner square
vertex { x = 4096.0; y = -4096.0; }
vertex { x = 4096.0; y = 4096.0; }
vertex { x = -4096.0; y = 4096.0; }
vertex { x = -4352.0; y = -4352.0; }  // 4..7: outer square
vertex { x = 4352.0; y = -4352.0; }
vertex { x = 4352.0; y = 4352.0; }
vertex { x = -4352.0; y = 4352.0; }

sector  // 0: open plain, sky ceiling
{
    heightfloor = 0;
    heightceiling = 1024;
    lightlevel = 255;
    texturefloor = "FLOOR4_8";
    textureceiling = "F_SKY1";
}

sector  // 1: outer ring with a low sky ceiling -> 128-tall horizon wall band
{
    heightfloor = 0;
    heightceiling = 128;
    lightlevel = 255;
    texturefloor = "FLOOR4_8";
    textureceiling = "F_SKY1";
}

sidedef { sector = 0; }  // 0: inner side (sky above the low ring, no textures)
sidedef { sector = 1; }  // 1: ring side of the inner boundary
sidedef                  // 2: outer solid wall
{
    sector = 1;
    texturemiddle = "BROWN96";
}

linedef { v1 = 1; v2 = 0; sidefront = 0; sideback = 1; twosided = true; }
linedef { v1 = 2; v2 = 1; sidefront = 0; sideback = 1; twosided = true; }
linedef { v1 = 3; v2 = 2; sidefront = 0; sideback = 1; twosided = true; }
linedef { v1 = 0; v2 = 3; sidefront = 0; sideback = 1; twosided = true; }
linedef { v1 = 5; v2 = 4; sidefront = 2; }
linedef { v1 = 6; v2 = 5; sidefront = 2; }
linedef { v1 = 7; v2 = 6; sidefront = 2; }
linedef { v1 = 4; v2 = 7; sidefront = 2; }
"""

lumps = [
    ("TESTFOG", b""),
    ("TEXTMAP", textmap.encode("utf-8")),
    ("ENDMAP", b""),
]

payload = b""
directory = b""
offset = 12
for name, data in lumps:
    payload += data
    directory += struct.pack("<II8s", offset, len(data), name.encode("ascii").ljust(8, b"\0"))
    offset += len(data)

wad = b"PWAD" + struct.pack("<II", len(lumps), offset) + payload + directory
with open(os.path.join(src, "maps", "testfog.wad"), "wb") as fh:
    fh.write(wad)
PYEOF

driver_pk3="${test_root}/fogpresets_test.pk3"
python3 - "${driver_src}" "${driver_pk3}" <<'EOF'
import os
import sys
import zipfile

src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as pk3:
    for root, _dirs, files in os.walk(src):
        for name in files:
            full = os.path.join(root, name)
            pk3.write(full, os.path.relpath(full, src))
EOF

# ---------------------------------------------------------------------------
# Run the engine
# ---------------------------------------------------------------------------

stdout_file="${test_root}/fogpresets.stdout"
log_file="${test_root}/fogpresets.log"
export FOGPRESET_TEST_OUT="${test_root}"

set +e
timeout --signal=INT --kill-after=5s "${test_timeout}s" \
    "${display_runner[@]}" "${engine_exe}" \
    -stdout -nosound -nointro -python \
    -config "${test_root}/fogpresets.ini" \
    -iwad "${iwad_path}" -file "${driver_pk3}" \
    +vid_activeinbackground true +i_pauseinbackground false \
    +vid_defwidth 640 +vid_defheight 480 +vid_fullscreen false \
    +logfile "${log_file}" +map testfog \
    >"${stdout_file}" 2>&1
status=$?
set -e

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    printf '%s\n' '--- engine output tail ---' >&2
    tail -n 60 "${stdout_file}" >&2 || true
    exit 1
}

[[ "${status}" -eq 0 ]] || fail "engine exited with status ${status}"
grep -Fq 'PYTEST FOGPRESETS_SHOTS_DONE' "${stdout_file}" || fail "driver did not complete"

for preset in $(seq 2 17); do
    on_shot=$(printf 'preset_%02d_on.png' "${preset}")
    off_shot=$(printf 'preset_%02d_off.png' "${preset}")
    [[ -f "${test_root}/${on_shot}" ]] || fail "missing screenshot ${on_shot}"
    [[ -f "${test_root}/${off_shot}" ]] || fail "missing screenshot ${off_shot}"
done

# ---------------------------------------------------------------------------
# Horizon seam metrics per preset
# ---------------------------------------------------------------------------

analyze="${repo_root}/tools/analyze_sky_fog.py"

# Junction seam: sky band vs. wall band across the sky/geometry boundary, with
# the boundary row detected on the control (veil-off) shot of the same scene.
junction_seam_of() {  # junction_seam_of <image> <control>
    python3 "${analyze}" "$1" --boundary "$2" | sed -n 's/.* seam=\([0-9.]*\)$/\1/p'
}

printf '%-8s %14s %14s\n' "preset" "junction(veil)" "junction(raw)"
failures=0
for preset in $(seq 2 17); do
    on_shot=$(printf 'preset_%02d_on.png' "${preset}")
    off_shot=$(printf 'preset_%02d_off.png' "${preset}")
    seam_on="$(junction_seam_of "${test_root}/${on_shot}" "${test_root}/${off_shot}")"
    seam_off="$(junction_seam_of "${test_root}/${off_shot}" "${test_root}/${off_shot}")"
    [[ -n "${seam_on}" && -n "${seam_off}" ]] || fail "could not compute junction seams for preset ${preset}"
    printf '%-8d %14s %14s\n' "${preset}" "${seam_on}" "${seam_off}"

    # 1. The veiled sky meets the fogged geometry at the horizon seamlessly.
    awk -v s="${seam_on}" 'BEGIN { exit !(s < 15.0) }' || {
        printf 'FAIL: preset %d horizon junction seam too large (%s)\n' "${preset}" "${seam_on}" >&2
        failures=1
    }
    # 2. The veil is what keeps it seamless: raw sky must read measurably worse.
    awk -v on="${seam_on}" -v off="${seam_off}" 'BEGIN { exit !(off > on + 8.0) }' || {
        printf 'FAIL: preset %d veil does not measurably improve the horizon (%s vs raw %s)\n' \
            "${preset}" "${seam_on}" "${seam_off}" >&2
        failures=1
    }
done

[[ "${failures}" -eq 0 ]] || exit 1
printf 'PASS: all fog presets keep the horizon seamless with a working sky veil\n'
