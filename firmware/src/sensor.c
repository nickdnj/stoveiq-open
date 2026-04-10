/**
 * sensor.c — Sensor abstraction layer implementation
 *
 * Switches between real MLX90640 I2C reads and the thermal emulator
 * based on compile-time flags.
 */

#include "sensor.h"
#include <string.h>
#include <stdbool.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Emulator backend                                                   */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_STOVEIQ_USE_EMULATOR

#include "thermal_emulator.h"

static bool s_initialized = false;
static emu_state_t s_emu_state;
static float s_emu_time = 0.0f;

/* Default scenario: empty room at 22C, no burners */
static const emu_scenario_t s_default_scenario = {
    .name = "default_empty",
    .ambient_temp = 22.0f,
    .noise_stddev = 0.3f,
    .angle_attenuation = 0.0f,
    .num_burners = 0,
    .num_events = 0,
};

sensor_err_t sensor_init(void)
{
    emu_init(&s_emu_state, &s_default_scenario);
    s_emu_time = 0.0f;
    s_initialized = true;
    return SENSOR_OK;
}

void sensor_emu_set_scenario(const emu_scenario_t *scenario)
{
    if (scenario) {
        emu_init(&s_emu_state, scenario);
        s_emu_time = 0.0f;
    }
}

void sensor_emu_set_time(float time_sec)
{
    s_emu_time = time_sec;
}

emu_state_t *sensor_emu_get_state(void)
{
    return &s_emu_state;
}

static sensor_err_t sensor_read_raw(float frame[STOVEIQ_FRAME_PIXELS])
{
    emu_generate_frame(&s_emu_state, s_emu_time, frame);
    return SENSOR_OK;
}

/* ------------------------------------------------------------------ */
/*  Real MLX90640 backend (stub — implemented when hardware arrives)   */
/* ------------------------------------------------------------------ */

#elif defined(CONFIG_STOVEIQ_TARGET_ESP32)

/* ------------------------------------------------------------------ */
/*  Real MLX90640 I2C backend for ESP32-S3                             */
/* ------------------------------------------------------------------ */

#include "driver/i2c.h"
#include "esp_log.h"
#include "MLX90640_I2C_Driver.h"
#include "MLX90640_API.h"

static const char *TAG = "sensor";

/* I2C configuration */
#define SENSOR_I2C_PORT     I2C_NUM_0
#define SENSOR_I2C_SDA      GPIO_NUM_1
#define SENSOR_I2C_SCL      GPIO_NUM_2
#define SENSOR_I2C_FREQ_HZ  400000      /* 400kHz — conservative, sensor supports 1MHz */
#define MLX90640_ADDR       0x33        /* Default I2C address */

/* Sensor configuration */
#define SENSOR_REFRESH_RATE 0x03        /* 0x03 = 4Hz (matches FR-100 requirement) */
#define SENSOR_EMISSIVITY   0.95f       /* Good for most cookware & stove surfaces */
#define SENSOR_ADC_RES      0x03        /* 18-bit ADC resolution */

/* Internal state */
static bool s_initialized = false;
static paramsMLX90640 s_params;         /* Calibration parameters from EEPROM */
static uint16_t s_raw_frame[834];       /* Raw frame data buffer (per Melexis spec) */
static float s_tr = 23.0f;             /* Reflected temperature estimate (room temp) */

/**
 * Initialize I2C master bus for MLX90640 communication.
 */
static esp_err_t sensor_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SENSOR_I2C_SDA,
        .scl_io_num = SENSOR_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SENSOR_I2C_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(SENSOR_I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(SENSOR_I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C bus 0 initialized: SDA=GPIO%d, SCL=GPIO%d, %dkHz",
             SENSOR_I2C_SDA, SENSOR_I2C_SCL, SENSOR_I2C_FREQ_HZ / 1000);

    /* Scan I2C bus to help debug wiring issues */
    ESP_LOGI(TAG, "Scanning I2C bus...");
    int found = 0;
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(SENSOR_I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "  No I2C devices found! Check wiring: SDA=GPIO%d, SCL=GPIO%d",
                 SENSOR_I2C_SDA, SENSOR_I2C_SCL);
    } else {
        ESP_LOGI(TAG, "  %d device(s) found on I2C bus", found);
    }

    return ESP_OK;
}

sensor_err_t sensor_init(void)
{
    ESP_LOGI(TAG, "Initializing MLX90640 sensor...");

    /* Step 1: Initialize I2C bus */
    if (sensor_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C initialization failed");
        return SENSOR_ERR_INIT;
    }

    /* Step 2: Verify sensor is present by reading device ID */
    static uint16_t ee_data[832];
    int status = MLX90640_DumpEE(MLX90640_ADDR, ee_data);
    if (status != 0) {
        ESP_LOGE(TAG, "MLX90640 EEPROM dump failed (status=%d) — sensor not found?", status);
        return SENSOR_ERR_INIT;
    }
    ESP_LOGI(TAG, "MLX90640 EEPROM read OK");

    /* Step 3: Extract calibration parameters */
    status = MLX90640_ExtractParameters(ee_data, &s_params);
    if (status != 0) {
        ESP_LOGE(TAG, "Parameter extraction failed (status=%d)", status);
        return SENSOR_ERR_INIT;
    }
    ESP_LOGI(TAG, "Calibration parameters extracted");

    /* Step 4: Configure sensor refresh rate and ADC resolution */
    status = MLX90640_SetRefreshRate(MLX90640_ADDR, SENSOR_REFRESH_RATE);
    if (status != 0) {
        ESP_LOGE(TAG, "SetRefreshRate failed (status=%d)", status);
        return SENSOR_ERR_INIT;
    }

    status = MLX90640_SetResolution(MLX90640_ADDR, SENSOR_ADC_RES);
    if (status != 0) {
        ESP_LOGE(TAG, "SetResolution failed (status=%d)", status);
        return SENSOR_ERR_INIT;
    }

    /* Step 5: Read one frame to prime the pipeline (sensor needs 2 subpage reads) */
    for (int subpage = 0; subpage < 2; subpage++) {
        status = MLX90640_GetFrameData(MLX90640_ADDR, s_raw_frame);
        if (status < 0) {
            ESP_LOGW(TAG, "Initial frame read %d failed (status=%d), retrying...", subpage, status);
            vTaskDelay(pdMS_TO_TICKS(500));
            status = MLX90640_GetFrameData(MLX90640_ADDR, s_raw_frame);
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "MLX90640 initialized: 4Hz refresh, 18-bit ADC, emissivity=%.2f", SENSOR_EMISSIVITY);
    return SENSOR_OK;
}

static sensor_err_t sensor_read_raw(float frame[STOVEIQ_FRAME_PIXELS])
{
    /* Read raw frame data (one subpage per call) */
    int status = MLX90640_GetFrameData(MLX90640_ADDR, s_raw_frame);
    if (status < 0) {
        ESP_LOGE(TAG, "GetFrameData failed (status=%d)", status);
        return SENSOR_ERR_READ;
    }

    /* Get the Vdd for compensation */
    float vdd = MLX90640_GetVdd(s_raw_frame, &s_params);
    if (vdd < 2.5f || vdd > 3.6f) {
        ESP_LOGW(TAG, "Unusual Vdd=%.2fV (expected 3.3V)", vdd);
    }

    /* Get ambient temperature for compensation */
    float ta = MLX90640_GetTa(s_raw_frame, &s_params);

    /* Update reflected temperature estimate from ambient */
    s_tr = ta - 8.0f;  /* Reflected temp ~8°C below ambient (typical indoor) */

    /* Calculate object temperatures for all 768 pixels */
    MLX90640_CalculateTo(s_raw_frame, &s_params, SENSOR_EMISSIVITY, s_tr, frame);

    return SENSOR_OK;
}

#else
#error "Define CONFIG_STOVEIQ_USE_EMULATOR or CONFIG_STOVEIQ_TARGET_ESP32"
#endif

/* ------------------------------------------------------------------ */
/*  Shared implementation (both backends)                              */
/* ------------------------------------------------------------------ */

bool sensor_is_initialized(void)
{
    return s_initialized;
}

/**
 * Validate a frame: reject flat frames and out-of-range values.
 */
static sensor_err_t validate_frame(const float frame[STOVEIQ_FRAME_PIXELS])
{
    float min_val = frame[0];
    float max_val = frame[0];

    for (int i = 0; i < STOVEIQ_FRAME_PIXELS; i++) {
        if (frame[i] < SENSOR_TEMP_MIN || frame[i] > SENSOR_TEMP_MAX) {
            return SENSOR_ERR_RANGE;
        }
        if (frame[i] < min_val) min_val = frame[i];
        if (frame[i] > max_val) max_val = frame[i];
    }

    /* Flat frame: all pixels within tolerance of each other */
    if ((max_val - min_val) < SENSOR_FLAT_TOLERANCE) {
        return SENSOR_ERR_FLAT;
    }

    return SENSOR_OK;
}

sensor_err_t sensor_read_frame(float frame[STOVEIQ_FRAME_PIXELS])
{
    if (!s_initialized) {
        return SENSOR_ERR_NO_INIT;
    }

    sensor_err_t err = sensor_read_raw(frame);
    if (err != SENSOR_OK) {
        return err;
    }

    return validate_frame(frame);
}

float sensor_get_max_temp(const float frame[STOVEIQ_FRAME_PIXELS])
{
    float max_t = frame[0];
    for (int i = 1; i < STOVEIQ_FRAME_PIXELS; i++) {
        if (frame[i] > max_t) max_t = frame[i];
    }
    return max_t;
}

/**
 * Simple insertion-based partial sort to find the 10th percentile.
 * We only need the smallest ~77 values out of 768, so we maintain
 * a small buffer and scan once.
 */
float sensor_get_ambient(const float frame[STOVEIQ_FRAME_PIXELS])
{
    /* 10th percentile of 768 pixels = 76.8 -> use 77 pixels */
    #define AMBIENT_COUNT 77

    float lowest[AMBIENT_COUNT];
    int count = 0;

    for (int i = 0; i < STOVEIQ_FRAME_PIXELS; i++) {
        float val = frame[i];
        if (isnan(val) || val < -40.0f || val > 500.0f) continue;

        if (count < AMBIENT_COUNT) {
            /* Fill the buffer first */
            int j = count;
            while (j > 0 && lowest[j - 1] > val) {
                lowest[j] = lowest[j - 1];
                j--;
            }
            lowest[j] = val;
            count++;
        } else if (val < lowest[AMBIENT_COUNT - 1]) {
            /* Replace the largest in our buffer */
            int j = AMBIENT_COUNT - 1;
            while (j > 0 && lowest[j - 1] > val) {
                lowest[j] = lowest[j - 1];
                j--;
            }
            lowest[j] = val;
        }
    }

    /* Average the 10th percentile */
    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        sum += lowest[i];
    }
    return sum / (float)count;

    #undef AMBIENT_COUNT
}
