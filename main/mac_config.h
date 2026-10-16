#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize custom MAC address handling.
 *
 * Checks NVS partition for key 'custom_mac'. If not found, falls back to
 * CONFIG_CUSTOM_MAC_ADDRESS from Kconfig if CONFIG_CUSTOM_MAC_ENABLE is set.
 * Applies the base MAC address via esp_base_mac_addr_set() before network init.
 *
 * @return esp_err_t ESP_OK on success, or error code
 */
esp_err_t mac_config_init(void);

/**
 * @brief Store a custom MAC address into NVS flash.
 *
 * @param mac_str MAC address string formatted as "XX:XX:XX:XX:XX:XX"
 * @return esp_err_t ESP_OK on success, or error code
 */
esp_err_t mac_config_set_in_nvs(const char *mac_str);

/**
 * @brief Get the currently active Base MAC address as a formatted string.
 *
 * @param mac_str Buffer of at least 18 bytes to receive "XX:XX:XX:XX:XX:XX"
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mac_config_get_current_str(char *mac_str, size_t max_len);

#ifdef __cplusplus
}
#endif
