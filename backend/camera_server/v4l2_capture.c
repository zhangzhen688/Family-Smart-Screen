/**
 * @file v4l2_capture.c
 * V4L2 camera capture implementation.
 *
 * Based on reference: refs/linux简易相机/v4l2-qt/v4l2.cpp
 *   - Open /dev/video0
 *   - Query capabilities (VIDIOC_QUERYCAP)
 *   - Set format MJPEG 640x480 (VIDIOC_S_FMT)
 *   - Request mmap buffers (VIDIOC_REQBUFS)
 *   - Queue buffers (VIDIOC_QBUF)
 *   - Start streaming (VIDIOC_STREAMON)
 *   - Dequeue frames (VIDIOC_DQBUF)
 */
#include "v4l2_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/videodev2.h>

#define CAM_BUF_COUNT  4

static int      g_video_fd = -1;
static int      g_streaming = 0;
static int      g_width = 0;
static int      g_height = 0;
static uint32_t g_pixel_format = 0;
static char    *g_buffers[CAM_BUF_COUNT];
static size_t   g_buf_lengths[CAM_BUF_COUNT];

int camera_init(void)
{
    /* Already initialised — idempotent */
    if (g_video_fd >= 0) {
        LOG_INFO("Camera already initialised (fd=%d), reusing", g_video_fd);
        return 0;
    }

    struct v4l2_capability cap;
    struct v4l2_format fmt;

    /* Try configured device first, then fall back through /dev/video0..5 */
    const char *paths[] = {
        CAM_DEVICE_NODE,
        "/dev/video0", "/dev/video1", "/dev/video2",
        "/dev/video3", "/dev/video4", "/dev/video5",
        NULL
    };

    g_video_fd = -1;
    for (int i = 0; paths[i] != NULL; i++) {
        g_video_fd = open(paths[i], O_RDWR);
        if (g_video_fd >= 0) {
            LOG_INFO("Opened camera device: %s", paths[i]);
            break;
        }
    }

    if (g_video_fd < 0) {
        LOG_ERROR("Cannot open any camera device (tried %s .. /dev/video5)",
                  CAM_DEVICE_NODE);
        return -1;
    }

    /* Query capabilities */
    memset(&cap, 0, sizeof(cap));
    if (ioctl(g_video_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("VIDIOC_QUERYCAP failed: %s", strerror(errno));
        close(g_video_fd); g_video_fd = -1;
        return -2;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOG_ERROR("Device does not support video capture");
        close(g_video_fd); g_video_fd = -1;
        return -3;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG_ERROR("Device does not support streaming I/O");
        close(g_video_fd); g_video_fd = -1;
        return -4;
    }

    LOG_INFO("Camera: %s (driver: %s)", cap.card, cap.driver);

    /* Set format: MJPEG 640x480.
     * Note: this HIK 1080P camera reports YUYV support via VIDIOC_S_FMT
     * but the hardware continues to output variable-size MJPEG frames
     * regardless — a known UVC quirk.  We request MJPEG explicitly so
     * the pixel-format metadata in the shm header is accurate. */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = CAM_DEFAULT_WIDTH;
    fmt.fmt.pix.height      = CAM_DEFAULT_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (ioctl(g_video_fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("VIDIOC_S_FMT MJPEG failed: %s — trying YUYV",
                  strerror(errno));
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (ioctl(g_video_fd, VIDIOC_S_FMT, &fmt) < 0) {
            LOG_ERROR("VIDIOC_S_FMT YUYV also failed");
            close(g_video_fd); g_video_fd = -1;
            return -5;
        }
    }

    g_width        = fmt.fmt.pix.width;
    g_height       = fmt.fmt.pix.height;
    g_pixel_format = fmt.fmt.pix.pixelformat;

    LOG_INFO("Camera format: %dx%d %c%c%c%c",
             g_width, g_height,
             g_pixel_format & 0xff,
             (g_pixel_format >> 8) & 0xff,
             (g_pixel_format >> 16) & 0xff,
             (g_pixel_format >> 24) & 0xff);

    return 0;
}

int camera_start_stream(void)
{
    if (g_video_fd < 0) return -1;

    /* Already streaming — idempotent */
    if (g_streaming) {
        LOG_INFO("Camera stream already active, reusing");
        return 0;
    }

    /* Request mmap buffers */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count  = CAM_BUF_COUNT;

    if (ioctl(g_video_fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR("VIDIOC_REQBUFS failed: %s", strerror(errno));
        return -1;
    }

    /* Map and queue each buffer */
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (ioctl(g_video_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("VIDIOC_QUERYBUF[%d] failed", i);
            return -1;
        }

        g_buffers[i] = mmap(NULL, buf.length,
                            PROT_READ | PROT_WRITE, MAP_SHARED,
                            g_video_fd, buf.m.offset);
        g_buf_lengths[i] = buf.length;

        if (g_buffers[i] == MAP_FAILED) {
            LOG_ERROR("mmap[%d] failed", i);
            return -1;
        }

        if (ioctl(g_video_fd, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("VIDIOC_QBUF[%d] failed", i);
            return -1;
        }
    }

    /* Start streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g_video_fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("VIDIOC_STREAMON failed: %s", strerror(errno));
        return -1;
    }

    g_streaming = 1;
    LOG_INFO("Camera streaming started (%d buffers)", req.count);
    return 0;
}

int camera_stop_stream(void)
{
    if (!g_streaming) return 0;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(g_video_fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        if (g_buffers[i] && g_buffers[i] != MAP_FAILED) {
            munmap(g_buffers[i], g_buf_lengths[i]);
        }
        g_buffers[i] = NULL;
    }

    g_streaming = 0;
    LOG_INFO("Camera streaming stopped");
    return 0;
}

int camera_capture_frame(char **out_buf, size_t *out_size)
{
    if (!g_streaming || !out_buf || !out_size) return -1;

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    /* Dequeue a filled buffer (blocks) */
    if (ioctl(g_video_fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return -EAGAIN;
        LOG_ERROR("VIDIOC_DQBUF failed: %s", strerror(errno));
        return -1;
    }

    /* Copy frame data to caller */
    *out_size = buf.bytesused;
    *out_buf  = malloc(buf.bytesused);
    if (*out_buf) {
        memcpy(*out_buf, g_buffers[buf.index], buf.bytesused);
    }

    /* Re-queue the buffer */
    if (ioctl(g_video_fd, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("VIDIOC_QBUF failed: %s", strerror(errno));
    }

    return (*out_buf) ? 0 : -1;
}

int camera_save_photo(const char *filename)
{
    if (!filename) return -1;

    /* Ensure photo directory exists */
    struct stat st;
    if (stat(CAM_PHOTO_DIR, &st) != 0) {
        mkdir(CAM_PHOTO_DIR, 0755);
    }

    char fullpath[256];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", CAM_PHOTO_DIR, filename);

    char *frame = NULL;
    size_t frame_size = 0;

    if (camera_capture_frame(&frame, &frame_size) < 0) {
        LOG_ERROR("Failed to capture frame for photo");
        return -1;
    }

    int fd = open(fullpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOG_ERROR("Cannot create photo file %s: %s", fullpath, strerror(errno));
        free(frame);
        return -1;
    }

    ssize_t written = write(fd, frame, frame_size);
    close(fd);
    free(frame);

    if (written < 0 || (size_t)written != frame_size) {
        LOG_ERROR("Write photo failed");
        return -1;
    }

    LOG_INFO("Photo saved: %s (%zu bytes)", fullpath, frame_size);
    return 0;
}

void camera_exit(void)
{
    camera_stop_stream();
    if (g_video_fd >= 0) {
        close(g_video_fd);
        g_video_fd = -1;
    }
    g_width = g_height = 0;
    g_pixel_format = 0;
    LOG_INFO("Camera closed");
}

int camera_get_width(void)
{
    return g_width;
}

int camera_get_height(void)
{
    return g_height;
}

uint32_t camera_get_pixel_format(void)
{
    return g_pixel_format;
}
