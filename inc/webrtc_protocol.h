/**
 * @file webrtc_protocol.h
 * Shared constants for the WebRTC remote video monitoring pipeline.
 *
 * Relay:    camera_server streams MJPEG frames over TCP :8081
 *           (4-byte length prefix + JPEG binary)
 * Signalling: web_server proxies SDP/ICE between browser and
 *           webrtc_server over TCP :8082 (newline-delimited JSON)
 */
#ifndef WEBRTC_PROTOCOL_H
#define WEBRTC_PROTOCOL_H

#include <stdint.h>

/* ── Port assignments ─────────────────────────────────────────────────── */

#define WEBRTC_RELAY_PORT      8081   /* camera_server → webrtc_server frame relay    */
#define WEBRTC_SIGNALING_PORT  8082   /* web_server ↔ webrtc_server signalling        */

/* ── Relay binary frame protocol ──────────────────────────────────────── */

#define WEBRTC_RELAY_MAX_FRAME  (1024 * 1024)  /* 1 MB — matches CAMERA_SHM_MAX_FRAME   */

/*
 * Each frame on the relay TCP stream is prefixed by a 4-byte big-endian
 * uint32_t frame_size, followed by frame_size bytes of JPEG data:
 *
 *   [uint32_t be: frame_size] [frame_size bytes: JPEG]
 *   [uint32_t be: frame_size] [frame_size bytes: JPEG]
 *   ...
 */
typedef struct {
    uint32_t frame_size;   /* network byte order (big-endian) */
    /* followed by frame_size bytes of JPEG data */
} webrtc_relay_frame_hdr_t;

/* ── Signalling protocol (newline-delimited JSON over TCP :8082) ─────── */

/*
 * Message types exchanged between web_server and webrtc_server:
 *
 * → {"type":"offer","sdp":"v=0\r\n..."}\n
 * ← {"type":"answer","sdp":"v=0\r\n..."}\n
 *
 * → {"type":"ice","candidate":"...","sdpMid":"0","sdpMLineIndex":0}\n
 * ← {"type":"ok"}\n
 */

/* ── Signalling message buffer size ───────────────────────────────────── */

#define WEBRTC_SIGNALING_BUF_SIZE  8192   /* enough for SDP + ICE messages */

#endif /* WEBRTC_PROTOCOL_H */
