/**
 * tasks.h -- FreeRTOS tasks / pthread emulation for StoveIQ
 *
 * 3-task cooking pipeline:
 *   Task 1: Sensor Read     (Core 1, Pri 20) -- reads MLX90640 at 4Hz
 *   Task 2: Cooking Engine  (Core 0, Pri 15) -- burner detect + alerts
 *   Task 3: Web Server      (Core 0, Pri 10) -- HTTP + WebSocket
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TASKS_H
#define TASKS_H

#include "stoveiq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    stoveiq_config_t config;
} tasks_config_t;

/**
 * Initialize and start all tasks.
 */
int tasks_start(const tasks_config_t *config);

/**
 * Stop all tasks and clean up.
 */
int tasks_stop(void);

/**
 * Check if tasks are running.
 */
bool tasks_is_running(void);

/**
 * Get the latest thermal snapshot (for external consumers).
 * Returns a copy — safe to read without locks.
 */
bool tasks_get_snapshot(thermal_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TASKS_H */
