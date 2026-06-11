/**
 * @file camera_reader.c
 * Consumer: reads JPEG frames from the POSIX shared-memory ring buffer.
 *
 * Designed to be called from an LVGL timer callback (UI thread).
 * Uses sem_trywait() so it never blocks — if no frame is available
 * the caller simply skips the refresh and waits for the next timer tick.
 */
#include "camera_reader.h"
#include "camera_shm.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ── File-static state ────────────────────────────────────────────────── */

static camera_shm_header_t *g_hdr       = NULL;
static int                   g_shm_fd   = -1;
static sem_t                *g_sem_empty = SEM_FAILED;
static sem_t                *g_sem_full  = SEM_FAILED;
static int                   g_ready    = 0;
static uint32_t              g_last_seq = 0;   /* last frame consumed */

/* ── Public API ───────────────────────────────────────────────────────── */

int camera_reader_init(void)
{
    if (g_ready) return 0;   /* already initialised */

    /* Open existing shared memory — producer must have created it */
    g_shm_fd = shm_open(CAMERA_SHM_NAME, O_RDWR, 0);
    if (g_shm_fd < 0) {
        LOG_ERROR("reader: shm_open(%s) failed: %s",
                  CAMERA_SHM_NAME, strerror(errno));
        return -1;
    }

    g_hdr = mmap(NULL, CAMERA_SHM_SIZE, PROT_READ | PROT_WRITE,
                 MAP_SHARED, g_shm_fd, 0);
    if (g_hdr == MAP_FAILED) {
        LOG_ERROR("reader: mmap shm failed: %s", strerror(errno));
        close(g_shm_fd); g_shm_fd = -1;
        return -1;
    }

    /* Validate magic */
    if (g_hdr->magic != CAMERA_SHM_MAGIC) {
        LOG_ERROR("reader: bad magic 0x%08X (expected 0x%08X)",
                  g_hdr->magic, CAMERA_SHM_MAGIC);
        munmap(g_hdr, CAMERA_SHM_SIZE); g_hdr = NULL;
        close(g_shm_fd); g_shm_fd = -1;
        return -1;
    }

    /* Open existing semaphores */
    g_sem_empty = sem_open(CAMERA_SEM_EMPTY_NAME, 0);
    g_sem_full  = sem_open(CAMERA_SEM_FULL_NAME, 0);

    if (g_sem_empty == SEM_FAILED || g_sem_full == SEM_FAILED) {
        LOG_ERROR("reader: sem_open failed: %s", strerror(errno));
        if (g_sem_empty != SEM_FAILED) sem_close(g_sem_empty);
        if (g_sem_full  != SEM_FAILED) sem_close(g_sem_full);
        munmap(g_hdr, CAMERA_SHM_SIZE); g_hdr = NULL;
        close(g_shm_fd); g_shm_fd = -1;
        return -1;
    }

    g_last_seq = 0;
    g_ready = 1;
    LOG_INFO("Camera reader ready (%dx%d)",
             g_hdr->width, g_hdr->height);
    return 0;
}

int camera_reader_read_frame(uint8_t **out_buf, size_t *out_size)
{
    if (!g_ready || !g_hdr || !out_buf || !out_size) return -1;

    /* Non-blocking wait — return immediately if no frame is ready */
    if (sem_trywait(g_sem_full) != 0) return -1;

    /* Check streaming is still active */
    if (!g_hdr->streaming) {
        sem_post(g_sem_full);   /* put it back */
        return -1;
    }

    uint32_t idx = g_hdr->read_idx;
    camera_shm_slot_t *slot = camera_shm_get_slot(g_hdr, idx);

    /* Skip duplicate (shouldn't happen with semaphores, but be safe) */
    if (slot->frame_seq == g_last_seq && g_last_seq != 0) {
        g_hdr->read_idx = (idx + 1) % CAMERA_SHM_NUM_SLOTS;
        sem_post(g_sem_empty);
        return -1;
    }

    g_last_seq = slot->frame_seq;

    /* Return pointer directly into shm — zero-copy read */
    *out_buf  = slot->data;
    *out_size = slot->data_size;

    /* Advance read cursor and signal an empty slot to the producer */
    g_hdr->read_idx = (idx + 1) % CAMERA_SHM_NUM_SLOTS;
    sem_post(g_sem_empty);

    return 0;
}

void camera_reader_get_dimensions(int *width, int *height)
{
    if (g_hdr && g_ready) {
        *width  = (int)g_hdr->width;
        *height = (int)g_hdr->height;
    } else {
        *width  = 0;
        *height = 0;
    }
}

void camera_reader_deinit(void)
{
    if (!g_ready) return;

    g_ready = 0;

    if (g_hdr && g_hdr != MAP_FAILED) {
        munmap(g_hdr, CAMERA_SHM_SIZE);
        g_hdr = NULL;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }
    if (g_sem_empty != SEM_FAILED) {
        sem_close(g_sem_empty);
        g_sem_empty = SEM_FAILED;
    }
    if (g_sem_full != SEM_FAILED) {
        sem_close(g_sem_full);
        g_sem_full = SEM_FAILED;
    }

    LOG_INFO("Camera reader deinitialised");
}

int camera_reader_is_ready(void)
{
    return g_ready;
}

uint32_t camera_reader_get_pixel_format(void)
{
    if (g_hdr && g_ready)
        return g_hdr->pixel_format;
    return 0;
}
