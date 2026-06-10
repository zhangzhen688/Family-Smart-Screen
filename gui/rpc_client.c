/**
 * @file rpc_client.c
 * JSON-RPC TCP client implementation.
 *
 * Pattern matched from refs/myapp/app/myapp/rpc_client.cpp:
 *   TCP connect → send() JSON request → recv() response → cJSON_Parse()
 */
#include "rpc_client.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

/* Catch SIGPIPE when writing to a closed socket */
static void ignore_sigpipe(void)
{
    static int done = 0;
    if (!done) {
        signal(SIGPIPE, SIG_IGN);
        done = 1;
    }
}

int rpc_connect(int port)
{
    ignore_sigpipe();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG_ERROR("rpc_connect: socket() failed: %s", strerror(errno));
        return -1;
    }

    /* Set receive timeout */
    struct timeval tv;
    tv.tv_sec  = RPC_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(RPC_SERVER_ADDR);
    memset(addr.sin_zero, 0, 8);

    if (connect(sock, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("rpc_connect: connect() to port %d failed: %s",
                  port, strerror(errno));
        close(sock);
        return -1;
    }

    return sock;
}

void rpc_disconnect(int fd)
{
    if (fd > 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

cJSON *rpc_call(int fd, const char *method, cJSON *params)
{
    if (fd < 0 || !method || !params) return NULL;

    /* Build JSON-RPC request */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "method", method);
    cJSON_AddItemToObject(req, "params", params);
    cJSON_AddStringToObject(req, "id", "1");

    char *req_str = cJSON_Print(req);
    cJSON_Delete(req);

    if (!req_str) return NULL;

    /* Send request */
    size_t req_len = strlen(req_str);
    ssize_t sent = send(fd, req_str, req_len, 0);
    free(req_str);

    if (sent < 0 || (size_t)sent != req_len) {
        LOG_ERROR("rpc_call: send failed for '%s': %s", method, strerror(errno));
        return NULL;
    }

    /* Read response — accumulate until we have a complete JSON object */
    char buf[4096];
    ssize_t total = 0;

    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) {
            if (n == 0) {
                LOG_ERROR("rpc_call: connection closed by server");
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Timeout */
                LOG_ERROR("rpc_call: timeout waiting for '%s' response", method);
            } else {
                LOG_ERROR("rpc_call: read error: %s", strerror(errno));
            }
            return NULL;
        }
        total += n;
        buf[total] = '\0';

        /* Check if we have a complete JSON object (rough heuristic) */
        int brace_count = 0, in_string = 0;
        for (ssize_t i = 0; i < total; i++) {
            if (buf[i] == '"' && (i == 0 || buf[i-1] != '\\')) in_string = !in_string;
            if (!in_string) {
                if (buf[i] == '{') brace_count++;
                else if (buf[i] == '}') brace_count--;
            }
        }
        if (brace_count == 0 && total > 2) break; /* complete JSON */
    }

    /* Skip leading whitespace/newlines (some servers send \r\n before response) */
    char *start = buf;
    while (*start == '\r' || *start == '\n' || *start == ' ') start++;

    /* Parse response */
    cJSON *resp = cJSON_Parse(start);
    if (!resp) {
        LOG_ERROR("rpc_call: JSON parse error for '%s'", method);
        return NULL;
    }

    /* Extract "result" */
    cJSON *result = cJSON_DetachItemFromObject(resp, "result");

    /* Check for "error" */
    cJSON *err = cJSON_GetObjectItem(resp, "error");
    if (err) {
        cJSON *msg = cJSON_GetObjectItem(err, "message");
        LOG_ERROR("rpc_call: server error for '%s': %s",
                  method, msg ? msg->valuestring : "unknown");
        if (result) cJSON_Delete(result);
        result = NULL;
    }

    cJSON_Delete(resp);
    return result;
}

cJSON *rpc_call_int1(int fd, const char *method, int a)
{
    cJSON *params = cJSON_CreateArray();
    cJSON_AddNumberToObject(params, NULL, a);
    return rpc_call(fd, method, params);
}

cJSON *rpc_call_int2(int fd, const char *method, int a, int b)
{
    cJSON *params = cJSON_CreateArray();
    cJSON_AddNumberToObject(params, NULL, a);
    cJSON_AddNumberToObject(params, NULL, b);
    return rpc_call(fd, method, params);
}

cJSON *rpc_call_void(int fd, const char *method)
{
    cJSON *params = cJSON_CreateArray();
    return rpc_call(fd, method, params);
}
