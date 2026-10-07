#!/usr/bin/env bash
# Regression test for the extended [SPRITES] namespace used by DSDHacked /
# DECOHack MBF21 weapon patches.  The fixture is generated at runtime so this
# test does not redistribute or depend on any third-party mod.
set -euo pipefail

usage() {
    cat <<'USAGE'
Verify DSDHacked extended sprite-number compatibility in the real engine.

Usage:
  ./tools/test-dehacked-extended-sprites.sh --iwad PATH [options]

Options:
  --iwad PATH              Doom or Doom II IWAD (required)
  --exe PATH               biaseddoom executable (default: build/biaseddoom)
  --timeout SEC            initialization deadline (default: 30)
  --keep-temp              retain the generated PWAD and logs
  --skip-if-unavailable    report SKIP instead of failing for missing prerequisites
  -h, --help               show this help

The generated PWAD contains a DSDHacked-format DEHACKED lump with:
  - Frame 1 pointing at sprite 8000, and [SPRITES] 8000 = PISG;
  - the largest signed 32-bit sprite ID (2147483647), also resolving to PISG;
  - 65,537 additional sparse IDs, proving the extended-entry budget exceeds
    the original 65,536-entry compatibility floor without growing the sprite
    definition list;
  - 2147483648, which must be rejected safely.
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path="${BIASEDDOOM_TEST_IWAD:-}"
test_timeout=30
keep_temp=0
skip_if_unavailable=0
test_root=""
engine_pid=""
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

missing() {
    if [[ "${skip_if_unavailable}" -eq 1 ]]; then
        printf 'SKIP: %s\n' "$1"
        exit 0
    fi
    printf 'error: %s\n' "$1" >&2
    exit 2
}

[[ -n "${iwad_path}" && -f "${iwad_path}" ]] || missing 'a valid Doom-family IWAD is required'
[[ -x "${engine_exe}" ]] || missing "executable not found: ${engine_exe}"
[[ "${test_timeout}" =~ ^[1-9][0-9]*$ ]] || missing '--timeout must be a positive integer'
command -v python3 >/dev/null 2>&1 || missing 'python3 is required to synthesize the PWAD fixture'

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-deh-sprites.XXXXXX")"

stop_engine() {
    [[ -n "${engine_pid}" ]] || return 0
    if kill -0 "${engine_pid}" 2>/dev/null; then
        # The null-video driver deliberately remains in its event loop. The
        # test only needs startup parsing, so stop it once initialization has
        # completed instead of spending the full deadline in that loop.
        kill -TERM "${engine_pid}" 2>/dev/null || true
        local stop_deadline=$((SECONDS + 2))
        while kill -0 "${engine_pid}" 2>/dev/null && (( SECONDS < stop_deadline )); do
            sleep 0.1
        done
        if kill -0 "${engine_pid}" 2>/dev/null; then
            kill -KILL "${engine_pid}" 2>/dev/null || true
        fi
    fi
    if ! wait "${engine_pid}" 2>/dev/null; then
        : # A deliberate signal shutdown is expected for the headless loop.
    fi
    engine_pid=""
}

cleanup() {
    stop_engine
    if [[ "${keep_temp}" -eq 1 || "${test_failed}" -ne 0 ]]; then
        printf 'Diagnostic artifacts: %s\n' "${test_root}"
    else
        rm -rf -- "${test_root}"
    fi
}
trap cleanup EXIT

fail() {
    test_failed=1
    printf 'FAIL: %s\n' "$1" >&2
    printf '%s\n' '--- engine output tail ---' >&2
    tail -n 120 "${test_root}/stdout.log" >&2 || true
    printf '%s\n' '--- engine logfile tail ---' >&2
    tail -n 160 "${test_root}/run.log" >&2 || true
    exit 1
}

# A minimal PWAD with a root-level DEHACKED lump. PISG is a stock Doom/Doom II
# weapon sprite, so a successful remap verifies both the parser and the state
# sprite lookup without bundling any copyrighted or third-party art.
fixture_wad="${test_root}/extended-sprites.wad"
python3 - "${fixture_wad}" <<'PY'
import struct
import sys

bulk_entry_count = 65537
bulk_first_sprite = 1000000
bulk_last_sprite = bulk_first_sprite + bulk_entry_count - 1

patch = f'''Patch File for DeHackEd v3.0
# Synthetic DSDHacked extended-sprite regression fixture.
Doom version = 2021
Patch format = 6

Frame 1
Sprite number = 8000

Frame 2
Sprite number = 2147483647

Frame 3
Sprite number = {bulk_last_sprite}

[SPRITES]
8000 = PISG
2147483647 = PISG
'''.encode("ascii")

# Repeat the already-known stock PISG name. This crosses the previous sparse
# entry limit while keeping the test lightweight: it exercises the map-entry
# budget without manufacturing tens of thousands of global sprite definitions.
patch += b"".join(
    f"{bulk_first_sprite + index} = PISG\n".encode("ascii")
    for index in range(bulk_entry_count)
)
patch += b"2147483648 = PISG\n"

output = sys.argv[1]
directory_offset = 12 + len(patch)
with open(output, "wb") as wad:
    wad.write(struct.pack("<4sii", b"PWAD", 1, directory_offset))
    wad.write(patch)
    wad.write(struct.pack("<ii8s", 12, len(patch), b"DEHACKED"))
PY

stdout_log="${test_root}/stdout.log"
run_log="${test_root}/run.log"
engine_dir="$(cd "$(dirname "${engine_exe}")" && pwd)"
engine_name="$(basename "${engine_exe}")"

(
    cd "${engine_dir}"
    exec "./${engine_name}" \
        -headless -stdout -nosound -nointro -noautoload \
        -config "${test_root}/biaseddoom.ini" \
        -savedir "${test_root}/saves" \
        -iwad "${iwad_path}" -file "${fixture_wad}" \
        +logfile "${run_log}"
) >"${stdout_log}" 2>&1 &
engine_pid=$!

# Patch installation and FinishDehPatch are synchronous during startup. M_Init
# appears after FinishDehPatch, so it proves the state remap has actually run.
ready=0
deadline=$((SECONDS + test_timeout))
while (( SECONDS < deadline )); do
    if [[ -f "${run_log}" ]] \
        && grep -Fq 'Adding dehacked patch extended-sprites.wad:DEHACKED' "${run_log}" \
        && grep -Fq 'Patch installed' "${run_log}" \
        && grep -Fq 'M_Init: Init menus.' "${run_log}"; then
        ready=1
        break
    fi
    if ! kill -0 "${engine_pid}" 2>/dev/null; then
        if wait "${engine_pid}"; then
            engine_status=0
        else
            engine_status=$?
        fi
        engine_pid=""
        fail "engine exited before DeHackEd initialization completed (status ${engine_status})"
    fi
    sleep 0.1
done

[[ "${ready}" -eq 1 ]] || fail "engine did not complete DeHackEd initialization within ${test_timeout}s"
stop_engine

# 8000 is the conventional DECOHack extended range used by MBF21 weapon mods.
# It, INT_MAX, and a tail entry beyond the former 65,536-entry cap must all
# resolve through the DSDHacked sparse sprite table.
if grep -Eq 'Sprite number (8000|2147483647|1065536) out of range\.' "${run_log}"; then
    fail 'a valid DSDHacked extended sprite ID was rejected'
fi
if grep -Eq 'Frame [0-9]+: Sprite (8000|2147483647|1065536)( out of range| \(PISG\) is undefined)' "${run_log}"; then
    fail 'an extended sprite state did not resolve to the stock PISG sprite'
fi

# Values outside the signed 32-bit DSDHacked namespace must be rejected before
# any storage growth or narrowing conversion can occur.
grep -Fq 'Sprite number 2147483648 out of range.' "${run_log}" \
    || fail 'out-of-range DSDHacked sprite ID was not rejected safely'

printf 'PASS: DSDHacked sprite IDs 8000, INT_MAX, and 65,537 sparse entries resolve;\n'
printf '      an ID above INT_MAX is rejected before allocation or narrowing.\n'
