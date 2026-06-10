/**
 * @file ui_device.c — Device control page, dark tech theme.
 */
#include "ui_device.h"
#include "common.h"
#include "rpc_client.h"

#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define ACCENT       0x00d4ff
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf
#define SUCCESS      0x00e676

static lv_obj_t *screen_device = NULL;
static lv_obj_t *led_switches[LED_COUNT];
static lv_obj_t *led_labels[LED_COUNT];
static lv_obj_t *curtain_slider, *curtain_label, *label_sensor;
static int g_dev_fd = -1;

extern lv_obj_t *g_main_screen;

static int conn(void) {
    if (g_dev_fd < 0) g_dev_fd = rpc_connect(DEVICE_SERVER_PORT);
    return g_dev_fd;
}

static void led_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(sw);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (conn() < 0) return;
    cJSON *r = rpc_call_int2(g_dev_fd, RPC_METHOD_LED_SET, i, on ? 1 : 0);
    if (r) {
        lv_obj_set_style_text_color(led_labels[i],
            on ? lv_color_hex(SUCCESS) : lv_color_hex(TEXT_GRAY), 0);
        cJSON_Delete(r);
    }
}

static void curtain_cb(lv_event_t *e) {
    int a = lv_slider_get_value(lv_event_get_target(e));
    if (conn() < 0) return;
    cJSON *r = rpc_call_int1(g_dev_fd, RPC_METHOD_SG90_SET, a);
    if (r) { char b[32]; snprintf(b, sizeof(b), "%d deg (%d%%)", a, a*100/180);
        lv_label_set_text(curtain_label, b); cJSON_Delete(r); }
}

static void refresh_cb(lv_event_t *e) {
    if (conn() < 0) return;
    cJSON *r = rpc_call_void(g_dev_fd, RPC_METHOD_DHT11_READ);
    if (r) {
        cJSON *h = cJSON_GetObjectItem(r, "humidity");
        cJSON *t = cJSON_GetObjectItem(r, "temp");
        cJSON *ap = rpc_call_void(g_dev_fd, RPC_METHOD_AP3216C_READ);
        char b[128];
        if (h && t) {
            snprintf(b, sizeof(b), "Temp: %d C   Humidity: %d%%", t->valueint, h->valueint);
            if (ap) {
                cJSON *als = cJSON_GetObjectItem(ap, "als");
                if (als) { int l = strlen(b);
                    snprintf(b+l, sizeof(b)-l, "\nLight: %d lux", als->valueint); }
                cJSON_Delete(ap);
            }
            lv_label_set_text(label_sensor, b);
        }
        cJSON_Delete(r);
    }
}

static void back_cb(lv_event_t *e) { if (g_main_screen) lv_scr_load(g_main_screen); }

static void card_style(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 14, 0);
}

void ui_device_page_create(void)
{
    screen_device = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_device, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen_device, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_device, 16, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(screen_device);
    lv_label_set_text(title, "Device Control");
    lv_obj_set_style_text_color(title, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 8);

    /* Back */
    lv_obj_t *bb = lv_btn_create(screen_device);
    lv_obj_set_size(bb, 80, 32); lv_obj_align(bb, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(bb, 8, 0);
    lv_obj_add_event_cb(bb, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb); lv_label_set_text(bl, "< Back"); lv_obj_center(bl);

    /* ── Lights ──────────────────────────────────────────────────── */
    lv_obj_t *ls = lv_obj_create(screen_device);
    lv_obj_set_size(ls, LV_PCT(100), 240);
    card_style(ls);
    lv_obj_align(ls, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *lt = lv_label_create(ls);
    lv_label_set_text(lt, "Light Control");
    lv_obj_set_style_text_color(lt, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_font(lt, &lv_font_montserrat_18, 0);
    lv_obj_align(lt, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t *lg = lv_obj_create(ls);
    lv_obj_set_size(lg, LV_PCT(100), 195);
    lv_obj_set_style_bg_opa(lg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lg, 0, 0);
    lv_obj_set_style_pad_all(lg, 0, 0);
    lv_obj_align(lg, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_flex_flow(lg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(lg, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const char *rooms[] = {
        "Living Room","Bedroom","Kitchen","Study","Hallway","Bathroom"};
    for (int i = 0; i < LED_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(lg);
        lv_obj_set_size(row, 220, 52);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0f2440), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 12, 0);

        led_labels[i] = lv_label_create(row);
        lv_label_set_text(led_labels[i], rooms[i]);
        lv_obj_set_style_text_color(led_labels[i], lv_color_hex(TEXT_GRAY), 0);
        lv_obj_set_style_text_font(led_labels[i], &lv_font_montserrat_14, 0);

        led_switches[i] = lv_switch_create(row);
        lv_obj_set_user_data(led_switches[i], (void *)(intptr_t)i);
        lv_obj_add_event_cb(led_switches[i], led_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* ── Curtain ─────────────────────────────────────────────────── */
    lv_obj_t *cs = lv_obj_create(screen_device);
    lv_obj_set_size(cs, LV_PCT(100), 85);
    card_style(cs);
    lv_obj_align(cs, LV_ALIGN_TOP_MID, 0, 302);

    lv_obj_t *ct = lv_label_create(cs);
    lv_label_set_text(ct, "Curtain Control");
    lv_obj_set_style_text_color(ct, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_font(ct, &lv_font_montserrat_18, 0);
    lv_obj_align(ct, LV_ALIGN_TOP_LEFT, 0, 2);

    curtain_label = lv_label_create(cs);
    lv_label_set_text(curtain_label, "90 deg (50%)");
    lv_obj_set_style_text_color(curtain_label, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_align(curtain_label, LV_ALIGN_TOP_RIGHT, -4, 2);

    curtain_slider = lv_slider_create(cs);
    lv_obj_set_size(curtain_slider, LV_PCT(96), 10);
    lv_slider_set_range(curtain_slider, 0, 180);
    lv_slider_set_value(curtain_slider, 90, LV_ANIM_OFF);
    lv_obj_align(curtain_slider, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(curtain_slider, curtain_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Sensor ──────────────────────────────────────────────────── */
    label_sensor = lv_label_create(screen_device);
    lv_label_set_text(label_sensor, "Press Refresh to read sensors");
    lv_obj_set_style_text_color(label_sensor, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_set_style_text_font(label_sensor, &lv_font_montserrat_16, 0);
    lv_obj_align(label_sensor, LV_ALIGN_TOP_MID, 0, 400);

    lv_obj_t *rb = lv_btn_create(screen_device);
    lv_obj_set_size(rb, 170, 38);
    lv_obj_set_style_bg_color(rb, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_radius(rb, 19, 0);
    lv_obj_align(rb, LV_ALIGN_TOP_MID, 0, 430);
    lv_obj_add_event_cb(rb, refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rb); lv_label_set_text(rl, "Refresh Sensors"); lv_obj_center(rl);

    LOG_INFO("Device page created");
}

void ui_device_page_show(void) { if (screen_device) lv_scr_load(screen_device); }
