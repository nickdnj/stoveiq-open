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

static const char FALLBACK_HTML[] =
"<!DOCTYPE html>\n"
"<html><head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>StoveIQ</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:system-ui,sans-serif;background:#111;color:#eee;padding:20px;"
"text-align:center}\n"
"h1{color:#f59e0b;margin-bottom:20px}\n"
"canvas{width:100%;max-width:480px;border-radius:8px;image-rendering:pixelated;"
"background:#222}\n"
".info{margin-top:16px;color:#888;font-size:14px}\n"
"#status{margin-top:8px;color:#4ade80}\n"
"</style></head><body>\n"
"<h1>StoveIQ</h1>\n"
"<canvas id=\"hm\" width=\"32\" height=\"24\"></canvas>\n"
"<div class=\"info\">Max: <b id=\"max\">--</b> | Ambient: <b id=\"amb\">--</b>"
" | Burners: <b id=\"bn\">0</b></div>\n"
"<div id=\"status\">Connecting...</div>\n"
"<script>\n"
"const cv=document.getElementById('hm'),cx=cv.getContext('2d');\n"
"const img=new ImageData(32,24);\n"
/* Inferno colormap LUT (simplified 8-stop version) */
"const CM=[[0,0,4],[40,11,84],[101,21,110],[159,42,99],[212,72,66],"
"[245,125,21],[250,193,39],[252,255,164]];\n"
"function lerp(a,b,t){return[a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,"
"a[2]+(b[2]-a[2])*t]}\n"
"function inferno(f){\n"
"  f=Math.max(0,Math.min(1,f));\n"
"  let s=f*(CM.length-1),i=Math.floor(s);\n"
"  if(i>=CM.length-1)return CM[CM.length-1];\n"
"  return lerp(CM[i],CM[i+1],s-i);\n"
"}\n"
"let ws,ok=0;\n"
"function connect(){\n"
"  ws=new WebSocket('ws://'+location.host+'/ws');\n"
"  ws.binaryType='arraybuffer';\n"
"  ws.onopen=()=>document.getElementById('status').textContent='Connected';\n"
"  ws.onclose=()=>{document.getElementById('status').textContent='Reconnecting...';"
"setTimeout(connect,2000)};\n"
"  ws.onmessage=(e)=>{\n"
"    if(typeof e.data==='string'){\n"
"      const d=JSON.parse(e.data);\n"
"      if(d.type==='status'){\n"
"        document.getElementById('bn').textContent=d.burners?d.burners.length:0;\n"
"      }\n"
"    }else{\n"
"      const dv=new DataView(e.data);\n"
"      let mn=9999,mx=-9999;\n"
"      const temps=new Float32Array(768);\n"
"      for(let i=0;i<768;i++){\n"
"        temps[i]=dv.getInt16(4+i*2,true)/10.0;\n"
"        if(temps[i]<mn)mn=temps[i];if(temps[i]>mx)mx=temps[i];\n"
"      }\n"
"      const range=mx-mn||1;\n"
"      for(let i=0;i<768;i++){\n"
"        const c=inferno((temps[i]-mn)/range);\n"
"        img.data[i*4]=c[0];img.data[i*4+1]=c[1];"
"img.data[i*4+2]=c[2];img.data[i*4+3]=255;\n"
"      }\n"
"      cx.putImageData(img,0,0);\n"
"      document.getElementById('max').textContent=mx.toFixed(1)+'C';\n"
"      document.getElementById('amb').textContent=mn.toFixed(1)+'C';\n"
"      ok++;document.getElementById('status').textContent='Live ('+ok+')';\n"
"    }\n"
"  };\n"
"}\n"
"connect();\n"
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
            else if (strstr((char *)buf, "\"set_threshold\""))
                cmd.type = CMD_SET_THRESHOLD;
            else if (strstr((char *)buf, "\"start_session\""))
                cmd.type = CMD_START_SESSION;
            else if (strstr((char *)buf, "\"stop_session\""))
                cmd.type = CMD_STOP_SESSION;
            else if (strstr((char *)buf, "\"set_wifi\""))
                cmd.type = CMD_SET_WIFI;
            else if (strstr((char *)buf, "\"test_buzzer\""))
                cmd.type = CMD_TEST_BUZZER;
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
                                 int alert_count)
{
    if (!s_server || s_ws_count == 0) return;

    /* Build JSON status message */
    char *json = heap_caps_malloc(1024, MALLOC_CAP_8BIT);
    if (!json) return;

    int n = snprintf(json, 1024,
        "{\"type\":\"status\",\"ambient\":%.1f,\"maxTemp\":%.1f,"
        "\"burners\":[",
        snapshot->ambient_temp, snapshot->max_temp);

    for (int i = 0; i < snapshot->burner_count && i < STOVEIQ_MAX_BURNERS; i++) {
        const burner_info_t *b = &snapshot->burners[i];
        if (i > 0) json[n++] = ',';
        n += snprintf(json + n, 1024 - n,
            "{\"id\":%d,\"state\":%d,\"temp\":%.1f,\"max\":%.1f,"
            "\"rate\":%.2f,\"row\":%d,\"col\":%d,\"px\":%d}",
            b->id, b->state, b->current_temp, b->max_temp,
            b->temp_rate, b->center_row, b->center_col, b->pixel_count);
    }

    n += snprintf(json + n, 1024 - n, "],\"alerts\":[");

    int first_alert = 1;
    for (int i = 0; i < alert_count; i++) {
        if (!alerts[i].active) continue;
        if (!first_alert) json[n++] = ',';
        first_alert = 0;
        n += snprintf(json + n, 1024 - n,
            "{\"type\":%d,\"burner\":%d,\"temp\":%.1f,\"active\":true}",
            alerts[i].type, alerts[i].burner_id, alerts[i].temp);
    }

    n += snprintf(json + n, 1024 - n, "]}");

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
