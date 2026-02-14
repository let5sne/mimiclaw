#include "display.h"
#include "ssd1306.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "display";

/* Display state */
static display_config_t s_config = {0};
static bool s_initialized = false;
static display_status_t s_status = DISPLAY_STATUS_IDLE;
static char s_status_text[64] = {0};
static char s_message_buffer[256] = {0};
static TimerHandle_t s_notification_timer = NULL;

/* Forward declarations */
static void notification_timer_callback(TimerHandle_t timer);
static void render_screen(void);

esp_err_t display_init(const display_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Display already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(display_config_t));

    esp_err_t ret = ESP_OK;

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ret = ssd1306_init(&s_config);
            break;

        case DISPLAY_TYPE_NONE:
            ESP_LOGI(TAG, "No display configured");
            return ESP_OK;

        default:
            ESP_LOGE(TAG, "Unsupported display type: %d", s_config.type);
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display: %d", ret);
        return ret;
    }

    /* Create notification timer */
    s_notification_timer = xTimerCreate("disp_notif", pdMS_TO_TICKS(3000),
                                        pdFALSE, NULL, notification_timer_callback);

    s_initialized = true;
    strcpy(s_status_text, "MimiClaw");

    display_clear();
    render_screen();
    display_update();

    ESP_LOGI(TAG, "Display initialized: %dx%d", s_config.width, s_config.height);
    return ESP_OK;
}

void display_deinit(void)
{
    if (!s_initialized) return;

    if (s_notification_timer) {
        xTimerDelete(s_notification_timer, 0);
        s_notification_timer = NULL;
    }

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_deinit();
            break;
        default:
            break;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Display deinitialized");
}

void display_clear(void)
{
    if (!s_initialized) return;

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_clear();
            break;
        default:
            break;
    }
}

void display_update(void)
{
    if (!s_initialized) return;

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_update();
            break;
        default:
            break;
    }
}

void display_set_status(const char *status)
{
    if (!s_initialized || !status) return;

    strncpy(s_status_text, status, sizeof(s_status_text) - 1);
    s_status_text[sizeof(s_status_text) - 1] = '\0';

    render_screen();
    display_update();
}

void display_show_notification(const char *text, int duration_ms)
{
    if (!s_initialized || !text) return;

    /* Stop existing timer */
    if (s_notification_timer) {
        xTimerStop(s_notification_timer, 0);
    }

    /* Show notification */
    display_clear();

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_draw_text(0, 20, text, 2);
            break;
        default:
            break;
    }

    display_update();

    /* Start timer to restore normal display */
    if (s_notification_timer && duration_ms > 0) {
        xTimerChangePeriod(s_notification_timer, pdMS_TO_TICKS(duration_ms), 0);
        xTimerStart(s_notification_timer, 0);
    }
}

void display_show_message(const char *role, const char *content)
{
    if (!s_initialized || !role || !content) return;

    /* Store message */
    snprintf(s_message_buffer, sizeof(s_message_buffer), "%s: %s", role, content);

    render_screen();
    display_update();
}

void display_set_display_status(display_status_t status)
{
    if (!s_initialized) return;

    s_status = status;
    render_screen();
    display_update();
}

void display_set_brightness(uint8_t brightness)
{
    if (!s_initialized) return;

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_set_contrast(brightness * 255 / 100);
            break;
        default:
            break;
    }
}

void display_set_power(bool on)
{
    if (!s_initialized) return;

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_set_power(on);
            break;
        default:
            break;
    }
}

/* Private functions */

static void notification_timer_callback(TimerHandle_t timer)
{
    /* Restore normal display */
    render_screen();
    display_update();
}

static void render_screen(void)
{
    if (!s_initialized) return;

    display_clear();

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306: {
            /* Status bar (top) */
            const char *status_icon = "●";
            switch (s_status) {
                case DISPLAY_STATUS_CONNECTING: status_icon = "○"; break;
                case DISPLAY_STATUS_CONNECTED:  status_icon = "●"; break;
                case DISPLAY_STATUS_THINKING:   status_icon = "◐"; break;
                case DISPLAY_STATUS_SPEAKING:   status_icon = "◑"; break;
                case DISPLAY_STATUS_ERROR:      status_icon = "✕"; break;
                default: status_icon = "●"; break;
            }

            char status_line[80];  /* Increased buffer size to prevent truncation */
            snprintf(status_line, sizeof(status_line), "%s %s", status_icon, s_status_text);
            ssd1306_draw_text(0, 0, status_line, 1);
            ssd1306_draw_line(0, 10, 127, 10);

            /* Message area */
            if (s_message_buffer[0]) {
                ssd1306_draw_text_wrapped(0, 14, s_message_buffer, 1, 16);
            }
            break;
        }
        default:
            break;
    }
}
