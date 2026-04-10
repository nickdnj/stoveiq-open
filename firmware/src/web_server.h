/**
 * web_server.h -- HTTP + WebSocket server for StoveIQ
 *
 * Serves the web UI from SPIFFS and streams thermal data
 * over WebSocket to connected browsers.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "stoveiq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize and start the HTTP + WebSocket server.
 * Registers captive portal handlers and serves UI from SPIFFS.
 */
esp_err_t web_server_init(void);

/**
 * Broadcast a thermal snapshot to all connected WebSocket clients.
 * Called by the cooking engine task after processing each frame.
 *
 * Binary format: 4-byte timestamp (LE) + 768 x int16 (temp*10, LE)
 */
void web_server_broadcast_frame(const thermal_snapshot_t *snapshot);

/**
 * Broadcast cooking status (burners + alerts) as JSON text message.
 * Called at 1Hz or on state change.
 */
void web_server_broadcast_status(const thermal_snapshot_t *snapshot,
                                 const cook_alert_t *alerts,
                                 int alert_count,
                                 const recipe_session_t *recipe);

/**
 * Check for pending WebSocket commands from clients.
 * Returns true if a command was dequeued, fills cmd.
 */
bool web_server_get_command(ws_command_t *cmd);

/**
 * Start the captive portal DNS server (redirects all DNS to 192.168.4.1).
 */
void web_server_start_dns(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SERVER_H */
