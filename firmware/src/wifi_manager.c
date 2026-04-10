/**
 * wifi_manager.c -- WiFi AP+STA manager
 *
 * Creates an open AP for direct connection and optionally
 * connects to a home WiFi network for LAN access.
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef CONFIG_STOVEIQ_TARGET_ESP32

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi";

#define WIFI_STA_CONNECTED_BIT  BIT0
#define NVS_NAMESPACE           "wifi_creds"
#define NVS_KEY_SSID            "ssid"
#define NVS_KEY_PASS            "pass"

static EventGroupHandle_t s_wifi_events;
static char s_sta_ip[16] = "";
static bool s_sta_connected = false;

/* ------------------------------------------------------------------ */
/*  Event handler                                                      */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Client connected to AP");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "Client disconnected from AP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "STA disconnected, reconnecting...");
            s_sta_connected = false;
            s_sta_ip[0] = '\0';
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR,
                 IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA connected: %s", s_sta_ip);
        xEventGroupSetBits(s_wifi_events, WIFI_STA_CONNECTED_BIT);
    }
}

/* ------------------------------------------------------------------ */
/*  NVS helpers                                                        */
/* ------------------------------------------------------------------ */

static bool load_sta_creds(char *ssid, size_t ssid_len,
                           char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;

    esp_err_t e1 = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(nvs, NVS_KEY_PASS, pass, &pass_len);
    nvs_close(nvs);
    return (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0');
}

static void save_sta_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return;
    nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_PASS, pass);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ------------------------------------------------------------------ */
/*  mDNS                                                               */
/* ------------------------------------------------------------------ */

static void start_mdns(const char *hostname)
{
    mdns_init();
    mdns_hostname_set(hostname);
    mdns_instance_name_set("StoveIQ Cooking Monitor");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", hostname);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t wifi_manager_init(const char *ap_ssid)
{
    s_wifi_events = xEventGroupCreate();

    /* netif/event loop/wifi may already be initialized by main.c (for BLE provisioning).
     * Tolerate ESP_ERR_INVALID_STATE which means "already initialized". */
    esp_err_t err;
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    /* Create both AP and STA netifs */
    esp_netif_create_default_wifi_ap();
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"))
        esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Configure AP */
    wifi_config_t ap_config = {
        .ap = {
            .channel = 6,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(ap_ssid);

    /* Check for stored STA creds */
    char ssid[33] = {0};
    char pass[65] = {0};
    bool has_sta = load_sta_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    if (has_sta) {
        /* AP+STA concurrent mode */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

        wifi_config_t sta_config = {0};
        strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_connect();

        ESP_LOGI(TAG, "AP+STA mode: AP='%s' + STA='%s'", ap_ssid, ssid);
    } else {
        /* AP only */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "AP mode: '%s' at 192.168.4.1", ap_ssid);
    }

    printf("\n");
    printf("*******************************************\n");
    printf("*  WiFi: %s (open)%*s*\n", ap_ssid,
           (int)(24 - strlen(ap_ssid)), "");
    printf("*  Dashboard: http://192.168.4.1          *\n");
    if (has_sta) {
        printf("*  Also joining: %s%*s*\n", ssid,
               (int)(25 - strlen(ssid)), "");
    }
    printf("*******************************************\n\n");

    start_mdns("stoveiq");

    return ESP_OK;
}

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password)
{
    save_sta_creds(ssid, password);

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password,
            sizeof(sta_config.sta.password) - 1);

    /* Switch to APSTA if currently AP-only */
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    return esp_wifi_connect();
}

bool wifi_manager_sta_connected(void)
{
    return s_sta_connected;
}

const char *wifi_manager_get_sta_ip(void)
{
    return s_sta_ip;
}

#endif /* CONFIG_STOVEIQ_TARGET_ESP32 */
