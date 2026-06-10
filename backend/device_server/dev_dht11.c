/**
 * @file dev_dht11.c
 * DHT11 temperature & humidity sensor — stub on Linux, real on ARM.
 */
#include "dev_dht11.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

static int initialized = 0;
static int stub_humidity = 55;
static int stub_temp = 26;

int dht11_init(void)
{
#ifdef SIMULATOR_LINUX
    srand(time(NULL));
    LOG_INFO("DHT11 subsystem initialized (simulator)");
#else
    LOG_INFO("DHT11 subsystem initialized (target, /dev/dht11)");
#endif
    initialized = 1;
    return 0;
}

int dht11_read(int *humidity, int *temp)
{
    if (!humidity || !temp) return -1;

#ifdef SIMULATOR_LINUX
    /* Simulate slight variation */
    int delta_h = (rand() % 5) - 2;   /* -2 to +2 */
    int delta_t = (rand() % 3) - 1;   /* -1 to +1 */
    stub_humidity += delta_h;
    stub_temp    += delta_t;

    /* Clamp to realistic range */
    if (stub_humidity < 20)  stub_humidity = 20;
    if (stub_humidity > 95)  stub_humidity = 95;
    if (stub_temp < 10)      stub_temp = 10;
    if (stub_temp > 45)      stub_temp = 45;

    *humidity = stub_humidity;
    *temp     = stub_temp;
    LOG_STUB("dht11_read: humidity=%d%%, temp=%dC", *humidity, *temp);
    return 0;
#else
    {
        /* On real board: read /dev/dht11, returns 5 bytes:
         * data[0]=humidity_int, data[1]=humidity_dec,
         * data[2]=temp_int, data[3]=temp_dec, data[4]=checksum */
        unsigned char data[5] = {0};
        int fd = open("/dev/dht11", O_RDONLY);
        if (fd < 0) {
            LOG_ERROR("dht11_read: cannot open /dev/dht11");
            return -1;
        }
        if (read(fd, data, 5) != 5) {
            LOG_ERROR("dht11_read: read failed");
            close(fd);
            return -1;
        }
        close(fd);

        /* Verify checksum */
        if ((data[0] + data[1] + data[2] + data[3]) != data[4]) {
            LOG_ERROR("dht11_read: checksum error");
            return -1;
        }
        *humidity = data[0];
        *temp     = data[2];
        return 0;
    }
#endif
}

void dht11_exit(void)
{
    LOG_INFO("DHT11 subsystem exited");
    initialized = 0;
}
