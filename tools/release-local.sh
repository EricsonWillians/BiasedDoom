#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

APPIMAGE_BUILDER_URL="${APPIMAGE_BUILDER_URL:-https://github.com/AppImageCrafters/appimage-builder/releases/download/v1.1.0/appimage-builder-1.1.0-x86_64.AppImage}"
APPIMAGE_BUILDER_SHA1="${APPIMAGE_BUILDER_SHA1:-6f83a789f6c47a745b97d1b0b3f1df2e7eea7a09}"

VERSION=""
BUILD_TYPE="Release"
ARTIFACT_DIR="artifacts"
LINUX_BUILD_DIR="build-release"
WINDOWS_BUILD_DIR="build-windows-mingw"
NATIVE_TOOLS_DIR="build-native-tools"
REMOTE_NAME="origin"
REPO_NAME=""
IWAD_PATH=""
TITLE=""
NOTES_FILE=""

BUILD_LINUX=1
BUILD_WINDOWS_MINGW=1
BUILD_LINUX_TARBALL=1
SKIP_BUILD=0
SKIP_SMOKE=0
REQUIRE_SMOKE=0
CLEAN=0
ALLOW_DIRTY=0
ALLOW_VERSION_MISMATCH=0
ALLOW_TAG_MISMATCH=0
CREATE_TAG=0
PUSH_TAG=0
PUBLISH=0
DRAFT=0
PRERELEASE=0
LATEST=1
CLOBBER=1
GENERATE_NOTES=0
DRY_RUN=0

ASSETS=()
CHECKSUMS=()
CLEANUP_PATHS=()
VCPKG_TOOLCHAIN_FILE=""

usage() {
    cat <<'USAGE'
Build, verify, and optionally publish a complete local BiasedDoom release.

This is the maintainer fallback when GitHub Actions is broken. It builds the
Linux AppImage and the Windows x64 MinGW zip from this Linux machine, writes
SHA256 files, can smoke-test them with an IWAD, and can publish a real GitHub
release through gh.

Usage:
  ./tools/release-local.sh --version X.Y.Z [options]

Common:
  --version X.Y.Z          release version to build/publish
  --clean                  remove release build directories before configuring
  --jobs N                 parallel jobs (default: host CPU count)
  --artifact-dir DIR       output directory (default: artifacts)
  --iwad PATH              IWAD used for Linux and Wine smoke tests
  --skip-smoke-tests       do not run runtime smoke tests
  --require-smoke-tests    fail if smoke tests cannot run

Build selection:
  --skip-build             reuse existing artifacts instead of building
  --linux-only             build/package only Linux artifacts
  --windows-only           build/package only Windows MinGW artifacts
  --no-linux               skip Linux AppImage and Linux tarball
  --no-windows-mingw       skip Windows MinGW package
  --no-linux-tarball       publish only the Linux AppImage, not the extra tarball
  --linux-build-dir DIR    Linux CMake build directory (default: build-release)
  --windows-build-dir DIR  MinGW CMake build directory (default: build-windows-mingw)
  --native-tools-dir DIR   native tool build directory for MinGW (default: build-native-tools)

Git/tag safeguards:
  --allow-dirty            allow tracked uncommitted source changes
  --allow-version-mismatch allow src/version.h VERSIONSTR to differ from --version
  --allow-tag-mismatch     allow vX.Y.Z to point at a commit other than HEAD
  --create-tag             create local tag vX.Y.Z at HEAD if missing
  --push-tag               push tag vX.Y.Z before publishing
  --remote NAME            git remote for tag checks/pushes (default: origin)

GitHub publishing:
  --publish                create/update the GitHub release with the artifacts
  --repo OWNER/REPO        pass an explicit repository to gh
  --title TEXT             release title (default: BiasedDoom X.Y.Z)
  --notes-file FILE        release notes file for gh
  --generate-notes         let GitHub generate release notes
  --draft                  save as draft instead of publishing
  --prerelease             mark as prerelease
  --no-latest              do not force "Latest"
  --no-clobber             do not replace existing release assets

Other:
  --dry-run                print the major commands without making changes
  -h, --help               show this help

Examples:
  ./tools/release-local.sh --version 4.15.2 --clean --iwad ~/doom2.wad
  ./tools/release-local.sh --version 4.15.2 --skip-build --publish
  ./tools/release-local.sh --version 4.15.2 --create-tag --push-tag --publish
USAGE
}

log() {
    printf '\n==> %s\n' "$*" >&2
}

ok() {
    printf 'OK: %s\n' "$*" >&2
}

warn() {
    printf 'warning: %s\n' "$*" >&2
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

cleanup() {
    local path
    for path in "${CLEANUP_PATHS[@]}"; do
        [[ -n "${path}" ]] || continue
        if [[ "$(basename "${path}")" == .release-wine-prefix.* ]] && command -v wineserver >/dev/null 2>&1; then
            env WINEPREFIX="${path}" wineserver -k >/dev/null 2>&1 || true
        fi
        chmod -R u+rwX "${path}" >/dev/null 2>&1 || true
        rm -rf "${path}" >/dev/null 2>&1 || true
    done
}
trap cleanup EXIT

host_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        printf '2\n'
    fi
}

JOBS="${NUM_JOBS:-$(host_jobs)}"

quote_command() {
    printf '> '
    printf '%q ' "$@"
    printf '\n'
}

run() {
    quote_command "$@" >&2
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        return 0
    fi
    "$@"
}

run_in_dir() {
    local dir="$1"
    shift

    printf '> cd %q && ' "${dir}" >&2
    printf '%q ' "$@" >&2
    printf '\n' >&2
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        return 0
    fi
    (cd "${dir}" && "$@")
}

require_command() {
    local name="$1"
    local hint="$2"

    if ! command -v "${name}" >/dev/null 2>&1; then
        die "Required command '${name}' was not found. ${hint}"
    fi
}

resolve_under_repo() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s/%s\n' "${PROJECT_ROOT}" "$1" ;;
    esac
}

validate_semver() {
    [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

source_version() {
    awk -F'"' '/^#define VERSIONSTR / {print $2; exit}' "${PROJECT_ROOT}/src/version.h"
}

get_vcpkg_gitlink_commit() {
    git -C "${PROJECT_ROOT}" ls-tree HEAD vcpkg 2>/dev/null | awk '{print $3}'
}

require_clean_tracked_tree() {
    [[ "${ALLOW_DIRTY}" -eq 1 ]] && return 0

    if ! git -C "${PROJECT_ROOT}" diff --quiet || ! git -C "${PROJECT_ROOT}" diff --cached --quiet; then
        die "Tracked source changes are present. Commit/stash them first, or rerun with --allow-dirty."
    fi
}

ensure_vcpkg() {
    local vcpkg_dir="${PROJECT_ROOT}/vcpkg"
    local toolchain="${vcpkg_dir}/scripts/buildsystems/vcpkg.cmake"
    local commit

    log "Preparing vcpkg"
    require_command git "Install git."

    if [[ ! -f "${toolchain}" ]]; then
        commit="$(get_vcpkg_gitlink_commit || true)"
        run rm -rf "${vcpkg_dir}"
        run git clone https://github.com/microsoft/vcpkg.git "${vcpkg_dir}"
        if [[ -n "${commit}" ]]; then
            run git -C "${vcpkg_dir}" checkout "${commit}"
        fi
    fi

    if [[ ! -x "${vcpkg_dir}/vcpkg" ]]; then
        run "${vcpkg_dir}/bootstrap-vcpkg.sh" -disableMetrics
    fi

    [[ "${DRY_RUN}" -eq 1 || -f "${toolchain}" ]] || die "vcpkg toolchain is missing after preparation: ${toolchain}"
    VCPKG_TOOLCHAIN_FILE="${toolchain}"
    ok "vcpkg is ready"
}

ensure_appimage_builder() {
    local destination="$1"
    local current_sha=""

    require_command sha1sum "Install coreutils."

    if [[ -f "${destination}" ]]; then
        current_sha="$(sha1sum "${destination}" | awk '{print $1}')"
    fi

    if [[ "${current_sha}" != "${APPIMAGE_BUILDER_SHA1}" ]]; then
        log "Downloading appimage-builder"
        run rm -f "${destination}"
        if command -v wget >/dev/null 2>&1; then
            run wget -qO "${destination}" "${APPIMAGE_BUILDER_URL}"
        elif command -v curl >/dev/null 2>&1; then
            run curl -fsSL -o "${destination}" "${APPIMAGE_BUILDER_URL}"
        else
            die "Need wget or curl to download appimage-builder."
        fi
    fi

    if [[ "${DRY_RUN}" -eq 0 ]]; then
        echo "${APPIMAGE_BUILDER_SHA1}  ${destination}" | sha1sum -c -
        chmod +x "${destination}"
    fi
}

tag_name() {
    printf 'v%s\n' "${VERSION}"
}

assert_version_and_tag() {
    local actual_version
    local tag
    local head_sha
    local tag_sha

    actual_version="$(source_version)"
    if [[ "${actual_version}" != "${VERSION}" && "${ALLOW_VERSION_MISMATCH}" -eq 0 ]]; then
        die "src/version.h VERSIONSTR is ${actual_version}, but --version is ${VERSION}. Use tools/bump-version.sh or --allow-version-mismatch."
    fi

    tag="$(tag_name)"
    if git -C "${PROJECT_ROOT}" rev-parse -q --verify "refs/tags/${tag}" >/dev/null 2>&1; then
        head_sha="$(git -C "${PROJECT_ROOT}" rev-parse HEAD)"
        tag_sha="$(git -C "${PROJECT_ROOT}" rev-list -n 1 "${tag}")"
        if [[ "${head_sha}" != "${tag_sha}" && "${ALLOW_TAG_MISMATCH}" -eq 0 ]]; then
            die "Tag ${tag} points at ${tag_sha}, but HEAD is ${head_sha}. Check out the tag commit or use --allow-tag-mismatch."
        fi
    elif [[ "${CREATE_TAG}" -eq 1 ]]; then
        log "Creating local tag ${tag}"
        run git -C "${PROJECT_ROOT}" tag "${tag}"
    elif [[ "${PUBLISH}" -eq 1 || "${PUSH_TAG}" -eq 1 ]]; then
        die "Local tag ${tag} does not exist. Create it first or rerun with --create-tag."
    else
        warn "Local tag ${tag} does not exist; artifacts will still be built."
    fi
}

push_tag_if_requested() {
    local tag

    [[ "${PUSH_TAG}" -eq 1 ]] || return 0
    tag="$(tag_name)"

    git -C "${PROJECT_ROOT}" remote get-url "${REMOTE_NAME}" >/dev/null 2>&1 || die "Remote '${REMOTE_NAME}' does not exist."
    run git -C "${PROJECT_ROOT}" push "${REMOTE_NAME}" "${tag}"
}

remote_tag_exists() {
    local tag="$1"

    git -C "${PROJECT_ROOT}" ls-remote --exit-code --tags "${REMOTE_NAME}" "refs/tags/${tag}" >/dev/null 2>&1
}

assert_jammy_release_host() {
    [[ -r /etc/os-release ]] || die "Cannot identify the Linux release host. Use Ubuntu 22.04 (Jammy)."
    # shellcheck disable=SC1091
    . /etc/os-release
    [[ "${ID:-}" == "ubuntu" && "${VERSION_CODENAME:-}" == "jammy" ]] \
        || die "Linux release packaging must run on Ubuntu 22.04 (Jammy) so binary and bundled SONAMEs match. Use the Release workflow or a Jammy VM/container."
}

configure_and_build_linux() {
    local build_path="$1"

    [[ "${SKIP_BUILD}" -eq 1 ]] && return 0

    assert_jammy_release_host
    require_command cmake "Install CMake 3.16 or newer."
    require_command ninja "Install ninja-build."

    if [[ "${CLEAN}" -eq 1 ]]; then
        run rm -rf "${build_path}"
    fi

    log "Configuring Linux Release build"
    run cmake -S "${PROJECT_ROOT}" -B "${build_path}" -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_TOOLCHAIN_FILE}" \
        -DBIASEDDOOM_ENABLE_GLTF=ON \
        -DBIASEDDOOM_BUILD_GLTF=ON \
        -DNEODOOM_ENABLE_GLTF=ON \
        -DBIASEDDOOM_ENABLE_PYTHON=ON \
        -DBIASEDDOOM_REQUIRE_PYTHON=ON \
        -DPK3_QUIET_ZIPDIR=ON

    log "Building Linux Release"
    run cmake --build "${build_path}" --config "${BUILD_TYPE}" --parallel "${JOBS}"
}

package_linux_appimage() {
    local build_path="$1"
    local artifact_path="$2"
    local package_name="BiasedDoom-${VERSION}-Linux-x86_64"
    local appimage_path="${artifact_path}/${package_name}.AppImage"
    local checksum_path="${appimage_path}.sha256"
    local builder="${build_path}/appimage-builder"
    local appimages=()

    [[ "${BUILD_LINUX}" -eq 1 ]] || return 0
    [[ "${DRY_RUN}" -eq 1 ]] && warn "Dry run: Linux AppImage packaging commands will be printed only."

    require_command sha256sum "Install coreutils."
    ensure_appimage_builder "${builder}"

    log "Packaging Linux AppImage"
    run mkdir -p "${artifact_path}"
    run rm -rf "${build_path}/AppDir" "${appimage_path}" "${checksum_path}"
    if [[ "${DRY_RUN}" -eq 0 ]]; then
        rm -f "${build_path}"/*.AppImage "${build_path}"/*.AppImage.zsync
    fi

    run cmake --install "${build_path}" --config "${BUILD_TYPE}" --prefix "${build_path}/AppDir/usr"
    if [[ "${DRY_RUN}" -eq 0 ]]; then
        find "${build_path}/AppDir" -type f -path '*/python/lib/python*/encodings/__init__.py' -print -quit | grep -q . \
            || die "Linux AppDir is missing the embedded Python standard library."
        find "${build_path}/AppDir" -type f -name 'libpython*.so*' -print -quit | grep -q . \
            || die "Linux AppDir is missing the embedded CPython runtime library."
        find "${build_path}/AppDir" -type f -path '*/python/LICENSE.txt' -print -quit | grep -q . \
            || die "Linux AppDir is missing the CPython license."
    fi
    run_in_dir "${build_path}" env GIT_DESCRIBE="$(tag_name)" ./appimage-builder --skip-tests --recipe ../tools/AppImageBuilder.yml

    if [[ "${DRY_RUN}" -eq 0 ]]; then
        mapfile -t appimages < <(find "${build_path}" -maxdepth 1 -type f -name '*.AppImage' | sort)
        if (( ${#appimages[@]} != 1 )); then
            printf 'Found AppImages:\n' >&2
            printf '  %s\n' "${appimages[@]}" >&2
            die "Expected exactly one generated AppImage in ${build_path}, found ${#appimages[@]}."
        fi
        "${PROJECT_ROOT}/tools/check-appimage-deps.sh" "${appimages[0]}"
        cp "${appimages[0]}" "${appimage_path}"
        chmod +x "${appimage_path}"
        (cd "${artifact_path}" && sha256sum "$(basename "${appimage_path}")" > "$(basename "${checksum_path}")")
    fi

    ASSETS+=("${appimage_path}")
    CHECKSUMS+=("${checksum_path}")
    ok "Linux AppImage: ${appimage_path}"
}

copy_linux_runtime_file() {
    local source="$1"
    local destination="$2"

    [[ -e "${source}" ]] || die "Missing Linux package file: ${source}"
    cp -a "${source}" "${destination}/"
}

package_linux_tarball() {
    local build_path="$1"
    local artifact_path="$2"
    local package_name="BiasedDoom-${VERSION}-Linux-x86_64"
    local stage_root="${artifact_path}/staging"
    local stage_dir="${stage_root}/${package_name}"
    local tar_path="${artifact_path}/${package_name}.tar.gz"
    local checksum_path="${tar_path}.sha256"
    local path

    [[ "${BUILD_LINUX}" -eq 1 && "${BUILD_LINUX_TARBALL}" -eq 1 ]] || return 0

    log "Packaging extra Linux portable tarball"
    run rm -rf "${stage_dir}" "${tar_path}" "${checksum_path}"
    run mkdir -p "${stage_dir}"

    if [[ "${DRY_RUN}" -eq 0 ]]; then
        copy_linux_runtime_file "${build_path}/biaseddoom" "${stage_dir}"
        shopt -s nullglob
        for path in "${build_path}"/*.pk3 "${build_path}"/libzmusic.so* "${build_path}"/libpython*.so*; do
            copy_linux_runtime_file "${path}" "${stage_dir}"
        done
        shopt -u nullglob

        for path in "${build_path}/soundfonts" "${build_path}/fm_banks" "${build_path}/python"; do
            if [[ -d "${path}" ]]; then
                cp -a "${path}" "${stage_dir}/"
            fi
        done

        compgen -G "${stage_dir}/*.pk3" >/dev/null || die "Linux tarball staging failed: no PK3 files were found."
        compgen -G "${stage_dir}/libpython*.so*" >/dev/null || die "Linux tarball is missing the embedded CPython runtime library."
        find "${stage_dir}/python" -type f -path '*/lib/python*/encodings/__init__.py' -print -quit | grep -q . \
            || die "Linux tarball is missing the embedded Python standard library."
        [[ -f "${stage_dir}/python/LICENSE.txt" ]] || die "Linux tarball is missing the CPython license."
        tar -czf "${tar_path}" -C "${stage_root}" "${package_name}"
        (cd "${artifact_path}" && sha256sum "$(basename "${tar_path}")" > "$(basename "${checksum_path}")")
        rm -rf "${stage_root}"
    fi

    ASSETS+=("${tar_path}")
    CHECKSUMS+=("${checksum_path}")
    ok "Linux tarball: ${tar_path}"
}

collect_linux_artifacts() {
    local artifact_path="$1"
    local package_name="BiasedDoom-${VERSION}-Linux-x86_64"
    local appimage_path="${artifact_path}/${package_name}.AppImage"
    local appimage_checksum="${appimage_path}.sha256"
    local tar_path="${artifact_path}/${package_name}.tar.gz"
    local tar_checksum="${tar_path}.sha256"

    [[ "${BUILD_LINUX}" -eq 1 ]] || return 0

    ASSETS+=("${appimage_path}")
    CHECKSUMS+=("${appimage_checksum}")

    if [[ "${BUILD_LINUX_TARBALL}" -eq 1 ]]; then
        ASSETS+=("${tar_path}")
        CHECKSUMS+=("${tar_checksum}")
    fi
}

build_windows_mingw() {
    local artifact_path="$1"
    local args=(
        "${SCRIPT_DIR}/build-windows-mingw.sh"
        --package
        --build-type "${BUILD_TYPE}"
        --jobs "${JOBS}"
        --native-tools-dir "${NATIVE_TOOLS_DIR}"
        --build-dir "${WINDOWS_BUILD_DIR}"
        --artifact-dir "${ARTIFACT_DIR}"
    )

    [[ "${BUILD_WINDOWS_MINGW}" -eq 1 ]] || return 0

    if [[ "${SKIP_BUILD}" -eq 1 ]]; then
        return 0
    fi

    if [[ "${CLEAN}" -eq 1 ]]; then
        args+=(--clean)
    fi

    log "Building Windows x64 MinGW package"
    run "${args[@]}"
}

collect_windows_mingw_artifact() {
    local artifact_path="$1"
    local package_name="BiasedDoom-${VERSION}-Windows-x64-MinGW"
    local zip_path="${artifact_path}/${package_name}.zip"
    local checksum_path="${zip_path}.sha256"

    [[ "${BUILD_WINDOWS_MINGW}" -eq 1 ]] || return 0

    ASSETS+=("${zip_path}")
    CHECKSUMS+=("${checksum_path}")
}

validate_artifacts_exist() {
    local path

    [[ "${DRY_RUN}" -eq 1 ]] && return 0

    for path in "${ASSETS[@]}" "${CHECKSUMS[@]}"; do
        [[ -s "${path}" ]] || die "Expected release artifact is missing or empty: ${path}"
    done
}

verify_checksums() {
    local checksum
    local base
    local dir

    [[ "${DRY_RUN}" -eq 1 ]] && return 0

    log "Verifying SHA256 checksums"
    for checksum in "${CHECKSUMS[@]}"; do
        base="$(basename "${checksum}")"
        dir="$(dirname "${checksum}")"
        run_in_dir "${dir}" sha256sum -c "${base}"
    done
}

inspect_packages() {
    local artifact_path="$1"
    local linux_appimage="${artifact_path}/BiasedDoom-${VERSION}-Linux-x86_64.AppImage"
    local windows_zip="${artifact_path}/BiasedDoom-${VERSION}-Windows-x64-MinGW.zip"
    local windows_package="BiasedDoom-${VERSION}-Windows-x64-MinGW"

    [[ "${DRY_RUN}" -eq 1 ]] && return 0

    log "Inspecting package contents"

    if [[ "${BUILD_LINUX}" -eq 1 ]]; then
        [[ -x "${linux_appimage}" ]] || die "Linux AppImage is not executable: ${linux_appimage}"
        if command -v file >/dev/null 2>&1; then
            file "${linux_appimage}"
        fi
    fi

    if [[ "${BUILD_WINDOWS_MINGW}" -eq 1 ]]; then
        require_command unzip "Install unzip."
        unzip -Z1 "${windows_zip}" | grep -Fx "${windows_package}/biaseddoom.exe" >/dev/null \
            || die "Windows zip is missing ${windows_package}/biaseddoom.exe."
        unzip -Z1 "${windows_zip}" | grep -Eq "^${windows_package}/[^/]+\\.pk3$" \
            || die "Windows zip is missing top-level PK3 resource files."
        ok "Windows zip contains biaseddoom.exe and PK3 resources"
    fi
}

run_accepting_norun_exit() {
    local description="$1"
    shift
    local status

    log "${description}"
    quote_command "$@" >&2
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        return 0
    fi

    set +e
    "$@"
    status=$?
    set -e

    case "${status}" in
        0|57)
            ok "${description}"
            ;;
        124)
            die "${description} timed out."
            ;;
        *)
            die "${description} failed with exit code ${status}."
            ;;
    esac
}

run_accepting_norun_exit_in_dir() {
    local description="$1"
    local dir="$2"
    shift 2
    local status

    log "${description}"
    printf '> cd %q && ' "${dir}" >&2
    printf '%q ' "$@" >&2
    printf '\n' >&2
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        return 0
    fi

    set +e
    (cd "${dir}" && "$@")
    status=$?
    set -e

    case "${status}" in
        0|57)
            ok "${description}"
            ;;
        124)
            die "${description} timed out."
            ;;
        *)
            die "${description} failed with exit code ${status}."
            ;;
    esac
}

run_smoke_tests() {
    local artifact_path="$1"
    local linux_appimage="${artifact_path}/BiasedDoom-${VERSION}-Linux-x86_64.AppImage"
    local windows_zip="${artifact_path}/BiasedDoom-${VERSION}-Windows-x64-MinGW.zip"
    local temp_root
    local wine_prefix
    local iwad_name

    [[ "${SKIP_SMOKE}" -eq 1 ]] && return 0

    if [[ -z "${IWAD_PATH}" ]]; then
        if [[ "${REQUIRE_SMOKE}" -eq 1 ]]; then
            die "--require-smoke-tests needs --iwad PATH."
        fi
        warn "No --iwad was provided; skipping runtime smoke tests."
        return 0
    fi

    IWAD_PATH="$(resolve_under_repo "${IWAD_PATH}")"
    [[ -f "${IWAD_PATH}" ]] || die "IWAD not found: ${IWAD_PATH}"

    if [[ "${BUILD_LINUX}" -eq 1 ]]; then
        run_accepting_norun_exit "Smoke-testing Linux AppImage" \
            timeout 90s env APPIMAGE_EXTRACT_AND_RUN=1 "${linux_appimage}" -stdout -iwad "${IWAD_PATH}" -norun
    fi

    if [[ "${BUILD_WINDOWS_MINGW}" -eq 1 ]]; then
        if ! command -v wine >/dev/null 2>&1; then
            if [[ "${REQUIRE_SMOKE}" -eq 1 ]]; then
                die "wine is required for the Windows smoke test. Install wine or drop --require-smoke-tests."
            fi
            warn "wine not found; skipping Windows smoke test."
            return 0
        fi
        require_command unzip "Install unzip."

        temp_root="$(mktemp -d "${PROJECT_ROOT}/.release-windows-smoke.XXXXXX")"
        wine_prefix="$(mktemp -d "${PROJECT_ROOT}/.release-wine-prefix.XXXXXX")"
        CLEANUP_PATHS+=("${temp_root}" "${wine_prefix}")
        iwad_name="$(basename "${IWAD_PATH}")"

        run unzip -q "${windows_zip}" -d "${temp_root}"
        run cp "${IWAD_PATH}" "${temp_root}/BiasedDoom-${VERSION}-Windows-x64-MinGW/${iwad_name}"
        run timeout --kill-after=30s 240s env WINEPREFIX="${wine_prefix}" WINEDEBUG=-all wineboot -u

        run_accepting_norun_exit_in_dir \
            "Smoke-testing Windows MinGW package through Wine" \
            "${temp_root}/BiasedDoom-${VERSION}-Windows-x64-MinGW" \
            timeout --kill-after=30s 240s env WINEPREFIX="${wine_prefix}" WINEDEBUG=-all \
            wine ./biaseddoom.exe -stdout -iwad "${iwad_name}" -norun
    fi
}

write_default_notes() {
    local notes
    local asset

    notes="$(mktemp "${PROJECT_ROOT}/.release-notes.XXXXXX.md")"
    CLEANUP_PATHS+=("${notes}")

    {
        printf 'BiasedDoom %s\n' "${VERSION}"
        printf '\n'
        printf "Manual release built from tag \`%s\`.\n" "$(tag_name)"
        printf '\n'
        printf 'Artifacts:\n'
        for asset in "${ASSETS[@]}"; do
            printf -- "- \`%s\`\n" "$(basename "${asset}")"
        done
        printf '\n'
        printf 'Linux users should download the AppImage, mark it executable if needed, and run it with an IWAD.\n'
        printf "Windows users should download and extract the MinGW zip, then run \`biaseddoom.exe\` with an IWAD.\n"
    } > "${notes}"

    NOTES_FILE="${notes}"
}

publish_release() {
    local tag
    local title
    local repo_args=()
    local release_args=()
    local edit_args=()
    local upload_args=()
    local upload_files=()

    [[ "${PUBLISH}" -eq 1 ]] || return 0

    tag="$(tag_name)"
    title="${TITLE:-BiasedDoom ${VERSION}}"
    if [[ -n "${REPO_NAME}" ]]; then
        repo_args=(-R "${REPO_NAME}")
    fi
    upload_files=("${ASSETS[@]}" "${CHECKSUMS[@]}")

    if [[ "${GENERATE_NOTES}" -eq 0 && -z "${NOTES_FILE}" ]]; then
        write_default_notes
    fi

    if [[ "${DRY_RUN}" -eq 1 ]]; then
        log "Dry-run GitHub release publish"
        printf 'Release: %s\n' "${tag}"
        printf 'Title: %s\n' "${title}"
        printf 'Assets:\n'
        printf '  %s\n' "${upload_files[@]}"
        return 0
    fi

    require_command gh "Install with: sudo apt install gh"
    git -C "${PROJECT_ROOT}" remote get-url "${REMOTE_NAME}" >/dev/null 2>&1 || die "Remote '${REMOTE_NAME}' does not exist."

    if ! remote_tag_exists "${tag}"; then
        die "Remote tag ${tag} is not present on ${REMOTE_NAME}. Push it first or rerun with --push-tag."
    fi

    if ! gh auth status >/dev/null 2>&1; then
        die "gh is not authenticated. Run: gh auth login"
    fi

    if gh release view "${tag}" "${repo_args[@]}" >/dev/null 2>&1; then
        log "Updating existing GitHub release ${tag}"
        upload_args=(release upload "${tag}" "${upload_files[@]}")
        if [[ "${CLOBBER}" -eq 1 ]]; then
            upload_args+=(--clobber)
        fi
        gh "${upload_args[@]}" "${repo_args[@]}"

        edit_args=(release edit "${tag}" --title "${title}")
        if [[ "${DRAFT}" -eq 1 ]]; then
            edit_args+=(--draft)
        else
            edit_args+=(--draft=false)
        fi
        if [[ "${PRERELEASE}" -eq 1 ]]; then
            edit_args+=(--prerelease)
        else
            edit_args+=(--prerelease=false)
        fi
        if [[ "${LATEST}" -eq 1 ]]; then
            edit_args+=(--latest)
        else
            edit_args+=(--latest=false)
        fi
        if [[ -n "${NOTES_FILE}" ]]; then
            edit_args+=(--notes-file "${NOTES_FILE}")
        fi
        gh "${edit_args[@]}" "${repo_args[@]}"
    else
        log "Creating GitHub release ${tag}"
        release_args=(release create "${tag}" "${upload_files[@]}" --title "${title}" --verify-tag)
        if [[ "${GENERATE_NOTES}" -eq 1 ]]; then
            release_args+=(--generate-notes)
        elif [[ -n "${NOTES_FILE}" ]]; then
            release_args+=(--notes-file "${NOTES_FILE}")
        fi
        if [[ "${DRAFT}" -eq 1 ]]; then
            release_args+=(--draft)
        fi
        if [[ "${PRERELEASE}" -eq 1 ]]; then
            release_args+=(--prerelease)
        fi
        if [[ "${LATEST}" -eq 1 ]]; then
            release_args+=(--latest)
        else
            release_args+=(--latest=false)
        fi
        gh "${release_args[@]}" "${repo_args[@]}"
    fi

    if [[ "${DRAFT}" -eq 0 ]]; then
        ok "Published non-draft GitHub release ${tag}"
    else
        ok "Saved draft GitHub release ${tag}"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            [[ $# -ge 2 ]] || die "--version requires X.Y.Z."
            VERSION="$2"
            shift 2
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs requires a value."
            JOBS="$2"
            shift 2
            ;;
        --artifact-dir)
            [[ $# -ge 2 ]] || die "--artifact-dir requires a value."
            ARTIFACT_DIR="$2"
            shift 2
            ;;
        --iwad)
            [[ $# -ge 2 ]] || die "--iwad requires a path."
            IWAD_PATH="$2"
            shift 2
            ;;
        --skip-smoke-tests)
            SKIP_SMOKE=1
            shift
            ;;
        --require-smoke-tests)
            REQUIRE_SMOKE=1
            shift
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --linux-only)
            BUILD_LINUX=1
            BUILD_WINDOWS_MINGW=0
            shift
            ;;
        --windows-only)
            BUILD_LINUX=0
            BUILD_WINDOWS_MINGW=1
            shift
            ;;
        --no-linux)
            BUILD_LINUX=0
            shift
            ;;
        --no-windows-mingw)
            BUILD_WINDOWS_MINGW=0
            shift
            ;;
        --no-linux-tarball)
            BUILD_LINUX_TARBALL=0
            shift
            ;;
        --linux-build-dir)
            [[ $# -ge 2 ]] || die "--linux-build-dir requires a directory."
            LINUX_BUILD_DIR="$2"
            shift 2
            ;;
        --windows-build-dir)
            [[ $# -ge 2 ]] || die "--windows-build-dir requires a directory."
            WINDOWS_BUILD_DIR="$2"
            shift 2
            ;;
        --native-tools-dir)
            [[ $# -ge 2 ]] || die "--native-tools-dir requires a directory."
            NATIVE_TOOLS_DIR="$2"
            shift 2
            ;;
        --allow-dirty)
            ALLOW_DIRTY=1
            shift
            ;;
        --allow-version-mismatch)
            ALLOW_VERSION_MISMATCH=1
            shift
            ;;
        --allow-tag-mismatch)
            ALLOW_TAG_MISMATCH=1
            shift
            ;;
        --create-tag)
            CREATE_TAG=1
            shift
            ;;
        --push-tag)
            PUSH_TAG=1
            shift
            ;;
        --remote)
            [[ $# -ge 2 ]] || die "--remote requires a name."
            REMOTE_NAME="$2"
            shift 2
            ;;
        --publish)
            PUBLISH=1
            shift
            ;;
        --repo)
            [[ $# -ge 2 ]] || die "--repo requires OWNER/REPO."
            REPO_NAME="$2"
            shift 2
            ;;
        --title)
            [[ $# -ge 2 ]] || die "--title requires text."
            TITLE="$2"
            shift 2
            ;;
        --notes-file)
            [[ $# -ge 2 ]] || die "--notes-file requires a path."
            NOTES_FILE="$2"
            shift 2
            ;;
        --generate-notes)
            GENERATE_NOTES=1
            shift
            ;;
        --draft)
            DRAFT=1
            shift
            ;;
        --prerelease)
            PRERELEASE=1
            shift
            ;;
        --no-latest)
            LATEST=0
            shift
            ;;
        --no-clobber)
            CLOBBER=0
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "Unknown option: $1"
            ;;
    esac
done

if [[ -z "${VERSION}" ]]; then
    VERSION="$(source_version)"
    [[ -n "${VERSION}" ]] || die "Could not infer version from src/version.h. Pass --version X.Y.Z."
fi

validate_semver "${VERSION}" || die "--version must be semantic X.Y.Z, got '${VERSION}'."
[[ "${JOBS}" =~ ^[0-9]+$ && "${JOBS}" -gt 0 ]] || die "--jobs must be a positive integer."
[[ "${BUILD_LINUX}" -eq 1 || "${BUILD_WINDOWS_MINGW}" -eq 1 ]] || die "Nothing selected to build."
[[ "${GENERATE_NOTES}" -eq 0 || -z "${NOTES_FILE}" ]] || die "Use either --generate-notes or --notes-file, not both."

cd "${PROJECT_ROOT}"

ARTIFACT_PATH="$(resolve_under_repo "${ARTIFACT_DIR}")"
LINUX_BUILD_PATH="$(resolve_under_repo "${LINUX_BUILD_DIR}")"
WINDOWS_BUILD_PATH="$(resolve_under_repo "${WINDOWS_BUILD_DIR}")"
NATIVE_TOOLS_PATH="$(resolve_under_repo "${NATIVE_TOOLS_DIR}")"
ARTIFACT_DIR="${ARTIFACT_PATH}"
LINUX_BUILD_DIR="${LINUX_BUILD_PATH}"
WINDOWS_BUILD_DIR="${WINDOWS_BUILD_PATH}"
NATIVE_TOOLS_DIR="${NATIVE_TOOLS_PATH}"

if [[ -n "${NOTES_FILE}" ]]; then
    NOTES_FILE="$(resolve_under_repo "${NOTES_FILE}")"
    [[ -f "${NOTES_FILE}" ]] || die "Release notes file not found: ${NOTES_FILE}"
fi

log "Release settings"
printf 'Version: %s\n' "${VERSION}"
printf 'Tag: %s\n' "$(tag_name)"
printf 'Artifacts: %s\n' "${ARTIFACT_PATH}"
printf 'Linux: %s\n' "$([[ "${BUILD_LINUX}" -eq 1 ]] && printf yes || printf no)"
printf 'Windows MinGW: %s\n' "$([[ "${BUILD_WINDOWS_MINGW}" -eq 1 ]] && printf yes || printf no)"
printf 'Publish: %s\n' "$([[ "${PUBLISH}" -eq 1 ]] && printf yes || printf no)"

require_command git "Install git."
require_command sha256sum "Install coreutils."
require_clean_tracked_tree
assert_version_and_tag
push_tag_if_requested

if [[ "${BUILD_LINUX}" -eq 1 ]]; then
    if [[ "${SKIP_BUILD}" -eq 1 ]]; then
        collect_linux_artifacts "${ARTIFACT_PATH}"
    else
        ensure_vcpkg
        configure_and_build_linux "${LINUX_BUILD_PATH}"
        package_linux_appimage "${LINUX_BUILD_PATH}" "${ARTIFACT_PATH}"
        package_linux_tarball "${LINUX_BUILD_PATH}" "${ARTIFACT_PATH}"
    fi
fi

if [[ "${BUILD_WINDOWS_MINGW}" -eq 1 ]]; then
    build_windows_mingw "${ARTIFACT_PATH}"
    collect_windows_mingw_artifact "${ARTIFACT_PATH}"
fi

validate_artifacts_exist
verify_checksums
inspect_packages "${ARTIFACT_PATH}"
run_smoke_tests "${ARTIFACT_PATH}"
publish_release

log "Release artifacts ready"
printf '%s\n' "${ASSETS[@]}" "${CHECKSUMS[@]}" | sed 's/^/  /'

if [[ "${PUBLISH}" -eq 0 ]]; then
    printf '\nTo publish a real non-draft release after review:\n'
    printf '  ./tools/release-local.sh --version %q --skip-build --publish\n' "${VERSION}"
fi
