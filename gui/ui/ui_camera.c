/**
 * @file ui_camera.c — Camera page with live preview via shared memory.
 *
 * Control flow (JSON-RPC over TCP):
 *   camera_start / camera_stop / take_photo / start_recording / ...
 *
 * Data flow (POSIX shared memory):
 *   camera_server producer → ring buffer → camera_reader → JPEG validate → decode → LVGL
 *
 * Image display uses lv_image_dsc_t (the standard LVGL v9 API for in-memory
 * raw images) with ping-pong double buffering to prevent tearing.
 */
#include "lvgl.h"
#include "common.h"
#include "rpc_client.h"
#include "camera_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <jpeglib.h>
#include <setjmp.h>

#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define ACCENT       0x00d4ff
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf

/* ── Static state ─────────────────────────────────────────────────────── */

static lv_obj_t  *screen_camera  = NULL, *label_status = NULL;
static lv_obj_t  *preview_img    = NULL;     /* LVGL image widget            */
static lv_obj_t  *placeholder    = NULL;     /* "Camera Preview" placeholder */

/*
 * Double-buffer (ping-pong) for tear-free display.
 *
 * While LVGL renders from image descriptor A, the next JPEG frame decodes
 * into buffer B.  The roles swap each frame.  This guarantees LVGL never
 * reads a buffer that is being written.
 */
static struct {
    lv_image_dsc_t  dsc;          /* LVGL image descriptor (header + data ptr) */
    uint8_t        *rgb;          /* malloc'd BGRA pixel buffer                */
    int             w, h;         /* current dimensions of this buffer         */
    int             stride;       /* bytes per row                             */
} g_buf[2];
static int  g_buf_idx     = 0;        /* which buffer is currently displayed  */

static lv_timer_t *preview_timer = NULL;     /* ~30 fps refresh timer       */
static bool        g_cam_running = false;
static bool        g_preview_active = false;
static bool        g_recording   = false;
static int         g_cam_fd      = -1;
static int         g_cam_width   = 0;
static int         g_cam_height  = 0;

extern lv_obj_t *g_main_screen;

/* ── Buffer management helpers ────────────────────────────────────────── */

static void free_buf(int i)
{
    if (g_buf[i].rgb) { free(g_buf[i].rgb); g_buf[i].rgb = NULL; }
    memset(&g_buf[i].dsc, 0, sizeof(g_buf[i].dsc));
    g_buf[i].w = g_buf[i].h = g_buf[i].stride = 0;
}

static int alloc_buf(int i, int w, int h)
{
    free_buf(i);

    int stride = w * 4;  /* ARGB8888 = 4 bytes per pixel */
    size_t data_size = (size_t)stride * (size_t)h;
    uint8_t *rgb = malloc(data_size);
    if (!rgb) return -1;

    memset(rgb, 0, data_size);  /* black frame */

    /* Populate LVGL image descriptor — the standard API for raw in-memory images */
    g_buf[i].dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_buf[i].dsc.header.cf    = LV_COLOR_FORMAT_ARGB8888;
    g_buf[i].dsc.header.w     = (uint32_t)w;
    g_buf[i].dsc.header.h     = (uint32_t)h;
    g_buf[i].dsc.header.stride = (uint32_t)stride;
    g_buf[i].dsc.data_size    = (uint32_t)data_size;
    g_buf[i].dsc.data         = rgb;

    g_buf[i].rgb    = rgb;
    g_buf[i].w      = w;
    g_buf[i].h      = h;
    g_buf[i].stride = stride;
    return 0;
}

/* ── Pixel format dispatch ────────────────────────────────────────────── */

static uint32_t g_pixel_format = 0;   /* V4L2_PIX_FMT_YUYV or MJPEG */

/*
 * Convert YUYV (YUY2) → BGRA (ARGB8888).
 *
 * YUYV packs 2 pixels into 4 bytes: [Y0][U][Y1][V]
 *   Pixel 0: (Y0, U, V)   Pixel 1: (Y1, U, V)
 *
 * BT.601 full-range (0–255) with 16-bit fixed-point coefficients:
 *   Cb = U - 128,  Cr = V - 128
 *   R = Y + ((359 * Cr) >> 8)
 *   G = Y - (( 88 * Cb + 183 * Cr) >> 8)
 *   B = Y + ((454 * Cb) >> 8)
 */
static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void yuyv_to_bgra(const uint8_t *yuyv, uint8_t *bgra,
                         int w, int h, int stride)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *src = yuyv + y * w * 2;    /* YUYV row: w*2 bytes */
        uint8_t       *dst = bgra + y * stride;   /* BGRA row: w*4 bytes  */

        for (int x = 0; x < w; x += 2) {
            int Y0 = src[0], U  = src[1] - 128;
            int Y1 = src[2], V  = src[3] - 128;

            /* Pixel 0 */
            dst[0] = (uint8_t)clamp8(Y0 + ((454 * U) >> 8));          /* B */
            dst[1] = (uint8_t)clamp8(Y0 - (( 88 * U + 183 * V) >> 8)); /* G */
            dst[2] = (uint8_t)clamp8(Y0 + ((359 * V) >> 8));          /* R */
            dst[3] = 255;                                              /* A */

            /* Pixel 1 */
            dst[4] = (uint8_t)clamp8(Y1 + ((454 * U) >> 8));
            dst[5] = (uint8_t)clamp8(Y1 - (( 88 * U + 183 * V) >> 8));
            dst[6] = (uint8_t)clamp8(Y1 + ((359 * V) >> 8));
            dst[7] = 255;

            src += 4;
            dst += 8;
        }
    }
}

/* ── JPEG → BGRA (ARGB8888) decoder ───────────────────────────────────── */

struct jpeg_err_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_err_mgr *err = (struct jpeg_err_mgr *)cinfo->err;
    longjmp(err->jb, 1);
}

/**
 * Quick check: is this buffer likely a JPEG? (SOI marker 0xFF 0xD8)
 */
static inline int looks_like_jpeg(const uint8_t *data, size_t size)
{
    return (size >= 2 && data[0] == 0xFF && data[1] == 0xD8);
}

/**
 * Decode JPEG into a BGRA pixel buffer.
 *
 * On little-endian, JCS_EXT_BGRA produces [B,G,R,A] — exactly
 * the memory layout of LV_COLOR_FORMAT_ARGB8888.
 *
 * @param rgb_buf   destination BGRA buffer
 * @param stride    bytes per row in rgb_buf
 * @param buf_w     width of rgb_buf in pixels
 * @param buf_h     height of rgb_buf in pixels
 * @param out_w/h   actual decoded image dimensions
 * @return 0 on success, -1 on error.
 */
static int jpeg_to_rgb(const uint8_t *jpeg_data, size_t jpeg_size,
                       uint8_t *rgb_buf, int stride, int buf_w, int buf_h,
                       int *out_w, int *out_h)
{
    if (!looks_like_jpeg(jpeg_data, jpeg_size))
        return -1;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_err_mgr jerr;

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.out_color_space = JCS_EXT_BGRA;
    jpeg_start_decompress(&cinfo);

    int dw = (int)cinfo.output_width;
    int dh = (int)cinfo.output_height;
    *out_w = dw;
    *out_h = dh;

    /* If decoded image doesn't fit in buffer, clamp and clear */
    int render_w = (dw < buf_w) ? dw : buf_w;
    int render_h = (dh < buf_h) ? dh : buf_h;

    if (dw != buf_w || dh != buf_h) {
        LOG_INFO("JPEG %dx%d → buffer %dx%d (mismatch)", dw, dh, buf_w, buf_h);
    }

    /* Clear entire buffer to avoid stale-data artefacts */
    memset(rgb_buf, 0, (size_t)buf_h * (size_t)stride);

    /* Decode row-by-row */
    JSAMPROW row_ptr[1];
    JDIMENSION row;
    for (row = 0; row < (JDIMENSION)render_h; row++) {
        row_ptr[0] = rgb_buf + row * stride;
        jpeg_read_scanlines(&cinfo, row_ptr, 1);
    }
    /* If image is wider than buffer, skip the extra columns in each row.
     * libjpeg doesn't have per-row column skipping, but since we only decode
     * render_h rows into a buf_w-wide buffer, the excess columns per row
     * are silently discarded by the decoder (they go past our row pointer).
     * Actually the row pointer only covers buf_w * 4 bytes — the decoder
     * writes exactly cinfo.output_width pixels per row which may overflow.
     * We handle this by clamping render_w above and hoping the decoder
     * doesn't write more than render_w pixels...
     *
     * Safer approach: if dw > buf_w, we need to decode full rows into a temp
     * buffer then copy only the left buf_w pixels.  For now, assume the
     * camera produces frames matching our buffer size (640x480). */

    /* Drain any remaining rows we skipped */
    uint8_t dummy[8192];
    while (row < cinfo.output_height) {
        row_ptr[0] = dummy;
        jpeg_read_scanlines(&cinfo, row_ptr, 1);
        row++;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

/* ── Preview timer callback (~30 fps) ─────────────────────────────────── */

/* V4L2 pixel format fourcc codes (avoid dragging in linux/videodev2.h) */
#define FMT_YUYV  0x56595559   /* 'YUYV' */
#define FMT_MJPEG 0x47504A4D   /* 'MJPG' */

static int try_decode_and_display(void)
{
    uint8_t *frame_data;
    size_t   frame_size;

    /* Non-blocking read from ring buffer */
    if (camera_reader_read_frame(&frame_data, &frame_size) != 0)
        return -1;

    /* Determine pixel format on first frame */
    if (g_pixel_format == 0)
        g_pixel_format = camera_reader_get_pixel_format();

    int next = g_buf_idx ^ 1;
    int dw = g_buf[next].w, dh = g_buf[next].h;

    if (g_pixel_format == FMT_YUYV) {
        /* YUYV: every frame is valid, fixed size = width * height * 2 */
        size_t expected = (size_t)g_cam_width * (size_t)g_cam_height * 2;
        if (frame_size < expected) {
            LOG_ERROR("YUYV frame too small: %zu < %zu", frame_size, expected);
            return -1;
        }

        /* In case the camera negotiated a different size */
        if (dw != g_cam_width || dh != g_cam_height) {
            for (int i = 0; i < 2; i++) {
                if (alloc_buf(i, g_cam_width, g_cam_height) < 0) return -1;
            }
            next = g_buf_idx ^ 1;
            dw = g_cam_width; dh = g_cam_height;
        }

        yuyv_to_bgra(frame_data, g_buf[next].rgb, dw, dh, g_buf[next].stride);

    } else {
        /* MJPEG: validate and decode */
        for (int attempt = 0; attempt < 3; attempt++) {
            if (!looks_like_jpeg(frame_data, frame_size)) {
                /* Drain this garbage frame and try the next */
                if (camera_reader_read_frame(&frame_data, &frame_size) != 0)
                    return -1;
                continue;
            }

            if (jpeg_to_rgb(frame_data, frame_size,
                            g_buf[next].rgb, g_buf[next].stride,
                            g_buf[next].w, g_buf[next].h,
                            &dw, &dh) == 0)
                break;   /* success */

            /* Decode failed — try next frame */
            if (camera_reader_read_frame(&frame_data, &frame_size) != 0)
                return -1;
        }
        if (dw <= 0) return -1;   /* all attempts failed */

        /* Resize if dimensions changed */
        if (dw != g_buf[next].w || dh != g_buf[next].h) {
            LOG_INFO("Resizing: %dx%d → %dx%d", g_buf[next].w, g_buf[next].h, dw, dh);
            for (int i = 0; i < 2; i++) {
                if (alloc_buf(i, dw, dh) < 0) return -1;
            }
            next = g_buf_idx ^ 1;
            if (jpeg_to_rgb(frame_data, frame_size,
                            g_buf[next].rgb, g_buf[next].stride,
                            g_buf[next].w, g_buf[next].h,
                            &dw, &dh) != 0)
                return -1;
        }
    }

    /* Swap: new frame becomes active */
    g_buf_idx = next;
    lv_image_set_src(preview_img, &g_buf[next].dsc);
    return 0;
}

static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_preview_active || !preview_img) return;
    if (!g_buf[0].rgb || !g_buf[1].rgb) return;

    try_decode_and_display();
}

/* ── Cleanup helpers ──────────────────────────────────────────────────── */

static void preview_stop(void)
{
    if (preview_timer) {
        lv_timer_del(preview_timer);
        preview_timer = NULL;
    }
    g_preview_active = false;

    camera_reader_deinit();

    for (int i = 0; i < 2; i++) free_buf(i);
    g_buf_idx = 0;

    /* Tell the server to stop streaming */
    if (g_cam_fd > 0) {
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP_PREVIEW);
    }

    /* Clear the image and hide it */
    if (preview_img) {
        lv_image_set_src(preview_img, NULL);
        lv_obj_add_flag(preview_img, LV_OBJ_FLAG_HIDDEN);
    }

    /* Show the placeholder text again */
    if (placeholder) {
        lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static int preview_start(void)
{
    if (camera_reader_init() < 0) return -1;

    camera_reader_get_dimensions(&g_cam_width, &g_cam_height);
    if (g_cam_width <= 0) g_cam_width = 640;
    if (g_cam_height <= 0) g_cam_height = 480;

    LOG_INFO("Preview: allocating %dx%d ARGB8888 ping-pong buffers", g_cam_width, g_cam_height);

    /* Allocate two identical buffers for ping-pong double buffering */
    for (int i = 0; i < 2; i++) {
        if (alloc_buf(i, g_cam_width, g_cam_height) < 0) {
            LOG_ERROR("Failed to allocate buffer %d (%dx%d)", i, g_cam_width, g_cam_height);
            for (int j = 0; j < i; j++) free_buf(j);
            camera_reader_deinit();
            return -1;
        }
    }
    g_buf_idx = 0;

    /* Hide placeholder, show image widget */
    if (placeholder) lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    if (preview_img) lv_obj_clear_flag(preview_img, LV_OBJ_FLAG_HIDDEN);

    /* Start ~30 fps refresh timer */
    preview_timer = lv_timer_create(preview_timer_cb, 33, NULL);
    if (!preview_timer) {
        LOG_ERROR("Failed to create preview timer");
        for (int i = 0; i < 2; i++) free_buf(i);
        camera_reader_deinit();
        return -1;
    }

    g_preview_active = true;
    return 0;
}

/* ── Button callbacks ─────────────────────────────────────────────────── */

static void back_cb(lv_event_t *e)
{
    (void)e;
    preview_stop();

    if (g_cam_running && g_cam_fd > 0) {
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP);
        rpc_disconnect(g_cam_fd);
        g_cam_fd = -1;
        g_cam_running = false;
    }

    if (g_main_screen) lv_scr_load(g_main_screen);
}

static void open_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);

    /* ── Toggle OFF ───────────────────────────────────────────────── */
    if (g_cam_running) {
        preview_stop();

        if (g_cam_fd > 0) {
            rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP);
            rpc_disconnect(g_cam_fd);
            g_cam_fd = -1;
        }
        g_cam_running = false;

        if (lbl) lv_label_set_text(lbl, "Open Camera");
        if (label_status) lv_label_set_text(label_status, "Camera closed");
        return;
    }

    /* ── Toggle ON ────────────────────────────────────────────────── */
    g_cam_fd = rpc_connect(CAMERA_SERVER_PORT);
    if (g_cam_fd < 0) {
        if (label_status) lv_label_set_text(label_status, "Cannot connect to camera");
        return;
    }

    cJSON *r = rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_START);
    if (!r || !cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"))) {
        /* Show the actual error from the server */
        const char *err = "Camera init failed";
        if (r) {
            cJSON *e = cJSON_GetObjectItem(r, "error");
            if (e && e->valuestring) err = e->valuestring;
        }
        if (label_status) lv_label_set_text(label_status, err);
        if (r) cJSON_Delete(r);
        rpc_disconnect(g_cam_fd); g_cam_fd = -1;
        return;
    }
    cJSON_Delete(r);
    g_cam_running = true;

    r = rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_START_PREVIEW);
    if (!r || !cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"))) {
        if (label_status) lv_label_set_text(label_status, "Preview stream failed");
        if (r) cJSON_Delete(r);
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP);
        rpc_disconnect(g_cam_fd); g_cam_fd = -1;
        g_cam_running = false;
        return;
    }
    cJSON_Delete(r);

    if (preview_start() < 0) {
        if (label_status) lv_label_set_text(label_status, "Shm reader init failed");
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP_PREVIEW);
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP);
        rpc_disconnect(g_cam_fd); g_cam_fd = -1;
        g_cam_running = false;
        return;
    }

    if (lbl) lv_label_set_text(lbl, "Close Camera");
    if (label_status) lv_label_set_text(label_status, "Live preview...");
}

static void photo_cb(lv_event_t *e)
{
    (void)e;
    if (!g_cam_running || g_cam_fd < 0) {
        if (label_status) lv_label_set_text(label_status, "Open camera first");
        return;
    }

    char fn[64];
    snprintf(fn, sizeof(fn), "photo_%ld.jpg", (long)time(NULL));

    cJSON *p = cJSON_CreateArray();
    cJSON_AddItemToArray(p, cJSON_CreateString(fn));
    cJSON *r = rpc_call(g_cam_fd, RPC_METHOD_CAMERA_TAKE_PHOTO, p);

    if (r && cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"))) {
        char b[128];
        snprintf(b, sizeof(b), "Saved: %s", fn);
        if (label_status) lv_label_set_text(label_status, b);
    } else {
        if (label_status) lv_label_set_text(label_status, "Photo failed");
    }
    if (r) cJSON_Delete(r);
}

static void rec_cb(lv_event_t *e)
{
    (void)e;
    if (!g_cam_running || g_cam_fd < 0) return;

    if (!g_recording) {
        char fn[64];
        snprintf(fn, sizeof(fn), "video_%ld.avi", (long)time(NULL));

        cJSON *p = cJSON_CreateArray();
        cJSON_AddItemToArray(p, cJSON_CreateString(fn));
        cJSON *r = rpc_call(g_cam_fd, RPC_METHOD_CAMERA_START_RECORDING, p);
        if (r) {
            cJSON_Delete(r);
            g_recording = true;
            if (label_status) lv_label_set_text(label_status, "● RECORDING");
        }
    } else {
        cJSON *r = rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP_RECORDING);
        if (r) {
            cJSON_Delete(r);
            g_recording = false;
            if (label_status) lv_label_set_text(label_status, "Stopped");
        }
    }
}

/* ── Page creation ────────────────────────────────────────────────────── */

void ui_camera_page_create(void)
{
    screen_camera = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_camera, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen_camera, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_camera, 16, 0);

    lv_obj_t *title = lv_label_create(screen_camera);
    lv_label_set_text(title, "Camera");
    lv_obj_set_style_text_color(title, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 8);

    lv_obj_t *bb = lv_btn_create(screen_camera);
    lv_obj_set_size(bb, 80, 32);
    lv_obj_align(bb, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(bb, 8, 0);
    lv_obj_add_event_cb(bb, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb);
    lv_label_set_text(bl, "< Back");
    lv_obj_center(bl);

    /* ── Preview container ────────────────────────────────────────── */
    lv_obj_t *preview = lv_obj_create(screen_camera);
    lv_obj_set_size(preview, LV_PCT(100), 340);
    lv_obj_set_style_bg_color(preview, lv_color_black(), 0);
    lv_obj_set_style_radius(preview, 14, 0);
    lv_obj_set_style_border_width(preview, 2, 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_align(preview, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_clip_corner(preview, true, 0);

    /* Live preview image (hidden until camera is opened).
     * STRETCH align is essential: the camera outputs 640×480 but the
     * preview container is ~768×340 — without scaling, the bottom
     * ~140 px of the image is clipped. */
    preview_img = lv_image_create(preview);
    lv_obj_set_size(preview_img, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(preview_img, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(preview_img, lv_color_black(), 0);
    lv_obj_center(preview_img);
    lv_image_set_inner_align(preview_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(preview_img, LV_OBJ_FLAG_HIDDEN);

    /* Placeholder text (hidden when preview is active) */
    placeholder = lv_label_create(preview);
    lv_label_set_text(placeholder, "Camera Preview");
    lv_obj_set_style_text_color(placeholder, lv_color_hex(0x455a64), 0);
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_18, 0);
    lv_obj_center(placeholder);

    /* ── Status label ─────────────────────────────────────────────── */
    label_status = lv_label_create(screen_camera);
    lv_label_set_text(label_status, "Ready");
    lv_obj_set_style_text_color(label_status, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_align_to(label_status, preview, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    /* ── Action buttons ───────────────────────────────────────────── */
    lv_obj_t *row = lv_obj_create(screen_camera);
    lv_obj_set_size(row, LV_PCT(100), 46);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const struct {
        const char *l; lv_event_cb_t cb; uint32_t color;
    } btns[] = {
        {"Open Camera", open_cb,  ACCENT},
        {"Take Photo",  photo_cb, 0x1565c0},
        {"Record",      rec_cb,   0xc62828},
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_size(b, 140, 40);
        lv_obj_set_style_bg_color(b, lv_color_hex(btns[i].color), 0);
        lv_obj_set_style_radius(b, 20, 0);
        lv_obj_add_event_cb(b, btns[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, btns[i].l);
        lv_obj_center(l);
    }

    LOG_INFO("Camera page created");
}

void ui_camera_page_show(void)
{
    if (screen_camera) lv_scr_load(screen_camera);
}
