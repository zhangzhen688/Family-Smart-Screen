/**
 * @file xiaozhi_ipc.c
 * UDP IPC listener: receives {"state":N,"text":"...","emotion":"..."}
 * from xiaozhi control_center on port 5679.
 */
#include "xiaozhi_ipc.h"
#include "common.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define UI_PORT_DOWN  5679
#define BUF_SIZE      2048

static int g_running = 0;
static pthread_t g_thread;

static xz_state_cb_t  g_state_cb = NULL;
static xz_text_cb_t   g_text_cb = NULL;
static xz_emotion_cb_t g_emotion_cb = NULL;

/* State strings matching xiaozhi DeviceState enum */
static const char *state_names[] = {
    "Unknown",          /* 0 */
    "Starting...",      /* 1 */
    "WiFi Config",      /* 2 */
    "Standby",          /* 3 */
    "Connecting...",    /* 4 */
    "Listening...",     /* 5 */
    "Speaking...",      /* 6 */
    "Upgrading...",     /* 7 */
    "Activation",       /* 8 — shows activation code */
    "Error",            /* 9 */
};

static const char *get_state_name(int state)
{
    if (state >= 0 && state <= 9) return state_names[state];
    return "Unknown";
}

static void *recv_thread(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOG_ERROR("xz_ipc: socket failed");
        return NULL;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UI_PORT_DOWN);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("xz_ipc: bind port %d failed: %s", UI_PORT_DOWN, strerror(errno));
        close(fd);
        return NULL;
    }

    LOG_INFO("XiaoZhi IPC listener on UDP port %d", UI_PORT_DOWN);

    char buf[BUF_SIZE];
    while (g_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(fd, buf, BUF_SIZE - 1, MSG_DONTWAIT,
                           (struct sockaddr *)&from, &from_len);
        if (len > 0) {
            buf[len] = '\0';
            cJSON *json = cJSON_Parse(buf);
            if (!json) continue;

            /* Device state: {"state": 5} */
            cJSON *state = cJSON_GetObjectItem(json, "state");
            if (state && g_state_cb) {
                int s = state->valueint;
                g_state_cb(s, get_state_name(s));
            }

            /* Text: {"text": "hello"} — STT results, activation codes */
            cJSON *text = cJSON_GetObjectItem(json, "text");
            if (text && text->valuestring && g_text_cb) {
                g_text_cb(text->valuestring);
            }

            /* Emotion: {"emotion": "happy"} */
            cJSON *emotion = cJSON_GetObjectItem(json, "emotion");
            if (emotion && emotion->valuestring && g_emotion_cb) {
                g_emotion_cb(emotion->valuestring);
            }

            cJSON_Delete(json);
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        usleep(30000); /* 30ms poll */
    }

    close(fd);
    return NULL;
}

int xz_ipc_start(void)
{
    if (g_running) return 0;
    g_running = 1;
    if (pthread_create(&g_thread, NULL, recv_thread, NULL) != 0) {
        g_running = 0;
        return -1;
    }
    return 0;
}

void xz_ipc_on_state(xz_state_cb_t cb)   { g_state_cb = cb; }
void xz_ipc_on_text(xz_text_cb_t cb)     { g_text_cb = cb; }
void xz_ipc_on_emotion(xz_emotion_cb_t cb) { g_emotion_cb = cb; }

void xz_ipc_stop(void)
{
    g_running = 0;
    if (g_thread) pthread_join(g_thread, NULL);
}
