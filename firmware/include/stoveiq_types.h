/**
 * stoveiq_types.h -- Core data types for StoveIQ Open Source firmware
 *
 * Smart cooking monitor types: thermal frames, burner zones, cooking
 * alerts, and session logging.  Platform-independent (ESP32 + native).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef STOVEIQ_TYPES_H
#define STOVEIQ_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Error codes (reuse esp_err_t pattern for both builds)              */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_STOVEIQ_TARGET_ESP32
#include "esp_err.h"
#else
#ifndef ESP_ERR_T_DEFINED
#define ESP_ERR_T_DEFINED
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL       -1
#define ESP_ERR_NO_MEM -2
#endif
#endif

/* ------------------------------------------------------------------ */
/*  MLX90640 frame dimensions                                          */
/* ------------------------------------------------------------------ */

#define STOVEIQ_FRAME_ROWS    24
#define STOVEIQ_FRAME_COLS    32
#define STOVEIQ_FRAME_PIXELS  (STOVEIQ_FRAME_ROWS * STOVEIQ_FRAME_COLS)  /* 768 */

#define STOVEIQ_MAX_BURNERS   4

/* ------------------------------------------------------------------ */
/*  Burner calibration (user-defined zones)                            */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     enabled;          /* Is this burner defined?              */
    char     name[16];         /* User label: "Front Left" etc.        */
    uint8_t  center_row;       /* Center in thermal image (0-23)       */
    uint8_t  center_col;       /* Center in thermal image (0-31)       */
    uint8_t  radius;           /* Circle radius in pixels (2-10)       */
} burner_cal_t;

typedef struct {
    uint32_t    magic;         /* 0x53495143 = "SIQC"                  */
    uint8_t     count;         /* Number of calibrated burners (0-4)   */
    burner_cal_t burners[STOVEIQ_MAX_BURNERS];
} calibration_t;

#define CALIBRATION_MAGIC  0x53495143

/* ------------------------------------------------------------------ */
/*  Recipe system                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    TRIGGER_MANUAL       = 0,   /* User taps "Next"                   */
    TRIGGER_BOIL         = 1,   /* Temp >= boil_temp_c and stable     */
    TRIGGER_SIMMER       = 2,   /* Temp in simmer range (85-95C)      */
    TRIGGER_TARGET       = 3,   /* Temp >= step target and stable     */
    TRIGGER_FOOD_DROP    = 4,   /* Sudden temp drop (>15C in 2s)      */
    TRIGGER_TIMER_DONE   = 5,   /* Step timer expired                 */
    TRIGGER_TEMP_BELOW   = 6,   /* Temp drops below target            */
    TRIGGER_CONFIRM      = 7,   /* User taps confirm button           */
} recipe_trigger_t;

#define RECIPE_MAX_STEPS     8
#define RECIPE_NAME_LEN     32
#define RECIPE_DESC_LEN     64

typedef struct {
    char              desc[RECIPE_DESC_LEN];  /* "Waiting for boil..." */
    float             target_temp;             /* Target for this step  */
    recipe_trigger_t  trigger;                 /* What advances to next */
    uint16_t          timer_sec;               /* Timer for this step   */
    char              coach_msg[RECIPE_DESC_LEN]; /* "Add pasta now!"  */
} recipe_step_t;

typedef struct {
    char          name[RECIPE_NAME_LEN];       /* "White Rice"         */
    uint8_t       step_count;
    recipe_step_t steps[RECIPE_MAX_STEPS];
} recipe_t;

typedef struct {
    bool          active;
    uint8_t       recipe_idx;          /* Index into recipe library     */
    uint8_t       current_step;
    int8_t        burner_id;           /* Calibrated burner ID (0-3)   */
    uint32_t      step_start_ms;       /* When current step began      */
    uint32_t      timer_start_ms;      /* When step timer started      */
    bool          timer_running;
    float         prev_temp;           /* For food-drop detection      */
} recipe_session_t;

/* ------------------------------------------------------------------ */
/*  Burner state                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    BURNER_STATE_OFF     = 0,
    BURNER_STATE_HEATING = 1,
    BURNER_STATE_STABLE  = 2,
    BURNER_STATE_COOLING = 3,
} burner_state_t;

/* ------------------------------------------------------------------ */
/*  Per-burner info (computed by cooking engine)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int            id;             /* 0-3 */
    int            center_row;     /* Centroid in thermal image (row)   */
    int            center_col;     /* Centroid in thermal image (col)   */
    int            pixel_count;    /* Pixels in this zone              */
    float          current_temp;   /* Average temp of zone (C)         */
    float          max_temp;       /* Max pixel in zone (C)            */
    float          min_temp;       /* Min pixel in zone (C)            */
    float          temp_rate;      /* dT/dt (C/sec) for trend detect   */
    uint32_t       on_since_ms;    /* Boot-relative time burner lit    */
    burner_state_t state;
} burner_info_t;

/* ------------------------------------------------------------------ */
/*  Thermal snapshot (produced by sensor, enriched by cooking engine)   */
/* ------------------------------------------------------------------ */

typedef struct {
    float          frame[STOVEIQ_FRAME_PIXELS];   /* Full 768-pixel data (C)  */
    float          ambient_temp;                   /* 10th percentile est (C)  */
    float          max_temp;                       /* Hottest pixel (C)        */
    uint32_t       timestamp_ms;                   /* Boot-relative millis     */
    int            burner_count;                    /* Active burners detected  */
    burner_info_t  burners[STOVEIQ_MAX_BURNERS];
} thermal_snapshot_t;

/* ------------------------------------------------------------------ */
/*  Cooking alert types                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    COOK_ALERT_BOIL_DETECTED   = 0,   /* Water reached boiling point    */
    COOK_ALERT_SMOKE_POINT     = 1,   /* Oil exceeding smoke threshold  */
    COOK_ALERT_PAN_PREHEATED   = 2,   /* Pan reached target temp        */
    COOK_ALERT_FORGOTTEN       = 3,   /* Burner on too long, no change  */
    COOK_ALERT_DEVICE_FAULT    = 4,   /* Sensor read failure            */
} cook_alert_type_t;

typedef struct {
    cook_alert_type_t type;
    int               burner_id;       /* Which burner triggered it     */
    float             temp;            /* Temperature at alert time (C) */
    uint32_t          timestamp_ms;
    bool              active;          /* Still alerting?               */
} cook_alert_t;

#define STOVEIQ_MAX_ALERTS  8

/* ------------------------------------------------------------------ */
/*  WebSocket command types (browser -> device)                        */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_SET_THRESHOLD   = 0,
    CMD_SILENCE_ALERT   = 1,
    CMD_START_SESSION   = 2,
    CMD_STOP_SESSION    = 3,
    CMD_SET_WIFI        = 4,
    CMD_SET_SETTING     = 5,
    CMD_TEST_BUZZER     = 6,
    CMD_SET_CALIBRATION = 7,
    CMD_START_RECIPE    = 8,
    CMD_RECIPE_NEXT     = 9,
    CMD_RECIPE_STOP     = 10,
    CMD_RECIPE_CONFIRM  = 11,
    CMD_SIM_TEMP        = 12,
} ws_command_type_t;

typedef struct {
    ws_command_type_t type;
    char              payload[128];   /* JSON payload */
} ws_command_t;

/* ------------------------------------------------------------------ */
/*  Runtime configuration (stored in NVS)                              */
/* ------------------------------------------------------------------ */

typedef enum {
    TEMP_UNIT_CELSIUS    = 0,
    TEMP_UNIT_FAHRENHEIT = 1,
} temp_unit_t;

typedef struct {
    /* Burner detection */
    float     burner_threshold_delta;  /* Degrees above ambient for active (default 30) */
    int       min_burner_pixels;       /* Min pixels for valid zone (default 4)          */
    int       max_burner_pixels;       /* Max pixels before rejection (default 200)      */

    /* Cooking alerts */
    float     boil_temp_c;             /* Boil detection temp (default 95)               */
    float     smoke_point_c;           /* Oil smoke point threshold (default 230)        */
    float     preheat_target_c;        /* Pan preheat target (default 200)               */
    uint32_t  forgotten_timeout_sec;   /* Forgotten burner timeout (default 1800 = 30m)  */

    /* Display */
    temp_unit_t temp_unit;             /* C or F display (default C)                     */
    uint8_t   colormap;                /* 0=Inferno, 1=Iron, 2=Hot, 3=Grayscale          */

    /* Hardware */
    uint8_t   buzzer_gpio;             /* GPIO for buzzer (default 39)                   */
    bool      buzzer_enabled;          /* Enable buzzer alerts (default true)             */
    bool      auto_session;            /* Auto-start session on burner detect (default 0) */

    /* Device */
    char      device_name[32];         /* mDNS name (default "stoveiq")                  */
} stoveiq_config_t;

#define STOVEIQ_CONFIG_DEFAULTS { \
    .burner_threshold_delta = 30.0f,   \
    .min_burner_pixels      = 4,       \
    .max_burner_pixels      = 200,     \
    .boil_temp_c            = 95.0f,   \
    .smoke_point_c          = 230.0f,  \
    .preheat_target_c       = 200.0f,  \
    .forgotten_timeout_sec  = 1800,    \
    .temp_unit              = TEMP_UNIT_CELSIUS, \
    .colormap               = 0,       \
    .buzzer_gpio            = 39,      \
    .buzzer_enabled         = true,    \
    .auto_session           = false,   \
    .device_name            = "stoveiq" \
}

/* ------------------------------------------------------------------ */
/*  Cook session logging                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t  timestamp_ms;            /* Relative to session start     */
    float     ambient;
    int       burner_count;
    struct {
        float   temp;
        float   rate;
        uint8_t state;
    } burners[STOVEIQ_MAX_BURNERS];
} session_sample_t;

/* ------------------------------------------------------------------ */
/*  BLE provisioning                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    PROV_STATUS_IDLE            = 0x00,
    PROV_STATUS_BLE_ACTIVE      = 0x01,
    PROV_STATUS_WIFI_CONNECTING = 0x02,
    PROV_STATUS_WIFI_CONNECTED  = 0x03,
    PROV_STATUS_COMPLETE        = 0x04,
} prov_status_t;

typedef enum {
    PROV_ERR_NONE           = 0x00,
    PROV_ERR_WIFI_AUTH_FAIL = 0x02,
    PROV_ERR_SSID_NOT_FOUND = 0x03,
    PROV_ERR_DHCP_TIMEOUT   = 0x04,
} prov_error_t;

typedef struct {
    char ssid[33];
    char password[65];
} wifi_creds_t;

/* ------------------------------------------------------------------ */
/*  Queue depths                                                       */
/* ------------------------------------------------------------------ */

#define FRAME_QUEUE_DEPTH    2
#define WS_QUEUE_DEPTH       4
#define CMD_QUEUE_DEPTH      4

#ifdef __cplusplus
}
#endif

#endif /* STOVEIQ_TYPES_H */
