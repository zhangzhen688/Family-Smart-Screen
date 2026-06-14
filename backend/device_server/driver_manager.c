/**
 * @file driver_manager.c
 * @brief Batch lifecycle management for all hardware subsystems.
 *
 * Implements the hal_all_init() / hal_all_exit() entry points declared
 * in driver_hal.h. Application code can either use the batch functions
 * or call individual hal_xxx_init/exit pairs.
 */

#include "driver_hal.h"

/* Individual subsystem headers */
#include "dev_led.h"
#include "dev_dht11.h"
#include "dev_sg90.h"
#include "dev_ap3216c.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* =========================================================================
 * Internal state
 * ========================================================================= */

static int g_all_initialised = 0;
static int g_hal_initialised[HAL_LED_MAX] = {0};

/* ── PIR motion internals ─────────────────────────────────────────────── */
static int              g_motion_fd = -1;
static hal_motion_cb_t  g_motion_cb = NULL;

static void motion_sigio_handler(int sig)
{
    (void)sig;
    if (g_motion_cb) {
        char val;
        if (pread(g_motion_fd, &val, 1, 0) == 1) {
            g_motion_cb(val ? HAL_MOTION_DETECTED : HAL_MOTION_NONE);
        }
    }
}

/* ── Bluetooth internals ──────────────────────────────────────────────── */
static int g_bt_fd = -1;
#include <termios.h>

/* =========================================================================
 * Batch Lifecycle
 * ========================================================================= */

int hal_all_init(void)
{
    if (g_all_initialised) return 0;

    int ret = 0;

    if (hal_led_init()       < 0) ret = -1;
    if (hal_env_init()       < 0) ret = -1;
    if (hal_light_init()     < 0) ret = -1;
    if (hal_curtain_init()   < 0) ret = -1;
    if (hal_motion_init()    < 0) ret = -1;
    /* Touch and Bluetooth are optional — don't fail if absent */

    if (ret == 0) g_all_initialised = 1;
    LOG_INFO("HAL: all subsystems initialised (status=%d)", ret);
    return ret;
}

void hal_all_exit(void)
{
    hal_curtain_exit();
    hal_light_exit();
    hal_env_exit();
    hal_motion_exit();
    hal_bluetooth_exit();
    hal_led_exit();
    g_all_initialised = 0;
    LOG_INFO("HAL: all subsystems exited");
}

/* =========================================================================
 * LED
 * ========================================================================= */

int hal_led_init(void)          { return led_init(); }

int hal_led_set(hal_led_id_t id, int on)
{
    if (id < 0 || id >= HAL_LED_MAX) return -1;
    return led_set((int)id, on);
}

int hal_led_get(hal_led_id_t id, int *on)
{
    if (id < 0 || id >= HAL_LED_MAX || !on) return -1;
    return led_get((int)id, on);
}

int hal_led_toggle(hal_led_id_t id)
{
    int cur;
    if (hal_led_get(id, &cur) < 0) return -1;
    return hal_led_set(id, !cur);
}

int hal_led_all_off(void)
{
    for (int i = 0; i < HAL_LED_MAX; i++)
        hal_led_set((hal_led_id_t)i, 0);
    return 0;
}

void hal_led_exit(void)         { led_exit(); }

/* =========================================================================
 * Buzzer — reuses LED driver with different device node
 * ========================================================================= */

static int g_buzzer_fd = -1;

int hal_buzzer_init(void)
{
#ifdef TARGET_ARM
    g_buzzer_fd = open("/dev/beep", O_RDWR);
    if (g_buzzer_fd < 0) {
        LOG_ERROR("hal_buzzer: cannot open /dev/beep: %s", strerror(errno));
        return -1;
    }
#else
    LOG_STUB("hal_buzzer_init()");
#endif
    return 0;
}

int hal_buzzer_on(void)
{
#ifdef TARGET_ARM
    char val = 1;
    return (g_buzzer_fd >= 0 && write(g_buzzer_fd, &val, 1) == 1) ? 0 : -1;
#else
    LOG_STUB("hal_buzzer_on()");
    return 0;
#endif
}

int hal_buzzer_off(void)
{
#ifdef TARGET_ARM
    char val = 0;
    return (g_buzzer_fd >= 0 && write(g_buzzer_fd, &val, 1) == 1) ? 0 : -1;
#else
    LOG_STUB("hal_buzzer_off()");
    return 0;
#endif
}

void hal_buzzer_exit(void)
{
    if (g_buzzer_fd >= 0) { close(g_buzzer_fd); g_buzzer_fd = -1; }
}

/* =========================================================================
 * Relay — reuses LED driver
 * ========================================================================= */

static int g_relay_fd = -1;

int hal_relay_init(void)
{
#ifdef TARGET_ARM
    g_relay_fd = open("/dev/jdq", O_RDWR);
    if (g_relay_fd < 0) {
        LOG_ERROR("hal_relay: cannot open /dev/jdq: %s", strerror(errno));
        return -1;
    }
#else
    LOG_STUB("hal_relay_init()");
#endif
    return 0;
}

int hal_relay_set(int on)
{
#ifdef TARGET_ARM
    char val = on ? 1 : 0;
    return (g_relay_fd >= 0 && write(g_relay_fd, &val, 1) == 1) ? 0 : -1;
#else
    LOG_STUB("hal_relay_set(on=%d)", on);
    return 0;
#endif
}

int hal_relay_get(int *on)
{
#ifdef TARGET_ARM
    char val;
    if (g_relay_fd < 0 || !on) return -1;
    if (read(g_relay_fd, &val, 1) != 1) return -1;
    *on = val;
    return 0;
#else
    if (on) *on = 0;
    return 0;
#endif
}

void hal_relay_exit(void)
{
    if (g_relay_fd >= 0) { close(g_relay_fd); g_relay_fd = -1; }
}

/* =========================================================================
 * DHT11 — Environment
 * ========================================================================= */

int hal_env_init(void)              { return dht11_init(); }

int hal_env_read(hal_env_data_t *out)
{
    if (!out) return -1;
    int humidity = 0, temp = 0;
    if (dht11_read(&humidity, &temp) < 0) return -1;
    out->humidity    = humidity;
    out->temperature = temp;
    return 0;
}

int hal_env_read_temp(int *celsius)
{
    int hum;
    if (!celsius) return -1;
    return dht11_read(&hum, celsius);
}

int hal_env_read_humidity(int *percent)
{
    int tmp;
    if (!percent) return -1;
    return dht11_read(percent, &tmp);
}

void hal_env_exit(void)             { dht11_exit(); }

/* =========================================================================
 * AP3216C — Ambient Light
 * ========================================================================= */

int hal_light_init(void)            { return ap3216c_init(); }

int hal_light_read(hal_light_data_t *out)
{
    if (!out) return -1;
    ap3216c_data_t raw;
    if (ap3216c_read(&raw) < 0) return -1;
    out->ambient_light = raw.als;
    out->proximity     = raw.ps;
    out->infrared      = raw.ir;
    return 0;
}

int hal_light_read_lux(uint16_t *lux)
{
    ap3216c_data_t raw;
    if (!lux || ap3216c_read(&raw) < 0) return -1;
    *lux = raw.als;
    return 0;
}

void hal_light_exit(void)           { ap3216c_exit(); }

/* =========================================================================
 * SG90 — Curtain
 * ========================================================================= */

int hal_curtain_init(void)          { return sg90_init(); }

int hal_curtain_set(int angle)
{
    if (angle < 0)   angle = 0;
    if (angle > 180) angle = 180;
    return sg90_set(angle);
}

int hal_curtain_get(int *angle)     { return sg90_get(angle); }

int hal_curtain_set_pct(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return hal_curtain_set((pct * 180) / 100);
}

int hal_curtain_get_pct(int *pct)
{
    int angle;
    if (!pct || sg90_get(&angle) < 0) return -1;
    *pct = (angle * 100) / 180;
    return 0;
}

void hal_curtain_exit(void)         { sg90_exit(); }

/* =========================================================================
 * SR501 — PIR Motion
 * ========================================================================= */

int hal_motion_init(void)
{
#ifdef TARGET_ARM
    g_motion_fd = open("/dev/sr501", O_RDONLY);
    if (g_motion_fd < 0) {
        LOG_ERROR("hal_motion: cannot open /dev/sr501: %s", strerror(errno));
        return -1;
    }
    /* Register SIGIO handler for async notification */
    signal(SIGIO, motion_sigio_handler);
#else
    LOG_STUB("hal_motion_init() — simulator mode");
#endif
    return 0;
}

int hal_motion_poll(void)
{
#ifdef TARGET_ARM
    char val;
    if (g_motion_fd < 0) return -1;
    if (pread(g_motion_fd, &val, 1, 0) == 1)
        return val ? HAL_MOTION_DETECTED : HAL_MOTION_NONE;
    return -1;
#else
    /* Simulate: return no motion */
    return HAL_MOTION_NONE;
#endif
}

int hal_motion_register_callback(hal_motion_cb_t cb)
{
    g_motion_cb = cb;
#ifdef TARGET_ARM
    if (g_motion_fd < 0) return -1;
    /* Enable async I/O on the device */
    fcntl(g_motion_fd, F_SETOWN, getpid());
    int flags = fcntl(g_motion_fd, F_GETFL);
    if (cb)
        fcntl(g_motion_fd, F_SETFL, flags | O_ASYNC);
    else
        fcntl(g_motion_fd, F_SETFL, flags & ~O_ASYNC);
#endif
    return 0;
}

void hal_motion_exit(void)
{
    if (g_motion_fd >= 0) {
        /* Disable async I/O */
        int flags = fcntl(g_motion_fd, F_GETFL);
        fcntl(g_motion_fd, F_SETFL, flags & ~O_ASYNC);
        close(g_motion_fd);
        g_motion_fd = -1;
    }
    g_motion_cb = NULL;
}

/* =========================================================================
 * GT9147 — Touchscreen
 * ========================================================================= */

int hal_touch_poll(int *x, int *y)
{
#ifdef TARGET_ARM
    /*
     * GT9147 reports through Linux input subsystem (/dev/input/eventX).
     * For a quick poll we can read the raw device — but the proper way
     * is to use evdev (libinput).  This is a lightweight stub.
     *
     * Application-layer touch handling is done via LVGL's evdev driver
     * in gui/main.c.  This polling API is for simple button-press
     * detection outside of LVGL.
     */
    (void)x; (void)y;
    return 0;  /* not touched */
#else
    (void)x; (void)y;
    return 0;  /* SDL mouse handled by LVGL */
#endif
}

/* =========================================================================
 * HC06 — Bluetooth UART
 * ========================================================================= */

int hal_bluetooth_init(const char *tty_path)
{
#ifdef TARGET_ARM
    if (!tty_path) tty_path = "/dev/ttySAC1";

    g_bt_fd = open(tty_path, O_RDWR | O_NOCTTY);
    if (g_bt_fd < 0) {
        LOG_ERROR("hal_bluetooth: cannot open %s: %s", tty_path, strerror(errno));
        return -1;
    }

    struct termios opts;
    tcgetattr(g_bt_fd, &opts);
    cfsetispeed(&opts, B9600);
    cfsetospeed(&opts, B9600);
    opts.c_cflag &= ~PARENB;   /* no parity */
    opts.c_cflag &= ~CSTOPB;   /* 1 stop bit */
    opts.c_cflag &= ~CSIZE;
    opts.c_cflag |= CS8;       /* 8 data bits */
    opts.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); /* raw mode */
    tcsetattr(g_bt_fd, TCSANOW, &opts);
    LOG_INFO("hal_bluetooth: %s configured 9600-8N1", tty_path);
#else
    LOG_STUB("hal_bluetooth_init(%s) — simulator mode", tty_path ? tty_path : "/dev/ttySAC1");
#endif
    return 0;
}

int hal_bluetooth_send(const uint8_t *data, int len)
{
#ifdef TARGET_ARM
    if (g_bt_fd < 0 || !data || len <= 0) return -1;
    return (int)write(g_bt_fd, data, (size_t)len);
#else
    LOG_STUB("hal_bluetooth_send(len=%d)", len);
    return len;
#endif
}

int hal_bluetooth_recv(uint8_t *buf, int max_len, int timeout_ms)
{
#ifdef TARGET_ARM
    if (g_bt_fd < 0 || !buf || max_len <= 0) return -1;

    if (timeout_ms > 0) {
        fd_set set;
        struct timeval tv;
        FD_ZERO(&set);
        FD_SET(g_bt_fd, &set);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(g_bt_fd + 1, &set, NULL, NULL, &tv) <= 0)
            return 0;  /* timeout, no data */
    }

    return (int)read(g_bt_fd, buf, (size_t)max_len);
#else
    (void)buf; (void)max_len; (void)timeout_ms;
    return 0;
#endif
}

void hal_bluetooth_exit(void)
{
    if (g_bt_fd >= 0) { close(g_bt_fd); g_bt_fd = -1; }
}
