/**
 * false_positive_sun.c — Scenario: Sunny window creates localized heat
 *
 * A sunbeam through a nearby window creates a warm spot on the stove
 * surface.  The spot is localized (like a burner) but only reaches ~45C
 * above ambient — below the 50C ON threshold.  Tests that the monitor
 * does NOT false-trigger.
 *
 * Then the sun shifts and creates a hotter spot at ~55C above ambient
 * for a brief period — tests that the 3-frame debounce filters out
 * transient spikes.
 */

#include "thermal_emulator.h"

const emu_scenario_t scenario_false_positive_sun = {
    .name = "false_positive_sun",
    .ambient_temp = 24.0f,       /* Warm day */
    .noise_stddev = 1.0f,        /* Higher noise on sunny day */
    .angle_attenuation = 0.1f,
    .num_burners = 2,
    .burners = {
        /* Sustained warm spot — below threshold */
        { .row = 10, .col = 20, .radius = 6.0f, .peak_temp = 45.0f },
        /* Brief hot spot — above threshold but transient */
        { .row = 14, .col = 12, .radius = 3.0f, .peak_temp = 60.0f },
    },
    .num_events = 4,
    .events = {
        /* Warm spot appears at t=0, ramps slowly (sun moving) */
        { .time_sec = 0,   .burner_index = 0, .target_temp = 45.0f, .ramp_rate = 0.5f },
        /* Brief hot flash at t=300s, only lasts ~2s (1 frame at 2Hz) */
        { .time_sec = 300, .burner_index = 1, .target_temp = 60.0f, .ramp_rate = 100.0f },
        { .time_sec = 301, .burner_index = 1, .target_temp = 0.0f,  .ramp_rate = 100.0f },
        /* Warm spot fades at t=600s */
        { .time_sec = 600, .burner_index = 0, .target_temp = 0.0f,  .ramp_rate = 0.3f },
    },
};
