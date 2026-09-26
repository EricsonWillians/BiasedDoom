#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: tools/check-appimage-deps.sh PATH-TO.AppImage

Extract an AppImage and verify that every ELF dependency resolves either inside
the bundle or from a deliberately small set of kernel, glibc, and hardware
interface libraries. This catches "works only on the build host" AppImages even
when the build host happens to provide every missing library.
USAGE
}

if [[ $# -ne 1 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit $(( $# == 1 ? 0 : 1 ))
fi

appimage="$(realpath "$1")"
[[ -f "${appimage}" ]] || { echo "error: AppImage not found: $1" >&2; exit 1; }
[[ -x "${appimage}" ]] || { echo "error: AppImage is not executable: $1" >&2; exit 1; }

require_command() {
    command -v "$1" >/dev/null 2>&1 || { echo "error: required command not found: $1" >&2; exit 1; }
}

require_command file
require_command find
require_command mktemp

loader="/lib64/ld-linux-x86-64.so.2"
[[ -x "${loader}" ]] || { echo "error: dynamic loader not found: ${loader}" >&2; exit 1; }

extract_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-appimage-check.XXXXXX")"
violations="$(mktemp "${TMPDIR:-/tmp}/biaseddoom-appimage-violations.XXXXXX")"
cleanup() {
    rm -rf "${extract_root}" "${violations}"
}
trap cleanup EXIT

(
    cd "${extract_root}"
    "${appimage}" --appimage-extract >/dev/null
)

root="${extract_root}/squashfs-root"
[[ -d "${root}" ]] || { echo "error: AppImage extraction did not produce squashfs-root" >&2; exit 1; }
root="$(realpath "${root}")"

if grep -q '^APPDIR_LIBC_LINKER_PATH={' "${root}/AppRun.env"; then
    echo "error: AppRun.env contains malformed APPDIR_LIBC_LINKER_PATH set literal" >&2
    exit 1
fi
if grep -q '^APPDIR_LIBC_VERSION=' "${root}/AppRun.env" && \
   [[ ! -e "${root}/runtime/compat/lib64/ld-linux-x86-64.so.2" ]]; then
    echo "error: bundled libc compat runtime is missing its dynamic loader" >&2
    exit 1
fi

library_path="${root}/usr/bin:${root}/usr/lib:${root}/usr/lib/x86_64-linux-gnu:${root}/usr/lib/x86_64-linux-gnu/pulseaudio:${root}/lib:${root}/lib/x86_64-linux-gnu:${root}/lib64:${root}/runtime/compat/lib64:${root}/runtime/compat/lib/x86_64-linux-gnu:${root}/runtime/compat/usr/lib/x86_64-linux-gnu"
if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
    library_path+=":${LD_LIBRARY_PATH}"
fi

is_allowed_system_library() {
    case "$1" in
        # Kernel/glibc runtime interfaces. These intentionally remain external;
        # the AppImage is built on the oldest supported Ubuntu glibc baseline.
        linux-vdso.so.*|ld-linux*.so.*|lib64/ld-linux*.so.*|ld-musl-*.so.*|\
        libc.so.6|libm.so.6|libmvec.so.1|libdl.so.2|libpthread.so.0|\
        librt.so.1|libresolv.so.2|libutil.so.1|libanl.so.1|libnss_*.so.2)
            return 0
            ;;
        # Generic graphics-driver interfaces must come from the host so the
        # bundle does not mask or conflict with the installed GPU driver stack.
        libGL.so.1|libEGL.so.1|libGLESv2.so.2|libOpenGL.so.0|libGLX.so.0|\
        libGLdispatch.so.0|libvulkan.so.1)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

elf_count=0
while IFS= read -r -d '' object; do
    if ! file -b "${object}" | grep -q '^ELF '; then
        continue
    fi

    elf_count=$((elf_count + 1))
    # Invoke the loader directly. Running the ldd shell wrapper with an
    # AppDir LD_LIBRARY_PATH would make the wrapper's own /bin/bash load
    # bundled libraries and pollute the dependency report.
    ldd_output="$("${loader}" --library-path "${library_path}" --list "${object}" 2>&1 || true)"
    relative_object="${object#"${root}/"}"

    while IFS= read -r missing_name; do
        [[ -n "${missing_name}" ]] || continue
        printf '%s\t%s => not found\n' "${relative_object}" "${missing_name}" >> "${violations}"
    done < <(awk '$2 == "=>" && $3 == "not" && $4 == "found" {print $1}' <<< "${ldd_output}")

    while IFS=$'\t' read -r library_name resolved_path; do
        [[ -n "${library_name}" && -n "${resolved_path}" ]] || continue

        if [[ "${resolved_path}" == "${root}/"* ]]; then
            continue
        fi
        if is_allowed_system_library "${library_name}"; then
            continue
        fi

        printf '%s\t%s => %s\n' "${relative_object}" "${library_name}" "${resolved_path}" >> "${violations}"
    done < <(
        awk '
            $2 == "=>" && $3 ~ /^\// {print $1 "\t" $3}
            $1 ~ /^\// {n = split($1, parts, "/"); print parts[n] "\t" $1}
        ' <<< "${ldd_output}"
    )
done < <(find "${root}" -type f -print0)

if [[ -s "${violations}" ]]; then
    echo "error: AppImage dependencies are not self-contained:" >&2
    sort -u "${violations}" >&2
    exit 1
fi

if [[ "${elf_count}" -eq 0 ]]; then
    echo "error: no ELF objects found in extracted AppImage" >&2
    exit 1
fi

echo "AppImage dependency closure OK (${elf_count} ELF objects checked)."
