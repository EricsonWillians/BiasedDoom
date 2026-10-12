# Linux-hosted x86_64-w64-mingw32 toolchain for producing Windows binaries.
#
# This file is intended to be used through vcpkg's chainload hook:
#
#   -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
#   -DVCPKG_TARGET_TRIPLET=x64-mingw-static
#   -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(BIASEDDOOM_MINGW_C_COMPILER
	NAMES x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc)
find_program(BIASEDDOOM_MINGW_CXX_COMPILER
	NAMES x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++)

if(NOT BIASEDDOOM_MINGW_C_COMPILER OR NOT BIASEDDOOM_MINGW_CXX_COMPILER)
	message(FATAL_ERROR "A complete x86_64 MinGW-w64 C/C++ toolchain was not found")
endif()

# Debian and Ubuntu install both POSIX and Win32 thread-model variants. Prefer
# POSIX because fastgltf's simdjson dependency requires the standard C++
# threading primitives; the generic alternatives may otherwise resolve to the
# Win32 variant, where std::thread and std::mutex are unavailable.
set(CMAKE_C_COMPILER "${BIASEDDOOM_MINGW_C_COMPILER}" CACHE FILEPATH "MinGW-w64 C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${BIASEDDOOM_MINGW_CXX_COMPILER}" CACHE FILEPATH "MinGW-w64 C++ compiler" FORCE)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres CACHE FILEPATH "MinGW-w64 resource compiler")

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# vcpkg's generated compatibility wrappers probe their installation prefix
# with absolute PATHS and NO_DEFAULT_PATH. With an ONLY root mode, CMake
# rewrites those paths below /usr/x86_64-w64-mingw32 and hides an otherwise
# valid MinGW package (notably zlib while CURL is being discovered). BOTH
# keeps the sysroot lookup first, then permits the explicit vcpkg prefix.
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
