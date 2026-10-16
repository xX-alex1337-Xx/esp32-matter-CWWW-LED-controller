#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_ota.h>
#include <nvs_flash.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "app_driver.h"
#include "mac_config.h"

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "app_main";

static uint16_t light_endpoint_id = 0;

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type,
                                         uint16_t endpoint_id,
                                         uint32_t cluster_id,
                                         uint32_t attribute_id,
                                         esp_matter_attr_val_t *val,
                                         void *priv_data)
{
    if (type == PRE_UPDATE) {
        // Driver update on pre-update to keep hardware responsive
        if (endpoint_id == light_endpoint_id) {
            if (cluster_id == OnOff::Id) {
                if (attribute_id == OnOff::Attributes::OnOff::Id) {
                    ESP_LOGI(TAG, "Matter OnOff updated: %d", val->val.b);
                    app_driver_light_set_power(val->val.b);
                }
            } else if (cluster_id == LevelControl::Id) {
                if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
                    ESP_LOGI(TAG, "Matter LevelControl updated: %d", val->val.u8);
                    app_driver_light_set_brightness(val->val.u8);
                }
            } else if (cluster_id == ColorControl::Id) {
                if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
                    ESP_LOGI(TAG, "Matter ColorTemp Mireds updated: %d", val->val.u16);
                    app_driver_light_set_color_temp(val->val.u16);
                }
            }
        }
    }
    return ESP_OK;
}

static esp_err_t app_identification_cb(identification::callback_type_t type,
                                       uint16_t endpoint_id,
                                       uint8_t effect_id,
                                       uint8_t effect_variant,
                                       void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback type: %u, effect: %u", type, effect_id);
    if (type == identification::callback_type_t::START) {
        app_driver_status_led_set_mode(STATUS_LED_MODE_BLINK_SLOW);
    } else if (type == identification::callback_type_t::STOP) {
        app_driver_status_led_set_mode(STATUS_LED_MODE_OFF);
    }
    return ESP_OK;
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete!");
        app_driver_status_led_set_mode(STATUS_LED_MODE_OFF);
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            // No fabrics remaining, revert to fast blink pairing mode
            app_driver_status_led_set_mode(STATUS_LED_MODE_BLINK_FAST);
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEAdvertisingChange:
        ESP_LOGI(TAG, "BLE advertising state changed");
        break;

    default:
        break;
    }
}

static void button_callback(bool short_press_toggle, bool long_press_reset)
{
    if (short_press_toggle) {
        // Read current On/Off attribute
        node_t *node = node::get();
        endpoint_t *ep = endpoint::get(node, light_endpoint_id);
        cluster_t *cl = cluster::get(ep, OnOff::Id);
        attribute_t *attr = attribute::get(cl, OnOff::Attributes::OnOff::Id);

        esp_matter_attr_val_t val = esp_matter_invalid(NULL);
        attribute::get_val(attr, &val);

        // Toggle state
        val.val.b = !val.val.b;
        ESP_LOGI(TAG, "Toggling On/Off to %d via button press", val.val.b);

        // Update attribute in Matter data model (triggers attribute update callback & reports to fabric)
        attribute::update(light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
    } else if (long_press_reset) {
        ESP_LOGW(TAG, "Triggering Matter Factory Reset from button long press...");
        app_driver_status_led_set_mode(STATUS_LED_MODE_BLINK_FAST);
        factory_reset();
    }
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "===============================================");
    ESP_LOGI(TAG, "  ESP32-C3 Matter CW-WW LED Controller Startup ");
    ESP_LOGI(TAG, "===============================================");

    // 1. Initialize NVS
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 2. Initialize Custom MAC Address (before Wi-Fi/BLE network initialization)
    err = mac_config_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Custom MAC init returned %s", esp_err_to_name(err));
    }

    // 3. Initialize Hardware Drivers (PWM, Status LED, Button)
    err = app_driver_init(button_callback);
    ESP_ERROR_CHECK(err);

    // 4. Create Matter Node
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    // 5. Create Color Temperature Light Endpoint
    color_temperature_light::config_t light_config;
    light_config.on_off.on_off = false;
    light_config.level_control.current_level = 254;
    light_config.color_control_color_temperature.color_temperature_mireds = 250; // ~4000K
    light_config.color_control_color_temperature.color_temp_physical_min_mireds = CONFIG_MIN_COLOR_TEMP_MIREDS;
    light_config.color_control_color_temperature.color_temp_physical_max_mireds = CONFIG_MAX_COLOR_TEMP_MIREDS;
    light_config.color_control_color_temperature.couple_color_temp_to_level_min_mireds = CONFIG_MIN_COLOR_TEMP_MIREDS;
    light_config.color_control_color_temperature.start_up_color_temperature_mireds = 250;

    endpoint_t *endpoint = color_temperature_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create Light endpoint");
        return;
    }

    light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Color Temperature Light created with endpoint_id: %d", light_endpoint_id);

    // 6. Initialize OTA Requestor
#if CONFIG_ENABLE_OTA_REQUESTOR
    esp_matter_ota_requestor_init();
#endif

    // 7. Start Matter Stack
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter stack: %s", esp_err_to_name(err));
        return;
    }

    // Check commissioning status to set initial status LED pattern
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        app_driver_status_led_set_mode(STATUS_LED_MODE_BLINK_FAST);
        ESP_LOGI(TAG, "Device is uncommissioned. In pairing mode (BLE advertising).");
    } else {
        app_driver_status_led_set_mode(STATUS_LED_MODE_OFF);
        ESP_LOGI(TAG, "Device is already commissioned to a Matter fabric.");
    }

    // Print setup pairing codes and instructions
    char active_mac[32] = {0};
    mac_config_get_current_str(active_mac, sizeof(active_mac));
    ESP_LOGI(TAG, "Active Wi-Fi Station MAC: %s", active_mac);

    PrintOnboardingCodes(chip::RendezvousInformationFlag::kBLE);
}
