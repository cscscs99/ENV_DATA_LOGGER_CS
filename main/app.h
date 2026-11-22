#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rmaker_core.h"

// ---- Wiring (ESP32, 30-pin dev board) ----
// Everything is on the right-hand header row (USB at the bottom), plus the board's own BOOT button.
// The MQ-135 digital output (DO) is used, so no ADC pin is needed. Power the module from 3V3 so that DO
// never exceeds 3.3 V; if its readings seem dead, power it from VIN and put a 10k/20k divider on DO.
#define PIN_DHT11           GPIO_NUM_4      // data line, 10 k pull-up to 3V3
#define PIN_AIR_DO          GPIO_NUM_23     // MQ-135 digital output
#define PIN_BUZZER          GPIO_NUM_18
#define PIN_ALARM_LED       GPIO_NUM_19     // on while any alarm is active
#define PIN_BUTTON          GPIO_NUM_0      // BOOT button, active low

// Most MQ-135 modules pull DO low when the gas level is above the trimmer-pot setting.
#define AIR_DO_ACTIVE_LOW   1

// Raises the provisioning components to DEBUG so a failed phone pairing can be diagnosed from the log.
#define PROV_DEBUG_LOGS     1

// 0 keeps the buzzer silent (app status and notifications still work). Set to 1 to enable it.
#define BUZZER_ENABLED      0

// Offsets the board's MAC, which also changes its Bluetooth address. Bump it if iOS keeps a stale cache of the
// board's Bluetooth services after the firmware's service layout changed.
#define BLE_ADDR_REROLL     3

// ---- Timing ----
#define SENSOR_PERIOD_MS            2000    // DHT11 cannot be read faster than once per second
#define DHT_START_LOW_MS            20      // host start signal: line held low for 18 ms or more
#define CLOUD_HEARTBEAT_MS          30000   // report even when nothing changed
#define CLOUD_TEMP_DELTA_C          0.5f
#define CLOUD_HUM_DELTA_PCT         2.0f
#define INSIGHTS_METRIC_PERIOD_MS   10000
#define ALERT_REPEAT_MS             60000   // re-notify while a condition persists
#define ALERT_HOUSEKEEPING_MS       1000    // lets the alert task flush alerts queued while offline
#define BUZZER_TOGGLE_MS            400
#define AIR_WARMUP_MS               30000   // MQ-135 heater needs time before DO means anything
#define AIR_CLEAR_SAMPLES           5       // good readings in a row before the air alarm clears
#define RECOVERY_KICK_MS            30000   // re-handshake with the AP if the cloud stays unreachable this long
#define BUTTON_MIN_PRESS_MS         50
#define BUTTON_WIFI_RESET_MS        3000
#define BUTTON_FACTORY_RESET_MS     10000

// ---- Thresholds ----
#define DEFAULT_TEMP_THRESHOLD_C    35
#define DEFAULT_HUM_THRESHOLD_PCT   80
#define TEMP_HYSTERESIS_C           2
#define HUM_HYSTERESIS_PCT          5

// ---- Event group bits ----
// State bits: kept up to date by event handlers.
#define EVT_WIFI_UP         BIT0
#define EVT_CLOUD_UP        BIT1
#define EVT_PROV_ACTIVE     BIT2
#define EVT_OTA_ACTIVE      BIT3
// Transient bits: consumed by the connectivity task.
#define EVT_STATE_CHANGED   BIT4
#define EVT_BTN_SHORT       BIT5
#define EVT_BTN_WIFI_RESET  BIT6
#define EVT_BTN_FACTORY     BIT7

// ---- Alarm sources ----
#define ALARM_TEMP          BIT0
#define ALARM_HUM           BIT1
#define ALARM_AIR           BIT2

typedef struct {
    float temperature_c;
    float humidity_pct;
    bool air_poor;      // MQ-135 digital output says the gas level is above its trimmer-pot setting
    bool dht_ok;        // false when the DHT11 did not answer: temperature and humidity are not valid
} sensor_sample_t;

typedef enum {
    CTRL_ALARM_ARM,     // value: 0/1, enables the buzzer
    CTRL_ALARM_MASK,    // value: bitmask of ALARM_* currently active
} ctrl_cmd_t;

typedef struct {
    ctrl_cmd_t cmd;
    int value;
} ctrl_msg_t;

// RainMaker device / parameter names shared between files.
#define DEV_TEMP_NAME       "Temperature Sensor"
#define DEV_HUM_NAME        "Humidity Sensor"
#define DEV_AIR_NAME        "Air Quality"
#define DEV_ALARM_NAME      "Alarm"
#define PARAM_HUMIDITY      "Humidity"
#define PARAM_THRESHOLD     "Threshold"
#define PARAM_STATUS        "Status"

extern EventGroupHandle_t g_events;
extern QueueHandle_t g_cloud_q;
extern QueueHandle_t g_alert_q;
extern QueueHandle_t g_ctrl_q;

// Written by the RainMaker write callback, read by the alert task (aligned 32-bit accesses are atomic).
extern volatile int g_temp_threshold_c;
extern volatile int g_hum_threshold_pct;

extern esp_rmaker_device_t *g_temp_dev;
extern esp_rmaker_device_t *g_hum_dev;
extern esp_rmaker_device_t *g_air_dev;
extern esp_rmaker_device_t *g_alarm_dev;

esp_err_t sensors_start(void);
esp_err_t cloud_start(void);
esp_err_t alert_start(void);
esp_err_t control_start(void);
esp_err_t connect_start(void);
esp_err_t ota_start(void);

// Updates a parameter and pushes it to the cloud only when MQTT is connected.
void param_set(const esp_rmaker_device_t *dev, const char *param_name, esp_rmaker_param_val_t val);

// "GOOD" or "POOR".
const char *air_label(bool air_poor);
