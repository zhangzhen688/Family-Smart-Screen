/**
 * @file ui_camera.c — Camera live preview.
 *
 * Pipeline: shm → JPEG validate → decode(cam res) → bilinear scale(view res)
 *           → lv_image_dsc_t XRGB8888 → LVGL display
 *
 * Key design decisions:
 *   - XRGB8888 (not ARGB8888): alpha=255 hardcoded, avoids premultiply issues
 *   - Exact-size buffers: image src matches widget pixel size → no LVGL scaling
 *   - Ping-pong buffers: two independent lv_image_dsc_t, alternated each frame
 *   - lv_obj_invalidate: force LVGL to re-read the image source each swap
 *   - All resolutions dynamic: camera from shm, view from LVGL widget
 */
#include "lvgl.h"
#include "common.h"
#include "rpc_client.h"
#include "camera_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <jpeglib.h>
#include <setjmp.h>

#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define ACCENT       0x00d4ff
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf

/* ── State ────────────────────────────────────────────────────────────── */

static lv_obj_t    *screen_camera = NULL, *label_status = NULL;
static lv_obj_t    *preview_img   = NULL;
static lv_obj_t    *placeholder   = NULL;
static lv_obj_t    *preview_ctnr  = NULL;

static int          g_cam_w  = 0, g_cam_h  = 0;   /* camera resolution       */
static int          g_view_w = 0, g_view_h = 0;   /* preview container size  */

/* Ping-pong image descriptors + pixel buffers */
static lv_image_dsc_t  g_dsc[2];
static uint8_t        *g_rgb[2];     /* malloc'd pixel data for each dsc    */
static int             g_cur = 0;    /* which dsc is currently displayed    */

static uint8_t        *g_decode_buf; /* temp: camera-resolution decode buf  */

static lv_timer_t     *preview_timer = NULL;
static bool             g_cam_running = false, g_preview_active = false;
static bool             g_recording   = false;
static int              g_cam_fd      = -1;

extern lv_obj_t *g_main_screen;

/* ── Buffer helpers ───────────────────────────────────────────────────── */

static void free_dsc(int i)
{
    if (g_rgb[i]) { free(g_rgb[i]); g_rgb[i] = NULL; }
    memset(&g_dsc[i], 0, sizeof(g_dsc[i]));
}

static int alloc_dsc(int i, int w, int h)
{
    free_dsc(i);
    int stride = w * 4;
    size_t sz  = (size_t)stride * (size_t)h;
    uint8_t *rgb = malloc(sz);
    if (!rgb) return -1;
    memset(rgb, 0, sz);

    g_dsc[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_dsc[i].header.cf     = LV_COLOR_FORMAT_XRGB8888;  /* no alpha channel */
    g_dsc[i].header.w      = (uint16_t)w;
    g_dsc[i].header.h      = (uint16_t)h;
    g_dsc[i].header.stride = (uint16_t)stride;
    g_dsc[i].data_size     = (uint32_t)sz;
    g_dsc[i].data          = rgb;
    g_rgb[i] = rgb;
    return 0;
}

/* ── Bilinear scale BGRA → BGRX ──────────────────────────────────────── */

static void scale_bilinear(const uint8_t *src, int sw, int sh, int sstride,
                           uint8_t *dst, int dw, int dh, int dstride)
{
    for (int dy = 0; dy < dh; dy++) {
        int sy_fp = (int)(((int64_t)dy * sh * 65536) / dh);
        int sy    = sy_fp >> 16, fy = sy_fp & 0xFFFF;
        int sy2   = (sy + 1 < sh) ? sy + 1 : sy;
        uint8_t *drow = dst + dy * dstride;

        for (int dx = 0; dx < dw; dx++) {
            int sx_fp = (int)(((int64_t)dx * sw * 65536) / dw);
            int sx    = sx_fp >> 16, fx = sx_fp & 0xFFFF;
            int sx2   = (sx + 1 < sw) ? sx + 1 : sx;

            const uint8_t *p00 = src + sy  * sstride + sx  * 4;
            const uint8_t *p01 = src + sy  * sstride + sx2 * 4;
            const uint8_t *p10 = src + sy2 * sstride + sx  * 4;
            const uint8_t *p11 = src + sy2 * sstride + sx2 * 4;
            uint8_t *out = drow + dx * 4;

            /* Blend B, G, R channels only; force A = 0xFF (XRGB) */
            for (int c = 0; c < 3; c++) {
                int t = (int)(((int64_t)p00[c] * (65536 - fx) + (int64_t)p01[c] * fx) >> 16);
                int b = (int)(((int64_t)p10[c] * (65536 - fx) + (int64_t)p11[c] * fx) >> 16);
                out[c] = (uint8_t)(((int64_t)t * (65536 - fy) + (int64_t)b * fy) >> 16);
            }
            out[3] = 0xFF;  /* alpha = fully opaque */
        }
    }
}

/* ── JPEG decoder (silent, validates completeness) ────────────────────── */

struct jpeg_err_mgr { struct jpeg_error_mgr pub; jmp_buf jb; int fatal; };
static void jpeg_silent(j_common_ptr c, int l) { (void)c; (void)l; }
static void jpeg_fatal(j_common_ptr c)
{ struct jpeg_err_mgr *e = (struct jpeg_err_mgr *)c->err; e->fatal = 1; longjmp(e->jb, 1); }

static inline int is_jpeg(const uint8_t *d, size_t s)
{ return s >= 2 && d[0] == 0xFF && d[1] == 0xD8; }

static int jpeg_decode(const uint8_t *data, size_t size,
                       uint8_t *rgb, int stride, int max_w, int max_h,
                       int *out_w, int *out_h)
{
    if (!is_jpeg(data, size)) return -1;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_err_mgr jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit   = jpeg_fatal;
    jerr.pub.emit_message = jpeg_silent;

    if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&cinfo); return -1; }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, size);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_BGRX;   /* B,G,R,0xFF → XRGB8888 */
    jpeg_start_decompress(&cinfo);

    int dw = (int)cinfo.output_width, dh = (int)cinfo.output_height;
    *out_w = dw; *out_h = dh;
    if (dw > max_w || dh > max_h) { jpeg_destroy_decompress(&cinfo); return -1; }

    memset(rgb, 0, (size_t)max_h * (size_t)stride);
    JSAMPROW rp[1]; JDIMENSION r;
    for (r = 0; r < cinfo.output_height; r++) {
        rp[0] = rgb + r * stride;
        jpeg_read_scanlines(&cinfo, rp, 1);
    }
    int ok = jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return (ok && !jerr.fatal) ? 0 : -1;
}

/* ── Frame pipeline ───────────────────────────────────────────────────── */

static int process_frame(void)
{
    uint8_t *data; size_t size;
    if (camera_reader_read_frame(&data, &size) != 0) return -1;

    /* Drain garbage frames, find a valid JPEG */
    for (int retry = 0; retry < 4; retry++) {
        if (!is_jpeg(data, size)) goto next;

        int dw, dh;
        if (jpeg_decode(data, size, g_decode_buf, g_cam_w * 4,
                        g_cam_w, g_cam_h, &dw, &dh) == 0) {
            if (dw != g_cam_w || dh != g_cam_h) {
                g_cam_w = dw; g_cam_h = dh;
                free(g_decode_buf);
                g_decode_buf = malloc((size_t)dw * (size_t)dh * 4);
                if (!g_decode_buf) return -1;
                if (jpeg_decode(data, size, g_decode_buf, dw * 4,
                                dw, dh, &dw, &dh) != 0) return -1;
            }
            goto decoded;
        }
next:
        if (camera_reader_read_frame(&data, &size) != 0) return -1;
    }
    return -1;

decoded:
    /* Scale camera → view size into the OTHER buffer */
    int next = g_cur ^ 1;
    scale_bilinear(g_decode_buf, g_cam_w, g_cam_h, g_cam_w * 4,
                   g_rgb[next], g_view_w, g_view_h, g_view_w * 4);

    /* Set the new source and invalidate to force LVGL to re-read */
    lv_image_set_src(preview_img, &g_dsc[next]);
    lv_obj_invalidate(preview_img);
    g_cur = next;
    return 0;
}

static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_preview_active) process_frame();
}

/* ── Start / stop ─────────────────────────────────────────────────────── */

static void preview_stop(void)
{
    if (preview_timer) { lv_timer_del(preview_timer); preview_timer = NULL; }
    g_preview_active = false;

    camera_reader_deinit();

    for (int i = 0; i < 2; i++) free_dsc(i);
    g_cur = 0;
    free(g_decode_buf); g_decode_buf = NULL;

    if (g_cam_fd > 0) rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP_PREVIEW);

    if (preview_img) {
        lv_image_set_src(preview_img, NULL);
        lv_obj_add_flag(preview_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (placeholder) lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
}

static int preview_start(void)
{
    if (camera_reader_init() < 0) return -1;

    camera_reader_get_dimensions(&g_cam_w, &g_cam_h);
    if (g_cam_w <= 0) g_cam_w = 640;
    if (g_cam_h <= 0) g_cam_h = 480;

    /* Read preview container content size at runtime */
    g_view_w = lv_obj_get_content_width(preview_ctnr);
    g_view_h = lv_obj_get_content_height(preview_ctnr);
    if (g_view_w <= 0) g_view_w = lv_obj_get_content_width(screen_camera) - 4;
    if (g_view_w <= 0) g_view_w = 764;
    if (g_view_h <= 0) g_view_h = 336;

    LOG_INFO("Preview: cam %dx%d → view %dx%d", g_cam_w, g_cam_h, g_view_w, g_view_h);

    /* Temp buffer for JPEG decode at camera resolution */
    g_decode_buf = malloc((size_t)g_cam_w * (size_t)g_cam_h * 4);
    if (!g_decode_buf) { camera_reader_deinit(); return -1; }

    /* Two image descriptors at exact view size */
    for (int i = 0; i < 2; i++) {
        if (alloc_dsc(i, g_view_w, g_view_h) < 0) {
            for (int j = 0; j < i; j++) free_dsc(j);
            free(g_decode_buf); g_decode_buf = NULL;
            camera_reader_deinit();
            return -1;
        }
    }
    g_cur = 0;

    /* Size the image widget to match our buffers exactly */
    lv_obj_set_size(preview_img, g_view_w, g_view_h);
    if (placeholder) lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    if (preview_img) lv_obj_clear_flag(preview_img, LV_OBJ_FLAG_HIDDEN);

    preview_timer = lv_timer_create(preview_timer_cb, 33, NULL);
    if (!preview_timer) {
        for (int i = 0; i < 2; i++) free_dsc(i);
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
        rpc_disconnect(g_cam_fd); g_cam_fd = -1; g_cam_running = false;
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
            rpc_disconnect(g_cam_fd); g_cam_fd = -1;
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
    cJSON_Delete(r); g_cam_running = true;

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
        if (label_status) lv_label_set_text(label_status, "Open camera first"); return;
    }
    char fn[64]; snprintf(fn, sizeof(fn), "photo_%ld.jpg", (long)time(NULL));
    cJSON *p = cJSON_CreateArray(); cJSON_AddItemToArray(p, cJSON_CreateString(fn));
    cJSON *r = rpc_call(g_cam_fd, RPC_METHOD_CAMERA_TAKE_PHOTO, p);
    if (r && cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"))) {
        char b[128]; snprintf(b, sizeof(b), "Saved: %s", fn);
        if (label_status) lv_label_set_text(label_status, b);
    } else { if (label_status) lv_label_set_text(label_status, "Photo failed"); }
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

    /* Preview container */
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

    /* Image widget */
    preview_img = lv_image_create(preview_ctnr);
    lv_obj_set_style_bg_opa(preview_img, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(preview_img, lv_color_black(), 0);
    lv_obj_add_flag(preview_img, LV_OBJ_FLAG_HIDDEN);

    /* Placeholder */
    placeholder = lv_label_create(preview_ctnr);
    lv_label_set_text(placeholder, "Camera Preview");
    lv_obj_set_style_text_color(placeholder, lv_color_hex(0x455a64), 0);
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_18, 0);
    lv_obj_center(placeholder);

    /* Status */
    label_status = lv_label_create(screen_camera);
    lv_label_set_text(label_status, "Ready");
    lv_obj_set_style_text_color(label_status, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_align_to(label_status, preview_ctnr, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    /* Buttons */
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
