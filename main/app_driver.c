#include "app_driver.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

static const char *TAG = "app_driver";

// Hardware constants from Kconfig or defaults
#ifndef CONFIG_LED_CW_GPIO
#define CONFIG_LED_CW_GPIO 0
#endif

#ifndef CONFIG_LED_WW_GPIO
#define CONFIG_LED_WW_GPIO 1
#endif

#ifndef CONFIG_STATUS_LED_GPIO
#define CONFIG_STATUS_LED_GPIO 8
#endif

#ifndef CONFIG_STATUS_LED_ACTIVE_LEVEL
#define CONFIG_STATUS_LED_ACTIVE_LEVEL 0
#endif

#ifndef CONFIG_BUTTON_GPIO
#define CONFIG_BUTTON_GPIO 9
#endif

#ifndef CONFIG_PWM_FREQUENCY_HZ
#define CONFIG_PWM_FREQUENCY_HZ 22000
#endif

#ifndef CONFIG_MIN_COLOR_TEMP_MIREDS
#define CONFIG_MIN_COLOR_TEMP_MIREDS 153 // 6500K
#endif

#ifndef CONFIG_MAX_COLOR_TEMP_MIREDS
#define CONFIG_MAX_COLOR_TEMP_MIREDS 370 // 2700K
#endif

#ifndef CONFIG_FACTORY_RESET_HOLD_TIME_SEC
#define CONFIG_FACTORY_RESET_HOLD_TIME_SEC 10
#endif

#define LEDC_RESOLUTION       LEDC_TIMER_11_BIT
#define LEDC_MAX_DUTY         ((1 << 11) - 1) // 2047
#define LEDC_CW_CHANNEL       LEDC_CHANNEL_0
#define LEDC_WW_CHANNEL       LEDC_CHANNEL_1

// Internal state
static bool s_power = false;
static uint8_t s_brightness = 254; // 1-254
static uint16_t s_color_temp_mireds = 250; // ~4000K
static status_led_mode_t s_status_mode = STATUS_LED_MODE_BLINK_FAST;
static app_driver_button_callback_t s_button_cb = NULL;

// Apply gamma correction (gamma 2.2)
static inline float apply_gamma(float linear_val)
{
    if (linear_val <= 0.0f) return 0.0f;
    if (linear_val >= 1.0f) return 1.0f;
    return powf(linear_val, 2.2f);
}

static void apply_pwm_outputs(void)
{
    if (!s_power || s_brightness == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL);
        return;
    }

    // Normalized brightness 0.0 to 1.0
    float norm_bright = (float)s_brightness / 254.0f;

    // Clamp color temperature within mired bounds
    uint16_t clamped_mireds = s_color_temp_mireds;
    if (clamped_mireds < CONFIG_MIN_COLOR_TEMP_MIREDS) {
        clamped_mireds = CONFIG_MIN_COLOR_TEMP_MIREDS;
    }
    if (clamped_mireds > CONFIG_MAX_COLOR_TEMP_MIREDS) {
        clamped_mireds = CONFIG_MAX_COLOR_TEMP_MIREDS;
    }

    // Temp factor: 0.0 = Coolest (MIN_MIREDS), 1.0 = Warmest (MAX_MIREDS)
    float temp_ratio = (float)(clamped_mireds - CONFIG_MIN_COLOR_TEMP_MIREDS) /
                       (float)(CONFIG_MAX_COLOR_TEMP_MIREDS - CONFIG_MIN_COLOR_TEMP_MIREDS);

    float ww_linear = norm_bright * temp_ratio;
    float cw_linear = norm_bright * (1.0f - temp_ratio);

    float cw_gamma = apply_gamma(cw_linear);
    float ww_gamma = apply_gamma(ww_linear);

    uint32_t cw_duty = (uint32_t)(cw_gamma * (float)LEDC_MAX_DUTY);
    uint32_t ww_duty = (uint32_t)(ww_gamma * (float)LEDC_MAX_DUTY);

    if (cw_duty > LEDC_MAX_DUTY) cw_duty = LEDC_MAX_DUTY;
    if (ww_duty > LEDC_MAX_DUTY) ww_duty = LEDC_MAX_DUTY;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL, cw_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL, ww_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL);

    ESP_LOGD(TAG, "PWM updated: Power=%d, Bright=%d, Mireds=%d -> CW_Duty=%lu, WW_Duty=%lu",
             s_power, s_brightness, s_color_temp_mireds, (unsigned long)cw_duty, (unsigned long)ww_duty);
}

esp_err_t app_driver_light_set_power(bool on)
{
    s_power = on;
    apply_pwm_outputs();
    return ESP_OK;
}

esp_err_t app_driver_light_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
    apply_pwm_outputs();
    return ESP_OK;
}

esp_err_t app_driver_light_set_color_temp(uint16_t color_temp_mireds)
{
    s_color_temp_mireds = color_temp_mireds;
    apply_pwm_outputs();
    return ESP_OK;
}

esp_err_t app_driver_light_update(bool on, uint8_t brightness, uint16_t color_temp_mireds)
{
    s_power = on;
    s_brightness = brightness;
    s_color_temp_mireds = color_temp_mireds;
    apply_pwm_outputs();
    return ESP_OK;
}

void app_driver_status_led_set_mode(status_led_mode_t mode)
{
    s_status_mode = mode;
}

static void status_led_task(void *pvParameters)
{
    bool led_state = false;
    while (1) {
        uint32_t delay_ms = 500;
        switch (s_status_mode) {
        case STATUS_LED_MODE_OFF:
            gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO, !CONFIG_STATUS_LED_ACTIVE_LEVEL);
            delay_ms = 500;
            break;
        case STATUS_LED_MODE_ON:
            gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO, CONFIG_STATUS_LED_ACTIVE_LEVEL);
            delay_ms = 500;
            break;
        case STATUS_LED_MODE_BLINK_FAST:
            led_state = !led_state;
            gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO,
                           led_state ? CONFIG_STATUS_LED_ACTIVE_LEVEL : !CONFIG_STATUS_LED_ACTIVE_LEVEL);
            delay_ms = 200;
            break;
        case STATUS_LED_MODE_BLINK_SLOW:
            led_state = !led_state;
            gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO,
                           led_state ? CONFIG_STATUS_LED_ACTIVE_LEVEL : !CONFIG_STATUS_LED_ACTIVE_LEVEL);
            delay_ms = 800;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void button_task(void *pvParameters)
{
    int last_level = 1;
    uint32_t press_start_tick = 0;
    bool reset_triggered = false;

    while (1) {
        int level = gpio_get_level((gpio_num_t)CONFIG_BUTTON_GPIO);
        uint32_t now = xTaskGetTickCount();

        if (level == 0 && last_level == 1) {
            // Button pressed down (active-low)
            press_start_tick = now;
            reset_triggered = false;
        } else if (level == 0 && last_level == 0) {
            // Button still held down
            uint32_t elapsed_ms = (now - press_start_tick) * portTICK_PERIOD_MS;
            if (!reset_triggered && elapsed_ms >= (CONFIG_FACTORY_RESET_HOLD_TIME_SEC * 1000)) {
                reset_triggered = true;
                ESP_LOGW(TAG, "Factory reset button threshold reached (%ds)!", CONFIG_FACTORY_RESET_HOLD_TIME_SEC);
                if (s_button_cb) {
                    s_button_cb(false, true);
                }
            }
        } else if (level == 1 && last_level == 0) {
            // Button released
            uint32_t elapsed_ms = (now - press_start_tick) * portTICK_PERIOD_MS;
            if (!reset_triggered && elapsed_ms >= 50 && elapsed_ms < 1500) {
                ESP_LOGI(TAG, "Button short press detected (%lums) -> Toggle Light", (unsigned long)elapsed_ms);
                if (s_button_cb) {
                    s_button_cb(true, false);
                }
            }
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(20)); // 20ms debounce polling
    }
}

esp_err_t app_driver_init(app_driver_button_callback_t btn_cb)
{
    s_button_cb = btn_cb;

    // 1. Initialize LEDC Timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_RESOLUTION,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = CONFIG_PWM_FREQUENCY_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 2. Initialize Cool White Channel (GPIO 0)
    ledc_channel_config_t cw_channel = {
        .gpio_num       = CONFIG_LED_CW_GPIO,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CW_CHANNEL,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0,
        .hpoint         = 0,
    };
    err = ledc_channel_config(&cw_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config CW failed: %s", esp_err_to_name(err));
        return err;
    }

    // 3. Initialize Warm White Channel (GPIO 1)
    ledc_channel_config_t ww_channel = {
        .gpio_num       = CONFIG_LED_WW_GPIO,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_WW_CHANNEL,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0,
        .hpoint         = 0,
    };
    err = ledc_channel_config(&ww_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config WW failed: %s", esp_err_to_name(err));
        return err;
    }

    // 4. Initialize Status LED (GPIO 8)
    gpio_config_t status_io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&status_io_conf);
    gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO, !CONFIG_STATUS_LED_ACTIVE_LEVEL);

    xTaskCreate(status_led_task, "status_led_task", 2048, NULL, 1, NULL);

    // 5. Initialize Push Button (GPIO 9)
    gpio_config_t btn_io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_io_conf);

    xTaskCreate(button_task, "button_task", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Hardware driver initialized (CW=GPIO %d, WW=GPIO %d, Freq=%d Hz, Status=GPIO %d, Btn=GPIO %d)",
             CONFIG_LED_CW_GPIO, CONFIG_LED_WW_GPIO, CONFIG_PWM_FREQUENCY_HZ,
             CONFIG_STATUS_LED_GPIO, CONFIG_BUTTON_GPIO);

    return ESP_OK;
}
