#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_driver_button_callback_t)(bool short_press_toggle, bool long_press_reset);

/**
 * @brief Initialize all hardware drivers (LEDC PWM, Status LED, Button).
 *
 * @param btn_cb Callback invoked on button events (short press / long press)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t app_driver_init(app_driver_button_callback_t btn_cb);

/**
 * @brief Set the light output state.
 *
 * @param on True if light is ON, false if OFF
 * @param brightness Brightness level (1 - 254)
 * @param color_temp_mireds Color temperature in mireds (153 - 370)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t app_driver_light_set_power(bool on);
esp_err_t app_driver_light_set_brightness(uint8_t brightness);
esp_err_t app_driver_light_set_color_temp(uint16_t color_temp_mireds);
esp_err_t app_driver_light_update(bool on, uint8_t brightness, uint16_t color_temp_mireds);

/**
 * @brief Status LED modes.
 */
typedef enum {
    STATUS_LED_MODE_OFF,
    STATUS_LED_MODE_ON,
    STATUS_LED_MODE_BLINK_FAST,  // Commissioning / Pairing mode
    STATUS_LED_MODE_BLINK_SLOW   // Connecting / Identifying mode
} status_led_mode_t;

/**
 * @brief Update the status LED indication mode.
 */
void app_driver_status_led_set_mode(status_led_mode_t mode);

#ifdef __cplusplus
}
#endif
