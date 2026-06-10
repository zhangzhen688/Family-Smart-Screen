/**
 * @file dev_ap3216c.h
 * AP3216C ambient light + proximity sensor driver interface (I2C).
 */
#ifndef DEV_AP3216C_H
#define DEV_AP3216C_H

#include "common.h"

/**
 * AP3216C reading: ambient light (ALS), proximity (PS), infrared (IR).
 */
typedef struct {
    uint16_t als;   /* Ambient Light Sensor value (lux) */
    uint16_t ps;    /* Proximity Sensor value */
    uint16_t ir;    /* Infrared value */
} ap3216c_data_t;

/**
 * Initialize AP3216C subsystem.
 * @return 0 on success, negative on error.
 */
int ap3216c_init(void);

/**
 * Read all sensor values from AP3216C.
 * @param data Output: sensor readings
 * @return 0 on success, negative on error.
 */
int ap3216c_read(ap3216c_data_t *data);

/**
 * Cleanup AP3216C subsystem.
 */
void ap3216c_exit(void);

#endif /* DEV_AP3216C_H */
