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
/*  Calibrated zone extraction (replaces CCL when calibrated)          */
/* ------------------------------------------------------------------ */

static int extract_calibrated_zones(const calibration_t *cal,
                                    const float frame[STOVEIQ_FRAME_PIXELS],
                                    burner_info_t out[STOVEIQ_MAX_BURNERS])
{
    int count = 0;
    for (int b = 0; b < cal->count && b < STOVEIQ_MAX_BURNERS; b++) {
        const burner_cal_t *bc = &cal->burners[b];
        if (!bc->enabled) continue;

        float sum = 0, mx = -999, mn = 9999;
        int px = 0;

        int r0 = bc->center_row - bc->radius;
        int r1 = bc->center_row + bc->radius;
        int c0 = bc->center_col - bc->radius;
        int c1 = bc->center_col + bc->radius;
        if (r0 < 0) r0 = 0;
        if (r1 >= STOVEIQ_FRAME_ROWS) r1 = STOVEIQ_FRAME_ROWS - 1;
        if (c0 < 0) c0 = 0;
        if (c1 >= STOVEIQ_FRAME_COLS) c1 = STOVEIQ_FRAME_COLS - 1;

        int r2 = bc->radius * bc->radius;

        for (int r = r0; r <= r1; r++) {
            for (int c = c0; c <= c1; c++) {
                int dr = r - bc->center_row;
                int dc = c - bc->center_col;
                if (dr * dr + dc * dc > r2) continue;

                float t = frame[r * STOVEIQ_FRAME_COLS + c];
                sum += t;
                if (t > mx) mx = t;
                if (t < mn) mn = t;
                px++;
            }
        }

        if (px > 0) {
            out[count].id = b;
            out[count].center_row = bc->center_row;
            out[count].center_col = bc->center_col;
            out[count].pixel_count = px;
            out[count].current_temp = sum / px;
            out[count].max_temp = mx;
            out[count].min_temp = mn;
            count++;
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/*  Recipe library                                                     */
/* ------------------------------------------------------------------ */

static const recipe_t s_recipes[] = {
    { "White Rice", 5, {
        { "Fill pot with water, set to high",  50.0f, TRIGGER_TARGET,     0,   "Water is heating up..." },
        { "Waiting for rolling boil...",      100.0f, TRIGGER_BOIL,       0,   "Boiling! Add rice now." },
        { "Added rice? Stir and reduce heat",  0.0f,  TRIGGER_CONFIRM,    0,   "" },
        { "Simmering... do NOT lift the lid",  0.0f,  TRIGGER_TIMER_DONE, 1080, "Timer done! Kill heat." },
        { "Rest 5 min, then fluff",            0.0f,  TRIGGER_TIMER_DONE, 300, "Rice is ready!" },
    }},
    { "Seared Steak", 7, {
        { "Place pan on burner, turn to high", 50.0f, TRIGGER_TARGET,     0,   "Pan is warming..." },
        { "Pan heating up...",                180.0f, TRIGGER_TARGET,     0,   "Almost there..." },
        { "Add oil now",                       0.0f,  TRIGGER_CONFIRM,    0,   "" },
        { "Oil heating...",                  230.0f,  TRIGGER_TARGET,     0,   "PAN IS SCREAMING HOT!" },
        { "Add steak!",                        0.0f,  TRIGGER_TIMER_DONE, 180, "FLIP NOW!" },
        { "Searing side 2...",                 0.0f,  TRIGGER_TIMER_DONE, 150, "Pull it! Rest time." },
        { "Rest 5 min (don't cut!)",           0.0f,  TRIGGER_TIMER_DONE, 300, "Steak is ready!" },
    }},
    { "Boiled Potatoes", 5, {
        { "Add potatoes + water, set high",   50.0f,  TRIGGER_TARGET,     0,   "Heating up..." },
        { "Waiting for boil...",             100.0f,  TRIGGER_BOIL,       0,   "Boiling! Reduce heat." },
        { "Reduce heat to medium",             0.0f,  TRIGGER_CONFIRM,    0,   "" },
        { "Simmering... wait for fork-tender", 0.0f,  TRIGGER_TIMER_DONE, 900, "Check with a fork!" },
        { "Drain and cool",                    0.0f,  TRIGGER_MANUAL,     0,   "" },
    }},
    { "Pasta", 5, {
        { "Fill pot with water, set to high", 50.0f,  TRIGGER_TARGET,     0,   "Heating up..." },
        { "Waiting for rolling boil...",     100.0f,  TRIGGER_BOIL,       0,   "Rolling boil!" },
        { "Add pasta and stir",                0.0f,  TRIGGER_CONFIRM,    0,   "" },
        { "Keep at boil. Stir occasionally.",  0.0f,  TRIGGER_TIMER_DONE, 480, "Check — al dente?" },
        { "Drain when ready",                  0.0f,  TRIGGER_MANUAL,     0,   "" },
    }},
    { "Fried Eggs", 5, {
        { "Place pan, set to medium-low",     50.0f,  TRIGGER_TARGET,     0,   "Warming up..." },
        { "Pan heating...",                  130.0f,  TRIGGER_TARGET,     0,   "Almost ready..." },
        { "Add butter or oil",                 0.0f,  TRIGGER_CONFIRM,    0,   "" },
        { "Butter melting... crack eggs when ready", 150.0f, TRIGGER_TARGET, 0, "Pan is perfect! Crack eggs." },
        { "Cooking...",                        0.0f,  TRIGGER_TIMER_DONE, 180, "Eggs are done!" },
    }},
    { "Caramelized Onions", 4, {
        { "Set to low heat, add oil/butter",  50.0f,  TRIGGER_TARGET,     0,   "Warming..." },
        { "Waiting for low heat...",         130.0f,  TRIGGER_TARGET,     0,   "Ready! Add onions." },
        { "Added onions? Stir occasionally",   0.0f,  TRIGGER_CONFIRM,    0,   "" },
        { "Low and slow... patience!",         0.0f,  TRIGGER_TIMER_DONE, 1800, "Golden brown! Done." },
    }},
};

#define NUM_RECIPES (sizeof(s_recipes) / sizeof(s_recipes[0]))

/* ------------------------------------------------------------------ */
/*  Recipe processing                                                  */
/* ------------------------------------------------------------------ */

static void process_recipe(cooking_engine_t *engine,
                           const thermal_snapshot_t *snap)
{
    recipe_session_t *rs = &engine->recipe;
    if (!rs->active) return;

    const recipe_t *recipe = &s_recipes[rs->recipe_idx];
    if (rs->current_step >= recipe->step_count) {
        rs->active = false;
        return;
    }

    const recipe_step_t *step = &recipe->steps[rs->current_step];

    /* Get temp for assigned burner */
    float temp = 0;
    bool found = false;
    for (int i = 0; i < snap->burner_count; i++) {
        if (snap->burners[i].id == rs->burner_id) {
            temp = snap->burners[i].current_temp;
            found = true;
            break;
        }
    }
    if (!found && rs->burner_id >= 0) return;

    /* Start timer on first frame of a TIMER_DONE step */
    if (step->timer_sec > 0 && !rs->timer_running &&
        step->trigger == TRIGGER_TIMER_DONE) {
        rs->timer_running = true;
        rs->timer_start_ms = snap->timestamp_ms;
    }

    /* Check trigger */
    bool advance = false;
    switch (step->trigger) {
    case TRIGGER_BOIL:
        if (temp >= engine->config.boil_temp_c) advance = true;
        break;
    case TRIGGER_SIMMER:
        if (temp >= 85.0f && temp <= 98.0f) advance = true;
        break;
    case TRIGGER_TARGET:
        if (temp >= step->target_temp) advance = true;
        break;
    case TRIGGER_FOOD_DROP:
        if (rs->prev_temp > 0 && (rs->prev_temp - temp) > 15.0f)
            advance = true;
        break;
    case TRIGGER_TIMER_DONE:
        if (rs->timer_running &&
            (snap->timestamp_ms - rs->timer_start_ms) >=
            (uint32_t)step->timer_sec * 1000) {
            advance = true;
        }
        break;
    case TRIGGER_TEMP_BELOW:
        if (temp < step->target_temp && temp > 0) advance = true;
        break;
    case TRIGGER_CONFIRM:
    case TRIGGER_MANUAL:
    default:
        break;
    }

    rs->prev_temp = temp;

    if (advance) {
        rs->current_step++;
        rs->step_start_ms = snap->timestamp_ms;
        rs->timer_running = false;
        rs->timer_start_ms = 0;
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

    burner_info_t new_burners[STOVEIQ_MAX_BURNERS];
    memset(new_burners, 0, sizeof(new_burners));
    int new_count;

    if (engine->use_calibration && engine->calibration.count > 0) {
        /* Calibrated mode: read temps from user-defined circles */
        new_count = extract_calibrated_zones(&engine->calibration,
                                             snapshot->frame, new_burners);
        track_burners(engine, new_burners, new_count, snapshot->timestamp_ms);
    } else {
        /* Auto mode: CCL-based detection */
        bool mask[STOVEIQ_FRAME_PIXELS];
        float threshold = snapshot->ambient_temp +
                          engine->config.burner_threshold_delta;
        for (int i = 0; i < STOVEIQ_FRAME_PIXELS; i++)
            mask[i] = (snapshot->frame[i] > threshold);

        static ccl_state_t ccl;
        ccl_label(mask, &ccl);

        new_count = extract_zones(&ccl, snapshot->frame,
                                  snapshot->ambient_temp,
                                  engine->config.min_burner_pixels,
                                  engine->config.max_burner_pixels,
                                  new_burners);
        track_burners(engine, new_burners, new_count, snapshot->timestamp_ms);
    }

    engine->burner_count = new_count;
    memcpy(engine->burners, new_burners, new_count * sizeof(burner_info_t));

    snapshot->burner_count = new_count;
    memcpy(snapshot->burners, new_burners, new_count * sizeof(burner_info_t));

    /* Inject simulated temp if active */
    if (engine->sim_active) {
        bool found_sim = false;
        for (int i = 0; i < snapshot->burner_count; i++) {
            if (snapshot->burners[i].id == engine->sim_burner_id) {
                snapshot->burners[i].current_temp = engine->sim_temp;
                snapshot->burners[i].max_temp = engine->sim_temp;
                engine->burners[i].current_temp = engine->sim_temp;
                engine->burners[i].max_temp = engine->sim_temp;
                found_sim = true;
                break;
            }
        }
        /* If no matching burner exists, create a virtual one */
        if (!found_sim && snapshot->burner_count < STOVEIQ_MAX_BURNERS) {
            int idx = snapshot->burner_count;
            snapshot->burners[idx].id = engine->sim_burner_id;
            snapshot->burners[idx].current_temp = engine->sim_temp;
            snapshot->burners[idx].max_temp = engine->sim_temp;
            snapshot->burners[idx].state = engine->sim_temp > 50.0f ?
                BURNER_STATE_HEATING : BURNER_STATE_OFF;
            snapshot->burners[idx].center_row = 12;
            snapshot->burners[idx].center_col = 16;
            snapshot->burners[idx].pixel_count = 20;
            snapshot->burner_count++;
            engine->burner_count = snapshot->burner_count;
            memcpy(engine->burners, snapshot->burners,
                   snapshot->burner_count * sizeof(burner_info_t));
        }
        if (engine->sim_temp > snapshot->max_temp)
            snapshot->max_temp = engine->sim_temp;
    }

    check_alerts(engine, snapshot);

    /* Process active recipe */
    process_recipe(engine, snapshot);
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

void cooking_engine_set_calibration(cooking_engine_t *engine,
                                    const calibration_t *cal)
{
    if (!engine || !cal) return;
    engine->calibration = *cal;
    engine->use_calibration = (cal->magic == CALIBRATION_MAGIC && cal->count > 0);
}

const calibration_t *cooking_engine_get_calibration(
    const cooking_engine_t *engine)
{
    return engine ? &engine->calibration : NULL;
}

void cooking_engine_start_recipe(cooking_engine_t *engine,
                                 uint8_t recipe_idx, int8_t burner_id)
{
    if (!engine || recipe_idx >= NUM_RECIPES) return;
    recipe_session_t *rs = &engine->recipe;
    memset(rs, 0, sizeof(*rs));
    rs->active = true;
    rs->recipe_idx = recipe_idx;
    rs->burner_id = burner_id;
    rs->current_step = 0;
}

void cooking_engine_recipe_next(cooking_engine_t *engine)
{
    if (!engine || !engine->recipe.active) return;
    const recipe_t *r = &s_recipes[engine->recipe.recipe_idx];
    engine->recipe.current_step++;
    engine->recipe.timer_running = false;
    engine->recipe.timer_start_ms = 0;
    if (engine->recipe.current_step >= r->step_count)
        engine->recipe.active = false;
}

void cooking_engine_recipe_confirm(cooking_engine_t *engine)
{
    if (!engine || !engine->recipe.active) return;
    const recipe_t *r = &s_recipes[engine->recipe.recipe_idx];
    if (engine->recipe.current_step >= r->step_count) return;
    const recipe_step_t *step = &r->steps[engine->recipe.current_step];
    if (step->trigger == TRIGGER_CONFIRM) {
        engine->recipe.current_step++;
        engine->recipe.timer_running = false;
        engine->recipe.timer_start_ms = 0;
        if (engine->recipe.current_step >= r->step_count)
            engine->recipe.active = false;
    }
}

void cooking_engine_recipe_stop(cooking_engine_t *engine)
{
    if (engine) engine->recipe.active = false;
}

const recipe_session_t *cooking_engine_get_recipe(
    const cooking_engine_t *engine)
{
    return engine ? &engine->recipe : NULL;
}

const recipe_t *cooking_engine_get_recipes(int *count)
{
    if (count) *count = NUM_RECIPES;
    return s_recipes;
}

void cooking_engine_set_sim_temp(cooking_engine_t *engine,
                                 int8_t burner_id, float temp)
{
    if (!engine) return;
    if (temp < 0) {
        engine->sim_active = false;
    } else {
        engine->sim_active = true;
        engine->sim_temp = temp;
        engine->sim_burner_id = burner_id;
    }
}
