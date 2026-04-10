/**
 * oven_interference.c — Scenario: Oven preheating causes gradual ambient rise
 *
 * Tests the adaptive rolling ambient baseline.  The oven (below the stove
 * top) preheats and gradually raises ambient temperature across the entire
 * frame.  No burner is actually on, so the monitor should NOT trigger a
 * stove-on alert despite rising temperatures.
 */

#include "thermal_emulator.h"

const emu_scenario_t scenario_oven_interference = {
    .name = "oven_interference",
    .ambient_temp = 22.0f,
    .noise_stddev = 0.6f,
    .angle_attenuation = 0.1f,
    .num_burners = 1,
    /* One "burner" represents the oven heat radiating up — large radius,
     * low peak, centered in the frame to simulate uniform warming */
    .burners = {
        { .row = 12, .col = 16, .radius = 20.0f, .peak_temp = 40.0f },
    },
    .num_events = 4,
    .events = {
        /* Oven starts preheating at t=0, slow ramp */
        { .time_sec = 0,    .burner_index = 0,  .target_temp = 40.0f, .ramp_rate = 0.5f  },
        /* Ambient itself rises slowly due to oven radiant heat */
        { .time_sec = 0,    .burner_index = -1, .target_temp = 32.0f, .ramp_rate = 0.02f },
        /* Oven reaches temp at ~t=300s, holds */
        /* Oven turns off at t=1200s (20 min) */
        { .time_sec = 1200, .burner_index = 0,  .target_temp = 0.0f,  .ramp_rate = 0.3f  },
        { .time_sec = 1200, .burner_index = -1, .target_temp = 22.0f, .ramp_rate = 0.01f },
    },
};
