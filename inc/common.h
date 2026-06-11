/**
 * @file common.h
 * @brief Common types and macros shared across the smart screen project.
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Platform Detection ─────────────────────────────────────────────── */

/* SIMULATOR_LINUX is set via CMake option; when not set, we target ARM */
#ifndef SIMULATOR_LINUX
#define TARGET_ARM
#endif

/* ── LED Count ──────────────────────────────────────────────────────── */

#define LED_COUNT    6

/* ── Camera Defaults ────────────────────────────────────────────────── */

#define CAM_DEFAULT_WIDTH   640
#define CAM_DEFAULT_HEIGHT  480
/* Use by-path symlink — stable across USB re-enumerations.
   If the camera changes USB port, update this path. */
#define CAM_DEVICE_NODE     "/dev/v4l/by-path/pci-0000:02:03.0-usb-0:1:1.0-video-index0"
#define CAM_PHOTO_DIR       "./photos"

/* ── Convenience Macros ─────────────────────────────────────────────── */

#define ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

#define LOG_INFO(fmt, ...)   printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_STUB(fmt, ...)   printf("[STUB] " fmt "\n", ##__VA_ARGS__)

#endif /* COMMON_H */
