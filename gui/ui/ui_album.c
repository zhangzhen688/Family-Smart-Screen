/**
 * @file ui_album.c — Photo album, dark theme.
 */
#include "lvgl.h"
#include "common.h"

#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf

static lv_obj_t *screen_album = NULL;
extern lv_obj_t *g_main_screen;

static void back_cb(lv_event_t *e) { if (g_main_screen) lv_scr_load(g_main_screen); }

void ui_album_page_create(void)
{
    screen_album = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_album, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen_album, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_album, 16, 0);

    lv_obj_t *title = lv_label_create(screen_album);
    lv_label_set_text(title, "Photo Album");
    lv_obj_set_style_text_color(title, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 8);

    lv_obj_t *bb = lv_btn_create(screen_album);
    lv_obj_set_size(bb, 80, 32); lv_obj_align(bb, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(bb, 8, 0);
    lv_obj_add_event_cb(bb, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb); lv_label_set_text(bl, "< Back"); lv_obj_center(bl);

    /* Album grid placeholder */
    lv_obj_t *box = lv_obj_create(screen_album);
    lv_obj_set_size(box, LV_PCT(100), 380);
    lv_obj_set_style_bg_color(box, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x2a3a5c), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg,
        "Photos will appear here\n\n"
        "Use the Camera to take photos.\n"
        "TODO: thumbnail grid with swipe.");
    lv_obj_set_style_text_color(msg, lv_color_hex(TEXT_GRAY), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(msg);

    LOG_INFO("Album page created");
}

void ui_album_page_show(void) { if (screen_album) lv_scr_load(screen_album); }
