/**
 * @file http_stub.cpp
 * Stub HTTP activation — used when libcurl is not available.
 * Returns 0 (already activated) so control_center can skip activation
 * and go directly to WebSocket connection.
 * Install libcurl-dev for real device activation.
 */
#include <cstdio>
#include "http.h"

int active_device(p_http_data_t pHttpData, char *codebuf)
{
    (void)pHttpData;
    if (codebuf) codebuf[0] = '\0';
    fprintf(stderr, "[STUB] http: device activation skipped (no libcurl)\n");
    return 0; /* 0 = already activated */
}
