/**
 * single_burner_30min.c — Scenario: One burner on for 30 minutes then off
 *
 * Simulates a typical cooking session with a single front-right burner.
 * Burner heats up over ~30 seconds, stays on for 30 minutes, then turns
 * off and cools.
 */

#include "thermal_emulator.h"

const emu_scenario_t scenario_single_burner_30min = {
    .name = "single_burner_30min",
    .ambient_temp = 22.0f,     /* Room temperature */
    .noise_stddev = 0.5f,      /* Moderate sensor noise */
    .angle_attenuation = 0.15f,/* Slight angle effect */
    .num_burners = 1,
    .burners = {
        {
            .row = 8,          /* Front-right area of stove */
            .col = 24,
            .radius = 5.0f,
            .peak_temp = 250.0f,
        },
    },
    .num_events = 2,
    .events = {
        /* t=10s:  Burner turns on, ramps up over 30s */
        { .time_sec = 10,   .burner_index = 0, .target_temp = 250.0f, .ramp_rate = 8.0f },
        /* t=1810s (30 min + 10s): Burner turns off, cools over 60s */
        { .time_sec = 1810, .burner_index = 0, .target_temp = 0.0f,   .ramp_rate = 4.0f },
    },
};
