/**
 * tasks.c -- FreeRTOS task wrappers / pthread emulation
 *
 * Cooking pipeline:
 *   Sensor Read --[frame_queue]--> Cooking Engine --[ws broadcast]--> Web Server
 *
 * On native/emulator: pthreads with mutex-protected ring buffers.
 * On ESP32: FreeRTOS with pinned tasks and queues.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tasks.h"
#include "sensor.h"
#include "cooking_engine.h"
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/*  EMULATOR / NATIVE: pthread-based task simulation                   */
/* ================================================================== */

#ifdef CONFIG_STOVEIQ_USE_EMULATOR

#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>

/* ---- Simple ring buffer queue ---- */

typedef struct {
    void           *buffer;
    size_t          item_size;
    int             capacity;
    int             count;
    int             head;
    int             tail;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} sim_queue_t;

static int sim_queue_init(sim_queue_t *q, size_t item_size,
                          int capacity, void *storage)
{
    q->buffer = storage;
    q->item_size = item_size;
    q->capacity = capacity;
    q->count = q->head = q->tail = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    return 0;
}

static void sim_queue_destroy(sim_queue_t *q)
{
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int sim_queue_send(sim_queue_t *q, const void *item)
{
    pthread_mutex_lock(&q->mutex);
    if (q->count >= q->capacity) {
        q->tail = (q->tail + 1) % q->capacity;
        q->count--;
    }
    memcpy((char *)q->buffer + q->head * q->item_size, item, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int sim_queue_receive(sim_queue_t *q, void *item, int timeout_ms)
{
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0 && timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (q->count == 0) {
            if (pthread_cond_timedwait(&q->cond, &q->mutex, &ts) != 0) break;
        }
    }
    if (q->count == 0) { pthread_mutex_unlock(&q->mutex); return -1; }
    memcpy(item, (char *)q->buffer + q->tail * q->item_size, q->item_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

/* ---- State ---- */

static volatile bool    s_running;
static pthread_t        s_sensor_thread;
static pthread_t        s_cooking_thread;
static thermal_snapshot_t s_frame_buf[FRAME_QUEUE_DEPTH];
static sim_queue_t      s_frame_queue;
static cooking_engine_t s_engine;
static tasks_config_t   s_config;
static uint32_t         s_boot_ms;

/* Latest snapshot for external consumers */
static pthread_mutex_t  s_snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static thermal_snapshot_t s_latest_snapshot;
static bool             s_snapshot_valid = false;

static uint32_t millis_since_boot(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000) - s_boot_ms;
}

/* ---- Sensor Read Task ---- */

static void *sensor_read_task(void *arg)
{
    (void)arg;
    printf("[TASK] Sensor read task started\n");
    float frame[STOVEIQ_FRAME_PIXELS];

    while (s_running) {
        uint32_t ms = millis_since_boot();
        sensor_emu_set_time((float)ms / 1000.0f);

        sensor_err_t err = sensor_read_frame(frame);
        if (err == SENSOR_OK) {
            thermal_snapshot_t snap = {0};
            memcpy(snap.frame, frame, sizeof(frame));
            snap.max_temp = sensor_get_max_temp(frame);
            snap.ambient_temp = sensor_get_ambient(frame);
            snap.timestamp_ms = ms;
            sim_queue_send(&s_frame_queue, &snap);
        }
        usleep(250000);  /* 4Hz */
    }
    printf("[TASK] Sensor read task stopped\n");
    return NULL;
}

/* ---- Cooking Engine Task ---- */

static void *cooking_engine_task(void *arg)
{
    (void)arg;
    printf("[TASK] Cooking engine task started\n");
    thermal_snapshot_t snap;

    while (s_running) {
        if (sim_queue_receive(&s_frame_queue, &snap, 1000) == 0) {
            cooking_engine_process(&s_engine, &snap);

            /* Update latest snapshot */
            pthread_mutex_lock(&s_snapshot_mutex);
            s_latest_snapshot = snap;
            s_snapshot_valid = true;
            pthread_mutex_unlock(&s_snapshot_mutex);

            /* Print status periodically */
            static int frame_count = 0;
            if (++frame_count % 16 == 0) {
                printf("[COOK] max=%.1fC amb=%.1fC burners=%d",
                       snap.max_temp, snap.ambient_temp, snap.burner_count);
                for (int i = 0; i < snap.burner_count; i++) {
                    printf(" [B%d: %.1fC %s]", i, snap.burners[i].current_temp,
                           snap.burners[i].state == BURNER_STATE_HEATING ? "HEAT" :
                           snap.burners[i].state == BURNER_STATE_STABLE ? "STBL" :
                           snap.burners[i].state == BURNER_STATE_COOLING ? "COOL" : "OFF");
                }
                printf("\n");
            }
        }
    }
    printf("[TASK] Cooking engine task stopped\n");
    return NULL;
}

/* ---- Public API ---- */

int tasks_start(const tasks_config_t *config)
{
    if (!config) return -1;
    s_config = *config;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_boot_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

    sim_queue_init(&s_frame_queue, sizeof(thermal_snapshot_t),
                   FRAME_QUEUE_DEPTH, s_frame_buf);

    sensor_init();
    cooking_engine_init(&s_engine, &s_config.config);

    s_running = true;
    pthread_create(&s_sensor_thread, NULL, sensor_read_task, NULL);
    pthread_create(&s_cooking_thread, NULL, cooking_engine_task, NULL);

    printf("[TASK] All tasks started (emulator mode)\n");
    return 0;
}

int tasks_stop(void)
{
    s_running = false;
    pthread_cond_signal(&s_frame_queue.cond);
    pthread_join(&s_sensor_thread, NULL);
    pthread_join(&s_cooking_thread, NULL);
    sim_queue_destroy(&s_frame_queue);
    return 0;
}

bool tasks_is_running(void) { return s_running; }

bool tasks_get_snapshot(thermal_snapshot_t *out)
{
    pthread_mutex_lock(&s_snapshot_mutex);
    if (s_snapshot_valid) {
        *out = s_latest_snapshot;
        pthread_mutex_unlock(&s_snapshot_mutex);
        return true;
    }
    pthread_mutex_unlock(&s_snapshot_mutex);
    return false;
}

/* ================================================================== */
/*  ESP32: FreeRTOS task wrappers                                      */
/* ================================================================== */

#elif defined(CONFIG_STOVEIQ_TARGET_ESP32)

#include "web_server.h"
#include "wifi_manager.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "tasks";

static QueueHandle_t     s_frame_queue;
static TaskHandle_t      s_sensor_handle;
static TaskHandle_t      s_cooking_handle;
static cooking_engine_t  s_engine;
static tasks_config_t    s_config;
static volatile bool     s_running;

/* Latest snapshot */
static SemaphoreHandle_t s_snapshot_mutex;
static thermal_snapshot_t s_latest_snapshot;
static bool              s_snapshot_valid = false;

/* ---- Task 1: Sensor Read (Core 1, Pri 20, 32KB stack) ---- */

static void sensor_read_task(void *arg)
{
    (void)arg;
    float frame[STOVEIQ_FRAME_PIXELS];
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "Sensor read task on core %d", xPortGetCoreID());

    sensor_err_t serr = sensor_init();
    if (serr != SENSOR_OK) {
        ESP_LOGE(TAG, "Sensor init FAILED (err=%d)", serr);
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        sensor_err_t err = sensor_read_frame(frame);
        if (err == SENSOR_OK) {
            thermal_snapshot_t snap = {0};
            memcpy(snap.frame, frame, sizeof(frame));
            snap.max_temp = sensor_get_max_temp(frame);
            snap.ambient_temp = sensor_get_ambient(frame);
            snap.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            xQueueOverwrite(s_frame_queue, &snap);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(250));  /* 4Hz */
    }
    vTaskDelete(NULL);
}

/* ---- Task 2: Cooking Engine (Core 0, Pri 15, 16KB stack) ---- */

static void cooking_engine_task(void *arg)
{
    (void)arg;
    thermal_snapshot_t snap;
    uint32_t last_status_ms = 0;

    ESP_LOGI(TAG, "Cooking engine task on core %d", xPortGetCoreID());

    while (s_running) {
        if (xQueueReceive(s_frame_queue, &snap, pdMS_TO_TICKS(1000)) == pdTRUE) {
            cooking_engine_process(&s_engine, &snap);

            /* Broadcast thermal frame to WebSocket clients */
            web_server_broadcast_frame(&snap);

            /* Broadcast status JSON at 1Hz */
            if (snap.timestamp_ms - last_status_ms >= 1000) {
                int alert_count;
                const cook_alert_t *alerts =
                    cooking_engine_get_alerts(&s_engine, &alert_count);
                const recipe_session_t *rs =
                    cooking_engine_get_recipe(&s_engine);
                web_server_broadcast_status(&snap, alerts, alert_count, rs);
                last_status_ms = snap.timestamp_ms;
            }

            /* Process WebSocket commands */
            ws_command_t cmd;
            while (web_server_get_command(&cmd)) {
                switch (cmd.type) {
                case CMD_SILENCE_ALERT:
                    cooking_engine_silence_all(&s_engine);
                    break;
                case CMD_SET_CALIBRATION: {
                    /* Parse calibration from JSON payload */
                    calibration_t cal = {0};
                    cal.magic = CALIBRATION_MAGIC;
                    /* Simple parser: "b":[ {r:row,c:col,rad:radius}, ...] */
                    const char *p = cmd.payload;
                    while (cal.count < STOVEIQ_MAX_BURNERS) {
                        const char *rp = strstr(p, "\"r\":");
                        if (!rp) break;
                        burner_cal_t *bc = &cal.burners[cal.count];
                        bc->enabled = true;
                        bc->center_row = (uint8_t)atoi(rp + 4);
                        const char *cp = strstr(rp, "\"c\":");
                        if (cp) bc->center_col = (uint8_t)atoi(cp + 4);
                        const char *radp = strstr(rp, "\"rad\":");
                        if (radp) bc->radius = (uint8_t)atoi(radp + 6);
                        cal.count++;
                        p = rp + 1;
                    }
                    cooking_engine_set_calibration(&s_engine, &cal);
                    ESP_LOGI(TAG, "Calibration set: %d burners", cal.count);
                    break;
                }
                case CMD_START_RECIPE: {
                    /* Parse: {"recipe":idx,"burner":bid} */
                    int ridx = 0, bid = 0;
                    const char *rp = strstr(cmd.payload, "\"recipe\":");
                    if (rp) ridx = atoi(rp + 9);
                    const char *bp = strstr(cmd.payload, "\"burner\":");
                    if (bp) bid = atoi(bp + 9);
                    cooking_engine_start_recipe(&s_engine, ridx, bid);
                    ESP_LOGI(TAG, "Recipe %d started on burner %d", ridx, bid);
                    break;
                }
                case CMD_RECIPE_NEXT:
                    cooking_engine_recipe_next(&s_engine);
                    break;
                case CMD_RECIPE_CONFIRM:
                    cooking_engine_recipe_confirm(&s_engine);
                    break;
                case CMD_RECIPE_STOP:
                    cooking_engine_recipe_stop(&s_engine);
                    break;
                case CMD_SIM_TEMP: {
                    float t = -1;
                    int bid = 0;
                    const char *tp = strstr(cmd.payload, "\"temp\":");
                    if (tp) t = (float)atof(tp + 7);
                    const char *bp = strstr(cmd.payload, "\"burner\":");
                    if (bp) bid = atoi(bp + 9);
                    cooking_engine_set_sim_temp(&s_engine, bid, t);
                    if (t >= 0)
                        ESP_LOGI(TAG, "Sim: burner %d = %.1fC", bid, t);
                    else
                        ESP_LOGI(TAG, "Sim: disabled");
                    break;
                }
                case CMD_SET_EMISSIVITY: {
                    const char *ep = strstr(cmd.payload, "\"value\":");
                    if (ep) {
                        float e = (float)atof(ep + 8);
                        sensor_set_emissivity(e);
                        ESP_LOGI(TAG, "Emissivity set to %.2f", e);
                    }
                    break;
                }
                case CMD_SET_TEMP_OFFSET: {
                    const char *op = strstr(cmd.payload, "\"value\":");
                    if (op) {
                        float o = (float)atof(op + 8);
                        sensor_set_temp_offset(o);
                        ESP_LOGI(TAG, "Temp offset set to %.1fC", o);
                    }
                    break;
                }
                case CMD_SET_WIFI:
                    break;
                default:
                    break;
                }
            }

            /* Update snapshot for external consumers */
            xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
            s_latest_snapshot = snap;
            s_snapshot_valid = true;
            xSemaphoreGive(s_snapshot_mutex);
        }
    }
    vTaskDelete(NULL);
}

/* ---- Public API ---- */

int tasks_start(const tasks_config_t *config)
{
    if (!config) return -1;
    s_config = *config;

    /* Use xQueueCreate with size 1 + xQueueOverwrite for latest-frame semantics */
    s_frame_queue = xQueueCreate(1, sizeof(thermal_snapshot_t));
    s_snapshot_mutex = xSemaphoreCreateMutex();

    if (!s_frame_queue || !s_snapshot_mutex) {
        ESP_LOGE(TAG, "Failed to create queues");
        return -1;
    }

    cooking_engine_init(&s_engine, &s_config.config);

    /* Init WiFi */
    wifi_manager_init("StoveIQ");

    /* Init web server */
    web_server_init();
    web_server_start_dns();

    s_running = true;

    xTaskCreatePinnedToCore(sensor_read_task, "sensor",
        32768, NULL, 20, &s_sensor_handle, 1);

    xTaskCreatePinnedToCore(cooking_engine_task, "cooking",
        16384, NULL, 15, &s_cooking_handle, 0);

    ESP_LOGI(TAG, "All tasks started");
    return 0;
}

int tasks_stop(void)
{
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (s_frame_queue) vQueueDelete(s_frame_queue);
    s_frame_queue = NULL;
    return 0;
}

bool tasks_is_running(void) { return s_running; }

bool tasks_get_snapshot(thermal_snapshot_t *out)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    if (s_snapshot_valid) {
        *out = s_latest_snapshot;
        xSemaphoreGive(s_snapshot_mutex);
        return true;
    }
    xSemaphoreGive(s_snapshot_mutex);
    return false;
}

#else
#error "Define CONFIG_STOVEIQ_USE_EMULATOR or CONFIG_STOVEIQ_TARGET_ESP32"
#endif
