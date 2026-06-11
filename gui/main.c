/**
 * @file main.c
 * Smart Screen GUI — LVGL frontend entry point.
 *
 * SDL2 backend on Linux, FBDEV/DRM on ARM.
 * Communicates with backend servers via JSON-RPC over TCP.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include "lvgl.h"
#include "lv_conf.h"
#include "common.h"
#include "rpc_client.h"
#include "camera_reader.h"
#include "xiaozhi_ipc.h"

/* ── External UI page init functions ────────────────────────────────── */
extern void ui_main_page_create(void);
extern void ui_device_page_create(void);
extern void ui_camera_page_create(void);
extern void ui_album_page_create(void);
extern void ui_voice_page_create(void);
extern void ui_settings_page_create(void);
extern void ui_scenes_page_create(void);

/* ── Thread safety ─────────────────────────────────────────────────── */
static pthread_mutex_t lvgl_mutex = PTHREAD_MUTEX_INITIALIZER;

void lvgl_lock(void)   { pthread_mutex_lock(&lvgl_mutex); }
void lvgl_unlock(void) { pthread_mutex_unlock(&lvgl_mutex); }

/* ── Display init ───────────────────────────────────────────────────── */
static void lv_display_init(void)
{
#ifdef SIMULATOR_LINUX
    lv_sdl_window_create(800, 480);
    LOG_INFO("LVGL display: SDL2 800x480");
#else
#if LV_USE_LINUX_DRM
    lv_display_t *disp = lv_linux_drm_create();
    lv_linux_drm_set_file(disp, "/dev/dri/card0", -1);
#elif LV_USE_LINUX_FBDEV
    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, "/dev/fb0");
#endif
#endif
}

/* ── Mouse input ────────────────────────────────────────────────────── */
static void lv_input_init(void)
{
#ifdef SIMULATOR_LINUX
    lv_sdl_mouse_create();
#else
    lv_indev_t *indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
    if (!indev) indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event0");
#endif
}

/* ── Background sensor poll thread ──────────────────────────────────── */
static volatile int g_sensor_running = 1;
static int g_device_fd = -1;

int g_ui_humidity = 55;
int g_ui_temp = 26;
bool g_ui_sensor_valid = false;

static void *sensor_poll_thread(void *arg)
{
    (void)arg;
    g_device_fd = rpc_connect(DEVICE_SERVER_PORT);
    if (g_device_fd < 0) {
        LOG_ERROR("Sensor thread: cannot connect to device server");
        return NULL;
    }
    while (g_sensor_running) {
        cJSON *result = rpc_call_void(g_device_fd, RPC_METHOD_DHT11_READ);
        if (result) {
            cJSON *h = cJSON_GetObjectItem(result, "humidity");
            cJSON *t = cJSON_GetObjectItem(result, "temp");
            if (h && t) {
                lvgl_lock();
                g_ui_humidity = h->valueint;
                g_ui_temp     = t->valueint;
                g_ui_sensor_valid = true;
                lvgl_unlock();
            }
            cJSON_Delete(result);
        }
        sleep(2);
    }
    rpc_disconnect(g_device_fd);
    return NULL;
}

/* ── Main ──────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    LOG_INFO("====================================");
    LOG_INFO("Smart Screen GUI — Starting...");
    LOG_INFO("====================================");

    lv_init();

    /* Initialize filesystem for FreeType font loading and image assets */
    lv_fs_stdio_init();

    lv_display_init();
    lv_input_init();

    pthread_t sensor_thread;
    pthread_create(&sensor_thread, NULL, sensor_poll_thread, NULL);

    ui_main_page_create();
    ui_device_page_create();
    ui_camera_page_create();
    ui_album_page_create();
    ui_voice_page_create();
    ui_settings_page_create();
    ui_scenes_page_create();

    /* Start xiaozhi UDP IPC listener (port 5679 ← control_center) */
    xz_ipc_start();

    LOG_INFO("GUI ready.");

    while (1) {
        lvgl_lock();
        uint32_t delay_ms = lv_timer_handler();
        lvgl_unlock();
        usleep(delay_ms * 1000 > 5000 ? 5000 : delay_ms * 1000);
    }

    g_sensor_running = 0;
    pthread_join(sensor_thread, NULL);
    return 0;
}
