/**
 * @file camera_shm.h
 * Shared-memory protocol for camera live preview.
 *
 * Control flow (start/stop/photo) stays on JSON-RPC over TCP.
 * Data flow (video frames) goes through POSIX shared memory —
 * a single-producer / single-consumer ring buffer synchronized
 * with two named POSIX semaphores.
 *
 * Layout:
 *   [camera_shm_header_t] [slot 0] [slot 1] [slot 2]
 *
 * Producer: camera_server streamer thread
 * Consumer: smart_gui LVGL timer callback
 */
#ifndef CAMERA_SHM_H
#define CAMERA_SHM_H

#include <stdint.h>
#include <stddef.h>

/* ── Shared memory region ─────────────────────────────────────────────── */

#define CAMERA_SHM_NAME         "/camera_shm"
#define CAMERA_SHM_NUM_SLOTS    3
#define CAMERA_SHM_MAX_FRAME    (1024 * 1024)      /* 1 MB — handles YUYV 640×480 = 614 KB */

/* slot body = metadata(16) + data(max_frame_size) */
#define CAMERA_SHM_SLOT_SIZE    (16 + CAMERA_SHM_MAX_FRAME)
#define CAMERA_SHM_SIZE         (sizeof(camera_shm_header_t) + \
                                 CAMERA_SHM_NUM_SLOTS * CAMERA_SHM_SLOT_SIZE)

/* ── Named semaphores ─────────────────────────────────────────────────── */

#define CAMERA_SEM_EMPTY_NAME   "/camera_sem_empty"  /* init = NUM_SLOTS */
#define CAMERA_SEM_FULL_NAME    "/camera_sem_full"   /* init = 0         */

/* ── Magic number ─────────────────────────────────────────────────────── */

#define CAMERA_SHM_MAGIC        0xCA4E0A01

/* ── Header (placed at the start of the shared memory) ────────────────── */

typedef struct {
    uint32_t magic;             /* CAMERA_SHM_MAGIC                       */
    uint32_t version;           /* protocol version (1)                   */
    uint32_t width;             /* frame width in pixels                  */
    uint32_t height;            /* frame height in pixels                 */
    uint32_t pixel_format;      /* V4L2 pixel format (e.g. MJPEG)         */
    uint32_t max_frame_size;    /* = CAMERA_SHM_MAX_FRAME                 */
    uint32_t slot_count;        /* = CAMERA_SHM_NUM_SLOTS                 */

    /* Cursors — each is written ONLY by its owner (SPSC safe)            */
    volatile uint32_t write_idx;  /* producer writes here                 */
    volatile uint32_t read_idx;   /* consumer reads from here             */
    volatile uint64_t frame_seq;  /* monotonic frame counter              */
    volatile uint32_t streaming;  /* 1 = producer active, 0 = stopped     */

    uint8_t  reserved[24];        /* pad header to 64 bytes (cache line)  */
} camera_shm_header_t;

/* ── Frame slot (placed after the header) ─────────────────────────────── */

typedef struct {
    uint32_t data_size;         /* actual JPEG size; 0 = empty            */
    uint32_t frame_seq;         /* copy of frame_seq at capture time      */
    uint64_t timestamp_ms;      /* CLOCK_MONOTONIC timestamp              */
    uint8_t  data[];            /* JPEG frame data (max_frame_size bytes) */
} camera_shm_slot_t;

/* ── Helper: get pointer to a slot in the shm region ─────────────────── */

static inline camera_shm_slot_t *
camera_shm_get_slot(camera_shm_header_t *hdr, uint32_t idx)
{
    uint8_t *base = (uint8_t *)hdr;
    return (camera_shm_slot_t *)(base + sizeof(camera_shm_header_t)
                                 + idx * CAMERA_SHM_SLOT_SIZE);
}

#endif /* CAMERA_SHM_H */
