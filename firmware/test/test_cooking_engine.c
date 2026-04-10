/**
 * test_cooking_engine.c -- Unit tests for cooking engine
 *
 * Tests burner detection (CCL), state tracking, and alert generation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "unity.h"
#include "cooking_engine.h"
#include "sensor.h"
#include <string.h>
#include <math.h>

static cooking_engine_t engine;
static stoveiq_config_t config;

void setUp(void)
{
    config = (stoveiq_config_t)STOVEIQ_CONFIG_DEFAULTS;
    cooking_engine_init(&engine, &config);
}

void tearDown(void) {}

/* ---- Helper: create a frame with a hot spot ---- */

static void make_frame_with_hotspot(float frame[STOVEIQ_FRAME_PIXELS],
                                     float ambient, float hot_temp,
                                     int center_row, int center_col,
                                     int radius)
{
    for (int r = 0; r < STOVEIQ_FRAME_ROWS; r++) {
        for (int c = 0; c < STOVEIQ_FRAME_COLS; c++) {
            int dr = r - center_row;
            int dc = c - center_col;
            float dist = sqrtf((float)(dr*dr + dc*dc));
            if (dist <= radius) {
                frame[r * STOVEIQ_FRAME_COLS + c] = hot_temp;
            } else {
                frame[r * STOVEIQ_FRAME_COLS + c] = ambient;
            }
        }
    }
}

/* ---- Tests ---- */

void test_no_burners_on_cold_frame(void)
{
    thermal_snapshot_t snap = {0};
    for (int i = 0; i < STOVEIQ_FRAME_PIXELS; i++)
        snap.frame[i] = 22.0f;
    snap.ambient_temp = 22.0f;
    snap.max_temp = 22.0f;
    snap.timestamp_ms = 1000;

    cooking_engine_process(&engine, &snap);

    TEST_ASSERT_EQUAL_INT(0, snap.burner_count);
}

void test_single_burner_detected(void)
{
    thermal_snapshot_t snap = {0};
    make_frame_with_hotspot(snap.frame, 22.0f, 200.0f, 12, 16, 3);
    snap.ambient_temp = 22.0f;
    snap.max_temp = 200.0f;
    snap.timestamp_ms = 1000;

    cooking_engine_process(&engine, &snap);

    TEST_ASSERT_EQUAL_INT(1, snap.burner_count);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 200.0f, snap.burners[0].current_temp);
    TEST_ASSERT_INT_WITHIN(2, 12, snap.burners[0].center_row);
    TEST_ASSERT_INT_WITHIN(2, 16, snap.burners[0].center_col);
}

void test_two_burners_detected(void)
{
    thermal_snapshot_t snap = {0};
    /* Fill with ambient */
    for (int i = 0; i < STOVEIQ_FRAME_PIXELS; i++)
        snap.frame[i] = 22.0f;

    /* Burner 1: top-left */
    make_frame_with_hotspot(snap.frame, 22.0f, 180.0f, 6, 8, 3);
    /* Burner 2: bottom-right (add without overwriting burner 1) */
    for (int r = 0; r < STOVEIQ_FRAME_ROWS; r++) {
        for (int c = 0; c < STOVEIQ_FRAME_COLS; c++) {
            int dr = r - 18;
            int dc = c - 24;
            float dist = sqrtf((float)(dr*dr + dc*dc));
            if (dist <= 3) {
                snap.frame[r * STOVEIQ_FRAME_COLS + c] = 250.0f;
            }
        }
    }

    snap.ambient_temp = 22.0f;
    snap.max_temp = 250.0f;
    snap.timestamp_ms = 1000;

    cooking_engine_process(&engine, &snap);

    TEST_ASSERT_EQUAL_INT(2, snap.burner_count);
}

void test_noise_rejected(void)
{
    thermal_snapshot_t snap = {0};
    for (int i = 0; i < STOVEIQ_FRAME_PIXELS; i++)
        snap.frame[i] = 22.0f;

    /* Single hot pixel (too small for min_burner_pixels=4) */
    snap.frame[100] = 200.0f;
    snap.ambient_temp = 22.0f;
    snap.max_temp = 200.0f;
    snap.timestamp_ms = 1000;

    cooking_engine_process(&engine, &snap);

    TEST_ASSERT_EQUAL_INT(0, snap.burner_count);
}

void test_smoke_point_alert(void)
{
    config.smoke_point_c = 180.0f;
    cooking_engine_init(&engine, &config);

    thermal_snapshot_t snap = {0};
    make_frame_with_hotspot(snap.frame, 22.0f, 250.0f, 12, 16, 3);
    snap.ambient_temp = 22.0f;
    snap.max_temp = 250.0f;
    snap.timestamp_ms = 1000;

    cooking_engine_process(&engine, &snap);

    int count;
    const cook_alert_t *alerts = cooking_engine_get_alerts(&engine, &count);
    TEST_ASSERT_GREATER_THAN(0, count);

    /* Find smoke point alert */
    bool found = false;
    for (int i = 0; i < count; i++) {
        if (alerts[i].type == COOK_ALERT_SMOKE_POINT) {
            found = true;
            TEST_ASSERT_TRUE(alerts[i].active);
        }
    }
    TEST_ASSERT_TRUE(found);
}

void test_silence_alert(void)
{
    config.smoke_point_c = 180.0f;
    cooking_engine_init(&engine, &config);

    thermal_snapshot_t snap = {0};
    make_frame_with_hotspot(snap.frame, 22.0f, 250.0f, 12, 16, 3);
    snap.ambient_temp = 22.0f;
    snap.max_temp = 250.0f;
    snap.timestamp_ms = 1000;

    cooking_engine_process(&engine, &snap);

    int count;
    cooking_engine_get_alerts(&engine, &count);
    TEST_ASSERT_GREATER_THAN(0, count);

    cooking_engine_silence_all(&engine);

    const cook_alert_t *alerts = cooking_engine_get_alerts(&engine, &count);
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_FALSE(alerts[i].active);
    }
}

void test_config_update(void)
{
    stoveiq_config_t new_config = config;
    new_config.burner_threshold_delta = 50.0f;
    cooking_engine_update_config(&engine, &new_config);

    /* With higher threshold, same hot spot shouldn't trigger if delta < 50 */
    thermal_snapshot_t snap = {0};
    make_frame_with_hotspot(snap.frame, 22.0f, 60.0f, 12, 16, 3);
    snap.ambient_temp = 22.0f;
    snap.max_temp = 60.0f;
    snap.timestamp_ms = 1000;

    cooking_engine_process(&engine, &snap);

    /* 60 - 22 = 38, which is < 50 threshold, so no burner */
    TEST_ASSERT_EQUAL_INT(0, snap.burner_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_burners_on_cold_frame);
    RUN_TEST(test_single_burner_detected);
    RUN_TEST(test_two_burners_detected);
    RUN_TEST(test_noise_rejected);
    RUN_TEST(test_smoke_point_alert);
    RUN_TEST(test_silence_alert);
    RUN_TEST(test_config_update);
    return UNITY_END();
}
