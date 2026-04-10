/**
 * cooking_engine.h -- Burner detection + cooking intelligence
 *
 * Pure logic module (no FreeRTOS deps).  Processes thermal frames
 * to detect burner zones, track per-burner state, and generate
 * cooking alerts (boil, smoke point, preheat, forgotten).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COOKING_ENGINE_H
#define COOKING_ENGINE_H

#include "stoveiq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Cooking engine state                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    stoveiq_config_t  config;
    burner_info_t     burners[STOVEIQ_MAX_BURNERS];
    int               burner_count;
    cook_alert_t      alerts[STOVEIQ_MAX_ALERTS];
    int               alert_count;
    float             prev_temps[STOVEIQ_MAX_BURNERS];  /* For dT/dt   */
    uint32_t          prev_timestamp_ms;
    bool              initialized;
} cooking_engine_t;

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/**
 * Initialize the cooking engine with configuration.
 */
void cooking_engine_init(cooking_engine_t *engine,
                         const stoveiq_config_t *config);

/**
 * Process a thermal frame.  Detects burner zones, updates state,
 * checks alert conditions.  Enriches the snapshot with burner data.
 *
 * @param engine   Engine state
 * @param snapshot Frame data (frame, ambient, max already filled).
 *                 burner_count and burners[] are filled by this call.
 */
void cooking_engine_process(cooking_engine_t *engine,
                            thermal_snapshot_t *snapshot);

/**
 * Get current active alerts.
 */
const cook_alert_t *cooking_engine_get_alerts(const cooking_engine_t *engine,
                                              int *count);

/**
 * Silence/dismiss a specific alert by index.
 */
void cooking_engine_silence_alert(cooking_engine_t *engine, int alert_idx);

/**
 * Silence all active alerts.
 */
void cooking_engine_silence_all(cooking_engine_t *engine);

/**
 * Update configuration at runtime (e.g., from web UI settings).
 */
void cooking_engine_update_config(cooking_engine_t *engine,
                                  const stoveiq_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* COOKING_ENGINE_H */
