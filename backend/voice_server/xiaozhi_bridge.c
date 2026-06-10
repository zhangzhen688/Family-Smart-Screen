/**
 * @file xiaozhi_bridge.c
 * Bridge: JSON-RPC (GUI) ↔ UDP IPC (xiaozhi control_center).
 *
 * Uses the same UDP IPC protocol as the reference xiaozhi-linux project.
 * Port layout from cfg.h:
 *   UI_PORT_UP   = 5678  (control_center receives from GUI)
 *   UI_PORT_DOWN = 5679  (control_center sends to GUI)
 */
#include "xiaozhi_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

#define UI_PORT_UP    5678
#define UI_PORT_DOWN  5679
#define MAX_MSG_LEN   4096

/* ── Internal state ─────────────────────────────────────────────────── */
static int g_sock_up = -1;    /* send to control_center (dest port 5678) */
static int g_sock_down = -1;  /* recv from control_center (bind 5679) */
static int g_running = 0;
static pthread_t g_recv_thread;

static xiaozhi_state_t g_state = XZ_STATE_IDLE;
static char g_stt_text[MAX_MSG_LEN] = "";
static char g_iot_data[MAX_MSG_LEN] = "";
static char g_recv_buf[MAX_MSG_LEN];

/* ── Receive thread ─────────────────────────────────────────────────── */
static void *recv_thread_func(void *arg)
{
    (void)arg;
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    while (g_running) {
        int len = recvfrom(g_sock_down, g_recv_buf, MAX_MSG_LEN - 1,
                           MSG_DONTWAIT, (struct sockaddr *)&from_addr,
                           &from_len);
        if (len > 0) {
            g_recv_buf[len] = '\0';

            /* Parse message from control_center.
             * Format: {"state": <int>} for device state
             *         {"text": "<stt_text>"} for STT results
             *         IoT updates come as raw JSON objects */
            if (strstr(g_recv_buf, "\"state\"")) {
                /* Extract state number */
                const char *p = strstr(g_recv_buf, "\"state\":");
                if (p) {
                    int s = atoi(p + 8);
                    if (s >= 0 && s <= 9) {
                        g_state = (xiaozhi_state_t)s;
                    }
                }
            } else if (strstr(g_recv_buf, "\"text\"")) {
                /* Extract STT text */
                const char *p = strchr(g_recv_buf, ':');
                if (p) {
                    p++; /* skip ':' */
                    while (*p == ' ' || *p == '"') p++;
                    /* Copy until closing quote */
                    char *dst = g_stt_text;
                    while (*p && *p != '"' && *p != '}' &&
                           (dst - g_stt_text) < MAX_MSG_LEN - 1) {
                        *dst++ = *p++;
                    }
                    *dst = '\0';
                    /* If we got text, device is speaking */
                    if (strlen(g_stt_text) > 0) {
                        g_state = XZ_STATE_SPEAKING;
                    }
                }
            } else if (strstr(g_recv_buf, "{")) {
                /* Generic JSON — treat as IoT data */
                strncpy(g_iot_data, g_recv_buf, MAX_MSG_LEN - 1);
                g_iot_data[MAX_MSG_LEN - 1] = '\0';
            }

            LOG_INFO("[xiaozhi] recv: %s", g_recv_buf);
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        usleep(50000); /* 50ms poll interval */
    }
    return NULL;
}

/* ── Init ───────────────────────────────────────────────────────────── */
int xz_bridge_init(void)
{
    struct sockaddr_in addr;
    int reuse = 1;

    /* Create send socket (to control_center on port UI_PORT_UP) */
    g_sock_up = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock_up < 0) {
        LOG_ERROR("xz_bridge: send socket failed");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UI_PORT_UP);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(g_sock_up, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("xz_bridge: connect to control_center failed: %s",
                  strerror(errno));
        /* Non-fatal — control_center may not be running yet */
    }

    /* Create receive socket (from control_center, bind to UI_PORT_DOWN) */
    g_sock_down = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock_down < 0) {
        close(g_sock_up);
        g_sock_up = -1;
        return -1;
    }

    setsockopt(g_sock_down, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UI_PORT_DOWN);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_sock_down, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("xz_bridge: bind recv port %d failed: %s",
                  UI_PORT_DOWN, strerror(errno));
        close(g_sock_up); g_sock_up = -1;
        close(g_sock_down); g_sock_down = -1;
        return -1;
    }

    /* Start receive thread */
    g_running = 1;
    if (pthread_create(&g_recv_thread, NULL, recv_thread_func, NULL) != 0) {
        LOG_ERROR("xz_bridge: thread create failed");
        g_running = 0;
        return -1;
    }

    LOG_INFO("XiaoZhi bridge initialized (UDP send→%d, recv←%d)",
             UI_PORT_UP, UI_PORT_DOWN);
    return 0;
}

/* ── Commands ───────────────────────────────────────────────────────── */
int xz_bridge_start_listening(void)
{
    if (g_sock_up < 0) return -1;
    const char *msg = "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}";
    send(g_sock_up, msg, strlen(msg), 0);
    g_state = XZ_STATE_LISTENING;
    LOG_STUB("xz: start listening");
    return 0;
}

int xz_bridge_send_tts(const char *text)
{
    if (g_sock_up < 0 || !text) return -1;
    char msg[MAX_MSG_LEN];
    /* xiaozhi format: {"type":"tts","text":"..."} */
    snprintf(msg, sizeof(msg),
             "{\"type\":\"tts\",\"text\":\"%.2000s\"}", text);
    send(g_sock_up, msg, strlen(msg), 0);
    g_state = XZ_STATE_SPEAKING;
    return 0;
}

int xz_bridge_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    /* Volume is handled by sound_app / ALSA mixer */
    LOG_STUB("xz: set volume %d", volume);
    return 0;
}

int xz_bridge_poll(void)
{
    /* State updates come from the receive thread */
    return 0;
}

xiaozhi_state_t xz_bridge_get_state(void)
{
    return g_state;
}

const char *xz_bridge_get_stt_text(void)
{
    return g_stt_text;
}

const char *xz_bridge_get_iot_data(void)
{
    return g_iot_data;
}

void xz_bridge_exit(void)
{
    g_running = 0;
    if (g_recv_thread) {
        pthread_join(g_recv_thread, NULL);
    }
    if (g_sock_up >= 0) { close(g_sock_up); g_sock_up = -1; }
    if (g_sock_down >= 0) { close(g_sock_down); g_sock_down = -1; }
    LOG_INFO("XiaoZhi bridge exited");
}
