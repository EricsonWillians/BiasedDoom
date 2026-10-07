#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Run the stock Doom II MAP01 companion regression with hostiles preserved.

The fixture queues native console input to walk Player 1 from the stock
floor-56 start through MAP01's first floor-8 ledge, then adds a companion
through the normal engine command path. It verifies nearby stock hostiles were
not removed and proves that the same companion pawn makes the lower exterior
before the normal catch-up window. There are no actor writes or teleports.

Usage:
  ./tools/test-bot-map01-hostiles.sh --iwad PATH [options]

Options:
  --iwad PATH              Doom II IWAD containing stock MAP01 (required,
                           unless BIASEDDOOM_TEST_IWAD is set)
  --exe PATH               biaseddoom executable (default: build/biaseddoom)
  --timeout SEC            wall-clock limit (default: 45)
  --keep-temp              retain isolated config and diagnostic logs
  --skip-if-unavailable    return success with a SKIP message instead of
                           failing when the IWAD, engine, or timeout utility
                           is unavailable
  -h, --help               show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
fixture_dir="${script_dir}/bot-map01-hostiles"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path="${BIASEDDOOM_TEST_IWAD:-}"
test_timeout=45
keep_temp=0
skip_if_unavailable=0
test_root=""
test_failed=0

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

[[ "${test_timeout}" =~ ^[1-9][0-9]*$ ]] || missing_dependency '--timeout must be a positive integer'
[[ -n "${iwad_path}" ]] || missing_dependency 'Doom II IWAD is required; pass --iwad PATH or set BIASEDDOOM_TEST_IWAD'
[[ -f "${iwad_path}" ]] || missing_dependency "IWAD not found: ${iwad_path}"
[[ -x "${engine_exe}" ]] || missing_dependency "executable not found or not executable: ${engine_exe}"
[[ -f "${fixture_dir}/PYTHON" ]] || missing_dependency "fixture missing: ${fixture_dir}"
command -v timeout >/dev/null 2>&1 || missing_dependency 'GNU timeout is required'

# The Python half may schedule the exact native input sequence but cannot move,
# damage, or destroy actors. That keeps the observed route inside the normal
# player/bot simulation rather than fabricating it through the test API.
readonly_python="${fixture_dir}/pyscripts/main.py"
for forbidden in '.set_position(' '.set_velocity(' '.destroy(' '.set_input(';
do
    if grep -Fq "${forbidden}" "${readonly_python}"; then
        printf 'error: read-only fixture contains forbidden mutation: %s\n' "${forbidden}" >&2
        exit 1
    fi
done
readonly allowed_native_input='bd.execute("addcompanion; wait 30; +forward; wait 20; -forward")'
if [[ "$(grep -Fc 'bd.execute(' "${readonly_python}")" -ne 1 ]] ||
    ! grep -Fq "${allowed_native_input}" "${readonly_python}"; then
    printf '%s\n' 'error: fixture may schedule only the reviewed native addcompanion/forward input sequence' >&2
    exit 1
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-bot-map01-hostiles.XXXXXX")"
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
    printf 'error: %s\n' "$1" >&2
    if [[ -f "${test_root}/run.log" ]]; then
        printf '%s\n' '--- engine log (tail) ---' >&2
        tail -n 180 "${test_root}/run.log" >&2 || true
    fi
    if [[ -f "${test_root}/stdout.log" ]]; then
        printf '%s\n' '--- process output (tail) ---' >&2
        tail -n 120 "${test_root}/stdout.log" >&2 || true
    fi
    exit 1
}

engine_dir="$(cd "$(dirname "${engine_exe}")" && pwd)"
engine_name="$(basename "${engine_exe}")"
run_log="${test_root}/run.log"
stdout_log="${test_root}/stdout.log"

printf 'Running stock Doom II MAP01 companion regression with hostiles preserved...\n'
set +e
(
    cd "${engine_dir}"
    timeout --signal=INT --kill-after=5s "${test_timeout}s" "./${engine_name}" \
        -headless -stdout -nosound -nointro -python \
        -config "${test_root}/biaseddoom.ini" \
        -savedir "${test_root}/saves" \
        -iwad "${iwad_path}" \
        -file "${fixture_dir}" \
        -skill 3 \
        +map MAP01 \
        -scripttest 280 8 \
        -pyerrorlog "${test_root}/python-errors.jsonl" \
        +logfile "${run_log}"
) >"${stdout_log}" 2>&1
run_status=$?
set -e

if [[ "${run_status}" -ne 0 ]]; then
    fail "engine regression run exited with status ${run_status}"
fi

required_markers=(
    'BOT_MAP01_HOSTILES map=MAP01'
    'BOT_MAP01_HOSTILES leader_start'
    'elevated_platform=True'
    'BOT_MAP01_HOSTILES hostiles_retained initial='
    'BOT_MAP01_HOSTILES leader_descended tic='
    'native_forward=True'
    'BOT_MAP01_HOSTILES companion_start'
    'BOT_MAP01_HOSTILES hostiles_at_join count='
    'BOT_MAP01_HOSTILES companion_descended tic='
    'original_pawn=True'
    'native_before_catchup=True'
    'BOT_MAP01_HOSTILES final'
    'SCRIPT TEST: PASS (0 Python error(s) in 280 tics)'
)
for marker in "${required_markers[@]}"; do
    if ! grep -Fq "${marker}" "${run_log}"; then
        fail "engine log is missing expected marker: ${marker}"
    fi
done

if [[ -s "${test_root}/python-errors.jsonl" ]]; then
    fail 'Python test reported an error or assertion'
fi

printf 'PASS: stock MAP01 companion follows native Player 1 with nearby hostiles intact.\n'
