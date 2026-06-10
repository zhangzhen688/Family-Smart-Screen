/**
 * @file rpc_server.c
 * Voice assistant JSON-RPC server (port 1236).
 *
 * Bridges our GUI (JSON-RPC/TCP) to the xiaozhi-linux UDP IPC bus,
 * which communicates with control_center and sound_app processes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "jsonrpc-c.h"
#include "rpc_protocol.h"
#include "common.h"
#include "xiaozhi_bridge.h"

static struct jrpc_server my_server;

static void signal_handler(int sig)
{
    (void)sig;
    LOG_INFO("Voice server shutting down...");
    jrpc_server_stop(&my_server);
}

/* ── RPC: voice_get_state ───────────────────────────────────────────── */
cJSON *rpc_voice_get_state(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params;

    xz_bridge_poll();
    xiaozhi_state_t s = xz_bridge_get_state();
    const char *stt = xz_bridge_get_stt_text();

    const char *state_str = "idle";
    switch (s) {
        case XZ_STATE_IDLE:       state_str = "idle"; break;
        case XZ_STATE_CONNECTING: state_str = "connecting"; break;
        case XZ_STATE_LISTENING:  state_str = "listening"; break;
        case XZ_STATE_SPEAKING:   state_str = "speaking"; break;
        case XZ_STATE_ACTIVATING: state_str = "activating"; break;
        case XZ_STATE_ERROR:      state_str = "error"; break;
        default: break;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", 1);
    cJSON_AddStringToObject(result, "state", state_str);
    cJSON_AddStringToObject(result, "text", stt ? stt : "");

    /* Include IoT data if available */
    const char *iot = xz_bridge_get_iot_data();
    if (iot && strlen(iot) > 2) {
        cJSON_AddStringToObject(result, "iot", iot);
    }

    return result;
}

/* ── RPC: voice_send_text ───────────────────────────────────────────── */
cJSON *rpc_voice_send_text(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx;
    cJSON *result = cJSON_CreateObject();

    cJSON *text = cJSON_GetArrayItem(params, 0);
    if (text && text->valuestring) {
        int ret = xz_bridge_send_tts(text->valuestring);
        if (ret == 0) {
            cJSON_AddBoolToObject(result, "ok", 1);
        } else {
            cJSON_AddBoolToObject(result, "ok", 0);
            cJSON_AddStringToObject(result, "error",
                    "control_center not running. Start xiaozhi services.");
        }
    } else {
        /* No text: start listening mode */
        int ret = xz_bridge_start_listening();
        cJSON_AddBoolToObject(result, "ok", (ret == 0));
    }
    return result;
}

/* ── RPC: voice_set_volume ──────────────────────────────────────────── */
cJSON *rpc_voice_set_volume(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx;
    cJSON *result = cJSON_CreateObject();

    cJSON *vol = cJSON_GetArrayItem(params, 0);
    if (vol) {
        int v = vol->valueint;
        xz_bridge_set_volume(v);
        cJSON_AddBoolToObject(result, "ok", 1);
        cJSON_AddNumberToObject(result, "volume", v);
    } else {
        cJSON_AddBoolToObject(result, "ok", 0);
    }
    return result;
}

int main(void)
{
    LOG_INFO("====================================");
    LOG_INFO("Smart Screen — Voice RPC Server");
    LOG_INFO("Port: %d (JSON-RPC)", VOICE_SERVER_PORT);
    LOG_INFO("XiaoZhi UDP: send→5678, recv←5679");
    LOG_INFO("====================================");

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize xiaozhi bridge (UDP IPC to control_center) */
    if (xz_bridge_init() < 0) {
        LOG_INFO("XiaoZhi bridge init failed — control_center may be offline.");
        LOG_INFO("Voice server will run in offline mode.");
    }

    /* Start JSON-RPC server */
    jrpc_server_init(&my_server, VOICE_SERVER_PORT);

    jrpc_register_procedure(&my_server, rpc_voice_get_state,
            RPC_METHOD_VOICE_GET_STATE, NULL);
    jrpc_register_procedure(&my_server, rpc_voice_send_text,
            RPC_METHOD_VOICE_SEND_TEXT, NULL);
    jrpc_register_procedure(&my_server, rpc_voice_set_volume,
            RPC_METHOD_VOICE_SET_VOLUME, NULL);

    LOG_INFO("Registered 3 procedures. Server ready.");

    jrpc_server_run(&my_server);
    jrpc_server_destroy(&my_server);
    xz_bridge_exit();

    LOG_INFO("Voice server stopped.");
    return 0;
}
