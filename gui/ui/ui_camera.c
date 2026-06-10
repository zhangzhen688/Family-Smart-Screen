/**
 * @file ui_camera.c — Camera page with dark theme styling.
 */
#include "lvgl.h"
#include "common.h"
#include "rpc_client.h"
#include <time.h>

#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define ACCENT       0x00d4ff
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf

static lv_obj_t *screen_camera = NULL, *label_status = NULL;
static int g_cam_fd = -1;
static bool g_cam_running = false, g_recording = false;
extern lv_obj_t *g_main_screen;

static void back_cb(lv_event_t *e) {
    if (g_cam_running && g_cam_fd > 0) {
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP);
        rpc_disconnect(g_cam_fd); g_cam_fd = -1; g_cam_running = false;
    }
    if (g_main_screen) lv_scr_load(g_main_screen);
}

static void open_cb(lv_event_t *e) {
    if (g_cam_running) {
        rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP);
        rpc_disconnect(g_cam_fd); g_cam_fd = -1; g_cam_running = false;
        lv_label_set_text(label_status, "Camera closed");
        lv_obj_t *lbl = lv_obj_get_child(lv_event_get_target(e), 0);
        if (lbl) lv_label_set_text(lbl, "Open Camera");
        return;
    }
    g_cam_fd = rpc_connect(CAMERA_SERVER_PORT);
    if (g_cam_fd < 0) { lv_label_set_text(label_status, "Cannot connect"); return; }
    cJSON *r = rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_START);
    if (r && cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"))) {
        g_cam_running = true;
        lv_label_set_text(label_status, "Camera running...");
        lv_obj_t *lbl = lv_obj_get_child(lv_event_get_target(e), 0);
        if (lbl) lv_label_set_text(lbl, "Close Camera");
    }
    if (r) cJSON_Delete(r);
}

static void photo_cb(lv_event_t *e) {
    if (!g_cam_running || g_cam_fd < 0) { lv_label_set_text(label_status, "Open camera first"); return; }
    char fn[64]; snprintf(fn, sizeof(fn), "photo_%ld.jpg", time(NULL));
    cJSON *p = cJSON_CreateArray(); cJSON_AddStringToObject(p, NULL, fn);
    cJSON *r = rpc_call(g_cam_fd, RPC_METHOD_CAMERA_TAKE_PHOTO, p);
    if (r && cJSON_IsTrue(cJSON_GetObjectItem(r, "ok"))) {
        char b[128]; snprintf(b, sizeof(b), "Saved: %s", fn); lv_label_set_text(label_status, b);
    }
    if (r) cJSON_Delete(r);
}

static void rec_cb(lv_event_t *e) {
    if (!g_cam_running || g_cam_fd < 0) return;
    if (!g_recording) {
        char fn[64]; snprintf(fn, sizeof(fn), "video_%ld.avi", time(NULL));
        cJSON *p = cJSON_CreateArray(); cJSON_AddStringToObject(p, NULL, fn);
        cJSON *r = rpc_call(g_cam_fd, RPC_METHOD_CAMERA_START_RECORDING, p);
        if (r) { cJSON_Delete(r); g_recording = true; lv_label_set_text(label_status, "* RECORDING"); }
    } else {
        cJSON *r = rpc_call_void(g_cam_fd, RPC_METHOD_CAMERA_STOP_RECORDING);
        if (r) { cJSON_Delete(r); g_recording = false; lv_label_set_text(label_status, "Stopped"); }
    }
}

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

    /* Preview box */
    lv_obj_t *preview = lv_obj_create(screen_camera);
    lv_obj_set_size(preview, LV_PCT(100), 340);
    lv_obj_set_style_bg_color(preview, lv_color_black(), 0);
    lv_obj_set_style_radius(preview, 14, 0);
    lv_obj_set_style_border_width(preview, 2, 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_align(preview, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_t *ph = lv_label_create(preview);
    lv_label_set_text(ph, "Camera Preview"); lv_obj_set_style_text_color(ph, lv_color_hex(0x455a64), 0);
    lv_obj_set_style_text_font(ph, &lv_font_montserrat_18, 0); lv_obj_center(ph);

    /* Status */
    label_status = lv_label_create(screen_camera);
    lv_label_set_text(label_status, "Ready");
    lv_obj_set_style_text_color(label_status, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_align_to(label_status, preview, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    /* Buttons */
    lv_obj_t *row = lv_obj_create(screen_camera);
    lv_obj_set_size(row, LV_PCT(100), 46);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const struct { const char *l; lv_event_cb_t cb; uint32_t color; } btns[] = {
        {"Open Camera", open_cb, ACCENT},
        {"Take Photo", photo_cb, 0x1565c0},
        {"Record", rec_cb, 0xc62828},
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

void ui_camera_page_show(void) { if (screen_camera) lv_scr_load(screen_camera); }
