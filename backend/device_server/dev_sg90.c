/**
 * @file dev_sg90.c
 * SG90 servo PWM driver — stub on Linux, real PWM sysfs on ARM.
 */
#include "dev_sg90.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

static int current_angle = 90; /* default: middle */

int sg90_init(void)
{
#ifdef SIMULATOR_LINUX
    LOG_INFO("SG90 servo initialized (simulator, angle=%d)", current_angle);
#else
    /* On ARM: expect /sys/class/pwm/pwmchip0/pwm0/ */
    LOG_INFO("SG90 servo initialized (target)");
#endif
    return 0;
}

int sg90_set(int angle)
{
    if (angle < 0 || angle > 180) {
        LOG_ERROR("sg90_set: invalid angle %d (must be 0-180)", angle);
        return -1;
    }

#ifdef SIMULATOR_LINUX
    LOG_STUB("sg90_set(angle=%d) — curtain position %d%%",
             angle, (angle * 100) / 180);
    current_angle = angle;
    return 0;
#else
    {
        /* SG90: 0° → 500us pulse, 90° → 1500us, 180° → 2500us
         * Period = 20ms = 20000000ns. duty = 500000 + angle*(2000000/180) ns */
        int duty_ns = 500000 + (int)(angle * 11111.1);
        int period_ns = 20000000;
        int fd;

        fd = open("/sys/class/pwm/pwmchip0/export", O_WRONLY);
        if (fd >= 0) {
            write(fd, "0", 1);
            close(fd);
        }

        fd = open("/sys/class/pwm/pwmchip0/pwm0/period", O_WRONLY);
        if (fd >= 0) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%d", period_ns);
            write(fd, buf, len);
            close(fd);
        }

        fd = open("/sys/class/pwm/pwmchip0/pwm0/duty_cycle", O_WRONLY);
        if (fd >= 0) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%d", duty_ns);
            write(fd, buf, len);
            close(fd);
        }

        fd = open("/sys/class/pwm/pwmchip0/pwm0/enable", O_WRONLY);
        if (fd >= 0) {
            write(fd, "1", 1);
            close(fd);
        }

        current_angle = angle;
        return 0;
    }
#endif
}

int sg90_get(int *angle)
{
    if (!angle) return -1;
    *angle = current_angle;
    return 0;
}

void sg90_exit(void)
{
    sg90_set(90); /* return to middle */
    LOG_INFO("SG90 servo exited");
}
