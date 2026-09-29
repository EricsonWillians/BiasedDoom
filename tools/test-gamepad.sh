#!/usr/bin/env bash
# Gamepad support test: verifies the default gamepad bindings (classic
# layout, no vertical aiming), the gamepadlayout presets, the new cvars, and
# that the curated haptics definitions load cleanly.
#
# Runs the engine under xvfb with a fresh config and a Python driver that
# queries bindings/cvars through the console. No physical controller is
# required: this covers the configuration layer end to end.
set -euo pipefail

usage() {
    cat <<'USAGE'
Verify gamepad defaults, layout presets, and haptics content in the engine.

Usage:
  ./tools/test-gamepad.sh --iwad PATH [options]

Options:
  --iwad PATH       IWAD to run (required, e.g. doom2.wad)
  --exe PATH        executable for the test (default: build/biaseddoom)
  --timeout SEC     maximum seconds for the engine process (default: 120)
  --keep-temp       retain the driver PK3, config, and logs
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

# Private X server: on a shared desktop the window's focus/visibility is racy.
display_runner=()
if command -v xvfb-run >/dev/null 2>&1; then
    display_runner=(xvfb-run -a -s "-screen 0 1280x800x24")
elif [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'error: no display available and xvfb-run is missing\n' >&2
    exit 2
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-gamepad-test.XXXXXX")"
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
"""Gamepad configuration test driver.

Queries the default (classic) pad bindings, cycles the layout presets, and
checks the new cvars, all through console commands with deterministic
markers.
"""

import biaseddoom as bd


def step_defaults():
    for key in ("pad_a", "rtrigger", "ltrigger", "pad_b", "rthumb",
                "rstickup", "pad_y"):
        bd.execute(f"bind {key}")
    bd.execute("echo MARK_DEFAULTS")


def step_modern():
    bd.execute("gamepadlayout 3")
    for key in ("rstickup", "rstickdown", "pad_y"):
        bd.execute(f"bind {key}")
    bd.execute("freelook")
    bd.execute("bd_classic_autoaim")
    bd.execute("echo MARK_MODERN")


def step_move():
    bd.execute("gamepadlayout 2")
    for key in ("rstickup", "rstickdown", "pad_y"):
        bd.execute(f"bind {key}")
    bd.execute("freelook")
    bd.execute("bd_classic_autoaim")
    # sv_freelook is a server-flag mask cvar: changes land on the next net
    # update, so the value set by the previous preset is only visible now.
    bd.execute("sv_freelook")
    bd.execute("echo MARK_MOVE")


def step_classic():
    bd.execute("gamepadlayout 1")
    for key in ("rstickup", "pad_y"):
        bd.execute(f"bind {key}")
    bd.execute("joy_padlayout")
    bd.execute("joy_gyro_look")
    bd.execute("joy_gyro_sensitivity_yaw")
    bd.execute("haptics_do_damage")
    bd.execute("sv_freelook")
    bd.execute("echo MARK_CLASSIC")


def step_final():
    bd.execute("sv_freelook")
    bd.execute("echo MARK_FINAL")


def step_done():
    bd.execute("echo PYTEST GAMEPAD_DONE; wait 5; quit")


@bd.on("map_load")
def on_map_load(event):
    bd.schedule(step_defaults, delay=5)
    bd.schedule(step_modern, delay=20)
    bd.schedule(step_move, delay=35)
    bd.schedule(step_classic, delay=50)
    bd.schedule(step_final, delay=65)
    bd.schedule(step_done, delay=80)
EOF

driver_pk3="${test_root}/gamepad_test.pk3"
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

stdout_file="${test_root}/gamepad.stdout"
log_file="${test_root}/gamepad.log"

set +e
timeout --signal=INT --kill-after=5s "${test_timeout}s" \
    "${display_runner[@]}" "${engine_exe}" \
    -stdout -nosound -nointro -python \
    -config "${test_root}/gamepad.ini" \
    -iwad "${iwad_path}" -file "${driver_pk3}" \
    +vid_activeinbackground true +i_pauseinbackground false \
    +vid_defwidth 640 +vid_defheight 480 +vid_fullscreen false \
    +haptics_debug true \
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
grep -Fq 'PYTEST GAMEPAD_DONE' "${stdout_file}" || fail "driver did not complete"

# ---------------------------------------------------------------------------
# Default (classic) bindings
# ---------------------------------------------------------------------------

grep -Fq '"pad_a" = "+use"' "${stdout_file}"       || fail "default: pad_a not bound to +use"
grep -Fq '"rtrigger" = "+attack"' "${stdout_file}" || fail "default: rtrigger not bound to +attack"
grep -Fq '"ltrigger" = "+use"' "${stdout_file}"    || fail "default: ltrigger not bound to +use"
grep -Fq '"pad_b" = "+speed"' "${stdout_file}"     || fail "default: pad_b not bound to +speed"
grep -Fq '"rthumb" = "+centerview"' "${stdout_file}" || fail "default: rthumb not bound to +centerview"
# Classic means no vertical aiming and no jump out of the box.
grep -Fq '"rstickup" = ""' "${stdout_file}"        || fail "default: rstickup should be unbound (classic, no verticality)"
grep -Fq '"pad_y" = ""' "${stdout_file}"           || fail "default: pad_y should be unbound (classic, no jump)"

# ---------------------------------------------------------------------------
# Layout presets
# ---------------------------------------------------------------------------

# Modern: freelook on the right stick, jump on pad_y.
grep -Fq '"rstickup" = "+lookup"' "${stdout_file}"   || fail "modern: rstickup not bound to +lookup"
grep -Fq '"rstickdown" = "+lookdown"' "${stdout_file}" || fail "modern: rstickdown not bound to +lookdown"
grep -Fq '"pad_y" = "+jump"' "${stdout_file}"        || fail "modern: pad_y not bound to +jump"
# Classic + Move: right stick drives forward/back, pad_y returns to unbound.
grep -Fq '"rstickup" = "+forward"' "${stdout_file}"  || fail "move: rstickup not bound to +forward"
grep -Fq '"rstickdown" = "+back"' "${stdout_file}"   || fail "move: rstickdown not bound to +back"
# Back to Classic: managed keys are wiped clean again.
awk '/MARK_MOVE/{m=1} m && /"pad_y" = ""/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "move: pad_y should be unbound again after leaving modern"
awk '/MARK_MODERN/{m=1} m && /"rstickup" = ""/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "classic: rstickup should be unbound again"
grep -Fq '"joy_padlayout" is "1"' "${stdout_file}"   || fail "joy_padlayout did not record the classic preset"

# Layout presets drive the aiming model with them: modern enables freelook,
# the classic layouts lock it off (which restores vanilla vertical autoaim).
# (Each step's queries print before that step's echo marker, and sv_freelook
# applies one net update after the preset change, so it is queried one step
# later.)
awk '/MARK_DEFAULTS/{m=1} /MARK_MODERN/{m=0} m && /"freelook" is "true"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "modern: freelook should be enabled"
awk '/MARK_DEFAULTS/{m=1} /MARK_MODERN/{m=0} m && /"bd_classic_autoaim" is "false"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "modern: bd_classic_autoaim should be off"
awk '/MARK_MODERN/{m=1} /MARK_MOVE/{m=0} m && /"sv_freelook" is "2"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "modern: sv_freelook should be 2 (on)"
awk '/MARK_MODERN/{m=1} /MARK_MOVE/{m=0} m && /"freelook" is "false"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "classic+move: freelook should be disabled"
awk '/MARK_MODERN/{m=1} /MARK_MOVE/{m=0} m && /"bd_classic_autoaim" is "true"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "classic+move: bd_classic_autoaim should be on"
awk '/MARK_MOVE/{m=1} /MARK_CLASSIC/{m=0} m && /"sv_freelook" is "1"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "classic+move: sv_freelook should be 1 (off)"
awk '/MARK_CLASSIC/{m=1} /MARK_FINAL/{m=0} m && /"sv_freelook" is "1"/{found=1} END{exit !found}' "${stdout_file}" \
    || fail "classic: sv_freelook should be 1 (off)"

# ---------------------------------------------------------------------------
# Cvars and haptics content
# ---------------------------------------------------------------------------

grep -Fq '"joy_gyro_look" is "false"' "${stdout_file}"           || fail "joy_gyro_look missing or wrong default"
grep -Fq '"joy_gyro_sensitivity_yaw" is "1"' "${stdout_file}"    || fail "joy_gyro_sensitivity_yaw missing or wrong default"
grep -Fq '"haptics_do_damage" is "true"' "${stdout_file}"        || fail "haptics_do_damage missing or wrong default"

# Curated weapon haptics must load (haptics_debug prints each definition).
for def in W_BULLET W_CHAINGUN W_SHELL W_SSG W_ROCKET W_ENERGY W_RAIL W_BFG W_SAW_FULL; do
    grep -Fq "rumble add ${def}" "${stdout_file}" || fail "haptics definition ${def} did not load"
done

printf 'PASS: gamepad defaults, layout presets, cvars, and haptics content verified\n'
