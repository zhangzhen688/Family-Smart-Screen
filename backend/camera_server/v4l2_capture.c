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

static int  g_video_fd = -1;
static int  g_streaming = 0;
static char *g_buffers[CAM_BUF_COUNT];
static size_t g_buf_lengths[CAM_BUF_COUNT];

int camera_init(void)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;

    g_video_fd = open(CAM_DEVICE_NODE, O_RDWR);
    if (g_video_fd < 0) {
        LOG_ERROR("Cannot open %s: %s", CAM_DEVICE_NODE, strerror(errno));
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

    /* Set format: MJPEG 640x480 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = CAM_DEFAULT_WIDTH;
    fmt.fmt.pix.height      = CAM_DEFAULT_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (ioctl(g_video_fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("VIDIOC_S_FMT MJPEG failed: %s — trying YUYV",
                  strerror(errno));
        /* Fallback: try YUYV */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (ioctl(g_video_fd, VIDIOC_S_FMT, &fmt) < 0) {
            LOG_ERROR("VIDIOC_S_FMT YUYV also failed");
            close(g_video_fd); g_video_fd = -1;
            return -5;
        }
    }

    LOG_INFO("Camera format: %dx%d %c%c%c%c",
             fmt.fmt.pix.width, fmt.fmt.pix.height,
             fmt.fmt.pix.pixelformat & 0xff,
             (fmt.fmt.pix.pixelformat >> 8) & 0xff,
             (fmt.fmt.pix.pixelformat >> 16) & 0xff,
             (fmt.fmt.pix.pixelformat >> 24) & 0xff);

    return 0;
}

int camera_start_stream(void)
{
    if (g_video_fd < 0 || g_streaming) return -1;

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
    LOG_INFO("Camera closed");
}
