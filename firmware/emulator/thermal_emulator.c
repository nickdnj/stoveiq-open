/**
 * thermal_emulator.c — Synthetic MLX90640 frame generator
 *
 * Generates realistic thermal frames by compositing:
 *   1. Ambient base temperature across all pixels
 *   2. Gaussian heat spots for each active burner
 *   3. Sensor mounting angle attenuation (rear pixels cooler)
 *   4. Random Gaussian noise on every pixel
 */

#include "thermal_emulator.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * Simple xorshift32 PRNG — fast, deterministic, no stdlib dependency.
 */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/**
 * Generate a Gaussian random value using Box-Muller transform.
 * Returns a value with mean=0, stddev=1.
 */
static float gaussian_noise(uint32_t *seed)
{
    /* Generate two uniform randoms in (0,1) */
    float u1 = (float)(xorshift32(seed) & 0x7FFFFFFF) / (float)0x7FFFFFFF;
    float u2 = (float)(xorshift32(seed) & 0x7FFFFFFF) / (float)0x7FFFFFFF;

    /* Clamp away from zero to avoid log(0) */
    if (u1 < 1e-10f) u1 = 1e-10f;

    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * 3.14159265f * u2);
}

/**
 * Compute heat contribution from a single burner at a pixel.
 * Uses a 2D Gaussian blob centered on the burner.
 */
static float burner_heat_at(const emu_burner_t *burner, float burner_temp,
                            int row, int col)
{
    if (burner_temp <= 0.0f) return 0.0f;

    float dr = (float)(row - burner->row);
    float dc = (float)(col - burner->col);
    float dist_sq = dr * dr + dc * dc;
    float sigma = burner->radius / 2.0f;  /* 2-sigma = full radius */
    float sigma_sq = sigma * sigma;

    if (sigma_sq < 0.01f) return 0.0f;

    return burner_temp * expf(-dist_sq / (2.0f * sigma_sq));
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void emu_init(emu_state_t *state, const emu_scenario_t *scenario)
{
    memset(state, 0, sizeof(emu_state_t));
    state->scenario = scenario;
    state->current_ambient = scenario->ambient_temp;
    state->seed = 42;  /* Deterministic seed for reproducibility */
    state->next_event_index = 0;
    state->last_time_sec = -1.0f;  /* Sentinel: no previous frame */

    for (int i = 0; i < EMU_MAX_BURNERS; i++) {
        state->burner_temps[i] = 0.0f;
        state->burner_targets[i] = 0.0f;
        state->burner_ramp_rates[i] = 0.0f;
    }
}

void emu_reset(emu_state_t *state)
{
    if (state->scenario) {
        emu_init(state, state->scenario);
    }
}

void emu_generate_frame(emu_state_t *state, float time_sec,
                         float frame_out[STOVEIQ_FRAME_PIXELS])
{
    const emu_scenario_t *sc = state->scenario;
    if (!sc) {
        memset(frame_out, 0, sizeof(float) * STOVEIQ_FRAME_PIXELS);
        return;
    }

    /* ---- Process any events that have fired by this time ---- */
    while (state->next_event_index < sc->num_events) {
        const emu_event_t *ev = &sc->events[state->next_event_index];
        if ((float)ev->time_sec > time_sec) break;

        if (ev->burner_index < 0) {
            /* Ambient change event */
            if (ev->ramp_rate <= 0.0f) {
                state->current_ambient = ev->target_temp;
            }
            /* For ramped ambient, handled in per-frame update below */
        } else if (ev->burner_index < sc->num_burners) {
            state->burner_targets[ev->burner_index] = ev->target_temp;
            state->burner_ramp_rates[ev->burner_index] =
                (ev->ramp_rate > 0.0f) ? ev->ramp_rate : 1000.0f; /* instant */
        }
        state->next_event_index++;
    }

    /* ---- Ramp burner temps toward targets ---- */
    float dt;
    if (state->last_time_sec < 0.0f) {
        dt = time_sec;  /* First frame: ramp from t=0 to now */
    } else {
        dt = time_sec - state->last_time_sec;
    }
    if (dt < 0.0f) dt = 0.0f;
    state->last_time_sec = time_sec;

    for (int b = 0; b < sc->num_burners; b++) {
        float diff = state->burner_targets[b] - state->burner_temps[b];
        float max_step = state->burner_ramp_rates[b] * dt;

        if (fabsf(diff) <= max_step) {
            state->burner_temps[b] = state->burner_targets[b];
        } else {
            state->burner_temps[b] += (diff > 0 ? max_step : -max_step);
        }
    }

    /* ---- Generate frame ---- */
    for (int r = 0; r < STOVEIQ_FRAME_ROWS; r++) {
        for (int c = 0; c < STOVEIQ_FRAME_COLS; c++) {
            int idx = r * STOVEIQ_FRAME_COLS + c;

            /* Start with ambient */
            float pixel = state->current_ambient;

            /* Add heat from each burner */
            for (int b = 0; b < sc->num_burners; b++) {
                pixel += burner_heat_at(&sc->burners[b],
                                        state->burner_temps[b],
                                        r, c);
            }

            /* Sensor angle attenuation: rear rows (high row numbers)
             * see less heat due to oblique viewing angle.
             * Row 0 = closest to sensor (full signal)
             * Row 23 = farthest (attenuated) */
            if (sc->angle_attenuation > 0.0f) {
                float row_factor = (float)r / (float)(STOVEIQ_FRAME_ROWS - 1);
                float attenuation = 1.0f - (sc->angle_attenuation * row_factor);
                /* Only attenuate the heat above ambient */
                float heat_above = pixel - state->current_ambient;
                pixel = state->current_ambient + heat_above * attenuation;
            }

            /* Add Gaussian noise */
            if (sc->noise_stddev > 0.0f) {
                pixel += gaussian_noise(&state->seed) * sc->noise_stddev;
            }

            frame_out[idx] = pixel;
        }
    }
}
