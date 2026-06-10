/**
 * @file xiaozhi_bridge.h
 * Bridge between our JSON-RPC GUI and xiaozhi-linux UDP IPC.
 *
 * The xiaozhi-linux architecture uses UDP IPC:
 *   control_center listens on UI_PORT_UP (5678), sends on UI_PORT_DOWN (5679)
 *   sound_app uploads audio to AUDIO_PORT_UP (5676), receives on AUDIO_PORT_DOWN (5677)
 *
 * This bridge:
 *   - Sends device state changes from control_center → JSON-RPC → GUI
 *   - Forwards voice commands from GUI → JSON-RPC → UDP → control_center
 *   - Monitors control_center health
 */
#ifndef XIAOZHI_BRIDGE_H
#define XIAOZHI_BRIDGE_H

#include "common.h"

/* ── Bridge state ───────────────────────────────────────────────────── */
typedef enum {
    XZ_STATE_UNKNOWN = 0,
    XZ_STATE_IDLE,
    XZ_STATE_CONNECTING,
    XZ_STATE_LISTENING,
    XZ_STATE_SPEAKING,
    XZ_STATE_ACTIVATING,
    XZ_STATE_ERROR,
} xiaozhi_state_t;

/* ── Bridge API ─────────────────────────────────────────────────────── */

/** Initialize UDP IPC endpoints to talk to control_center. */
int xz_bridge_init(void);

/** Send a start-listening command to control_center. */
int xz_bridge_start_listening(void);

/** Send a text-to-speech request. */
int xz_bridge_send_tts(const char *text);

/** Set output volume (0-100). */
int xz_bridge_set_volume(int volume);

/** Poll for incoming messages from control_center (non-blocking).
 *  Updates internal state; call xz_bridge_get_state() after. */
int xz_bridge_poll(void);

/** Get current xiaozhi state. */
xiaozhi_state_t xz_bridge_get_state(void);

/** Get last recognized STT text (caller must copy, pointer is internal). */
const char *xz_bridge_get_stt_text(void);

/** Get last received IoT data as JSON string. */
const char *xz_bridge_get_iot_data(void);

/** Cleanup. */
void xz_bridge_exit(void);

#endif /* XIAOZHI_BRIDGE_H */
