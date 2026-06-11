/**
 * @file camera_streamer.c
 * Producer: pumps V4L2 frames into the POSIX shared-memory ring buffer.
 *
 * Single-producer / single-consumer design:
 *   - Producer waits on sem_empty (free slot)
 *   - Copies JPEG frame into slot at write_idx
 *   - Advances write_idx, posts sem_full (filled slot)
 *   - Consumer does the inverse
 *
 * Lifecycle:
 *   camera_streamer_start()  → creates shm + sems + spawns thread
 *   camera_streamer_stop()   → signals stop + joins thread + tears down
 */
#include "camera_streamer.h"
#include "camera_shm.h"
#include "v4l2_capture.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

/* ── File-static globals ──────────────────────────────────────────────── */

static camera_shm_header_t *g_shm_hdr   = NULL;
static int                   g_shm_fd   = -1;
static sem_t                *g_sem_empty = SEM_FAILED;
static sem_t                *g_sem_full  = SEM_FAILED;
static pthread_t             g_thread;
static volatile int          g_running   = 0;

/* ── Capture thread ───────────────────────────────────────────────────── */

static void *streamer_thread(void *arg)
{
    (void)arg;

    while (g_shm_hdr->streaming) {
        /* Wait for at least one free slot (back-pressure) */
        if (sem_wait(g_sem_empty) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Check again — streaming may have been set to 0 while we waited */
        if (!g_shm_hdr->streaming) {
            sem_post(g_sem_empty);   /* put the slot back */
            break;
        }

        /* Dequeue a filled V4L2 buffer (blocking) */
        char *frame = NULL;
        size_t size = 0;
        if (camera_capture_frame(&frame, &size) < 0 || !frame) {
            /* On error, put the slot back and keep trying */
            sem_post(g_sem_empty);
            usleep(10000);
            continue;
        }

        /* Write frame into the slot */
        uint32_t idx = g_shm_hdr->write_idx;
        camera_shm_slot_t *slot = camera_shm_get_slot(g_shm_hdr, idx);

        slot->data_size = (uint32_t)size;
        slot->frame_seq = (uint32_t)(++g_shm_hdr->frame_seq);

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        slot->timestamp_ms = (uint64_t)ts.tv_sec * 1000ULL
                           + (uint64_t)ts.tv_nsec / 1000000ULL;

        memcpy(slot->data, frame, size);
        free(frame);

        /* Advance write cursor */
        g_shm_hdr->write_idx = (idx + 1) % CAMERA_SHM_NUM_SLOTS;

        /* Signal consumer: one more filled slot */
        sem_post(g_sem_full);
    }

    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */

int camera_streamer_start(void)
{
    if (g_running) return 0;  /* already running */

    /* ── Clean up stale semaphores from a previous crash ──────────── */
    sem_unlink(CAMERA_SEM_EMPTY_NAME);
    sem_unlink(CAMERA_SEM_FULL_NAME);

    /* ── Create / open shared memory ──────────────────────────────── */
    g_shm_fd = shm_open(CAMERA_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_shm_fd < 0) {
        LOG_ERROR("streamer: shm_open(%s) failed: %s",
                  CAMERA_SHM_NAME, strerror(errno));
        return -1;
    }

    if (ftruncate(g_shm_fd, CAMERA_SHM_SIZE) < 0) {
        LOG_ERROR("streamer: ftruncate failed: %s", strerror(errno));
        close(g_shm_fd); g_shm_fd = -1;
        return -1;
    }

    g_shm_hdr = mmap(NULL, CAMERA_SHM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, g_shm_fd, 0);
    if (g_shm_hdr == MAP_FAILED) {
        LOG_ERROR("streamer: mmap shm failed: %s", strerror(errno));
        close(g_shm_fd); g_shm_fd = -1;
        return -1;
    }

    /* ── Initialise header ────────────────────────────────────────── */
    memset(g_shm_hdr, 0, sizeof(camera_shm_header_t));
    g_shm_hdr->magic          = CAMERA_SHM_MAGIC;
    g_shm_hdr->version        = 1;
    g_shm_hdr->width          = (uint32_t)camera_get_width();
    g_shm_hdr->height         = (uint32_t)camera_get_height();
    g_shm_hdr->pixel_format   = camera_get_pixel_format();
    g_shm_hdr->max_frame_size = CAMERA_SHM_MAX_FRAME;
    g_shm_hdr->slot_count     = CAMERA_SHM_NUM_SLOTS;
    g_shm_hdr->write_idx      = 0;
    g_shm_hdr->read_idx       = 0;
    g_shm_hdr->frame_seq      = 0;
    g_shm_hdr->streaming      = 1;

    /* ── Create named semaphores ──────────────────────────────────── */
    g_sem_empty = sem_open(CAMERA_SEM_EMPTY_NAME, O_CREAT, 0666,
                           CAMERA_SHM_NUM_SLOTS);
    g_sem_full  = sem_open(CAMERA_SEM_FULL_NAME, O_CREAT, 0666, 0);

    if (g_sem_empty == SEM_FAILED || g_sem_full == SEM_FAILED) {
        LOG_ERROR("streamer: sem_open failed: %s", strerror(errno));
        if (g_sem_empty != SEM_FAILED) sem_close(g_sem_empty);
        if (g_sem_full  != SEM_FAILED) sem_close(g_sem_full);
        munmap(g_shm_hdr, CAMERA_SHM_SIZE);
        close(g_shm_fd); g_shm_fd = -1;
        g_shm_hdr = NULL;
        return -1;
    }

    /* ── Spawn producer thread ────────────────────────────────────── */
    g_running = 1;
    if (pthread_create(&g_thread, NULL, streamer_thread, NULL) != 0) {
        LOG_ERROR("streamer: pthread_create failed");
        g_running = 0;
        g_shm_hdr->streaming = 0;
        sem_close(g_sem_empty);
        sem_close(g_sem_full);
        munmap(g_shm_hdr, CAMERA_SHM_SIZE);
        close(g_shm_fd); g_shm_fd = -1;
        g_shm_hdr = NULL;
        return -1;
    }

    LOG_INFO("Camera streamer started (%dx%d, %d slots)",
             g_shm_hdr->width, g_shm_hdr->height, CAMERA_SHM_NUM_SLOTS);
    return 0;
}

void camera_streamer_stop(void)
{
    if (!g_running) return;

    LOG_INFO("Stopping camera streamer...");

    /* Signal the thread to exit */
    g_shm_hdr->streaming = 0;

    /* Post to both semaphores to unblock the thread if it's waiting */
    sem_post(g_sem_empty);
    sem_post(g_sem_full);

    /* Wait for thread to finish */
    pthread_join(g_thread, NULL);
    g_running = 0;

    /* Tear down shared memory */
    if (g_shm_hdr && g_shm_hdr != MAP_FAILED) {
        munmap(g_shm_hdr, CAMERA_SHM_SIZE);
        g_shm_hdr = NULL;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        shm_unlink(CAMERA_SHM_NAME);
        g_shm_fd = -1;
    }

    /* Tear down semaphores */
    if (g_sem_empty != SEM_FAILED) {
        sem_close(g_sem_empty);
        sem_unlink(CAMERA_SEM_EMPTY_NAME);
        g_sem_empty = SEM_FAILED;
    }
    if (g_sem_full != SEM_FAILED) {
        sem_close(g_sem_full);
        sem_unlink(CAMERA_SEM_FULL_NAME);
        g_sem_full = SEM_FAILED;
    }

    LOG_INFO("Camera streamer stopped");
}

int camera_streamer_is_running(void)
{
    return g_running;
}
