/**
 * main.c -- StoveIQ Open Source firmware entry point
 *
 * ESP32: Init NVS, check for stored WiFi creds or start BLE
 *        provisioning, then launch the 3-task cooking pipeline.
 *
 * Emulator: Run thermal scenarios through the cooking engine
 *           to demonstrate burner detection and alerts.
 *
 * SPDX-License-Identifier: MIT
 */

#include "stoveiq_types.h"
#include "sensor.h"
#include "cooking_engine.h"
#include "tasks.h"
#include <stdio.h>
#include <string.h>

#define STOVEIQ_FW_VERSION "1.0.0"

/* ================================================================== */
/*  Native / Emulator entry point                                      */
/* ================================================================== */

#ifdef CONFIG_STOVEIQ_USE_EMULATOR

#include "thermal_emulator.h"
#include "scenarios/scenarios.h"
#include <unistd.h>

/**
 * Demo: Run a cooking scenario through the cooking engine.
 * Shows burner detection, state tracking, and alert generation.
 */
static void run_cooking_demo(void)
{
    printf("\n--- StoveIQ Cooking Engine Demo ---\n\n");

    sensor_init();
    sensor_emu_set_scenario(&scenario_all_burners);

    stoveiq_config_t config = STOVEIQ_CONFIG_DEFAULTS;
    cooking_engine_t engine;
    cooking_engine_init(&engine, &config);

    float frame[STOVEIQ_FRAME_PIXELS];
    float dt = 0.25f;  /* 4Hz */
    int total_frames = (int)(120.0f / dt);  /* 2 minutes */

    for (int i = 0; i < total_frames; i++) {
        float t = (float)i * dt;
        uint32_t ms = (uint32_t)(t * 1000.0f);

        sensor_emu_set_time(t);
        sensor_err_t err = sensor_read_frame(frame);
        if (err != SENSOR_OK) continue;

        thermal_snapshot_t snap = {0};
        memcpy(snap.frame, frame, sizeof(frame));
        snap.max_temp = sensor_get_max_temp(frame);
        snap.ambient_temp = sensor_get_ambient(frame);
        snap.timestamp_ms = ms;

        cooking_engine_process(&engine, &snap);

        if (i % 32 == 0) {
            printf("t=%5.0fs  max=%.1fC  amb=%.1fC  burners=%d",
                   t, snap.max_temp, snap.ambient_temp, snap.burner_count);
            for (int b = 0; b < snap.burner_count; b++) {
                const char *states[] = {"OFF", "HEAT", "STBL", "COOL"};
                printf("  [B%d:%.0fC/%s]", b,
                       snap.burners[b].current_temp,
                       states[snap.burners[b].state]);
            }
            printf("\n");
        }
    }

    int alert_count;
    const cook_alert_t *alerts = cooking_engine_get_alerts(&engine, &alert_count);
    if (alert_count > 0) {
        printf("\nAlerts generated: %d\n", alert_count);
        const char *alert_names[] = {
            "BOIL", "SMOKE_POINT", "PREHEATED", "FORGOTTEN", "FAULT"
        };
        for (int i = 0; i < alert_count; i++) {
            printf("  [%s] burner=%d temp=%.1fC t=%ums\n",
                   alert_names[alerts[i].type],
                   alerts[i].burner_id, alerts[i].temp,
                   alerts[i].timestamp_ms);
        }
    }

    printf("\nCooking demo complete.\n");
}

/**
 * Demo: Run the threaded pipeline for a few seconds.
 */
static void run_threaded_demo(void)
{
    printf("\n--- Threaded Pipeline Demo (10 seconds) ---\n\n");

    sensor_init();
    sensor_emu_set_scenario(&scenario_single_burner_30min);

    tasks_config_t config;
    config.config = (stoveiq_config_t)STOVEIQ_CONFIG_DEFAULTS;

    tasks_start(&config);
    sleep(10);

    printf("\nStopping tasks...\n");
    tasks_stop();
    printf("Threaded demo complete.\n");
}

int main(void)
{
    printf("StoveIQ Open Source v%s (emulator)\n", STOVEIQ_FW_VERSION);
    printf("=========================================================\n");

    run_cooking_demo();
    run_threaded_demo();

    return 0;
}

/* ================================================================== */
/*  ESP32-S3 entry point                                               */
/* ================================================================== */

#elif defined(CONFIG_STOVEIQ_TARGET_ESP32)

#include "ble_provision.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "main";
static ble_provision_ctx_t s_prov_ctx;

static void on_provisioning_complete(const wifi_creds_t *creds, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Provisioning complete: SSID=%s", creds->ssid);
    ble_provision_stop(&s_prov_ctx);

    tasks_config_t config;
    config.config = (stoveiq_config_t)STOVEIQ_CONFIG_DEFAULTS;
    tasks_start(&config);
}

static void on_provisioning_error(prov_error_t error, void *ctx)
{
    (void)ctx;
    ESP_LOGE(TAG, "Provisioning error: 0x%02x", error);
}

void app_main(void)
{
    ESP_LOGI(TAG, "StoveIQ Open Source v%s", STOVEIQ_FW_VERSION);

    /* Init NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Init networking stack — required before WiFi provisioning */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    /* Check for stored WiFi creds (from BLE provisioning or web UI) */
    wifi_creds_t stored_creds;
    if (ble_provision_has_stored_creds(&stored_creds)) {
        ESP_LOGI(TAG, "Found stored WiFi: SSID=%s", stored_creds.ssid);
        tasks_config_t config;
        config.config = (stoveiq_config_t)STOVEIQ_CONFIG_DEFAULTS;
        tasks_start(&config);
    } else {
        ESP_LOGI(TAG, "No stored WiFi — starting BLE provisioning");
        printf("\n");
        printf("*******************************************\n");
        printf("*  BLE Provisioning Active                *\n");
        printf("*  Connect with ESP BLE Prov app          *\n");
        printf("*  PoP: stoveiq-setup                     *\n");
        printf("*******************************************\n\n");

        ble_provision_init(&s_prov_ctx,
                           "StoveIQ", STOVEIQ_FW_VERSION,
                           on_provisioning_complete,
                           on_provisioning_error,
                           NULL);
        ble_provision_start(&s_prov_ctx);
    }
}

#else
#error "Define CONFIG_STOVEIQ_USE_EMULATOR or CONFIG_STOVEIQ_TARGET_ESP32"
#endif
