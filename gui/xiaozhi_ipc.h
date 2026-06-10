/**
 * @file xiaozhi_ipc.h
 * UDP IPC listener for xiaozhi control_center → GUI communication.
 * Receives device state, STT/TTS text, emotions on port 5679.
 */
#ifndef XIAOZHI_IPC_H
#define XIAOZHI_IPC_H

#include "common.h"

/* Callbacks registered by the voice UI */
typedef void (*xz_state_cb_t)(int state, const char *state_str);
typedef void (*xz_text_cb_t)(const char *text);
typedef void (*xz_emotion_cb_t)(const char *emotion);

/** Start UDP listener on port 5679. Non-blocking background thread. */
int xz_ipc_start(void);

/** Register state change callback. */
void xz_ipc_on_state(xz_state_cb_t cb);

/** Register text callback (STT result, activation code, etc). */
void xz_ipc_on_text(xz_text_cb_t cb);

/** Register emotion callback. */
void xz_ipc_on_emotion(xz_emotion_cb_t cb);

/** Stop listener. */
void xz_ipc_stop(void);

#endif
