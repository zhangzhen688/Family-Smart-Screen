/**
 * @file http_server.h
 * Minimal embedded HTTP server for remote control.
 * Pure C, single-threaded, event-driven with select().
 * Supports: static HTML, JSON API, MJPEG streaming, SSE.
 */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "common.h"
#include <stddef.h>

/* HTTP methods */
typedef enum { HTTP_GET, HTTP_POST, HTTP_UNKNOWN } http_method_t;

/* Request context passed to route handlers */
typedef struct {
    http_method_t method;
    char path[256];
    char body[4096];
    int body_len;
    /* Query string parsing */
    const char *qs;
} http_req_t;

/* Response builder */
typedef struct {
    int fd;
    int status_sent;
} http_res_t;

/* Route handler: returns 0 on success */
typedef int (*http_handler_t)(http_req_t *req, http_res_t *res);

/** Start HTTP server on given port. Non-blocking — call http_server_poll() in loop. */
int http_server_start(int port);

/** Process pending connections (call in main loop or thread). */
void http_server_poll(void);

/** Stop server. */
void http_server_stop(void);

/** Register a route. */
void http_route(const char *method_path, http_handler_t handler);

/* ── Response helpers ──────────────────────────────────────────────── */
void http_send_html(http_res_t *res, const char *html);
void http_send_json(http_res_t *res, const char *json);
void http_send_error(http_res_t *res, int code, const char *msg);
void http_send_ok(http_res_t *res, const char *body, const char *ctype);

/* For streaming (MJPEG, SSE): get raw fd for manual writes */
int  http_res_get_fd(http_res_t *res);
void http_start_stream(http_res_t *res, const char *content_type);
void http_write_chunk(http_res_t *res, const char *data, int len);

#endif
