/**
 * thermal_emulator.h — Synthetic MLX90640 frame generator
 *
 * Generates 32x24 float frames simulating an IR thermal camera looking
 * at a stove top.  Supports multiple burners, time-based scenarios,
 * Gaussian noise injection, and sensor mounting angle attenuation.
 *
 * Used for desktop testing when CONFIG_STOVEIQ_USE_EMULATOR is set.
 */

#ifndef THERMAL_EMULATOR_H
#define THERMAL_EMULATOR_H

#include "stoveiq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define EMU_MAX_BURNERS     6
#define EMU_MAX_EVENTS     64

/* ------------------------------------------------------------------ */
/*  Burner definition                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int     row;            /* Center row (0-23)                     */
    int     col;            /* Center column (0-31)                  */
    float   radius;         /* Heat radius in pixels                 */
    float   peak_temp;      /* Peak temperature when fully on (C)   */
} emu_burner_t;

/* ------------------------------------------------------------------ */
/*  Scenario event — defines when burners change state                 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t  time_sec;       /* Seconds from scenario start       */
    int       burner_index;   /* Which burner (0-based), -1 = ambient */
    float     target_temp;    /* Target temp (C).  0 = burner off  */
    float     ramp_rate;      /* Degrees per second ramp speed.  0 = instant */
} emu_event_t;

/* ------------------------------------------------------------------ */
/*  Full scenario definition                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char   *name;
    float         ambient_temp;           /* Starting ambient (C)       */
    float         noise_stddev;           /* Gaussian noise sigma (C)   */
    float         angle_attenuation;      /* 0.0-1.0: rear pixel factor */
    int           num_burners;
    emu_burner_t  burners[EMU_MAX_BURNERS];
    int           num_events;
    emu_event_t   events[EMU_MAX_EVENTS];
} emu_scenario_t;

/* ------------------------------------------------------------------ */
/*  Emulator state (opaque internal)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const emu_scenario_t *scenario;
    float   current_ambient;              /* Current ambient after drift */
    float   burner_temps[EMU_MAX_BURNERS];/* Current temp per burner    */
    float   burner_targets[EMU_MAX_BURNERS];
    float   burner_ramp_rates[EMU_MAX_BURNERS];
    int     next_event_index;
    float   last_time_sec;                /* Previous frame time for dt  */
    uint32_t seed;                        /* PRNG state for noise       */
} emu_state_t;

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/**
 * Initialize emulator with a scenario.
 * Must be called before generate_frame.
 */
void emu_init(emu_state_t *state, const emu_scenario_t *scenario);

/**
 * Generate a synthetic 32x24 thermal frame for the given time.
 *
 * @param state       Emulator state (updated in place)
 * @param time_sec    Current scenario time in seconds
 * @param frame_out   Output buffer, must hold STOVEIQ_FRAME_PIXELS floats
 */
void emu_generate_frame(emu_state_t *state, float time_sec, float frame_out[STOVEIQ_FRAME_PIXELS]);

/**
 * Reset the emulator to its initial state (re-run same scenario).
 */
void emu_reset(emu_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* THERMAL_EMULATOR_H */
