// Task 2: publishes sensor data to the RainMaker cloud and to Espressif Insights.
#include <math.h>
#include "esp_log.h"
#include "esp_rmaker_standard_params.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"

#ifdef CONFIG_ESP_INSIGHTS_ENABLED
#include "esp_diagnostics_metrics.h"
#endif

static const char *TAG = "cloud";

void param_set(const esp_rmaker_device_t *dev, const char *param_name, esp_rmaker_param_val_t val)
{
    const esp_rmaker_param_t *param = esp_rmaker_device_get_param_by_name(dev, param_name);
    if (!param) {
        return;
    }
    if (xEventGroupGetBits(g_events) & EVT_CLOUD_UP) {
        esp_rmaker_param_update_and_report(param, val);
    } else {
        esp_rmaker_param_update(param, val);
    }
}

const char *air_label(bool air_poor)
{
    return air_poor ? "POOR" : "GOOD";
}

static void insights_metrics(const sensor_sample_t *s)
{
#ifdef CONFIG_ESP_INSIGHTS_ENABLED
    if (s->dht_ok) {
        esp_diag_metrics_add_float("temp", s->temperature_c);
        esp_diag_metrics_add_float("hum", s->humidity_pct);
    }
    esp_diag_metrics_add_uint("air", s->air_poor);
#endif
}

static float round1(float v)
{
    return roundf(v * 10.0f) / 10.0f;
}

static void cloud_task(void *arg)
{
    float last_temp = NAN, last_hum = NAN;
    int last_air = -1;
    TickType_t last_report = 0, last_metric = 0;
    bool reported_once = false, metric_once = false;

    for (;;) {
        sensor_sample_t s;
        if (xQueueReceive(g_cloud_q, &s, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        TickType_t now = xTaskGetTickCount();

        if (!metric_once || now - last_metric >= pdMS_TO_TICKS(INSIGHTS_METRIC_PERIOD_MS)) {
            insights_metrics(&s);
            last_metric = now;
            metric_once = true;
        }

        // Offline samples are skipped; the first sample after reconnecting is always sent.
        if (!(xEventGroupGetBits(g_events) & EVT_CLOUD_UP)) {
            reported_once = false;
            continue;
        }

        bool changed = (int)s.air_poor != last_air ||
                       (s.dht_ok && (fabsf(s.temperature_c - last_temp) >= CLOUD_TEMP_DELTA_C ||
                                     fabsf(s.humidity_pct - last_hum) >= CLOUD_HUM_DELTA_PCT));
        bool heartbeat = now - last_report >= pdMS_TO_TICKS(CLOUD_HEARTBEAT_MS);
        if (reported_once && !changed && !heartbeat) {
            continue;
        }

        if (s.dht_ok) {
            param_set(g_temp_dev, ESP_RMAKER_DEF_TEMPERATURE_NAME, esp_rmaker_float(round1(s.temperature_c)));
            param_set(g_hum_dev, PARAM_HUMIDITY, esp_rmaker_float(round1(s.humidity_pct)));
            last_temp = s.temperature_c;
            last_hum = s.humidity_pct;
        }
        param_set(g_air_dev, PARAM_STATUS, esp_rmaker_str(air_label(s.air_poor)));
        ESP_LOGI(TAG, "reported %.1f C, %.1f %%RH, air %s", s.temperature_c, s.humidity_pct, air_label(s.air_poor));

        last_air = s.air_poor;
        last_report = now;
        reported_once = true;
    }
}

esp_err_t cloud_start(void)
{
#ifdef CONFIG_ESP_INSIGHTS_ENABLED
    esp_diag_metrics_register("sensor", "temp", "Temperature", "env.temp", ESP_DIAG_DATA_TYPE_FLOAT);
    esp_diag_metrics_register("sensor", "hum", "Humidity", "env.hum", ESP_DIAG_DATA_TYPE_FLOAT);
    esp_diag_metrics_register("sensor", "air", "Air quality poor", "env.air", ESP_DIAG_DATA_TYPE_UINT);
    esp_diag_metrics_register("alarm", "alarm", "Alarm mask", "env.alarm", ESP_DIAG_DATA_TYPE_UINT);
#endif
    return xTaskCreate(cloud_task, "cloud", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
