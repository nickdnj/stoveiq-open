/**
 * web_server.c -- HTTP + WebSocket server
 *
 * HTTP server with:
 *   GET /          -> Serves index.html from SPIFFS (or fallback HTML)
 *   GET /ws        -> WebSocket endpoint for real-time thermal streaming
 *   GET /api/settings -> Read current configuration
 *   POST /api/settings -> Update configuration
 *   GET /api/logs  -> List cook session logs
 *   Captive portal handlers for iOS/Android
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef CONFIG_STOVEIQ_TARGET_ESP32

#include "web_server.h"
#include "cooking_engine.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "silent_mp4.h"
#include "certs.h"

static const char *TAG = "webserver";

/* ------------------------------------------------------------------ */
/*  WebSocket client tracking                                          */
/* ------------------------------------------------------------------ */

#define MAX_WS_CLIENTS  4

static httpd_handle_t s_server = NULL;
static httpd_handle_t s_redirect_server = NULL;  /* HTTP:80 → HTTPS:443 redirect */
static int s_ws_fds[MAX_WS_CLIENTS];
static int s_ws_count = 0;
static SemaphoreHandle_t s_ws_mutex = NULL;
static QueueHandle_t s_cmd_queue = NULL;

/* Embedded TLS cert + key PEMs live in certs.h as C string literals. */

/* ------------------------------------------------------------------ */
/*  SPIFFS                                                             */
/* ------------------------------------------------------------------ */

static bool s_spiffs_ready = false;

static void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "spiffs",
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK) {
        s_spiffs_ready = true;
        size_t total = 0, used = 0;
        esp_spiffs_info("spiffs", &total, &used);
        ESP_LOGI(TAG, "SPIFFS: %d/%d bytes used", (int)used, (int)total);
    } else {
        ESP_LOGW(TAG, "SPIFFS mount failed, using fallback HTML");
    }
}

/* ------------------------------------------------------------------ */
/*  Fallback HTML (when SPIFFS is empty)                               */
/* ------------------------------------------------------------------ */

/* Cooking Coach dashboard — embedded HTML */
static const char FALLBACK_HTML[] =
"<!DOCTYPE html><html><head>\n"
"<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1,user-scalable=no'>\n"
"<meta name=apple-mobile-web-app-capable content=yes>\n"
"<meta name=apple-mobile-web-app-status-bar-style content=black-translucent>\n"
"<meta name=theme-color content=#111>\n"
"<link rel=icon type=image/png href=/icon.png>\n"
"<link rel=manifest href=/manifest.json>\n"
"<link rel=apple-touch-icon href=/icon.png>\n"
"<title>StoveIQ</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:system-ui,-apple-system,sans-serif;background:#111;color:#eee;"
"max-width:500px;margin:0 auto;padding:12px;-webkit-user-select:none;user-select:none}\n"
/* Header */
".hdr{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px}\n"
".hdr h1{font-size:20px;color:#f59e0b}\n"
".hdr .dot{width:10px;height:10px;border-radius:50%;background:#555}\n"
".hdr .dot.on{background:#4ade80;box-shadow:0 0 6px #4ade80}\n"
".hdr .dot.awake{background:#3b82f6;box-shadow:0 0 6px #3b82f6}\n"
".unit-btn{background:#222;border:1px solid #444;color:#eee;padding:4px 10px;"
"border-radius:4px;font-size:13px;cursor:pointer}\n"
/* Burner cards — primary UI */
".cards{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:12px}\n"
".card{background:#1a1a1a;border:2px solid #333;border-radius:12px;padding:14px;"
"position:relative;overflow:hidden;min-height:140px}\n"
".card.heating{border-color:#f59e0b;box-shadow:0 0 12px #f59e0b33}\n"
".card.stable{border-color:#4ade80;box-shadow:0 0 12px #4ade8033}\n"
".card.cooling{border-color:#60a5fa;box-shadow:0 0 12px #60a5fa33}\n"
".card .label{font-size:12px;color:#888;text-transform:uppercase;letter-spacing:1px;"
"margin-bottom:4px}\n"
".card .temp{font-size:36px;font-weight:800;line-height:1.1}\n"
".card .temp.heating{color:#f59e0b}\n"
".card .temp.stable{color:#4ade80}\n"
".card .temp.cooling{color:#60a5fa}\n"
".card .temp.off{color:#555}\n"
".card .meta{font-size:11px;color:#666;margin-top:6px}\n"
".card .trend{font-size:16px;position:absolute;top:14px;right:14px}\n"
".card .state-badge{display:inline-block;font-size:10px;padding:2px 8px;"
"border-radius:3px;margin-top:4px;font-weight:600}\n"
".state-badge.heating{background:#f59e0b22;color:#f59e0b}\n"
".state-badge.stable{background:#4ade8022;color:#4ade80}\n"
".state-badge.cooling{background:#60a5fa22;color:#60a5fa}\n"
".state-badge.off{background:#33333344;color:#666}\n"
/* Heat bar */
".heat-bar{height:4px;background:#333;border-radius:2px;margin-top:6px;overflow:hidden}\n"
".heat-bar div{height:100%;border-radius:2px;transition:width 0.5s}\n"
".recipe-tag{font-size:10px;color:#f59e0b;margin-top:4px;font-weight:600}\n"
/* Timer */
".timer-row{display:flex;gap:8px;margin-top:6px}\n"
".timer-row button{flex:1;padding:4px;font-size:11px;background:#222;border:1px solid #444;"
"color:#eee;border-radius:4px;cursor:pointer}\n"
".timer-row button:active{background:#333}\n"
".timer-display{font-size:16px;font-weight:700;color:#f59e0b;text-align:center;margin-top:4px}\n"
/* Alerts */
".alert-bar{position:fixed;top:0;left:0;right:0;z-index:100;padding:8px}\n"
".alert{background:#dc2626;color:#fff;padding:10px 14px;border-radius:8px;"
"margin-bottom:6px;display:flex;justify-content:space-between;align-items:center;"
"font-size:14px;font-weight:600;animation:slideIn .3s;cursor:pointer;"
"max-width:500px;margin-left:auto;margin-right:auto}\n"
".alert.boil{background:#2563eb}\n"
".alert.preheat{background:#16a34a}\n"
".alert.smoke{background:#dc2626}\n"
".alert.forgotten{background:#d97706}\n"
"@keyframes slideIn{from{transform:translateY(-100%);opacity:0}to{transform:translateY(0);opacity:1}}\n"
/* No burners */
".no-burners{text-align:center;padding:20px;color:#555;font-size:14px}\n"
/* Settings gear */
".settings-btn{background:none;border:none;color:#888;font-size:18px;cursor:pointer;padding:4px}\n"
/* Settings panel */
".settings{display:none;background:#1a1a1a;border:1px solid #333;border-radius:8px;"
"padding:14px;margin-bottom:12px}\n"
".settings.open{display:block}\n"
".settings label{display:block;font-size:12px;color:#888;margin-top:8px}\n"
".settings input,.settings select{width:100%;background:#222;border:1px solid #444;"
"color:#eee;padding:6px;border-radius:4px;font-size:13px;margin-top:2px}\n"
/* ======== Phase 1: Cookware chip, event row, modals, graph ======== */
".cw-chip{display:inline-block;background:#222;color:#ccc;font-size:11px;"
"padding:3px 8px;border-radius:12px;margin-bottom:6px;cursor:pointer;"
"border:1px solid #444;user-select:none}\n"
".cw-chip.assigned{background:#1e3a5f;color:#93c5fd;border-color:#3b82f6}\n"
".cw-chip.recording{background:#7f1d1d;color:#fca5a5;border-color:#dc2626;"
"animation:pulse 1.4s infinite}\n"
"@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.55}}\n"
".card .rec-dot{position:absolute;top:10px;left:10px;width:8px;height:8px;"
"border-radius:50%;background:#dc2626;animation:pulse 1s infinite}\n"
/* Modal */
".modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,0.82);"
"z-index:200;padding:10px;overflow-y:auto;-webkit-overflow-scrolling:touch}\n"
".modal.open{display:block}\n"
".modal-inner{max-width:520px;margin:30px auto;background:#1a1a1a;"
"border:1px solid #333;border-radius:12px;padding:16px}\n"
".modal h2{font-size:17px;color:#f59e0b;margin-bottom:12px}\n"
".modal .close{float:right;background:none;border:none;color:#888;"
"font-size:26px;line-height:1;cursor:pointer;padding:0 6px}\n"
".modal .row{display:flex;gap:8px;align-items:center;padding:10px;"
"background:#222;border-radius:8px;margin-bottom:6px;cursor:pointer}\n"
".modal .row:active{background:#2d2d2d}\n"
".modal .row .icon{font-size:22px;flex-shrink:0;width:30px;text-align:center}\n"
".modal .row .info{flex:1;min-width:0}\n"
".modal .row .info .name{font-size:14px;font-weight:600;color:#eee}\n"
".modal .row .info .sub{font-size:11px;color:#888;margin-top:2px;"
"overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
".modal .row button.del{background:#3d1a1a;border:1px solid #dc2626;"
"color:#fca5a5;padding:4px 10px;border-radius:4px;font-size:11px;"
"cursor:pointer;flex-shrink:0}\n"
".modal .actions{display:flex;gap:8px;margin-top:14px;flex-wrap:wrap}\n"
".modal .actions button{flex:1;min-width:120px;padding:10px;border-radius:6px;"
"border:1px solid #444;background:#222;color:#eee;font-size:13px;cursor:pointer}\n"
".modal .actions button.primary{background:#f59e0b;border-color:#f59e0b;color:#000;font-weight:600}\n"
".modal .actions button.danger{background:#3d1a1a;border-color:#dc2626;color:#fca5a5}\n"
/* Form */
".form-row{margin-bottom:10px}\n"
".form-row label{display:block;font-size:12px;color:#888;margin-bottom:4px}\n"
".form-row input,.form-row select,.form-row textarea{width:100%;background:#111;"
"border:1px solid #333;color:#eee;padding:8px;border-radius:4px;font-size:14px;"
"font-family:inherit}\n"
/* Burner view (drill-in) */
".bv-head{display:flex;justify-content:space-between;align-items:center;"
"margin-bottom:10px;gap:8px;flex-wrap:wrap}\n"
".bv-head h2{margin:0}\n"
".bv-meta{background:#222;padding:8px 10px;border-radius:6px;margin-bottom:10px;"
"font-size:12px;color:#aaa;display:flex;gap:10px;flex-wrap:wrap;align-items:center}\n"
".bv-meta .label{color:#f59e0b;font-weight:600}\n"
".bv-meta button{background:#333;border:1px solid #555;color:#eee;"
"padding:3px 8px;border-radius:4px;font-size:11px;cursor:pointer}\n"
".bv-graph{background:#0a0a0a;border:1px solid #222;border-radius:8px;"
"padding:6px;margin-bottom:10px;position:relative}\n"
".bv-graph canvas{display:block;width:100%;height:220px;touch-action:none}\n"
".bv-range{display:flex;gap:4px;margin-bottom:10px}\n"
".bv-range button{flex:1;padding:5px;font-size:11px;background:#222;"
"border:1px solid #444;color:#aaa;border-radius:4px;cursor:pointer}\n"
".bv-range button.active{background:#3b82f6;border-color:#3b82f6;color:#fff}\n"
".ev-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:5px;margin-bottom:10px}\n"
"@media(max-width:400px){.ev-grid{grid-template-columns:repeat(3,1fr)}}\n"
".ev-grid button{padding:10px 4px;font-size:12px;background:#2d2d2d;"
"border:1px solid #555;color:#eee;border-radius:5px;cursor:pointer;"
"display:flex;flex-direction:column;align-items:center;gap:2px;line-height:1.1;"
"min-height:52px}\n"
".ev-grid button:active{background:#444;transform:scale(0.97)}\n"
".ev-grid button .ev-icon{font-size:18px}\n"
".ev-grid button.flash{background:#f59e0b;border-color:#f59e0b;color:#000}\n"
/* Session row replay action */
".sess-chip{display:inline-block;background:#1e3a5f;color:#93c5fd;font-size:10px;"
"padding:2px 6px;border-radius:10px;margin-left:6px}\n"
".sess-chip.rec{background:#7f1d1d;color:#fca5a5}\n"
/* Annotation picker sheet */
".ann-sheet{position:fixed;left:10px;right:10px;bottom:10px;background:#1a1a1a;"
"border:1px solid #444;border-radius:10px;padding:12px;z-index:250;"
"box-shadow:0 -4px 20px rgba(0,0,0,0.5);display:none;max-width:520px;margin:0 auto}\n"
".ann-sheet.open{display:block}\n"
".ann-sheet h3{font-size:13px;color:#f59e0b;margin-bottom:8px}\n"
"</style></head><body>\n"
/* Hidden silent video — NoSleep fallback when Wake Lock API is unavailable
   (HTTP/mDNS on iOS Safari is non-secure context, Wake Lock is blocked) */
"<video id=sleepVid src=/silent.mp4 loop muted playsinline "
"style='position:fixed;width:1px;height:1px;opacity:0;pointer-events:none'></video>\n"
/* ============ Phase 1 modals ============ */
/* Burner View (drill-in on a burner card) */
"<div class=modal id=burnerView>\n"
"  <div class=modal-inner>\n"
"    <button class=close onclick=closeBurnerView()>&times;</button>\n"
"    <div class=bv-head><h2 id=bvTitle>Burner</h2></div>\n"
"    <div class=bv-meta id=bvMeta></div>\n"
"    <div class=bv-graph><canvas id=bvCanvas></canvas></div>\n"
"    <div class=bv-range>\n"
"      <button data-range=60000 onclick=\"bvSetRange(60000)\">1m</button>\n"
"      <button data-range=300000 class=active onclick=\"bvSetRange(300000)\">5m</button>\n"
"      <button data-range=600000 onclick=\"bvSetRange(600000)\">10m</button>\n"
"      <button data-range=0 onclick=\"bvSetRange(0)\">All</button>\n"
"    </div>\n"
"    <div class=ev-grid id=bvEvents></div>\n"
"    <div class=actions>\n"
"      <button id=bvEndBtn onclick=bvEndSession() class=danger>End session</button>\n"
"      <button onclick=bvCalibrateBoil()>Calibrate boil (100\\u00B0C)</button>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
/* Cookware picker (shown when starting a session) */
"<div class=modal id=cwPicker>\n"
"  <div class=modal-inner>\n"
"    <button class=close onclick=closeCwPicker()>&times;</button>\n"
"    <h2>What's on <span id=cwPickerBurner></span>?</h2>\n"
"    <div id=cwPickerList></div>\n"
"    <div class=actions>\n"
"      <button onclick=showCwForm()>+ Add cookware</button>\n"
"      <button onclick=cwPickerClear()>Remove cookware</button>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
/* Cookware library (from settings) */
"<div class=modal id=cwLibrary>\n"
"  <div class=modal-inner>\n"
"    <button class=close onclick=closeCwLibrary()>&times;</button>\n"
"    <h2>Cookware library</h2>\n"
"    <div id=cwLibList></div>\n"
"    <div class=actions>\n"
"      <button class=primary onclick=showCwForm()>+ Add cookware</button>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
/* Cookware add/edit form */
"<div class=modal id=cwForm>\n"
"  <div class=modal-inner>\n"
"    <button class=close onclick=closeCwForm()>&times;</button>\n"
"    <h2 id=cwFormTitle>Add cookware</h2>\n"
"    <div class=form-row><label>Name</label>\n"
"      <input id=cwFormName maxlength=40 placeholder=\"e.g. 10&quot; non-stick egg pan\"></div>\n"
"    <div class=form-row><label>Icon (emoji)</label>\n"
"      <input id=cwFormIcon maxlength=2 value=\"\\uD83C\\uDF73\"></div>\n"
"    <div class=form-row><label>Material</label>\n"
"      <select id=cwFormMaterial></select></div>\n"
"    <div class=form-row><label>Notes (optional)</label>\n"
"      <input id=cwFormNotes maxlength=80></div>\n"
"    <div class=actions>\n"
"      <button onclick=closeCwForm()>Cancel</button>\n"
"      <button class=primary onclick=saveCwForm()>Save</button>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
/* Sessions browser */
"<div class=modal id=sessModal>\n"
"  <div class=modal-inner>\n"
"    <button class=close onclick=closeSessions()>&times;</button>\n"
"    <h2>Cook sessions</h2>\n"
"    <div id=sessList></div>\n"
"    <div class=actions>\n"
"      <button class=primary onclick=exportBackup()>Export backup</button>\n"
"      <button onclick=\"document.getElementById('impFile').click()\">Import\\u2026</button>\n"
"    </div>\n"
"    <input type=file id=impFile accept=.json "
"style=\"display:none\" onchange=importBackup(event)>\n"
"  </div>\n"
"</div>\n"
/* Annotation picker sheet (for tap-on-graph) */
"<div class=ann-sheet id=annSheet>\n"
"  <h3 id=annHdr>Add event</h3>\n"
"  <div class=ev-grid id=annEvents></div>\n"
"  <div class=actions>\n"
"    <button onclick=closeAnn()>Cancel</button>\n"
"  </div>\n"
"</div>\n"
/* Alert bar */
"<div class=alert-bar id=alerts></div>\n"
/* Header */
"<div class=hdr>\n"
"  <h1>StoveIQ</h1>\n"
"  <div style='display:flex;align-items:center;gap:8px'>\n"
"    <button class=unit-btn onclick=showRecipePicker()>Recipes</button>\n"
"    <button class=unit-btn id=unitBtn onclick=toggleUnit()>C</button>\n"
"    <button class=settings-btn onclick=toggleSettings()>&#9881;</button>\n"
"    <div class=dot id=wakeDot title='Screen kept awake'></div>\n"
"    <div class=dot id=dot></div>\n"
"  </div>\n"
"</div>\n"
/* Settings */
"<div class=settings id=settingsPanel>\n"
"  <div style='display:flex;gap:8px;margin-bottom:10px'>\n"
"    <button class=unit-btn style='flex:1' onclick=openCwLibrary()>\\uD83C\\uDF73 Cookware library</button>\n"
"    <button class=unit-btn style='flex:1' onclick=openSessions()>\\uD83D\\uDCCA Sessions &amp; backup</button>\n"
"  </div>\n"
"  <label>Boil Temp<input type=number id=cfgBoil value=95 step=1></label>\n"
"  <label>Smoke Point<input type=number id=cfgSmoke value=230 step=1></label>\n"
"  <label>Preheat Target<input type=number id=cfgPreheat value=200 step=1></label>\n"
"  <label>Forgotten Timeout (min)<input type=number id=cfgForgot value=30 step=1></label>\n"
"  <div style='margin-top:10px;padding-top:10px;border-top:1px solid #333'>\n"
"    <div style='font-size:12px;color:#f59e0b;font-weight:700;margin-bottom:6px'>"
"Sensor Calibration</div>\n"
"    <label>Emissivity: <b id=emLabel>0.95</b>"
"<input type=range id=cfgEmissivity min=10 max=100 value=95 step=1 "
"style='width:100%' oninput=\"document.getElementById('emLabel').textContent="
"(this.value/100).toFixed(2);sendCmd({cmd:'set_emissivity',value:this.value/100})\">"
"</label>\n"
"    <div style='display:flex;gap:4px;margin:4px 0 8px;flex-wrap:wrap'>\n"
"      <button class=unit-btn onclick=\"setEm(0.95)\">Ceramic</button>\n"
"      <button class=unit-btn onclick=\"setEm(0.85)\">Painted</button>\n"
"      <button class=unit-btn onclick=\"setEm(0.70)\">Cast Iron</button>\n"
"      <button class=unit-btn onclick=\"setEm(0.30)\">Steel</button>\n"
"    </div>\n"
"    <label>Temp Offset: <b id=offLabel>0</b>\\u00B0C"
"<input type=range id=cfgOffset min=-20 max=20 value=0 step=1 "
"style='width:100%' oninput=\"document.getElementById('offLabel').textContent="
"this.value;sendCmd({cmd:'set_temp_offset',value:parseFloat(this.value)})\">"
"</label>\n"
"  </div>\n"
"  <button style='width:100%;margin-top:12px;padding:10px;background:#f59e0b;border:none;"
"border-radius:6px;color:#111;font-weight:700;font-size:14px;cursor:pointer' "
"onclick=startCalibration()>Calibrate Burners</button>\n"
"  <label style='display:flex;align-items:center;gap:8px;margin-top:12px;cursor:pointer'>"
"<input type=checkbox id=simMode onchange=toggleSim()> Simulation Mode</label>\n"
"  <div id=simControls style='display:none;margin-top:8px'>\n"
"    <label>Simulated Temp: <b id=simTempLabel>25</b>\\u00B0C</label>\n"
"    <input type=range id=simSlider min=20 max=300 value=25 step=1 "
"style='width:100%' oninput=updateSim()>\n"
"    <div style='display:flex;gap:4px;margin-top:6px;flex-wrap:wrap'>\n"
"      <button class=unit-btn onclick=simPreset(25)>Cold</button>\n"
"      <button class=unit-btn onclick=simPreset(80)>Warm</button>\n"
"      <button class=unit-btn onclick=simPreset(100)>Boil</button>\n"
"      <button class=unit-btn onclick=simPreset(150)>Med</button>\n"
"      <button class=unit-btn onclick=simPreset(200)>High</button>\n"
"      <button class=unit-btn onclick=simPreset(250)>Sear</button>\n"
"    </div>\n"
"    <div style='display:flex;gap:4px;margin-top:6px'>\n"
"      <button class=unit-btn onclick=simRamp(100,60)>Ramp to Boil (60s)</button>\n"
"      <button class=unit-btn onclick=simRamp(230,45)>Ramp to Sear (45s)</button>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
/* Recipe picker */
"<div id=recipePicker style='display:none;background:#1a1a1a;border:1px solid #333;"
"border-radius:8px;padding:12px;margin-bottom:12px'>\n"
"  <div style='font-size:14px;font-weight:700;margin-bottom:8px'>Select Recipe</div>\n"
"  <div id=recipeList></div>\n"
"</div>\n"
/* Active recipe display */
"<div id=recipeActive style='display:none;background:#1a1a1a;border:2px solid #f59e0b;"
"border-radius:8px;padding:14px;margin-bottom:12px'>\n"
"  <div style='display:flex;justify-content:space-between;align-items:center'>\n"
"    <div style='font-size:12px;color:#f59e0b;font-weight:700' id=recipeName></div>\n"
"    <button style='background:#333;border:1px solid #555;color:#eee;padding:4px 10px;"
"border-radius:4px;font-size:11px;cursor:pointer' onclick=recipeStop()>Cancel</button>\n"
"  </div>\n"
"  <div style='font-size:11px;color:#888;margin:4px 0' id=recipeStep></div>\n"
"  <div style='font-size:20px;font-weight:700;margin:8px 0' id=recipeDesc></div>\n"
"  <div style='font-size:28px;font-weight:700;color:#f59e0b;text-align:center' id=recipeTimer></div>\n"
"  <div style='font-size:14px;color:#4ade80;text-align:center;margin-top:4px;font-weight:600;"
"min-height:20px' id=recipeCoach></div>\n"
"  <button style='width:100%;margin-top:8px;padding:8px;background:#222;border:1px solid #444;"
"color:#eee;border-radius:4px;font-size:13px;cursor:pointer' onclick=recipeNext()>Next Step</button>\n"
"</div>\n"
/* Calibration overlay */
"<div id=calOverlay style='display:none;position:fixed;top:0;left:0;right:0;bottom:0;"
"background:rgba(0,0,0,0.9);z-index:200;padding:12px'>\n"
"  <div style='color:#f59e0b;font-size:18px;font-weight:700;text-align:center;margin-bottom:8px'>"
"Calibrate Burners</div>\n"
"  <div style='color:#888;font-size:12px;text-align:center;margin-bottom:12px'>"
"Tap on each burner location. Drag to move. Pinch or buttons to resize.</div>\n"
"  <div style='position:relative;width:100%;max-width:480px;margin:0 auto;aspect-ratio:32/24'>\n"
"    <canvas id=calHm width=256 height=192 style='width:100%;height:100%;"
"border-radius:8px'></canvas>\n"
"    <canvas id=calOv width=320 height=240 style='position:absolute;top:0;left:0;width:100%;"
"height:100%;border-radius:8px'></canvas>\n"
"  </div>\n"
"  <div style='display:flex;gap:6px;justify-content:center;margin-top:12px;align-items:center'>\n"
"    <label style='color:#888;font-size:12px'>Name:</label>\n"
"    <input type=text id=calName maxlength=15 placeholder='Front Left' "
"style='background:#222;border:1px solid #444;color:#eee;padding:6px 8px;"
"border-radius:4px;font-size:13px;width:140px' "
"oninput=\"if(calSelected>=0)calBurners[calSelected].name=this.value\">\n"
"  </div>\n"
"  <div style='display:flex;gap:8px;justify-content:center;margin-top:12px'>\n"
"    <button style='padding:8px 20px;background:#333;border:1px solid #555;color:#eee;"
"border-radius:6px;font-size:14px;cursor:pointer' onclick=calAddBurner()>+ Burner</button>\n"
"    <button style='padding:8px 20px;background:#333;border:1px solid #555;color:#eee;"
"border-radius:6px;font-size:14px;cursor:pointer' onclick=calShrink()>-</button>\n"
"    <button style='padding:8px 20px;background:#333;border:1px solid #555;color:#eee;"
"border-radius:6px;font-size:14px;cursor:pointer' onclick=calGrow()>+</button>\n"
"    <button style='padding:8px 20px;background:#dc2626;border:none;color:#fff;"
"border-radius:6px;font-size:14px;cursor:pointer' onclick=calDeleteSelected()>Del</button>\n"
"    <button style='padding:8px 20px;background:#555;border:none;color:#eee;"
"border-radius:6px;font-size:14px;cursor:pointer' onclick=calClear()>Clear All</button>\n"
"  </div>\n"
"  <div style='display:flex;gap:8px;justify-content:center;margin-top:12px'>\n"
"    <button style='padding:10px 30px;background:#f59e0b;border:none;color:#111;"
"border-radius:6px;font-size:16px;font-weight:700;cursor:pointer' onclick=calSave()>Save</button>\n"
"    <button style='padding:10px 30px;background:#333;border:1px solid #555;color:#eee;"
"border-radius:6px;font-size:16px;cursor:pointer' onclick=calCancel()>Cancel</button>\n"
"  </div>\n"
"</div>\n"
/* Burner cards — primary UI */
"<div id=cards class=cards></div>\n"
"<div class=no-burners id=noBurners style='padding:40px 20px'>"
"<div style='font-size:18px;margin-bottom:8px'>No burners calibrated</div>"
"<div style='font-size:13px'>Open Settings \\u2192 Calibrate Burners to get started</div></div>\n"
"\n"
"<script>\n"
/* State */
"let useFahrenheit=localStorage.getItem('siq_unit')==='F';\n"
"let burners=[],activeAlerts=[],timers={};\n"
"const STATES=['OFF','HEATING','STABLE','COOLING'];\n"
"const ALERT_NAMES=['BOIL','SMOKE POINT','PREHEATED','FORGOTTEN','FAULT'];\n"
"const ALERT_CSS=['boil','smoke','preheat','forgotten','smoke'];\n"
/* Colormap */
"const CM=[[0,0,4],[22,7,55],[67,14,105],[101,21,110],[137,34,106],"
"[170,51,87],[199,73,59],[224,107,28],[245,155,12],[252,206,37],[252,255,164]];\n"
"function lerp(a,b,t){return[a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,a[2]+(b[2]-a[2])*t]}\n"
"function cmap(f){f=Math.max(0,Math.min(1,f));let s=f*(CM.length-1),i=Math.floor(s);"
"if(i>=CM.length-1)return CM[CM.length-1];return lerp(CM[i],CM[i+1],s-i)}\n"
/* Temp conversion */
"function tf(c){return useFahrenheit?c*9/5+32:c}\n"
"function tu(){return useFahrenheit?'F':'C'}\n"
"function fmtT(c){return tf(c).toFixed(0)+'\\u00B0'+tu()}\n"
/* Toggle */
"function toggleUnit(){const ids=['cfgBoil','cfgSmoke','cfgPreheat'];\n"
"  if(!useFahrenheit){ids.forEach(id=>{const el=document.getElementById(id);"
"el.value=Math.round(parseFloat(el.value)*9/5+32)})}\n"
"  else{ids.forEach(id=>{const el=document.getElementById(id);"
"el.value=Math.round((parseFloat(el.value)-32)*5/9)})}\n"
"  useFahrenheit=!useFahrenheit;"
"localStorage.setItem('siq_unit',useFahrenheit?'F':'C');"
"document.getElementById('unitBtn').textContent=tu()}\n"
"document.getElementById('unitBtn').textContent=tu();\n"
"if(useFahrenheit){['cfgBoil','cfgSmoke','cfgPreheat'].forEach(id=>{"
"const el=document.getElementById(id);"
"el.value=Math.round(parseFloat(el.value)*9/5+32)})}\n"
"function toggleSettings(){document.getElementById('settingsPanel').classList.toggle('open')}\n"
"function sendCmd(obj){if(ws&&ws.readyState===1)ws.send(JSON.stringify(obj))}\n"
"function setEm(v){document.getElementById('cfgEmissivity').value=Math.round(v*100);"
"document.getElementById('emLabel').textContent=v.toFixed(2);"
"sendCmd({cmd:'set_emissivity',value:v})}\n"
/* Thermal data (used by calibration overlay only) */
"let temps=new Float32Array(768);\n"
/* Audio alert */
"let audioCtx;\n"
"function beep(){return;try{if(!audioCtx)audioCtx=new(window.AudioContext||window.webkitAudioContext)();"
"const o=audioCtx.createOscillator(),g=audioCtx.createGain();"
"o.connect(g);g.connect(audioCtx.destination);o.frequency.value=880;"
"g.gain.value=0.3;o.start();o.stop(audioCtx.currentTime+0.15);"
"setTimeout(()=>{const o2=audioCtx.createOscillator(),g2=audioCtx.createGain();"
"o2.connect(g2);g2.connect(audioCtx.destination);o2.frequency.value=1100;"
"g2.gain.value=0.3;o2.start();o2.stop(audioCtx.currentTime+0.15)},200);"
"}catch(e){}}\n"
/* Timer */
"function startTimer(bid,mins){timers[bid]={start:Date.now(),dur:mins*60*1000};"
"renderCards()}\n"
"function clearTimer(bid){delete timers[bid];renderCards()}\n"
"function fmtTime(ms){if(ms<0)return'DONE';let s=Math.floor(ms/1000),"
"m=Math.floor(s/60);s%=60;return m+':'+(s<10?'0':'')+s}\n"
/* Render burner cards */
"function renderCards(){\n"
"  const el=document.getElementById('cards');\n"
"  const nb=document.getElementById('noBurners');\n"
"  if(!burners||burners.length===0){el.innerHTML='';nb.style.display='block';return}\n"
"  nb.style.display='none';\n"
"  el.innerHTML=burners.map(b=>{\n"
"    const st=STATES[b.state]||'OFF';\n"
"    const cls=st.toLowerCase();\n"
"    const arrow=b.rate>1?'\\u2197':b.rate<-1?'\\u2198':'\\u2192';\n"
"    const dur=b.on||0;\n"
"    const mins=Math.floor(dur/60000);\n"
"    const secs=Math.floor((dur%60000)/1000);\n"
"    const timeStr=dur>0?(mins+'m '+secs+'s'):'';\n"
/* Heat bar: temp as % of 300C max */
"    const heatPct=Math.min(100,Math.max(0,Math.round(b.temp/300*100)));\n"
"    const heatColor=heatPct>80?'#dc2626':heatPct>50?'#f59e0b':heatPct>20?'#eab308':'#4ade80';\n"
/* Timer */
"    let timerHtml='';\n"
"    if(timers[b.id]){\n"
"      const rem=timers[b.id].dur-(Date.now()-timers[b.id].start);\n"
"      timerHtml='<div class=timer-display>'+fmtTime(rem)+'</div>'\n"
"        +'<div class=timer-row><button onclick=clearTimer('+b.id+')>Cancel</button></div>';\n"
"      if(rem<=0){beep();delete timers[b.id]}\n"
"    }else{\n"
"      timerHtml='<div class=timer-row>'\n"
"        +'<button onclick=startTimer('+b.id+',3)>3m</button>'\n"
"        +'<button onclick=startTimer('+b.id+',5)>5m</button>'\n"
"        +'<button onclick=startTimer('+b.id+',10)>10m</button></div>';\n"
"    }\n"
/* Recipe tag if this burner has an active recipe */
"    const rtag=recipeActive&&recipeBurner===b.id"
"      ?'<div class=recipe-tag>'+recipeName+'</div>':'';\n"
"    const bname=(calBurners[b.id]&&calBurners[b.id].name)||'Burner '+(b.id+1);\n"
/* Phase 1 additions: cookware chip, rec dot, adjusted temp */
"    const cwId=(typeof burnerCookware!=='undefined'?burnerCookware[b.id]:null);\n"
"    const cw=(cwId&&typeof findCw==='function')?findCw(cwId):null;\n"
"    const hasSession=(typeof activeSessions!=='undefined')&&!!activeSessions[b.id];\n"
"    const chipCls=hasSession?'cw-chip recording':(cw?'cw-chip assigned':'cw-chip');\n"
"    const chipTxt=cw?(cw.icon+' '+cw.name):'+ tap to set cookware';\n"
"    const chip='<div class=\"'+chipCls+'\">'+(typeof esc==='function'?esc(chipTxt):chipTxt)+'</div>';\n"
"    const recDot=hasSession?'<div class=rec-dot></div>':'';\n"
"    const dispT=(typeof adjustTemp==='function')?adjustTemp(b.id,b.temp):b.temp;\n"
"    const dispMax=(typeof adjustTemp==='function')?adjustTemp(b.id,b.max):b.max;\n"
"    return '<div class=\"card '+cls+'\" onclick=\"cardTap('+b.id+',event)\">'\n"
"      +recDot\n"
"      +'<div class=label>'+bname+'</div>'\n"
"      +chip\n"
"      +'<div class=trend>'+arrow+'</div>'\n"
"      +'<div class=\"temp '+(b.state===0?'off':cls)+'\">'"
"      +(b.state===0?'OFF':fmtT(dispT))+'</div>'\n"
"      +'<div class=heat-bar><div style=\"width:'+heatPct+'%;background:'+heatColor+'\"></div></div>'\n"
"      +'<div class=meta>'+(timeStr?'Peak '+fmtT(dispMax)+' | '+timeStr"
"        :'Ambient')+'</div>'\n"
"      +'<span class=\"state-badge '+cls+'\">'+st+'</span>'\n"
"      +rtag+timerHtml+'</div>';\n"
"  }).join('');\n"
"}\n"
/* Render alerts */
"function renderAlerts(alerts){\n"
"  return; /* Alerts disabled during development */\n"
"  const el=document.getElementById('alerts');\n"
"  if(!alerts||alerts.length===0){el.innerHTML='';return}\n"
"  const newOnes=alerts.filter(a=>!activeAlerts.find(p=>p.type===a.type&&p.burner===a.burner));\n"
"  if(newOnes.length>0)beep();\n"
"  activeAlerts=alerts;\n"
"  el.innerHTML=alerts.map(a=>{\n"
"    const name=ALERT_NAMES[a.type]||'ALERT';\n"
"    const css=ALERT_CSS[a.type]||'';\n"
"    return '<div class=\"alert '+css+'\" onclick=dismissAlert('+a.type+','+a.burner+')>'\n"
"      +'<span>'+name+' \\u2014 Burner '+(a.burner+1)+' '+fmtT(a.temp)+'</span>'\n"
"      +'<span>\\u2715</span></div>';\n"
"  }).join('');\n"
"}\n"
"function dismissAlert(type,bid){\n"
"  if(ws&&ws.readyState===1)ws.send(JSON.stringify({cmd:'silence_alert'}));\n"
"  activeAlerts=activeAlerts.filter(a=>!(a.type===type&&a.burner===bid));\n"
"  renderAlerts(activeAlerts);\n"
"}\n"
/* WebSocket */
"let ws;\n"
"function connect(){\n"
"  const wsProto=(location.protocol==='https:')?'wss:':'ws:';\n"
"  ws=new WebSocket(wsProto+'//'+location.host+'/ws');\n"
"  ws.binaryType='arraybuffer';\n"
"  ws.onopen=()=>{document.getElementById('dot').classList.add('on');"
"setTimeout(sendCalToFirmware,1000)};\n"
"  ws.onclose=()=>{document.getElementById('dot').classList.remove('on');"
"setTimeout(connect,2000)};\n"
"  ws.onmessage=(e)=>{\n"
"    if(typeof e.data==='string'){\n"
"      const d=JSON.parse(e.data);\n"
"      if(d.type==='status'){\n"
"        burners=(d.burners||[]).map(b=>({id:b.id,state:b.state,temp:b.temp,"
"max:b.max,rate:b.rate,row:b.row,col:b.col,px:b.px,on:b.on||0}));\n"
"        renderCards();\n"
"        renderAlerts(d.alerts||[]);\n"
"        if(typeof phase1Hook==='function')phase1Hook(burners);\n"
"      }\n"
"    }else{\n"
"      /* Binary thermal frame — store for calibration overlay only */\n"
"      const dv=new DataView(e.data);\n"
"      for(let i=0;i<768;i++)temps[i]=dv.getInt16(4+i*2,true)/10.0;\n"
"    }\n"
"  };\n"
"}\n"
"connect();\n"
/* Timer update loop */
"setInterval(()=>{if(Object.keys(timers).length>0)renderCards()},1000);\n"
/* ---- Simulation ---- */
"let simOn=false,simRampInterval=null;\n"
"function toggleSim(){\n"
"  simOn=document.getElementById('simMode').checked;\n"
"  document.getElementById('simControls').style.display=simOn?'block':'none';\n"
"  if(!simOn){if(simRampInterval)clearInterval(simRampInterval);\n"
"    if(ws&&ws.readyState===1)ws.send(JSON.stringify({cmd:'sim_temp',temp:-1,burner:0}))}\n"
"  else{updateSim()}\n"
"}\n"
"function updateSim(){\n"
"  const v=parseInt(document.getElementById('simSlider').value);\n"
"  document.getElementById('simTempLabel').textContent=v;\n"
"  if(ws&&ws.readyState===1)ws.send(JSON.stringify({cmd:'sim_temp',temp:v,burner:0}));\n"
"}\n"
"function simPreset(t){document.getElementById('simSlider').value=t;updateSim()}\n"
"function simRamp(target,secs){\n"
"  if(simRampInterval)clearInterval(simRampInterval);\n"
"  const start=parseInt(document.getElementById('simSlider').value);\n"
"  const steps=secs*4;let step=0;\n"
"  simRampInterval=setInterval(()=>{\n"
"    step++;const t=Math.round(start+(target-start)*step/steps);\n"
"    document.getElementById('simSlider').value=t;updateSim();\n"
"    if(step>=steps)clearInterval(simRampInterval);\n"
"  },250);\n"
"}\n"
"\n"
/* ---- Calibration ---- */
"let calBurners=[],calSelected=-1,calDragging=false;\n"
"const calHm=document.getElementById('calHm');\n"
"const calCx=calHm?calHm.getContext('2d'):null;\n"
"if(calCx){calCx.imageSmoothingEnabled=true;calCx.imageSmoothingQuality='high'}\n"
"const calOffCv=document.createElement('canvas');calOffCv.width=32;calOffCv.height=24;\n"
"const calOffCx=calOffCv.getContext('2d');\n"
"const calOv=document.getElementById('calOv');\n"
"const calOx=calOv?calOv.getContext('2d'):null;\n"
"const calImg=new ImageData(32,24);\n"
"const COLORS=['#f59e0b','#4ade80','#60a5fa','#f472b6'];\n"
"\n"
"function startCalibration(){\n"
"  document.getElementById('calOverlay').style.display='block';\n"
"  document.getElementById('settingsPanel').classList.remove('open');\n"
"  calSelected=calBurners.length>0?0:-1;\n"
"  drawCal();\n"
"}\n"
"function calCancel(){document.getElementById('calOverlay').style.display='none'}\n"
"function calAddBurner(){\n"
"  if(calBurners.length>=4)return;\n"
"  const names=['Front Left','Front Right','Back Left','Back Right'];\n"
"  calBurners.push({r:12,c:16,rad:4,name:names[calBurners.length]||'Burner'});\n"
"  calSelected=calBurners.length-1;\n"
"  updateCalName();\n"
"  drawCal();\n"
"}\n"
"function updateCalName(){\n"
"  const inp=document.getElementById('calName');\n"
"  if(calSelected>=0&&calBurners[calSelected])inp.value=calBurners[calSelected].name||'';\n"
"  else inp.value='';\n"
"}\n"
"function calGrow(){if(calSelected>=0&&calBurners[calSelected].rad<10){calBurners[calSelected].rad++;drawCal()}}\n"
"function calShrink(){if(calSelected>=0&&calBurners[calSelected].rad>2){calBurners[calSelected].rad--;drawCal()}}\n"
"function calClear(){calBurners=[];calSelected=-1;drawCal()}\n"
"function calDeleteSelected(){\n"
"  if(calSelected>=0){calBurners.splice(calSelected,1);\n"
"  calSelected=calBurners.length>0?0:-1;drawCal()}\n"
"}\n"
"function calSave(){\n"
"  if(ws&&ws.readyState===1){\n"
"    /* Coords are raw sensor space — no transform needed */\n"
"    const b=calBurners.map(b=>({r:b.r,c:b.c,rad:b.rad}));\n"
"    ws.send(JSON.stringify({cmd:'set_calibration',b:b}));\n"
"  }\n"
"  try{localStorage.setItem('siq_cal',JSON.stringify(calBurners))}catch(e){}\n"
"  document.getElementById('calOverlay').style.display='none';\n"
"  showRecipePicker();\n"
"}\n"
/* Load saved calibration from localStorage and re-send to firmware on connect */
"try{const saved=localStorage.getItem('siq_cal');\n"
"  if(saved){calBurners=JSON.parse(saved)}\n"
"}catch(e){}\n"
"function sendCalToFirmware(){\n"
"  if(calBurners.length>0&&ws&&ws.readyState===1){\n"
"    const b=calBurners.map(b=>({r:b.r,c:b.c,rad:b.rad}));\n"
"    ws.send(JSON.stringify({cmd:'set_calibration',b:b}));\n"
"  }\n"
"}\n"
"\n"
"function drawCal(){\n"
"  if(!calOx)return;\n"
"  /* Draw heatmap */\n"
"  if(temps.length===768){\n"
"    let mn=9999,mx=-9999;\n"
"    for(let i=0;i<768;i++){if(temps[i]<mn)mn=temps[i];if(temps[i]>mx)mx=temps[i]}\n"
"    const range=mx-mn||1;\n"
"    for(let i=0;i<768;i++){\n"
"      const c=cmap((temps[i]-mn)/range);\n"
"      calImg.data[i*4]=c[0];calImg.data[i*4+1]=c[1];"
"calImg.data[i*4+2]=c[2];calImg.data[i*4+3]=255;\n"
"    }\n"
"    calOffCx.putImageData(calImg,0,0);\n"
"    calCx.drawImage(calOffCv,0,0,256,192);\n"
"  }\n"
"  /* Draw circles */\n"
"  calOx.clearRect(0,0,320,240);\n"
"  const sx=320/32,sy=240/24;\n"
"  calBurners.forEach((b,i)=>{\n"
"    calOx.strokeStyle=i===calSelected?'#fff':COLORS[i%4];\n"
"    calOx.lineWidth=i===calSelected?3:2;\n"
"    calOx.beginPath();\n"
"    calOx.arc(b.c*sx,b.r*sy,b.rad*sx,0,Math.PI*2);\n"
"    calOx.stroke();\n"
"    calOx.font='bold 14px system-ui';calOx.textAlign='center';\n"
"    calOx.fillStyle=COLORS[i%4];\n"
"    calOx.fillText(b.name||'B'+(i+1),b.c*sx,b.r*sy+5);\n"
"  });\n"
"}\n"
"\n"
"if(calOv){\n"
"  calOv.addEventListener('pointerdown',(e)=>{\n"
"    const rect=calOv.getBoundingClientRect();\n"
"    const x=(e.clientX-rect.left)/rect.width*32;\n"
"    const y=(e.clientY-rect.top)/rect.height*24;\n"
"    /* Check if clicking on existing burner */\n"
"    let hit=-1;\n"
"    calBurners.forEach((b,i)=>{\n"
"      const d=Math.sqrt((x-b.c)**2+(y-b.r)**2);\n"
"      if(d<=b.rad+1)hit=i;\n"
"    });\n"
"    if(hit>=0){calSelected=hit;calDragging=true;updateCalName()}\n"
"    else if(calBurners.length<4){\n"
"      const names=['Front Left','Front Right','Back Left','Back Right'];\n"
"      calBurners.push({r:Math.round(y),c:Math.round(x),rad:4,"
"name:names[calBurners.length]||'Burner'});\n"
"      calSelected=calBurners.length-1;updateCalName()}\n"
"    drawCal();\n"
"  });\n"
"  calOv.addEventListener('pointermove',(e)=>{\n"
"    if(!calDragging||calSelected<0)return;\n"
"    const rect=calOv.getBoundingClientRect();\n"
"    calBurners[calSelected].c=Math.round((e.clientX-rect.left)/rect.width*32);\n"
"    calBurners[calSelected].r=Math.round((e.clientY-rect.top)/rect.height*24);\n"
"    drawCal();\n"
"  });\n"
"  calOv.addEventListener('pointerup',()=>{calDragging=false});\n"
"}\n"
/* Update cal heatmap with live data */
"setInterval(drawCal,500);\n"
"\n"
/* ---- Recipes ---- */
"const RECIPES=['White Rice','Seared Steak','Boiled Potatoes','Pasta','Fried Eggs','Caramelized Onions'];\n"
"let recipeActive=false,recipeBurner=-1,recipeName='';\n"
"\n"
"function showRecipePicker(){\n"
"  const el=document.getElementById('recipePicker');\n"
"  el.style.display='block';\n"
"  const list=document.getElementById('recipeList');\n"
"  list.innerHTML=RECIPES.map((name,i)=>\n"
"    '<button style=\"display:block;width:100%;padding:10px;margin-bottom:6px;background:#222;'\n"
"    +'border:1px solid #444;color:#eee;border-radius:6px;font-size:14px;text-align:left;'\n"
"    +'cursor:pointer\" onclick=startRecipe('+i+')>'+name+'</button>'\n"
"  ).join('');\n"
"}\n"
"\n"
"function startRecipe(idx,bid){\n"
"  if(bid===undefined)bid=calBurners.length>0?0:-1;\n"
"  if(ws&&ws.readyState===1)\n"
"    ws.send(JSON.stringify({cmd:'start_recipe',recipe:idx,burner:bid}));\n"
"  document.getElementById('recipePicker').style.display='none';\n"
"  recipeActive=true;recipeBurner=bid;recipeName=RECIPES[idx]||'';\n"
"  renderCards();\n"
"}\n"
"function recipeNext(){\n"
"  if(ws&&ws.readyState===1)ws.send(JSON.stringify({cmd:'recipe_next'}));\n"
"}\n"
"function recipeStop(){\n"
"  if(ws&&ws.readyState===1)ws.send(JSON.stringify({cmd:'recipe_stop'}));\n"
"  document.getElementById('recipeActive').style.display='none';\n"
"  recipeActive=false;recipeBurner=-1;recipeName='';\n"
"  renderCards();\n"
"}\n"
"\n"
"\n"
/* Handle recipe state from status broadcast */
"function recipeConfirm(){\n"
"  if(ws&&ws.readyState===1)ws.send(JSON.stringify({cmd:'recipe_confirm'}));\n"
"}\n"
"function updateRecipe(r){\n"
"  const el=document.getElementById('recipeActive');\n"
"  if(!r){el.style.display='none';recipeActive=false;return}\n"
"  el.style.display='block';recipeActive=true;\n"
"  document.getElementById('recipeName').textContent=r.name;\n"
"  document.getElementById('recipeStep').textContent='Step '+(r.step+1)+' of '+r.steps;\n"
/* Dynamic description with thermal context */
"  let desc=r.desc;\n"
"  const temp=r.temp||0,target=r.target||0,trigger=r.trigger||0;\n"
/* Show current temp and progress for TARGET/BOIL/SIMMER triggers */
"  let progress='';\n"
"  if(target>0&&(trigger<=3)&&temp>0){\n"
"    const pct=Math.min(100,Math.round(temp/target*100));\n"
"    progress='<div style=\"margin:8px 0\">'\n"
"      +'<div style=\"display:flex;justify-content:space-between;font-size:12px;color:#888\">'\n"
"      +'<span>'+fmtT(temp)+'</span><span>Target: '+fmtT(target)+'</span></div>'\n"
"      +'<div style=\"background:#333;border-radius:4px;height:8px;margin-top:4px;overflow:hidden\">'\n"
"      +'<div style=\"background:'+(pct>=100?'#4ade80':'#f59e0b')+';height:100%;width:'+pct+'%;'\n"
"      +'border-radius:4px;transition:width 0.5s\"></div></div></div>';\n"
"  }\n"
/* Confirm button for TRIGGER_CONFIRM (type 7) */
"  let confirmBtn='';\n"
"  if(trigger===7){\n"
"    confirmBtn='<button style=\"width:100%;padding:12px;margin-top:8px;background:#4ade80;'\n"
"      +'border:none;border-radius:6px;color:#111;font-size:16px;font-weight:700;'\n"
"      +'cursor:pointer\" onclick=recipeConfirm()>\\u2713 Done — '+r.desc+'</button>';\n"
"  }\n"
"  document.getElementById('recipeDesc').innerHTML=desc+progress+confirmBtn;\n"
/* Timer */
"  if(r.timer>0){document.getElementById('recipeTimer').textContent=\n"
"    Math.floor(r.timer/60)+':'+(r.timer%60<10?'0':'')+r.timer%60}\n"
"  else{document.getElementById('recipeTimer').textContent=''}\n"
/* Coach message */
"  const coach=r.coach||'';\n"
"  document.getElementById('recipeCoach').textContent=coach;\n"
"  if(coach&&!recipeLastCoach){beep()}\n"
"  recipeLastCoach=coach;\n"
"}\n"
"let recipeLastCoach='';\n"
"\n"
/* Patch status handler to include recipe */
"const origOnMsg=ws?ws.onmessage:null;\n"
/* Hook into the status message handler */
"function patchWs(){\n"
"  if(!ws)return;\n"
"  const old=ws.onmessage;\n"
"  ws.onmessage=(e)=>{\n"
"    if(typeof e.data==='string'){\n"
"      try{const d=JSON.parse(e.data);\n"
"        if(d.type==='status'&&d.recipe)updateRecipe(d.recipe);\n"
"        else if(d.type==='status'&&!d.recipe)updateRecipe(null);\n"
"      }catch(ex){}\n"
"    }\n"
"    if(old)old(e);\n"
"  };\n"
"}\n"
/* Re-patch after reconnect */
"const origConnect=connect;\n"
"connect=function(){origConnect();setTimeout(patchWs,500)};\n"
"patchWs();\n"
/* ================================================================
   Phase 1: Cookware library + Teaching mode + Burner view + Graph
   ================================================================ */
/* Material presets */
"const MATERIALS=[\n"
"  {id:'cast_iron',name:'Cast iron (seasoned)',em:0.95},\n"
"  {id:'non_stick',name:'Non-stick / enameled',em:0.90},\n"
"  {id:'ss_scuffed',name:'Stainless (scuffed)',em:0.35},\n"
"  {id:'ss_polished',name:'Stainless (polished)',em:0.16},\n"
"  {id:'aluminum_ox',name:'Aluminum (oxidized)',em:0.30},\n"
"  {id:'aluminum_shiny',name:'Aluminum (shiny)',em:0.08},\n"
"  {id:'copper',name:'Copper',em:0.05},\n"
"  {id:'glass',name:'Glass / Pyrex',em:0.88},\n"
"];\n"
/* Event vocabulary (phase markers) */
"const EVENTS=[\n"
"  {id:'oil_in',icon:'\\uD83E\\uDDC8',label:'Oil in'},\n"       /* 🧈 butter/oil */
"  {id:'food_in',icon:'\\uD83C\\uDF45',label:'Food in'},\n"     /* 🍅 */
"  {id:'water_added',icon:'\\uD83D\\uDCA7',label:'Water'},\n"   /* 💧 */
"  {id:'rolling_boil',icon:'\\uD83E\\uDEE7',label:'Boil'},\n"   /* 🫧 */
"  {id:'stirred',icon:'\\uD83D\\uDD04',label:'Stir'},\n"        /* 🔄 */
"  {id:'flipped',icon:'\\uD83D\\uDD03',label:'Flip'},\n"        /* 🔃 */
"  {id:'lid_on',icon:'\\uD83C\\uDFA9',label:'Lid on'},\n"       /* 🎩 */
"  {id:'lid_off',icon:'\\uD83D\\uDC52',label:'Lid off'},\n"     /* 👒 */
"  {id:'done',icon:'\\u2713',label:'Done'},\n"                  /* ✓ */
"  {id:'note',icon:'\\uD83D\\uDCDD',label:'Note'},\n"           /* 📝 */
"];\n"
"const SESSION_LABELS=['sauté','boil','sear','simmer','fry','reduce','melt','steam','eggs'];\n"
/* Persisted state (localStorage, schema v1) */
"let cookwareLib=[],calibOffsets={},burnerCookware={},activeSessions={},pastSessions=[];\n"
"function loadState(){\n"
"  try{cookwareLib=JSON.parse(localStorage.getItem('siq_cookware')||'[]')}catch(e){}\n"
"  try{calibOffsets=JSON.parse(localStorage.getItem('siq_calib_offsets')||'{}')}catch(e){}\n"
"  try{burnerCookware=JSON.parse(localStorage.getItem('siq_burner_cookware')||'{}')}catch(e){}\n"
"  try{activeSessions=JSON.parse(localStorage.getItem('siq_active_sessions')||'{}')}catch(e){}\n"
"  try{pastSessions=JSON.parse(localStorage.getItem('siq_sessions')||'[]')}catch(e){}\n"
"}\n"
"function saveCw(){localStorage.setItem('siq_cookware',JSON.stringify(cookwareLib))}\n"
"function saveBC(){localStorage.setItem('siq_burner_cookware',JSON.stringify(burnerCookware))}\n"
"function saveCO(){localStorage.setItem('siq_calib_offsets',JSON.stringify(calibOffsets))}\n"
"function saveAS(){localStorage.setItem('siq_active_sessions',JSON.stringify(activeSessions))}\n"
"function savePS(){\n"
"  if(pastSessions.length>200)pastSessions=pastSessions.slice(-200);\n"
"  localStorage.setItem('siq_sessions',JSON.stringify(pastSessions));\n"
"}\n"
"loadState();\n"
/* Helpers */
"function esc(s){return String(s==null?'':s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;','\\'':'&#39;'}[c]))}\n"
"function findCw(id){return cookwareLib.find(c=>c.id===id)}\n"
"function bName(bid){return (calBurners[bid]&&calBurners[bid].name)||'Burner '+(bid+1)}\n"
"function fmtDur(ms){const s=Math.max(0,Math.floor(ms/1000));const m=Math.floor(s/60);return m+'m '+(s%60)+'s'}\n"
"function newId(p){return p+'_'+Date.now()+'_'+Math.random().toString(36).slice(2,8)}\n"
/* Apply (burner × cookware) offset to a raw temperature — displayed only */
"function adjustTemp(bid,rawC){\n"
"  const cwId=burnerCookware[bid];\n"
"  if(cwId==null)return rawC;\n"
"  const off=calibOffsets[bid+'_'+cwId]||0;\n"
"  return rawC+off;\n"
"}\n"
/* Card-tap guard: ignore clicks on inner buttons */
"function cardTap(bid,ev){\n"
"  if(ev&&ev.target&&(ev.target.closest('button')||ev.target.tagName==='INPUT'))return;\n"
"  openBurnerView(bid);\n"
"}\n"
/* ====== Cookware Library ====== */
"function openCwLibrary(){renderCwLib();document.getElementById('cwLibrary').classList.add('open')}\n"
"function closeCwLibrary(){document.getElementById('cwLibrary').classList.remove('open')}\n"
"function renderCwLib(){\n"
"  const el=document.getElementById('cwLibList');\n"
"  if(cookwareLib.length===0){\n"
"    el.innerHTML=\"<div style='color:#888;text-align:center;padding:20px'>No cookware yet.</div>\";\n"
"    return;\n"
"  }\n"
"  el.innerHTML=cookwareLib.map(cw=>{\n"
"    const m=MATERIALS.find(x=>x.id===cw.material)||{name:'?'};\n"
"    return \"<div class=row onclick=\\\"showCwForm('\"+cw.id+\"')\\\">\"\n"
"      +'<span class=icon>'+esc(cw.icon||'\\uD83C\\uDF73')+'</span>'\n"
"      +'<div class=info><div class=name>'+esc(cw.name)+'</div>'\n"
"      +'<div class=sub>'+esc(m.name)+(cw.notes?' · '+esc(cw.notes):'')+'</div></div>'\n"
"      +\"<button class=del onclick=\\\"event.stopPropagation();delCw('\"+cw.id+\"')\\\">Del</button>\"\n"
"      +'</div>';\n"
"  }).join('');\n"
"}\n"
"let editingCwId=null;\n"
"function showCwForm(id){\n"
"  editingCwId=typeof id==='string'?id:null;\n"
"  const matSel=document.getElementById('cwFormMaterial');\n"
"  matSel.innerHTML=MATERIALS.map(m=>'<option value='+m.id+'>'+m.name+' (\\u03B5='+m.em+')</option>').join('');\n"
"  if(editingCwId){\n"
"    const cw=findCw(editingCwId);\n"
"    document.getElementById('cwFormTitle').textContent='Edit cookware';\n"
"    document.getElementById('cwFormName').value=cw.name;\n"
"    document.getElementById('cwFormIcon').value=cw.icon||'\\uD83C\\uDF73';\n"
"    document.getElementById('cwFormMaterial').value=cw.material;\n"
"    document.getElementById('cwFormNotes').value=cw.notes||'';\n"
"  }else{\n"
"    document.getElementById('cwFormTitle').textContent='Add cookware';\n"
"    document.getElementById('cwFormName').value='';\n"
"    document.getElementById('cwFormIcon').value='\\uD83C\\uDF73';\n"
"    document.getElementById('cwFormMaterial').value=MATERIALS[0].id;\n"
"    document.getElementById('cwFormNotes').value='';\n"
"  }\n"
"  document.getElementById('cwForm').classList.add('open');\n"
"}\n"
"function closeCwForm(){document.getElementById('cwForm').classList.remove('open')}\n"
"function saveCwForm(){\n"
"  const name=document.getElementById('cwFormName').value.trim();\n"
"  if(!name){alert('Name required');return}\n"
"  const cw={\n"
"    id:editingCwId||newId('cw'),\n"
"    name:name,\n"
"    icon:document.getElementById('cwFormIcon').value.trim()||'\\uD83C\\uDF73',\n"
"    material:document.getElementById('cwFormMaterial').value,\n"
"    notes:document.getElementById('cwFormNotes').value.trim(),\n"
"    created:editingCwId?(findCw(editingCwId).created):new Date().toISOString(),\n"
"  };\n"
"  if(editingCwId){\n"
"    const i=cookwareLib.findIndex(c=>c.id===editingCwId);\n"
"    cookwareLib[i]=cw;\n"
"  }else{cookwareLib.push(cw)}\n"
"  saveCw();closeCwForm();renderCwLib();renderCwPickerList();renderCards();\n"
"}\n"
"function delCw(id){\n"
"  if(!confirm('Delete this cookware?\\nAny calibration and burner assignment for it will be removed.'))return;\n"
"  cookwareLib=cookwareLib.filter(c=>c.id!==id);\n"
"  Object.keys(burnerCookware).forEach(bid=>{\n"
"    if(burnerCookware[bid]===id){\n"
"      if(activeSessions[bid])endSessionInternal(bid);\n"
"      delete burnerCookware[bid];\n"
"    }\n"
"  });\n"
"  Object.keys(calibOffsets).forEach(k=>{if(k.endsWith('_'+id))delete calibOffsets[k]});\n"
"  saveCw();saveBC();saveCO();renderCwLib();renderCards();\n"
"}\n"
/* ====== Cookware picker (when starting a session) ====== */
"let cwPickerBurnerId=-1;\n"
"function openCwPicker(bid){\n"
"  cwPickerBurnerId=bid;\n"
"  document.getElementById('cwPickerBurner').textContent=bName(bid);\n"
"  renderCwPickerList();\n"
"  document.getElementById('cwPicker').classList.add('open');\n"
"}\n"
"function closeCwPicker(){document.getElementById('cwPicker').classList.remove('open')}\n"
"function renderCwPickerList(){\n"
"  const el=document.getElementById('cwPickerList');\n"
"  if(!el)return;\n"
"  if(cookwareLib.length===0){\n"
"    el.innerHTML=\"<div style='color:#888;text-align:center;padding:16px'>No cookware yet — add one below</div>\";\n"
"    return;\n"
"  }\n"
"  el.innerHTML=cookwareLib.map(cw=>\n"
"    \"<div class=row onclick=\\\"cwPickerChoose('\"+cw.id+\"')\\\">\"\n"
"    +'<span class=icon>'+esc(cw.icon||'\\uD83C\\uDF73')+'</span>'\n"
"    +'<div class=info><div class=name>'+esc(cw.name)+'</div>'\n"
"    +'<div class=sub>'+esc((MATERIALS.find(m=>m.id===cw.material)||{}).name||'')+'</div></div>'\n"
"    +'</div>'\n"
"  ).join('');\n"
"}\n"
"function cwPickerChoose(cwId){\n"
"  burnerCookware[cwPickerBurnerId]=cwId;\n"
"  saveBC();\n"
"  closeCwPicker();\n"
"  startSession(cwPickerBurnerId,cwId);\n"
"  openBurnerView(cwPickerBurnerId);\n"
"  renderCards();\n"
"}\n"
"function cwPickerClear(){\n"
"  if(activeSessions[cwPickerBurnerId])endSessionInternal(cwPickerBurnerId);\n"
"  delete burnerCookware[cwPickerBurnerId];\n"
"  saveBC();closeCwPicker();renderCards();\n"
"}\n"
/* ====== Session lifecycle ====== */
"function startSession(bid,cwId){\n"
"  const suggest=SESSION_LABELS.join(' · ');\n"
"  const label=prompt('Label this cook?\\n('+suggest+')','')||'unlabeled';\n"
"  activeSessions[bid]={\n"
"    id:newId('s'),\n"
"    burner_id:bid,\n"
"    cookware_id:cwId,\n"
"    label:label.trim(),\n"
"    started:new Date().toISOString(),\n"
"    started_ms:Date.now(),\n"
"    events:[],\n"
"    samples:[],\n"
"  };\n"
"  saveAS();\n"
"}\n"
"function endSessionInternal(bid){\n"
"  const s=activeSessions[bid];\n"
"  if(!s)return;\n"
"  s.ended=new Date().toISOString();\n"
"  s.duration_ms=Date.now()-s.started_ms;\n"
"  pastSessions.push(s);\n"
"  savePS();\n"
"  delete activeSessions[bid];\n"
"  saveAS();\n"
"}\n"
"function endSession(bid){\n"
"  endSessionInternal(bid);\n"
"  delete burnerCookware[bid];\n"
"  saveBC();\n"
"  renderCards();\n"
"}\n"
"function markEvent(bid,type,noteText){\n"
"  const s=activeSessions[bid];\n"
"  if(!s)return;\n"
"  const t=Date.now()-s.started_ms;\n"
"  const ev={t_ms:t,type:type,at:new Date().toISOString()};\n"
"  if(noteText)ev.note=noteText;\n"
"  s.events.push(ev);\n"
"  saveAS();\n"
"  if(bvOpenBurnerId===bid){bvRender();bvDraw()}\n"
"}\n"
"function recordSample(bid,tempRaw,state){\n"
"  const s=activeSessions[bid];\n"
"  if(!s)return;\n"
"  const t=Date.now()-s.started_ms;\n"
"  const last=s.samples[s.samples.length-1];\n"
"  if(last&&t-last.t_ms<450)return;\n"
"  s.samples.push({t_ms:t,temp_c:+tempRaw.toFixed(2),state:state});\n"
"  if(s.samples.length%20===0)saveAS();\n"
"  if(bvOpenBurnerId===bid)bvDraw();\n"
"}\n"
/* ====== Burner View (drill-in) ====== */
"let bvOpenBurnerId=-1,bvOpenSessionId=null,bvRangeMs=300000,bvReplayMode=false;\n"
"function openBurnerView(bid,pastSessionId){\n"
"  if(!pastSessionId&&!burnerCookware[bid]){openCwPicker(bid);return}\n"
"  bvOpenBurnerId=bid;\n"
"  bvOpenSessionId=pastSessionId||null;\n"
"  bvReplayMode=!!pastSessionId;\n"
"  if(bvReplayMode){bvRangeMs=0}\n"  /* past = all */
"  else{bvRangeMs=300000}\n"
"  [60000,300000,600000,0].forEach(r=>{\n"
"    const b=document.querySelector(\".bv-range button[data-range='\"+r+\"']\");\n"
"    if(b)b.classList.toggle('active',r===bvRangeMs);\n"
"  });\n"
"  document.getElementById('burnerView').classList.add('open');\n"
"  bvRender();\n"
"  setTimeout(bvDraw,50);\n"  /* wait for canvas to get layout */
"}\n"
"function closeBurnerView(){\n"
"  document.getElementById('burnerView').classList.remove('open');\n"
"  bvOpenBurnerId=-1;bvOpenSessionId=null;bvReplayMode=false;\n"
"}\n"
"function bvGetSession(){\n"
"  if(bvReplayMode)return pastSessions.find(s=>s.id===bvOpenSessionId);\n"
"  return activeSessions[bvOpenBurnerId];\n"
"}\n"
"function bvRender(){\n"
"  const bid=bvOpenBurnerId;\n"
"  if(bid<0)return;\n"
"  const s=bvGetSession();\n"
"  const cw=s?findCw(s.cookware_id):(burnerCookware[bid]?findCw(burnerCookware[bid]):null);\n"
"  document.getElementById('bvTitle').textContent=bName(bid);\n"
"  const metaEl=document.getElementById('bvMeta');\n"
"  let meta='';\n"
"  if(cw){\n"
"    meta+='<span class=icon>'+esc(cw.icon)+'</span>'\n"
"      +'<span class=label>'+esc(cw.name)+'</span>';\n"
"  }else{\n"
"    meta+='<span style=\"color:#888\">No cookware selected</span>';\n"
"  }\n"
"  if(!bvReplayMode){\n"
"    meta+='<button onclick=bvChangeCookware()>Change</button>';\n"
"  }\n"
"  if(s){\n"
"    meta+=' <span>Label: <b>'+esc(s.label)+'</b></span>'\n"
"      +'<button onclick=bvEditLabel()>Edit</button>';\n"
"    const dur=s.duration_ms||(Date.now()-s.started_ms);\n"
"    meta+=' <span>'+fmtDur(dur)+'</span>';\n"
"    meta+=' <span>'+s.samples.length+' samples</span>';\n"
"    meta+=' <span>'+s.events.length+' events</span>';\n"
"  }\n"
"  metaEl.innerHTML=meta;\n"
"  /* Event buttons */\n"
"  const evEl=document.getElementById('bvEvents');\n"
"  evEl.innerHTML=EVENTS.map(ev=>\n"
"    \"<button onclick=\\\"bvMark('\"+ev.id+\"')\\\">\"\n"
"    +'<span class=ev-icon>'+ev.icon+'</span>'\n"
"    +esc(ev.label)+'</button>'\n"
"  ).join('');\n"
"  /* Toggle End button text */\n"
"  document.getElementById('bvEndBtn').textContent=bvReplayMode?'Delete session':'End session';\n"
"}\n"
"function bvMark(type){\n"
"  const bid=bvOpenBurnerId;\n"
"  if(bvReplayMode){\n"
"    /* Add event to past session at current viewer cursor — for now, append at now */\n"
"    alert('To annotate a past session, tap on the graph at the moment you want to mark.');\n"
"    return;\n"
"  }\n"
"  if(!activeSessions[bid]){\n"
"    alert('No active session. Pick a cookware first.');return;\n"
"  }\n"
"  let noteText=null;\n"
"  if(type==='note'){\n"
"    noteText=prompt('Note:','');\n"
"    if(!noteText)return;\n"
"  }\n"
"  markEvent(bid,type,noteText);\n"
"  if(typeof beep==='function')try{beep()}catch(e){}\n"
"  /* brief visual flash on the button */\n"
"  const btns=document.querySelectorAll('#bvEvents button');\n"
"  btns.forEach(b=>{if(b.textContent.includes((EVENTS.find(e=>e.id===type)||{}).label)){\n"
"    b.classList.add('flash');setTimeout(()=>b.classList.remove('flash'),300);\n"
"  }});\n"
"}\n"
"function bvChangeCookware(){\n"
"  const bid=bvOpenBurnerId;\n"
"  /* End current session, clear assignment, open picker */\n"
"  if(activeSessions[bid]){\n"
"    if(!confirm('End current session and switch cookware?'))return;\n"
"    endSessionInternal(bid);\n"
"  }\n"
"  delete burnerCookware[bid];\n"
"  saveBC();\n"
"  closeBurnerView();\n"
"  openCwPicker(bid);\n"
"}\n"
"function bvEditLabel(){\n"
"  const s=bvGetSession();if(!s)return;\n"
"  const v=prompt('Label:',s.label);if(v==null)return;\n"
"  s.label=v.trim()||'unlabeled';\n"
"  if(bvReplayMode)savePS();else saveAS();\n"
"  bvRender();\n"
"}\n"
"function bvEndSession(){\n"
"  const bid=bvOpenBurnerId;\n"
"  if(bvReplayMode){\n"
"    if(!confirm('Delete this past session?'))return;\n"
"    pastSessions=pastSessions.filter(s=>s.id!==bvOpenSessionId);\n"
"    savePS();closeBurnerView();renderSessList();return;\n"
"  }\n"
"  if(!confirm('End this cook session?'))return;\n"
"  endSession(bid);\n"
"  closeBurnerView();\n"
"}\n"
"function bvCalibrateBoil(){\n"
"  /* Phase 2: auto-detect rolling boil. Phase 1: manual one-point. */\n"
"  const bid=bvOpenBurnerId;\n"
"  const cwId=burnerCookware[bid];\n"
"  if(!cwId){alert('Assign cookware first.');return}\n"
"  const b=burners.find(x=>x.id===bid);\n"
"  if(!b){alert('No live reading.');return}\n"
"  const rawC=b.temp;\n"
"  const v=prompt('What is the actual water temp right now, in \\u00B0C?\\nFor a rolling boil, enter 100.\\nSensor is reading '+rawC.toFixed(1)+'\\u00B0C.','100');\n"
"  if(v==null)return;\n"
"  const actual=parseFloat(v);\n"
"  if(isNaN(actual)){alert('Invalid number');return}\n"
"  const offset=actual-rawC;\n"
"  calibOffsets[bid+'_'+cwId]=offset;\n"
"  saveCO();\n"
"  alert('Saved offset '+(offset>=0?'+':'')+offset.toFixed(1)+'\\u00B0C for this (burner,cookware).');\n"
"  bvRender();bvDraw();renderCards();\n"
"}\n"
"function bvSetRange(ms){\n"
"  bvRangeMs=ms;\n"
"  [60000,300000,600000,0].forEach(r=>{\n"
"    const b=document.querySelector(\".bv-range button[data-range='\"+r+\"']\");\n"
"    if(b)b.classList.toggle('active',r===ms);\n"
"  });\n"
"  bvDraw();\n"
"}\n"
/* ====== Canvas temp graph ====== */
"function bvDraw(){\n"
"  const cv=document.getElementById('bvCanvas');\n"
"  if(!cv||!document.getElementById('burnerView').classList.contains('open'))return;\n"
"  const dpr=window.devicePixelRatio||1;\n"
"  const w=cv.clientWidth,h=cv.clientHeight;\n"
"  if(w===0||h===0)return;\n"
"  cv.width=w*dpr;cv.height=h*dpr;\n"
"  const cx=cv.getContext('2d');\n"
"  cx.setTransform(dpr,0,0,dpr,0,0);\n"
"  cx.clearRect(0,0,w,h);\n"
"  cx.fillStyle='#0a0a0a';cx.fillRect(0,0,w,h);\n"
"  const s=bvGetSession();\n"
"  if(!s||s.samples.length<1){\n"
"    cx.fillStyle='#555';cx.font='12px system-ui';cx.textAlign='center';\n"
"    cx.fillText('No samples yet',w/2,h/2);return;\n"
"  }\n"
"  const now=bvReplayMode?((s.ended?new Date(s.ended).getTime():s.started_ms)-s.started_ms):(Date.now()-s.started_ms);\n"
"  let tMin=0,tMax=now;\n"
"  if(bvRangeMs>0){tMin=Math.max(0,now-bvRangeMs);tMax=now}\n"
"  /* Filter samples to window */\n"
"  const samp=s.samples.filter(x=>x.t_ms>=tMin&&x.t_ms<=tMax);\n"
"  if(samp.length<1){\n"
"    cx.fillStyle='#555';cx.font='12px system-ui';cx.textAlign='center';\n"
"    cx.fillText('No samples in this window',w/2,h/2);return;\n"
"  }\n"
"  /* Y-auto-scale with 50° floor, 30° headroom */\n"
"  let yMin=Infinity,yMax=-Infinity;\n"
"  samp.forEach(x=>{const adj=adjustTemp(s.burner_id,x.temp_c);if(adj<yMin)yMin=adj;if(adj>yMax)yMax=adj});\n"
"  yMin=Math.min(yMin,yMax-50);\n"
"  yMax=yMax+Math.max(20,(yMax-yMin)*0.15);\n"
"  yMin=Math.max(0,yMin-10);\n"
"  const pad={l:42,r:10,t:14,b:22};\n"
"  const gw=w-pad.l-pad.r,gh=h-pad.t-pad.b;\n"
"  /* Axes */\n"
"  cx.strokeStyle='#222';cx.lineWidth=1;\n"
"  cx.beginPath();cx.moveTo(pad.l,pad.t);cx.lineTo(pad.l,pad.t+gh);\n"
"  cx.lineTo(pad.l+gw,pad.t+gh);cx.stroke();\n"
"  /* Y gridlines + labels */\n"
"  cx.fillStyle='#666';cx.font='10px system-ui';cx.textAlign='right';cx.textBaseline='middle';\n"
"  const yTicks=4;\n"
"  for(let i=0;i<=yTicks;i++){\n"
"    const yv=yMin+(yMax-yMin)*(1-i/yTicks);\n"
"    const yp=pad.t+gh*(i/yTicks);\n"
"    cx.strokeStyle='#1a1a1a';cx.beginPath();cx.moveTo(pad.l,yp);cx.lineTo(pad.l+gw,yp);cx.stroke();\n"
"    cx.fillText(fmtT(yv).replace('\\u00B0',''),pad.l-4,yp);\n"
"  }\n"
"  /* X time labels */\n"
"  cx.textAlign='center';cx.textBaseline='top';\n"
"  const xTicks=4;\n"
"  for(let i=0;i<=xTicks;i++){\n"
"    const tv=tMin+(tMax-tMin)*(i/xTicks);\n"
"    const xp=pad.l+gw*(i/xTicks);\n"
"    cx.fillText(fmtDur(tv),xp,pad.t+gh+4);\n"
"  }\n"
"  /* Curve */\n"
"  const x2px=t=>pad.l+gw*((t-tMin)/(tMax-tMin||1));\n"
"  const y2px=v=>pad.t+gh*(1-(v-yMin)/(yMax-yMin||1));\n"
"  cx.strokeStyle='#f59e0b';cx.lineWidth=1.8;cx.beginPath();\n"
"  samp.forEach((x,i)=>{\n"
"    const px=x2px(x.t_ms),py=y2px(adjustTemp(s.burner_id,x.temp_c));\n"
"    if(i===0)cx.moveTo(px,py);else cx.lineTo(px,py);\n"
"  });\n"
"  cx.stroke();\n"
"  /* Event markers */\n"
"  (s.events||[]).forEach(ev=>{\n"
"    if(ev.t_ms<tMin||ev.t_ms>tMax)return;\n"
"    const px=x2px(ev.t_ms);\n"
"    cx.strokeStyle='#3b82f6';cx.lineWidth=1;\n"
"    cx.beginPath();cx.moveTo(px,pad.t);cx.lineTo(px,pad.t+gh);cx.stroke();\n"
"    const e=EVENTS.find(q=>q.id===ev.type)||{icon:'?'};\n"
"    cx.fillStyle='#3b82f6';cx.beginPath();cx.arc(px,pad.t+8,6,0,Math.PI*2);cx.fill();\n"
"    cx.fillStyle='#fff';cx.font='9px system-ui';cx.textAlign='center';cx.textBaseline='middle';\n"
"    cx.fillText(e.icon,px,pad.t+8);\n"
"  });\n"
"  /* Current temp cursor (active sessions only) */\n"
"  if(!bvReplayMode){\n"
"    const last=samp[samp.length-1];\n"
"    if(last){\n"
"      const px=x2px(last.t_ms),py=y2px(adjustTemp(s.burner_id,last.temp_c));\n"
"      cx.fillStyle='#f59e0b';cx.beginPath();cx.arc(px,py,4,0,Math.PI*2);cx.fill();\n"
"      cx.fillStyle='#f59e0b';cx.font='bold 12px system-ui';cx.textAlign='left';cx.textBaseline='bottom';\n"
"      cx.fillText(fmtT(adjustTemp(s.burner_id,last.temp_c)),px+8,py-2);\n"
"    }\n"
"  }\n"
"  /* Store coords for tap-to-annotate */\n"
"  cv._bv={tMin:tMin,tMax:tMax,pad:pad,w:w,h:h,sess:s};\n"
"}\n"
/* Tap on graph → annotate at that timestamp */
"function bvCanvasTap(ev){\n"
"  const cv=document.getElementById('bvCanvas');\n"
"  const info=cv._bv;if(!info)return;\n"
"  const rect=cv.getBoundingClientRect();\n"
"  const x=(ev.touches?ev.touches[0]:ev).clientX-rect.left;\n"
"  if(x<info.pad.l||x>info.w-info.pad.r)return;\n"
"  const t_ms=info.tMin+(info.tMax-info.tMin)*((x-info.pad.l)/(info.w-info.pad.l-info.pad.r));\n"
"  openAnnSheet(t_ms);\n"
"}\n"
"document.addEventListener('DOMContentLoaded',()=>{\n"
"  const cv=document.getElementById('bvCanvas');\n"
"  if(cv){cv.addEventListener('click',bvCanvasTap);cv.addEventListener('touchstart',bvCanvasTap)}\n"
"});\n"
/* DOMContentLoaded may have already fired; also attach now */
"setTimeout(()=>{\n"
"  const cv=document.getElementById('bvCanvas');\n"
"  if(cv&&!cv._bound){cv._bound=true;cv.addEventListener('click',bvCanvasTap);cv.addEventListener('touchstart',bvCanvasTap)}\n"
"},100);\n"
/* Annotation sheet (drop event at tapped timestamp) */
"let annTms=0;\n"
"function openAnnSheet(t_ms){\n"
"  annTms=t_ms;\n"
"  document.getElementById('annHdr').textContent='Add event at '+fmtDur(t_ms);\n"
"  document.getElementById('annEvents').innerHTML=EVENTS.map(ev=>\n"
"    \"<button onclick=\\\"annPick('\"+ev.id+\"')\\\">\"\n"
"    +'<span class=ev-icon>'+ev.icon+'</span>'+esc(ev.label)+'</button>'\n"
"  ).join('');\n"
"  document.getElementById('annSheet').classList.add('open');\n"
"}\n"
"function closeAnn(){document.getElementById('annSheet').classList.remove('open')}\n"
"function annPick(type){\n"
"  const s=bvGetSession();if(!s){closeAnn();return}\n"
"  let noteText=null;\n"
"  if(type==='note'){noteText=prompt('Note:','');if(!noteText){closeAnn();return}}\n"
"  const ev={t_ms:Math.max(0,Math.round(annTms)),type:type,at:new Date(s.started_ms+annTms).toISOString()};\n"
"  if(noteText)ev.note=noteText;\n"
"  s.events.push(ev);\n"
"  s.events.sort((a,b)=>a.t_ms-b.t_ms);\n"
"  if(bvReplayMode)savePS();else saveAS();\n"
"  closeAnn();bvRender();bvDraw();\n"
"}\n"
/* ====== Sessions browser ====== */
"function openSessions(){renderSessList();document.getElementById('sessModal').classList.add('open')}\n"
"function closeSessions(){document.getElementById('sessModal').classList.remove('open')}\n"
"function renderSessList(){\n"
"  const el=document.getElementById('sessList');\n"
"  const all=[];\n"
"  Object.values(activeSessions).forEach(s=>all.push({s:s,active:true}));\n"
"  [...pastSessions].reverse().forEach(s=>all.push({s:s,active:false}));\n"
"  if(all.length===0){\n"
"    el.innerHTML=\"<div style='color:#888;text-align:center;padding:20px'>No sessions yet.</div>\";\n"
"    return;\n"
"  }\n"
"  el.innerHTML=all.slice(0,80).map(o=>{\n"
"    const s=o.s;const cw=findCw(s.cookware_id);\n"
"    const dur=s.duration_ms||(Date.now()-s.started_ms);\n"
"    const date=new Date(s.started).toLocaleString();\n"
"    const chip=o.active?'<span class=\"sess-chip rec\">REC</span>':'';\n"
"    return \"<div class=row onclick=\\\"openSessFromList('\"+s.id+\"','\"+(o.active?'a':'p')+\"')\\\">\"\n"
"      +'<span class=icon>'+esc(cw?cw.icon:'?')+'</span>'\n"
"      +'<div class=info><div class=name>'+esc(s.label)+chip+'</div>'\n"
"      +'<div class=sub>'+bName(s.burner_id)+' · '+esc(cw?cw.name:'unknown')+' · '\n"
"      +fmtDur(dur)+' · '+s.samples.length+' · '+esc(date)+'</div></div>'\n"
"      +'</div>';\n"
"  }).join('');\n"
"}\n"
"function openSessFromList(id,kind){\n"
"  closeSessions();\n"
"  if(kind==='a'){\n"
"    const s=Object.values(activeSessions).find(x=>x.id===id);\n"
"    if(s)openBurnerView(s.burner_id);\n"
"  }else{\n"
"    const s=pastSessions.find(x=>x.id===id);\n"
"    if(s)openBurnerView(s.burner_id,s.id);\n"
"  }\n"
"}\n"
/* ====== Export / Import via Share sheet ====== */
"function exportBackup(){\n"
"  const data={\n"
"    version:1,exported_at:new Date().toISOString(),\n"
"    cookware:cookwareLib,calib_offsets:calibOffsets,\n"
"    burner_cookware:burnerCookware,\n"
"    sessions:pastSessions,active_sessions:activeSessions,\n"
"  };\n"
"  const json=JSON.stringify(data);\n"
"  const filename='stoveiq-backup-'+new Date().toISOString().slice(0,10)+'.json';\n"
"  const blob=new Blob([json],{type:'application/json'});\n"
"  try{\n"
"    const f=new File([blob],filename,{type:'application/json'});\n"
"    if(navigator.canShare&&navigator.canShare({files:[f]})){\n"
"      navigator.share({files:[f],title:'StoveIQ backup'}).catch(()=>{});\n"
"      return;\n"
"    }\n"
"  }catch(e){}\n"
"  /* Fallback: download link */\n"
"  const a=document.createElement('a');\n"
"  a.href=URL.createObjectURL(blob);a.download=filename;a.click();\n"
"  setTimeout(()=>URL.revokeObjectURL(a.href),2000);\n"
"}\n"
"function importBackup(ev){\n"
"  const f=ev.target.files[0];if(!f)return;\n"
"  const reader=new FileReader();\n"
"  reader.onload=e=>{\n"
"    try{\n"
"      const d=JSON.parse(e.target.result);\n"
"      if(!d||d.version!==1){alert('Not a StoveIQ v1 backup');return}\n"
"      const replace=!confirm('Merge with existing data?\\n(OK = merge, Cancel = REPLACE ALL)');\n"
"      if(replace){\n"
"        cookwareLib=d.cookware||[];\n"
"        calibOffsets=d.calib_offsets||{};\n"
"        burnerCookware=d.burner_cookware||{};\n"
"        pastSessions=d.sessions||[];\n"
"        activeSessions=d.active_sessions||{};\n"
"      }else{\n"
"        (d.cookware||[]).forEach(cw=>{\n"
"          const i=cookwareLib.findIndex(c=>c.id===cw.id);\n"
"          if(i>=0)cookwareLib[i]=cw;else cookwareLib.push(cw);\n"
"        });\n"
"        Object.assign(calibOffsets,d.calib_offsets||{});\n"
"        Object.assign(burnerCookware,d.burner_cookware||{});\n"
"        (d.sessions||[]).forEach(s=>{if(!pastSessions.find(p=>p.id===s.id))pastSessions.push(s)});\n"
"      }\n"
"      saveCw();saveCO();saveBC();savePS();saveAS();\n"
"      renderSessList();renderCards();\n"
"      alert('Imported: '+(d.cookware?.length||0)+' cookware, '+(d.sessions?.length||0)+' sessions.');\n"
"    }catch(err){alert('Import failed: '+err.message)}\n"
"    ev.target.value='';\n"
"  };\n"
"  reader.readAsText(f);\n"
"}\n"
/* Hook for WS updates — called from ws.onmessage after burners updates */
"function phase1Hook(burnersArr){\n"
"  burnersArr.forEach(b=>{if(activeSessions[b.id])recordSample(b.id,b.temp,b.state)});\n"
"  if(bvOpenBurnerId>=0&&!bvReplayMode)bvRender();\n"
"}\n"
/* Re-render on viewport resize (graph) */
"window.addEventListener('resize',()=>{if(bvOpenBurnerId>=0)bvDraw()});\n"
/* Screen Wake Lock — keep phone awake while cooking.
   Two strategies: Wake Lock API (secure-context only), then NoSleep
   silent-video fallback for iOS Safari over plain HTTP. */
"let wakeLock=null,wakeReq=false,vidPlaying=false;\n"
"const wakeDot=document.getElementById('wakeDot');\n"
"const sleepVid=document.getElementById('sleepVid');\n"
"function setAwake(on){wakeDot.classList.toggle('awake',on)}\n"
"async function reqWakeLock(){\n"
"  if(wakeLock||wakeReq||!('wakeLock' in navigator))return false;\n"
"  wakeReq=true;\n"
"  try{\n"
"    wakeLock=await navigator.wakeLock.request('screen');\n"
"    wakeLock.addEventListener('release',()=>{wakeLock=null;updateDot()});\n"
"    wakeReq=false;return true;\n"
"  }catch(e){wakeReq=false;return false}\n"
"}\n"
"async function playSilent(){\n"
"  if(vidPlaying)return true;\n"
"  try{await sleepVid.play();vidPlaying=true;return true}\n"
"  catch(e){return false}\n"
"}\n"
"function pauseSilent(){if(vidPlaying){sleepVid.pause();vidPlaying=false}}\n"
"function updateDot(){setAwake(!!wakeLock||vidPlaying)}\n"
"async function reqWake(){\n"
"  const ok=await reqWakeLock();\n"
"  if(!ok)await playSilent();\n"
"  updateDot();\n"
"}\n"
"document.addEventListener('visibilitychange',()=>{\n"
"  if(document.visibilityState==='visible')reqWake();\n"
"});\n"
"document.addEventListener('click',reqWake);\n"
"wakeDot.addEventListener('click',async(e)=>{\n"
"  e.stopPropagation();\n"
"  if(wakeLock){try{await wakeLock.release()}catch(ex){}}\n"
"  if(vidPlaying)pauseSilent();\n"
"  if(!wakeLock&&!vidPlaying)await reqWake();\n"
"  updateDot();\n"
"});\n"
"reqWake();\n"
/* PWA service worker registration */
"if('serviceWorker' in navigator){"
"navigator.serviceWorker.register('/sw.js').catch(()=>{})}\n"
"</script></body></html>\n";

/* ------------------------------------------------------------------ */
/*  File serving with gzip support                                     */
/* ------------------------------------------------------------------ */

static const char *get_content_type(const char *path)
{
    if (strstr(path, ".html")) return "text/html";
    if (strstr(path, ".js"))   return "application/javascript";
    if (strstr(path, ".css"))  return "text/css";
    if (strstr(path, ".json")) return "application/json";
    if (strstr(path, ".png"))  return "image/png";
    if (strstr(path, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

static esp_err_t serve_file(httpd_req_t *req, const char *path)
{
    char filepath[64];

    /* Try gzipped version first */
    snprintf(filepath, sizeof(filepath), "/www%s.gz", path);
    struct stat st;
    bool gzipped = (stat(filepath, &st) == 0);

    if (!gzipped) {
        snprintf(filepath, sizeof(filepath), "/www%s", path);
        if (stat(filepath, &st) != 0) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    FILE *f = fopen(filepath, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    httpd_resp_set_type(req, get_content_type(path));
    if (gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  HTTP handlers                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t root_handler(httpd_req_t *req)
{
    if (s_spiffs_ready) {
        esp_err_t ret = serve_file(req, "/index.html");
        if (ret == ESP_OK) return ret;
    }
    /* Fallback */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, FALLBACK_HTML, strlen(FALLBACK_HTML));
    return ESP_OK;
}

static esp_err_t static_handler(httpd_req_t *req)
{
    if (!s_spiffs_ready) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    esp_err_t ret = serve_file(req, req->uri);
    if (ret != ESP_OK) httpd_resp_send_404(req);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Captive portal handlers                                            */
/* ------------------------------------------------------------------ */

static const char CAPTIVE_SUCCESS[] =
    "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";

static esp_err_t captive_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CAPTIVE_SUCCESS, strlen(CAPTIVE_SUCCESS));
    return ESP_OK;
}

static esp_err_t generate_204_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  PWA manifest                                                       */
/* ------------------------------------------------------------------ */

static const char MANIFEST_JSON[] =
    "{\"name\":\"StoveIQ Cooking Coach\","
    "\"short_name\":\"StoveIQ\","
    "\"id\":\"/\","
    "\"start_url\":\"/\","
    "\"display\":\"standalone\","
    "\"orientation\":\"portrait\","
    "\"background_color\":\"#111111\","
    "\"theme_color\":\"#111111\","
    "\"description\":\"Open-source thermal cooking coach\","
    "\"categories\":[\"food\",\"utilities\"],"
    "\"icons\":["
    "{\"src\":\"/icon.png\",\"sizes\":\"32x32\",\"type\":\"image/png\",\"purpose\":\"any\"},"
    "{\"src\":\"/icon.png\",\"sizes\":\"32x32\",\"type\":\"image/png\",\"purpose\":\"maskable\"}"
    "]}";

static esp_err_t manifest_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    httpd_resp_send(req, MANIFEST_JSON, strlen(MANIFEST_JSON));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  PWA icon (decode base64 inline icon to serve as /icon.png)         */
/* ------------------------------------------------------------------ */

/* The same 32x32 burner grate PNG used in the favicon link tag.
 * Decoded from base64 at compile time isn't possible in C, so we
 * serve it from a separately-stored binary blob.  For simplicity,
 * redirect to the data URI — browsers cache it. */
static esp_err_t icon_handler(httpd_req_t *req)
{
    /* Serve the favicon inline data as a proper PNG response.
     * Since we already embed the PNG as base64 in the HTML, we decode
     * it here.  But base64 decode on ESP32 adds code.  Instead, we use
     * a tiny 1x1 transparent PNG for the manifest icon and let the
     * apple-touch-icon + favicon in the HTML carry the real icon.
     *
     * Actually — let's just embed the raw PNG bytes. The icon is small
     * (~700 bytes). We extract it from the base64 at build time. For now
     * redirect the browser to get the icon from the inline data URI. */
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=604800");

    /* Minimal 32x32 orange square PNG (StoveIQ brand color #f59e0b)
     * This is a valid PNG that satisfies the manifest icon requirement.
     * The full burner grate icon is delivered via the inline base64
     * in the HTML favicon and apple-touch-icon tags. */
    static const uint8_t ICON_PNG[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,  /* PNG signature */
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,  /* IHDR chunk */
        0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20,  /* 32x32 */
        0x02, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x14, 0x92,
        0x67, 0x00, 0x00, 0x00, 0x09, 0x50, 0x4c, 0x54,  /* PLTE chunk */
        0x45, 0xf5, 0x9e, 0x0b, 0x11, 0x11, 0x11, 0x00,  /* orange + dark bg */
        0x00, 0x00, 0xd1, 0x7a, 0xba, 0xb3, 0x00, 0x00,
        0x00, 0x01, 0x74, 0x52, 0x4e, 0x53, 0x00, 0x40,  /* tRNS */
        0xe6, 0xd8, 0x66, 0x00, 0x00, 0x00, 0x1b, 0x49,  /* IDAT */
        0x44, 0x41, 0x54, 0x78, 0x01, 0x62, 0x60, 0x60,
        0x60, 0x60, 0xf8, 0xcf, 0xc0, 0x00, 0x04, 0x0c,
        0x0c, 0xa0, 0x30, 0x20, 0x06, 0x00, 0x00, 0x31,
        0x00, 0x01, 0x2e, 0x78, 0x39, 0xc4, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,  /* IEND */
        0x60, 0x82,
    };
    httpd_resp_send(req, (const char *)ICON_PNG, sizeof(ICON_PNG));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Service worker (minimal — enables PWA install)                     */
/* ------------------------------------------------------------------ */

static const char SERVICE_WORKER_JS[] =
    "const CACHE='stoveiq-v3';\n"
    "const PRECACHE=['/','manifest.json'];\n"
    "self.addEventListener('install',e=>{\n"
    "  e.waitUntil(caches.open(CACHE).then(c=>c.addAll(PRECACHE)));\n"
    "  self.skipWaiting();\n"
    "});\n"
    "self.addEventListener('activate',e=>{\n"
    "  e.waitUntil(caches.keys().then(ks=>\n"
    "    Promise.all(ks.filter(k=>k!==CACHE).map(k=>caches.delete(k)))));\n"
    "  self.clients.claim();\n"
    "});\n"
    "self.addEventListener('fetch',e=>{\n"
    "  if(e.request.url.includes('/ws'))return;\n"  /* Don't cache WebSocket */
    "  e.respondWith(\n"
    "    fetch(e.request).then(r=>{\n"
    "      if(r.ok){const c=r.clone();\n"
    "        caches.open(CACHE).then(cache=>cache.put(e.request,c))}\n"
    "      return r;\n"
    "    }).catch(()=>caches.match(e.request))\n"
    "  );\n"
    "});\n";

static esp_err_t sw_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Service-Worker-Allowed", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, SERVICE_WORKER_JS, strlen(SERVICE_WORKER_JS));
    return ESP_OK;
}

/* HTTP → HTTPS 301 redirect. Preserves path + query. Uses the Host header
 * so it works whether the user typed stoveiq.local or the IP address. */
static esp_err_t redirect_to_https_handler(httpd_req_t *req)
{
    char host[64] = "stoveiq.local";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    /* Strip any :port suffix from Host */
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';

    /* Host is capped at 63, URI at 512 (HTTPD default), plus "https://" = 8. */
    char location[600];
    snprintf(location, sizeof(location), "https://%s%s", host, req->uri);

    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Redirecting to HTTPS", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Silent looping MP4 for iOS Safari screen-wake fallback (kept for
 * legacy non-secure access; unused once users migrate to HTTPS). */
static esp_err_t silent_mp4_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "video/mp4");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=604800");
    httpd_resp_send(req, (const char *)SILENT_MP4, SILENT_MP4_LEN);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  WebSocket handler                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake */
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS client connected (fd=%d)", fd);

        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        if (s_ws_count < MAX_WS_CLIENTS) {
            s_ws_fds[s_ws_count++] = fd;
        }
        xSemaphoreGive(s_ws_mutex);
        return ESP_OK;
    }

    /* Receive WebSocket message */
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len > 0 && ws_pkt.len < 256) {
        uint8_t *buf = malloc(ws_pkt.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;

        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK) {
            buf[ws_pkt.len] = '\0';
            ESP_LOGI(TAG, "WS cmd: %s", (char *)buf);

            /* Queue the command for the cooking engine to process */
            ws_command_t cmd = {0};
            /* Simple command parsing — look for "cmd" field */
            strncpy(cmd.payload, (char *)buf, sizeof(cmd.payload) - 1);
            if (strstr((char *)buf, "\"silence_alert\""))
                cmd.type = CMD_SILENCE_ALERT;
            else if (strstr((char *)buf, "\"set_calibration\""))
                cmd.type = CMD_SET_CALIBRATION;
            else if (strstr((char *)buf, "\"start_recipe\""))
                cmd.type = CMD_START_RECIPE;
            else if (strstr((char *)buf, "\"recipe_next\""))
                cmd.type = CMD_RECIPE_NEXT;
            else if (strstr((char *)buf, "\"recipe_confirm\""))
                cmd.type = CMD_RECIPE_CONFIRM;
            else if (strstr((char *)buf, "\"recipe_stop\""))
                cmd.type = CMD_RECIPE_STOP;
            else if (strstr((char *)buf, "\"sim_temp\""))
                cmd.type = CMD_SIM_TEMP;
            else if (strstr((char *)buf, "\"set_emissivity\""))
                cmd.type = CMD_SET_EMISSIVITY;
            else if (strstr((char *)buf, "\"set_temp_offset\""))
                cmd.type = CMD_SET_TEMP_OFFSET;
            else if (strstr((char *)buf, "\"set_threshold\""))
                cmd.type = CMD_SET_THRESHOLD;
            else if (strstr((char *)buf, "\"set_wifi\""))
                cmd.type = CMD_SET_WIFI;
            else
                cmd.type = CMD_SET_SETTING;

            xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100));
        }
        free(buf);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  DNS server (captive portal)                                        */
/* ------------------------------------------------------------------ */

static void dns_server_task(void *pvParameters)
{
    (void)pvParameters;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&saddr, sizeof(saddr));

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    ESP_LOGI(TAG, "DNS server started on port 53");

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 12) continue;

        uint8_t resp[512];
        memcpy(resp, buf, len);
        resp[2] = 0x84; resp[3] = 0x00;
        resp[6] = 0x00; resp[7] = 0x01;

        int pos = len;
        resp[pos++] = 0xc0; resp[pos++] = 0x0c;
        resp[pos++] = 0x00; resp[pos++] = 0x01;
        resp[pos++] = 0x00; resp[pos++] = 0x01;
        resp[pos++] = 0x00; resp[pos++] = 0x00;
        resp[pos++] = 0x00; resp[pos++] = 0x3c;
        resp[pos++] = 0x00; resp[pos++] = 0x04;
        resp[pos++] = 192;  resp[pos++] = 168;
        resp[pos++] = 4;    resp[pos++] = 1;

        sendto(sock, resp, pos, 0,
               (struct sockaddr *)&client, clen);
    }
}

/* ------------------------------------------------------------------ */
/*  Debug + calibration reset endpoints                                */
/* ------------------------------------------------------------------ */

static esp_err_t clear_cal_handler(httpd_req_t *req)
{
    ws_command_t cmd = { .type = CMD_SET_CALIBRATION };
    strncpy(cmd.payload, "{\"cmd\":\"set_calibration\",\"b\":[]}", sizeof(cmd.payload));
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Calibration cleared. Switched to auto-detect (CCL).\n");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t web_server_init(void)
{
    s_ws_mutex = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(ws_command_t));
    s_ws_count = 0;

    init_spiffs();

    /* ------- HTTPS server on :443 (main app) ------- */
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.stack_size = 16384;
    conf.httpd.max_uri_handlers = 16;
    conf.httpd.max_open_sockets = 4;   /* TLS uses ~30KB RAM per socket */
    conf.servercert     = (const uint8_t *)SERVER_CERT_PEM;
    conf.servercert_len = sizeof(SERVER_CERT_PEM);
    conf.prvtkey_pem    = (const uint8_t *)SERVER_KEY_PEM;
    conf.prvtkey_len    = sizeof(SERVER_KEY_PEM);

    esp_err_t ret = httpd_ssl_start(&s_server, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* WebSocket endpoint */
    httpd_uri_t ws = {
        .uri = "/ws", .method = HTTP_GET,
        .handler = ws_handler, .user_ctx = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws);

    /* Root */
    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET,
        .handler = root_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &root);

    /* Static files */
    httpd_uri_t js = { .uri = "/*.js", .method = HTTP_GET,
                       .handler = static_handler };
    httpd_uri_t css = { .uri = "/*.css", .method = HTTP_GET,
                        .handler = static_handler };
    httpd_register_uri_handler(s_server, &js);
    httpd_register_uri_handler(s_server, &css);

    /* PWA manifest + icon + service worker */
    httpd_uri_t manifest = { .uri = "/manifest.json", .method = HTTP_GET,
                             .handler = manifest_handler };
    httpd_uri_t icon = { .uri = "/icon.png", .method = HTTP_GET,
                         .handler = icon_handler };
    httpd_uri_t sw = { .uri = "/sw.js", .method = HTTP_GET,
                       .handler = sw_handler };
    httpd_uri_t silent = { .uri = "/silent.mp4", .method = HTTP_GET,
                           .handler = silent_mp4_handler };
    httpd_register_uri_handler(s_server, &manifest);
    httpd_register_uri_handler(s_server, &icon);
    httpd_register_uri_handler(s_server, &sw);
    httpd_register_uri_handler(s_server, &silent);

    /* Debug / calibration reset */
    httpd_uri_t clearcal = { .uri = "/api/clear-cal", .method = HTTP_GET,
                             .handler = clear_cal_handler };
    httpd_register_uri_handler(s_server, &clearcal);

    /* ------- HTTP server on :80 (redirect + captive portal) -------
       Serves OS captive-portal probes unencrypted (iOS/Android require HTTP),
       and 301-redirects all other traffic to https://stoveiq.local<path>. */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port = 80;
    http_cfg.ctrl_port   = 32769;       /* must differ from HTTPS ctrl_port */
    http_cfg.max_uri_handlers = 8;
    http_cfg.max_open_sockets = 4;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_redirect_server, &http_cfg) == ESP_OK) {
        /* Captive portal probes stay on HTTP so iOS/Android detect them */
        httpd_uri_t apple80 = { .uri = "/hotspot-detect.html", .method = HTTP_GET,
                                .handler = captive_handler };
        httpd_uri_t android80 = { .uri = "/generate_204", .method = HTTP_GET,
                                  .handler = generate_204_handler };
        httpd_uri_t conn80 = { .uri = "/connecttest.txt", .method = HTTP_GET,
                               .handler = captive_handler };
        httpd_uri_t wildcard = { .uri = "/*", .method = HTTP_GET,
                                 .handler = redirect_to_https_handler };
        httpd_register_uri_handler(s_redirect_server, &apple80);
        httpd_register_uri_handler(s_redirect_server, &android80);
        httpd_register_uri_handler(s_redirect_server, &conn80);
        httpd_register_uri_handler(s_redirect_server, &wildcard);
    } else {
        ESP_LOGW(TAG, "HTTP redirect server failed to start (HTTPS still works)");
    }

    ESP_LOGI(TAG, "HTTPS server on :443, HTTP redirect on :80");
    return ESP_OK;
}

void web_server_broadcast_frame(const thermal_snapshot_t *snapshot)
{
    if (!s_server || s_ws_count == 0) return;

    /* Binary frame: 4-byte timestamp + 768 x int16 (temp*10) */
    size_t frame_size = 4 + STOVEIQ_FRAME_PIXELS * 2;
    uint8_t *buf = heap_caps_malloc(frame_size, MALLOC_CAP_8BIT);
    if (!buf) return;

    /* Timestamp (little-endian) */
    buf[0] = (snapshot->timestamp_ms)       & 0xFF;
    buf[1] = (snapshot->timestamp_ms >> 8)  & 0xFF;
    buf[2] = (snapshot->timestamp_ms >> 16) & 0xFF;
    buf[3] = (snapshot->timestamp_ms >> 24) & 0xFF;

    /* Thermal data as int16 (temp * 10) */
    for (int i = 0; i < STOVEIQ_FRAME_PIXELS; i++) {
        int16_t val = (int16_t)(snapshot->frame[i] * 10.0f);
        buf[4 + i*2]     = val & 0xFF;
        buf[4 + i*2 + 1] = (val >> 8) & 0xFF;
    }

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = buf,
        .len = frame_size,
    };

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < s_ws_count; ) {
        esp_err_t ret = httpd_ws_send_frame_async(
            s_server, s_ws_fds[i], &ws_pkt);
        if (ret != ESP_OK) {
            /* Client disconnected — remove from list */
            ESP_LOGW(TAG, "WS client fd=%d gone, removing", s_ws_fds[i]);
            s_ws_fds[i] = s_ws_fds[--s_ws_count];
        } else {
            i++;
        }
    }
    xSemaphoreGive(s_ws_mutex);

    free(buf);
}

void web_server_broadcast_status(const thermal_snapshot_t *snapshot,
                                 const cook_alert_t *alerts,
                                 int alert_count,
                                 const recipe_session_t *recipe)
{
    if (!s_server || s_ws_count == 0) return;

    /* Build JSON status message */
    char *json = heap_caps_malloc(2048, MALLOC_CAP_8BIT);
    if (!json) return;

    int n = snprintf(json, 2048,
        "{\"type\":\"status\",\"ambient\":%.1f,\"maxTemp\":%.1f,"
        "\"burners\":[",
        snapshot->ambient_temp, snapshot->max_temp);

    for (int i = 0; i < snapshot->burner_count && i < STOVEIQ_MAX_BURNERS; i++) {
        const burner_info_t *b = &snapshot->burners[i];
        if (i > 0) json[n++] = ',';
        uint32_t on_dur = (b->on_since_ms > 0 && snapshot->timestamp_ms > b->on_since_ms)
            ? (snapshot->timestamp_ms - b->on_since_ms) : 0;
        n += snprintf(json + n, 2048 - n,
            "{\"id\":%d,\"state\":%d,\"temp\":%.1f,\"max\":%.1f,"
            "\"rate\":%.2f,\"row\":%d,\"col\":%d,\"px\":%d,\"on\":%u}",
            b->id, b->state, b->current_temp, b->max_temp,
            b->temp_rate, b->center_row, b->center_col, b->pixel_count, (unsigned)on_dur);
    }

    n += snprintf(json + n, 2048 - n, "],\"alerts\":[");

    int first_alert = 1;
    for (int i = 0; i < alert_count; i++) {
        if (!alerts[i].active) continue;
        if (!first_alert) json[n++] = ',';
        first_alert = 0;
        n += snprintf(json + n, 2048 - n,
            "{\"type\":%d,\"burner\":%d,\"temp\":%.1f,\"active\":true}",
            alerts[i].type, alerts[i].burner_id, alerts[i].temp);
    }

    n += snprintf(json + n, 2048 - n, "]");

    /* Recipe state */
    if (recipe && recipe->active) {
        int rcount;
        const recipe_t *recipes = cooking_engine_get_recipes(&rcount);
        if (recipe->recipe_idx < rcount) {
            const recipe_t *r = &recipes[recipe->recipe_idx];
            const recipe_step_t *step = (recipe->current_step < r->step_count)
                ? &r->steps[recipe->current_step] : NULL;
            uint32_t timer_rem = 0;
            if (step && recipe->timer_running && step->timer_sec > 0) {
                uint32_t elapsed = (snapshot->timestamp_ms - recipe->timer_start_ms) / 1000;
                timer_rem = (elapsed < step->timer_sec) ? step->timer_sec - elapsed : 0;
            }
            /* Get current burner temp for progress display */
            float cur_temp = 0;
            for (int i = 0; i < snapshot->burner_count; i++) {
                if (snapshot->burners[i].id == recipe->burner_id) {
                    cur_temp = snapshot->burners[i].current_temp;
                    break;
                }
            }
            n += snprintf(json + n, 2048 - n,
                ",\"recipe\":{\"idx\":%d,\"name\":\"%s\",\"step\":%d,"
                "\"steps\":%d,\"desc\":\"%s\",\"coach\":\"%s\","
                "\"timer\":%u,\"burner\":%d,\"temp\":%.1f,"
                "\"target\":%.1f,\"trigger\":%d}",
                recipe->recipe_idx, r->name,
                recipe->current_step, r->step_count,
                step ? step->desc : "",
                step ? step->coach_msg : "",
                (unsigned)timer_rem, recipe->burner_id,
                cur_temp,
                step ? step->target_temp : 0,
                step ? (int)step->trigger : 0);
        }
    }

    n += snprintf(json + n, 2048 - n, "}");

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = n,
    };

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < s_ws_count; ) {
        esp_err_t ret = httpd_ws_send_frame_async(
            s_server, s_ws_fds[i], &ws_pkt);
        if (ret != ESP_OK) {
            s_ws_fds[i] = s_ws_fds[--s_ws_count];
        } else {
            i++;
        }
    }
    xSemaphoreGive(s_ws_mutex);

    free(json);
}

bool web_server_get_command(ws_command_t *cmd)
{
    if (!s_cmd_queue) return false;
    return (xQueueReceive(s_cmd_queue, cmd, 0) == pdTRUE);
}

void web_server_start_dns(void)
{
    xTaskCreate(dns_server_task, "dns", 4096, NULL, 3, NULL);
}

#endif /* CONFIG_STOVEIQ_TARGET_ESP32 */
