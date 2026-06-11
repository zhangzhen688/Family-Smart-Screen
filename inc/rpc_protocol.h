/**
 * @file rpc_protocol.h
 * @brief Shared JSON-RPC protocol definitions for smart screen project.
 *
 * Defines port numbers, method names, and JSON structures used by
 * the GUI frontend and all backend server processes.
 */

#ifndef RPC_PROTOCOL_H
#define RPC_PROTOCOL_H

/* ── Server Port Numbers ────────────────────────────────────────────── */

#define DEVICE_SERVER_PORT  1234
#define CAMERA_SERVER_PORT  1235
#define VOICE_SERVER_PORT   1236

/* ── Default Connection ─────────────────────────────────────────────── */

#define RPC_SERVER_ADDR     "127.0.0.1"
#define RPC_TIMEOUT_SEC     5

/* ── Device Server Methods ──────────────────────────────────────────── */

#define RPC_METHOD_LED_SET       "led_set"
#define RPC_METHOD_LED_GET       "led_get"
#define RPC_METHOD_DHT11_READ    "dht11_read"
#define RPC_METHOD_SG90_SET      "sg90_set"
#define RPC_METHOD_AP3216C_READ  "ap3216c_read"

/* ── Camera Server Methods ──────────────────────────────────────────── */

#define RPC_METHOD_CAMERA_START           "camera_start"
#define RPC_METHOD_CAMERA_STOP            "camera_stop"
#define RPC_METHOD_CAMERA_CAPTURE_FRAME   "camera_capture_frame"
#define RPC_METHOD_CAMERA_TAKE_PHOTO      "camera_take_photo"
#define RPC_METHOD_CAMERA_START_RECORDING "camera_start_recording"
#define RPC_METHOD_CAMERA_STOP_RECORDING  "camera_stop_recording"
#define RPC_METHOD_CAMERA_LIST_PHOTOS     "camera_list_photos"
#define RPC_METHOD_CAMERA_DELETE_PHOTO    "camera_delete_photo"
#define RPC_METHOD_CAMERA_START_PREVIEW  "camera_start_preview"
#define RPC_METHOD_CAMERA_STOP_PREVIEW   "camera_stop_preview"

/* ── Voice Server Methods ───────────────────────────────────────────── */

#define RPC_METHOD_VOICE_GET_STATE   "voice_get_state"
#define RPC_METHOD_VOICE_SEND_TEXT   "voice_send_text"
#define RPC_METHOD_VOICE_SET_VOLUME  "voice_set_volume"

/* ── Common Error Codes ─────────────────────────────────────────────── */

#define RPC_OK             0
#define RPC_ERR_CONNECT    -1
#define RPC_ERR_SEND       -2
#define RPC_ERR_RECV       -3
#define RPC_ERR_PARSE      -4
#define RPC_ERR_TIMEOUT    -5
#define RPC_ERR_DEVICE     -6

#endif /* RPC_PROTOCOL_H */
