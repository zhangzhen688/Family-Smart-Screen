/**
 * @file rpc_client.h
 * Generic JSON-RPC TCP client for the GUI frontend.
 *
 * Connects to each backend server (device/camera/voice) and sends
 * synchronous JSON-RPC requests, returning parsed cJSON responses.
 */
#ifndef RPC_CLIENT_H
#define RPC_CLIENT_H

#include "cJSON.h"
#include "rpc_protocol.h"

/**
 * Initialize connection to a specific RPC server.
 * @param port Server port number (DEVICE_SERVER_PORT, CAMERA_SERVER_PORT, etc.)
 * @return socket fd (>0) on success, -1 on error.
 */
int rpc_connect(int port);

/**
 * Disconnect from an RPC server.
 * @param fd Socket fd returned by rpc_connect().
 */
void rpc_disconnect(int fd);

/**
 * Send a JSON-RPC request and get the response.
 *
 * @param fd     Connected socket fd.
 * @param method Method name (e.g., "led_set").
 * @param params cJSON array of parameters (can be empty array).
 * @return Parsed cJSON "result" object (caller must cJSON_Delete),
 *         or NULL on error.
 */
cJSON *rpc_call(int fd, const char *method, cJSON *params);

/**
 * Convenience: build a params array from 1 integer argument and call.
 */
cJSON *rpc_call_int1(int fd, const char *method, int a);

/**
 * Convenience: build a params array from 2 integer arguments and call.
 */
cJSON *rpc_call_int2(int fd, const char *method, int a, int b);

/**
 * Convenience: call with no parameters.
 */
cJSON *rpc_call_void(int fd, const char *method);

#endif /* RPC_CLIENT_H */
