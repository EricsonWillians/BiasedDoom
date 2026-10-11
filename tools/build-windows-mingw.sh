#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Build a Windows x64 biaseddoom.exe from Linux using MinGW-w64.

Usage:
  ./tools/build-windows-mingw.sh [options]

Options:
  --clean                 remove native/cross build directories before configuring
  --configure-only        configure native tools and the MinGW build, then stop
  --build-only            skip configure and build an existing MinGW build directory
  --package               create artifacts/BiasedDoom-<version>-Windows-x64-MinGW.zip
  --build-type TYPE       Debug, Release, RelWithDebInfo, or MinSizeRel (default: Release)
  --jobs N                parallel build jobs (default: host CPU count)
  --native-tools-dir DIR  host build directory for zipdir/lemon/re2c (default: build-native-tools)
  --build-dir DIR         MinGW build directory (default: build-windows-mingw)
  --artifact-dir DIR      package output directory (default: artifacts)
  --triplet NAME          vcpkg triplet (default: x64-mingw-static)
  --install               run cmake --install after building
  --install-prefix DIR    install prefix used with --install
  --no-gltf               disable glTF support
  --no-vulkan             disable Vulkan support
  --no-openal-vcpkg       do not use vcpkg OpenAL Soft
  --no-libsndfile-vcpkg   do not embed libsndfile and compressed audio codecs
  --no-libvpx-vcpkg       do not use vcpkg libvpx
  -h, --help              show this help

Required Ubuntu packages:
  sudo apt install autoconf autoconf-archive automake libtool \
    mingw-w64 g++-mingw-w64 gcc-mingw-w64 nasm

Python scripting is unavailable in MinGW builds. Use the native MSVC package
when a mod needs Python; ACS and ZScript remain enabled here.
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

require_command() {
    local name="$1"
    local hint="$2"

    if ! command -v "${name}" >/dev/null 2>&1; then
        die "Required command '${name}' was not found. ${hint}"
    fi
}

require_autoconf_archive() {
    local macro_dir

    # libsodium is part of the online multiplayer client and vcpkg builds it
    # from source for the MinGW triplet. The package supplies M4 macros rather
    # than a command, so checking only autoreconf/autoconf gives a confusing
    # failure much later inside the vcpkg port.
    macro_dir="$(aclocal --print-ac-dir 2>/dev/null || true)"
    if [[ -z "${macro_dir}" ]] || ! find "${macro_dir}" -maxdepth 1 -type f -name 'ax_*.m4' -print -quit | grep -q .; then
        die "autoconf-archive is required to build the online client. Install it with: sudo apt install autoconf-archive"
    fi
}

archive_member_format() {
    local archive="$1"
    local member
    local temp_dir

    [[ -f "${archive}" ]] || return 1
    member="$(x86_64-w64-mingw32-ar t "${archive}" | head -n 1)"
    [[ -n "${member}" ]] || return 1

    temp_dir="$(mktemp -d)"
    (
        cd "${temp_dir}"
        x86_64-w64-mingw32-ar x "${archive}" "${member}"
        file "${member}"
    )
    rm -rf "${temp_dir}"
}

archive_is_mingw_coff() {
    local archive="$1"
    archive_member_format "${archive}" | grep -Eq 'COFF object|MS Windows'
}

resolve_under_repo() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s/%s\n' "${REPO_ROOT}" "$1" ;;
    esac
}

run() {
    {
        printf '> '
        printf '%q ' "$@"
        printf '\n'
    } >&2
    "$@"
}

host_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        printf '2\n'
    fi
}

prefer_mingw_posix_thread_model() {
    local c_compiler
    local cxx_compiler

    c_compiler="$(command -v x86_64-w64-mingw32-gcc-posix || true)"
    cxx_compiler="$(command -v x86_64-w64-mingw32-g++-posix || true)"
    rm -rf "${MINGW_POSIX_SHIM_PATH}"

    if [[ -z "${c_compiler}" || -z "${cxx_compiler}" ]]; then
        warn "POSIX MinGW-w64 compilers were not found; using the system's generic thread-model alternatives."
        return
    fi

    # vcpkg's built-in MinGW toolchain searches only the generic compiler
    # names. Put stable aliases to the POSIX variants first on PATH so vcpkg
    # dependencies and the main chainloaded build use the same thread model.
    mkdir -p "${MINGW_POSIX_SHIM_PATH}"
    ln -s "${c_compiler}" "${MINGW_POSIX_SHIM_PATH}/x86_64-w64-mingw32-gcc"
    ln -s "${cxx_compiler}" "${MINGW_POSIX_SHIM_PATH}/x86_64-w64-mingw32-g++"
    export PATH="${MINGW_POSIX_SHIM_PATH}:${PATH}"
    USING_MINGW_POSIX_THREADS=1
    ok "Using the POSIX MinGW-w64 thread model for vcpkg and BiasedDoom"
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${script_dir}/.." && pwd)"

BUILD_TYPE="${BUILD_TYPE:-Release}"
NATIVE_TOOLS_DIR="${NATIVE_TOOLS_DIR:-build-native-tools}"
BUILD_DIR="${BUILD_DIR:-build-windows-mingw}"
ARTIFACT_DIR="${ARTIFACT_DIR:-artifacts}"
TRIPLET="${VCPKG_TARGET_TRIPLET:-x64-mingw-static}"
JOBS="${NUM_JOBS:-$(host_jobs)}"
INSTALL_PREFIX=""

CLEAN=0
CONFIGURE_ONLY=0
BUILD_ONLY=0
PACKAGE=0
INSTALL=0
ENABLE_GLTF=ON
ENABLE_VULKAN=ON
OPENAL_VCPKG=ON
LIBSNDFILE_VCPKG=ON
LIBVPX_VCPKG=ON
USING_MINGW_POSIX_THREADS=0
REBUILD_MINGW=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        --configure-only)
            CONFIGURE_ONLY=1
            shift
            ;;
        --build-only)
            BUILD_ONLY=1
            shift
            ;;
        --package)
            PACKAGE=1
            shift
            ;;
        --build-type)
            [[ $# -ge 2 ]] || die "--build-type requires a value."
            BUILD_TYPE="$2"
            shift 2
            ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs requires a value."
            JOBS="$2"
            shift 2
            ;;
        --native-tools-dir)
            [[ $# -ge 2 ]] || die "--native-tools-dir requires a value."
            NATIVE_TOOLS_DIR="$2"
            shift 2
            ;;
        --build-dir)
            [[ $# -ge 2 ]] || die "--build-dir requires a value."
            BUILD_DIR="$2"
            shift 2
            ;;
        --artifact-dir)
            [[ $# -ge 2 ]] || die "--artifact-dir requires a value."
            ARTIFACT_DIR="$2"
            shift 2
            ;;
        --triplet)
            [[ $# -ge 2 ]] || die "--triplet requires a value."
            TRIPLET="$2"
            shift 2
            ;;
        --install)
            INSTALL=1
            shift
            ;;
        --install-prefix)
            [[ $# -ge 2 ]] || die "--install-prefix requires a value."
            INSTALL=1
            INSTALL_PREFIX="$2"
            shift 2
            ;;
        --no-gltf)
            ENABLE_GLTF=OFF
            shift
            ;;
        --no-vulkan)
            ENABLE_VULKAN=OFF
            shift
            ;;
        --no-openal-vcpkg)
            OPENAL_VCPKG=OFF
            shift
            ;;
        --no-libsndfile-vcpkg)
            LIBSNDFILE_VCPKG=OFF
            shift
            ;;
        --no-libvpx-vcpkg)
            LIBVPX_VCPKG=OFF
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

case "${BUILD_TYPE}" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    *) die "--build-type must be Debug, Release, RelWithDebInfo, or MinSizeRel." ;;
esac

[[ "${JOBS}" =~ ^[0-9]+$ && "${JOBS}" -gt 0 ]] || die "--jobs must be a positive integer."

NATIVE_TOOLS_PATH="$(resolve_under_repo "${NATIVE_TOOLS_DIR}")"
BUILD_PATH="$(resolve_under_repo "${BUILD_DIR}")"
ARTIFACT_PATH="$(resolve_under_repo "${ARTIFACT_DIR}")"
TOOLCHAIN_PATH="${REPO_ROOT}/cmake/toolchains/mingw-w64-x86_64.cmake"
IMPORT_FILE="${NATIVE_TOOLS_PATH}/ImportExecutables.cmake"
VCPKG_TOOLCHAIN_FILE=""
MINGW_POSIX_SHIM_PATH="${BUILD_PATH}-toolchain-bin"

get_vcpkg_gitlink_commit() {
    git -C "${REPO_ROOT}" ls-tree HEAD vcpkg 2>/dev/null | awk '{print $3}'
}

detect_stale_mingw_thread_model() {
    local prefix="${BUILD_PATH}/vcpkg_installed/${TRIPLET}"
    local archives=()
    local archive

    [[ "${USING_MINGW_POSIX_THREADS}" -eq 1 && "${CLEAN}" -eq 0 ]] || return 0

    shopt -s nullglob
    archives=("${prefix}/lib/"*.a "${prefix}/debug/lib/"*.a)
    shopt -u nullglob

    for archive in "${archives[@]}"; do
        # A Win32-thread-model libstdc++ archive cannot be linked into the
        # POSIX-thread-model build selected above. Catch old vcpkg artifacts
        # before the final link instead of failing with __gthr_win32_* errors.
        if x86_64-w64-mingw32-nm -u "${archive}" 2>/dev/null |
            awk '/__gthr_win32_/ { found = 1 } END { exit !found }'; then
            if [[ "${BUILD_ONLY}" -eq 1 ]]; then
                die "${archive} uses MinGW's Win32 thread model, but this build uses the POSIX thread model. Rerun with --clean (without --build-only)."
            fi
            warn "Found stale Win32-thread-model vcpkg archive: ${archive}"
            warn "The MinGW build directory will be rebuilt with a consistent POSIX thread runtime."
            REBUILD_MINGW=1
            return
        fi
    done
}

ensure_vcpkg() {
    local vcpkg_dir="${REPO_ROOT}/vcpkg"
    local toolchain="${vcpkg_dir}/scripts/buildsystems/vcpkg.cmake"
    local commit

    log "Preparing vcpkg"
    if [[ ! -f "${toolchain}" ]]; then
        commit="$(get_vcpkg_gitlink_commit || true)"
        rm -rf "${vcpkg_dir}"
        run git clone https://github.com/microsoft/vcpkg.git "${vcpkg_dir}"
        if [[ -n "${commit}" ]]; then
            run git -C "${vcpkg_dir}" checkout "${commit}"
        fi
    fi

    # This is also a quick `vcpkg version` health check for an executable left
    # by an interrupted earlier package build. It returns immediately when the
    # pinned tool is usable, otherwise it performs the bounded recovery path.
    run "${REPO_ROOT}/tools/bootstrap-vcpkg.sh" "${vcpkg_dir}"

    ok "vcpkg is ready"
    VCPKG_TOOLCHAIN_FILE="${toolchain}"
}

configure_native_tools() {
    if [[ "${BUILD_ONLY}" -eq 1 ]]; then
        [[ -f "${IMPORT_FILE}" ]] || die "--build-only needs ${IMPORT_FILE}; run once without --build-only first."
        return
    fi

    if [[ "${CLEAN}" -eq 1 ]]; then
        rm -rf "${NATIVE_TOOLS_PATH}"
    fi

    log "Configuring host tools for cross compilation"
    run cmake -S "${REPO_ROOT}" -B "${NATIVE_TOOLS_PATH}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBIASEDDOOM_ENABLE_ONLINE=OFF \
        -DBIASEDDOOM_ENABLE_GLTF=OFF \
        -DBIASEDDOOM_BUILD_GLTF=OFF \
		-DBIASEDDOOM_ENABLE_PYTHON=OFF \
        -DNEODOOM_ENABLE_GLTF=OFF \
        -DPK3_QUIET_ZIPDIR=ON

    log "Building host tools"
    run cmake --build "${NATIVE_TOOLS_PATH}" --target zipdir lemon re2c --parallel "${JOBS}"

    [[ -f "${IMPORT_FILE}" ]] || die "Expected host tool export was not created: ${IMPORT_FILE}"
}

configure_mingw() {
    local vcpkg_toolchain="$1"
    local cmake_args=(
        -S "${REPO_ROOT}"
        -B "${BUILD_PATH}"
        -G Ninja
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
        -DCMAKE_TOOLCHAIN_FILE="${vcpkg_toolchain}"
        -DVCPKG_TARGET_TRIPLET="${TRIPLET}"
        -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="${TOOLCHAIN_PATH}"
        -DIMPORT_EXECUTABLES="${IMPORT_FILE}"
        -DBIASEDDOOM_ENABLE_GLTF="${ENABLE_GLTF}"
        -DBIASEDDOOM_BUILD_GLTF="${ENABLE_GLTF}"
		-DBIASEDDOOM_ENABLE_PYTHON=OFF
		-DBIASEDDOOM_BUILD_AUDIO_TESTS=ON
        -DNEODOOM_ENABLE_GLTF="${ENABLE_GLTF}"
        -DLIBVPX_VCPKG="${LIBVPX_VCPKG}"
        -DOPENAL_SOFT_VCPKG="${OPENAL_VCPKG}"
        -DVCPKG_LIBSNDFILE="${LIBSNDFILE_VCPKG}"
        -DHAVE_VULKAN="${ENABLE_VULKAN}"
        -DPK3_QUIET_ZIPDIR=ON
    )

    if [[ "${TRIPLET}" == *mingw* && "${LIBVPX_VCPKG}" == "ON" ]]; then
        # The current vcpkg libvpx manifest pulls a host helper marked as
        # native-Windows-only before continuing into its MinGW build branch.
        cmake_args+=(-DVCPKG_INSTALL_OPTIONS=--allow-unsupported)
    fi

    if [[ "${OPENAL_VCPKG}" == "ON" ]]; then
        cmake_args+=(-DDYN_OPENAL=OFF)
    else
        cmake_args+=(-DDYN_OPENAL=ON)
    fi

    if [[ "${LIBSNDFILE_VCPKG}" == "ON" ]]; then
        cmake_args+=(-DDYN_SNDFILE=OFF)
    else
        cmake_args+=(-DDYN_SNDFILE=ON)
    fi

    if [[ "${INSTALL}" -eq 1 && -n "${INSTALL_PREFIX}" ]]; then
        cmake_args+=(-DCMAKE_INSTALL_PREFIX="$(resolve_under_repo "${INSTALL_PREFIX}")")
    fi

    if [[ "${CLEAN}" -eq 1 || "${REBUILD_MINGW}" -eq 1 ]]; then
        rm -rf "${BUILD_PATH}"
    fi

    log "Configuring Windows x64 MinGW build"
    run cmake "${cmake_args[@]}"
}

repair_mingw_libvpx_variant() {
    local variant="$1"
    local install_prefix="$2"
    local archive="$3"
    local build_dir="${BUILD_PATH}/libvpx-mingw-repair-${variant}"
    local source_dir
    local configure_script
    local options=(
        --target=x86_64-win64-gcc
        --disable-examples
        --disable-tools
        --disable-docs
        --disable-unit-tests
        --enable-pic
        --enable-static
        --disable-shared
        --prefix="${install_prefix}"
        --as=nasm
    )

    if archive_is_mingw_coff "${archive}"; then
        ok "libvpx ${variant} archive is MinGW COFF"
        return
    fi

    warn "vcpkg produced a non-MinGW libvpx ${variant} archive: $(archive_member_format "${archive}" || printf 'missing archive')"
    log "Rebuilding libvpx ${variant} with MinGW-w64"

    configure_script="$(find "${REPO_ROOT}/vcpkg/buildtrees/libvpx/src" -maxdepth 2 -type f -name configure | sort | head -n 1)"
    [[ -n "${configure_script}" ]] || die "Could not find vcpkg's extracted libvpx configure script."
    source_dir="$(dirname "${configure_script}")"

    if [[ "${variant}" == "debug" ]]; then
        options+=(--enable-debug-libs --enable-debug)
    fi

    rm -rf "${build_dir}"
    mkdir -p "${build_dir}"
    (
        cd "${build_dir}"
        export CROSS=x86_64-w64-mingw32-
        run bash --noprofile --norc "${source_dir}/configure" "${options[@]}"
        run make -j"${JOBS}"
        run make install
    )

    archive_is_mingw_coff "${archive}" || die "libvpx ${variant} archive is still not a MinGW COFF archive after rebuild."
    ok "Rebuilt libvpx ${variant} archive: ${archive}"
}

repair_mingw_libvpx() {
    local prefix="${BUILD_PATH}/vcpkg_installed/${TRIPLET}"
    local release_archive="${prefix}/lib/libvpx.a"
    local debug_archive="${prefix}/debug/lib/libvpx.a"

    [[ "${TRIPLET}" == *mingw* && "${LIBVPX_VCPKG}" == "ON" ]] || return 0

    log "Validating vcpkg libvpx for MinGW"
    repair_mingw_libvpx_variant release "${prefix}" "${release_archive}"

    if [[ "${BUILD_TYPE}" == "Debug" ]]; then
        repair_mingw_libvpx_variant debug "${prefix}/debug" "${debug_archive}"
    elif [[ -f "${debug_archive}" ]] && ! archive_is_mingw_coff "${debug_archive}"; then
        repair_mingw_libvpx_variant debug "${prefix}/debug" "${debug_archive}"
    fi
}

find_biaseddoom_exe() {
    local candidates=(
        "${BUILD_PATH}/biaseddoom.exe"
        "${BUILD_PATH}/Release/biaseddoom.exe"
        "${BUILD_PATH}/bin/biaseddoom.exe"
        "${BUILD_PATH}/${BUILD_TYPE}/biaseddoom.exe"
    )
    local candidate

    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    find "${BUILD_PATH}" -maxdepth 4 -type f -iname 'biaseddoom*.exe' | head -n 1
}

find_audio_probe() {
    local candidates=(
        "${BUILD_PATH}/biaseddoom-audio-probe.exe"
        "${BUILD_PATH}/Release/biaseddoom-audio-probe.exe"
        "${BUILD_PATH}/${BUILD_TYPE}/biaseddoom-audio-probe.exe"
    )
    local candidate

    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    find "${BUILD_PATH}" -maxdepth 4 -type f -name 'biaseddoom-audio-probe.exe' | head -n 1
}

validate_static_audio() {
    local exe="$1"
    local exe_dir
    local zmusic
    local imports
    local symbols

    exe_dir="$(dirname "${exe}")"
    zmusic="${exe_dir}/libzmusic.dll"
    [[ -f "${zmusic}" ]] || zmusic="${BUILD_PATH}/libzmusic.dll"
    [[ -f "${zmusic}" ]] || die "Audio validation could not find libzmusic.dll."

    if [[ "${OPENAL_VCPKG}" == "ON" ]]; then
        imports="$(x86_64-w64-mingw32-objdump -p "${exe}" | sed -n 's/^[[:space:]]*DLL Name: //p')"
        if grep -Eiq '(^|[/\\])(openal32|soft_oal)\.dll$' <<<"${imports}"; then
            die "biaseddoom.exe still imports a loose OpenAL DLL."
        fi
        if ! strings -a "${exe}" |
            awk 'index($0, "OpenAL Soft") { found = 1 } END { exit !found }'; then
            die "OpenAL Soft was not found in the linked Windows executable."
        fi
    fi

    if [[ "${LIBSNDFILE_VCPKG}" == "ON" ]]; then
        imports="$(x86_64-w64-mingw32-objdump -p "${zmusic}" | sed -n 's/^[[:space:]]*DLL Name: //p')"
        if grep -Eiq '(sndfile|mpg123|vorbis|opus|ogg|flac).*\.dll$' <<<"${imports}"; then
            die "libzmusic.dll still imports a loose compressed-audio decoder DLL."
        fi

        symbols="$(x86_64-w64-mingw32-nm -g "${zmusic}")"
        for symbol in sf_open_virtual vorbis_synthesis opus_decoder_create mpg123_init; do
            if ! grep -Eq "[[:space:]]${symbol}$" <<<"${symbols}"; then
                die "Static codec validation did not find ${symbol} in libzmusic.dll."
            fi
        done
    fi

    ok "Windows audio dependencies are linked and compressed codecs are present"
}

copy_runtime_files() {
    local source_dir="$1"
    local destination_dir="$2"
    local files=()
    local path

    [[ -d "${source_dir}" ]] || return 0

    shopt -s nullglob
    files=("${source_dir}"/*.pk3 "${source_dir}"/*.dll)
    shopt -u nullglob

    if (( ${#files[@]} > 0 )); then
        cp -a "${files[@]}" "${destination_dir}/"
    fi

    for path in "${source_dir}/soundfonts" "${source_dir}/fm_banks"; do
        if [[ -d "${path}" ]]; then
            rm -rf "${destination_dir:?}/$(basename "${path}")"
            cp -a "${path}" "${destination_dir}/"
        fi
    done
}

copy_mingw_runtime_files() {
    local destination_dir="$1"
    local compiler="${CMAKE_CXX_COMPILER:-x86_64-w64-mingw32-g++}"
    local runtime
    local runtime_path

    for runtime in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
        runtime_path="$(${compiler} -print-file-name="${runtime}")"
        if [[ "${runtime_path}" == "${runtime}" || ! -f "${runtime_path}" ]]; then
            die "Could not locate required MinGW runtime ${runtime}."
        fi
        cp -a "${runtime_path}" "${destination_dir}/"
    done
}

run_audio_probe_with_wine() {
    local audio_probe="$1"
    local probe_dir
    local zmusic
    local temp_dir

    if [[ "${OPENAL_VCPKG}" != "ON" || "${LIBSNDFILE_VCPKG}" != "ON" ]]; then
        warn "Skipping the Wine audio probe because bundled OpenAL or libsndfile was disabled."
        return
    fi
    if ! command -v wine >/dev/null 2>&1; then
        warn "Wine was not found; static audio validation passed, but the Windows runtime probe was skipped."
        return
    fi

    probe_dir="$(dirname "${audio_probe}")"
    zmusic="${probe_dir}/libzmusic.dll"
    [[ -f "${zmusic}" ]] || zmusic="${BUILD_PATH}/libzmusic.dll"
    [[ -f "${zmusic}" ]] || die "Wine audio probe could not find libzmusic.dll."

    temp_dir="$(mktemp -d)"
    cp "${audio_probe}" "${zmusic}" "${temp_dir}/"
    copy_mingw_runtime_files "${temp_dir}"
    cp "${REPO_ROOT}/wadsrc/static/sounds/dsquake.ogg" "${temp_dir}/"
    cp "${REPO_ROOT}/wadsrc/static/sounds/dssecret.flac" "${temp_dir}/"

    log "Running the Windows OpenAL/OGG/FLAC regression probe under Wine"
    if ! run env ALSOFT_DRIVERS=null WINEDEBUG=-all wine \
        "${temp_dir}/biaseddoom-audio-probe.exe" \
        "${temp_dir}/dsquake.ogg" "${temp_dir}/dssecret.flac"; then
        rm -rf "${temp_dir}"
        die "The Windows audio regression probe failed under Wine."
    fi
    rm -rf "${temp_dir}"
    ok "Windows OpenAL runtime and compressed-audio decoding passed under Wine"
}

write_package_readme() {
    local destination_dir="$1"

    cat > "${destination_dir}/README-Windows-MinGW.txt" <<'README'
BiasedDoom for Windows x64

This package was cross-compiled on Linux with MinGW-w64.

Run biaseddoom.exe to start the engine. You still need a supported IWAD such as DOOM2.WAD.

Example:
  biaseddoom.exe -iwad C:\Games\Doom\DOOM2.WAD

Wine smoke test from the extracted package directory:
  wine biaseddoom.exe -stdout -iwad doom2.wad +quit

Use -stdout when testing under Wine so startup errors are printed to the terminal.
The -norun diagnostic path intentionally pauses before closing in Windows GUI builds.

Keep the PK3 files, DLLs, soundfonts, and fm_banks folders beside biaseddoom.exe.
OpenAL Soft and the compressed-audio codecs are built in; do not copy OpenAL or sndfile DLLs from another source port into this folder.

This MinGW build does not contain Python scripting. ACS and ZScript work normally.
Use the native MSVC Windows package for Python mods.

For a complete startup/audio report that exits after checking, run:
  biaseddoom.exe -stdout -audiodiagnostics -norun

The snd_status and snd_listdrivers console commands add backend and device details to that log.
The automatic log is written to %LOCALAPPDATA%\biaseddoom\biaseddoom-audio.log, with a fallback beside biaseddoom.exe.
Disconnected endpoints are reopened automatically; if Windows temporarily has no usable output, BiasedDoom keeps retrying with bounded backoff.
See TROUBLESHOOTING.md for IWAD, audio, mod, and build diagnostics.
README
}

write_dependency_report() {
    local exe="$1"
    local destination_dir="$2"

    if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
        {
            printf 'DLL imports reported by x86_64-w64-mingw32-objdump:\n\n'
            x86_64-w64-mingw32-objdump -p "${exe}" | sed -n 's/^[[:space:]]*DLL Name: //p' | sort -u
        } > "${destination_dir}/DEPENDENCIES-MinGW.txt"
    fi
}

package_windows_zip() {
    local exe="$1"
    local version="$2"
    local package_name="BiasedDoom-${version}-Windows-x64-MinGW"
    local stage_dir="${ARTIFACT_PATH}/${package_name}"
    local zip_path="${ARTIFACT_PATH}/${package_name}.zip"
    local checksum_path="${zip_path}.sha256"
    local exe_dir
    local vcpkg_bin="${BUILD_PATH}/vcpkg_installed/${TRIPLET}/bin"

    log "Creating Windows MinGW package"
    rm -rf "${stage_dir}" "${zip_path}" "${checksum_path}"
    mkdir -p "${stage_dir}" "${ARTIFACT_PATH}"

    cp "${exe}" "${stage_dir}/biaseddoom.exe"
    exe_dir="$(dirname "${exe}")"

    copy_runtime_files "${exe_dir}" "${stage_dir}"
    copy_runtime_files "${BUILD_PATH}" "${stage_dir}"
    copy_runtime_files "${vcpkg_bin}" "${stage_dir}"
    copy_mingw_runtime_files "${stage_dir}"
    cp "${REPO_ROOT}/TROUBLESHOOTING.md" "${stage_dir}/"
    write_package_readme "${stage_dir}"
    write_dependency_report "${stage_dir}/biaseddoom.exe" "${stage_dir}"

    [[ -f "${stage_dir}/biaseddoom.exe" ]] || die "Package staging failed: biaseddoom.exe is missing."
    [[ -f "${stage_dir}/libzmusic.dll" ]] || die "Package staging failed: libzmusic.dll is missing."
    [[ -f "${stage_dir}/TROUBLESHOOTING.md" ]] || die "Package staging failed: TROUBLESHOOTING.md is missing."
    compgen -G "${stage_dir}/*.pk3" >/dev/null || die "Package staging failed: no PK3 resource files were found."
    if [[ "${OPENAL_VCPKG}" == "ON" ]] && compgen -G "${stage_dir}/[Oo]pen[Aa][Ll]*.dll" >/dev/null; then
        die "Package staging failed: bundled OpenAL build contains a loose OpenAL DLL."
    fi
    if [[ "${LIBSNDFILE_VCPKG}" == "ON" ]] && find "${stage_dir}" -maxdepth 1 -type f \( -iname '*sndfile*.dll' -o -iname '*mpg123*.dll' \) -print -quit | grep -q .; then
        die "Package staging failed: bundled codec build contains a loose decoder DLL."
    fi

    (cd "${ARTIFACT_PATH}" && cmake -E tar cf "${zip_path}" --format=zip "${package_name}")
    (cd "${ARTIFACT_PATH}" && sha256sum "$(basename "${zip_path}")" > "$(basename "${checksum_path}")")

    ok "Package: ${zip_path}"
    ok "Checksum: ${checksum_path}"
}

version_from_source() {
    awk -F'"' '/^#define VERSIONSTR / {print $2; exit}' "${REPO_ROOT}/src/version.h"
}

log "Checking tools"
require_command git "Install git."
require_command cmake "Install CMake 3.16 or newer."
require_command ninja "Install ninja-build."
require_command x86_64-w64-mingw32-gcc "Install with: sudo apt install mingw-w64 g++-mingw-w64 gcc-mingw-w64"
require_command x86_64-w64-mingw32-g++ "Install with: sudo apt install mingw-w64 g++-mingw-w64 gcc-mingw-w64"
require_command x86_64-w64-mingw32-windres "Install with: sudo apt install mingw-w64 g++-mingw-w64 gcc-mingw-w64"
require_command x86_64-w64-mingw32-objdump "Install with: sudo apt install mingw-w64"
require_command x86_64-w64-mingw32-nm "Install with: sudo apt install mingw-w64"
require_command nasm "Install with: sudo apt install nasm"
require_command autoconf "Install with: sudo apt install autoconf autoconf-archive automake libtool"
require_command autoreconf "Install with: sudo apt install autoconf autoconf-archive automake libtool"
require_command automake "Install with: sudo apt install autoconf autoconf-archive automake libtool"
require_command libtoolize "Install with: sudo apt install autoconf autoconf-archive automake libtool"
require_autoconf_archive
ok "MinGW-w64 tools found"
prefer_mingw_posix_thread_model
detect_stale_mingw_thread_model

ensure_vcpkg
configure_native_tools

if [[ "${BUILD_ONLY}" -ne 1 ]]; then
    configure_mingw "${VCPKG_TOOLCHAIN_FILE}"
fi
repair_mingw_libvpx

if [[ "${CONFIGURE_ONLY}" -eq 1 ]]; then
    ok "Configure-only MinGW run complete"
    exit 0
fi

log "Building Windows x64 MinGW executable"
run cmake --build "${BUILD_PATH}" --config "${BUILD_TYPE}" --parallel "${JOBS}"

exe="$(find_biaseddoom_exe)"
[[ -n "${exe}" && -f "${exe}" ]] || die "Build finished, but biaseddoom.exe was not found under ${BUILD_PATH}."
ok "Executable: ${exe}"
audio_probe="$(find_audio_probe)"
[[ -n "${audio_probe}" && -f "${audio_probe}" ]] || die "Build finished, but biaseddoom-audio-probe.exe was not found."
validate_static_audio "${exe}"
run_audio_probe_with_wine "${audio_probe}"

if [[ "${INSTALL}" -eq 1 ]]; then
    install_args=(--install "${BUILD_PATH}" --config "${BUILD_TYPE}")
    if [[ -n "${INSTALL_PREFIX}" ]]; then
        install_args+=(--prefix "$(resolve_under_repo "${INSTALL_PREFIX}")")
    fi
    log "Installing Windows x64 MinGW build"
    run cmake "${install_args[@]}"
fi

if [[ "${PACKAGE}" -eq 1 ]]; then
    version="$(version_from_source)"
    [[ -n "${version}" ]] || version="dev"
    package_windows_zip "${exe}" "${version}"
fi

ok "Windows MinGW build complete"
