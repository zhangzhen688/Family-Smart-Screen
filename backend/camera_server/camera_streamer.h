/**
 * @file camera_streamer.h
 * Producer-side shared-memory streaming interface.
 *
 * Spawns a dedicated thread that pumps V4L2 frames into the
 * POSIX shared-memory ring buffer, synchronized via named semaphores.
 */
#ifndef CAMERA_STREAMER_H
#define CAMERA_STREAMER_H

/**
 * Start the streaming producer thread.
 *
 * Creates/opens the shared memory region, initialises the ring-buffer
 * header, creates named semaphores (empty / full), and spawns a
 * dedicated capture thread.
 *
 * Prerequisites: camera_init() and camera_start_stream() must already
 * have been called successfully.
 *
 * @return 0 on success, -1 on error.
 */
int camera_streamer_start(void);

/**
 * Stop the streaming producer thread.
 *
 * Signals the thread to exit, joins it, unmaps shared memory,
 * and destroys the named semaphores.
 */
void camera_streamer_stop(void);

/**
 * Query streamer state.
 * @return 1 if the producer thread is running, 0 otherwise.
 */
int camera_streamer_is_running(void);

#endif /* CAMERA_STREAMER_H */
