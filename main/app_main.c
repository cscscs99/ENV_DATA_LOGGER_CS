// Environmental Data Logger: temperature, humidity and air quality node for ESP32 on ESP RainMaker.
//
// Tasks (all event-driven, none polls with a blocking delay):
//   sensor   - esp_timer notifies it every SENSOR_PERIOD_MS to read the DHT11 and the MQ-135
//   cloud    - queue of samples -> RainMaker parameters and Insights metrics
//   alert    - queue of samples -> thresholds, push notifications (RainMaker alerts)
//   control  - queue of commands (app / Alexa / Google Assistant / schedules) -> buzzer, alarm LED
//   connect  - event group bits from Wi-Fi/MQTT/provisioning events -> recovery watchdog
//   button   - GPIO interrupt queue -> short/long/very long press detection
//   ota_evt  - queue of RainMaker OTA events
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rmaker_core.h"
#include "esp_rmaker_ota.h"
#include "esp_rmaker_standard_devices.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_types.h"
#include "esp_rmaker_system_ctrl.h"
#include "nvs_flash.h"
#include "app_insights.h"
#include "app_network.h"
#include "app.h"

static const char *TAG = "app_main";

EventGroupHandle_t g_events;
QueueHandle_t g_cloud_q;
QueueHandle_t g_alert_q;
QueueHandle_t g_ctrl_q;

volatile int g_temp_threshold_c = DEFAULT_TEMP_THRESHOLD_C;
volatile int g_hum_threshold_pct = DEFAULT_HUM_THRESHOLD_PCT;

esp_rmaker_device_t *g_temp_dev;
esp_rmaker_device_t *g_hum_dev;
esp_rmaker_device_t *g_air_dev;
esp_rmaker_device_t *g_alarm_dev;

// Passed as priv_data so one callback can serve every device.
typedef enum { DEV_ID_ALARM = 1, DEV_ID_TEMP, DEV_ID_HUM, DEV_ID_AIR } dev_id_t;

static void post_ctrl(ctrl_cmd_t cmd, int value)
{
    ctrl_msg_t m = { .cmd = cmd, .value = value };
    if (xQueueSend(g_ctrl_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "control queue full, command dropped");
    }
}

// Runs for cloud writes (app, voice assistants, schedules) and once at boot for persisted parameters.
static esp_err_t write_cb(const esp_rmaker_device_t *device, const esp_rmaker_param_write_req_t write_req[],
                          uint8_t count, void *priv_data, esp_rmaker_write_ctx_t *ctx)
{
    dev_id_t id = (dev_id_t)(uintptr_t)priv_data;
    for (int i = 0; i < count; i++) {
        const esp_rmaker_param_t *param = write_req[i].param;
        const char *name = esp_rmaker_param_get_name(param);
        esp_rmaker_param_val_t val = write_req[i].val;
        ESP_LOGI(TAG, "%s/%s written via %s", esp_rmaker_device_get_name(device), name,
                 ctx ? esp_rmaker_device_cb_src_to_str(ctx->src) : "?");

        if (id == DEV_ID_ALARM && strcmp(name, ESP_RMAKER_DEF_POWER_NAME) == 0) {
            post_ctrl(CTRL_ALARM_ARM, val.val.b);
        } else if (id == DEV_ID_TEMP && strcmp(name, PARAM_THRESHOLD) == 0) {
            g_temp_threshold_c = val.val.i;
        } else if (id == DEV_ID_HUM && strcmp(name, PARAM_THRESHOLD) == 0) {
            g_hum_threshold_pct = val.val.i;
        } else {
            continue;
        }
        esp_rmaker_param_update(param, val);
    }
    return ESP_OK;
}

static esp_rmaker_param_t *threshold_param_create(int def, int min, int max)
{
    esp_rmaker_param_t *p = esp_rmaker_param_create(PARAM_THRESHOLD, "my.param.threshold", esp_rmaker_int(def),
                                                    PROP_FLAG_READ | PROP_FLAG_WRITE | PROP_FLAG_PERSIST);
    if (p) {
        esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_SLIDER);
        esp_rmaker_param_add_bounds(p, esp_rmaker_int(min), esp_rmaker_int(max), esp_rmaker_int(1));
    }
    return p;
}

// A read-only percentage parameter that the app shows as text and records as a time series.
static esp_rmaker_param_t *percent_param_create(const char *name, const char *type)
{
    esp_rmaker_param_t *p = esp_rmaker_param_create(name, type, esp_rmaker_float(0),
                                                    PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    if (p) {
        esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_TEXT);
        esp_rmaker_param_add_bounds(p, esp_rmaker_float(0), esp_rmaker_float(100), esp_rmaker_float(1));
    }
    return p;
}

static esp_rmaker_param_t *text_param_create(const char *name, const char *type, const char *def)
{
    esp_rmaker_param_t *p = esp_rmaker_param_create(name, type, esp_rmaker_str(def), PROP_FLAG_READ);
    if (p) {
        esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_TEXT);
    }
    return p;
}

static void create_devices(esp_rmaker_node_t *node)
{
    // Temperature sensor: standard type, so voice assistants can answer "what is the temperature".
    g_temp_dev = esp_rmaker_temp_sensor_device_create(DEV_TEMP_NAME, (void *)DEV_ID_TEMP, 25.0f);
    esp_rmaker_device_add_bulk_cb(g_temp_dev, write_cb, (void *)DEV_ID_TEMP);
    esp_rmaker_device_add_param(g_temp_dev, threshold_param_create(DEFAULT_TEMP_THRESHOLD_C, 20, 60));
    esp_rmaker_node_add_device(node, g_temp_dev);

    g_hum_dev = esp_rmaker_device_create(DEV_HUM_NAME, "my.device.humidity-sensor", (void *)DEV_ID_HUM);
    esp_rmaker_device_add_bulk_cb(g_hum_dev, write_cb, (void *)DEV_ID_HUM);
    esp_rmaker_device_add_param(g_hum_dev, esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM, DEV_HUM_NAME));
    esp_rmaker_param_t *hum = percent_param_create(PARAM_HUMIDITY, "my.param.humidity");
    esp_rmaker_device_add_param(g_hum_dev, hum);
    esp_rmaker_device_assign_primary_param(g_hum_dev, hum);
    esp_rmaker_device_add_param(g_hum_dev, threshold_param_create(DEFAULT_HUM_THRESHOLD_PCT, 30, 100));
    esp_rmaker_node_add_device(node, g_hum_dev);

    g_air_dev = esp_rmaker_device_create(DEV_AIR_NAME, "my.device.air-quality", (void *)DEV_ID_AIR);
    esp_rmaker_device_add_bulk_cb(g_air_dev, write_cb, (void *)DEV_ID_AIR);
    esp_rmaker_device_add_param(g_air_dev, esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM, DEV_AIR_NAME));
    // The MQ-135 gives an on/off reading (its trimmer pot sets the trip point), so only a status is shown.
    esp_rmaker_param_t *air = text_param_create(PARAM_STATUS, "my.param.air-quality", "GOOD");
    esp_rmaker_device_add_param(g_air_dev, air);
    esp_rmaker_device_assign_primary_param(g_air_dev, air);
    esp_rmaker_node_add_device(node, g_air_dev);

    // Power = alarm armed (buzzer enabled). Status shows what is currently triggering it.
    g_alarm_dev = esp_rmaker_switch_device_create(DEV_ALARM_NAME, (void *)DEV_ID_ALARM, true);
    esp_rmaker_device_add_bulk_cb(g_alarm_dev, write_cb, (void *)DEV_ID_ALARM);
    esp_rmaker_device_add_param(g_alarm_dev, text_param_create(PARAM_STATUS, "my.param.status", "Normal"));
    esp_rmaker_node_add_device(node, g_alarm_dev);
}

void app_main(void)
{
    // Must run before Wi-Fi and Bluetooth start. The Bluetooth address follows the base MAC.
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    mac[4] += BLE_ADDR_REROLL;
    mac[5] ^= 0x5A;
    ESP_ERROR_CHECK(esp_base_mac_addr_set(mac));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // The Wi-Fi driver prints a warning on every failed reconnect, which floods the console and slows the node.
    esp_log_level_set("wifi", ESP_LOG_ERROR);

#if PROV_DEBUG_LOGS
    esp_log_level_set("protocomm", ESP_LOG_DEBUG);
    esp_log_level_set("protocomm_nimble", ESP_LOG_DEBUG);
    esp_log_level_set("network_prov_mgr", ESP_LOG_DEBUG);
    esp_log_level_set("network_prov_scheme_ble", ESP_LOG_DEBUG);
    esp_log_level_set("security1", ESP_LOG_DEBUG);
    esp_log_level_set("security2", ESP_LOG_DEBUG);
#endif

    // Queues must exist before the node is created: persisted parameters invoke write_cb during creation.
    g_events = xEventGroupCreate();
    g_cloud_q = xQueueCreate(4, sizeof(sensor_sample_t));
    g_alert_q = xQueueCreate(4, sizeof(sensor_sample_t));
    g_ctrl_q = xQueueCreate(8, sizeof(ctrl_msg_t));
    ESP_ERROR_CHECK((g_events && g_cloud_q && g_alert_q && g_ctrl_q) ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(control_start());

    app_network_init();

    // Time series history in the app needs a valid clock, so the agent syncs SNTP before connecting.
    esp_rmaker_config_t rainmaker_cfg = { .enable_time_sync = true };
    esp_rmaker_node_t *node = esp_rmaker_node_init(&rainmaker_cfg, "Environmental Logger", "Environmental Data Logger");
    if (!node) {
        ESP_LOGE(TAG, "could not initialise the RainMaker node");
        abort();
    }
    create_devices(node);

    esp_rmaker_ota_enable_default();
    esp_rmaker_timezone_service_enable();

    esp_rmaker_system_serv_config_t system_cfg = {
        .flags = SYSTEM_SERV_FLAGS_ALL,
        .reboot_seconds = 2,
        .reset_seconds = 2,
        .reset_reboot_seconds = 2,
    };
    esp_rmaker_system_service_enable(&system_cfg);

    app_insights_enable();

    // Local monitoring and alarms start before the network and keep working while it is down.
    ESP_ERROR_CHECK(ota_start());
    ESP_ERROR_CHECK(connect_start());
    ESP_ERROR_CHECK(cloud_start());
    ESP_ERROR_CHECK(alert_start());
    ESP_ERROR_CHECK(sensors_start());

    esp_rmaker_start();

    // The RainMaker Home app identifies the device by this BLE advertisement data.
    app_network_set_custom_mfg_data(MFG_DATA_DEVICE_TYPE_SWITCH, MFG_DATA_DEVICE_SUBTYPE_SWITCH);

    // Asynchronous: returns at once. Stored credentials are used automatically after a power outage,
    // otherwise BLE provisioning starts.
    ESP_ERROR_CHECK(app_network_start(POP_TYPE_RANDOM));
}
