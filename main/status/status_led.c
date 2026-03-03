#include "status_led.h"

#include "mimi_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "status_led";

#if MIMI_STATUS_LED_ENABLED

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    bool blink;
    uint32_t period_ms;
} led_pattern_t;

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static TaskHandle_t s_task;

static status_led_wifi_state_t s_wifi_state = STATUS_LED_WIFI_UNKNOWN;
static status_led_service_state_t s_tg_state = STATUS_LED_SERVICE_UNKNOWN;
static status_led_service_state_t s_llm_state = STATUS_LED_SERVICE_UNKNOWN;
static status_led_activity_t s_activity = STATUS_LED_ACTIVITY_BOOT;

static uint8_t s_last_r = 0xFF;
static uint8_t s_last_g = 0xFF;
static uint8_t s_last_b = 0xFF;

static uint8_t scale_channel(uint8_t value)
{
    return (uint8_t)((value * MIMI_STATUS_LED_BRIGHTNESS) / 255U);
}

static led_pattern_t current_pattern_locked(void)
{
    if (s_wifi_state == STATUS_LED_WIFI_ERROR) {
        return (led_pattern_t){255, 180, 0, true, 600};
    }
    if (s_wifi_state == STATUS_LED_WIFI_CONNECTING) {
        return (led_pattern_t){255, 180, 0, true, 1200};
    }
    if (s_tg_state == STATUS_LED_SERVICE_AUTH_ERROR) {
        return (led_pattern_t){255, 0, 0, true, 600};
    }
    if (s_llm_state == STATUS_LED_SERVICE_AUTH_ERROR) {
        return (led_pattern_t){0, 80, 255, true, 600};
    }
    if (s_tg_state == STATUS_LED_SERVICE_ERROR ||
        s_llm_state == STATUS_LED_SERVICE_ERROR) {
        return (led_pattern_t){180, 0, 180, true, 400};
    }
    if (s_wifi_state == STATUS_LED_WIFI_UNKNOWN) {
        return (led_pattern_t){255, 255, 255, true, 800};
    }

    switch (s_activity) {
        case STATUS_LED_ACTIVITY_THINKING:
            return (led_pattern_t){0, 180, 255, false, 0};
        case STATUS_LED_ACTIVITY_SPEAKING:
            return (led_pattern_t){255, 255, 255, false, 0};
        case STATUS_LED_ACTIVITY_BOOT:
            return (led_pattern_t){255, 255, 255, true, 800};
        case STATUS_LED_ACTIVITY_IDLE:
        default:
            return (led_pattern_t){0, 255, 0, false, 0};
    }
}

static void apply_color_locked(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized || !s_strip) return;
    if (r == s_last_r && g == s_last_g && b == s_last_b) return;

    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);

    s_last_r = r;
    s_last_g = g;
    s_last_b = b;
}

static void status_led_task(void *arg)
{
    (void)arg;

    while (1) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        led_pattern_t pattern = current_pattern_locked();
        bool on = true;
        if (pattern.blink && pattern.period_ms > 0) {
            uint64_t half_period_ms = pattern.period_ms / 2U;
            if (half_period_ms == 0) {
                half_period_ms = 1;
            }
            on = (((esp_timer_get_time() / 1000ULL) / half_period_ms) % 2ULL) == 0ULL;
        }

        uint8_t r = on ? scale_channel(pattern.r) : 0;
        uint8_t g = on ? scale_channel(pattern.g) : 0;
        uint8_t b = on ? scale_channel(pattern.b) : 0;
        apply_color_locked(r, g, b);
        xSemaphoreGive(s_lock);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void update_state_locked(void *target, size_t size, const void *src)
{
    if (!s_initialized || !s_lock) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(target, src, size);
    xSemaphoreGive(s_lock);
}

esp_err_t status_led_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = MIMI_STATUS_LED_PIN,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init LED strip on GPIO%d: %s",
                 MIMI_STATUS_LED_PIN, esp_err_to_name(ret));
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ret;
    }

    s_initialized = true;

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        status_led_task, "status_led",
        2048, NULL, 2, &s_task, 0);
    if (task_ret != pdPASS) {
        led_strip_clear(s_strip);
        s_strip = NULL;
        s_initialized = false;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Status LED initialized on GPIO%d", MIMI_STATUS_LED_PIN);
    return ESP_OK;
}

void status_led_set_wifi_state(status_led_wifi_state_t state)
{
    update_state_locked(&s_wifi_state, sizeof(s_wifi_state), &state);
}

void status_led_set_telegram_state(status_led_service_state_t state)
{
    update_state_locked(&s_tg_state, sizeof(s_tg_state), &state);
}

void status_led_set_llm_state(status_led_service_state_t state)
{
    update_state_locked(&s_llm_state, sizeof(s_llm_state), &state);
}

void status_led_set_activity(status_led_activity_t activity)
{
    update_state_locked(&s_activity, sizeof(s_activity), &activity);
}

#else

esp_err_t status_led_init(void)
{
    return ESP_OK;
}

void status_led_set_wifi_state(status_led_wifi_state_t state)
{
    (void)state;
}

void status_led_set_telegram_state(status_led_service_state_t state)
{
    (void)state;
}

void status_led_set_llm_state(status_led_service_state_t state)
{
    (void)state;
}

void status_led_set_activity(status_led_activity_t activity)
{
    (void)activity;
}

#endif
