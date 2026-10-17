#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_ota.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <app/server/Dnssd.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "app_driver.h"
#include "mac_config.h"

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "app_main";

static uint16_t light_endpoint_id = 0;
static bool s_light_power = false;
static esp_timer_handle_t s_wifi_keepalive_timer = NULL;

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type,
                                         uint16_t endpoint_id,
                                         uint32_t cluster_id,
                                         uint32_t attribute_id,
                                         esp_matter_attr_val_t *val,
                                         void *priv_data)
{
    if (type == PRE_UPDATE) {
        if (endpoint_id == light_endpoint_id) {
            if (cluster_id == OnOff::Id) {
                if (attribute_id == OnOff::Attributes::OnOff::Id) {
                    s_light_power = val->val.b;
                    ESP_LOGI(TAG, "Matter OnOff updated: %d", val->val.b);
                    app_driver_light_set_power(val->val.b);
                }
            } else if (cluster_id == LevelControl::Id) {
                if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
                    // Ignore transient CurrentLevel=1 emitted by LevelControl::OnOnOffChanged during On/Off transitions
                    if (!s_light_power && val->val.u8 <= 1) {
                        ESP_LOGD(TAG, "Ignoring internal off-transition CurrentLevel=%u while power is OFF", val->val.u8);
                        return ESP_OK;
                    }
                    ESP_LOGI(TAG, "Matter LevelControl updated: %u", val->val.u8);
                    app_driver_light_set_brightness(val->val.u8);
                }
            } else if (cluster_id == ColorControl::Id) {
                if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
                    ESP_LOGI(TAG, "Matter ColorTemp Mireds updated: %u", val->val.u16);
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

static void wifi_keepalive_timer_cb(void *arg)
{
    // 1. Ensure Wi-Fi power saving stays disabled so multicast mDNS / IPv6 NDP never drop
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 2. Check station link health and trigger reconnect if dropped
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
            ESP_LOGW(TAG, "Wi-Fi link down; triggering station reconnect...");
            esp_wifi_connect();
        }
        return;
    }

    // 3. Re-announce operational mDNS service records on CHIP thread for Apple Home Hub reachability
    CHIP_ERROR cerr = chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
            chip::app::DnssdServer::Instance().StartServer();
        }
    }, 0);
    if (cerr != CHIP_NO_ERROR) {
        ESP_LOGD(TAG, "mDNS refresh schedule skipped: %" CHIP_ERROR_FORMAT, cerr.Format());
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete!");
        app_driver_status_led_set_mode(STATUS_LED_MODE_OFF);
        esp_wifi_set_ps(WIFI_PS_NONE);
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            app_driver_status_led_set_mode(STATUS_LED_MODE_BLINK_FAST);
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
        ESP_LOGI(TAG, "Wi-Fi connectivity changed");
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
        esp_wifi_set_max_tx_power(80);
        break;

    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "IP address changed -> refreshing mDNS operational advertisement");
        esp_wifi_set_ps(WIFI_PS_NONE);
        chip::app::DnssdServer::Instance().StartServer();
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
        CHIP_ERROR cerr = chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) {
            node_t *node = node::get();
            endpoint_t *ep = endpoint::get(node, light_endpoint_id);
            if (!ep) return;
            cluster_t *cl = cluster::get(ep, OnOff::Id);
            if (!cl) return;
            attribute_t *attr = attribute::get(cl, OnOff::Attributes::OnOff::Id);
            if (!attr) return;

            esp_matter_attr_val_t val = esp_matter_invalid(NULL);
            attribute::get_val(attr, &val);

            val.val.b = !val.val.b;
            ESP_LOGI(TAG, "Toggling On/Off to %d via button press", val.val.b);

            attribute::update(light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
        }, 0);
        if (cerr != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to schedule button toggle: %" CHIP_ERROR_FORMAT, cerr.Format());
        }
    } else if (long_press_reset) {
        ESP_LOGW(TAG, "Triggering Matter Factory Reset from button long press...");
        app_driver_status_led_set_mode(STATUS_LED_MODE_BLINK_FAST);
        CHIP_ERROR cerr = chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) {
            factory_reset();
        }, 0);
        if (cerr != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to schedule factory reset: %" CHIP_ERROR_FORMAT, cerr.Format());
        }
    }
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "===============================================");
    ESP_LOGI(TAG, "  ESP32 (" CONFIG_IDF_TARGET ") Matter CW-WW LED Controller Startup ");
    ESP_LOGI(TAG, "===============================================");

    // 1. Initialize NVS
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 2. Initialize Custom MAC Address & Wi-Fi Robustness Handler
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

    // 5. Create Color Temperature Light Endpoint with Full 50-400 Mired Spectrum & NVS State Restoration
    color_temperature_light::config_t light_config;
    light_config.on_off.on_off = false;
    light_config.on_off_lighting.start_up_on_off = nullptr; // Restore previous OnOff state from NVS
    light_config.level_control.current_level = 254;
    light_config.level_control.on_level = nullptr;
    light_config.level_control_lighting.start_up_current_level = nullptr; // Restore previous brightness from NVS
    light_config.color_control.color_mode = 2; // ColorMode::kColorTemperatureMireds
    light_config.color_control.enhanced_color_mode = 2; // EnhancedColorMode::kColorTemperatureMireds
    light_config.color_control_color_temperature.color_temperature_mireds = 225; // Neutral white midpoint
    light_config.color_control_color_temperature.color_temp_physical_min_mireds = CONFIG_MIN_COLOR_TEMP_MIREDS;
    light_config.color_control_color_temperature.color_temp_physical_max_mireds = CONFIG_MAX_COLOR_TEMP_MIREDS;
    light_config.color_control_color_temperature.couple_color_temp_to_level_min_mireds = CONFIG_MIN_COLOR_TEMP_MIREDS;
    light_config.color_control_color_temperature.start_up_color_temperature_mireds = nullptr; // Restore previous ColorTemp

    endpoint_t *endpoint = color_temperature_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create Light endpoint");
        return;
    }

    light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Color Temperature Light created with endpoint_id: %d (Mireds: %d..%d)",
             light_endpoint_id, CONFIG_MIN_COLOR_TEMP_MIREDS, CONFIG_MAX_COLOR_TEMP_MIREDS);

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

    // 8. Force-sync physical Mired bounds & NVS startup attributes in case NVS held legacy 153..370 bounds
    CHIP_ERROR sync_err = chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        esp_matter_attr_val_t min_val = esp_matter_uint16(CONFIG_MIN_COLOR_TEMP_MIREDS);
        esp_matter_attr_val_t max_val = esp_matter_uint16(CONFIG_MAX_COLOR_TEMP_MIREDS);
        attribute::update(light_endpoint_id, ColorControl::Id,
                          ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, &min_val);
        attribute::update(light_endpoint_id, ColorControl::Id,
                          ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, &max_val);
        attribute::update(light_endpoint_id, ColorControl::Id,
                          ColorControl::Attributes::CoupleColorTempToLevelMinMireds::Id, &min_val);

        esp_matter_attr_val_t null_u8 = esp_matter_nullable_uint8(nullable<uint8_t>());
        attribute::update(light_endpoint_id, LevelControl::Id,
                          LevelControl::Attributes::StartUpCurrentLevel::Id, &null_u8);
        attribute::update(light_endpoint_id, OnOff::Id,
                          OnOff::Attributes::StartUpOnOff::Id, &null_u8);

        esp_matter_attr_val_t null_u16 = esp_matter_nullable_uint16(nullable<uint16_t>());
        attribute::update(light_endpoint_id, ColorControl::Id,
                          ColorControl::Attributes::StartUpColorTemperatureMireds::Id, &null_u16);

        // Read back restored state and synchronize hardware driver
        node_t *active_node = node::get();
        endpoint_t *active_ep = endpoint::get(active_node, light_endpoint_id);
        if (active_ep) {
            bool restored_power = false;
            uint8_t restored_brightness = 254;
            uint16_t restored_mireds = 225;

            cluster_t *on_off_cl = cluster::get(active_ep, OnOff::Id);
            if (on_off_cl) {
                attribute_t *attr = attribute::get(on_off_cl, OnOff::Attributes::OnOff::Id);
                esp_matter_attr_val_t val = esp_matter_invalid(NULL);
                if (attribute::get_val(attr, &val) == ESP_OK) {
                    restored_power = val.val.b;
                }
            }

            cluster_t *lvl_cl = cluster::get(active_ep, LevelControl::Id);
            if (lvl_cl) {
                attribute_t *attr = attribute::get(lvl_cl, LevelControl::Attributes::CurrentLevel::Id);
                esp_matter_attr_val_t val = esp_matter_invalid(NULL);
                if (attribute::get_val(attr, &val) == ESP_OK) {
                    restored_brightness = val.val.u8;
                }
                // Recover from legacy StartUpCurrentLevel=0 bug that clamped CurrentLevel to 1
                if (restored_brightness <= 1) {
                    restored_brightness = 254;
                    esp_matter_attr_val_t full_lvl = esp_matter_nullable_uint8(254);
                    attribute::update(light_endpoint_id, LevelControl::Id,
                                      LevelControl::Attributes::CurrentLevel::Id, &full_lvl);
                }
            }

            cluster_t *color_cl = cluster::get(active_ep, ColorControl::Id);
            if (color_cl) {
                attribute_t *attr = attribute::get(color_cl, ColorControl::Attributes::ColorTemperatureMireds::Id);
                esp_matter_attr_val_t val = esp_matter_invalid(NULL);
                if (attribute::get_val(attr, &val) == ESP_OK) {
                    restored_mireds = val.val.u16;
                }
            }

            s_light_power = restored_power;
            ESP_LOGI(TAG, "Restored state on startup: Power=%d, Brightness=%u, ColorTemp=%u Mireds (Range %d..%d)",
                     restored_power, restored_brightness, restored_mireds,
                     CONFIG_MIN_COLOR_TEMP_MIREDS, CONFIG_MAX_COLOR_TEMP_MIREDS);
            app_driver_light_update(restored_power, restored_brightness, restored_mireds);
        }
    }, 0);
    if (sync_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to schedule startup attribute sync: %" CHIP_ERROR_FORMAT, sync_err.Format());
    }

    // 9. Start 60s Wi-Fi & mDNS keepalive timer for Apple Home responsiveness
    esp_timer_create_args_t keepalive_args = {
        .callback = wifi_keepalive_timer_cb,
        .name = "wifi_keepalive"
    };
    if (esp_timer_create(&keepalive_args, &s_wifi_keepalive_timer) == ESP_OK) {
        esp_timer_start_periodic(s_wifi_keepalive_timer, 60ULL * 1000000ULL);
    }

    // 10. Cancel OTA rollback after successful startup
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_ota_mark_app_valid_cancel_rollback();
#endif

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
