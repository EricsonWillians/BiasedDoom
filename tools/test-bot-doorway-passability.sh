#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Verify that a co-op companion cannot body-block Player 1 in a narrow doorway.

Usage:
  ./tools/test-bot-doorway-passability.sh --iwad PATH [options]

Options:
  --iwad PATH              Doom-compatible IWAD (required)
  --exe PATH               biaseddoom executable (default: build/biaseddoom)
  --timeout SEC            wall-clock limit (default: 45)
  --keep-temp              retain diagnostic files
  --skip-if-unavailable    return success with a SKIP message if prerequisites are absent
  -h, --help               show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
fixture_dir="${script_dir}/bot-doorway-passability"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path="${BIASEDDOOM_TEST_IWAD:-}"
test_timeout=45
keep_temp=0
skip_if_unavailable=0
test_root=""
test_failed=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iwad) iwad_path="$2"; shift 2 ;;
        --exe) engine_exe="$2"; shift 2 ;;
        --timeout) test_timeout="$2"; shift 2 ;;
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

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-bot-doorpass.XXXXXX")"
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
    tail -n 160 "${test_root}/stdout.log" >&2 || true
    tail -n 160 "${test_root}/run.log" >&2 || true
    exit 1
}

driver_src="${test_root}/driver"
mkdir -p "${driver_src}/maps"
cp -R "${fixture_dir}/." "${driver_src}/"
python3 "${driver_src}/build_map.py" "${driver_src}/maps/doorpass.wad"
python3 - "${driver_src}" "${test_root}/doorpass.pk3" <<'PY'
import os
import sys
import zipfile

src, output = sys.argv[1:]
with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
    for root, _dirs, files in os.walk(src):
        for name in files:
            path = os.path.join(root, name)
            archive.write(path, os.path.relpath(path, src))
PY

engine_dir="$(cd "$(dirname "${engine_exe}")" && pwd)"
engine_name="$(basename "${engine_exe}")"
set +e
(
    cd "${engine_dir}"
    timeout --signal=INT --kill-after=5s "${test_timeout}s" "./${engine_name}" \
        -headless -stdout -nosound -nointro -python \
        -config "${test_root}/biaseddoom.ini" \
        -savedir "${test_root}/saves" \
        -iwad "${iwad_path}" -file "${test_root}/doorpass.pk3" \
        +map DOORPASS -scripttest 140 8 \
        -pyerrorlog "${test_root}/python-errors.jsonl" \
        +logfile "${test_root}/run.log"
) >"${test_root}/stdout.log" 2>&1
status=$?
set -e
[[ "${status}" -eq 0 ]] || fail "engine exited with status ${status}"
[[ ! -s "${test_root}/python-errors.jsonl" ]] || fail 'Python assertion failed'
for marker in 'BOT_DOORPASS companion_joined' 'BOT_DOORPASS frozen_companion' 'BOT_DOORPASS final' 'BOT_DOORPASS PASS' 'SCRIPT TEST: PASS'; do
    grep -Fq "${marker}" "${test_root}/run.log" || fail "missing expected marker: ${marker}"
done
printf 'PASS: Player 1 crosses a frozen companion while the terminal wall remains solid.\n'
