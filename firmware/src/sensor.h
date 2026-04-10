/**
 * sensor.h — Sensor abstraction layer
 *
 * Provides a unified interface for reading thermal frames, whether from
 * the real MLX90640 sensor (via I2C) or the desktop emulator.
 *
 * Compile-time selection:
 *   CONFIG_STOVEIQ_USE_EMULATOR  -> thermal emulator backend
 *   CONFIG_STOVEIQ_TARGET_ESP32  -> real MLX90640 I2C backend
 */

#ifndef SENSOR_H
#define SENSOR_H

#include "stoveiq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Return codes                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    SENSOR_OK            =  0,
    SENSOR_ERR_INIT      = -1,   /* Initialization failed           */
    SENSOR_ERR_READ      = -2,   /* Frame read failed               */
    SENSOR_ERR_FLAT      = -3,   /* Frame rejected: all-same values */
    SENSOR_ERR_RANGE     = -4,   /* Frame rejected: values out of range */
    SENSOR_ERR_NO_INIT   = -5,   /* sensor_read called before init  */
} sensor_err_t;

/* ------------------------------------------------------------------ */
/*  Valid temperature range for frame validation                       */
/* ------------------------------------------------------------------ */

#define SENSOR_TEMP_MIN  (-40.0f)   /* MLX90640 spec minimum */
#define SENSOR_TEMP_MAX  (300.0f)   /* MLX90640 spec maximum */
#define SENSOR_FLAT_TOLERANCE (0.1f) /* Max deviation for "flat frame" */

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/**
 * Initialize the sensor backend.
 *
 * In emulator mode: prepares the emulator with the given scenario time.
 * In hardware mode: initializes I2C, configures MLX90640 registers.
 *
 * @return SENSOR_OK on success, negative error code on failure.
 */
sensor_err_t sensor_init(void);

/**
 * Read one complete 32x24 thermal frame.
 *
 * Performs sanity checks:
 *   - Rejects flat frames (all pixels within SENSOR_FLAT_TOLERANCE of each other)
 *   - Rejects frames with any pixel outside [SENSOR_TEMP_MIN, SENSOR_TEMP_MAX]
 *
 * @param frame  Output buffer for 768 float values (row-major, C)
 * @return SENSOR_OK on success, SENSOR_ERR_FLAT or SENSOR_ERR_RANGE on bad frame
 */
sensor_err_t sensor_read_frame(float frame[STOVEIQ_FRAME_PIXELS]);

/**
 * Extract maximum temperature from a frame.
 */
float sensor_get_max_temp(const float frame[STOVEIQ_FRAME_PIXELS]);

/**
 * Extract ambient temperature estimate from the coldest 10th percentile
 * of pixels in the frame.
 */
float sensor_get_ambient(const float frame[STOVEIQ_FRAME_PIXELS]);

/**
 * Check if the sensor has been initialized.
 */
bool sensor_is_initialized(void);

/* ------------------------------------------------------------------ */
/*  Emulator-only API                                                  */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_STOVEIQ_USE_EMULATOR

#include "thermal_emulator.h"

/**
 * Set the scenario and time for emulated frame generation.
 * Must be called after sensor_init() in emulator mode.
 */
void sensor_emu_set_scenario(const emu_scenario_t *scenario);

/**
 * Advance emulator time.  The next sensor_read_frame() call will
 * generate a frame for this time.
 */
void sensor_emu_set_time(float time_sec);

/**
 * Get the internal emulator state (for test inspection).
 */
emu_state_t *sensor_emu_get_state(void);

#endif /* CONFIG_STOVEIQ_USE_EMULATOR */

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_H */
