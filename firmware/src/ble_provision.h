/**
 * ble_provision.h -- BLE WiFi provisioning for StoveIQ
 *
 * ESP32: Uses ESP-IDF wifi_provisioning with BLE transport.
 * Emulator: Reads creds from env vars or uses defaults.
 *
 * Stores credentials in NVS for reconnection after reboot.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef BLE_PROVISION_H
#define BLE_PROVISION_H

#include "stoveiq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STOVEIQ_BLE_POP "stoveiq-setup"

typedef void (*prov_complete_cb_t)(const wifi_creds_t *creds, void *ctx);
typedef void (*prov_error_cb_t)(prov_error_t error, void *ctx);

typedef struct {
    wifi_creds_t       creds;
    prov_status_t      status;
    prov_error_t       last_error;
    prov_complete_cb_t complete_cb;
    prov_error_cb_t    error_cb;
    void              *cb_ctx;
    char               device_name[32];
    char               fw_version[16];
} ble_provision_ctx_t;

esp_err_t ble_provision_init(ble_provision_ctx_t *ctx,
                              const char *device_name,
                              const char *fw_version,
                              prov_complete_cb_t complete_cb,
                              prov_error_cb_t error_cb,
                              void *cb_ctx);

esp_err_t ble_provision_start(ble_provision_ctx_t *ctx);
esp_err_t ble_provision_stop(ble_provision_ctx_t *ctx);
prov_status_t ble_provision_get_status(const ble_provision_ctx_t *ctx);
bool ble_provision_has_stored_creds(wifi_creds_t *creds);
esp_err_t ble_provision_clear_creds(void);
void ble_provision_inject_creds(ble_provision_ctx_t *ctx,
                                 const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif /* BLE_PROVISION_H */
