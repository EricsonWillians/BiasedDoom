#!/bin/bash
#
# BiasedDoom Supreme Build Script with glTF 2.0 Support
#
# This script provides a comprehensive build system for BiasedDoom with:
# - Automatic vcpkg dependency management
# - glTF 2.0 support via fastgltf
# - Configurable build types (Debug/Release/RelWithDebInfo)
# - Parallel compilation optimization
# - Comprehensive error handling and logging
#

set -e  # Exit on error
set -o pipefail  # Catch errors in pipes

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}"
BUILD_DIR="${PROJECT_ROOT}/build"
VCPKG_DIR="${PROJECT_ROOT}/vcpkg"
LOG_FILE="${PROJECT_ROOT}/build.log"

# Build configuration
BUILD_TYPE="${BUILD_TYPE:-Debug}"  # Debug, Release, or RelWithDebInfo
CLEAN_BUILD="${CLEAN_BUILD:-false}"
NUM_JOBS="${NUM_JOBS:-$(nproc 2>/dev/null || echo 4)}"
VERBOSE="${VERBOSE:-false}"

# Feature flags
ENABLE_GLTF="${ENABLE_GLTF:-ON}"
BUILD_GLTF="${BUILD_GLTF:-ON}"
ENABLE_VULKAN="${ENABLE_VULKAN:-ON}"
ENABLE_OPENAL_VCPKG="${ENABLE_OPENAL_VCPKG:-OFF}"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# ============================================================================
# Utility Functions
# ============================================================================

print_header() {
    echo -e "${CYAN}╔════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║${NC}  $1"
    echo -e "${CYAN}╚════════════════════════════════════════════════════════════════╝${NC}"
}

print_step() {
    echo -e "${BLUE}[*]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[✓]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[!]${NC} $1"
}

print_error() {
    echo -e "${RED}[✗]${NC} $1"
}

log_command() {
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*" >> "${LOG_FILE}"
}

check_command() {
    if ! command -v "$1" &> /dev/null; then
        print_error "Required command '$1' not found. Please install it first."
        exit 1
    fi
}

# ============================================================================
# Build Functions
# ============================================================================

show_configuration() {
    print_header "BiasedDoom Build Configuration"
    echo "  Project Root:      ${PROJECT_ROOT}"
    echo "  Build Directory:   ${BUILD_DIR}"
    echo "  Build Type:        ${BUILD_TYPE}"
    echo "  Parallel Jobs:     ${NUM_JOBS}"
    echo "  Clean Build:       ${CLEAN_BUILD}"
    echo ""
    echo "  glTF Support:      ${ENABLE_GLTF}"
    echo "  glTF Build:        ${BUILD_GLTF}"
    echo "  Vulkan:            ${ENABLE_VULKAN}"
    echo "  OpenAL (vcpkg):    ${ENABLE_OPENAL_VCPKG}"
    echo ""
}

check_dependencies() {
    print_step "Checking system dependencies..."

    check_command cmake
    check_command git
    check_command ninja || check_command make

    # Check for C++ compiler
    if command -v g++ &> /dev/null; then
        print_success "Found g++ $(g++ --version | head -n1)"
    elif command -v clang++ &> /dev/null; then
        print_success "Found clang++ $(clang++ --version | head -n1)"
    else
        print_error "No C++ compiler found (g++ or clang++)"
        exit 1
    fi

    # Check CMake version
    CMAKE_VERSION=$(cmake --version | grep -oP '\d+\.\d+\.\d+' | head -n1)
    print_success "CMake ${CMAKE_VERSION} detected"
}

bootstrap_vcpkg() {
    print_step "Setting up vcpkg dependency manager..."

    if [ ! -d "${VCPKG_DIR}" ]; then
        print_step "Cloning vcpkg repository..."
        git clone https://github.com/Microsoft/vcpkg.git "${VCPKG_DIR}" >> "${LOG_FILE}" 2>&1
    fi

    cd "${VCPKG_DIR}"

    # Bootstrap vcpkg if not already done
    if [ ! -f "${VCPKG_DIR}/vcpkg" ]; then
        print_step "Bootstrapping vcpkg..."
        ./bootstrap-vcpkg.sh >> "${LOG_FILE}" 2>&1
        print_success "vcpkg bootstrapped successfully"
    else
        print_success "vcpkg already bootstrapped"
    fi

    cd "${PROJECT_ROOT}"
}

clean_build_directory() {
    if [ "${CLEAN_BUILD}" = "true" ]; then
        print_step "Performing clean build..."
        if [ -d "${BUILD_DIR}" ]; then
            print_warning "Removing existing build directory..."
            rm -rf "${BUILD_DIR}"
            print_success "Build directory cleaned"
        fi
    fi
}

configure_cmake() {
    print_step "Configuring CMake with glTF support..."

    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"

    # Build CMake arguments
    CMAKE_ARGS=(
        -S "${PROJECT_ROOT}"
        -B "${BUILD_DIR}"
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake"
        -DBIASEDDOOM_ENABLE_GLTF="${ENABLE_GLTF}"
        -DBIASEDDOOM_BUILD_GLTF="${BUILD_GLTF}"
        -DHAVE_VULKAN="${ENABLE_VULKAN}"
        -DOPENAL_SOFT_VCPKG="${ENABLE_OPENAL_VCPKG}"
    )

    # Use Ninja if available for faster builds
    if command -v ninja &> /dev/null; then
        CMAKE_ARGS+=(-G Ninja)
        print_step "Using Ninja build system"
    fi

    if [ "${VERBOSE}" = "true" ]; then
        CMAKE_ARGS+=(--debug-output)
    fi

    print_step "Running CMake configuration..."
    log_command "cmake ${CMAKE_ARGS[*]}"

    if cmake "${CMAKE_ARGS[@]}" 2>&1 | tee -a "${LOG_FILE}"; then
        print_success "CMake configuration successful"
    else
        print_error "CMake configuration failed. Check ${LOG_FILE} for details."
        exit 1
    fi

    cd "${PROJECT_ROOT}"
}

build_project() {
    print_step "Building BiasedDoom..."

    cd "${BUILD_DIR}"

    BUILD_ARGS=(
        --build "${BUILD_DIR}"
        --config "${BUILD_TYPE}"
        --parallel "${NUM_JOBS}"
    )

    if [ "${VERBOSE}" = "true" ]; then
        BUILD_ARGS+=(--verbose)
    fi

    print_step "Compiling with ${NUM_JOBS} parallel jobs..."
    log_command "cmake ${BUILD_ARGS[*]}"

    if cmake "${BUILD_ARGS[@]}" 2>&1 | tee -a "${LOG_FILE}"; then
        print_success "Build completed successfully!"
    else
        print_error "Build failed. Check ${LOG_FILE} for details."
        cd "${PROJECT_ROOT}"
        exit 1
    fi

    cd "${PROJECT_ROOT}"
}

verify_build() {
    print_step "Verifying build output..."

    EXECUTABLE="${BUILD_DIR}/biaseddoom"

    if [ -f "${EXECUTABLE}" ]; then
        print_success "Executable found: ${EXECUTABLE}"

        # Get file size
        SIZE=$(du -h "${EXECUTABLE}" | cut -f1)
        print_success "Binary size: ${SIZE}"

        # Check if executable is valid
        if file "${EXECUTABLE}" | grep -q "ELF.*executable"; then
            print_success "Valid ELF executable detected"
        fi

        # Check for glTF symbols (if nm is available)
        if command -v nm &> /dev/null; then
            if nm "${EXECUTABLE}" 2>/dev/null | grep -q "gltf\|GLTF"; then
                print_success "glTF symbols detected in binary"
            else
                print_warning "No glTF symbols found - glTF may not be compiled in"
            fi
        fi
    else
        print_error "Executable not found at ${EXECUTABLE}"
        exit 1
    fi
}

show_summary() {
    print_header "Build Summary"

    echo "  Status:            ${GREEN}SUCCESS${NC}"
    echo "  Build Type:        ${BUILD_TYPE}"
    echo "  Executable:        ${BUILD_DIR}/biaseddoom"
    echo "  Build Log:         ${LOG_FILE}"
    echo ""
    echo "  To run BiasedDoom:"
    echo "    cd ${BUILD_DIR}"
    echo "    ./biaseddoom"
    echo ""
}

# ============================================================================
# Main Build Process
# ============================================================================

main() {
    # Initialize log file
    echo "BiasedDoom Build Started: $(date)" > "${LOG_FILE}"

    print_header "BiasedDoom Supreme Build Script"
    echo ""

    show_configuration
    check_dependencies
    bootstrap_vcpkg
    clean_build_directory
    configure_cmake
    build_project
    verify_build
    show_summary

    print_success "All operations completed successfully!"
}

# ============================================================================
# Script Entry Point
# ============================================================================

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --release)
            BUILD_TYPE=Release
            shift
            ;;
        --debug)
            BUILD_TYPE=Debug
            shift
            ;;
        --relwithdebinfo)
            BUILD_TYPE=RelWithDebInfo
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --no-gltf)
            ENABLE_GLTF=OFF
            BUILD_GLTF=OFF
            shift
            ;;
        --jobs)
            NUM_JOBS="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --clean              Clean build directory before building"
            echo "  --release            Build in Release mode"
            echo "  --debug              Build in Debug mode (default)"
            echo "  --relwithdebinfo     Build with debug info"
            echo "  --verbose            Enable verbose output"
            echo "  --no-gltf            Disable glTF support"
            echo "  --jobs N             Use N parallel jobs (default: auto)"
            echo "  --help               Show this help message"
            echo ""
            echo "Environment Variables:"
            echo "  BUILD_TYPE           Set build type (Debug/Release/RelWithDebInfo)"
            echo "  CLEAN_BUILD          Set to 'true' for clean build"
            echo "  NUM_JOBS             Number of parallel compilation jobs"
            echo ""
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Run main build process
main
