/**
 * @file ui_main.c
 * Main home screen — clock, weather, sensor cards, shortcut buttons.
 * Dark tech theme with gradient accents.
 */
#include "ui_main.h"
#include "common.h"
#include <time.h>

lv_obj_t *ui_main_label_humidity = NULL;
lv_obj_t *ui_main_label_temp     = NULL;
lv_obj_t *ui_main_label_clock    = NULL;
lv_obj_t *g_main_screen = NULL;

extern void ui_device_page_show(void);
extern void ui_camera_page_show(void);
extern void ui_album_page_show(void);
extern void ui_voice_page_show(void);
extern void ui_settings_page_show(void);
extern void ui_scenes_page_show(void);
extern int g_ui_humidity, g_ui_temp;
extern bool g_ui_sensor_valid;

/* ── Design tokens ──────────────────────────────────────────────────── */
#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define ACCENT       0x00d4ff
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf
#define SUCCESS      0x00e676
#define WARNING      0xff9100

static lv_style_t style_card, style_card_gradient;

static void init_styles(void)
{
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(CARD_COLOR));
    lv_style_set_radius(&style_card, 14);
    lv_style_set_border_width(&style_card, 0);
    lv_style_set_pad_all(&style_card, 16);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);

    lv_style_init(&style_card_gradient);
    lv_style_set_bg_color(&style_card_gradient, lv_color_hex(CARD_COLOR));
    lv_style_set_radius(&style_card_gradient, 14);
    lv_style_set_border_width(&style_card_gradient, 2);
    lv_style_set_border_color(&style_card_gradient, lv_color_hex(ACCENT));
    lv_style_set_border_opa(&style_card_gradient, LV_OPA_50);
    lv_style_set_pad_all(&style_card_gradient, 16);
    lv_style_set_bg_opa(&style_card_gradient, LV_OPA_COVER);
}

static void make_card(lv_obj_t *obj) {
    lv_obj_add_style(obj, &style_card, 0);
}

static void make_accent_card(lv_obj_t *obj) {
    lv_obj_add_style(obj, &style_card_gradient, 0);
}

/* ── Clock ──────────────────────────────────────────────────────────── */
static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!ui_main_label_clock) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    lv_label_set_text(ui_main_label_clock, buf);
}

/* ── Sensor update ──────────────────────────────────────────────────── */
static void sensor_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_ui_sensor_valid) return;
    char buf[32];
    if (ui_main_label_humidity) {
        snprintf(buf, sizeof(buf), "%d%%", g_ui_humidity);
        lv_label_set_text(ui_main_label_humidity, buf);
    }
    if (ui_main_label_temp) {
        snprintf(buf, sizeof(buf), "%d°", g_ui_temp);
        lv_label_set_text(ui_main_label_temp, buf);
    }
}

/* ── Shortcuts ──────────────────────────────────────────────────────── */
static void btn_dev_cb(lv_event_t *e) { ui_device_page_show(); }
static void btn_cam_cb(lv_event_t *e) { ui_camera_page_show(); }
static void btn_alb_cb(lv_event_t *e) { ui_album_page_show(); }
static void btn_voc_cb(lv_event_t *e) { ui_voice_page_show(); }
static void btn_set_cb(lv_event_t *e) { ui_settings_page_show(); }
static void btn_scn_cb(lv_event_t *e) { ui_scenes_page_show(); }

/* ── Create ─────────────────────────────────────────────────────────── */
void ui_main_page_create(void)
{
    init_styles();

    g_main_screen = lv_obj_create(NULL);
    lv_scr_load(g_main_screen);
    lv_obj_set_style_bg_color(g_main_screen, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(g_main_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_main_screen, 16, 0);

    /* ── Top: time + weather ──────────────────────────────────────── */
    lv_obj_t *top_row = lv_obj_create(g_main_screen);
    lv_obj_set_size(top_row, LV_PCT(100), 80);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_row, 0, 0);
    lv_obj_set_style_pad_all(top_row, 0, 0);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Clock */
    ui_main_label_clock = lv_label_create(top_row);
    lv_label_set_text(ui_main_label_clock, "00:00");
    lv_obj_set_style_text_color(ui_main_label_clock, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(ui_main_label_clock, &lv_font_montserrat_36, 0);

    /* Date + weather placeholder */
    lv_obj_t *right_col = lv_obj_create(top_row);
    lv_obj_set_size(right_col, 180, 70);
    lv_obj_set_style_bg_opa(right_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_col, 0, 0);
    lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *date_lbl = lv_label_create(right_col);
    lv_label_set_text(date_lbl, "Smart Home");
    lv_obj_set_style_text_color(date_lbl, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_16, 0);

    lv_obj_t *weather_lbl = lv_label_create(right_col);
    lv_label_set_text(weather_lbl, "22° Sunny");
    lv_obj_set_style_text_color(weather_lbl, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_set_style_text_font(weather_lbl, &lv_font_montserrat_14, 0);

    /* ── Sensor cards row ─────────────────────────────────────────── */
    lv_obj_t *sensor_row = lv_obj_create(g_main_screen);
    lv_obj_set_size(sensor_row, LV_PCT(100), 90);
    lv_obj_set_style_bg_opa(sensor_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sensor_row, 0, 0);
    lv_obj_set_style_pad_all(sensor_row, 0, 0);
    lv_obj_set_flex_flow(sensor_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sensor_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(sensor_row, LV_ALIGN_TOP_MID, 0, 100);

    /* Temp card */
    lv_obj_t *card_t = lv_obj_create(sensor_row);
    lv_obj_set_size(card_t, 230, 80);
    make_accent_card(card_t);
    lv_obj_t *tl = lv_label_create(card_t);
    lv_label_set_text(tl, "Temperature");
    lv_obj_set_style_text_color(tl, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_14, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_main_label_temp = lv_label_create(card_t);
    lv_label_set_text(ui_main_label_temp, "--°");
    lv_obj_set_style_text_color(ui_main_label_temp, lv_color_hex(WARNING), 0);
    lv_obj_set_style_text_font(ui_main_label_temp, &lv_font_montserrat_28, 0);
    lv_obj_align(ui_main_label_temp, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Humidity card */
    lv_obj_t *card_h = lv_obj_create(sensor_row);
    lv_obj_set_size(card_h, 230, 80);
    make_accent_card(card_h);
    lv_obj_t *hl = lv_label_create(card_h);
    lv_label_set_text(hl, "Humidity");
    lv_obj_set_style_text_color(hl, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_14, 0);
    lv_obj_align(hl, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_main_label_humidity = lv_label_create(card_h);
    lv_label_set_text(ui_main_label_humidity, "--%");
    lv_obj_set_style_text_color(ui_main_label_humidity, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_font(ui_main_label_humidity, &lv_font_montserrat_28, 0);
    lv_obj_align(ui_main_label_humidity, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Status dot */
    lv_obj_t *dot = lv_obj_create(sensor_row);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_bg_color(dot, lv_color_hex(SUCCESS), 0);
    lv_obj_set_style_radius(dot, 6, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    /* ── Weather bar ──────────────────────────────────────────────── */
    lv_obj_t *wx = lv_obj_create(g_main_screen);
    lv_obj_set_size(wx, LV_PCT(100), 36);
    lv_obj_set_style_bg_opa(wx, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(wx, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(wx, 10, 0);
    lv_obj_set_style_border_width(wx, 0, 0);
    lv_obj_set_style_pad_hor(wx, 14, 0);
    lv_obj_align(wx, LV_ALIGN_TOP_MID, 0, 206);
    lv_obj_set_flex_flow(wx, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wx, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *wl = lv_label_create(wx);
    lv_label_set_text(wl, "Shenzhen  22°  Sunny");
    lv_obj_set_style_text_color(wl, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_set_style_text_font(wl, &lv_font_montserrat_14, 0);

    lv_obj_t *wr = lv_label_create(wx);
    lv_label_set_text(wr, "Humidity 65%");
    lv_obj_set_style_text_color(wr, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_font(wr, &lv_font_montserrat_14, 0);

    /* ── Shortcut buttons (3x2 grid) ──────────────────────────────── */
    lv_obj_t *btn_grid = lv_obj_create(g_main_screen);
    lv_obj_set_size(btn_grid, LV_PCT(100), 195);
    lv_obj_set_style_bg_opa(btn_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_grid, 0, 0);
    lv_obj_set_style_pad_all(btn_grid, 0, 0);
    lv_obj_align(btn_grid, LV_ALIGN_TOP_MID, 0, 252);
    lv_obj_set_flex_flow(btn_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(btn_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const struct {
        const char *icon; const char *label; lv_event_cb_t cb; uint32_t color;
    } btns[] = {
        {LV_SYMBOL_SETTINGS, "Devices",  btn_dev_cb, 0x00d4ff},
        {LV_SYMBOL_IMAGE,    "Camera",   btn_cam_cb, 0x7c4dff},
        {LV_SYMBOL_HOME,     "Scenes",   btn_scn_cb, 0xff9100},
        {LV_SYMBOL_AUDIO,    "Voice AI", btn_voc_cb, 0xe91e63},
        {LV_SYMBOL_LIST,     "Album",    btn_alb_cb, 0x00e676},
        {LV_SYMBOL_WIFI,     "Settings", btn_set_cb, 0x8e9aaf},
    };

    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_btn_create(btn_grid);
        lv_obj_set_size(btn, 112, 88);
        make_card(btn);
        lv_obj_add_event_cb(btn, btns[i].cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, btns[i].icon);
        lv_obj_set_style_text_color(icon, lv_color_hex(btns[i].color), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, btns[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(TEXT_WHITE), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    /* ── Footer status bar ─────────────────────────────────────────── */
    lv_obj_t *footer = lv_obj_create(g_main_screen);
    lv_obj_set_size(footer, LV_PCT(100), 28);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *fdot = lv_obj_create(footer);
    lv_obj_set_size(fdot, 8, 8);
    lv_obj_set_style_bg_color(fdot, lv_color_hex(SUCCESS), 0);
    lv_obj_set_style_radius(fdot, 4, 0);
    lv_obj_set_style_border_width(fdot, 0, 0);
    lv_obj_set_style_bg_opa(fdot, LV_OPA_COVER, 0);

    lv_obj_t *flbl = lv_label_create(footer);
    lv_label_set_text(flbl, "  System Online");
    lv_obj_set_style_text_color(flbl, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_set_style_text_font(flbl, &lv_font_montserrat_14, 0);

    /* ── Timers ───────────────────────────────────────────────────── */
    lv_timer_create(clock_timer_cb, 1000, NULL);
    lv_timer_create(sensor_timer_cb, 2000, NULL);

    LOG_INFO("Main page created");
}
