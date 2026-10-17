#include "app_driver.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
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
#define CONFIG_PWM_FREQUENCY_HZ 4000
#endif

#ifndef CONFIG_LIGHT_FADE_TIME_MS
#define CONFIG_LIGHT_FADE_TIME_MS 250
#endif

#ifndef CONFIG_MIN_COLOR_TEMP_MIREDS
#define CONFIG_MIN_COLOR_TEMP_MIREDS 50
#endif

#ifndef CONFIG_MAX_COLOR_TEMP_MIREDS
#define CONFIG_MAX_COLOR_TEMP_MIREDS 400
#endif

#ifndef CONFIG_FACTORY_RESET_HOLD_TIME_SEC
#define CONFIG_FACTORY_RESET_HOLD_TIME_SEC 10
#endif

// Select optimal PWM resolution based on frequency
#if (CONFIG_PWM_FREQUENCY_HZ <= 4000)
#define LEDC_RESOLUTION       LEDC_TIMER_13_BIT
#define LEDC_MAX_DUTY         ((1 << 13) - 1) // 8191
#else
#define LEDC_RESOLUTION       LEDC_TIMER_11_BIT
#define LEDC_MAX_DUTY         ((1 << 11) - 1) // 2047
#endif

#define LEDC_CW_CHANNEL       LEDC_CHANNEL_0
#define LEDC_WW_CHANNEL       LEDC_CHANNEL_1

// Ratio-locked software fade tick interval (5 ms = 200 Hz update rate)
#define FADE_TICK_MS          5

// Internal state
static bool s_power = false;
static uint8_t s_brightness = 254;          // 1-254
static uint16_t s_color_temp_mireds = 225;  // Midpoint of 50..400 Mireds

// Fade engine state (protected by s_driver_mutex)
static float s_current_lum = 0.0f;
static float s_target_lum = 0.0f;
static float s_lum_step = 0.0f;

static float s_current_ratio = 0.5f;
static float s_target_ratio = 0.5f;
static float s_ratio_step = 0.0f;

static esp_timer_handle_t s_fade_timer = NULL;
static status_led_mode_t s_status_mode = STATUS_LED_MODE_BLINK_FAST;
static app_driver_button_callback_t s_button_cb = NULL;
static SemaphoreHandle_t s_driver_mutex = NULL;
static TaskHandle_t s_button_task_handle = NULL;
static esp_timer_handle_t s_status_timer = NULL;
static bool s_status_led_blink_state = false;

// Apply gamma correction (gamma 2.2) to normalized brightness (0.0 to 1.0)
static inline float apply_gamma(float linear_val)
{
    if (linear_val <= 0.0f) return 0.0f;
    if (linear_val >= 1.0f) return 1.0f;
    return powf(linear_val, 2.2f);
}

static void write_pwm_channels(float lum, float temp_ratio)
{
    uint32_t cw_duty = 0;
    uint32_t ww_duty = 0;

    if (lum > 0.00001f) {
        float cw_linear = lum * (1.0f - temp_ratio);
        float ww_linear = lum * temp_ratio;

        cw_duty = (uint32_t)roundf(cw_linear * (float)LEDC_MAX_DUTY);
        ww_duty = (uint32_t)roundf(ww_linear * (float)LEDC_MAX_DUTY);

        if (cw_duty > LEDC_MAX_DUTY) cw_duty = LEDC_MAX_DUTY;
        if (ww_duty > LEDC_MAX_DUTY) ww_duty = LEDC_MAX_DUTY;

        // Low-brightness floor when power is ON so light never extinguishes at 1% brightness
        if (cw_duty == 0 && ww_duty == 0 && s_power && s_brightness > 0) {
            if (temp_ratio >= 0.5f) {
                ww_duty = 1;
            } else {
                cw_duty = 1;
            }
        }
    }

    // Update both channels synchronously with hpoint = 0 so CW/WW ratio is 100% locked
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL, cw_duty);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL, ww_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL);
}

static void fade_timer_callback(void *arg)
{
    if (xSemaphoreTake(s_driver_mutex, 0) != pdTRUE) {
        return;
    }

    bool lum_done = false;
    bool ratio_done = false;

    // 1. Advance luminance toward target
    if (fabsf(s_target_lum - s_current_lum) <= fabsf(s_lum_step) || s_lum_step == 0.0f) {
        s_current_lum = s_target_lum;
        lum_done = true;
    } else {
        s_current_lum += s_lum_step;
        if (s_current_lum < 0.0f) s_current_lum = 0.0f;
        if (s_current_lum > 1.0f) s_current_lum = 1.0f;
    }

    // 2. Advance color temperature ratio toward target
    if (fabsf(s_target_ratio - s_current_ratio) <= fabsf(s_ratio_step) || s_ratio_step == 0.0f) {
        s_current_ratio = s_target_ratio;
        ratio_done = true;
    } else {
        s_current_ratio += s_ratio_step;
        if (s_current_ratio < 0.0f) s_current_ratio = 0.0f;
        if (s_current_ratio > 1.0f) s_current_ratio = 1.0f;
    }

    write_pwm_channels(s_current_lum, s_current_ratio);

    if (lum_done && ratio_done) {
        esp_timer_stop(s_fade_timer);
    }

    xSemaphoreGive(s_driver_mutex);
}

static void update_fade_targets_locked(void)
{
    // Clamp color temperature within mired bounds [CONFIG_MIN_COLOR_TEMP_MIREDS, CONFIG_MAX_COLOR_TEMP_MIREDS]
    uint16_t clamped_mireds = s_color_temp_mireds;
    if (clamped_mireds < CONFIG_MIN_COLOR_TEMP_MIREDS) {
        clamped_mireds = CONFIG_MIN_COLOR_TEMP_MIREDS;
    }
    if (clamped_mireds > CONFIG_MAX_COLOR_TEMP_MIREDS) {
        clamped_mireds = CONFIG_MAX_COLOR_TEMP_MIREDS;
    }

    float new_ratio = 0.5f;
    if (CONFIG_MAX_COLOR_TEMP_MIREDS > CONFIG_MIN_COLOR_TEMP_MIREDS) {
        new_ratio = (float)(clamped_mireds - CONFIG_MIN_COLOR_TEMP_MIREDS) /
                    (float)(CONFIG_MAX_COLOR_TEMP_MIREDS - CONFIG_MIN_COLOR_TEMP_MIREDS);
    }

    float new_lum = 0.0f;
    if (s_power && s_brightness > 0) {
        // Apply perceptual gamma 2.2 with a minimum floor of 0.002f so both CW and WW
        // maintain their exact color ratio even at 1% brightness
        float norm_bright = (float)s_brightness / 254.0f;
        new_lum = apply_gamma(norm_bright);
        if (new_lum < 0.002f) {
            new_lum = 0.002f;
        }
    }

    // If the light is currently OFF (or just turning ON from dark), lock the color ratio
    // immediately so there is NEVER a color shift while fading from OFF to ON!
    if (s_current_lum <= 0.0025f || !s_power) {
        s_current_ratio = new_ratio;
    }

    s_target_lum = new_lum;
    s_target_ratio = new_ratio;

#if (CONFIG_LIGHT_FADE_TIME_MS > 0)
    if (s_fade_timer) {
        float steps = (float)CONFIG_LIGHT_FADE_TIME_MS / (float)FADE_TICK_MS;
        if (steps < 1.0f) steps = 1.0f;

        s_lum_step = (s_target_lum - s_current_lum) / steps;
        s_ratio_step = (s_target_ratio - s_current_ratio) / steps;

        if (!esp_timer_is_active(s_fade_timer)) {
            esp_timer_start_periodic(s_fade_timer, FADE_TICK_MS * 1000);
        }
    } else {
        s_current_lum = s_target_lum;
        s_current_ratio = s_target_ratio;
        write_pwm_channels(s_current_lum, s_current_ratio);
    }
#else
    s_current_lum = s_target_lum;
    s_current_ratio = s_target_ratio;
    write_pwm_channels(s_current_lum, s_current_ratio);
#endif

    ESP_LOGD(TAG, "Targets updated: Power=%d, Bright=%u, Mireds=%u -> TargetLum=%.4f, TargetRatio=%.3f",
             s_power, s_brightness, s_color_temp_mireds, s_target_lum, s_target_ratio);
}

esp_err_t app_driver_light_set_power(bool on)
{
    if (xSemaphoreTake(s_driver_mutex, portMAX_DELAY) == pdTRUE) {
        s_power = on;
        update_fade_targets_locked();
        xSemaphoreGive(s_driver_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t app_driver_light_set_brightness(uint8_t brightness)
{
    if (xSemaphoreTake(s_driver_mutex, portMAX_DELAY) == pdTRUE) {
        s_brightness = brightness;
        update_fade_targets_locked();
        xSemaphoreGive(s_driver_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t app_driver_light_set_color_temp(uint16_t color_temp_mireds)
{
    if (xSemaphoreTake(s_driver_mutex, portMAX_DELAY) == pdTRUE) {
        s_color_temp_mireds = color_temp_mireds;
        update_fade_targets_locked();
        xSemaphoreGive(s_driver_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t app_driver_light_update(bool on, uint8_t brightness, uint16_t color_temp_mireds)
{
    if (xSemaphoreTake(s_driver_mutex, portMAX_DELAY) == pdTRUE) {
        s_power = on;
        s_brightness = brightness;
        s_color_temp_mireds = color_temp_mireds;
        update_fade_targets_locked();
        xSemaphoreGive(s_driver_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

static void status_led_timer_callback(void *arg)
{
    s_status_led_blink_state = !s_status_led_blink_state;
    gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO,
                   s_status_led_blink_state ? CONFIG_STATUS_LED_ACTIVE_LEVEL : !CONFIG_STATUS_LED_ACTIVE_LEVEL);
}

void app_driver_status_led_set_mode(status_led_mode_t mode)
{
    s_status_mode = mode;
    if (!s_status_timer) {
        return;
    }

    esp_timer_stop(s_status_timer);

    switch (mode) {
    case STATUS_LED_MODE_OFF:
        gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO, !CONFIG_STATUS_LED_ACTIVE_LEVEL);
        break;
    case STATUS_LED_MODE_ON:
        gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO, CONFIG_STATUS_LED_ACTIVE_LEVEL);
        break;
    case STATUS_LED_MODE_BLINK_FAST:
        s_status_led_blink_state = true;
        gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO, CONFIG_STATUS_LED_ACTIVE_LEVEL);
        esp_timer_start_periodic(s_status_timer, 200 * 1000); // 200ms
        break;
    case STATUS_LED_MODE_BLINK_SLOW:
        s_status_led_blink_state = true;
        gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO, CONFIG_STATUS_LED_ACTIVE_LEVEL);
        esp_timer_start_periodic(s_status_timer, 800 * 1000); // 800ms
        break;
    }
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t high_task_wakeup = pdFALSE;
    if (s_button_task_handle) {
        vTaskNotifyGiveFromISR(s_button_task_handle, &high_task_wakeup);
        portYIELD_FROM_ISR(high_task_wakeup);
    }
}

static void button_task(void *pvParameters)
{
    s_button_task_handle = xTaskGetCurrentTaskHandle();

    while (1) {
        // Sleep until a button press interrupt wakes this task (0% idle CPU)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Debounce initial press
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level((gpio_num_t)CONFIG_BUTTON_GPIO) == 0) {
            uint32_t press_start_tick = xTaskGetTickCount();
            bool reset_triggered = false;

            // Monitor button while held down
            while (gpio_get_level((gpio_num_t)CONFIG_BUTTON_GPIO) == 0) {
                uint32_t elapsed_ms = (xTaskGetTickCount() - press_start_tick) * portTICK_PERIOD_MS;
                if (!reset_triggered && elapsed_ms >= (CONFIG_FACTORY_RESET_HOLD_TIME_SEC * 1000)) {
                    reset_triggered = true;
                    ESP_LOGW(TAG, "Factory reset button threshold reached (%ds)!", CONFIG_FACTORY_RESET_HOLD_TIME_SEC);
                    if (s_button_cb) {
                        s_button_cb(false, true);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            // Button released
            uint32_t elapsed_ms = (xTaskGetTickCount() - press_start_tick) * portTICK_PERIOD_MS;
            if (!reset_triggered && elapsed_ms >= 50 && elapsed_ms < 1500) {
                ESP_LOGI(TAG, "Button short press detected (%lums) -> Toggle Light", (unsigned long)elapsed_ms);
                if (s_button_cb) {
                    s_button_cb(true, false);
                }
            }
        }
    }
}

esp_err_t app_driver_init(app_driver_button_callback_t btn_cb)
{
    s_button_cb = btn_cb;

    if (!s_driver_mutex) {
        s_driver_mutex = xSemaphoreCreateMutex();
        if (!s_driver_mutex) {
            ESP_LOGE(TAG, "Failed to create driver mutex");
            return ESP_ERR_NO_MEM;
        }
    }

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

    // 2. Initialize Cool White Channel (GPIO 0, hpoint = 0)
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

    // 3. Initialize Warm White Channel (GPIO 1, hpoint = 0 to avoid timer-wrap duty clipping)
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

    // Create high-precision ratio-locked fade timer (200 Hz)
    esp_timer_create_args_t fade_timer_args = {
        .callback = fade_timer_callback,
        .name = "led_fade_tmr"
    };
    esp_timer_create(&fade_timer_args, &s_fade_timer);

    // 4. Initialize Status LED (GPIO 8) with esp_timer
    gpio_config_t status_io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&status_io_conf);
    gpio_set_level((gpio_num_t)CONFIG_STATUS_LED_GPIO, !CONFIG_STATUS_LED_ACTIVE_LEVEL);

    esp_timer_create_args_t timer_args = {
        .callback = status_led_timer_callback,
        .name = "status_led_tmr"
    };
    esp_timer_create(&timer_args, &s_status_timer);

    // 5. Initialize Push Button (GPIO 9) with Interrupt Wakeup
    gpio_config_t btn_io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_io_conf);

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service returned: %s", esp_err_to_name(err));
    }
    gpio_isr_handler_add((gpio_num_t)CONFIG_BUTTON_GPIO, button_isr_handler, NULL);

    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Hardware driver initialized (CW=GPIO %d, WW=GPIO %d, Freq=%d Hz, Res=%d-bit, Status=GPIO %d, Btn=GPIO %d)",
             CONFIG_LED_CW_GPIO, CONFIG_LED_WW_GPIO, CONFIG_PWM_FREQUENCY_HZ,
             (LEDC_RESOLUTION == LEDC_TIMER_13_BIT ? 13 : 11),
             CONFIG_STATUS_LED_GPIO, CONFIG_BUTTON_GPIO);

    return ESP_OK;
}
