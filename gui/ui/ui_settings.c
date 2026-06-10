/**
 * @file ui_settings.c — System settings page.
 * Brightness, volume, sleep timer, about info.
 */
#include "lvgl.h"
#include "common.h"

#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define ACCENT       0x00d4ff
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf

static lv_obj_t *screen_settings = NULL;
static lv_obj_t *brightness_slider, *brightness_label;
static lv_obj_t *volume_slider, *volume_label;
static lv_obj_t *sleep_sw;

extern lv_obj_t *g_main_screen;

static void back_cb(lv_event_t *e) { if (g_main_screen) lv_scr_load(g_main_screen); }

static void brightness_cb(lv_event_t *e) {
    int v = lv_slider_get_value(brightness_slider);
    char b[32]; snprintf(b, sizeof(b), "Brightness: %d%%", v);
    lv_label_set_text(brightness_label, b);
    /* TODO: set actual backlight via sysfs or RPC */
}

static void volume_cb(lv_event_t *e) {
    int v = lv_slider_get_value(volume_slider);
    char b[32]; snprintf(b, sizeof(b), "Volume: %d%%", v);
    lv_label_set_text(volume_label, b);
    /* TODO: set actual volume via ALSA or RPC */
}

void ui_settings_page_create(void)
{
    screen_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_settings, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_settings, 16, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(screen_settings);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 8);

    lv_obj_t *bb = lv_btn_create(screen_settings);
    lv_obj_set_size(bb, 80, 32); lv_obj_align(bb, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(bb, 8, 0);
    lv_obj_add_event_cb(bb, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb); lv_label_set_text(bl, "< Back"); lv_obj_center(bl);

    int y = 60;

    /* ── Brightness ────────────────────────────────────────────────── */
    lv_obj_t *bc = lv_obj_create(screen_settings);
    lv_obj_set_size(bc, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(bc, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(bc, 12, 0); lv_obj_set_style_border_width(bc, 0, 0);
    lv_obj_set_style_bg_opa(bc, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bc, 14, 0);
    lv_obj_align(bc, LV_ALIGN_TOP_MID, 0, y); y += 90;

    brightness_label = lv_label_create(bc);
    lv_label_set_text(brightness_label, "Brightness: 80%");
    lv_obj_set_style_text_color(brightness_label, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(brightness_label, &lv_font_montserrat_16, 0);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 0, 2);

    brightness_slider = lv_slider_create(bc);
    lv_obj_set_size(brightness_slider, LV_PCT(90), 8);
    lv_slider_set_range(brightness_slider, 10, 100);
    lv_slider_set_value(brightness_slider, 80, LV_ANIM_OFF);
    lv_obj_align(brightness_slider, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_event_cb(brightness_slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Volume ────────────────────────────────────────────────────── */
    lv_obj_t *vc = lv_obj_create(screen_settings);
    lv_obj_set_size(vc, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(vc, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(vc, 12, 0); lv_obj_set_style_border_width(vc, 0, 0);
    lv_obj_set_style_bg_opa(vc, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(vc, 14, 0);
    lv_obj_align(vc, LV_ALIGN_TOP_MID, 0, y); y += 90;

    volume_label = lv_label_create(vc);
    lv_label_set_text(volume_label, "Volume: 60%");
    lv_obj_set_style_text_color(volume_label, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(volume_label, &lv_font_montserrat_16, 0);
    lv_obj_align(volume_label, LV_ALIGN_TOP_LEFT, 0, 2);

    volume_slider = lv_slider_create(vc);
    lv_obj_set_size(volume_slider, LV_PCT(90), 8);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, 60, LV_ANIM_OFF);
    lv_obj_align(volume_slider, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_event_cb(volume_slider, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Sleep Timer ───────────────────────────────────────────────── */
    lv_obj_t *sc = lv_obj_create(screen_settings);
    lv_obj_set_size(sc, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(sc, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(sc, 12, 0); lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_set_style_bg_opa(sc, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(sc, 14, 0);
    lv_obj_align(sc, LV_ALIGN_TOP_MID, 0, y); y += 70;
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *sl = lv_label_create(sc);
    lv_label_set_text(sl, "Sleep Timer");
    lv_obj_set_style_text_color(sl, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);

    sleep_sw = lv_switch_create(sc);

    /* ── About ─────────────────────────────────────────────────────── */
    lv_obj_t *ac = lv_obj_create(screen_settings);
    lv_obj_set_size(ac, LV_PCT(100), 110);
    lv_obj_set_style_bg_color(ac, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(ac, 12, 0); lv_obj_set_style_border_width(ac, 0, 0);
    lv_obj_set_style_bg_opa(ac, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ac, 14, 0);
    lv_obj_align(ac, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t *at = lv_label_create(ac);
    lv_label_set_text(at, "About");
    lv_obj_set_style_text_color(at, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_font(at, &lv_font_montserrat_18, 0);
    lv_obj_align(at, LV_ALIGN_TOP_LEFT, 0, 2);

    lv_obj_t *ai = lv_label_create(ac);
    lv_label_set_text(ai, "Smart Screen v1.0\nFamily Smart Home System\nLVGL + XiaoZhi AI + V4L2\nRemote control: port 8080");
    lv_obj_set_style_text_color(ai, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_set_style_text_font(ai, &lv_font_montserrat_14, 0);
    lv_obj_align(ai, LV_ALIGN_TOP_LEFT, 0, 28);

    LOG_INFO("Settings page created");
}

void ui_settings_page_show(void) { if (screen_settings) lv_scr_load(screen_settings); }
