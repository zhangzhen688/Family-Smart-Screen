/**
 * @file ui_album.c — Photo album with thumbnail grid.
 *
 * Scans ./photos directory, displays BMP/JPEG thumbnails in a
 * scrollable grid. BMP loaded via LVGL built-in decoder; JPEG
 * decoded manually via libjpeg.
 */
#include "lvgl.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <jpeglib.h>
#include <setjmp.h>

#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf
#define THUMB_SIZE   150
#define THUMB_PAD    6

static lv_obj_t *screen_album = NULL;
static lv_obj_t *album_grid   = NULL;
extern lv_obj_t *g_main_screen;

/* ── JPEG decoder (same pattern as camera) ───────────────────────────── */

struct jpeg_err_mgr { struct jpeg_error_mgr pub; jmp_buf jb; int fatal; };
static void jpeg_silent(j_common_ptr c, int l) { (void)c; (void)l; }
static void jpeg_fatal(j_common_ptr c)
{ struct jpeg_err_mgr *e = (struct jpeg_err_mgr *)c->err; e->fatal = 1; longjmp(e->jb, 1); }

static uint8_t *jpeg_load(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4*1024*1024) { fclose(f); return NULL; }
    uint8_t *data = malloc(sz);
    if (!data || fread(data, 1, sz, f) != (size_t)sz) { free(data); fclose(f); return NULL; }
    fclose(f);

    struct jpeg_decompress_struct cinfo;
    struct jpeg_err_mgr jerr;
    memset(&cinfo, 0, sizeof(cinfo)); memset(&jerr, 0, sizeof(jerr));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_fatal; jerr.pub.emit_message = jpeg_silent;
    if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&cinfo); free(data); return NULL; }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, sz);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_BGRX;
    jpeg_start_decompress(&cinfo);
    *w = cinfo.output_width; *h = cinfo.output_height;

    int stride = (*w) * 4;
    uint8_t *rgb = malloc((size_t)stride * (*h));
    if (!rgb) { jpeg_destroy_decompress(&cinfo); free(data); return NULL; }
    memset(rgb, 0, (size_t)stride * (*h));

    JSAMPROW rp[1]; JDIMENSION r;
    for (r = 0; r < cinfo.output_height; r++) {
        rp[0] = rgb + r * stride;
        jpeg_read_scanlines(&cinfo, rp, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(data);
    return rgb;
}

/* ── Nearest-neighbour scale to thumbnail ────────────────────────────── */

static void scale_thumb(const uint8_t *src, int sw, int sh,
                        uint8_t *dst, int dw, int dh)
{
    int sstride = sw * 4, dstride = dw * 4;
    for (int y = 0; y < dh; y++) {
        int sy = ((int64_t)y * sh) / dh;
        const uint8_t *sr = src + sy * sstride;
        uint8_t *dr = dst + y * dstride;
        for (int x = 0; x < dw; x++) {
            int sx = ((int64_t)x * sw) / dw;
            const uint8_t *sp = sr + sx * 4;
            uint8_t *dp = dr + x * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 0xFF;
        }
    }
}

/* ── Create a thumbnail card ─────────────────────────────────────────── */

static void add_thumbnail(const char *path, const char *name)
{
    lv_obj_t *card = lv_obj_create(album_grid);
    lv_obj_set_size(card, THUMB_SIZE + THUMB_PAD * 2,
                    THUMB_SIZE + THUMB_PAD * 2 + 20);
    lv_obj_set_style_bg_color(card, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, THUMB_PAD, 0);
    lv_obj_set_style_border_width(card, 0, 0);

    lv_obj_t *img = lv_image_create(card);
    const char *ext = strrchr(name, '.');
    int is_bmp = ext && strcasecmp(ext, ".bmp") == 0;

    if (is_bmp) {
        char lv_path[512];
        snprintf(lv_path, sizeof(lv_path), "A:%s", path);
        lv_image_set_src(img, lv_path);
        lv_obj_set_size(img, THUMB_SIZE, THUMB_SIZE);
    } else {
        /* JPEG — decode manually */
        int jw, jh;
        uint8_t *jrgb = jpeg_load(path, &jw, &jh);
        if (jrgb && jw > 0 && jh > 0) {
            int tw = THUMB_SIZE, th = THUMB_SIZE;
            if (jw * th > jh * tw) th = (tw * jh) / jw;
            else tw = (th * jw) / jh;
            if (tw < 1) tw = 1; if (th < 1) th = 1;

            uint8_t *thumb = malloc((size_t)tw * th * 4);
            if (thumb) {
                scale_thumb(jrgb, jw, jh, thumb, tw, th);
                lv_image_dsc_t *dsc = malloc(sizeof(*dsc));
                if (dsc) {
                    memset(dsc, 0, sizeof(*dsc));
                    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
                    dsc->header.cf     = LV_COLOR_FORMAT_XRGB8888;
                    dsc->header.w      = (uint16_t)tw;
                    dsc->header.h      = (uint16_t)th;
                    dsc->header.stride = (uint16_t)(tw * 4);
                    dsc->data_size     = (uint32_t)(tw * th * 4);
                    dsc->data          = thumb;
                    lv_image_set_src(img, dsc);
                }
            }
            free(jrgb);
        }
        lv_obj_set_size(img, THUMB_SIZE, THUMB_SIZE);
    }

    /* Filename */
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_color(label, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align_to(label, img, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
}

/* ── Refresh grid from photos directory ──────────────────────────────── */

static void refresh_grid(void)
{
    lv_obj_clean(album_grid);

    DIR *dir = opendir("./photos");
    if (!dir) {
        lv_obj_t *msg = lv_label_create(album_grid);
        lv_label_set_text(msg, "No photos yet.\nUse Camera to take photos.");
        lv_obj_set_style_text_color(msg, lv_color_hex(TEXT_GRAY), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    /* Collect filenames */
    char files[256][256];
    int count = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && count < 256) {
        const char *ext = strrchr(e->d_name, '.');
        if (ext && (strcasecmp(ext, ".jpg") == 0 ||
                    strcasecmp(ext, ".bmp") == 0 ||
                    strcasecmp(ext, ".jpeg") == 0)) {
            strncpy(files[count], e->d_name, 255);
            files[count][255] = '\0';
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        lv_obj_t *msg = lv_label_create(album_grid);
        lv_label_set_text(msg, "No photos yet.\nUse Camera to take photos.");
        lv_obj_set_style_text_color(msg, lv_color_hex(TEXT_GRAY), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    for (int i = 0; i < count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "./photos/%s", files[i]);
        add_thumbnail(path, files[i]);
    }
}

/* ── Back ────────────────────────────────────────────────────────────── */

static void back_cb(lv_event_t *e)
{
    (void)e;
    if (g_main_screen) lv_scr_load(g_main_screen);
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

    /* Scrollable grid */
    album_grid = lv_obj_create(screen_album);
    lv_obj_set_size(album_grid, LV_PCT(100), LV_PCT(100));
    lv_obj_align(album_grid, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_height(album_grid, 380);
    lv_obj_set_style_bg_opa(album_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(album_grid, 0, 0);
    lv_obj_set_style_pad_all(album_grid, 4, 0);
    lv_obj_set_flex_flow(album_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scrollbar_mode(album_grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(album_grid, LV_DIR_VER);

    refresh_grid();

    LOG_INFO("Album page created");
}

void ui_album_page_show(void)
{
    if (album_grid) refresh_grid();
    if (screen_album) lv_scr_load(screen_album);
}
