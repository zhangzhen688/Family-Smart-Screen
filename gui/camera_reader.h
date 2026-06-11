/**
 * @file camera_reader.h
 * Consumer-side shared-memory frame reader.
 *
 * Opens the POSIX shared memory and named semaphores created by
 * the camera_server streamer.  Provides non-blocking frame reads
 * suitable for calling from an LVGL timer callback.
 */
#ifndef CAMERA_READER_H
#define CAMERA_READER_H

#include <stdint.h>
#include <stddef.h>

/**
 * Open the shared memory region and named semaphores.
 *
 * The producer (camera_server streamer) must already have created
 * them; this function only opens, it never creates.
 *
 * @return 0 on success, -1 on error.
 */
int camera_reader_init(void);

/**
 * Try to read the next available frame from the ring buffer.
 *
 * Non-blocking: if no frame is ready, returns immediately with -1.
 * On success, *out_buf points directly into the shared memory region
 * and remains valid until the next camera_reader_read_frame() call
 * (which will advance the read cursor).
 *
 * @param out_buf   [out] pointer to JPEG frame data (do NOT free).
 * @param out_size  [out] size of the JPEG frame in bytes.
 * @return 0 on success, -1 if no frame is available.
 */
int camera_reader_read_frame(uint8_t **out_buf, size_t *out_size);

/**
 * Retrieve the video dimensions stored in the shared-memory header.
 *
 * @param width   [out] frame width in pixels.
 * @param height  [out] frame height in pixels.
 */
void camera_reader_get_dimensions(int *width, int *height);

/**
 * Release all resources: unmap shared memory, close semaphores.
 *
 * Does NOT unlink the semaphores or the shm — the producer owns
 * their lifecycle.
 */
void camera_reader_deinit(void);

/**
 * Query whether the reader has been successfully initialised.
 *
 * @return 1 if ready, 0 otherwise.
 */
int camera_reader_is_ready(void);

/**
 * Get the V4L2 pixel format from the shared-memory header.
 * @return V4L2 fourcc pixel format code (e.g. V4L2_PIX_FMT_YUYV).
 */
uint32_t camera_reader_get_pixel_format(void);

#endif /* CAMERA_READER_H */
