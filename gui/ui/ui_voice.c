/**
 * @file ui_voice.c
 * XiaoZhi AI voice assistant page.
 *
 * Character emoji, state bar, chat text, activation code display.
 * Tap-to-talk. Receives state/text/emotion from control_center via UDP.
 * Uses FreeType + HarmonyOS font for Chinese text rendering.
 */
#include "lvgl.h"
#include "src/libs/freetype/lv_freetype.h"
#include "common.h"
#include "rpc_client.h"
#include "xiaozhi_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Font path ──────────────────────────────────────────────────────── */
#define FONT_PATH  "assets/HarmonyOS_Sans_SC_Regular.ttf"

/* ── Globals ────────────────────────────────────────────────────────── */
static lv_obj_t *screen_voice = NULL;
static lv_obj_t *label_state;
static lv_obj_t *img_emoji;
static lv_obj_t *label_chat;

/* Chinese fonts via FreeType */
static lv_font_t *g_font_chat  = NULL;  /* 26px for chat text */
static lv_font_t *g_font_state = NULL;  /* 20px for state bar */

/* Styles using the Chinese fonts */
static lv_style_t g_style_chat;
static lv_style_t g_style_state;

static char g_cur_state[64] = "Standby";

extern lv_obj_t *g_main_screen;
extern void lvgl_lock(void);
extern void lvgl_unlock(void);

/* ── Image assets ───────────────────────────────────────────────────── */
#define ASSETS_PATH  "A:assets/"

static const char *img_idle    = ASSETS_PATH "img_naughty.png";
static const char *img_listen  = ASSETS_PATH "img_naughty.png";
static const char *img_speak   = ASSETS_PATH "img_joke.png";
static const char *img_think   = ASSETS_PATH "img_think.png";
static const char *img_worry_s = ASSETS_PATH "img_worry.png";

/* ── UDP send to control_center (port 5678) ─────────────────────────── */
static int g_udp_fd = -1;

static void udp_send(const char *msg)
{
    if (g_udp_fd < 0) {
        g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_udp_fd < 0) return;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(5678);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        connect(g_udp_fd, (struct sockaddr *)&a, sizeof(a));
    }
    send(g_udp_fd, msg, strlen(msg), 0);
}

static void send_listen_start(void) {
    udp_send("{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}");
}
static void send_listen_stop(void) {
    udp_send("{\"type\":\"listen\",\"state\":\"stop\"}");
}

/* ── Font init ──────────────────────────────────────────────────────── */
static void init_chinese_fonts(void)
{
#if LV_USE_FREETYPE
    /* Already initialized globally in lv_fs_stdio_init() + lv_freetype
     * being linked. Just create the fonts. */
    lv_freetype_init(256);

    g_font_chat = lv_freetype_font_create(FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 26,
        LV_FREETYPE_FONT_STYLE_NORMAL);

    g_font_state = lv_freetype_font_create(FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 20,
        LV_FREETYPE_FONT_STYLE_NORMAL);

    if (!g_font_chat || !g_font_state) {
        LOG_ERROR("Failed to load Chinese font from %s", FONT_PATH);
        g_font_chat = NULL;
        g_font_state = NULL;
        return;
    }

    /* Create styles */
    lv_style_init(&g_style_chat);
    lv_style_set_text_font(&g_style_chat, g_font_chat);
    lv_style_set_text_align(&g_style_chat, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&g_style_state);
    lv_style_set_text_font(&g_style_state, g_font_state);
    lv_style_set_text_align(&g_style_state, LV_TEXT_ALIGN_CENTER);

    LOG_INFO("Chinese fonts loaded (chat=26px, state=20px)");
#endif
}

/* ── IPC callbacks (called from UDP recv thread) ────────────────────── */
static void on_state_cb(int state, const char *name)
{
    lvgl_lock();
    snprintf(g_cur_state, sizeof(g_cur_state), "%s", name);

    if (label_state) {
        /* Map device state to Chinese for display */
        static const char *zh[] = {
            "Unknown", "Starting...", "WiFi Config", "Standby",
            "Connecting...", "Listening...", "Speaking...",
            "Upgrading...", "Activation", "Error"
        };
        const char *s = (state >= 0 && state <= 9) ? zh[state] : name;
        lv_label_set_text(label_state, s);
    }

    /* Emoji by state */
    const char *emoji = img_idle;
    switch (state) {
    case 0: case 1: case 2: case 4: case 7:
        emoji = img_think; break;
    case 3: emoji = img_idle;   break;
    case 5: emoji = img_listen; break;
    case 6: emoji = img_speak;  break;
    case 8: emoji = img_think;  break;
    case 9: emoji = img_worry_s; break;
    }
    if (img_emoji) lv_image_set_src(img_emoji, emoji);

    lvgl_unlock();
}

static void on_text_cb(const char *text)
{
    lvgl_lock();
    if (label_chat) {
        if (strstr(text, "Active-Code") || strstr(text, "Active Code")) {
            char buf[512];
            snprintf(buf, sizeof(buf),
                "Activation Code\n\n%s\n\nVisit xiaozhi.me to activate", text);
            lv_label_set_text(label_chat, buf);
        } else {
            lv_label_set_text(label_chat, text);
        }
    }
    lvgl_unlock();
}

static void on_emotion_cb(const char *emotion)
{
    lvgl_lock();
    const char *path = img_idle;
    if (strstr(emotion, "happy") || strstr(emotion, "laughing") ||
        strstr(emotion, "funny") || strstr(emotion, "cool"))
        path = img_speak;
    else if (strstr(emotion, "thinking") || strstr(emotion, "confused") ||
             strstr(emotion, "surprised"))
        path = img_think;
    else if (strstr(emotion, "sad") || strstr(emotion, "angry") ||
             strstr(emotion, "crying"))
        path = img_worry_s;
    if (img_emoji) lv_image_set_src(img_emoji, path);
    lvgl_unlock();
}

/* ── Tap to talk ────────────────────────────────────────────────────── */
static void screen_tap_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (strstr(g_cur_state, "Standby") || strstr(g_cur_state, "Idle")) {
        send_listen_start();
        if (label_state) lv_label_set_text(label_state, "Listening...");
        if (img_emoji) lv_image_set_src(img_emoji, img_listen);
    } else {
        send_listen_stop();
        if (label_state) lv_label_set_text(label_state, "Standby");
        if (img_emoji) lv_image_set_src(img_emoji, img_idle);
    }
}

/* ── Back ───────────────────────────────────────────────────────────── */
static void btn_back_cb(lv_event_t *e)
{
    if (g_main_screen) lv_scr_load(g_main_screen);
}

/* ── Page creation ──────────────────────────────────────────────────── */
void ui_voice_page_create(void)
{
    /* Load Chinese fonts via FreeType */
    init_chinese_fonts();

    screen_voice = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_voice, lv_color_hex(0x0d0d1a), 0);
    lv_obj_set_style_bg_opa(screen_voice, LV_OPA_COVER, 0);

    /* ── Status bar ────────────────────────────────────────────────── */
    lv_obj_t *bar = lv_obj_create(screen_voice);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 42);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    /* WiFi icon */
    lv_obj_t *wifi = lv_label_create(bar);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi, lv_color_hex(0x66bb6a), 0);

    /* State text — use Chinese font if available, fallback to built-in */
    label_state = lv_label_create(bar);
    lv_label_set_text(label_state, "Standby");
    lv_obj_set_style_text_color(label_state, lv_color_white(), 0);
    if (g_font_state)
        lv_obj_add_style(label_state, &g_style_state, 0);
    else
        lv_obj_set_style_text_font(label_state, &lv_font_montserrat_16, 0);
    lv_obj_set_width(label_state, LV_PCT(60));
    lv_obj_set_style_text_align(label_state, LV_TEXT_ALIGN_CENTER, 0);

    /* Battery icon */
    lv_obj_t *bat = lv_label_create(bar);
    lv_label_set_text(bat, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(bat, lv_color_hex(0x66bb6a), 0);

    /* ── Emoji character ───────────────────────────────────────────── */
    img_emoji = lv_image_create(screen_voice);
    lv_image_set_src(img_emoji, img_idle);
    lv_obj_align(img_emoji, LV_ALIGN_CENTER, 0, -30);

    /* ── Chat text ─────────────────────────────────────────────────── */
    label_chat = lv_label_create(screen_voice);
    lv_obj_set_width(label_chat, LV_PCT(88));
    lv_obj_set_style_text_color(label_chat, lv_color_hex(0xe0e0e0), 0);
    lv_label_set_long_mode(label_chat, LV_LABEL_LONG_WRAP);
    if (g_font_chat)
        lv_obj_add_style(label_chat, &g_style_chat, 0);
    else
        lv_obj_set_style_text_font(label_chat, &lv_font_montserrat_18, 0);
    lv_label_set_text(label_chat,
        "Tap anywhere to talk\n\n"
        "Start xiaozhi control_center\n"
        "for AI voice experience.");
    lv_obj_align_to(label_chat, img_emoji, LV_ALIGN_OUT_BOTTOM_MID, 0, 15);

    /* ── Back button ───────────────────────────────────────────────── */
    lv_obj_t *btn_back = lv_btn_create(screen_voice);
    lv_obj_set_size(btn_back, 90, 32);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 10, 44);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_50, 0);
    lv_obj_add_event_cb(btn_back, btn_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn_back);
    lv_label_set_text(lbl, "< Back");
    lv_obj_center(lbl);

    /* ── Tap-to-talk ───────────────────────────────────────────────── */
    lv_obj_add_flag(screen_voice, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen_voice, screen_tap_cb, LV_EVENT_CLICKED, NULL);

    /* ── Register IPC callbacks ────────────────────────────────────── */
    xz_ipc_on_state(on_state_cb);
    xz_ipc_on_text(on_text_cb);
    xz_ipc_on_emotion(on_emotion_cb);

    LOG_INFO("Voice (XiaoZhi AI) page created");
}

void ui_voice_page_show(void)
{
    if (screen_voice) {
        lv_scr_load(screen_voice);
        xz_ipc_start();
    }
}
