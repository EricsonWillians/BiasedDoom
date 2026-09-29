#!/usr/bin/env bash
# Sky-fog horizon continuity test: verifies that the sky veil physically
# matches the fog applied to level geometry, so open maps no longer show a
# hard contrast line at the horizon.
#
# Runs the engine (xvfb when no display is available) with a generated test
# PK3 containing:
#   - a tiny UDMF map: a flat open plain under a sky ceiling, ringed by a low
#     wall, with MAPINFO-authored fog (fade + fogdensity, skyfog=0),
#   - a Python driver that captures four screenshots at a level horizon:
#       global_strength1 / global_strength0  (biased global fog, veil on/off)
#       map_strength1    / map_strength0     (map-authored fog, veil on/off)
#
# tools/analyze_sky_fog.py then measures the horizon seam (mean color delta
# between the band just above and just below the horizon) and asserts:
#   1. with bd_fog_sky_strength 1 the seam is small (sky matches geometry),
#   2. with bd_fog_sky_strength 0 the seam is large (the metric can see the
#      original bug),
#   3. the veil shrinks the seam by at least a factor of two.
set -euo pipefail

usage() {
    cat <<'USAGE'
Verify sky-fog horizon continuity in the real engine.

Usage:
  ./tools/test-sky-fog.sh --iwad PATH [options]

Options:
  --iwad PATH       IWAD to run (required, e.g. doom2.wad)
  --exe PATH        executable for the test (default: build/biaseddoom)
  --timeout SEC     maximum seconds for the engine process (default: 120)
  --keep-temp       retain the driver PK3, config, logs, and screenshots
  -h, --help        show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path=""
test_timeout=120
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

# Prefer a private X server: the test pops a game window, and on a shared
# desktop the focus/visibility of that window is racy (Vulkan swapchain
# readback can silently drop screenshots when the window is backgrounded).
# xvfb keeps the run hermetic and does not disturb the user's session.
display_runner=()
if command -v xvfb-run >/dev/null 2>&1; then
    display_runner=(xvfb-run -a -s "-screen 0 1280x800x24")
elif [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'error: no display available and xvfb-run is missing\n' >&2
    exit 2
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-skyfog-test.XXXXXX")"
cleanup() {
    if [[ "${keep_temp}" -eq 0 ]]; then
        rm -rf "${test_root}"
    else
        printf 'keeping test artifacts in %s\n' "${test_root}"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Generate the driver PK3 (test map + MAPINFO + Python driver)
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
"""Sky-fog horizon test driver.

Captures four screenshots of a level horizon on the flat TESTFOG plain:
biased global fog and map-authored fog, each with the sky veil physically
matched (bd_fog_sky_strength 1) and disabled (0).
"""

import os

import biaseddoom as bd

OUT_DIR = os.environ.get("SKYFOG_TEST_OUT", "/tmp")


def shot(name):
    bd.execute(f"screenshot {os.path.join(OUT_DIR, name)}.png")


def step_setup():
    # Deterministic atmosphere: no turbulence, no gradient, no aerial haze,
    # no fog wall; the map's own fade color doubles as the biased fog color.
    # Fullscreen world view (no status bar) so the horizon sits exactly at
    # the frame's vertical center for the seam metric.
    bd.execute("screenblocks 11")
    bd.execute("bd_fog_turbulence 0")
    bd.execute("bd_fog_gradient_mode 0")
    bd.execute("bd_fog_thick_distance 0")
    bd.execute("bd_aerial_strength 0")
    bd.execute("bd_fog_height_falloff 0.72")
    bd.execute("bd_fog_min_visibility 0.05")
    bd.execute("bd_fog_quality 2")


def step_global_on():
    bd.execute("bd_fog_mode 1")
    bd.execute("bd_fog_density 155")
    bd.execute("bd_fog_color_mode 1")
    bd.execute('bd_fog_color "c8 c8 be"')
    bd.execute("bd_fog_sky_strength 1")
    shot("global_strength1")


def step_global_off():
    bd.execute("bd_fog_sky_strength 0")
    shot("global_strength0")


def step_map_on():
    # Map-authored fog only: the veil must still match the geometry fog.
    bd.execute("bd_fog_mode 0")
    bd.execute("bd_fog_sky_strength 1")
    shot("map_strength1")


def step_map_off():
    bd.execute("bd_fog_sky_strength 0")
    shot("map_strength0")
    bd.execute("echo PYTEST SKYFOG_SHOTS_DONE; wait 5; quit")


@bd.on("map_load")
def on_map_load(event):
    bd.schedule(step_setup, delay=5)
    bd.schedule(step_global_on, delay=40)
    bd.schedule(step_global_off, delay=80)
    bd.schedule(step_map_on, delay=120)
    bd.schedule(step_map_off, delay=160)
EOF

# UDMF test map: an 8192x8192 plain (sky ceiling) inside a low outer ring
# (ceiling 128, also sky), so the horizon sits at the frame's vertical center.
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

driver_pk3="${test_root}/skyfog_test.pk3"
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

stdout_file="${test_root}/skyfog.stdout"
log_file="${test_root}/skyfog.log"
export SKYFOG_TEST_OUT="${test_root}"

set +e
timeout --signal=INT --kill-after=5s "${test_timeout}s" \
    "${display_runner[@]}" "${engine_exe}" \
    -stdout -nosound -nointro -python \
    -config "${test_root}/skyfog.ini" \
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
grep -Fq 'PYTEST SKYFOG_SHOTS_DONE' "${stdout_file}" || fail "driver did not complete"

for shot in global_strength1 global_strength0 map_strength1 map_strength0; do
    [[ -f "${test_root}/${shot}.png" ]] || fail "missing screenshot ${shot}.png"
done

# ---------------------------------------------------------------------------
# Horizon seam metrics
# ---------------------------------------------------------------------------
#
# The physically consistent veil must (a) converge the sky toward the fog
# color at the horizon, (b) leave the horizon seamless, and (c) do strictly
# better than the disabled veil, which must (d) remain clearly detectable
# (otherwise the metric could not see the original bug).
# The map's fog color is fixed by the driver/MAPINFO to c8c8be = (200,200,190).

analyze="${repo_root}/tools/analyze_sky_fog.py"

band_of() {  # band_of <shot> <above|below> -> "r g b"
    python3 "${analyze}" "${test_root}/$1.png" \
        | sed -n "s/.* $2=(\([0-9.]*\),\([0-9.]*\),\([0-9.]*\)).*/\1 \2 \3/p"
}

fog_delta() {  # mean per-channel |band - fogcolor|
    awk -v r="$1" -v g="$2" -v b="$3" 'BEGIN {
        dr = r - 200.0; if (dr < 0) dr = -dr;
        dg = g - 200.0; if (dg < 0) dg = -dg;
        db = b - 190.0; if (db < 0) db = -db;
        printf "%.2f", (dr + dg + db) / 3.0 }'
}

band_delta() {  # mean per-channel |a - b|
    awk -v r1="$1" -v g1="$2" -v b1="$3" -v r2="$4" -v g2="$5" -v b2="$6" 'BEGIN {
        dr = r1 - r2; if (dr < 0) dr = -dr;
        dg = g1 - g2; if (dg < 0) dg = -dg;
        db = b1 - b2; if (db < 0) db = -db;
        printf "%.2f", (dr + dg + db) / 3.0 }'
}

for mode in global map; do
    above_on=( $(band_of "${mode}_strength1" above) )
    below_on=( $(band_of "${mode}_strength1" below) )
    above_off=( $(band_of "${mode}_strength0" above) )
    below_off=( $(band_of "${mode}_strength0" below) )
    [[ "${#above_on[@]}" -eq 3 && "${#below_on[@]}" -eq 3 && "${#above_off[@]}" -eq 3 && "${#below_off[@]}" -eq 3 ]] \
        || fail "could not parse metric bands for ${mode} mode"

    fog_on="$(fog_delta "${above_on[@]}")"
    fog_off="$(fog_delta "${above_off[@]}")"
    seam_on="$(band_delta "${above_on[@]}" "${below_on[@]}")"
    seam_off="$(band_delta "${above_off[@]}" "${below_off[@]}")"
    printf 'SKYFOG CHECK %s: fog_delta on=%s off=%s | horizon seam on=%s off=%s\n' \
        "${mode}" "${fog_on}" "${fog_off}" "${seam_on}" "${seam_off}"

    # (a) with the veil at full strength the sky converges to the fog color
    awk -v d="${fog_on}" 'BEGIN { exit !(d < 30.0) }' \
        || fail "${mode}: veiled sky did not converge to the fog color (delta ${fog_on})"
    # (b) the horizon is seamless
    awk -v s="${seam_on}" 'BEGIN { exit !(s < 20.0) }' \
        || fail "${mode}: horizon seam too large with veil at full strength (${seam_on})"
    # (c) the veil strictly improves fog-color convergence
    awk -v on="${fog_on}" -v off="${fog_off}" 'BEGIN { exit !(on * 1.0 < off) }' \
        || fail "${mode}: veil did not improve fog-color convergence (${fog_on} vs ${fog_off})"
    # (d) sanity: with the veil off the metric clearly sees the unfogged sky
    awk -v d="${fog_off}" 'BEGIN { exit !(d > 40.0) }' \
        || fail "${mode}: unfogged sky unexpectedly close to fog color (delta ${fog_off})"
done

printf 'PASS: sky fog physically matches geometry fog at the horizon (global and map fog)\n'
