/**
 * @file camera_relay.h
 * TCP relay server that fans out MJPEG frames from the camera streamer
 * thread to network consumers (e.g. the WebRTC server).
 *
 * Single-threaded select()-based server. Non-blocking writes prevent a
 * slow client from back-pressuring the V4L2 capture pipeline.
 */
#ifndef CAMERA_RELAY_H
#define CAMERA_RELAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the relay server on @p port.
 * Spawns a dedicated thread that accept()s clients and broadcasts frames.
 * Returns 0 on success, -1 on error.
 */
int camera_relay_start(uint16_t port);

/**
 * Signal the relay thread to stop and join it.
 * Disconnects all clients gracefully.
 */
void camera_relay_stop(void);

/**
 * Push one JPEG frame to all connected clients.
 * Non-blocking: slow clients will have this frame dropped and may be
 * disconnected after a consecutive-failure threshold.
 *
 * @param data   Pointer to JPEG frame bytes
 * @param size   Size of JPEG frame in bytes
 */
void camera_relay_broadcast(const uint8_t *data, uint32_t size);

/**
 * Returns 1 if the relay server is running, 0 otherwise.
 */
int camera_relay_is_running(void);

/**
 * Returns the number of currently connected clients.
 */
int camera_relay_client_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_RELAY_H */
