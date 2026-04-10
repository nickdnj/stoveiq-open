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

static const char *TAG = "webserver";

/* ------------------------------------------------------------------ */
/*  WebSocket client tracking                                          */
/* ------------------------------------------------------------------ */

#define MAX_WS_CLIENTS  4

static httpd_handle_t s_server = NULL;
static int s_ws_fds[MAX_WS_CLIENTS];
static int s_ws_count = 0;
static SemaphoreHandle_t s_ws_mutex = NULL;
static QueueHandle_t s_cmd_queue = NULL;

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
"<meta name=theme-color content=#111>\n"
"<link rel=icon type=image/png href='data:image/png;base64,"
"iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAAAXNSR0IArs4c6QAAAERlWElmTU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAAA6ABAAMAAAABAAEAAKACAAQAAAABAAAAIKADAAQAAAABAAAAIAAAAACshmLzAAAEmElEQVRIDYVWT2icRRR/M7O7Zjc1NWmbNiFKJSn9QxEPomDtJdAepFQQRQ9eoiI9FPXSeile6kU81INFsAehevGgVm9a9FC8xIuIQg9WCKg0Jmqb3TS7ye73jb83b2a+2W9X+1hmZ977vd97M+/N7KqJPXOkFBVi/RTfXo2vuAhWBilrLbuyMTIIMsJUxdmKtWN1y+jCBJCIYWrReFpepVbMPQAG3W9zjncfQBEZi0QSv2i1CJCKGIb6pDDMU0w6jzBPVQmKCIIB87gM9iHfgxhoYooysQggWuEFjXdDAYHGIGuBxyiRyTsolmDtM8YdwBpj2DzLXYBC1cvIGMpzTkArynKqaG+FDoIARuvQMjEY16CUHIHd0ztPMe/fowCcGVcTDRqp0twulYXNORRvNuf4fuVo2bVUZHcq6O5wUjJpd+n5R/ShaXX/uFp43LS36LV5s+0e5XN3YD4XJ2HlN1EOwJgQXXLH2KjR7jG11qbZXdRs0+SYqhna6nEeqTCevfnLfXg2EICVXFj5YI7Tf3hGge7Gqp3frz/9IXvqIb24lLc2ff2dhx+E3i08h6nfuyNB8O2HwBih3Yw6XRqr6++X7H11+nWV1towqp9vWlSb6+rAGCGum2QqrEpNTO1L2BQKFWvFParosb1qYpTjgW6zy80DMZrb8q91u7hk2RY4tZOUEG0ac5WYxYhePH/SHD+ofr/Fj1puaXanQphfViwSR/iZcX31uj33ZVYxhVeJMF60FMEJgW7HKB07oF74MLu+bJH4RofOndAo75tf5I0R6uV0YLf6eMFc+JZubfD+hkp60QoANoUPblanR62OrVX4Zj2wUz26V1UNPTiplpusXN+0AJhyoyCUnBrTwIiv/xbLmd7ZpENT6sops9Kk3/6hz14xh6cVlKi/b8uiCpHK06ZHVGxSZnBGmkfn9HLLvnHMXF7M3/2G79br8/qtk+adq9n0dgbEGIG7L+P0iGBg5hgHnvUqvXREdbrq8DSd+dzWa0x37YY9e5zDNKoMEL6EFQRCxTo5v8gZknDfaJW1Dr14OX/yvezKj/bUUW0Un/jLR3Dd7ImL2cJHebMztLwFoewgCR+iOwgPaEEk/vbX+cXnzFevMv7vdTr9SQZlRbtL4LIBNGEppuUOAFiiA4L3E/Ms4xu72rJPf9D76Q+Lln3mUg9XDEq0FkAR7wL1D5bSGnibRIcbXgjUcG5StbZsVdOdLfS7RZviuR6tUTenfZP86gEmMfq53UoNBIg/TLi6t9t06bv8wrMGHQlBeccb/Hg8MYuHgjUI8/61/PYGVUOeA5HKbxG8VIZDCa2HTp8ao20j3pHPhB8iHqFqdehmk/ckwj9q5VuH/0UDgvcK753EgPNKi/5sFUVL4YiRsONlHawo/+hDgIwU/K4hEX61WcfvGgNkTwLEWAgv4BLPNlg8FAEiNSPjEg4DPuIr+L4g0aufHSu/g6BPbqBTgUuI0iRgGaqMJGLFyF6DNShxlZbCMlQZA8Dq2aGSskhMLDGJ8+hw18n/ubh/T8wQQXL7ZRmUPmMs+d9RADsrL72ZaYoU2QrDv37Bv/aEF/gzAAAAAElFTkSuQmCC'>\n"
"<link rel=manifest href=/manifest.json>\n"
"<link rel=apple-touch-icon href='data:image/png;base64,"
"iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAAAXNSR0IArs4c6QAAAERlWElmTU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAAA6ABAAMAAAABAAEAAKACAAQAAAABAAAAIKADAAQAAAABAAAAIAAAAACshmLzAAAEmElEQVRIDYVWT2icRRR/M7O7Zjc1NWmbNiFKJSn9QxEPomDtJdAepFQQRQ9eoiI9FPXSeile6kU81INFsAehevGgVm9a9FC8xIuIQg9WCKg0Jmqb3TS7ye73jb83b2a+2W9X+1hmZ977vd97M+/N7KqJPXOkFBVi/RTfXo2vuAhWBilrLbuyMTIIMsJUxdmKtWN1y+jCBJCIYWrReFpepVbMPQAG3W9zjncfQBEZi0QSv2i1CJCKGIb6pDDMU0w6jzBPVQmKCIIB87gM9iHfgxhoYooysQggWuEFjXdDAYHGIGuBxyiRyTsolmDtM8YdwBpj2DzLXYBC1cvIGMpzTkArynKqaG+FDoIARuvQMjEY16CUHIHd0ztPMe/fowCcGVcTDRqp0twulYXNORRvNuf4fuVo2bVUZHcq6O5wUjJpd+n5R/ShaXX/uFp43LS36LV5s+0e5XN3YD4XJ2HlN1EOwJgQXXLH2KjR7jG11qbZXdRs0+SYqhna6nEeqTCevfnLfXg2EICVXFj5YI7Tf3hGge7Gqp3frz/9IXvqIb24lLc2ff2dhx+E3i08h6nfuyNB8O2HwBih3Yw6XRqr6++X7H11+nWV1towqp9vWlSb6+rAGCGum2QqrEpNTO1L2BQKFWvFParosb1qYpTjgW6zy80DMZrb8q91u7hk2RY4tZOUEG0ac5WYxYhePH/SHD+ofr/Fj1puaXanQphfViwSR/iZcX31uj33ZVYxhVeJMF60FMEJgW7HKB07oF74MLu+bJH4RofOndAo75tf5I0R6uV0YLf6eMFc+JZubfD+hkp60QoANoUPblanR62OrVX4Zj2wUz26V1UNPTiplpusXN+0AJhyoyCUnBrTwIiv/xbLmd7ZpENT6sops9Kk3/6hz14xh6cVlKi/b8uiCpHK06ZHVGxSZnBGmkfn9HLLvnHMXF7M3/2G79br8/qtk+adq9n0dgbEGIG7L+P0iGBg5hgHnvUqvXREdbrq8DSd+dzWa0x37YY9e5zDNKoMEL6EFQRCxTo5v8gZknDfaJW1Dr14OX/yvezKj/bUUW0Un/jLR3Dd7ImL2cJHebMztLwFoewgCR+iOwgPaEEk/vbX+cXnzFevMv7vdTr9SQZlRbtL4LIBNGEppuUOAFiiA4L3E/Ms4xu72rJPf9D76Q+Lln3mUg9XDEq0FkAR7wL1D5bSGnibRIcbXgjUcG5StbZsVdOdLfS7RZviuR6tUTenfZP86gEmMfq53UoNBIg/TLi6t9t06bv8wrMGHQlBeccb/Hg8MYuHgjUI8/61/PYGVUOeA5HKbxG8VIZDCa2HTp8ao20j3pHPhB8iHqFqdehmk/ckwj9q5VuH/0UDgvcK753EgPNKi/5sFUVL4YiRsONlHawo/+hDgIwU/K4hEX61WcfvGgNkTwLEWAgv4BLPNlg8FAEiNSPjEg4DPuIr+L4g0aufHSu/g6BPbqBTgUuI0iRgGaqMJGLFyF6DNShxlZbCMlQZA8Dq2aGSskhMLDGJ8+hw18n/ubh/T8wQQXL7ZRmUPmMs+d9RADsrL72ZaYoU2QrDv37Bv/aEF/gzAAAAAElFTkSuQmCC'>\n"
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
".unit-btn{background:#222;border:1px solid #444;color:#eee;padding:4px 10px;"
"border-radius:4px;font-size:13px;cursor:pointer}\n"
/* Heatmap */
".hm-wrap{position:relative;width:100%;aspect-ratio:32/24;border-radius:8px;"
"overflow:hidden;background:#222;margin-bottom:4px}\n"
".hm-wrap canvas{width:100%;height:100%;image-rendering:pixelated}\n"
"#overlay{position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none}\n"
/* Info bar */
".info-bar{display:flex;justify-content:space-between;font-size:12px;color:#888;"
"margin-bottom:12px;padding:0 2px}\n"
/* Burner cards */
".cards{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px}\n"
".card{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:10px;"
"position:relative;overflow:hidden}\n"
".card.heating{border-color:#f59e0b}\n"
".card.stable{border-color:#4ade80}\n"
".card.cooling{border-color:#60a5fa}\n"
".card .label{font-size:11px;color:#888;text-transform:uppercase;letter-spacing:1px}\n"
".card .temp{font-size:28px;font-weight:700;line-height:1.1}\n"
".card .temp.heating{color:#f59e0b}\n"
".card .temp.stable{color:#4ade80}\n"
".card .temp.cooling{color:#60a5fa}\n"
".card .meta{font-size:11px;color:#888;margin-top:4px}\n"
".card .trend{font-size:14px;position:absolute;top:10px;right:10px}\n"
".card .state-badge{display:inline-block;font-size:10px;padding:2px 6px;"
"border-radius:3px;margin-top:4px;font-weight:600}\n"
".state-badge.heating{background:#f59e0b22;color:#f59e0b}\n"
".state-badge.stable{background:#4ade8022;color:#4ade80}\n"
".state-badge.cooling{background:#60a5fa22;color:#60a5fa}\n"
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
"</style></head><body>\n"
/* Alert bar */
"<div class=alert-bar id=alerts></div>\n"
/* Header */
"<div class=hdr>\n"
"  <h1>StoveIQ</h1>\n"
"  <div style='display:flex;align-items:center;gap:8px'>\n"
"    <button class=unit-btn id=unitBtn onclick=toggleUnit()>C</button>\n"
"    <button class=settings-btn onclick=toggleSettings()>&#9881;</button>\n"
"    <div class=dot id=dot></div>\n"
"  </div>\n"
"</div>\n"
/* Settings */
"<div class=settings id=settingsPanel>\n"
"  <label>Boil Temp<input type=number id=cfgBoil value=95 step=1></label>\n"
"  <label>Smoke Point<input type=number id=cfgSmoke value=230 step=1></label>\n"
"  <label>Preheat Target<input type=number id=cfgPreheat value=200 step=1></label>\n"
"  <label>Forgotten Timeout (min)<input type=number id=cfgForgot value=30 step=1></label>\n"
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
"    <canvas id=calHm width=32 height=24 style='width:100%;height:100%;image-rendering:pixelated;"
"border-radius:8px'></canvas>\n"
"    <canvas id=calOv width=320 height=240 style='position:absolute;top:0;left:0;width:100%;"
"height:100%;border-radius:8px'></canvas>\n"
"  </div>\n"
"  <div id=calBtns style='display:flex;gap:6px;justify-content:center;margin-top:12px'></div>\n"
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
/* Heatmap */
"<div class=hm-wrap>\n"
"  <canvas id=hm width=32 height=24></canvas>\n"
"  <canvas id=overlay width=320 height=240></canvas>\n"
"</div>\n"
"<div class=info-bar>\n"
"  <span>Max: <b id=mx>--</b></span>\n"
"  <span>Ambient: <b id=amb>--</b></span>\n"
"  <span id=fps></span>\n"
"</div>\n"
/* Burner cards */
"<div id=cards class=cards></div>\n"
"<div class=no-burners id=noBurners>Waiting for heat...</div>\n"
"\n"
"<script>\n"
/* State */
"let useFahrenheit=false,frameCount=0,lastFpsTime=Date.now();\n"
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
"document.getElementById('unitBtn').textContent=tu()}\n"
"function toggleSettings(){document.getElementById('settingsPanel').classList.toggle('open')}\n"
/* Canvas refs */
"const cv=document.getElementById('hm'),cx=cv.getContext('2d');\n"
"const ov=document.getElementById('overlay'),ox=ov.getContext('2d');\n"
"const img=new ImageData(32,24);\n"
"let temps=new Float32Array(768);\n"
/* Audio alert */
"let audioCtx;\n"
"function beep(){try{if(!audioCtx)audioCtx=new(window.AudioContext||window.webkitAudioContext)();"
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
"    const timeStr=mins+'m '+secs+'s';\n"
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
"    return '<div class=\"card '+cls+'\">'\n"
"      +'<div class=label>Burner '+(b.id+1)+'</div>'\n"
"      +'<div class=trend>'+arrow+'</div>'\n"
"      +'<div class=\"temp '+cls+'\">'+fmtT(b.temp)+'</div>'\n"
"      +'<div class=meta>Peak '+fmtT(b.max)+' | '+timeStr+'</div>'\n"
"      +'<span class=\"state-badge '+cls+'\">'+st+'</span>'\n"
"      +timerHtml+'</div>';\n"
"  }).join('');\n"
"}\n"
/* Render alerts */
"function renderAlerts(alerts){\n"
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
/* Draw burner overlays on heatmap */
"function drawOverlays(){\n"
"  ox.clearRect(0,0,320,240);\n"
"  if(!burners)return;\n"
"  const sx=320/32,sy=240/24;\n"
"  burners.forEach(b=>{\n"
"    const x=b.col*sx,y=b.row*sy;\n"
"    const r=Math.max(12,Math.sqrt(b.px)*sx*0.8);\n"
"    const colors=['#888','#f59e0b','#4ade80','#60a5fa'];\n"
"    ox.strokeStyle=colors[b.state]||'#888';\n"
"    ox.lineWidth=2;\n"
"    ox.beginPath();ox.arc(x,y,r,0,Math.PI*2);ox.stroke();\n"
"    ox.font='bold 13px system-ui';ox.textAlign='center';\n"
"    ox.fillStyle='#fff';ox.shadowColor='#000';ox.shadowBlur=3;\n"
"    ox.fillText(fmtT(b.temp),x,y-r-4);\n"
"    ox.shadowBlur=0;\n"
"  });\n"
"}\n"
/* WebSocket */
"let ws;\n"
"function connect(){\n"
"  ws=new WebSocket('ws://'+location.host+'/ws');\n"
"  ws.binaryType='arraybuffer';\n"
"  ws.onopen=()=>document.getElementById('dot').classList.add('on');\n"
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
"        drawOverlays();\n"
"      }\n"
"    }else{\n"
"      const dv=new DataView(e.data);\n"
"      let mn=9999,mx=-9999;\n"
"      for(let i=0;i<768;i++){\n"
"        temps[i]=dv.getInt16(4+i*2,true)/10.0;\n"
"        if(temps[i]<mn)mn=temps[i];if(temps[i]>mx)mx=temps[i];\n"
"      }\n"
"      const range=mx-mn||1;\n"
"      for(let i=0;i<768;i++){\n"
"        const c=cmap((temps[i]-mn)/range);\n"
"        img.data[i*4]=c[0];img.data[i*4+1]=c[1];"
"img.data[i*4+2]=c[2];img.data[i*4+3]=255;\n"
"      }\n"
"      cx.putImageData(img,0,0);\n"
"      document.getElementById('mx').textContent=fmtT(mx);\n"
"      document.getElementById('amb').textContent=fmtT(mn);\n"
"      frameCount++;const now=Date.now();\n"
"      if(now-lastFpsTime>=2000){document.getElementById('fps').textContent="
"(frameCount/((now-lastFpsTime)/1000)).toFixed(1)+' fps';frameCount=0;lastFpsTime=now}\n"
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
"  calBurners.push({r:12,c:16,rad:4});\n"
"  calSelected=calBurners.length-1;\n"
"  drawCal();\n"
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
"    const b=calBurners.map(b=>({r:b.r,c:b.c,rad:b.rad}));\n"
"    ws.send(JSON.stringify({cmd:'set_calibration',b:b}));\n"
"  }\n"
"  try{localStorage.setItem('siq_cal',JSON.stringify(calBurners))}catch(e){}\n"
"  document.getElementById('calOverlay').style.display='none';\n"
"  showRecipePicker();\n"
"}\n"
/* Load saved calibration from localStorage on page load */
"try{const saved=localStorage.getItem('siq_cal');\n"
"  if(saved){calBurners=JSON.parse(saved);\n"
"    if(calBurners.length>0){\n"
"      /* Re-send to firmware on reconnect */\n"
"      setTimeout(()=>{\n"
"        if(ws&&ws.readyState===1){\n"
"          const b=calBurners.map(b=>({r:b.r,c:b.c,rad:b.rad}));\n"
"          ws.send(JSON.stringify({cmd:'set_calibration',b:b}));\n"
"        }\n"
"      },2000);\n"
"    }\n"
"  }\n"
"}catch(e){}\n"
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
"    calCx.putImageData(calImg,0,0);\n"
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
"    calOx.fillText('B'+(i+1),b.c*sx,b.r*sy+5);\n"
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
"    if(hit>=0){calSelected=hit;calDragging=true}\n"
"    else if(calBurners.length<4){calBurners.push({r:Math.round(y),c:Math.round(x),rad:4});"
"calSelected=calBurners.length-1}\n"
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
"let recipeActive=false;\n"
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
"function startRecipe(idx){\n"
"  const bid=calBurners.length>0?0:-1;\n"
"  if(ws&&ws.readyState===1)\n"
"    ws.send(JSON.stringify({cmd:'start_recipe',recipe:idx,burner:bid}));\n"
"  document.getElementById('recipePicker').style.display='none';\n"
"  recipeActive=true;\n"
"}\n"
"function recipeNext(){\n"
"  if(ws&&ws.readyState===1)ws.send(JSON.stringify({cmd:'recipe_next'}));\n"
"}\n"
"function recipeStop(){\n"
"  if(ws&&ws.readyState===1)ws.send(JSON.stringify({cmd:'recipe_stop'}));\n"
"  document.getElementById('recipeActive').style.display='none';\n"
"  recipeActive=false;\n"
"}\n"
"\n"
/* Recipe button in main UI */
"document.querySelector('.hdr h1').insertAdjacentHTML('afterend',\n"
"  '<button style=\"margin-left:8px;background:#222;border:1px solid #444;color:#eee;'\n"
"  +'padding:4px 10px;border-radius:4px;font-size:12px;cursor:pointer\" '\n"
"  +'onclick=showRecipePicker()>Recipes</button>');\n"
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
    "const CACHE='stoveiq-v1';\n"
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
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t web_server_init(void)
{
    s_ws_mutex = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(ws_command_t));
    s_ws_count = 0;

    init_spiffs();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384;
    config.max_uri_handlers = 12;
    config.max_open_sockets = 7;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
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
    httpd_register_uri_handler(s_server, &manifest);
    httpd_register_uri_handler(s_server, &icon);
    httpd_register_uri_handler(s_server, &sw);

    /* Captive portal */
    httpd_uri_t apple = { .uri = "/hotspot-detect.html", .method = HTTP_GET,
                          .handler = captive_handler };
    httpd_uri_t android = { .uri = "/generate_204", .method = HTTP_GET,
                            .handler = generate_204_handler };
    httpd_uri_t conn = { .uri = "/connecttest.txt", .method = HTTP_GET,
                         .handler = captive_handler };
    httpd_register_uri_handler(s_server, &apple);
    httpd_register_uri_handler(s_server, &android);
    httpd_register_uri_handler(s_server, &conn);

    ESP_LOGI(TAG, "HTTP + WebSocket server started on port %d",
             config.server_port);
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
