# Builds jsonrpc-c + libev as a static library.
# jsonrpc-c bundles its own cJSON; we use the shared cjson from libs/cjson.

# ── libev (event loop) ────────────────────────────────────────────────
set(LIBEV_SRCS
    libev/ev.c
)

add_library(libev STATIC ${LIBEV_SRCS})
target_include_directories(libev PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libev)
target_link_libraries(libev PUBLIC m)

# ── jsonrpc-c ─────────────────────────────────────────────────────────
set(JSONRPC_SRCS
    src/jsonrpc-c.c
)

add_library(jsonrpc STATIC ${JSONRPC_SRCS})
target_include_directories(jsonrpc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libev
    ${CMAKE_SOURCE_DIR}/libs/cjson
)
target_link_libraries(jsonrpc libev cjson)
