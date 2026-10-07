#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Verify that a co-op companion routes around a two-portal L turn to its leader.

Usage:
  ./tools/test-bot-companion-route.sh --iwad PATH [options]

Options:
  --iwad PATH              Doom-compatible IWAD (required)
  --exe PATH               biaseddoom executable (default: build/biaseddoom)
  --timeout SEC            wall-clock limit (default: 45)
  --keep-temp              retain diagnostic files
  --skip-if-unavailable    return success with a SKIP message if a prerequisite is unavailable
  -h, --help               show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
fixture_dir="${script_dir}/bot-companion-route"
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
        --keep-temp) keep_temp=1; shift ;;
        --skip-if-unavailable) skip_if_unavailable=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

missing() {
    if [[ "${skip_if_unavailable}" -eq 1 ]]; then
        printf 'SKIP: %s\n' "$1"
        exit 0
    fi
    printf 'error: %s\n' "$1" >&2
    exit 2
}

[[ -n "${iwad_path}" && -f "${iwad_path}" ]] || missing 'a valid IWAD is required'
[[ -x "${engine_exe}" ]] || missing "executable not found: ${engine_exe}"
[[ -f "${fixture_dir}/PYTHON" && -f "${fixture_dir}/build_map.py" ]] || missing 'fixture files are missing'
[[ "${test_timeout}" =~ ^[1-9][0-9]*$ ]] || missing '--timeout must be a positive integer'
command -v timeout >/dev/null 2>&1 || missing 'GNU timeout is required'
command -v python3 >/dev/null 2>&1 || missing 'python3 is required'

# The fixture must observe native bot movement, rather than fabricate a route
# through the Python API. Its only allowed mutation moves Player 1 before the
# companion joins, which creates a deterministic physical route to follow.
readonly_python="${fixture_dir}/pyscripts/main.py"
for forbidden in '.set_velocity(' '.destroy(' '.set_input(';
do
    if grep -Fq "${forbidden}" "${readonly_python}"; then
        printf 'error: read-only fixture contains forbidden mutation: %s\n' "${forbidden}" >&2
        exit 1
    fi
done
if [[ "$(grep -Fc '.set_position(' "${readonly_python}")" -ne 1 ]] ||
    ! grep -Fq 'if now == 1:' "${readonly_python}" ||
    ! grep -Fq 'moved = leader.actor.set_position(LEADER_X, LEADER_Y, 0.0, check=True, fog=False)' "${readonly_python}"; then
    printf '%s\n' 'error: fixture may move only the leader once before companion join' >&2
    exit 1
fi
if [[ "$(grep -Fc 'bd.execute(' "${readonly_python}")" -ne 1 ]] ||
    ! grep -Fq 'bd.execute("addcompanion")' "${readonly_python}"; then
    printf '%s\n' 'error: fixture may schedule only the reviewed native addcompanion command' >&2
    exit 1
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-bot-follow-route.XXXXXX")"
cleanup() {
    if [[ "${keep_temp}" -eq 1 || "${test_failed}" -ne 0 ]]; then
        printf 'Diagnostic artifacts: %s\n' "${test_root}"
    else
        rm -rf -- "${test_root}"
    fi
}
trap cleanup EXIT

fail() {
    test_failed=1
    printf 'error: %s\n' "$1" >&2
    tail -n 180 "${test_root}/stdout.log" >&2 || true
    tail -n 180 "${test_root}/run.log" >&2 || true
    exit 1
}

driver_src="${test_root}/driver"
mkdir -p "${driver_src}/maps"
if ! cp -R "${fixture_dir}/." "${driver_src}/"; then
    fail 'could not stage the route fixture'
fi
if ! python3 -B "${driver_src}/build_map.py" "${driver_src}/maps/followrt.wad"; then
    fail 'could not build the route fixture map'
fi
if ! python3 - "${driver_src}" "${test_root}/followroute.pk3" <<'PY'
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
    fail 'could not package the route fixture'
fi

engine_dir="$(cd "$(dirname "${engine_exe}")" && pwd)"
engine_name="$(basename "${engine_exe}")"
set +e
(
    cd "${engine_dir}"
    timeout --signal=INT --kill-after=5s "${test_timeout}s" "./${engine_name}" \
        -headless -stdout -noautoload -nosound -nointro -python \
        -config "${test_root}/biaseddoom.ini" \
        -savedir "${test_root}/saves" \
        -iwad "${iwad_path}" -file "${test_root}/followroute.pk3" \
        +map FOLLOWRT -scripttest 190 8 \
        -pyerrorlog "${test_root}/python-errors.jsonl" \
        +logfile "${test_root}/run.log"
) >"${test_root}/stdout.log" 2>&1
status=$?
set -e
[[ "${status}" -eq 0 ]] || fail "engine exited with status ${status}"
[[ ! -s "${test_root}/python-errors.jsonl" ]] || fail 'Python assertion failed'
for marker in 'BOT_FOLLOW_ROUTE leader_ready' 'BOT_FOLLOW_ROUTE companion_start' 'BOT_FOLLOW_ROUTE final' 'BOT_FOLLOW_ROUTE PASS' 'SCRIPT TEST: PASS'; do
    grep -Fq "${marker}" "${test_root}/run.log" || fail "missing expected marker: ${marker}"
done
printf 'PASS: Companion follows the leader through a physical L-route without teleport recovery.\n'
