/**
 * @file ui_scenes.c — Scene linkage: one-tap multi-device automation.
 * Scenes: Home, Away, Sleep, Movie, Party.
 */
#include "lvgl.h"
#include "common.h"
#include "rpc_client.h"

#define BG_COLOR     0x0a0e27
#define CARD_COLOR   0x16213e
#define ACCENT       0x00d4ff
#define TEXT_WHITE   0xe8eaed
#define TEXT_GRAY    0x8e9aaf
#define SUCCESS      0x00e676

static lv_obj_t *screen_scenes = NULL;
extern lv_obj_t *g_main_screen;

/* Scene definition */
typedef struct {
    const char *name;
    const char *desc;
    int leds[LED_COUNT];   /* 1=on, 0=off, -1=no change */
    int curtain;           /* 0-180, -1=no change */
} scene_t;

static const scene_t scenes[] = {
    {"Home",     "All lights on, curtain open",
     {1,1,1,1,1,1}, 180},
    {"Away",     "All off, curtain closed",
     {0,0,0,0,0,0}, 0},
    {"Sleep",    "Only bedroom dim, curtain closed",
     {0,1,0,0,0,0}, 0},
    {"Movie",    "Living room dim, curtain closed",
     {1,0,0,0,0,0}, 90},
    {"Party",    "All lights on, curtain open",
     {1,1,1,1,1,1}, 180},
};

static int scene_count = sizeof(scenes) / sizeof(scenes[0]);

static void back_cb(lv_event_t *e) { if (g_main_screen) lv_scr_load(g_main_screen); }

static void activate_scene(int idx)
{
    int fd = rpc_connect(DEVICE_SERVER_PORT);
    if (fd < 0) return;

    const scene_t *s = &scenes[idx];
    LOG_INFO("Activating scene: %s", s->name);

    /* Set LEDs */
    for (int i = 0; i < LED_COUNT; i++) {
        if (s->leds[i] >= 0) {
            cJSON *r = rpc_call_int2(fd, RPC_METHOD_LED_SET, i, s->leds[i]);
            if (r) cJSON_Delete(r);
        }
    }

    /* Set curtain */
    if (s->curtain >= 0) {
        cJSON *r = rpc_call_int1(fd, RPC_METHOD_SG90_SET, s->curtain);
        if (r) cJSON_Delete(r);
    }

    rpc_disconnect(fd);
}

static void scene_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    activate_scene(idx);
}

void ui_scenes_page_create(void)
{
    screen_scenes = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_scenes, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen_scenes, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_scenes, 16, 0);

    lv_obj_t *title = lv_label_create(screen_scenes);
    lv_label_set_text(title, "Scene Linkage");
    lv_obj_set_style_text_color(title, lv_color_hex(TEXT_WHITE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 8);

    lv_obj_t *bb = lv_btn_create(screen_scenes);
    lv_obj_set_size(bb, 80, 32); lv_obj_align(bb, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_style_bg_color(bb, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(bb, 8, 0);
    lv_obj_add_event_cb(bb, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb); lv_label_set_text(bl, "< Back"); lv_obj_center(bl);

    /* Scene cards */
    lv_obj_t *grid = lv_obj_create(screen_scenes);
    lv_obj_set_size(grid, LV_PCT(100), 400);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < scene_count; i++) {
        lv_obj_t *card = lv_btn_create(grid);
        lv_obj_set_size(card, LV_PCT(100), 70);
        lv_obj_set_style_bg_color(card, lv_color_hex(CARD_COLOR), 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_margin_bottom(card, 10, 0);
        lv_obj_set_user_data(card, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, scene_btn_cb, LV_EVENT_CLICKED, NULL);

        /* Name */
        lv_obj_t *nm = lv_label_create(card);
        lv_label_set_text(nm, scenes[i].name);
        lv_obj_set_style_text_color(nm, lv_color_hex(TEXT_WHITE), 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 2);

        /* Description */
        lv_obj_t *ds = lv_label_create(card);
        lv_label_set_text(ds, scenes[i].desc);
        lv_obj_set_style_text_color(ds, lv_color_hex(TEXT_GRAY), 0);
        lv_obj_set_style_text_font(ds, &lv_font_montserrat_14, 0);
        lv_obj_align(ds, LV_ALIGN_BOTTOM_LEFT, 0, -2);

        /* Activate indicator */
        lv_obj_t *ar = lv_label_create(card);
        lv_label_set_text(ar, ">");
        lv_obj_set_style_text_color(ar, lv_color_hex(ACCENT), 0);
        lv_obj_set_style_text_font(ar, &lv_font_montserrat_24, 0);
        lv_obj_align(ar, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    LOG_INFO("Scenes page created");
}

void ui_scenes_page_show(void) { if (screen_scenes) lv_scr_load(screen_scenes); }
