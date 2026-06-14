/**
 * @file ui_album.c — Photo album, simple filename list.
 *
 * Scans ./photos directory, shows photos as a scrollable list.
 * BMP thumbnails shown via LVGL native decoder; JPEG files shown
 * as filename-only (JPEG decode is heavy for thumbnails).
 */
#include "lvgl.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define ACCENT       0x00d4ff
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf

static lv_obj_t *screen_album = NULL;
static lv_obj_t *list_box      = NULL;
extern lv_obj_t *g_main_screen;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static int has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    /* case-insensitive compare */
    size_t len1 = strlen(dot), len2 = strlen(ext);
    if (len1 != len2) return 0;
    for (size_t i = 0; i < len1; i++) {
        char a = dot[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* ── Back ────────────────────────────────────────────────────────────── */

static void back_cb(lv_event_t *e)
{
    (void)e;
    if (g_main_screen) lv_scr_load(g_main_screen);
}

/* ── Refresh ──────────────────────────────────────────────────────────── */

static void refresh_list(void)
{
    lv_obj_clean(list_box);

    DIR *dir = opendir(CAM_PHOTO_DIR);
    if (!dir) {
        lv_obj_t *msg = lv_label_create(list_box);
        lv_label_set_text(msg, "No photos directory.\n"
                               "Take a photo first.");
        lv_obj_set_style_text_color(msg, lv_color_hex(TEXT_GRAY), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    int count = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && count < 200) {
        if (!has_ext(e->d_name, ".jpg") &&
            !has_ext(e->d_name, ".bmp") &&
            !has_ext(e->d_name, ".jpeg"))
            continue;

        /* Create a row for this photo */
        lv_obj_t *row = lv_obj_create(list_box);
        lv_obj_set_size(row, LV_PCT(100), 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(CARD_COLOR), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_style_margin_bottom(row, 4, 0);

        /* Thumbnail (BMP only) or icon */
        lv_obj_t *icon;
        int is_bmp = has_ext(e->d_name, ".bmp");
        if (is_bmp) {
            icon = lv_image_create(row);
            char lv_path[512];
            snprintf(lv_path, sizeof(lv_path), "A:%s/%s", CAM_PHOTO_DIR, e->d_name);
            lv_image_set_src(icon, lv_path);
            lv_obj_set_size(icon, 36, 36);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        } else {
            icon = lv_label_create(row);
            lv_label_set_text(icon, "JPEG");
            lv_obj_set_style_text_color(icon, lv_color_hex(ACCENT), 0);
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
        }

        /* Filename */
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, e->d_name);
        lv_obj_set_style_text_color(label, lv_color_hex(TEXT_WHITE), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_align_to(label, icon, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

        count++;
    }
    closedir(dir);

    if (count == 0) {
        lv_obj_t *msg = lv_label_create(list_box);
        lv_label_set_text(msg, "No photos yet.\n"
                               "Use Camera to take photos.");
        lv_obj_set_style_text_color(msg, lv_color_hex(TEXT_GRAY), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
    }

    LOG_INFO("Album: %d photos loaded", count);
}

/* ── Page creation ────────────────────────────────────────────────────── */

void ui_album_page_create(void)
{
    screen_album = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_album, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen_album, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_album, 16, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(screen_album);
    lv_label_set_text(title, "Photo Album");
    lv_obj_set_style_text_color(title, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 8);

    /* Back */
    lv_obj_t *bb = lv_btn_create(screen_album);
    lv_obj_set_size(bb, 80, 32); lv_obj_align(bb, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(bb, 8, 0);
    lv_obj_add_event_cb(bb, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb); lv_label_set_text(bl, "< Back"); lv_obj_center(bl);

    /* Scrollable list box */
    list_box = lv_obj_create(screen_album);
    lv_obj_set_size(list_box, LV_PCT(100), 380);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 0, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);

    refresh_list();

    LOG_INFO("Album page created");
}

void ui_album_page_show(void)
{
    if (list_box) refresh_list();
    if (screen_album) lv_scr_load(screen_album);
}
