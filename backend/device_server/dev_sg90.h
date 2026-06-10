/**
 * @file dev_sg90.h
 * SG90 servo motor driver interface (curtain control via PWM).
 */
#ifndef DEV_SG90_H
#define DEV_SG90_H

#include "common.h"

/**
 * Initialize SG90 servo subsystem.
 * @return 0 on success, negative on error.
 */
int sg90_init(void);

/**
 * Set servo angle.
 * @param angle Angle in degrees (0-180).
 *              0 = curtain closed, 180 = curtain fully open
 * @return 0 on success, negative on error.
 */
int sg90_set(int angle);

/**
 * Get current servo angle.
 * @param angle Output: current angle
 * @return 0 on success, negative on error.
 */
int sg90_get(int *angle);

/**
 * Cleanup SG90 subsystem.
 */
void sg90_exit(void);

#endif /* DEV_SG90_H */
