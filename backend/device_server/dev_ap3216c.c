/**
 * @file dev_ap3216c.c
 * AP3216C I2C sensor — stub on Linux, real I2C on ARM.
 */
#include "dev_ap3216c.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static int initialized = 0;

int ap3216c_init(void)
{
#ifdef SIMULATOR_LINUX
    srand(time(NULL));
    LOG_INFO("AP3216C sensor initialized (simulator)");
#else
    /* On ARM: /dev/ap3216c or /sys/bus/i2c/... */
    LOG_INFO("AP3216C sensor initialized (target)");
#endif
    initialized = 1;
    return 0;
}

int ap3216c_read(ap3216c_data_t *data)
{
    if (!data) return -1;

#ifdef SIMULATOR_LINUX
    /* Simulate reasonable indoor values */
    data->als = 100 + (rand() % 400);    /* 100-500 lux (indoor) */
    data->ps  = (rand() % 100);          /* proximity: 0=nothing near */
    data->ir  = 5 + (rand() % 20);       /* IR value */
    LOG_STUB("ap3216c_read: als=%u, ps=%u, ir=%u",
            data->als, data->ps, data->ir);
    return 0;
#else
    {
        /* Real: read via I2C /dev/ap3216c */
        int fd = open("/dev/ap3216c", O_RDONLY);
        if (fd < 0) {
            LOG_ERROR("ap3216c_read: cannot open /dev/ap3216c");
            return -1;
        }
        /* Device returns 6 bytes: als_hi, als_lo, ps_hi, ps_lo, ir_hi, ir_lo */
        unsigned char buf[6];
        if (read(fd, buf, 6) != 6) {
            LOG_ERROR("ap3216c_read: read failed");
            close(fd);
            return -1;
        }
        data->als = (buf[0] << 8) | buf[1];
        data->ps  = (buf[2] << 8) | buf[3];
        data->ir  = (buf[4] << 8) | buf[5];
        close(fd);
        return 0;
    }
#endif
}

void ap3216c_exit(void)
{
    LOG_INFO("AP3216C sensor exited");
    initialized = 0;
}
