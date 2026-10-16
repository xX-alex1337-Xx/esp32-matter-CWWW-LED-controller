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
#define CONFIG_MIN_COLOR_TEMP_MIREDS 153 // 6500K
#endif

#ifndef CONFIG_MAX_COLOR_TEMP_MIREDS
#define CONFIG_MAX_COLOR_TEMP_MIREDS 370 // 2700K
#endif

#ifndef CONFIG_FACTORY_RESET_HOLD_TIME_SEC
#define CONFIG_FACTORY_RESET_HOLD_TIME_SEC 10
#endif

// Select optimal PWM resolution based on frequency
#if (CONFIG_PWM_FREQUENCY_HZ <= 4000)
#define LEDC_RESOLUTION       LEDC_TIMER_12_BIT
#define LEDC_MAX_DUTY         ((1 << 12) - 1) // 4095
#else
#define LEDC_RESOLUTION       LEDC_TIMER_11_BIT
#define LEDC_MAX_DUTY         ((1 << 11) - 1) // 2047
#endif

#define LEDC_CW_CHANNEL       LEDC_CHANNEL_0
#define LEDC_WW_CHANNEL       LEDC_CHANNEL_1

// Phase offset for WW channel to interleave switching and halve power supply current ripple
#define LEDC_WW_HPOINT_OFFSET ((LEDC_MAX_DUTY + 1) / 2)

// Internal state
static bool s_power = false;
static uint8_t s_brightness = 254; // 1-254
static uint16_t s_color_temp_mireds = 250; // ~4000K
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

static void apply_pwm_outputs_locked(void)
{
    uint32_t cw_duty = 0;
    uint32_t ww_duty = 0;

    if (s_power && s_brightness > 0) {
        // 1. Normalized brightness 0.0 to 1.0
        float norm_bright = (float)s_brightness / 254.0f;

        // 2. Perceptual gamma 2.2 applied to TOTAL power first
        float total_lum = apply_gamma(norm_bright);

        // 3. Clamp color temperature within mired bounds
        uint16_t clamped_mireds = s_color_temp_mireds;
        if (clamped_mireds < CONFIG_MIN_COLOR_TEMP_MIREDS) {
            clamped_mireds = CONFIG_MIN_COLOR_TEMP_MIREDS;
        }
        if (clamped_mireds > CONFIG_MAX_COLOR_TEMP_MIREDS) {
            clamped_mireds = CONFIG_MAX_COLOR_TEMP_MIREDS;
        }

        // 4. Calculate warm/cool ratio: 0.0 = Coolest (MIN_MIREDS), 1.0 = Warmest (MAX_MIREDS)
        float temp_ratio = 0.5f;
        if (CONFIG_MAX_COLOR_TEMP_MIREDS > CONFIG_MIN_COLOR_TEMP_MIREDS) {
            temp_ratio = (float)(clamped_mireds - CONFIG_MIN_COLOR_TEMP_MIREDS) /
                         (float)(CONFIG_MAX_COLOR_TEMP_MIREDS - CONFIG_MIN_COLOR_TEMP_MIREDS);
        }

        // 5. Constant-power distribution: cw + ww == total_lum
        float cw_linear = total_lum * (1.0f - temp_ratio);
        float ww_linear = total_lum * temp_ratio;

        cw_duty = (uint32_t)roundf(cw_linear * (float)LEDC_MAX_DUTY);
        ww_duty = (uint32_t)roundf(ww_linear * (float)LEDC_MAX_DUTY);

        if (cw_duty > LEDC_MAX_DUTY) cw_duty = LEDC_MAX_DUTY;
        if (ww_duty > LEDC_MAX_DUTY) ww_duty = LEDC_MAX_DUTY;

        // 6. Low-brightness floor: ensure the light never drops to 0 PWM when s_power is ON
        if (cw_duty == 0 && ww_duty == 0) {
            if (temp_ratio >= 0.5f) {
                ww_duty = 1;
            } else {
                cw_duty = 1;
            }
        }
    }

#if (CONFIG_LIGHT_FADE_TIME_MS > 0)
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL, cw_duty, CONFIG_LIGHT_FADE_TIME_MS);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL, LEDC_FADE_NO_WAIT);

    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL, ww_duty, CONFIG_LIGHT_FADE_TIME_MS);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL, LEDC_FADE_NO_WAIT);
#else
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL, cw_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CW_CHANNEL);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL, ww_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_WW_CHANNEL);
#endif

    ESP_LOGD(TAG, "PWM updated: Power=%d, Bright=%d, Mireds=%d -> CW_Duty=%lu, WW_Duty=%lu (Max=%d)",
             s_power, s_brightness, s_color_temp_mireds, (unsigned long)cw_duty, (unsigned long)ww_duty, LEDC_MAX_DUTY);
}

esp_err_t app_driver_light_set_power(bool on)
{
    if (xSemaphoreTake(s_driver_mutex, portMAX_DELAY) == pdTRUE) {
        s_power = on;
        apply_pwm_outputs_locked();
        xSemaphoreGive(s_driver_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t app_driver_light_set_brightness(uint8_t brightness)
{
    if (xSemaphoreTake(s_driver_mutex, portMAX_DELAY) == pdTRUE) {
        s_brightness = brightness;
        apply_pwm_outputs_locked();
        xSemaphoreGive(s_driver_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t app_driver_light_set_color_temp(uint16_t color_temp_mireds)
{
    if (xSemaphoreTake(s_driver_mutex, portMAX_DELAY) == pdTRUE) {
        s_color_temp_mireds = color_temp_mireds;
        apply_pwm_outputs_locked();
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
        apply_pwm_outputs_locked();
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

    // 3. Initialize Warm White Channel (GPIO 1, hpoint = LEDC_WW_HPOINT_OFFSET for phase-interleaving)
    ledc_channel_config_t ww_channel = {
        .gpio_num       = CONFIG_LED_WW_GPIO,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_WW_CHANNEL,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0,
        .hpoint         = LEDC_WW_HPOINT_OFFSET,
    };
    err = ledc_channel_config(&ww_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config WW failed: %s", esp_err_to_name(err));
        return err;
    }

    // Install hardware fade service
    err = ledc_fade_func_install(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "ledc_fade_func_install returned: %s", esp_err_to_name(err));
    }

    // 4. Initialize Status LED (GPIO 8) with esp_timer (zero stack / zero task wakeups)
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
             (LEDC_RESOLUTION == LEDC_TIMER_12_BIT ? 12 : 11),
             CONFIG_STATUS_LED_GPIO, CONFIG_BUTTON_GPIO);

    return ESP_OK;
}
