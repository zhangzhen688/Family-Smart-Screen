/**
 * @file v4l2_capture.h
 * V4L2 camera capture module — open /dev/video0, mmap buffers,
 * capture JPEG frames, save photos, basic video recording.
 */
#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H

#include "common.h"
#include <stddef.h>

/**
 * Initialize V4L2 camera.
 * Opens /dev/video0, queries caps, sets MJPEG 640x480, mmap buffers.
 * @return 0 on success, negative on error.
 */
int camera_init(void);

/**
 * Start video streaming.
 * @return 0 on success, negative on error.
 */
int camera_start_stream(void);

/**
 * Stop video streaming and release buffers.
 * @return 0 on success, negative on error.
 */
int camera_stop_stream(void);

/**
 * Capture a single JPEG frame (blocking until available).
 * @param out_buf  Output: pointer to frame data (caller must free).
 * @param out_size Output: size of frame data in bytes.
 * @return 0 on success, negative on error.
 */
int camera_capture_frame(char **out_buf, size_t *out_size);

/**
 * Save a JPEG frame to file.
 * @param filename Output file path (relative to photo dir).
 * @return 0 on success, negative on error.
 */
int camera_save_photo(const char *filename);

/**
 * Cleanup and close camera.
 */
void camera_exit(void);

#endif /* V4L2_CAPTURE_H */
