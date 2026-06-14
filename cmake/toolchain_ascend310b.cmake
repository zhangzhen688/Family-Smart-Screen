#
# toolchain_ascend310b.cmake — Cross-compile for Huawei Ascend 310B ARM platform.
#
# Usage:
#   cmake -B build_arm \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_ascend310b.cmake \
#         -DSIMULATOR_LINUX=OFF \
#         -DCMAKE_BUILD_TYPE=Release
#   make -C build_arm -j$(nproc)
#
# Adjust ASCEND_SDK_PATH to point at your Ascend310B-sdk/ installation.
#

# ── SDK location ─────────────────────────────────────────────────────────
if(NOT DEFINED ASCEND_SDK_PATH)
    set(ASCEND_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../Ascend310B-sdk"
        CACHE PATH "Path to Ascend310B-sdk directory")
endif()

set(ASCEND_TOOLCHAIN "${ASCEND_SDK_PATH}/toolchain")

# ── System ───────────────────────────────────────────────────────────────
set(CMAKE_SYSTEM_NAME  Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_CROSSCOMPILING TRUE)

# ── Compilers ────────────────────────────────────────────────────────────
set(CMAKE_C_COMPILER   "${ASCEND_TOOLCHAIN}/bin/aarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${ASCEND_TOOLCHAIN}/bin/aarch64-linux-gnu-g++")
set(CMAKE_AR           "${ASCEND_TOOLCHAIN}/bin/aarch64-linux-gnu-ar"
    CACHE FILEPATH "Archiver")
set(CMAKE_STRIP        "${ASCEND_TOOLCHAIN}/bin/aarch64-linux-gnu-strip"
    CACHE FILEPATH "Strip")

# ── Sysroot ──────────────────────────────────────────────────────────────
set(CMAKE_SYSROOT "${ASCEND_TOOLCHAIN}/sysroot")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")

# ── Search policy — only look in sysroot (not host) ──────────────────────
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ── C/C++ flags ──────────────────────────────────────────────────────────
set(CMAKE_C_FLAGS_INIT   "-mcpu=cortex-a55 -march=armv8.2-a -O2")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a55 -march=armv8.2-a -O2")

# ── Linker flags ─────────────────────────────────────────────────────────
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-rpath-link=${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")

# ── pkg-config ───────────────────────────────────────────────────────────
set(PKG_CONFIG_EXECUTABLE "${ASCEND_TOOLCHAIN}/bin/aarch64-linux-gnu-pkg-config"
    CACHE FILEPATH "pkg-config for cross-compilation")

message(STATUS "Ascend 310B toolchain configured")
message(STATUS "  SDK:      ${ASCEND_SDK_PATH}")
message(STATUS "  CC:       ${CMAKE_C_COMPILER}")
message(STATUS "  CXX:      ${CMAKE_CXX_COMPILER}")
message(STATUS "  Sysroot:  ${CMAKE_SYSROOT}")
