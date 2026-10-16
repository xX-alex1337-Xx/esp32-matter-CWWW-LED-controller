#include "mac_config.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "mac_config";
static const char *NVS_NAMESPACE = "custom_cfg";
static const char *NVS_KEY_MAC = "custom_mac";

static bool s_custom_mac_valid = false;
static uint8_t s_custom_mac[6] = {0};

static bool parse_mac_string(const char *str, uint8_t mac[6])
{
    if (!str || strlen(str) != 17) {
        return false;
    }

    unsigned int b[6];
    char sep;
    int parsed = sscanf(str, "%2x%c%2x%c%2x%c%2x%c%2x%c%2x",
                        &b[0], &sep, &b[1], &sep, &b[2], &sep,
                        &b[3], &sep, &b[4], &sep, &b[5]);

    if (parsed == 11) {
        for (int i = 0; i < 6; i++) {
            if (b[i] > 0xFF) {
                ESP_LOGE(TAG, "Invalid MAC byte value: 0x%X", b[i]);
                return false;
            }
            mac[i] = (uint8_t)b[i];
        }

        // Bit 0 of byte 0 must be 0 (unicast)
        if (mac[0] & 0x01) {
            ESP_LOGE(TAG, "Invalid MAC: multicast bit is set (byte 0 = 0x%02X)", mac[0]);
            return false;
        }

        // Bit 1 of byte 0 should be 1 (locally administered)
        if (!(mac[0] & 0x02)) {
            ESP_LOGW(TAG, "Warning: Custom MAC byte 0 (0x%02X) does not have locally administered bit set (bit 1)", mac[0]);
        }

        return true;
    }

    return false;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_custom_mac_valid) {
            esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, s_custom_mac);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Wi-Fi Station MAC overridden to: %02X:%02X:%02X:%02X:%02X:%02X",
                         s_custom_mac[0], s_custom_mac[1], s_custom_mac[2],
                         s_custom_mac[3], s_custom_mac[4], s_custom_mac[5]);
            } else {
                ESP_LOGE(TAG, "esp_wifi_set_mac failed: %s", esp_err_to_name(err));
            }
        }
    }
}

esp_err_t mac_config_set_in_nvs(const char *mac_str)
{
    uint8_t dummy_mac[6];
    if (!parse_mac_string(mac_str, dummy_mac)) {
        ESP_LOGE(TAG, "Invalid MAC address string: %s", mac_str ? mac_str : "NULL");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_MAC, mac_str);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        ESP_LOGI(TAG, "Saved custom MAC %s to NVS", mac_str);
    } else {
        ESP_LOGE(TAG, "Failed to write MAC to NVS: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t mac_config_init(void)
{
    uint8_t original_mac[6] = {0};
    esp_read_mac(original_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "Factory eFuse Wi-Fi STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             original_mac[0], original_mac[1], original_mac[2],
             original_mac[3], original_mac[4], original_mac[5]);

    char mac_str[32] = {0};
    bool found_in_nvs = false;

    // 1. Try reading from NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t required_size = sizeof(mac_str);
        err = nvs_get_str(handle, NVS_KEY_MAC, mac_str, &required_size);
        if (err == ESP_OK && strlen(mac_str) > 0) {
            ESP_LOGI(TAG, "Read custom MAC from NVS: %s", mac_str);
            found_in_nvs = true;
        }
        nvs_close(handle);
    }

    // 2. Fall back to Kconfig if enabled and not in NVS
#if defined(CONFIG_CUSTOM_MAC_ENABLE) && CONFIG_CUSTOM_MAC_ENABLE
    if (!found_in_nvs) {
        const char *kconfig_mac = CONFIG_CUSTOM_MAC_ADDRESS;
        if (kconfig_mac && strlen(kconfig_mac) > 0) {
            strncpy(mac_str, kconfig_mac, sizeof(mac_str) - 1);
            ESP_LOGI(TAG, "Using custom MAC from Kconfig: %s", mac_str);
        }
    }
#endif

    // 3. Apply custom MAC if available
    if (strlen(mac_str) > 0) {
        if (parse_mac_string(mac_str, s_custom_mac)) {
            s_custom_mac_valid = true;

            // Set base MAC (drives SoftAP and Bluetooth LE)
            err = esp_base_mac_addr_set(s_custom_mac);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Successfully applied custom Base MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                         s_custom_mac[0], s_custom_mac[1], s_custom_mac[2],
                         s_custom_mac[3], s_custom_mac[4], s_custom_mac[5]);
            } else {
                ESP_LOGE(TAG, "esp_base_mac_addr_set failed: %s", esp_err_to_name(err));
            }

            // Ensure default event loop exists before registering event handler
            esp_err_t loop_err = esp_event_loop_create_default();
            if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "esp_event_loop_create_default returned: %s", esp_err_to_name(loop_err));
            }

            // Register event handler to override Wi-Fi STA MAC as soon as station starts
            err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_START, &wifi_event_handler, NULL, NULL);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to register WIFI_EVENT_STA_START handler: %s", esp_err_to_name(err));
            }

            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Provided custom MAC string '%s' is invalid, using eFuse MAC", mac_str);
        }
    } else {
        ESP_LOGI(TAG, "No custom MAC configured, keeping eFuse MAC");
    }

    return ESP_OK;
}

esp_err_t mac_config_get_current_str(char *mac_str, size_t max_len)
{
    if (!mac_str || max_len < 18) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (err == ESP_OK) {
        snprintf(mac_str, max_len, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return err;
}
