// Task 4: executes control commands (app, Alexa/Google Assistant and schedules all arrive as RainMaker writes)
// and owns the buzzer and alarm LED.
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rmaker_standard_params.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"

static const char *TAG = "control";

static void outputs_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_BUZZER) | BIT64(PIN_ALARM_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_BUZZER, 0);
    gpio_set_level(PIN_ALARM_LED, 0);
}

static void status_text(int mask, char *out, size_t len)
{
    if (!mask) {
        snprintf(out, len, "Normal");
        return;
    }
    out[0] = '\0';
    if (mask & ALARM_TEMP) {
        strlcat(out, "High Temperature, ", len);
    }
    if (mask & ALARM_HUM) {
        strlcat(out, "High Humidity, ", len);
    }
    if (mask & ALARM_AIR) {
        strlcat(out, "Poor Air Quality, ", len);
    }
    out[strlen(out) - 2] = '\0';        // drop the trailing ", "
}

static void control_task(void *arg)
{
    bool armed = true, buzzer = false;
    int mask = 0;

    for (;;) {
        // The wait timeout doubles as the buzzer beep timer, so no separate delay or timer is needed.
        bool beeping = BUZZER_ENABLED && (mask & ALARM_AIR) && armed;
        ctrl_msg_t m;
        if (xQueueReceive(g_ctrl_q, &m, beeping ? pdMS_TO_TICKS(BUZZER_TOGGLE_MS) : portMAX_DELAY) == pdTRUE) {
            switch (m.cmd) {
            case CTRL_ALARM_ARM:
                armed = m.value;
                break;
            case CTRL_ALARM_MASK: {
                char text[64];
                mask = m.value;
                status_text(mask, text, sizeof(text));
                param_set(g_alarm_dev, PARAM_STATUS, esp_rmaker_str(text));
                break;
            }
            }
            ESP_LOGI(TAG, "armed %d, alarm mask 0x%x", armed, mask);
        } else {
            buzzer = !buzzer;
        }

        // Buzzer and LED follow only the air quality alarm; temperature and humidity alarms stay app-only.
        if (!(BUZZER_ENABLED && (mask & ALARM_AIR) && armed)) {
            buzzer = false;
        }
        gpio_set_level(PIN_ALARM_LED, (mask & ALARM_AIR) != 0);
        gpio_set_level(PIN_BUZZER, buzzer);
    }
}

esp_err_t control_start(void)
{
    outputs_init();
    return xTaskCreate(control_task, "control", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
