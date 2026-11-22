// Connectivity recovery and the physical reset / re-handshake button.
//
// Event handlers only set event-group bits; connect_task reacts to them and runs the recovery watchdog.
// Nothing here blocks the sensor, alert, cloud or control tasks, and none of them wait for Wi-Fi.
//
// Button: short press = re-handshake with the AP, 3 s = Wi-Fi reset (re-provision), 10 s = factory reset.
#include "esp_event.h"
#include "esp_log.h"
#include "esp_rmaker_common_events.h"
#include "esp_rmaker_system_ctrl.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_provisioning/manager.h"
#include "app.h"

static const char *TAG = "connect";

static QueueHandle_t s_btn_q;

// ---- Event handlers (run in the default event loop task) ----

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // The network helper reconnects at once, so a missing AP repeats this event in a tight loop.
        static unsigned count;
        if ((count++ % 50) == 0) {
            const wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "Wi-Fi disconnected, reason %d (201 = network not found, 15/204 = wrong password)",
                     d ? d->reason : -1);
        }
        xEventGroupClearBits(g_events, EVT_WIFI_UP | EVT_CLOUD_UP);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected, got an IP address");
        xEventGroupSetBits(g_events, EVT_WIFI_UP);
    } else if (base == RMAKER_COMMON_EVENT && id == RMAKER_MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "cloud connected");
        xEventGroupSetBits(g_events, EVT_CLOUD_UP);
    } else if (base == RMAKER_COMMON_EVENT && id == RMAKER_MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "cloud disconnected");
        xEventGroupClearBits(g_events, EVT_CLOUD_UP);
    } else if (base == NETWORK_PROV_EVENT && id == NETWORK_PROV_START) {
        xEventGroupSetBits(g_events, EVT_PROV_ACTIVE);
    } else if (base == NETWORK_PROV_EVENT && (id == NETWORK_PROV_END || id == NETWORK_PROV_DEINIT)) {
        xEventGroupClearBits(g_events, EVT_PROV_ACTIVE);
    } else {
        return;
    }
    xEventGroupSetBits(g_events, EVT_STATE_CHANGED);
}

// ---- Button ----

static void IRAM_ATTR button_isr(void *arg)
{
    uint8_t token = 0;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_btn_q, &token, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void button_task(void *arg)
{
    bool pressed = false, hint_wifi = false, hint_factory = false;
    TickType_t t0 = 0;

    for (;;) {
        // While the button is held, the receive timeout wakes the task at the next hold threshold.
        TickType_t wait = portMAX_DELAY;
        if (pressed && !hint_factory) {
            TickType_t next = pdMS_TO_TICKS(hint_wifi ? BUTTON_FACTORY_RESET_MS : BUTTON_WIFI_RESET_MS);
            TickType_t held = xTaskGetTickCount() - t0;
            wait = held >= next ? 0 : next - held;
        }

        uint8_t token;
        bool edge = xQueueReceive(s_btn_q, &token, wait) == pdTRUE;
        bool down = gpio_get_level(PIN_BUTTON) == 0;   // the pin level is the truth; bounces cancel out
        TickType_t held = xTaskGetTickCount() - t0;

        if (edge && down && !pressed) {
            pressed = true;
            hint_wifi = hint_factory = false;
            t0 = xTaskGetTickCount();
        } else if (edge && !down && pressed) {
            pressed = false;
            if (held >= pdMS_TO_TICKS(BUTTON_FACTORY_RESET_MS)) {
                xEventGroupSetBits(g_events, EVT_BTN_FACTORY);
            } else if (held >= pdMS_TO_TICKS(BUTTON_WIFI_RESET_MS)) {
                xEventGroupSetBits(g_events, EVT_BTN_WIFI_RESET);
            } else if (held >= pdMS_TO_TICKS(BUTTON_MIN_PRESS_MS)) {
                xEventGroupSetBits(g_events, EVT_BTN_SHORT);
            }
        } else if (!edge && pressed) {
            if (!hint_wifi && held >= pdMS_TO_TICKS(BUTTON_WIFI_RESET_MS)) {
                hint_wifi = true;
                ESP_LOGI(TAG, "release now for Wi-Fi reset, keep holding for factory reset");
            } else if (!hint_factory && held >= pdMS_TO_TICKS(BUTTON_FACTORY_RESET_MS)) {
                hint_factory = true;
                ESP_LOGI(TAG, "release now for factory reset");
            }
        }
    }
}

// ---- Recovery ----

static bool wifi_has_credentials(void)
{
    wifi_config_t cfg;
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0';
}

// Drops and re-establishes the Wi-Fi link. The disconnect event makes the network helper reconnect;
// if there was nothing to disconnect, reconnect explicitly.
static void rehandshake(void)
{
    ESP_LOGW(TAG, "re-handshake with the access point");
    if (esp_wifi_disconnect() != ESP_OK) {
        esp_wifi_connect();
    }
}

// While the cloud is unreachable the task wakes periodically to run the recovery watchdog.
static TickType_t wait_period(EventBits_t st)
{
    return (st & (EVT_CLOUD_UP | EVT_PROV_ACTIVE)) ? portMAX_DELAY : pdMS_TO_TICKS(5000);
}

static void connect_task(void *arg)
{
    const EventBits_t wake = EVT_STATE_CHANGED | EVT_BTN_SHORT | EVT_BTN_WIFI_RESET | EVT_BTN_FACTORY;
    bool down = false;
    TickType_t down_since = 0;

    for (;;) {
        EventBits_t st = xEventGroupGetBits(g_events);
        EventBits_t ev = xEventGroupWaitBits(g_events, wake, pdTRUE, pdFALSE, wait_period(st));
        st = xEventGroupGetBits(g_events);

        if (ev & EVT_BTN_FACTORY) {
            ESP_LOGW(TAG, "factory reset requested");
            esp_rmaker_factory_reset(2, 2);
        } else if (ev & EVT_BTN_WIFI_RESET) {
            ESP_LOGW(TAG, "Wi-Fi reset requested, device will re-provision");
            esp_rmaker_wifi_reset(2, 2);
        } else if (ev & EVT_BTN_SHORT) {
            rehandshake();
            down_since = xTaskGetTickCount();
        }

        TickType_t now = xTaskGetTickCount();
        if ((st & (EVT_CLOUD_UP | EVT_PROV_ACTIVE)) || !wifi_has_credentials()) {
            down = false;
        } else if (!down) {
            down = true;
            down_since = now;
        } else if (now - down_since >= pdMS_TO_TICKS(RECOVERY_KICK_MS)) {
            rehandshake();
            down_since = now;
        }
    }
}

esp_err_t connect_start(void)
{
    s_btn_q = xQueueCreate(8, sizeof(uint8_t));
    if (!s_btn_q) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t btn_cfg = {
        .pin_bit_mask = BIT64(PIN_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    esp_err_t isr_err = gpio_install_isr_service(0);     // INVALID_STATE: already installed elsewhere
    ESP_ERROR_CHECK(isr_err == ESP_ERR_INVALID_STATE ? ESP_OK : isr_err);
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_BUTTON, button_isr, NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(RMAKER_COMMON_EVENT, ESP_EVENT_ANY_ID, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, net_event_handler, NULL));

    if (xTaskCreate(connect_task, "connect", 4096, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(button_task, "button", 3072, NULL, 7, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
