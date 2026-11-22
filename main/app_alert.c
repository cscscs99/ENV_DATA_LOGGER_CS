// Task 3: threshold detection with hysteresis and push notifications through RainMaker alerts.
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"

#ifdef CONFIG_ESP_INSIGHTS_ENABLED
#include "esp_diagnostics_metrics.h"
#endif

static const char *TAG = "alert";

typedef struct {
    bool active;
    bool pending;       // notification still to be delivered (new alarm, repeat due, or cloud was down)
    TickType_t last_tx;
    float value;
} alarm_t;

enum { IDX_TEMP, IDX_HUM, IDX_AIR, ALARM_COUNT };
static alarm_t s_alarm[ALARM_COUNT];

static void post_ctrl(ctrl_cmd_t cmd, int value)
{
    ctrl_msg_t m = { .cmd = cmd, .value = value };
    if (xQueueSend(g_ctrl_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "control queue full");
    }
}

static void evaluate(alarm_t *a, float value, float threshold, float hysteresis)
{
    a->value = value;
    if (!a->active && value >= threshold) {
        a->active = true;
        a->pending = true;
    } else if (a->active && value < threshold - hysteresis) {
        a->active = false;
        a->pending = false;
    }
}

// The air sensor is on/off, so the alarm clears only after several good readings in a row.
static void evaluate_air(alarm_t *a, bool poor)
{
    static int good_streak;
    a->value = poor;
    if (poor) {
        good_streak = 0;
        if (!a->active) {
            a->active = true;
            a->pending = true;
        }
    } else if (a->active && ++good_streak >= AIR_CLEAR_SAMPLES) {
        a->active = false;
        a->pending = false;
    }
}

static void deliver(alarm_t *a, const char *msg, TickType_t now)
{
    if (!a->active) {
        return;
    }
    if (!a->pending && now - a->last_tx >= pdMS_TO_TICKS(ALERT_REPEAT_MS)) {
        a->pending = true;
    }
    if (!a->pending || !(xEventGroupGetBits(g_events) & EVT_CLOUD_UP)) {
        return;     // stays pending and goes out as soon as the cloud is back
    }
    if (esp_rmaker_raise_alert(msg) == ESP_OK) {
        a->pending = false;
        a->last_tx = now;
    }
}

static void alert_task(void *arg)
{
    const TickType_t boot = xTaskGetTickCount();
    int last_mask = 0;

    for (;;) {
        sensor_sample_t s;
        TickType_t now;
        if (xQueueReceive(g_alert_q, &s, pdMS_TO_TICKS(ALERT_HOUSEKEEPING_MS)) == pdTRUE) {
            now = xTaskGetTickCount();
            if (s.dht_ok) {
                evaluate(&s_alarm[IDX_TEMP], s.temperature_c, g_temp_threshold_c, TEMP_HYSTERESIS_C);
                evaluate(&s_alarm[IDX_HUM], s.humidity_pct, g_hum_threshold_pct, HUM_HYSTERESIS_PCT);
            }
            if (now - boot >= pdMS_TO_TICKS(AIR_WARMUP_MS)) {
                evaluate_air(&s_alarm[IDX_AIR], s.air_poor);
            }
        } else {
            now = xTaskGetTickCount();
        }

        int mask = (s_alarm[IDX_TEMP].active ? ALARM_TEMP : 0) |
                   (s_alarm[IDX_HUM].active ? ALARM_HUM : 0) |
                   (s_alarm[IDX_AIR].active ? ALARM_AIR : 0);
        if (mask != last_mask) {
            post_ctrl(CTRL_ALARM_MASK, mask);
#ifdef CONFIG_ESP_INSIGHTS_ENABLED
            esp_diag_metrics_add_uint("alarm", (uint32_t)mask);
#endif
            ESP_LOGW(TAG, "alarm mask 0x%x (temp %.1f C, hum %.1f %%, air %s)", mask,
                     s_alarm[IDX_TEMP].value, s_alarm[IDX_HUM].value, s_alarm[IDX_AIR].active ? "POOR" : "GOOD");
            last_mask = mask;
        }

        char msg[ESP_RMAKER_MAX_ALERT_LEN + 1];
        snprintf(msg, sizeof(msg), "Warning! High Temperature Detected: %.0f\xC2\xB0""C", s_alarm[IDX_TEMP].value);
        deliver(&s_alarm[IDX_TEMP], msg, now);
        snprintf(msg, sizeof(msg), "Warning! High Humidity Detected: %.0f%%", s_alarm[IDX_HUM].value);
        deliver(&s_alarm[IDX_HUM], msg, now);
        deliver(&s_alarm[IDX_AIR], "Warning! Poor Air Quality Detected", now);
    }
}

esp_err_t alert_start(void)
{
    return xTaskCreate(alert_task, "alert", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
