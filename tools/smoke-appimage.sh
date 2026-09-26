#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    cat <<'USAGE'
Usage: tools/smoke-appimage.sh PATH-TO.AppImage [docker-image]

Run the AppImage in a clean Ubuntu container with no desktop libraries or
display server. The engine is expected to reach its normal "no IWAD" startup
error; dynamic-loader and AppRun failures are release blockers.
USAGE
    exit $(( $# >= 1 && $# <= 2 ? 0 : 1 ))
fi

appimage="$(realpath "$1")"
image="${2:-ubuntu:22.04}"

[[ -f "${appimage}" ]] || { echo "error: AppImage not found: $1" >&2; exit 1; }
[[ -x "${appimage}" ]] || { echo "error: AppImage is not executable: $1" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "error: docker is required" >&2; exit 1; }

docker run --rm -i \
    -v "${appimage}:/tmp/biaseddoom.AppImage:ro" \
    "${image}" \
    bash -s <<'CONTAINER_SCRIPT'
set +e
output="$(timeout 60s env \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    BIASEDDOOM_HEADLESS=1 \
    SDL_VIDEODRIVER=dummy \
    XDG_RUNTIME_DIR=/tmp \
    /tmp/biaseddoom.AppImage -headless -norun 2>&1)"
status=$?
printf '%s\n' "${output}"
printf 'smoke exit status: %d\n' "${status}"

if grep -Eq 'error while loading shared libraries|APPRUN_ERROR|GLIBC_.*not found' <<< "${output}"; then
    exit 1
fi

case "${status}" in
    0|57)
        exit 0
        ;;
    255)
        grep -q 'Cannot find a game IWAD' <<< "${output}"
        exit $?
        ;;
    *)
        exit "${status}"
        ;;
esac
CONTAINER_SCRIPT
