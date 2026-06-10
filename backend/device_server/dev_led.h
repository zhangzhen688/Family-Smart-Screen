/**
 * @file dev_led.h
 * LED GPIO driver interface.
 */
#ifndef DEV_LED_H
#define DEV_LED_H

#include "common.h"

/**
 * Initialize LED subsystem.
 * On ARM: opens /dev/led_X device nodes.
 * On simulator: just prints init message.
 * @return 0 on success, negative on error.
 */
int led_init(void);

/**
 * Set LED on/off.
 * @param index LED index (0..LED_COUNT-1)
 * @param on    1=on, 0=off
 * @return 0 on success, negative on error.
 */
int led_set(int index, int on);

/**
 * Get LED state.
 * @param index LED index (0..LED_COUNT-1)
 * @param on    Output: 1=on, 0=off
 * @return 0 on success, negative on error.
 */
int led_get(int index, int *on);

/**
 * Cleanup LED subsystem.
 */
void led_exit(void);

#endif /* DEV_LED_H */
