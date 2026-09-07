#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Validate and package every embedded-Python example.

Usage:
  ./tools/test-python-examples.sh [options]

Options:
  --iwad PATH       additionally run each PK3 briefly in the real engine
  --exe PATH        executable for smoke tests (default: build/biaseddoom)
  --timeout SEC     maximum seconds per engine process (default: 30)
  --keep-temp       retain packages, configs, and logs
  -h, --help        show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
examples_root="${repo_root}/examples/python"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path=""
test_timeout=30
keep_temp=0

trim_manifest_line() {
    local value="${1%%#*}"
    value="${value%$'\r'}"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "${value}"
}

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
            printf 'error: unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ "${test_timeout}" =~ ^[1-9][0-9]*$ ]] || {
    printf 'error: --timeout must be a positive integer\n' >&2
    exit 2
}
command -v python3 >/dev/null 2>&1 || {
    printf 'error: python3 is required for source validation\n' >&2
    exit 2
}
command -v cmake >/dev/null 2>&1 || {
    printf 'error: cmake is required for PK3 packaging\n' >&2
    exit 2
}

if [[ -n "${iwad_path}" ]]; then
    [[ -f "${iwad_path}" ]] || { printf 'error: IWAD not found: %s\n' "${iwad_path}" >&2; exit 2; }
    [[ -x "${engine_exe}" ]] || { printf 'error: executable not found: %s\n' "${engine_exe}" >&2; exit 2; }
    command -v timeout >/dev/null 2>&1 || { printf 'error: GNU timeout is required for smoke tests\n' >&2; exit 2; }
    iwad_path="$(cd "$(dirname "${iwad_path}")" && pwd)/$(basename "${iwad_path}")"
    engine_exe="$(cd "$(dirname "${engine_exe}")" && pwd)/$(basename "${engine_exe}")"
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-python-examples.XXXXXX")"
cleanup() {
    if [[ "${keep_temp}" -eq 1 ]]; then
        printf 'Kept example test artifacts at %s\n' "${test_root}"
    elif [[ -n "${test_root}" && -d "${test_root}" ]]; then
        rm -rf -- "${test_root}"
    fi
}
trap cleanup EXIT

mapfile -t python_sources < <(find "${examples_root}" -type f -name '*.py' -print | sort)
[[ ${#python_sources[@]} -gt 0 ]] || { printf 'error: no Python sources found\n' >&2; exit 1; }
printf 'Syntax-checking %d Python source files...\n' "${#python_sources[@]}"
PYTHONPYCACHEPREFIX="${test_root}/pycache" python3 -m py_compile "${python_sources[@]}"

packages_dir="${test_root}/packages"
"${script_dir}/build-python-examples.sh" --output-dir "${packages_dir}"
mapfile -t packages < <(find "${packages_dir}" -maxdepth 1 -type f -name '*.pk3' -print | sort)
[[ ${#packages[@]} -gt 0 ]] || { printf 'error: no example packages were built\n' >&2; exit 1; }

printf 'Checking %d package manifests...\n' "${#packages[@]}"
for package in "${packages[@]}"; do
    name="$(basename "${package}" .pk3)"
    manifest="${examples_root}/${name}/PYTHON"
    mapfile -t archive_entries < <(cmake -E tar tf "${package}")
    printf '%s\n' "${archive_entries[@]}" | grep -Fxq PYTHON || {
        printf 'error: %s has no root PYTHON manifest\n' "${package}" >&2
        exit 1
    }
    while IFS= read -r entry || [[ -n "${entry}" ]]; do
        entry="$(trim_manifest_line "${entry}")"
        [[ -n "${entry}" ]] || continue
        printf '%s\n' "${archive_entries[@]}" | grep -Fxq "${entry}" || {
            printf 'error: %s manifest references missing %s\n' "${package}" "${entry}" >&2
            exit 1
        }
    done < "${manifest}"
done

if [[ -n "${iwad_path}" ]]; then
    display_runner=()
    if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]] && command -v xvfb-run >/dev/null 2>&1; then
        display_runner=(xvfb-run -a)
    fi
    engine_dir="$(cd "$(dirname "${engine_exe}")" && pwd)"
    engine_name="$(basename "${engine_exe}")"
    harness_dir="${test_root}/smoke-harness"
    harness_package="${test_root}/python-example-smoke-harness.pk3"
    mkdir -p "${harness_dir}/pyscripts"
    printf '%s\n' 'pyscripts/main.py' > "${harness_dir}/PYTHON"
    printf '%s\n' \
        '"""Enter a map, allow initial callbacks to run, then exit cleanly."""' \
        '' \
        'import biaseddoom as bd' \
        '' \
        '' \
        '_done = [False]' \
        '_frames = [0]' \
        '' \
        '' \
        'def finish_smoke_test():' \
        '    if _done[0]:' \
        '        return False' \
        '    _done[0] = True' \
        '    bd.log("PYTHON_EXAMPLE_SMOKE_DONE")' \
        '    bd.execute("quit")' \
        '    return False' \
        '' \
        '' \
        '@bd.on("map_load", priority=-100000)' \
        'def map_loaded(event):' \
        '    bd.schedule(finish_smoke_test, delay=8)' \
        '' \
        '' \
        '@bd.on("imgui_frame")' \
        'def frame_fallback(event):' \
        '    # A mod may engine-pause the world (frozen gametics stall the' \
        '    # scheduled quit); quit after ~2s of rendered frames instead.' \
        '    _frames[0] += 1' \
        '    if _frames[0] >= 120:' \
        '        finish_smoke_test()' \
        > "${harness_dir}/pyscripts/main.py"
    (
        cd "${harness_dir}"
        cmake -E tar cf "${harness_package}" --format=zip PYTHON pyscripts
    )
    printf 'Smoke-testing %d packages in BiasedDoom...\n' "${#packages[@]}"
    for package in "${packages[@]}"; do
        name="$(basename "${package}" .pk3)"
        expected_scripts=1
        while IFS= read -r entry || [[ -n "${entry}" ]]; do
            entry="$(trim_manifest_line "${entry}")"
            [[ -n "${entry}" ]] && ((expected_scripts += 1))
        done < "${examples_root}/${name}/PYTHON"
        stdout_file="${test_root}/${name}.stdout"
        log_file="${test_root}/${name}.log"
        set +e
        (
            cd "${engine_dir}"
            timeout --signal=INT --kill-after=5s "${test_timeout}s" \
                "${display_runner[@]}" "./${engine_name}" \
                -stdout -nosound -nointro -python \
                -config "${test_root}/${name}.ini" \
                -iwad "${iwad_path}" -file "${package}" "${harness_package}" \
                +vid_activeinbackground true +i_pauseinbackground false \
                +logfile "${log_file}" +map map01
        ) >"${stdout_file}" 2>&1
        status=$?
        set -e
        if [[ "${status}" -ne 0 ]] || \
            ! grep -Eq "Python: CPython .* loaded ${expected_scripts}/${expected_scripts} scripts?\\." "${stdout_file}" || \
            ! grep -Fq 'PYTHON_EXAMPLE_SMOKE_DONE' "${stdout_file}" || \
            grep -Eq '^Python .* failed' "${stdout_file}"; then
            printf 'error: engine smoke test failed for %s (status %d)\n' "${name}" "${status}" >&2
            tail -n 100 "${stdout_file}" >&2 || true
            exit 1
        fi
    done
fi

printf 'PASS: %d Python files compiled and %d example PK3s passed manifest validation' \
    "${#python_sources[@]}" "${#packages[@]}"
if [[ -n "${iwad_path}" ]]; then
    printf ' and engine smoke testing'
fi
printf '.\n'
