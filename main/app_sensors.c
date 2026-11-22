// Task 1: periodic reading of the DHT11 (temperature, humidity) and the MQ-135 digital output (air quality).
//
// Reading a DHT11 needs a host start pulse of at least 18 ms followed by a timing-critical 4 ms exchange.
// The pulse is timed by an esp_timer one-shot so the task never sleeps in a delay call, and the exchange
// itself runs in a short critical section.
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"

static const char *TAG = "sensors";

static TaskHandle_t s_task;
static esp_timer_handle_t s_dht_timer;
static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t air_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_AIR_DO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&cfg);
}

static bool air_is_poor(void)
{
    bool low = gpio_get_level(PIN_AIR_DO) == 0;
    return AIR_DO_ACTIVE_LOW ? low : !low;
}

// ---- DHT11 ----

// Waits for the line to reach `level`; returns the time it took in microseconds, or -1 on timeout.
static int dht_wait(int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(PIN_DHT11) != level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - start);
}

// Reads the 40 data bits after the start pulse. Timing: response 80 us low + 80 us high, then each bit is
// 50 us low followed by a high of about 27 us (0) or 70 us (1).
static esp_err_t dht_read_frame(uint8_t data[5])
{
    esp_err_t err = ESP_ERR_TIMEOUT;
    memset(data, 0, 5);

    portENTER_CRITICAL(&s_dht_mux);
    gpio_set_level(PIN_DHT11, 1);                       // release the line, the pull-up takes it high
    if (dht_wait(0, 100) < 0 || dht_wait(1, 100) < 0 || dht_wait(0, 100) < 0) {
        goto out;
    }
    for (int i = 0; i < 40; i++) {
        int high_start = dht_wait(1, 80);               // end of the 50 us low
        int high_us = dht_wait(0, 100);                 // duration of the high pulse
        if (high_start < 0 || high_us < 0) {
            goto out;
        }
        data[i / 8] <<= 1;
        if (high_us > 40) {
            data[i / 8] |= 1;
        }
    }
    err = ESP_OK;
out:
    portEXIT_CRITICAL(&s_dht_mux);
    return err;
}

static bool dht_decode(const uint8_t d[5], float *temp, float *hum)
{
    if (((d[0] + d[1] + d[2] + d[3]) & 0xFF) != d[4]) {
        return false;
    }
    *hum = d[0] + d[1] * 0.1f;
    *temp = d[2] + (d[3] & 0x7F) * 0.1f;
    if (d[3] & 0x80) {
        *temp = -*temp;
    }
    return true;
}

static esp_err_t dht_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_DHT11),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "dht gpio");
    return gpio_set_level(PIN_DHT11, 1);
}

// ---- Task ----

static void notify_cb(void *arg)
{
    xTaskNotifyGive(s_task);
}

// Returns false when the sample should be skipped.
static bool read_hardware(sensor_sample_t *s, unsigned *dht_failures)
{
    int idle_high = gpio_get_level(PIN_DHT11);          // before the start pulse: tells open from shorted wiring
    gpio_set_level(PIN_DHT11, 0);                       // start signal
    esp_timer_start_once(s_dht_timer, DHT_START_LOW_MS * 1000ULL);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);            // end of the start pulse (esp_timer)

    uint8_t frame[5];
    esp_err_t frame_err = dht_read_frame(frame);
    s->dht_ok = frame_err == ESP_OK && dht_decode(frame, &s->temperature_c, &s->humidity_pct);
    if (!s->dht_ok && ((*dht_failures)++ % 10) == 0) {
        if (frame_err == ESP_OK) {
            ESP_LOGW(TAG, "DHT11 answered but the checksum is wrong (noise or a loose wire)");
        } else if (idle_high) {
            ESP_LOGW(TAG, "DHT11 not answering: data line idles high, so check its power, ground and data pin");
        } else {
            ESP_LOGW(TAG, "DHT11 data line is stuck LOW: it is shorted to GND or wired to the wrong pin");
        }
    } else if (s->dht_ok) {
        *dht_failures = 0;
    }

    s->air_poor = air_is_poor();
    return true;
}

static void sensor_task(void *arg)
{
    unsigned dht_failures = 0;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);        // sampling period (esp_timer)

        sensor_sample_t s = { 0 };
        if (!read_hardware(&s, &dht_failures)) {
            continue;
        }

        // A full queue means the consumer is behind; dropping the newest sample is harmless.
        xQueueSend(g_cloud_q, &s, 0);
        xQueueSend(g_alert_q, &s, 0);
    }
}

esp_err_t sensors_start(void)
{
    ESP_RETURN_ON_ERROR(air_init(), TAG, "air sensor");
    ESP_RETURN_ON_ERROR(dht_init(), TAG, "dht");
    ESP_RETURN_ON_FALSE(xTaskCreate(sensor_task, "sensor", 3072, NULL, 5, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");

    const esp_timer_create_args_t dht_args = { .callback = notify_cb, .name = "dht_start" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&dht_args, &s_dht_timer), TAG, "dht timer");

    const esp_timer_create_args_t period_args = { .callback = notify_cb, .name = "sample" };
    esp_timer_handle_t period_timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&period_args, &period_timer), TAG, "sample timer");
    return esp_timer_start_periodic(period_timer, SENSOR_PERIOD_MS * 1000ULL);
}
