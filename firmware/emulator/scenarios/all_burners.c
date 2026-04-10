/**
 * all_burners.c — Scenario: All 4 burners cycling on and off
 *
 * Simulates a busy cooking session where burners are turned on and off
 * at different times, testing the monitor's ability to track multiple
 * heat sources and correctly detect stove-off only when ALL are off.
 */

#include "thermal_emulator.h"

const emu_scenario_t scenario_all_burners = {
    .name = "all_burners",
    .ambient_temp = 23.0f,
    .noise_stddev = 0.8f,
    .angle_attenuation = 0.2f,
    .num_burners = 4,
    .burners = {
        { .row = 6,  .col = 8,  .radius = 4.5f, .peak_temp = 230.0f },  /* Front-left  */
        { .row = 6,  .col = 24, .radius = 4.5f, .peak_temp = 260.0f },  /* Front-right */
        { .row = 18, .col = 8,  .radius = 4.0f, .peak_temp = 200.0f },  /* Rear-left   */
        { .row = 18, .col = 24, .radius = 4.0f, .peak_temp = 220.0f },  /* Rear-right  */
    },
    .num_events = 8,
    .events = {
        /* Front-right on at t=5s */
        { .time_sec = 5,    .burner_index = 1, .target_temp = 260.0f, .ramp_rate = 10.0f },
        /* Front-left on at t=60s */
        { .time_sec = 60,   .burner_index = 0, .target_temp = 230.0f, .ramp_rate = 8.0f  },
        /* Rear burners on at t=120s */
        { .time_sec = 120,  .burner_index = 2, .target_temp = 200.0f, .ramp_rate = 6.0f  },
        { .time_sec = 120,  .burner_index = 3, .target_temp = 220.0f, .ramp_rate = 6.0f  },
        /* Front-right off at t=600s (10 min) */
        { .time_sec = 600,  .burner_index = 1, .target_temp = 0.0f,   .ramp_rate = 5.0f  },
        /* Rear burners off at t=900s (15 min) */
        { .time_sec = 900,  .burner_index = 2, .target_temp = 0.0f,   .ramp_rate = 4.0f  },
        { .time_sec = 900,  .burner_index = 3, .target_temp = 0.0f,   .ramp_rate = 4.0f  },
        /* Front-left off at t=1200s (20 min) */
        { .time_sec = 1200, .burner_index = 0, .target_temp = 0.0f,   .ramp_rate = 5.0f  },
    },
};
