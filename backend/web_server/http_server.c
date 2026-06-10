/**
 * @file http_server.c
 * Minimal embedded HTTP/1.1 server.
 *
 * Single-threaded, select()-based event loop.
 * Supports GET/POST, JSON API, MJPEG streaming, SSE.
 *
 * Usage:
 *   http_server_start(8080);
 *   http_route("GET /api/status", my_handler);
 *   while (running) http_server_poll();
 */
#include "http_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define MAX_ROUTES      32
#define MAX_CLIENTS     (FD_SETSIZE - 4)
#define BUF_SIZE        8192
#define MAX_HEADERS     32

/* ── Route table ────────────────────────────────────────────────────── */
typedef struct {
    http_method_t method;
    char path[128];
    http_handler_t handler;
} route_t;

static route_t g_routes[MAX_ROUTES];
static int g_route_count = 0;

/* ── Client connection ──────────────────────────────────────────────── */
typedef struct {
    int fd;
    char buf[BUF_SIZE];
    int  buf_len;
    http_method_t method;
    char path[256];
    char body[4096];
    int  body_len;
    int  content_length;
    int  headers_done;
    int  keep_alive;
} client_t;

static client_t g_clients[MAX_CLIENTS];
static int g_client_count = 0;
static int g_server_fd = -1;
static int g_running = 0;

/* ── Route registration ─────────────────────────────────────────────── */
void http_route(const char *method_path, http_handler_t handler)
{
    if (g_route_count >= MAX_ROUTES) return;
    route_t *r = &g_routes[g_route_count++];

    if (strncmp(method_path, "GET ", 4) == 0) {
        r->method = HTTP_GET;
        strncpy(r->path, method_path + 4, sizeof(r->path) - 1);
    } else if (strncmp(method_path, "POST ", 5) == 0) {
        r->method = HTTP_POST;
        strncpy(r->path, method_path + 5, sizeof(r->path) - 1);
    } else {
        r->method = HTTP_UNKNOWN;
    }
    r->handler = handler;
}

/* ── Response helpers ───────────────────────────────────────────────── */
static void send_response(int fd, const char *status, const char *ctype,
                          const char *body, int body_len)
{
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", status, ctype, body_len);
    send(fd, hdr, hl, 0);
    if (body && body_len > 0)
        send(fd, body, body_len, 0);
}

void http_send_html(http_res_t *res, const char *html)
    { send_response(res->fd, "200 OK", "text/html; charset=utf-8", html, strlen(html)); }
void http_send_json(http_res_t *res, const char *json)
    { send_response(res->fd, "200 OK", "application/json", json, strlen(json)); }
void http_send_error(http_res_t *res, int code, const char *msg)
    { char s[8]; snprintf(s, sizeof(s), "%d ERR", code);
      send_response(res->fd, s, "text/plain", msg, strlen(msg)); }
void http_send_ok(http_res_t *res, const char *body, const char *ctype)
    { send_response(res->fd, "200 OK", ctype, body, strlen(body)); }

int http_res_get_fd(http_res_t *res) { return res->fd; }

void http_start_stream(http_res_t *res, const char *content_type)
{
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", content_type);
    send(res->fd, hdr, hl, 0);
}

void http_write_chunk(http_res_t *res, const char *data, int len)
{
    if (len > 0) send(res->fd, data, len, 0);
}

/* ── Request parsing ────────────────────────────────────────────────── */
static void parse_request(client_t *c)
{
    /* First line: METHOD /path HTTP/1.x */
    char *line = c->buf;
    char *end = strstr(line, "\r\n");
    if (!end) return;

    if (strncmp(line, "GET ", 4) == 0) {
        c->method = HTTP_GET;
        line += 4;
    } else if (strncmp(line, "POST ", 5) == 0) {
        c->method = HTTP_POST;
        line += 5;
    } else {
        c->method = HTTP_UNKNOWN;
        return;
    }

    char *path_end = strchr(line, ' ');
    if (path_end) {
        int plen = path_end - line;
        if (plen > 254) plen = 254;
        memcpy(c->path, line, plen);
        c->path[plen] = '\0';
    }

    /* Parse headers */
    c->content_length = 0;
    char *p = end + 2;
    while (*p && strncmp(p, "\r\n", 2) != 0) {
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            c->content_length = atoi(p + 15);
        }
        if (strncasecmp(p, "Connection: keep-alive", 22) == 0)
            c->keep_alive = 1;
        char *nl = strstr(p, "\r\n");
        if (!nl) break;
        p = nl + 2;
    }

    /* Body for POST */
    if (c->method == HTTP_POST && c->content_length > 0) {
        char *body_start = strstr(p, "\r\n");
        if (body_start) body_start += 2;
        else body_start = p + 2;
        int blen = c->buf_len - (body_start - c->buf);
        if (blen > c->content_length) blen = c->content_length;
        if (blen > 4095) blen = 4095;
        memcpy(c->body, body_start, blen);
        c->body[blen] = '\0';
        c->body_len = blen;
    }

    c->headers_done = 1;
}

/* ── Route matching ─────────────────────────────────────────────────── */
static http_handler_t find_route(http_method_t method, const char *path)
{
    for (int i = 0; i < g_route_count; i++) {
        if (g_routes[i].method == method) {
            /* Exact match or prefix match ending with * */
            size_t rlen = strlen(g_routes[i].path);
            if (g_routes[i].path[rlen-1] == '*') {
                if (strncmp(g_routes[i].path, path, rlen-1) == 0)
                    return g_routes[i].handler;
            } else {
                if (strcmp(g_routes[i].path, path) == 0)
                    return g_routes[i].handler;
            }
        }
    }
    return NULL;
}

/* ── Client handling ────────────────────────────────────────────────── */
static void handle_client(client_t *c)
{
    if (!c->headers_done) return;

    http_req_t req;
    memset(&req, 0, sizeof(req));
    req.method = c->method;
    strncpy(req.path, c->path, sizeof(req.path)-1);
    if (c->body_len > 0) {
        memcpy(req.body, c->body, c->body_len);
        req.body_len = c->body_len;
    }

    http_res_t res;
    memset(&res, 0, sizeof(res));
    res.fd = c->fd;

    http_handler_t h = find_route(c->method, c->path);
    if (h) {
        h(&req, &res);
    } else {
        /* Try matching with query string stripped */
        char *q = strchr(req.path, '?');
        if (q) *q = '\0';
        h = find_route(c->method, req.path);
        if (h) {
            if (q) req.qs = q + 1;
            h(&req, &res);
        } else {
            http_send_error(&res, 404, "Not Found");
        }
    }
}

/* ── Server lifecycle ───────────────────────────────────────────────── */
int http_server_start(int port)
{
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) { LOG_ERROR("http: socket failed"); return -1; }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    fcntl(g_server_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("http: bind port %d failed: %s", port, strerror(errno));
        close(g_server_fd); g_server_fd = -1;
        return -1;
    }
    listen(g_server_fd, 10);
    g_running = 1;

    LOG_INFO("HTTP server started on port %d", port);
    return 0;
}

void http_server_poll(void)
{
    if (!g_running) return;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(g_server_fd, &rfds);
    int maxfd = g_server_fd;

    for (int i = 0; i < g_client_count; i++) {
        FD_SET(g_clients[i].fd, &rfds);
        if (g_clients[i].fd > maxfd) maxfd = g_clients[i].fd;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 }; /* 50ms */
    int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (n <= 0) {
        /* Clean up closed clients */
        for (int i = 0; i < g_client_count; ) {
            if (g_clients[i].headers_done && !g_clients[i].keep_alive) {
                shutdown(g_clients[i].fd, SHUT_RDWR);
                close(g_clients[i].fd);
                g_clients[i] = g_clients[--g_client_count];
            } else i++;
        }
        return;
    }

    /* Accept new connections */
    if (FD_ISSET(g_server_fd, &rfds) && g_client_count < MAX_CLIENTS) {
        int fd = accept(g_server_fd, NULL, NULL);
        if (fd >= 0) {
            fcntl(fd, F_SETFL, O_NONBLOCK);
            client_t *c = &g_clients[g_client_count++];
            memset(c, 0, sizeof(*c));
            c->fd = fd;
        }
    }

    /* Handle client I/O */
    for (int i = 0; i < g_client_count; ) {
        client_t *c = &g_clients[i];
        if (FD_ISSET(c->fd, &rfds)) {
            char tmp[4096];
            int nr = read(c->fd, tmp, sizeof(tmp));
            if (nr > 0) {
                int space = BUF_SIZE - c->buf_len - 1;
                if (nr > space) nr = space;
                memcpy(c->buf + c->buf_len, tmp, nr);
                c->buf_len += nr;
                c->buf[c->buf_len] = '\0';

                if (!c->headers_done && strstr(c->buf, "\r\n\r\n")) {
                    parse_request(c);
                }
                if (c->headers_done) {
                    handle_client(c);
                    /* Close after handling unless keep-alive */
                    shutdown(c->fd, SHUT_RDWR);
                    close(c->fd);
                    /* Mark for removal */
                    c->fd = -1;
                } else {
                    i++;
                }
            } else {
                close(c->fd);
                c->fd = -1;
            }
        } else {
            i++;
        }

        /* Remove dead clients */
        if (c->fd < 0) {
            g_clients[i] = g_clients[--g_client_count];
        } else {
            i++;
        }
    }
}

void http_server_stop(void)
{
    g_running = 0;
    for (int i = 0; i < g_client_count; i++)
        close(g_clients[i].fd);
    g_client_count = 0;
    if (g_server_fd >= 0) { close(g_server_fd); g_server_fd = -1; }
}
