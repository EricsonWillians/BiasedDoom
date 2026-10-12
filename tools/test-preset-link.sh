#!/usr/bin/env bash
# Preset layer semantics test: verifies the linked/unlinked preset model,
# the explicit "diverge to Custom" rule, and the resetrenderpresets CCMD.
#
# Model under test:
#   - Linked (bd_preset_locked on): a graphics preset always drives the
#     lighting/fog selectors to its paired values; manually changing a
#     lighting/fog preset afterwards drops the graphics selector to Custom.
#   - Unlinked (off): graphics preset changes never touch the other layers.
#   - resetrenderpresets: one click back to Vanilla (graphics 1, lighting 1,
#     fog 1).
#
# Runs the engine under xvfb by default (or the engine's null-video driver
# with --headless) with a fresh config. It first asserts the shipped Vanilla
# defaults, then uses a Python driver to switch presets and query selectors.
set -euo pipefail

usage() {
    cat <<'USAGE'
Verify preset layer linking and the vanilla reset in the real engine.

Usage:
  ./tools/test-preset-link.sh --iwad PATH [options]

Options:
  --iwad PATH       IWAD to run (required, e.g. doom2.wad)
  --exe PATH        executable for the test (default: build/biaseddoom)
  --timeout SEC     maximum seconds for the engine process (default: 120)
  --headless        use BiasedDoom's null-video driver instead of Xvfb
  --keep-temp       retain the driver PK3, config, and logs
  -h, --help        show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path=""
test_timeout=120
headless=0
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
        --headless)
            headless=1
            shift
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

display_runner=()
video_args=()
if [[ "${headless}" -eq 1 ]]; then
    video_args=(-headless)
elif command -v xvfb-run >/dev/null 2>&1; then
    display_runner=(xvfb-run -a -s "-screen 0 1280x800x24")
elif [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'error: no display available and xvfb-run is missing\n' >&2
    exit 2
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-preset-link-test.XXXXXX")"
cleanup() {
    if [[ "${keep_temp}" -eq 0 ]]; then
        rm -rf "${test_root}"
    else
        printf 'keeping test artifacts in %s\n' "${test_root}"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Generate the Python driver PK3
# ---------------------------------------------------------------------------

driver_src="${test_root}/driver"
mkdir -p "${driver_src}/pyscripts"

cat > "${driver_src}/PYTHON" <<'EOF'
# One UTF-8 VFS path per line. Paths are resolved inside this PK3 only.
pyscripts/main.py
EOF

cat > "${driver_src}/pyscripts/main.py" <<'EOF'
"""Preset layer semantics test driver."""

import biaseddoom as bd

SELECTORS = ("bd_graphics_preset", "bd_lighting_preset", "bd_fog_preset")
CAMERA_DEFAULTS = (
    "chase_enabled",
    "chasedemo",
    "cam_experimental_mode",
    "r_deathcamera",
)
VANILLA_DEFAULTS = (
    "bd_postfx_enable",
    "bd_postfx_quality",
    "bd_bloom_enable",
    "bd_vignette_enable",
    "bd_vibrance_enable",
    "bd_whitebalance_enable",
    "bd_grade_enable",
    "bd_hueshift_enable",
    "bd_posterize_enable",
    "bd_edgeglow_enable",
    "bd_godrays_enable",
    "bd_lensflare_enable",
    "bd_clarity_enable",
    "bd_chromatic_enable",
    "bd_filmgrain_enable",
    "bd_sharpen_enable",
    "bd_retro_pixel_enable",
    "bd_vhs_enable",
    "bd_vhs_panic_enable",
    "bd_colorgrade_mode",
    "bd_colorgrade_lut",
    "gl_crt_mode",
    "gl_tonemap",
    "gl_atmosphere",
    "gl_fxaa",
    "gl_ssao",
    "bd_fog_mode",
    "bd_fog_sky_strength",
    "bd_fog_thick_distance",
    "bd_fog_gradient_mode",
    "bd_fog_gradient_strength",
    "bd_fog_gradient_scale",
    "bd_fog_quality",
    "bd_fog_height_falloff",
    "bd_fog_turbulence",
)


def query_all():
    for cvar in SELECTORS:
        bd.execute(cvar)


def step_check_defaults():
    for cvar in SELECTORS + CAMERA_DEFAULTS + VANILLA_DEFAULTS:
        bd.execute(cvar)
    bd.execute("echo MARK_DEFAULTS")


def step_link_on():
    bd.execute("bd_preset_locked true")
    bd.execute("bd_graphics_preset 65")


def step_check_paired():
    query_all()
    bd.execute("echo MARK_PAIRED")


def step_new_pair():
    bd.execute("bd_graphics_preset 100")


def step_check_new_pair():
    query_all()
    bd.execute("echo MARK_NEW_PAIR")


def step_diverge():
    bd.execute("bd_fog_preset 3")


def step_check_diverged():
    query_all()
    bd.execute("echo MARK_DIVERGED")


def step_unlink():
    bd.execute("bd_preset_locked false")
    bd.execute("bd_lighting_preset 5")
    bd.execute("bd_fog_preset 9")
    bd.execute("bd_graphics_preset 4")


def step_check_unlinked():
    query_all()
    bd.execute("echo MARK_UNLINKED")


def step_reset():
    bd.execute("resetrenderpresets")


def step_check_reset():
    query_all()
    bd.execute("echo MARK_RESET")
    bd.execute("echo PYTEST PRESETLINK_DONE; wait 5; quit")


@bd.on("map_load")
def on_map_load(event):
    bd.schedule(step_check_defaults, delay=5)
    bd.schedule(step_link_on, delay=15)
    bd.schedule(step_check_paired, delay=25)
    bd.schedule(step_new_pair, delay=35)
    bd.schedule(step_check_new_pair, delay=45)
    bd.schedule(step_diverge, delay=55)
    bd.schedule(step_check_diverged, delay=65)
    bd.schedule(step_unlink, delay=75)
    bd.schedule(step_check_unlinked, delay=85)
    bd.schedule(step_reset, delay=95)
    bd.schedule(step_check_reset, delay=105)
EOF

driver_pk3="${test_root}/presetlink_test.pk3"
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

stdout_file="${test_root}/presetlink.stdout"
log_file="${test_root}/presetlink.log"

set +e
timeout --signal=INT --kill-after=5s "${test_timeout}s" \
    "${display_runner[@]}" "${engine_exe}" \
    "${video_args[@]}" -stdout -nosound -nointro -python \
    -config "${test_root}/presetlink.ini" \
    -iwad "${iwad_path}" -file "${driver_pk3}" \
    +vid_activeinbackground true +i_pauseinbackground false \
    +vid_defwidth 640 +vid_defheight 480 +vid_fullscreen false \
    +logfile "${log_file}" +map map01 \
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
grep -Fq 'PYTEST PRESETLINK_DONE' "${stdout_file}" || fail "driver did not complete"

# ---------------------------------------------------------------------------
# Fresh configuration: all rendering layers must start on their Vanilla
# presets. The graphics pipeline remains available, but every visual effect
# (including all engine-added fog) is disabled.
# ---------------------------------------------------------------------------

awk '/MARK_DEFAULTS/{exit} /"bd_graphics_preset" is "1"/{g=1} /"bd_lighting_preset" is "1"/{l=1} /"bd_fog_preset" is "1"/{f=1} /"bd_postfx_enable" is "true"/{p=1} /"bd_postfx_quality" is "1"/{q=1} /"bd_bloom_enable" is "false"/{b=1} /"bd_vignette_enable" is "false"/{v=1} /"bd_vibrance_enable" is "false"/{i=1} /"bd_whitebalance_enable" is "false"/{w=1} /"bd_grade_enable" is "false"/{g2=1} /"bd_hueshift_enable" is "false"/{u=1} /"bd_posterize_enable" is "false"/{o=1} /"bd_edgeglow_enable" is "false"/{e=1} /"bd_godrays_enable" is "false"/{y=1} /"bd_lensflare_enable" is "false"/{l2=1} /"bd_clarity_enable" is "false"/{c2=1} /"bd_chromatic_enable" is "false"/{c=1} /"bd_filmgrain_enable" is "false"/{n=1} /"bd_sharpen_enable" is "false"/{s=1} /"bd_retro_pixel_enable" is "false"/{r=1} /"bd_vhs_enable" is "false"/{h=1} /"bd_vhs_panic_enable" is "false"/{x=1} /"bd_colorgrade_mode" is "0"/{d=1} /"bd_colorgrade_lut" is "0"/{l3=1} /"gl_crt_mode" is "0"/{j=1} /"gl_tonemap" is "0"/{t=1} /"gl_atmosphere" is "0"/{a=1} /"gl_fxaa" is "0"/{f2=1} /"gl_ssao" is "0"/{s2=1} /"bd_fog_mode" is "0"/{m=1} /"bd_fog_sky_strength" is "0"/{k=1} /"bd_fog_thick_distance" is "0"/{i2=1} /"bd_fog_gradient_mode" is "0"/{g3=1} /"bd_fog_gradient_strength" is "0"/{g4=1} /"bd_fog_gradient_scale" is "1"/{g5=1} /"bd_fog_quality" is "0"/{z=1} /"bd_fog_height_falloff" is "0"/{h2=1} /"bd_fog_turbulence" is "0"/{u2=1} END{exit !(g&&l&&f&&p&&q&&b&&v&&i&&w&&g2&&u&&o&&e&&y&&l2&&c2&&c&&n&&s&&r&&h&&x&&d&&l3&&j&&t&&a&&f2&&s2&&m&&k&&i2&&g3&&g4&&g5&&z&&h2&&u2)}' "${stdout_file}" \
    || fail "fresh config should start with the complete Vanilla rendering configuration"

awk '/MARK_DEFAULTS/{exit} /"chase_enabled" is "false"/{chase=1} /"chasedemo" is "false"/{demo=1} /"cam_experimental_mode" is "0"/{experimental=1} /"r_deathcamera" is "false"/{death=1} END{exit !(chase&&demo&&experimental&&death)}' "${stdout_file}" \
    || fail "fresh config should start with the normal first-person camera"

# ---------------------------------------------------------------------------
# Linked: graphics preset 65 (Absolution) must drive lighting 40 and fog 18.
# (Each step's queries print before that step's echo marker.)
# ---------------------------------------------------------------------------

awk '/MARK_PAIRED/{exit} /"bd_graphics_preset" is "65"/{g=1} /"bd_lighting_preset" is "40"/{l=1} /"bd_fog_preset" is "18"/{f=1} END{exit !(g&&l&&f)}' "${stdout_file}" \
    || fail "linked: graphics preset 65 should pair lighting 40 and fog 18"

# New append-only graphics presets must participate in the same linked model.
awk '/MARK_PAIRED/{m=1} /MARK_NEW_PAIR/{m=0} m && /"bd_graphics_preset" is "100"/{g=1} m && /"bd_lighting_preset" is "75"/{l=1} m && /"bd_fog_preset" is "53"/{f=1} END{exit !(g&&l&&f)}' "${stdout_file}" \
    || fail "linked: graphics preset 100 should pair lighting 75 and fog 53"

# Linked + manual fog change: the graphics selector must drop to Custom.
awk '/MARK_NEW_PAIR/{m=1} /MARK_DIVERGED/{m=0} m && /"bd_graphics_preset" is "0"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "linked: manual fog preset change should drop graphics preset to Custom"
awk '/MARK_NEW_PAIR/{m=1} /MARK_DIVERGED/{m=0} m && /"bd_fog_preset" is "3"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "linked: manual fog preset 3 should stick"

# ---------------------------------------------------------------------------
# Unlinked: graphics preset 4 must not touch the lighting/fog selectors
# (lighting 5, fog 9 set manually beforehand).
# ---------------------------------------------------------------------------

awk '/MARK_DIVERGED/{m=1} /MARK_UNLINKED/{m=0} m && /"bd_graphics_preset" is "4"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "unlinked: graphics preset 4 should be selected"
awk '/MARK_DIVERGED/{m=1} /MARK_UNLINKED/{m=0} m && /"bd_lighting_preset" is "5"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "unlinked: lighting preset must stay at the manual choice"
awk '/MARK_DIVERGED/{m=1} /MARK_UNLINKED/{m=0} m && /"bd_fog_preset" is "9"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "unlinked: fog preset must stay at the manual choice"

# ---------------------------------------------------------------------------
# resetrenderpresets: graphics 1, lighting 1, fog 1 (Vanilla everywhere).
# ---------------------------------------------------------------------------

awk '/MARK_UNLINKED/{m=1} /MARK_RESET/{m=0} m && /"bd_graphics_preset" is "1"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "reset: graphics preset should be 1 (Vanilla+)"
awk '/MARK_UNLINKED/{m=1} /MARK_RESET/{m=0} m && /"bd_lighting_preset" is "1"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "reset: lighting preset should be 1 (Classic Balanced)"
awk '/MARK_UNLINKED/{m=1} /MARK_RESET/{m=0} m && /"bd_fog_preset" is "1"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "reset: fog preset should be 1 (Disabled)"

printf 'PASS: vanilla defaults, preset linking, diverge-to-custom, and vanilla reset verified\n'
