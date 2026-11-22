// Task 5: tracks RainMaker OTA progress. The download itself runs in RainMaker's OTA agent; this task
// raises a push notification if an update fails. Rollback protection comes from
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE: the new image is only kept once it has reached the cloud.
#include "esp_event.h"
#include "esp_log.h"
#include "esp_rmaker_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"

static const char *TAG = "ota";

static QueueHandle_t s_ota_q;

static void ota_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    int32_t event = id;
    xQueueSend(s_ota_q, &event, 0);
    if (id == RMAKER_OTA_EVENT_IN_PROGRESS && data) {
        ESP_LOGI(TAG, "%s", (const char *)data);
    }
}

static void ota_task(void *arg)
{
    for (;;) {
        int32_t event;
        if (xQueueReceive(s_ota_q, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (event) {
        case RMAKER_OTA_EVENT_STARTING:
            ESP_LOGI(TAG, "firmware update started");
            xEventGroupSetBits(g_events, EVT_OTA_ACTIVE | EVT_STATE_CHANGED);
            break;
        case RMAKER_OTA_EVENT_SUCCESSFUL:
            ESP_LOGI(TAG, "firmware update downloaded, rebooting into the new image");
            break;
        case RMAKER_OTA_EVENT_FAILED:
        case RMAKER_OTA_EVENT_REJECTED:
        case RMAKER_OTA_EVENT_DELAYED:
            ESP_LOGW(TAG, "firmware update did not complete (event %d)", (int)event);
            xEventGroupClearBits(g_events, EVT_OTA_ACTIVE);
            xEventGroupSetBits(g_events, EVT_STATE_CHANGED);
            if (event == RMAKER_OTA_EVENT_FAILED && (xEventGroupGetBits(g_events) & EVT_CLOUD_UP)) {
                esp_rmaker_raise_alert("Firmware update failed");
            }
            break;
        default:
            break;
        }
    }
}

esp_err_t ota_start(void)
{
    s_ota_q = xQueueCreate(8, sizeof(int32_t));
    if (!s_ota_q) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(RMAKER_OTA_EVENT, ESP_EVENT_ANY_ID, ota_event_handler, NULL));
    return xTaskCreate(ota_task, "ota_evt", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
