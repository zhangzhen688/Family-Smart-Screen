/**
 * @file dev_dht11.h
 * DHT11 temperature & humidity sensor driver interface.
 */
#ifndef DEV_DHT11_H
#define DEV_DHT11_H

#include "common.h"

/**
 * Initialize DHT11 subsystem.
 * @return 0 on success, negative on error.
 */
int dht11_init(void);

/**
 * Read temperature and humidity from DHT11.
 * @param humidity Output: humidity percentage (0-100)
 * @param temp     Output: temperature in Celsius (-20..+60)
 * @return 0 on success, negative on error.
 */
int dht11_read(int *humidity, int *temp);

/**
 * Cleanup DHT11 subsystem.
 */
void dht11_exit(void);

#endif /* DEV_DHT11_H */
