#!/usr/bin/env bash
# Regression smoke test for Brutal Doom's ACS-driven flashlight.  The mod is
# intentionally supplied by the caller: this repository neither includes nor
# redistributes third-party IWAD/PWAD/PK3 content.
set -euo pipefail

usage() {
    cat <<'USAGE'
Exercise Brutal Doom's high-density flashlight path in an isolated headless run.

Usage:
  ./tools/test-brutal-doom-flashlight.sh --iwad PATH --mod PATH [options]

Options:
  --iwad PATH              Doom II IWAD containing MAP01 (required unless
                           BIASEDDOOM_TEST_IWAD is set)
  --mod PATH               Brutal Doom PK3 containing ToggleFlashlight
                           (required unless BIASEDDOOM_TEST_BRUTAL_DOOM is set)
  --exe PATH               biaseddoom executable (default: build/biaseddoom)
  --tics COUNT             time to hold the flashlight on (default: 700)
  --procedural             run the reported gothic procedural recipe instead
                           of MAP01, with the player kept alive for the full test
  --timeout SEC            wall-clock deadline (default: 60)
  --keep-temp              retain isolated config and diagnostic logs
  --skip-if-unavailable    report SKIP instead of failing if a prerequisite is
                           unavailable
  -h, --help               show this help

The test loads only the supplied IWAD and PK3, selects Brutal Doom's high
flashlight quality, invokes its ToggleFlashlight ACS script, and lets the
client-side light loop run under -scripttest's deterministic 8x fast-forward.
It checks that the engine exits
cleanly and reports SCRIPT TEST: PASS.  It is a simulation/rollback regression
test; renderer-specific visual QA remains a separate interactive check.
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
observer_fixture_dir="${script_dir}/brutal-doom-flashlight-observer"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path="${BIASEDDOOM_TEST_IWAD:-}"
mod_path="${BIASEDDOOM_TEST_BRUTAL_DOOM:-}"
test_tics=700
test_timeout=60
keep_temp=0
skip_if_unavailable=0
procedural=0
test_root=""
test_failed=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iwad)
            [[ $# -ge 2 ]] || { printf 'error: --iwad requires a path\n' >&2; exit 2; }
            iwad_path="$2"
            shift 2
            ;;
        --mod)
            [[ $# -ge 2 ]] || { printf 'error: --mod requires a path\n' >&2; exit 2; }
            mod_path="$2"
            shift 2
            ;;
        --exe)
            [[ $# -ge 2 ]] || { printf 'error: --exe requires a path\n' >&2; exit 2; }
            engine_exe="$2"
            shift 2
            ;;
        --tics)
            [[ $# -ge 2 ]] || { printf 'error: --tics requires a count\n' >&2; exit 2; }
            test_tics="$2"
            shift 2
            ;;
        --timeout)
            [[ $# -ge 2 ]] || { printf 'error: --timeout requires seconds\n' >&2; exit 2; }
            test_timeout="$2"
            shift 2
            ;;
        --procedural)
            procedural=1
            shift
            ;;
        --keep-temp)
            keep_temp=1
            shift
            ;;
        --skip-if-unavailable)
            skip_if_unavailable=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'error: unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

missing_dependency() {
    local message="$1"
    if [[ "${skip_if_unavailable}" -eq 1 ]]; then
        printf 'SKIP: %s\n' "${message}"
        exit 0
    fi
    printf 'error: %s\n' "${message}" >&2
    exit 2
}

[[ "${test_tics}" =~ ^[1-9][0-9]*$ ]] || missing_dependency '--tics must be a positive integer'
[[ "${test_timeout}" =~ ^[1-9][0-9]*$ ]] || missing_dependency '--timeout must be a positive integer'
[[ -n "${iwad_path}" && -f "${iwad_path}" ]] || missing_dependency 'a valid Doom II IWAD is required'
[[ -n "${mod_path}" && -f "${mod_path}" ]] || missing_dependency 'a Brutal Doom PK3 is required'
[[ -x "${engine_exe}" ]] || missing_dependency "executable not found or not executable: ${engine_exe}"
[[ -f "${observer_fixture_dir}/PYTHON" && -f "${observer_fixture_dir}/pyscripts/main.py" ]] || missing_dependency 'flashlight observer fixture is missing'
command -v timeout >/dev/null 2>&1 || missing_dependency 'GNU timeout is required'
command -v python3 >/dev/null 2>&1 || missing_dependency 'python3 is required'

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-brutal-flashlight.XXXXXX")"

cleanup() {
    if [[ "${keep_temp}" -eq 1 || "${test_failed}" -ne 0 ]]; then
        printf 'Diagnostic artifacts: %s\n' "${test_root}"
    elif [[ -n "${test_root}" && -d "${test_root}" ]]; then
        rm -rf -- "${test_root}"
    fi
}
trap cleanup EXIT

fail() {
    test_failed=1
    printf 'FAIL: %s\n' "$1" >&2
    if [[ -f "${test_root}/run.log" ]]; then
        printf '%s\n' '--- engine log (tail) ---' >&2
        tail -n 180 "${test_root}/run.log" >&2 || true
    fi
    if [[ -f "${test_root}/stdout.log" ]]; then
        printf '%s\n' '--- process output (tail) ---' >&2
        tail -n 180 "${test_root}/stdout.log" >&2 || true
    fi
    exit 1
}

engine_dir="$(cd "$(dirname "${engine_exe}")" && pwd)"
engine_name="$(basename "${engine_exe}")"
run_log="${test_root}/run.log"
stdout_log="${test_root}/stdout.log"
python_error_log="${test_root}/python-errors.jsonl"
observer_src="${test_root}/observer"
observer_pk3="${test_root}/brutal-flashlight-observer.pk3"
if ! cp -R "${observer_fixture_dir}" "${observer_src}"; then
    fail 'could not stage the flashlight observer fixture'
fi
if ! python3 -B - "${observer_src}" "${observer_pk3}" <<'PY'
import os
import sys
import zipfile

src, output = sys.argv[1:]
with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
    for root, dirs, files in os.walk(src):
        dirs[:] = [name for name in dirs if name != "__pycache__"]
        for name in files:
            if name.endswith((".pyc", ".pyo")):
                continue
            path = os.path.join(root, name)
            archive.write(path, os.path.relpath(path, src))
PY
then
    fail 'could not package the flashlight observer fixture'
fi
map_name="MAP01"
procgen_args=()
if [[ "${procedural}" -eq 1 ]]; then
    # This is the reported procedural recipe. Keep it fixed as an integration
    # regression: its purpose is to exercise the map loader, predicted
    # flashlight actors, and normal procedural combat world together.
    map_name="PROCMAP"
    procgen_args=(
        +procgen_seed 972996588
        +procgen_theme gothic
        +procgen_difficulty 3
        +procgen_size 3
        +procgen_layout 2
        +procgen_verticality 2
        +procgen_detail 1
        +procgen_outdoors 2
    )
fi

printf 'Running Brutal Doom flashlight rollback regression on %s (%s tics)...\n' \
    "${map_name}" "${test_tics}"
# Fast-forward reduces wall-clock test time without changing map-time or the
# predicted-light lifecycle being exercised.
set +e
(
    cd "${engine_dir}"
    timeout --signal=TERM --kill-after=10s "${test_timeout}s" "./${engine_name}" \
        -headless -stdout -nosound -nointro -noautoload -python \
        -config "${test_root}/biaseddoom.ini" \
        -savedir "${test_root}/saves" \
        -iwad "${iwad_path}" \
        -file "${mod_path}" "${observer_pk3}" \
        +developer 2 \
        +sv_cheats 1 \
        +god \
        +cl_debugprediction 2 \
        +cl_bd_disableflashlight false \
        +cl_bd_flashlighttype 2 \
        "${procgen_args[@]}" \
        +map "${map_name}" \
        +pukename ToggleFlashlight \
        -scripttest "${test_tics}" 8 \
        -pyerrorlog "${python_error_log}" \
        +logfile "${run_log}"
) >"${stdout_log}" 2>&1
run_status=$?
set -e

if [[ "${run_status}" -ne 0 ]]; then
    fail "engine flashlight regression run exited with status ${run_status}"
fi

if ! grep -Fq "SCRIPT TEST: PASS" "${run_log}"; then
    fail 'engine did not report a successful scripted run'
fi
if [[ -s "${python_error_log}" ]]; then
    fail 'flashlight observer reported a Python assertion failure'
fi
if grep -Eqi 'SCRIPT TEST: FAIL|I_Error|Fatal error|Segmentation fault|AddressSanitizer|UndefinedBehaviorSanitizer' \
    "${run_log}" "${stdout_log}"; then
    fail 'engine reported a fatal error during the flashlight run'
fi
if grep -Eqi '(unknown|no such|not found).{0,80}ToggleFlashlight|ToggleFlashlight.{0,80}(unknown|no such|not found)' \
    "${run_log}" "${stdout_log}"; then
    fail 'the supplied mod did not expose the ToggleFlashlight ACS script'
fi
if [[ "${procedural}" -eq 1 ]] && ! grep -Fq 'PROCMAP - Unnamed' "${run_log}"; then
    fail 'the requested procedural recipe did not load PROCMAP'
fi
if ! grep -Fq 'BRUTAL_FLASHLIGHT_OBSERVER active' "${run_log}"; then
    fail 'ToggleFlashlight did not enable Brutal Doom flashlight state'
fi

# Older third-party client-side ACS effects may create ordinary thinkers while
# prediction is live.  Newer engine paths can legitimately avoid this warning,
# so count it for diagnostics rather than making the regression depend on it.
flashlight_events="$(grep -Ehc 'Spawned non-client-side Thinker (FlashLight|Flashlight)' "${run_log}" 2>/dev/null || true)"
prediction_events="$(grep -Ehc '(Spawned|Destroyed) non-client-side (Thinker|Object) ' "${run_log}" 2>/dev/null || true)"
if [[ "${flashlight_events}" -gt 8 ]]; then
    fail "prediction warning limiter emitted ${flashlight_events} flashlight warnings (expected at most 8 per map)"
fi
if [[ "${prediction_events}" -gt 8 ]]; then
	fail "prediction warning limiter emitted ${prediction_events} total spawn/destroy warnings (expected at most 8 per map)"
fi
printf 'PASS: Brutal Doom flashlight high-density path completed (%s observed legacy prediction warning(s)).\n' \
    "${flashlight_events}"
