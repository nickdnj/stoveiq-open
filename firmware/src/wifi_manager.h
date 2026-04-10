/**
 * wifi_manager.h -- WiFi AP+STA manager for StoveIQ
 *
 * Manages WiFi in AP+STA concurrent mode:
 *   - Always creates "StoveIQ" AP at 192.168.4.1 (zero config)
 *   - Optionally joins home WiFi (creds stored in NVS)
 *   - mDNS: stoveiq.local
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "stoveiq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize WiFi in AP mode.  Creates "StoveIQ" open hotspot.
 * If stored STA credentials exist in NVS, also connects to home WiFi.
 */
esp_err_t wifi_manager_init(const char *ap_ssid);

/**
 * Connect to a home WiFi network (STA mode).
 * Credentials are stored in NVS for next boot.
 */
esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password);

/**
 * Check if STA (home WiFi) is connected.
 */
bool wifi_manager_sta_connected(void);

/**
 * Get STA IP address as string.  Returns empty string if not connected.
 */
const char *wifi_manager_get_sta_ip(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
