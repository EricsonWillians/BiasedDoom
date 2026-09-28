#!/usr/bin/env bash
# Console smoke test: verifies the live default console key binding, the new
# console option cvars (con_font, con_timestamps), and open/close rendering
# via pixel comparison against a baseline frame.
#
# Runs the engine headless (xvfb when no display is available) with a small
# generated Python driver PK3 that:
#   1. queries `bind <backtick>` (must report toggleconsole),
#   2. captures a baseline screenshot of the static world,
#   3. applies non-default console options (classic font, timestamps,
#      scale 2, dim 0.5) and queries two of them for deterministic log lines,
#   4. opens the console, screenshots it, closes it, screenshots again,
#   5. quits.
# The script then asserts the log markers and compares the screenshots:
# baseline vs closed must match, baseline vs open must differ.
set -euo pipefail

usage() {
    cat <<'USAGE'
Smoke-test the game console in the real engine.

Usage:
  ./tools/test-console.sh --iwad PATH [options]

Options:
  --iwad PATH       IWAD to run (required, e.g. doom2.wad)
  --exe PATH        executable for the test (default: build/biaseddoom)
  --timeout SEC     maximum seconds for the engine process (default: 60)
  --keep-temp       retain the driver PK3, config, logs, and screenshots
  -h, --help        show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path=""
test_timeout=60
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

display_runner=()
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    if command -v xvfb-run >/dev/null 2>&1; then
        display_runner=(xvfb-run -a)
    else
        printf 'error: no display available and xvfb-run is missing\n' >&2
        exit 2
    fi
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-console-test.XXXXXX")"
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
"""Console smoke test driver.

Sequence (all delays in tics):
  - kill monsters so the world renders deterministically,
  - query the backquote binding and capture the baseline frame,
  - apply non-default console options (also exercises the new cvars),
  - chain open/screenshot/close/screenshot/quit through the console command
    buffer's `wait`, which keeps running while the open console pauses the
    world ticker (bd.schedule freezes there).
"""

import os

import biaseddoom as bd

OUT_DIR = os.environ.get("CONSOLE_TEST_OUT", "/tmp")


def shot(name):
    bd.execute(f"screenshot {os.path.join(OUT_DIR, name)}.png")


def step_settle():
    bd.execute("kill monsters")


def step_baseline():
    bd.execute("bind `")
    shot("baseline")


def step_options():
    bd.execute("con_font 1")
    bd.execute("con_timestamps true")
    bd.execute("con_scale 2")
    bd.execute("con_alpha 0.5")
    bd.execute("con_font")
    bd.execute("con_timestamps")
    bd.execute("echo CONSOLE_OPTIONS_APPLIED")


def step_chain():
    # Opening the console pauses the world ticker in single player
    # (P_CheckTickerPaused keys off ConsoleState), which freezes bd.schedule.
    # The console command buffer's own `wait` keeps running, so the whole
    # open/screenshot/close/screenshot sequence is chained through it.
    bd.execute(
        f"toggleconsole; wait 70; screenshot {OUT_DIR}/open.png; "
        f"toggleconsole; wait 70; screenshot {OUT_DIR}/closed.png; "
        "wait 10; echo PYTEST CONSOLE_SMOKE_DONE; quit"
    )


@bd.on("map_load")
def on_map_load(event):
    bd.schedule(step_settle, delay=5)
    bd.schedule(step_baseline, delay=40)
    bd.schedule(step_options, delay=45)
    bd.schedule(step_chain, delay=55)
EOF

driver_pk3="${test_root}/console_test.pk3"
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

stdout_file="${test_root}/console.stdout"
log_file="${test_root}/console.log"
export CONSOLE_TEST_OUT="${test_root}"

set +e
timeout --signal=INT --kill-after=5s "${test_timeout}s" \
    "${display_runner[@]}" "${engine_exe}" \
    -stdout -nosound -nointro -python \
    -config "${test_root}/console.ini" \
    -iwad "${iwad_path}" -file "${driver_pk3}" \
    +vid_activeinbackground true +i_pauseinbackground false \
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
grep -Fq 'PYTEST CONSOLE_SMOKE_DONE' "${stdout_file}" || fail "driver did not complete"

# Live default binding check: the backquote key must still invoke toggleconsole.
grep -Fq '"`" = "toggleconsole"' "${stdout_file}" || fail "default console key binding missing or changed"

# The new console option cvars must exist, accept values, and report them back.
grep -Fq '"con_font" is "1"' "${stdout_file}" || fail "con_font cvar query mismatch"
grep -Fq '"con_timestamps" is "true"' "${stdout_file}" || fail "con_timestamps cvar query mismatch"
grep -Fq 'CONSOLE_OPTIONS_APPLIED' "${stdout_file}" || fail "console option commands did not execute"

# ---------------------------------------------------------------------------
# Screenshot comparisons
# ---------------------------------------------------------------------------

for shot in baseline open closed; do
    [[ -f "${test_root}/${shot}.png" ]] || fail "missing screenshot ${shot}.png"
done

compare="${repo_root}/tools/compare_screenshots.py"

# Closed console must render like the baseline (small tolerance for animated
# textures/light pulses in the starting view).
if ! python3 "${compare}" "${test_root}/baseline.png" "${test_root}/closed.png" \
        --threshold 2 --max-diff-pct 1.0 >/dev/null; then
    fail "console did not fully close (closed frame differs from baseline)"
fi

# Open console must visibly differ from the baseline.
set +e
python3 "${compare}" "${test_root}/baseline.png" "${test_root}/open.png" \
    --threshold 2 --max-diff-pct 1.0 >/dev/null
open_cmp=$?
set -e
if [[ "${open_cmp}" -ne 1 ]]; then
    fail "open console frame did not differ from baseline as expected (compare exit ${open_cmp})"
fi

printf 'PASS: console binding, option cvars, and open/close rendering verified\n'
