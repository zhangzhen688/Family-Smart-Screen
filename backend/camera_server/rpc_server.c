/**
 * @file rpc_server.c
 * Camera JSON-RPC server (port 1235).
 *
 * Registers RPC methods for camera control, photo capture,
 * photo album listing/deletion.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "jsonrpc-c.h"
#include "rpc_protocol.h"
#include "v4l2_capture.h"
#include "camera_streamer.h"

static struct jrpc_server my_server;
static volatile int running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    LOG_INFO("Camera server shutting down...");
    jrpc_server_stop(&my_server);
    running = 0;
}

/* ── RPC: camera_start ──────────────────────────────────────────────── */
cJSON *rpc_camera_start(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params;
    cJSON *result = cJSON_CreateObject();

    if (camera_init() < 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "Cannot open camera device");
        return result;
    }
    if (camera_start_stream() < 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "Cannot start stream");
        return result;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "width",  CAM_DEFAULT_WIDTH);
    cJSON_AddNumberToObject(result, "height", CAM_DEFAULT_HEIGHT);
    return result;
}

/* ── RPC: camera_stop ───────────────────────────────────────────────── */
cJSON *rpc_camera_stop(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params;
    camera_stop_stream();
    camera_exit();

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    return result;
}

/* ── RPC: camera_capture_frame ──────────────────────────────────────── */
cJSON *rpc_camera_capture_frame(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params;
    cJSON *result = cJSON_CreateObject();
    char *frame = NULL;
    size_t size = 0;

    if (camera_capture_frame(&frame, &size) < 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        return result;
    }

    /* Encode JPEG data as base64 for JSON transport */
    /* Simple base64 encoder (inline) */
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t b64_len = ((size + 2) / 3) * 4 + 1;
    char *b64_data = malloc(b64_len);
    if (!b64_data) {
        free(frame);
        cJSON_AddBoolToObject(result, "ok", false);
        return result;
    }

    size_t out_pos = 0;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t triple = ((uint8_t)frame[i]) << 16;
        if (i + 1 < size) triple |= ((uint8_t)frame[i + 1]) << 8;
        if (i + 2 < size) triple |= ((uint8_t)frame[i + 2]);
        b64_data[out_pos++] = b64[(triple >> 18) & 0x3F];
        b64_data[out_pos++] = b64[(triple >> 12) & 0x3F];
        b64_data[out_pos++] = (i + 1 < size) ? b64[(triple >> 6) & 0x3F] : '=';
        b64_data[out_pos++] = (i + 2 < size) ? b64[triple & 0x3F] : '=';
    }
    b64_data[out_pos] = '\0';

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "data", b64_data);
    cJSON_AddNumberToObject(result, "size", (int)size);

    free(frame);
    free(b64_data);
    return result;
}

/* ── RPC: camera_take_photo ─────────────────────────────────────────── */
cJSON *rpc_camera_take_photo(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx;
    cJSON *result = cJSON_CreateObject();

    cJSON *fn = cJSON_GetArrayItem(params, 0);
    const char *filename = fn ? fn->valuestring : NULL;

    if (!filename) {
        /* Generate default name */
        static char auto_name[64];
        snprintf(auto_name, sizeof(auto_name), "photo_%ld.jpg", (long)time(NULL));
        filename = auto_name;
    }

    int ret = camera_save_photo(filename);
    cJSON_AddBoolToObject(result, "ok", (ret == 0));
    if (ret == 0) {
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", CAM_PHOTO_DIR, filename);
        cJSON_AddStringToObject(result, "path", full);
    }
    return result;
}

/* ── RPC: camera_start_recording ────────────────────────────────────── */
cJSON *rpc_camera_start_recording(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params; (void)id;
    cJSON *result = cJSON_CreateObject();
    /* TODO: Implement video recording (MJPG → AVI container) */
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "warning", "Recording not yet implemented");
    return result;
}

/* ── RPC: camera_stop_recording ─────────────────────────────────────── */
cJSON *rpc_camera_stop_recording(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    return result;
}

/* ── RPC: camera_list_photos ────────────────────────────────────────── */
cJSON *rpc_camera_list_photos(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params;
    cJSON *result = cJSON_CreateObject();
    cJSON *photos = cJSON_CreateArray();

    /* Ensure photo dir exists */
    mkdir(CAM_PHOTO_DIR, 0755);

    DIR *dir = opendir(CAM_PHOTO_DIR);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                cJSON_AddItemToArray(photos,
                    cJSON_CreateString(entry->d_name));
            }
        }
        closedir(dir);
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "photos", photos);
    return result;
}

/* ── RPC: camera_delete_photo ───────────────────────────────────────── */
cJSON *rpc_camera_delete_photo(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx;
    cJSON *result = cJSON_CreateObject();

    cJSON *fn = cJSON_GetArrayItem(params, 0);
    if (!fn || !fn->valuestring) {
        cJSON_AddBoolToObject(result, "ok", false);
        return result;
    }

    char fullpath[256];
    snprintf(fullpath, sizeof(fullpath), "%s/%s",
             CAM_PHOTO_DIR, fn->valuestring);

    cJSON_AddBoolToObject(result, "ok", (unlink(fullpath) == 0));
    return result;
}

/* ── RPC: camera_start_preview ────────────────────────────────────────── */
cJSON *rpc_camera_start_preview(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params;
    cJSON *result = cJSON_CreateObject();

    if (camera_streamer_start() < 0) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "Cannot start preview streamer");
        return result;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    return result;
}

/* ── RPC: camera_stop_preview ─────────────────────────────────────────── */
cJSON *rpc_camera_stop_preview(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx; (void)params;
    camera_streamer_stop();

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    return result;
}

int main(void)
{
    LOG_INFO("====================================");
    LOG_INFO("Smart Screen — Camera RPC Server");
    LOG_INFO("Port: %d", CAMERA_SERVER_PORT);
    LOG_INFO("====================================");

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    jrpc_server_init(&my_server, CAMERA_SERVER_PORT);

    jrpc_register_procedure(&my_server, rpc_camera_start,
            RPC_METHOD_CAMERA_START, NULL);
    jrpc_register_procedure(&my_server, rpc_camera_stop,
            RPC_METHOD_CAMERA_STOP, NULL);
    jrpc_register_procedure(&my_server, rpc_camera_capture_frame,
            RPC_METHOD_CAMERA_CAPTURE_FRAME, NULL);
    jrpc_register_procedure(&my_server, rpc_camera_take_photo,
            RPC_METHOD_CAMERA_TAKE_PHOTO, NULL);
    jrpc_register_procedure(&my_server, rpc_camera_start_recording,
            RPC_METHOD_CAMERA_START_RECORDING, NULL);
    jrpc_register_procedure(&my_server, rpc_camera_stop_recording,
            RPC_METHOD_CAMERA_STOP_RECORDING, NULL);
    jrpc_register_procedure(&my_server, rpc_camera_list_photos,
            RPC_METHOD_CAMERA_LIST_PHOTOS, NULL);
    jrpc_register_procedure(&my_server, rpc_camera_delete_photo,
            RPC_METHOD_CAMERA_DELETE_PHOTO, NULL);
    jrpc_register_procedure(&my_server, rpc_camera_start_preview,
            RPC_METHOD_CAMERA_START_PREVIEW, NULL);
    jrpc_register_procedure(&my_server, rpc_camera_stop_preview,
            RPC_METHOD_CAMERA_STOP_PREVIEW, NULL);

    LOG_INFO("Registered 10 procedures");
    LOG_INFO("Server ready.");

    jrpc_server_run(&my_server);
    jrpc_server_destroy(&my_server);
    camera_exit();

    LOG_INFO("Camera server stopped.");
    return 0;
}
