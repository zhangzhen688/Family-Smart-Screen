cmake_minimum_required(VERSION 3.15)
project(smart_screen VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SIMULATOR_LINUX "Build for Linux (x86) simulator instead of ARM target" ON)

if(SIMULATOR_LINUX)
    add_compile_definitions(SIMULATOR_LINUX)
    message(STATUS "Building for Linux x86 simulator")
else()
    message(STATUS "Building for ARM target")
endif()

# ── Find system libraries used by LVGL ────────────────────────────────
find_package(Freetype REQUIRED)

# ── Include search paths (global) ─────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/inc)
include_directories(${CMAKE_SOURCE_DIR}/libs/cjson)
include_directories(${CMAKE_SOURCE_DIR}/libs/jsonrpc/include)
include_directories(${CMAKE_SOURCE_DIR}/libs/lvgl)
include_directories(/usr/include/freetype2)

# ── LVGL config path (must be set BEFORE lvgl subdirectory) ───────────
set(LV_CONF_PATH "${CMAKE_SOURCE_DIR}/gui/lv_conf.h")
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON)
set(LV_CONF_BUILD_DISABLE_DEMOS ON)

# ── Subdirectories ────────────────────────────────────────────────────
add_subdirectory(libs/cjson)
add_subdirectory(libs/jsonrpc)
add_subdirectory(libs/lvgl)

# ── Link freetype to LVGL ─────────────────────────────────────────────
target_link_libraries(lvgl PUBLIC freetype)
add_subdirectory(backend)
add_subdirectory(gui)
