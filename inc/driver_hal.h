/**
 * @file driver_hal.h
 * @brief Unified Hardware Abstraction Layer — single-include entry point.
 *
 * ## Design Philosophy
 *
 * Every hardware device exposes a standard lifecycle pattern:
 *   init() → read/write/control() → exit()
 *
 * All functions return 0 on success, negative on error. This consistency
 * lets application code chain calls and handle errors uniformly.
 *
 * ## Platform Switching
 *
 *   cmake -DSIMULATOR_LINUX=ON  → x86 stub (dev without hardware)
 *   cmake -DSIMULATOR_LINUX=OFF → Ascend 310B ARM (real drivers)
 *
 * ## Usage
 *
 *   #include "driver_hal.h"
 *
 *   int main() {
 *       hal_all_init();                    // batch-init every device
 *       hal_led_set(HAL_LED_KITCHEN, 1);  // turn kitchen light on
 *       hal_curtain_set(90);              // curtain to half-open
 *       hal_all_exit();                   // safe shutdown
 *   }
 */

#ifndef DRIVER_HAL_H
#define DRIVER_HAL_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Type Definitions
 * ========================================================================= */

/** Temperature + humidity reading. */
typedef struct {
    int temperature;   /**< Celsius, range -20..+60 */
    int humidity;      /**< Percentage, range 0..100 */
} hal_env_data_t;

/** Ambient light + proximity reading. */
typedef struct {
    uint16_t ambient_light;  /**< Ambient light in lux */
    uint16_t proximity;      /**< Proximity: 0=far, 1023=near */
    uint16_t infrared;       /**< Raw IR value */
} hal_light_data_t;

/** PIR motion sensor event. */
typedef enum {
    HAL_MOTION_NONE    = 0,
    HAL_MOTION_DETECTED = 1,
} hal_motion_t;

/** LED identifiers (semantic naming). */
typedef enum {
    HAL_LED_LIVING_ROOM = 0,
    HAL_LED_BEDROOM     = 1,
    HAL_LED_KITCHEN     = 2,
    HAL_LED_STUDY       = 3,
    HAL_LED_HALLWAY     = 4,
    HAL_LED_BATHROOM    = 5,
    HAL_LED_MAX         = 6,
} hal_led_id_t;

/* =========================================================================
 * Lifecycle — batch management
 * ========================================================================= */

/**
 * Initialise ALL hardware subsystems.
 * Safe to call multiple times (idempotent).
 * @return 0 on success, negative if any subsystem failed.
 */
int hal_all_init(void);

/**
 * Shut down ALL hardware subsystems safely
 * (turn off outputs, return servo to neutral, etc.).
 */
void hal_all_exit(void);

/* =========================================================================
 * LED / Output (GPIO)
 * ========================================================================= */

int hal_led_init(void);
int hal_led_set(hal_led_id_t id, int on);   /**< 1=on, 0=off */
int hal_led_get(hal_led_id_t id, int *on);
int hal_led_toggle(hal_led_id_t id);        /**< flip current state */
int hal_led_all_off(void);                  /**< convenience: all off */
void hal_led_exit(void);

/* =========================================================================
 * Buzzer / Relay (same GPIO driver, different semantic)
 * ========================================================================= */

int hal_buzzer_init(void);
int hal_buzzer_on(void);
int hal_buzzer_off(void);
void hal_buzzer_exit(void);

int hal_relay_init(void);
int hal_relay_set(int on);                  /**< 1=closed, 0=open */
int hal_relay_get(int *on);
void hal_relay_exit(void);

/* =========================================================================
 * DHT11 — Temperature & Humidity (OneWire GPIO)
 * ========================================================================= */

int hal_env_init(void);                     /**< alias for dht11_init */
int hal_env_read(hal_env_data_t *out);      /**< read both values at once */
int hal_env_read_temp(int *celsius);        /**< temperature only */
int hal_env_read_humidity(int *percent);    /**< humidity only */
void hal_env_exit(void);

/* =========================================================================
 * AP3216C — Ambient Light + Proximity (I2C)
 * ========================================================================= */

int hal_light_init(void);
int hal_light_read(hal_light_data_t *out);
int hal_light_read_lux(uint16_t *lux);     /**< ambient light only */
void hal_light_exit(void);

/* =========================================================================
 * SG90 — Curtain / Servo (PWM)
 * ========================================================================= */

int hal_curtain_init(void);

/**
 * Set curtain position.
 * @param angle 0=fully closed, 90=half open, 180=fully open
 */
int hal_curtain_set(int angle);

int hal_curtain_get(int *angle);            /**< query current position */

/**
 * Convenience percentages.
 * @param pct 0=closed, 50=half, 100=fully open (clamped)
 */
int hal_curtain_set_pct(int pct);
int hal_curtain_get_pct(int *pct);
void hal_curtain_exit(void);

/* =========================================================================
 * SR501 — PIR Motion Sensor (GPIO interrupt)
 * ========================================================================= */

int hal_motion_init(void);

/**
 * Poll current motion state.
 * @return HAL_MOTION_DETECTED or HAL_MOTION_NONE, negative on error.
 */
int hal_motion_poll(void);

/**
 * Register a callback for async motion events (SIGIO-based).
 * Only one callback at a time; pass NULL to deregister.
 * @param cb Called with HAL_MOTION_DETECTED or HAL_MOTION_NONE.
 */
typedef void (*hal_motion_cb_t)(hal_motion_t state);
int hal_motion_register_callback(hal_motion_cb_t cb);
void hal_motion_exit(void);

/* =========================================================================
 * GT9147 — Touchscreen (input subsystem, read-only)
 * ========================================================================= */

/**
 * Query whether the touchscreen is being touched right now.
 * @param x  Output: X coordinate (0..max_x)
 * @param y  Output: Y coordinate (0..max_y)
 * @return 1 if touched, 0 if not, negative on error.
 */
int hal_touch_poll(int *x, int *y);

/* =========================================================================
 * HC06 — Bluetooth UART
 * ========================================================================= */

int hal_bluetooth_init(const char *tty_path);  /**< e.g. "/dev/ttySAC1" */
int hal_bluetooth_send(const uint8_t *data, int len);
int hal_bluetooth_recv(uint8_t *buf, int max_len, int timeout_ms);
void hal_bluetooth_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_HAL_H */
