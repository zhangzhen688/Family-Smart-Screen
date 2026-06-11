/**
 * @file ui_camera.c — Camera page with live preview via shared memory.
 *
 * Control flow (JSON-RPC over TCP):   camera_start / camera_stop / …
 * Data flow  (POSIX shared memory):   ring buffer → JPEG decode →
 *   bilinear scale to exact preview size → LVGL display (no LVGL scaling).
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
static lv_obj_t  *preview_img    = NULL;
static lv_obj_t  *placeholder    = NULL;
static lv_obj_t  *preview_ctnr   = NULL;     /* preview container (for size) */

/*
 * Ping-pong display buffers sized to the preview container (e.g. 768×340).
 * JPEG is first decoded at camera resolution (640×480) into g_decode_buf,
 * then bilinear-scaled to one of these display buffers.  LVGL renders the
 * other.  This eliminates all LVGL-internal scaling — we always feed LVGL
 * a buffer that exactly matches the widget size.
 */
static struct {
    lv_image_dsc_t  dsc;
    uint8_t        *rgb;
    int             w, h, stride;
} g_buf[2];
static int       g_buf_idx    = 0;
static uint8_t  *g_decode_buf = NULL;  /* temp: camera-res decode (640×480)   */
static int       g_cam_w = 640, g_cam_h = 480;
static int       g_view_w = 0, g_view_h = 0;  /* preview container pixel size */

static lv_timer_t *preview_timer = NULL;
static bool        g_cam_running = false, g_preview_active = false;
static bool        g_recording   = false;
static int         g_cam_fd      = -1;

extern lv_obj_t *g_main_screen;

/* ── Buffer helpers ───────────────────────────────────────────────────── */

static void free_buf(int i)
{
    if (g_buf[i].rgb) { free(g_buf[i].rgb); g_buf[i].rgb = NULL; }
    memset(&g_buf[i].dsc, 0, sizeof(g_buf[i].dsc));
    g_buf[i].w = g_buf[i].h = g_buf[i].stride = 0;
}

static int alloc_buf(int i, int w, int h)
{
    free_buf(i);
    int stride = w * 4;
    size_t sz  = (size_t)stride * (size_t)h;
    uint8_t *rgb = malloc(sz);
    if (!rgb) return -1;
    memset(rgb, 0, sz);

    g_buf[i].dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_buf[i].dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    g_buf[i].dsc.header.w      = (uint16_t)w;
    g_buf[i].dsc.header.h      = (uint16_t)h;
    g_buf[i].dsc.header.stride = (uint16_t)stride;
    g_buf[i].dsc.data_size     = (uint32_t)sz;
    g_buf[i].dsc.data          = rgb;
    g_buf[i].rgb    = rgb;
    g_buf[i].w      = w;
    g_buf[i].h      = h;
    g_buf[i].stride = stride;
    return 0;
}

/* ── YUYV → BGRA converter ───────────────────────────────────────────── */

static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void yuyv_to_bgra(const uint8_t *yuyv, uint8_t *bgra,
                         int w, int h, int stride)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *src = yuyv + y * w * 2;
        uint8_t       *dst = bgra + y * stride;
        for (int x = 0; x < w; x += 2) {
            int Y0 = src[0], U = src[1] - 128, Y1 = src[2], V = src[3] - 128;
            dst[0] = (uint8_t)clamp8(Y0 + ((454 * U) >> 8));
            dst[1] = (uint8_t)clamp8(Y0 - (( 88 * U + 183 * V) >> 8));
            dst[2] = (uint8_t)clamp8(Y0 + ((359 * V) >> 8));
            dst[3] = 255;
            dst[4] = (uint8_t)clamp8(Y1 + ((454 * U) >> 8));
            dst[5] = (uint8_t)clamp8(Y1 - (( 88 * U + 183 * V) >> 8));
            dst[6] = (uint8_t)clamp8(Y1 + ((359 * V) >> 8));
            dst[7] = 255;
            src += 4; dst += 8;
        }
    }
}

/* ── Bilinear scale BGRA → BGRA ──────────────────────────────────────── */

static void scale_bilinear(const uint8_t *src, int sw, int sh, int sstride,
                           uint8_t *dst, int dw, int dh, int dstride)
{
    /*
     * For each destination pixel (dx,dy), map back to source coordinates
     * (sx,sy) using fixed-point arithmetic, then blend the 4 nearest
     * source pixels.
     */
    for (int dy = 0; dy < dh; dy++) {
        /* source y with 16-bit fractional part — use 64-bit to avoid overflow */
        int sy_fp = (int)(((int64_t)dy * sh * 65536) / dh);
        int sy    = sy_fp >> 16;            /* integer part */
        int fy    = sy_fp & 0xFFFF;         /* fractional [0,65535] */
        int sy2   = (sy + 1 < sh) ? sy + 1 : sy;

        uint8_t *drow = dst + dy * dstride;

        for (int dx = 0; dx < dw; dx++) {
            int sx_fp = (int)(((int64_t)dx * sw * 65536) / dw);
            int sx    = sx_fp >> 16;
            int fx    = sx_fp & 0xFFFF;
            int sx2   = (sx + 1 < sw) ? sx + 1 : sx;

            /* 4 source pixels */
            const uint8_t *p00 = src + sy  * sstride + sx  * 4;
            const uint8_t *p01 = src + sy  * sstride + sx2 * 4;
            const uint8_t *p10 = src + sy2 * sstride + sx  * 4;
            const uint8_t *p11 = src + sy2 * sstride + sx2 * 4;

            uint8_t *out = drow + dx * 4;

            /* Blend each channel — 16-bit fixed-point weights */
            for (int c = 0; c < 4; c++) {
                int v00 = p00[c], v01 = p01[c], v10 = p10[c], v11 = p11[c];
                /* top row blend */
                int top = ((v00 * (65536 - fx)) + (v01 * fx)) >> 16;
                /* bottom row blend */
                int bot = ((v10 * (65536 - fx)) + (v11 * fx)) >> 16;
                /* vertical blend */
                out[c] = (uint8_t)(((top * (65536 - fy)) + (bot * fy)) >> 16);
            }
        }
    }
}

/* ── JPEG decoder (silent, validates completeness) ────────────────────── */

struct jpeg_err_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    int     fatal;
};

static void silent_emit_message(j_common_ptr cinfo, int level)
{ (void)cinfo; (void)level; }

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_err_mgr *err = (struct jpeg_err_mgr *)cinfo->err;
    err->fatal = 1;
    longjmp(err->jb, 1);
}

static inline int looks_like_jpeg(const uint8_t *data, size_t size)
{ return (size >= 2 && data[0] == 0xFF && data[1] == 0xD8); }

static int jpeg_to_rgb(const uint8_t *jpeg_data, size_t jpeg_size,
                       uint8_t *rgb_buf, int stride, int buf_w, int buf_h,
                       int *out_w, int *out_h)
{
    if (!looks_like_jpeg(jpeg_data, jpeg_size)) return -1;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_err_mgr jerr;

    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit   = jpeg_error_exit;
    jerr.pub.emit_message = silent_emit_message;
    jerr.fatal             = 0;

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
    *out_w = dw; *out_h = dh;

    if (dw > buf_w || dh > buf_h) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    memset(rgb_buf, 0, (size_t)buf_h * (size_t)stride);

    JSAMPROW row_ptr[1];
    JDIMENSION row;
    for (row = 0; row < cinfo.output_height; row++) {
        row_ptr[0] = rgb_buf + row * stride;
        jpeg_read_scanlines(&cinfo, row_ptr, 1);
    }

    int complete = jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    if (!complete || jerr.fatal) return -1;
    return 0;
}

/* ── Pixel format dispatch & frame display ────────────────────────────── */

#define FMT_YUYV  0x56595559
#define FMT_MJPEG 0x47504A4D
static uint32_t g_pixel_format = 0;

static int try_decode_and_display(void)
{
    uint8_t *frame_data;
    size_t   frame_size;

    if (camera_reader_read_frame(&frame_data, &frame_size) != 0)
        return -1;

    if (g_pixel_format == 0)
        g_pixel_format = camera_reader_get_pixel_format();

    int next = g_buf_idx ^ 1;

    if (g_pixel_format == FMT_YUYV) {
        /* YUYV path: convert directly, then scale to view size */
        size_t expected = (size_t)g_cam_w * (size_t)g_cam_h * 2;
        if (frame_size < expected) return -1;

        yuyv_to_bgra(frame_data, g_decode_buf, g_cam_w, g_cam_h, g_cam_w * 4);

    } else {
        /* MJPEG path: skip garbage frames, decode, validate completeness */
        for (int attempt = 0; attempt < 3; attempt++) {
            if (!looks_like_jpeg(frame_data, frame_size)) {
                if (camera_reader_read_frame(&frame_data, &frame_size) != 0)
                    return -1;
                continue;
            }
            int dw, dh;
            if (jpeg_to_rgb(frame_data, frame_size,
                            g_decode_buf, g_cam_w * 4,
                            g_cam_w, g_cam_h, &dw, &dh) == 0) {
                if (dw != g_cam_w || dh != g_cam_h) {
                    /* Camera changed resolution — realloc decode buf */
                    g_cam_w = dw; g_cam_h = dh;
                    free(g_decode_buf);
                    g_decode_buf = malloc((size_t)dw * (size_t)dh * 4);
                    if (!g_decode_buf) return -1;
                    /* Re-decode into new buffer */
                    if (jpeg_to_rgb(frame_data, frame_size,
                                    g_decode_buf, dw * 4,
                                    dw, dh, &dw, &dh) != 0)
                        return -1;
                }
                break;   /* success */
            }
            /* decode failed — try next frame */
            if (camera_reader_read_frame(&frame_data, &frame_size) != 0)
                return -1;
        }
    }

    /* Bilinear scale from camera resolution → preview container size */
    scale_bilinear(g_decode_buf, g_cam_w, g_cam_h, g_cam_w * 4,
                   g_buf[next].rgb, g_buf[next].w, g_buf[next].h,
                   g_buf[next].stride);

    /* Swap and push to LVGL */
    g_buf_idx = next;
    lv_image_set_src(preview_img, &g_buf[next].dsc);
    return 0;
}

static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_preview_active || !preview_img) return;
    if (!g_buf[0].rgb || !g_buf[1].rgb) return;
    if (!g_decode_buf) return;

    try_decode_and_display();
}

/* ── Preview start / stop ─────────────────────────────────────────────── */

static void preview_stop(void)
{
    if (preview_timer) { lv_timer_del(preview_timer); preview_timer = NULL; }
    g_preview_active = false;

    camera_reader_deinit();

    for (int i = 0; i < 2; i++) free_buf(i);
    g_buf_idx = 0;

    free(g_decode_buf); g_decode_buf = NULL;

    if (g_cam_fd > 0)
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP_PREVIEW);

    if (preview_img) {
        lv_image_set_src(preview_img, NULL);
        lv_obj_add_flag(preview_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (placeholder)
        lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
}

static int preview_start(void)
{
    if (camera_reader_init() < 0) return -1;

    camera_reader_get_dimensions(&g_cam_w, &g_cam_h);
    if (g_cam_w <= 0) g_cam_w = 640;
    if (g_cam_h <= 0) g_cam_h = 480;

    /* Get preview container content size in actual pixels */
    if (preview_ctnr) {
        g_view_w = lv_obj_get_content_width(preview_ctnr);
        g_view_h = lv_obj_get_content_height(preview_ctnr);
    }
    if (g_view_w <= 0) g_view_w = 768;
    if (g_view_h <= 0) g_view_h = 340;

    LOG_INFO("Preview: camera %dx%d → view %dx%d", g_cam_w, g_cam_h, g_view_w, g_view_h);

    /* Temp buffer for JPEG decode at camera resolution */
    g_decode_buf = malloc((size_t)g_cam_w * (size_t)g_cam_h * 4);
    if (!g_decode_buf) { camera_reader_deinit(); return -1; }

    /* Two display buffers at exact preview container size */
    for (int i = 0; i < 2; i++) {
        if (alloc_buf(i, g_view_w, g_view_h) < 0) {
            for (int j = 0; j < i; j++) free_buf(j);
            free(g_decode_buf); g_decode_buf = NULL;
            camera_reader_deinit();
            return -1;
        }
    }
    g_buf_idx = 0;

    if (placeholder) lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    if (preview_img) {
        /* Match image widget size to display buffer size exactly */
        lv_obj_set_size(preview_img, g_view_w, g_view_h);
        lv_obj_clear_flag(preview_img, LV_OBJ_FLAG_HIDDEN);
    }

    preview_timer = lv_timer_create(preview_timer_cb, 33, NULL);
    if (!preview_timer) {
        for (int i = 0; i < 2; i++) free_buf(i);
        free(g_decode_buf); g_decode_buf = NULL;
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
        g_cam_fd = -1; g_cam_running = false;
    }
    if (g_main_screen) lv_scr_load(g_main_screen);
}

static void open_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);

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

    g_cam_fd = rpc_connect(CAMERA_SERVER_PORT);
    if (g_cam_fd < 0) {
        if (label_status) lv_label_set_text(label_status, "Cannot connect to camera");
        return;
    }

    cJSON *r = rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_START);
    if (!r || !cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"))) {
        const char *err = "Camera init failed";
        if (r) { cJSON *e = cJSON_GetObjectItem(r, "error"); if (e && e->valuestring) err = e->valuestring; }
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
        rpc_disconnect(g_cam_fd); g_cam_fd = -1; g_cam_running = false;
        return;
    }
    cJSON_Delete(r);

    if (preview_start() < 0) {
        if (label_status) lv_label_set_text(label_status, "Preview init failed");
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP_PREVIEW);
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP);
        rpc_disconnect(g_cam_fd); g_cam_fd = -1; g_cam_running = false;
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
    char fn[64]; snprintf(fn, sizeof(fn), "photo_%ld.jpg", (long)time(NULL));
    cJSON *p = cJSON_CreateArray(); cJSON_AddItemToArray(p, cJSON_CreateString(fn));
    cJSON *r = rpc_call(g_cam_fd, RPC_METHOD_CAMERA_TAKE_PHOTO, p);
    if (r && cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"))) {
        char b[128]; snprintf(b, sizeof(b), "Saved: %s", fn);
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
        char fn[64]; snprintf(fn, sizeof(fn), "video_%ld.avi", (long)time(NULL));
        cJSON *p = cJSON_CreateArray(); cJSON_AddItemToArray(p, cJSON_CreateString(fn));
        cJSON *r = rpc_call(g_cam_fd, RPC_METHOD_CAMERA_START_RECORDING, p);
        if (r) { cJSON_Delete(r); g_recording = true;
            if (label_status) lv_label_set_text(label_status, "● RECORDING"); }
    } else {
        cJSON *r = rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP_RECORDING);
        if (r) { cJSON_Delete(r); g_recording = false;
            if (label_status) lv_label_set_text(label_status, "Stopped"); }
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
    lv_obj_set_size(bb, 80, 32); lv_obj_align(bb, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(bb, 8, 0);
    lv_obj_add_event_cb(bb, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb); lv_label_set_text(bl, "< Back"); lv_obj_center(bl);

    /* ── Preview container ────────────────────────────────────────── */
    preview_ctnr = lv_obj_create(screen_camera);
    lv_obj_set_size(preview_ctnr, LV_PCT(100), 340);
    lv_obj_set_style_bg_color(preview_ctnr, lv_color_black(), 0);
    lv_obj_set_style_radius(preview_ctnr, 14, 0);
    lv_obj_set_style_border_width(preview_ctnr, 2, 0);
    lv_obj_set_style_border_color(preview_ctnr, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_bg_opa(preview_ctnr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(preview_ctnr, 0, 0);
    lv_obj_align(preview_ctnr, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_clip_corner(preview_ctnr, true, 0);

    /* Image widget — sized to exact preview pixels in preview_start() */
    preview_img = lv_image_create(preview_ctnr);
    lv_obj_set_style_bg_opa(preview_img, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(preview_img, lv_color_black(), 0);
    lv_obj_add_flag(preview_img, LV_OBJ_FLAG_HIDDEN);

    placeholder = lv_label_create(preview_ctnr);
    lv_label_set_text(placeholder, "Camera Preview");
    lv_obj_set_style_text_color(placeholder, lv_color_hex(0x455a64), 0);
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_18, 0);
    lv_obj_center(placeholder);

    label_status = lv_label_create(screen_camera);
    lv_label_set_text(label_status, "Ready");
    lv_obj_set_style_text_color(label_status, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_align_to(label_status, preview_ctnr, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

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

    static const struct { const char *l; lv_event_cb_t cb; uint32_t color; } btns[] = {
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
        lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, btns[i].l); lv_obj_center(l);
    }

    LOG_INFO("Camera page created");
}

void ui_camera_page_show(void)
{ if (screen_camera) lv_scr_load(screen_camera); }
