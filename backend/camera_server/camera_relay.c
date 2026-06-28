/**
 * @file camera_relay.c
 * TCP relay server — fans out MJPEG frames to network consumers.
 *
 * Design:
 *   - Dedicated thread runs a select()-based accept/write loop.
 *   - Connected clients live in a singly-linked list.
 *   - camera_relay_broadcast() builds the 4-byte length prefix + JPEG
 *     payload ONCE into a stack buffer, then non-blocking-send()s it to
 *     every client.  Slow consumers get the frame dropped; after 3 s of
 *     consecutive EAGAIN they are disconnected.
 *   - Thread-safety: broadcast() is called from the V4L2 streamer thread
 *     and only appends to a lock-free work queue.  The relay thread drains
 *     the queue and does the actual I/O.  (Simplified v1: use a mutex
 *     around the broadcast — the critical section is a memcpy + linked-list
 *     walk, < 100 µs, so contention is negligible.)
 */
#include "camera_relay.h"
#include "webrtc_protocol.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>

/* ── Constants ─────────────────────────────────────────────────────────── */

#define RELAY_MAX_CLIENTS  16
#define RELAY_BACKLOG       4
#define RELAY_STALL_TIMEOUT 3     /* seconds before disconnecting a stuck client */
#define RELAY_SELECT_US     50000 /* 50 ms select timeout                       */

/* ── Client node ───────────────────────────────────────────────────────── */

typedef struct client_node {
    int             fd;
    time_t          last_ok;       /* last time send() succeeded                */
    int             stall_count;   /* consecutive EAGAIN / short-write count    */
    struct client_node *next;
} client_node_t;

/* ── File-static state ─────────────────────────────────────────────────── */

static pthread_t       g_thread;
static volatile int    g_running   = 0;
static int             g_listen_fd = -1;
static uint16_t        g_port      = 0;

static client_node_t  *g_clients   = NULL;
static int             g_client_count = 0;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* Set a socket to non-blocking mode. */
static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Remove and free a single client from the linked list.  Caller holds g_mutex. */
static void client_remove_locked(client_node_t *prev, client_node_t *cur)
{
    if (!cur) return;
    if (prev)
        prev->next = cur->next;
    else
        g_clients = cur->next;

    close(cur->fd);
    free(cur);
    g_client_count--;
}

/* ── Relay thread ──────────────────────────────────────────────────────── */

static void *relay_thread(void *arg)
{
    (void)arg;

    fd_set rfds;
    struct timeval tv;
    int max_fd;

    LOG_INFO("Relay: thread started on port %u", g_port);

    while (g_running) {
        FD_ZERO(&rfds);
        FD_SET(g_listen_fd, &rfds);
        max_fd = g_listen_fd;

        /* Also monitor client sockets (currently only for error detection;
         * we don't read from clients — they are write-only).  We still
         * include them so select() can detect closed connections via
         * readable-ready-with-0-bytes. */
        pthread_mutex_lock(&g_mutex);
        for (client_node_t *c = g_clients; c; c = c->next) {
            FD_SET(c->fd, &rfds);
            if (c->fd > max_fd) max_fd = c->fd;
        }
        pthread_mutex_unlock(&g_mutex);

        tv.tv_sec  = 0;
        tv.tv_usec = RELAY_SELECT_US;

        int n = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Relay: select error: %s", strerror(errno));
            break;
        }

        /* Accept new connections */
        if (FD_ISSET(g_listen_fd, &rfds)) {
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            int client_fd = accept(g_listen_fd, (struct sockaddr *)&addr, &addr_len);
            if (client_fd >= 0) {
                set_nonblock(client_fd);

                pthread_mutex_lock(&g_mutex);
                if (g_client_count < RELAY_MAX_CLIENTS) {
                    client_node_t *node = calloc(1, sizeof(*node));
                    node->fd      = client_fd;
                    node->last_ok = time(NULL);
                    node->next    = g_clients;
                    g_clients     = node;
                    g_client_count++;
                    LOG_INFO("Relay: client connected from %s:%d (%d total)",
                             inet_ntoa(addr.sin_addr), ntohs(addr.sin_port),
                             g_client_count);
                } else {
                    LOG_INFO("Relay: rejecting client — max %d reached",
                             RELAY_MAX_CLIENTS);
                    close(client_fd);
                }
                pthread_mutex_unlock(&g_mutex);
            }
            n--;
        }

        /* Detect disconnected clients (readable with 0 bytes = EOF) */
        if (n > 0) {
            pthread_mutex_lock(&g_mutex);
            client_node_t *prev = NULL;
            client_node_t *cur  = g_clients;
            while (cur) {
                client_node_t *next = cur->next;
                if (FD_ISSET(cur->fd, &rfds)) {
                    char dummy;
                    int rc = recv(cur->fd, &dummy, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (rc == 0) {
                        /* Client closed connection */
                        client_remove_locked(prev, cur);
                        cur = (prev ? prev->next : g_clients);
                        continue;
                    }
                }
                prev = cur;
                cur  = next;
            }
            pthread_mutex_unlock(&g_mutex);
        }
    }

    /* Clean up all clients */
    pthread_mutex_lock(&g_mutex);
    while (g_clients) {
        client_node_t *next = g_clients->next;
        close(g_clients->fd);
        free(g_clients);
        g_clients = next;
    }
    g_client_count = 0;
    pthread_mutex_unlock(&g_mutex);

    LOG_INFO("Relay: thread stopped");
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────────── */

int camera_relay_start(uint16_t port)
{
    if (g_running) return 0;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        LOG_ERROR("Relay: socket() failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Relay: bind(:%u) failed: %s", port, strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    if (listen(g_listen_fd, RELAY_BACKLOG) < 0) {
        LOG_ERROR("Relay: listen() failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    g_port    = port;
    g_running = 1;

    if (pthread_create(&g_thread, NULL, relay_thread, NULL) != 0) {
        LOG_ERROR("Relay: pthread_create failed");
        g_running = 0;
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    LOG_INFO("Relay: listening on port %u", port);
    return 0;
}

void camera_relay_stop(void)
{
    if (!g_running) return;

    LOG_INFO("Relay: stopping...");
    g_running = 0;

    /* Wake up select() by connecting to our own listen socket */
    int wake_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (wake_fd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(g_port);
        connect(wake_fd, (struct sockaddr *)&addr, sizeof(addr));
        close(wake_fd);
    }

    pthread_join(g_thread, NULL);

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

void camera_relay_broadcast(const uint8_t *data, uint32_t size)
{
    if (!g_running || !data || size == 0 || size > WEBRTC_RELAY_MAX_FRAME)
        return;

    /*
     * Build the framed packet once on the stack.
     * WEBRTC_RELAY_MAX_FRAME = 1 MB, plus 4-byte header.
     * For MJPEG 640×480 this is typically 30-60 KB, well within limits.
     */
    uint32_t be_size = htonl(size);
    /*
     * We send header + payload in two writev-style calls to avoid
     * a 1 MB stack allocation.  Build the header in a tiny buffer,
     * then writev() or two send()s.
     */

    pthread_mutex_lock(&g_mutex);

    time_t now = time(NULL);
    client_node_t *prev = NULL;
    client_node_t *cur  = g_clients;

    while (cur) {
        client_node_t *next = cur->next;

        /* Send 4-byte length prefix */
        ssize_t sent = send(cur->fd, &be_size, 4, MSG_NOSIGNAL);
        if (sent == 4) {
            /* Send JPEG payload */
            sent = send(cur->fd, data, size, MSG_NOSIGNAL);
        }

        if (sent == (ssize_t)size || (sent == 4 && size == 0)) {
            /* Success — reset stall tracking */
            cur->last_ok     = now;
            cur->stall_count = 0;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Socket buffer full — drop this frame for this client */
            cur->stall_count++;
            if (now - cur->last_ok > RELAY_STALL_TIMEOUT) {
                LOG_INFO("Relay: disconnecting stalled client (fd=%d, %ds)",
                         cur->fd, RELAY_STALL_TIMEOUT);
                client_remove_locked(prev, cur);
                cur = (prev ? prev->next : g_clients);
                continue;
            }
        } else {
            /* Write error — disconnect */
            client_remove_locked(prev, cur);
            cur = (prev ? prev->next : g_clients);
            continue;
        }

        prev = cur;
        cur  = next;
    }

    pthread_mutex_unlock(&g_mutex);
}

int camera_relay_is_running(void)
{
    return g_running;
}

int camera_relay_client_count(void)
{
    int count;
    pthread_mutex_lock(&g_mutex);
    count = g_client_count;
    pthread_mutex_unlock(&g_mutex);
    return count;
}
