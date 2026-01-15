#!/usr/bin/env bash
#
# BiasedDoom Safe Build Cleaner
#
# Removes the last build directory safely with guardrails and confirmations.
# Defaults to removing the "build" directory under the repo root. You can
# override via --build-dir or BUILD_DIR env var. Supports dry-run.
#
# Usage:
#   ./clean-build.sh [--build-dir DIR] [--yes] [--dry-run] [--cmake-only]
#
# Options:
#   --build-dir DIR  Path to build directory (default: ./build)
#   --yes            Do not prompt for confirmation (non-interactive)
#   --dry-run        Show what would be deleted without deleting
#   --cmake-only     Only remove CMake cache/files inside build dir
#
set -euo pipefail

# Formatting
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_BUILD_DIR="build"

BUILD_DIR="${BUILD_DIR:-$DEFAULT_BUILD_DIR}"
ASSUME_YES=false
DRY_RUN=false
CMAKE_ONLY=false

log() { # level message...
  local level="$1"; shift
  local msg="$*"
  case "$level" in
    INFO)  echo -e "${GREEN}${BOLD}✓ INFO${NC} $msg";;
    WARN)  echo -e "${YELLOW}${BOLD}⚠ WARNING${NC} ${YELLOW}$msg${NC}";;
    ERROR) echo -e "${RED}${BOLD}✗ ERROR${NC} ${RED}$msg${NC}";;
    STAGE) echo -e "\n${BLUE}${BOLD}—— $msg ——${NC}";;
    *)     echo "$msg";;
  esac
}

usage() {
  sed -n '1,40p' "${BASH_SOURCE[0]}" | sed -n '1,30p' | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -lt 2 ]] && { log ERROR "--build-dir requires a path"; exit 2; }
      BUILD_DIR="$2"; shift 2;;
    --yes|-y)
      ASSUME_YES=true; shift;;
    --dry-run)
      DRY_RUN=true; shift;;
    --cmake-only)
      CMAKE_ONLY=true; shift;;
    --help|-h)
      usage; exit 0;;
    *)
      log ERROR "Unknown argument: $1"; usage; exit 2;;
  esac
done

# Resolve absolute path to build dir
case "$BUILD_DIR" in
  /*) BUILD_ABS="$BUILD_DIR";;
  *)  BUILD_ABS="$PROJECT_ROOT/$BUILD_DIR";;
esac

# Safety checks
if [[ -z "$BUILD_ABS" ]]; then
  log ERROR "Resolved build directory is empty (internal error)."; exit 1
fi

# Ensure build dir is inside project root
PROJECT_ROOT_ABS="$(cd "$PROJECT_ROOT" && pwd)"
# Normalize build path robustly across environments
if command -v realpath >/dev/null 2>&1; then
  BUILD_ABS_NORM="$(realpath -m -- "$BUILD_ABS")"
elif command -v readlink >/dev/null 2>&1; then
  BUILD_ABS_NORM="$(readlink -f -- "$BUILD_ABS" 2>/dev/null || echo "$BUILD_ABS")"
elif command -v python3 >/dev/null 2>&1; then
  BUILD_ABS_NORM="$(python3 - "$BUILD_ABS" <<'PY'
import os, sys
print(os.path.realpath(sys.argv[1]))
PY
)"
else
  BUILD_ABS_NORM="$BUILD_ABS"
fi

case "$BUILD_ABS_NORM" in
  "$PROJECT_ROOT_ABS"|"$PROJECT_ROOT_ABS"/*) ;;
  *)
    log ERROR "Refusing to operate outside project root: $BUILD_ABS_NORM"; exit 1;;
esac

if [[ ! -d "$BUILD_ABS_NORM" ]]; then
  log INFO "No build directory found at: $BUILD_ABS_NORM"
  exit 0
fi

log STAGE "Preparing clean"
log INFO "Project root: $PROJECT_ROOT_ABS"
log INFO "Build dir:    $BUILD_ABS_NORM"

# What will be removed
if $CMAKE_ONLY; then
  TO_DELETE=(
    "$BUILD_ABS_NORM/CMakeCache.txt"
    "$BUILD_ABS_NORM/CMakeFiles"
    "$BUILD_ABS_NORM/cmake_install.cmake"
    "$BUILD_ABS_NORM/Makefile"
    "$BUILD_ABS_NORM/build.ninja"
    "$BUILD_ABS_NORM/rules.ninja"
  )
  ACTION_DESC="CMake cache and generated files"
else
  TO_DELETE=("$BUILD_ABS_NORM")
  ACTION_DESC="Entire build directory"
fi

log INFO "Action: $ACTION_DESC"

# Show size if removing entire dir
if ! $CMAKE_ONLY; then
  if command -v du >/dev/null 2>&1; then
    SIZE=$(du -sh "$BUILD_ABS_NORM" 2>/dev/null | awk '{print $1}')
    [[ -n "$SIZE" ]] && log INFO "Size to remove: $SIZE"
  fi
fi

echo -e "${DIM}Items:${NC}"
for p in "${TO_DELETE[@]}"; do
  echo "  - $p"
done

if $DRY_RUN; then
  log INFO "Dry run: nothing deleted"
  exit 0
fi

if ! $ASSUME_YES; then
  read -r -p "Proceed with deletion? [y/N]: " REPLY
  case "$REPLY" in
    [yY][eE][sS]|[yY]) ;;
    *) log INFO "Aborted by user"; exit 0;;
  esac
fi

log STAGE "Cleaning"

safe_rm() {
  local target="$1"
  [[ -z "$target" ]] && { log ERROR "Internal error: empty target"; return 1; }
  # Normalize
  local tnorm
  tnorm="$(realpath -m "$target" 2>/dev/null || echo "$target")"
  case "$tnorm" in
    "$PROJECT_ROOT_ABS"|"$PROJECT_ROOT_ABS"/*) ;;
    *) log ERROR "Refusing to delete outside project: $tnorm"; return 1;;
  esac
  if [[ -e "$tnorm" ]]; then
    rm -rf --one-file-system -- "$tnorm"
  fi
}

errors=0
for p in "${TO_DELETE[@]}"; do
  if [[ -e "$p" ]]; then
    if ! safe_rm "$p"; then
      errors=$((errors+1))
    fi
  fi
done

if (( errors > 0 )); then
  log ERROR "Completed with $errors error(s). Some items may remain."
  exit 1
fi

if $CMAKE_ONLY; then
  log INFO "CMake cache cleaned: $BUILD_ABS_NORM"
else
  log INFO "Build directory removed: $BUILD_ABS_NORM"
fi

exit 0
