/**
 * cooking_engine.c -- Burner detection + cooking intelligence
 *
 * Connected Component Labeling (CCL) on thresholded thermal image
 * to detect burner zones.  Tracks per-burner state and generates
 * cooking alerts.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cooking_engine.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Connected Component Labeling (CCL) internals                       */
/* ------------------------------------------------------------------ */

#define CCL_MAX_LABELS  32   /* Max temporary labels (practical limit) */

typedef struct {
    uint8_t  labels[STOVEIQ_FRAME_PIXELS];
    int      parent[CCL_MAX_LABELS];  /* Union-find parent array */
    int      next_label;
} ccl_state_t;

static int ccl_find(ccl_state_t *s, int x)
{
    while (s->parent[x] != x) {
        s->parent[x] = s->parent[s->parent[x]];  /* Path compression */
        x = s->parent[x];
    }
    return x;
}

static void ccl_union(ccl_state_t *s, int a, int b)
{
    int ra = ccl_find(s, a);
    int rb = ccl_find(s, b);
    if (ra != rb) {
        s->parent[rb] = ra;
    }
}

/**
 * Two-pass CCL with 8-connectivity on a binary mask.
 * Returns the number of unique labels (after filtering).
 */
static int ccl_label(const bool mask[STOVEIQ_FRAME_PIXELS],
                     ccl_state_t *s)
{
    memset(s->labels, 0, sizeof(s->labels));
    for (int i = 0; i < CCL_MAX_LABELS; i++)
        s->parent[i] = i;
    s->next_label = 1;

    /* Pass 1: Assign temporary labels */
    for (int r = 0; r < STOVEIQ_FRAME_ROWS; r++) {
        for (int c = 0; c < STOVEIQ_FRAME_COLS; c++) {
            int idx = r * STOVEIQ_FRAME_COLS + c;
            if (!mask[idx]) continue;

            /* Check 4 neighbors: N, NW, W, NE (already visited) */
            int neighbors[4] = {0, 0, 0, 0};
            int nn = 0;

            if (r > 0 && s->labels[(r-1)*STOVEIQ_FRAME_COLS + c])
                neighbors[nn++] = s->labels[(r-1)*STOVEIQ_FRAME_COLS + c];
            if (r > 0 && c > 0 && s->labels[(r-1)*STOVEIQ_FRAME_COLS + (c-1)])
                neighbors[nn++] = s->labels[(r-1)*STOVEIQ_FRAME_COLS + (c-1)];
            if (c > 0 && s->labels[r*STOVEIQ_FRAME_COLS + (c-1)])
                neighbors[nn++] = s->labels[r*STOVEIQ_FRAME_COLS + (c-1)];
            if (r > 0 && c < STOVEIQ_FRAME_COLS-1 &&
                s->labels[(r-1)*STOVEIQ_FRAME_COLS + (c+1)])
                neighbors[nn++] = s->labels[(r-1)*STOVEIQ_FRAME_COLS + (c+1)];

            if (nn == 0) {
                /* New label */
                if (s->next_label < CCL_MAX_LABELS) {
                    s->labels[idx] = s->next_label++;
                }
            } else {
                /* Use smallest neighbor label */
                int min_label = neighbors[0];
                for (int i = 1; i < nn; i++) {
                    if (neighbors[i] < min_label)
                        min_label = neighbors[i];
                }
                s->labels[idx] = min_label;

                /* Union all neighbor labels */
                for (int i = 0; i < nn; i++) {
                    ccl_union(s, min_label, neighbors[i]);
                }
            }
        }
    }

    /* Pass 2: Resolve equivalences */
    for (int i = 0; i < STOVEIQ_FRAME_PIXELS; i++) {
        if (s->labels[i] > 0) {
            s->labels[i] = ccl_find(s, s->labels[i]);
        }
    }

    return s->next_label - 1;
}

/* ------------------------------------------------------------------ */
/*  Zone extraction from labeled image                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int   label;
    int   pixel_count;
    float sum_temp;
    float max_temp;
    float min_temp;
    int   sum_row;
    int   sum_col;
    float weighted_row;
    float weighted_col;
} zone_stats_t;

static int extract_zones(const ccl_state_t *s,
                         const float frame[STOVEIQ_FRAME_PIXELS],
                         float ambient,
                         int min_pixels, int max_pixels,
                         burner_info_t out_burners[STOVEIQ_MAX_BURNERS])
{
    zone_stats_t zones[CCL_MAX_LABELS];
    memset(zones, 0, sizeof(zones));

    /* Initialize min_temp to something high */
    for (int i = 0; i < CCL_MAX_LABELS; i++) {
        zones[i].min_temp = 9999.0f;
    }

    /* Accumulate stats per label */
    for (int r = 0; r < STOVEIQ_FRAME_ROWS; r++) {
        for (int c = 0; c < STOVEIQ_FRAME_COLS; c++) {
            int idx = r * STOVEIQ_FRAME_COLS + c;
            int lbl = s->labels[idx];
            if (lbl == 0) continue;

            zone_stats_t *z = &zones[lbl];
            z->label = lbl;
            z->pixel_count++;
            z->sum_temp += frame[idx];
            z->sum_row += r;
            z->sum_col += c;
            z->weighted_row += r * frame[idx];
            z->weighted_col += c * frame[idx];
            if (frame[idx] > z->max_temp) z->max_temp = frame[idx];
            if (frame[idx] < z->min_temp) z->min_temp = frame[idx];
        }
    }

    /* Filter and rank zones by total heat */
    typedef struct { int label; float heat; } ranked_t;
    ranked_t ranked[CCL_MAX_LABELS];
    int n_ranked = 0;

    for (int i = 1; i < CCL_MAX_LABELS; i++) {
        if (zones[i].pixel_count >= min_pixels &&
            zones[i].pixel_count <= max_pixels) {
            ranked[n_ranked].label = i;
            ranked[n_ranked].heat = zones[i].sum_temp -
                                    (ambient * zones[i].pixel_count);
            n_ranked++;
        }
    }

    /* Simple insertion sort by heat (descending) — tiny array */
    for (int i = 1; i < n_ranked; i++) {
        ranked_t tmp = ranked[i];
        int j = i - 1;
        while (j >= 0 && ranked[j].heat < tmp.heat) {
            ranked[j+1] = ranked[j];
            j--;
        }
        ranked[j+1] = tmp;
    }

    /* Take top MAX_BURNERS */
    int count = (n_ranked < STOVEIQ_MAX_BURNERS) ?
                 n_ranked : STOVEIQ_MAX_BURNERS;

    for (int i = 0; i < count; i++) {
        zone_stats_t *z = &zones[ranked[i].label];
        burner_info_t *b = &out_burners[i];

        b->id = i;
        b->pixel_count = z->pixel_count;
        b->current_temp = z->sum_temp / z->pixel_count;
        b->max_temp = z->max_temp;
        b->min_temp = z->min_temp;
        /* Temperature-weighted centroid */
        b->center_row = (int)(z->weighted_row / z->sum_temp + 0.5f);
        b->center_col = (int)(z->weighted_col / z->sum_temp + 0.5f);
    }

    return count;
}

/* ------------------------------------------------------------------ */
/*  Burner state tracking                                              */
/* ------------------------------------------------------------------ */

static burner_state_t classify_state(float temp_rate)
{
    if (temp_rate > 2.0f)  return BURNER_STATE_HEATING;
    if (temp_rate < -2.0f) return BURNER_STATE_COOLING;
    return BURNER_STATE_STABLE;
}

/* Match new burners to previous by centroid proximity */
static void track_burners(cooking_engine_t *engine,
                          burner_info_t new_burners[], int new_count,
                          uint32_t timestamp_ms)
{
    float dt = 0.0f;
    if (engine->prev_timestamp_ms > 0 && timestamp_ms > engine->prev_timestamp_ms) {
        dt = (float)(timestamp_ms - engine->prev_timestamp_ms) / 1000.0f;
    }

    for (int i = 0; i < new_count; i++) {
        /* Find closest previous burner by centroid distance */
        int best_match = -1;
        int best_dist = 9999;

        for (int j = 0; j < engine->burner_count; j++) {
            int dr = new_burners[i].center_row - engine->burners[j].center_row;
            int dc = new_burners[i].center_col - engine->burners[j].center_col;
            int dist = dr * dr + dc * dc;
            if (dist < best_dist && dist <= 9) {  /* Within 3 pixels */
                best_dist = dist;
                best_match = j;
            }
        }

        if (best_match >= 0 && dt > 0.01f) {
            /* Continuing burner — compute rate */
            float prev_t = engine->prev_temps[best_match];
            new_burners[i].temp_rate =
                (new_burners[i].current_temp - prev_t) / dt;
            new_burners[i].on_since_ms = engine->burners[best_match].on_since_ms;
        } else {
            /* New burner */
            new_burners[i].temp_rate = 0.0f;
            new_burners[i].on_since_ms = timestamp_ms;
        }

        new_burners[i].state = classify_state(new_burners[i].temp_rate);
    }

    /* Save for next frame */
    for (int i = 0; i < new_count && i < STOVEIQ_MAX_BURNERS; i++) {
        engine->prev_temps[i] = new_burners[i].current_temp;
    }
    engine->prev_timestamp_ms = timestamp_ms;
}

/* ------------------------------------------------------------------ */
/*  Alert detection                                                    */
/* ------------------------------------------------------------------ */

static void add_alert(cooking_engine_t *engine, cook_alert_type_t type,
                      int burner_id, float temp, uint32_t ts)
{
    /* Check if this alert type + burner already active */
    for (int i = 0; i < engine->alert_count; i++) {
        if (engine->alerts[i].type == type &&
            engine->alerts[i].burner_id == burner_id &&
            engine->alerts[i].active) {
            return;  /* Already alerting */
        }
    }

    if (engine->alert_count < STOVEIQ_MAX_ALERTS) {
        cook_alert_t *a = &engine->alerts[engine->alert_count++];
        a->type = type;
        a->burner_id = burner_id;
        a->temp = temp;
        a->timestamp_ms = ts;
        a->active = true;
    }
}

static void check_alerts(cooking_engine_t *engine,
                         const thermal_snapshot_t *snap)
{
    const stoveiq_config_t *cfg = &engine->config;

    for (int i = 0; i < snap->burner_count; i++) {
        const burner_info_t *b = &snap->burners[i];

        /* Boil detection: temp near boil_temp_c and rate near zero
         * after sustained rise */
        if (b->current_temp >= cfg->boil_temp_c &&
            b->state == BURNER_STATE_STABLE &&
            fabsf(b->temp_rate) < 0.5f) {
            add_alert(engine, COOK_ALERT_BOIL_DETECTED,
                      b->id, b->current_temp, snap->timestamp_ms);
        }

        /* Oil smoke point */
        if (b->current_temp >= cfg->smoke_point_c) {
            add_alert(engine, COOK_ALERT_SMOKE_POINT,
                      b->id, b->current_temp, snap->timestamp_ms);
        }

        /* Pan preheated: reached target and stable */
        if (b->current_temp >= cfg->preheat_target_c &&
            b->state == BURNER_STATE_STABLE) {
            add_alert(engine, COOK_ALERT_PAN_PREHEATED,
                      b->id, b->current_temp, snap->timestamp_ms);
        }

        /* Forgotten burner: on for too long with stable temp */
        if (b->on_since_ms > 0 && b->state == BURNER_STATE_STABLE) {
            uint32_t on_duration_ms = snap->timestamp_ms - b->on_since_ms;
            if (on_duration_ms >= cfg->forgotten_timeout_sec * 1000) {
                add_alert(engine, COOK_ALERT_FORGOTTEN,
                          b->id, b->current_temp, snap->timestamp_ms);
            }
        }
    }

    /* Clear alerts for burners that are no longer active */
    for (int i = 0; i < engine->alert_count; i++) {
        if (!engine->alerts[i].active) continue;

        bool still_active = false;
        for (int j = 0; j < snap->burner_count; j++) {
            if (snap->burners[j].id == engine->alerts[i].burner_id) {
                still_active = true;
                break;
            }
        }

        /* Clear smoke point / forgotten alerts when burner turns off */
        if (!still_active &&
            (engine->alerts[i].type == COOK_ALERT_SMOKE_POINT ||
             engine->alerts[i].type == COOK_ALERT_FORGOTTEN)) {
            engine->alerts[i].active = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void cooking_engine_init(cooking_engine_t *engine,
                         const stoveiq_config_t *config)
{
    memset(engine, 0, sizeof(*engine));
    engine->config = *config;
    engine->initialized = true;
}

void cooking_engine_process(cooking_engine_t *engine,
                            thermal_snapshot_t *snapshot)
{
    if (!engine->initialized) return;

    /* Step 1: Create binary mask (above ambient + threshold) */
    bool mask[STOVEIQ_FRAME_PIXELS];
    float threshold = snapshot->ambient_temp +
                      engine->config.burner_threshold_delta;

    for (int i = 0; i < STOVEIQ_FRAME_PIXELS; i++) {
        mask[i] = (snapshot->frame[i] > threshold);
    }

    /* Step 2: Connected Component Labeling */
    static ccl_state_t ccl;  /* ~800 bytes, keep off stack */
    ccl_label(mask, &ccl);

    /* Step 3: Extract and rank burner zones */
    burner_info_t new_burners[STOVEIQ_MAX_BURNERS];
    memset(new_burners, 0, sizeof(new_burners));

    int new_count = extract_zones(&ccl, snapshot->frame,
                                  snapshot->ambient_temp,
                                  engine->config.min_burner_pixels,
                                  engine->config.max_burner_pixels,
                                  new_burners);

    /* Step 4: Track burners across frames (compute dT/dt, state) */
    track_burners(engine, new_burners, new_count, snapshot->timestamp_ms);

    /* Step 5: Update engine state */
    engine->burner_count = new_count;
    memcpy(engine->burners, new_burners,
           new_count * sizeof(burner_info_t));

    /* Step 6: Copy to snapshot for downstream consumers */
    snapshot->burner_count = new_count;
    memcpy(snapshot->burners, new_burners,
           new_count * sizeof(burner_info_t));

    /* Step 7: Check alert conditions */
    check_alerts(engine, snapshot);
}

const cook_alert_t *cooking_engine_get_alerts(
    const cooking_engine_t *engine, int *count)
{
    *count = engine->alert_count;
    return engine->alerts;
}

void cooking_engine_silence_alert(cooking_engine_t *engine, int alert_idx)
{
    if (alert_idx >= 0 && alert_idx < engine->alert_count) {
        engine->alerts[alert_idx].active = false;
    }
}

void cooking_engine_silence_all(cooking_engine_t *engine)
{
    for (int i = 0; i < engine->alert_count; i++) {
        engine->alerts[i].active = false;
    }
}

void cooking_engine_update_config(cooking_engine_t *engine,
                                  const stoveiq_config_t *config)
{
    engine->config = *config;
}
