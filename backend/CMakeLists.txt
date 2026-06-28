# Backend servers — each is a separate executable.

# ── Device Server (port 1234) ─────────────────────────────────────────
add_executable(device_server
    device_server/rpc_server.c
    device_server/dev_led.c
    device_server/dev_dht11.c
    device_server/dev_sg90.c
    device_server/dev_ap3216c.c
    device_server/driver_manager.c
)
target_include_directories(device_server PRIVATE ${CMAKE_SOURCE_DIR}/inc)
target_link_libraries(device_server jsonrpc cjson pthread m)

# ── Camera Server (port 1235) ─────────────────────────────────────────
add_executable(camera_server
    camera_server/rpc_server.c
    camera_server/v4l2_capture.c
    camera_server/camera_streamer.c
    camera_server/camera_relay.c
)
target_include_directories(camera_server PRIVATE ${CMAKE_SOURCE_DIR}/inc)
target_link_libraries(camera_server jsonrpc cjson pthread m rt)

# ── Voice Server (port 1236 + UDP IPC to xiaozhi control_center) ──────
add_executable(voice_server
    voice_server/rpc_server.c
    voice_server/xiaozhi_bridge.c
)
target_link_libraries(voice_server jsonrpc cjson pthread m)

# ── Web Remote Control Server (port 8080) ─────────────────────────────
add_executable(web_server
    web_server/main.c
    web_server/http_server.c
    camera_server/v4l2_capture.c
    ../gui/rpc_client.c
)
target_include_directories(web_server PRIVATE
    ${CMAKE_SOURCE_DIR}/gui
    ${CMAKE_SOURCE_DIR}/libs/cjson
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_server
)
target_link_libraries(web_server jsonrpc cjson pthread m)
target_compile_definitions(web_server PRIVATE SIMULATOR_LINUX)

# ── XiaoZhi Control Center (WebSocket to cloud, UDP IPC) ──────────────
# Dependencies: libssl-dev, websocketpp, libcurl-dev (optional)
find_package(OpenSSL QUIET)
find_package(CURL QUIET)

# Select HTTP implementation based on CURL availability
if(CURL_FOUND)
    set(XZ_HTTP_SRC voice_server/xiaozhi/http.cpp)
    message(STATUS "XiaoZhi HTTP: using libcurl")
else()
    set(XZ_HTTP_SRC voice_server/xiaozhi/http_stub.cpp)
    message(STATUS "XiaoZhi HTTP: using stub (no libcurl)")
endif()

set(XZ_CC_SRCS
    voice_server/xiaozhi/ipc_udp.cpp
    voice_server/xiaozhi/uuid.cpp
    ${XZ_HTTP_SRC}
    voice_server/xiaozhi/websocket_client.cpp
    voice_server/xiaozhi/control_center.cpp
)

find_path(WEBSOCKETPP_INCLUDE websocketpp/client.hpp
    PATHS /usr/include /usr/local/include
)
find_path(BOOST_INCLUDE boost/version.hpp
    PATHS /usr/include /usr/local/include
)
if(WEBSOCKETPP_INCLUDE AND OPENSSL_FOUND AND BOOST_INCLUDE)
    add_executable(xiaozhi_control_center ${XZ_CC_SRCS})
    target_include_directories(xiaozhi_control_center PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/voice_server/xiaozhi
        ${WEBSOCKETPP_INCLUDE}
        ${BOOST_INCLUDE}
    )
    add_definitions(-DBOOST_ALL_NO_LIB -DBOOST_SYSTEM_NO_DEPRECATED)
    set(XZ_LIBS OpenSSL::SSL OpenSSL::Crypto pthread)
    if(CURL_FOUND)
        list(APPEND XZ_LIBS CURL::libcurl)
    endif()
    target_link_libraries(xiaozhi_control_center ${XZ_LIBS})
    message(STATUS "XiaoZhi control_center: ENABLED")
else()
    message(STATUS "XiaoZhi control_center: DISABLED"
            " (needs: websocketpp, OpenSSL, boost)")
    if(NOT WEBSOCKETPP_INCLUDE)
        message(STATUS "  -> missing: libwebsocketpp-dev")
    endif()
    if(NOT OPENSSL_FOUND)
        message(STATUS "  -> missing: libssl-dev")
    endif()
    if(NOT BOOST_INCLUDE)
        message(STATUS "  -> missing: libboost-dev")
    endif()
endif()

# ── XiaoZhi Sound App (ALSA audio + Opus codec) ───────────────────────
# Dependencies: libasound2-dev, libopus-dev, libspeexdsp-dev
find_package(ALSA QUIET)
find_library(OPUS_LIB opus)
find_path(OPUS_INCLUDE opus/opus.h)
find_library(SPEEXDSP_LIB speexdsp)

if(ALSA_FOUND AND OPUS_LIB AND OPUS_INCLUDE AND SPEEXDSP_LIB)
    add_executable(xiaozhi_sound_app
        voice_server/xiaozhi/sound_app.cpp
        voice_server/xiaozhi/record.cpp
        voice_server/xiaozhi/aplay.cpp
        voice_server/xiaozhi/opus.cpp
        voice_server/xiaozhi/ipc_udp.cpp
    )
    target_include_directories(xiaozhi_sound_app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/voice_server/xiaozhi
        ${ALSA_INCLUDE_DIRS}
        ${OPUS_INCLUDE}
    )
    target_link_libraries(xiaozhi_sound_app
        ${ALSA_LIBRARIES} ${OPUS_LIB} ${SPEEXDSP_LIB} pthread
    )
    message(STATUS "XiaoZhi sound_app: ENABLED")
else()
    message(STATUS "XiaoZhi sound_app: DISABLED"
            " (needs: alsa, opus, speexdsp)")
endif()

# ── WebRTC Remote Video Server ──────────────────────────────────────────
# Dependencies: libdatachannel (headers in libs/rtc/, .so in libs/libdatachannel/)
# + OpenSSL (already checked by XiaoZhi section above).
#
# On x86 simulator:   requires x86_64 libdatachannel.so in libs/libdatachannel/
# On ARM cross-compile: requires aarch64 (or armv7) libdatachannel.so
#
# We avoid find_library() with cross-compilation root-path issues by
# directly checking for the .so file and using link_directories().

set(LIBDATACHANNEL_INCLUDE ${CMAKE_SOURCE_DIR}/libs)
set(LIBDATACHANNEL_DIR     ${CMAKE_SOURCE_DIR}/libs/libdatachannel)

# Check headers in libs/rtc/
if(NOT EXISTS "${LIBDATACHANNEL_INCLUDE}/rtc/rtc.hpp")
    set(WEBRTC_MISSING "rtc headers (expected: libs/rtc/rtc.hpp)")
endif()

# Library detection — strategy differs for native vs cross-compile:
#   Native:   find_library validates ABI compatibility (x86 .so for x86, etc.)
#   Cross:    find_library may miss .so outside sysroot; fall back to EXISTS
find_library(LIBDATACHANNEL libdatachannel
    PATHS ${LIBDATACHANNEL_DIR} /usr/lib /usr/local/lib
    NO_DEFAULT_PATH
)
if(NOT LIBDATACHANNEL)
    # Also try default system paths
    find_library(LIBDATACHANNEL libdatachannel)
endif()

if(NOT LIBDATACHANNEL AND CMAKE_CROSSCOMPILING)
    # Cross-compile fallback: find_library may skip paths outside sysroot.
    # Trust the user placed the correct-architecture .so here.
    if(EXISTS "${LIBDATACHANNEL_DIR}/libdatachannel.so")
        set(LIBDATACHANNEL "${LIBDATACHANNEL_DIR}/libdatachannel.so")
    elseif(EXISTS "${LIBDATACHANNEL_DIR}/libdatachannel.so.0.22")
        set(LIBDATACHANNEL "${LIBDATACHANNEL_DIR}/libdatachannel.so.0.22")
    endif()
endif()

if(NOT LIBDATACHANNEL AND NOT WEBRTC_MISSING)
    set(WEBRTC_MISSING "libdatachannel.so (place correct-architecture .so in libs/libdatachannel/)")
endif()
if(NOT OPENSSL_FOUND AND NOT WEBRTC_MISSING)
    set(WEBRTC_MISSING "OpenSSL dev libraries")
endif()

if(LIBDATACHANNEL AND NOT WEBRTC_MISSING AND OPENSSL_FOUND)
    add_executable(webrtc_server
        webrtc_server/main.cpp
        webrtc_server/frame_source.cpp
        webrtc_server/peer_manager.cpp
        webrtc_server/signaling_server.cpp
    )
    target_include_directories(webrtc_server PRIVATE
        ${LIBDATACHANNEL_INCLUDE}
        ${CMAKE_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/voice_server/xiaozhi
    )
    # Use link_directories (not find_library paths) for robustness
    # with cross-compilation — same approach as luckfox project.
    link_directories(${LIBDATACHANNEL_DIR})
    target_link_libraries(webrtc_server
        datachannel OpenSSL::SSL OpenSSL::Crypto pthread
    )
    message(STATUS "WebRTC server: ENABLED")
    message(STATUS "  libdatachannel: ${LIBDATACHANNEL}")
    message(STATUS "  headers:        ${LIBDATACHANNEL_INCLUDE}")
else()
    message(STATUS "WebRTC server: DISABLED")
    if(WEBRTC_MISSING)
        message(STATUS "  -> ${WEBRTC_MISSING}")
    endif()
endif()
