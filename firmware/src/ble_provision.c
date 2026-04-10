/**
 * ble_provision.c -- BLE WiFi provisioning implementation
 *
 * Emulator: provides env-var or hardcoded credentials.
 * ESP32: uses ESP-IDF wifi_provisioning with BLE transport.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ble_provision.h"
#include <string.h>
#include <stdio.h>

/* ================================================================== */
/*  EMULATOR / NATIVE: Stub implementation                             */
/* ================================================================== */

#ifdef CONFIG_STOVEIQ_USE_EMULATOR

#include <stdlib.h>

esp_err_t ble_provision_init(ble_provision_ctx_t *ctx,
                              const char *device_name,
                              const char *fw_version,
                              prov_complete_cb_t complete_cb,
                              prov_error_cb_t error_cb,
                              void *cb_ctx)
{
    if (!ctx) return ESP_FAIL;
    memset(ctx, 0, sizeof(ble_provision_ctx_t));
    ctx->complete_cb = complete_cb;
    ctx->error_cb = error_cb;
    ctx->cb_ctx = cb_ctx;
    ctx->status = PROV_STATUS_IDLE;
    if (device_name)
        strncpy(ctx->device_name, device_name, sizeof(ctx->device_name) - 1);
    if (fw_version)
        strncpy(ctx->fw_version, fw_version, sizeof(ctx->fw_version) - 1);
    return ESP_OK;
}

esp_err_t ble_provision_start(ble_provision_ctx_t *ctx)
{
    if (!ctx) return ESP_FAIL;
    ctx->status = PROV_STATUS_BLE_ACTIVE;
    printf("[BLE-EMU] Advertising as %s\n", ctx->device_name);

    const char *ssid = getenv("STOVEIQ_WIFI_SSID");
    const char *pass = getenv("STOVEIQ_WIFI_PASS");

    if (ssid && pass) {
        printf("[BLE-EMU] Using WiFi creds from environment: SSID=%s\n", ssid);
        strncpy(ctx->creds.ssid, ssid, sizeof(ctx->creds.ssid) - 1);
        strncpy(ctx->creds.password, pass, sizeof(ctx->creds.password) - 1);
    } else {
        strncpy(ctx->creds.ssid, "EmulatorWiFi", sizeof(ctx->creds.ssid) - 1);
        strncpy(ctx->creds.password, "emulator123", sizeof(ctx->creds.password) - 1);
        printf("[BLE-EMU] No env creds, using defaults: SSID=EmulatorWiFi\n");
    }

    ctx->status = PROV_STATUS_WIFI_CONNECTING;
    ctx->status = PROV_STATUS_WIFI_CONNECTED;
    ctx->status = PROV_STATUS_COMPLETE;
    printf("[BLE-EMU] Provisioning complete\n");

    if (ctx->complete_cb) {
        ctx->complete_cb(&ctx->creds, ctx->cb_ctx);
    }
    return ESP_OK;
}

esp_err_t ble_provision_stop(ble_provision_ctx_t *ctx)
{
    if (!ctx) return ESP_FAIL;
    ctx->status = PROV_STATUS_IDLE;
    return ESP_OK;
}

prov_status_t ble_provision_get_status(const ble_provision_ctx_t *ctx)
{
    return ctx ? ctx->status : PROV_STATUS_IDLE;
}

static bool s_has_stored = false;
static wifi_creds_t s_stored;

bool ble_provision_has_stored_creds(wifi_creds_t *creds)
{
    if (s_has_stored && creds) *creds = s_stored;
    return s_has_stored;
}

esp_err_t ble_provision_clear_creds(void)
{
    s_has_stored = false;
    memset(&s_stored, 0, sizeof(s_stored));
    return ESP_OK;
}

void ble_provision_inject_creds(ble_provision_ctx_t *ctx,
                                 const char *ssid, const char *password)
{
    if (!ctx) return;
    if (ssid) strncpy(ctx->creds.ssid, ssid, sizeof(ctx->creds.ssid) - 1);
    if (password) strncpy(ctx->creds.password, password, sizeof(ctx->creds.password) - 1);
    s_stored = ctx->creds;
    s_has_stored = true;
}

/* ================================================================== */
/*  ESP32: Real BLE provisioning via wifi_provisioning component       */
/* ================================================================== */

#elif defined(CONFIG_STOVEIQ_TARGET_ESP32)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "ble_prov";

#define NVS_WIFI_NS   "wifi_creds"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

static ble_provision_ctx_t *s_active_ctx = NULL;

static void prov_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    ble_provision_ctx_t *ctx = s_active_ctx;
    if (!ctx) return;

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            ctx->status = PROV_STATUS_BLE_ACTIVE;
            break;

        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received WiFi creds: SSID=%s", cfg->ssid);
            strncpy(ctx->creds.ssid, (char *)cfg->ssid,
                    sizeof(ctx->creds.ssid) - 1);
            strncpy(ctx->creds.password, (char *)cfg->password,
                    sizeof(ctx->creds.password) - 1);
            ctx->status = PROV_STATUS_WIFI_CONNECTING;
            break;
        }

        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *reason =
                (wifi_prov_sta_fail_reason_t *)event_data;
            ctx->last_error = (*reason == WIFI_PROV_STA_AUTH_ERROR)
                ? PROV_ERR_WIFI_AUTH_FAIL : PROV_ERR_SSID_NOT_FOUND;
            if (ctx->error_cb)
                ctx->error_cb(ctx->last_error, ctx->cb_ctx);
            break;
        }

        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            ctx->status = PROV_STATUS_WIFI_CONNECTED;

            /* Store in NVS */
            {
                nvs_handle_t nvs;
                if (nvs_open(NVS_WIFI_NS, NVS_READWRITE, &nvs) == ESP_OK) {
                    nvs_set_str(nvs, NVS_KEY_SSID, ctx->creds.ssid);
                    nvs_set_str(nvs, NVS_KEY_PASS, ctx->creds.password);
                    nvs_commit(nvs);
                    nvs_close(nvs);
                }
            }

            ctx->status = PROV_STATUS_COMPLETE;
            if (ctx->complete_cb)
                ctx->complete_cb(&ctx->creds, ctx->cb_ctx);
            break;

        case WIFI_PROV_END:
            wifi_prov_mgr_deinit();
            break;
        default:
            break;
        }
    }
}

esp_err_t ble_provision_init(ble_provision_ctx_t *ctx,
                              const char *device_name,
                              const char *fw_version,
                              prov_complete_cb_t complete_cb,
                              prov_error_cb_t error_cb,
                              void *cb_ctx)
{
    if (!ctx) return ESP_FAIL;
    memset(ctx, 0, sizeof(ble_provision_ctx_t));
    ctx->complete_cb = complete_cb;
    ctx->error_cb = error_cb;
    ctx->cb_ctx = cb_ctx;
    ctx->status = PROV_STATUS_IDLE;
    if (device_name)
        strncpy(ctx->device_name, device_name, sizeof(ctx->device_name) - 1);
    if (fw_version)
        strncpy(ctx->fw_version, fw_version, sizeof(ctx->fw_version) - 1);
    s_active_ctx = ctx;
    return ESP_OK;
}

esp_err_t ble_provision_start(ble_provision_ctx_t *ctx)
{
    if (!ctx) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL));

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    ESP_LOGI(TAG, "Starting BLE provisioning: %s", ctx->device_name);
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_1, (const void *)STOVEIQ_BLE_POP,
        ctx->device_name, NULL));

    ctx->status = PROV_STATUS_BLE_ACTIVE;
    return ESP_OK;
}

esp_err_t ble_provision_stop(ble_provision_ctx_t *ctx)
{
    if (!ctx) return ESP_FAIL;
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    ctx->status = PROV_STATUS_IDLE;
    return ESP_OK;
}

prov_status_t ble_provision_get_status(const ble_provision_ctx_t *ctx)
{
    return ctx ? ctx->status : PROV_STATUS_IDLE;
}

bool ble_provision_has_stored_creds(wifi_creds_t *creds)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_WIFI_NS, NVS_READONLY, &nvs) != ESP_OK)
        return false;
    size_t ssid_len = sizeof(creds->ssid);
    size_t pass_len = sizeof(creds->password);
    bool found = (nvs_get_str(nvs, NVS_KEY_SSID, creds->ssid, &ssid_len) == ESP_OK &&
                  nvs_get_str(nvs, NVS_KEY_PASS, creds->password, &pass_len) == ESP_OK);
    nvs_close(nvs);
    return found;
}

esp_err_t ble_provision_clear_creds(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_WIFI_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    return ESP_OK;
}

void ble_provision_inject_creds(ble_provision_ctx_t *ctx,
                                 const char *ssid, const char *password)
{
    (void)ctx; (void)ssid; (void)password;
}

#else
#error "Define CONFIG_STOVEIQ_USE_EMULATOR or CONFIG_STOVEIQ_TARGET_ESP32"
#endif
