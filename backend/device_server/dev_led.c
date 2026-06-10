/**
 * @file dev_led.c
 * LED GPIO driver — stub on Linux simulator, real GPIO on ARM.
 */
#include "dev_led.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* Static state for all LEDs (used in both stub and real mode) */
static int led_state[LED_COUNT];
static int initialized = 0;

int led_init(void)
{
    int i;
    for (i = 0; i < LED_COUNT; i++) {
        led_state[i] = 0; /* all off */
    }

#ifdef SIMULATOR_LINUX
    LOG_INFO("LED subsystem initialized (simulator, %d LEDs)", LED_COUNT);
#else
    /* On real ARM board, LED driver creates /dev/led_0 through /dev/led_5
     * via the platform_driver in led_drv.ko. We don't keep them open;
     * we open/close per operation. */
    LOG_INFO("LED subsystem initialized (target, %d LEDs)", LED_COUNT);
#endif
    initialized = 1;
    return 0;
}

int led_set(int index, int on)
{
    if (index < 0 || index >= LED_COUNT) {
        LOG_ERROR("led_set: invalid index %d", index);
        return -1;
    }

#ifdef SIMULATOR_LINUX
    LOG_STUB("led_set(index=%d, on=%d)", index, on);
    led_state[index] = on;
    return 0;
#else
    {
        char dev_path[64];
        char val = on ? 0 : 1; /* active-low: 0=on, 1=off */
        int fd;

        snprintf(dev_path, sizeof(dev_path), "/dev/led_%d", index);
        fd = open(dev_path, O_RDWR);
        if (fd < 0) {
            LOG_ERROR("led_set: cannot open %s", dev_path);
            return -1;
        }
        if (write(fd, &val, 1) < 0) {
            LOG_ERROR("led_set: write failed to %s", dev_path);
            close(fd);
            return -1;
        }
        led_state[index] = on;
        close(fd);
        return 0;
    }
#endif
}

int led_get(int index, int *on)
{
    if (index < 0 || index >= LED_COUNT || !on) {
        return -1;
    }

#ifdef SIMULATOR_LINUX
    *on = led_state[index];
    return 0;
#else
    {
        char dev_path[64];
        char val;
        int fd;

        snprintf(dev_path, sizeof(dev_path), "/dev/led_%d", index);
        fd = open(dev_path, O_RDWR);
        if (fd < 0) {
            LOG_ERROR("led_get: cannot open %s", dev_path);
            return -1;
        }
        if (read(fd, &val, 1) < 0) {
            LOG_ERROR("led_get: read failed from %s", dev_path);
            close(fd);
            return -1;
        }
        /* active-low logic: 0=on, 1=off */
        *on = (val == 0) ? 1 : 0;
        close(fd);
        return 0;
    }
#endif
}

void led_exit(void)
{
    int i;
    /* Turn all LEDs off on exit */
    for (i = 0; i < LED_COUNT; i++) {
        led_set(i, 0);
    }
    LOG_INFO("LED subsystem exited");
    initialized = 0;
}
