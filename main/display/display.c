#include "display.h"
#include "ssd1306.h"
#include "st7789.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
static SemaphoreHandle_t s_display_mutex = NULL;
static bool s_prev_had_message = false;

#define DISPLAY_LOCK() do { \
    if (s_display_mutex) xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY); \
} while (0)

#define DISPLAY_UNLOCK() do { \
    if (s_display_mutex) xSemaphoreGiveRecursive(s_display_mutex); \
} while (0)

/* Forward declarations */
static void notification_timer_callback(TimerHandle_t timer);
static void render_screen(void);

esp_err_t display_init(const display_config_t *config)
{
    bool created_mutex = false;
    if (!s_display_mutex) {
        s_display_mutex = xSemaphoreCreateRecursiveMutex();
        if (!s_display_mutex) {
            ESP_LOGE(TAG, "Failed to create display mutex");
            return ESP_ERR_NO_MEM;
        }
        created_mutex = true;
    }

    DISPLAY_LOCK();

    if (s_initialized) {
        ESP_LOGW(TAG, "Display already initialized");
        DISPLAY_UNLOCK();
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Invalid config");
        DISPLAY_UNLOCK();
        if (created_mutex) {
            vSemaphoreDelete(s_display_mutex);
            s_display_mutex = NULL;
        }
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(display_config_t));

    esp_err_t ret = ESP_OK;
    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ret = ssd1306_init(&s_config);
            break;
        case DISPLAY_TYPE_ST7789:
            ret = st7789_init(&s_config);
            break;
        case DISPLAY_TYPE_NONE:
            ESP_LOGI(TAG, "No display configured");
            DISPLAY_UNLOCK();
            return ESP_OK;
        default:
            ESP_LOGE(TAG, "Unsupported display type: %d", s_config.type);
            DISPLAY_UNLOCK();
            if (created_mutex) {
                vSemaphoreDelete(s_display_mutex);
                s_display_mutex = NULL;
            }
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display: %d", ret);
        DISPLAY_UNLOCK();
        if (created_mutex) {
            vSemaphoreDelete(s_display_mutex);
            s_display_mutex = NULL;
        }
        return ret;
    }

    s_notification_timer = xTimerCreate("disp_notif", pdMS_TO_TICKS(3000),
                                        pdFALSE, NULL, notification_timer_callback);
    if (!s_notification_timer) {
        ESP_LOGE(TAG, "Failed to create notification timer");
        switch (s_config.type) {
            case DISPLAY_TYPE_SSD1306: ssd1306_deinit(); break;
            case DISPLAY_TYPE_ST7789:  st7789_deinit(); break;
            default: break;
        }
        DISPLAY_UNLOCK();
        if (created_mutex) {
            vSemaphoreDelete(s_display_mutex);
            s_display_mutex = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    s_prev_had_message = false;
    strcpy(s_status_text, "MimiClaw");

    render_screen();
    display_update();

    ESP_LOGI(TAG, "Display initialized: %dx%d", s_config.width, s_config.height);
    DISPLAY_UNLOCK();
    return ESP_OK;
}

void display_deinit(void)
{
    if (!s_display_mutex) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }

    if (s_notification_timer) {
        xTimerDelete(s_notification_timer, 0);
        s_notification_timer = NULL;
    }

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_deinit();
            break;
        case DISPLAY_TYPE_ST7789:
            st7789_deinit();
            break;
        default:
            break;
    }

    s_initialized = false;
    s_prev_had_message = false;
    ESP_LOGI(TAG, "Display deinitialized");
    DISPLAY_UNLOCK();
    vSemaphoreDelete(s_display_mutex);
    s_display_mutex = NULL;
}

void display_clear(void)
{
    if (!s_display_mutex) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_clear();
            break;
        case DISPLAY_TYPE_ST7789:
            st7789_clear();
            break;
        default:
            break;
    }
    s_prev_had_message = false;
    DISPLAY_UNLOCK();
}

void display_update(void)
{
    if (!s_display_mutex) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_update();
            break;
        case DISPLAY_TYPE_ST7789:
            st7789_update();
            break;
        default:
            break;
    }
    DISPLAY_UNLOCK();
}

void display_set_status(const char *status)
{
    if (!s_display_mutex || !status) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }

    strncpy(s_status_text, status, sizeof(s_status_text) - 1);
    s_status_text[sizeof(s_status_text) - 1] = '\0';

    render_screen();
    display_update();
    DISPLAY_UNLOCK();
}

void display_show_notification(const char *text, int duration_ms)
{
    if (!s_display_mutex || !text) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }

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
        case DISPLAY_TYPE_ST7789:
            st7789_draw_text(4, 80, text, 2, 0xFFFF, 0x0000);
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
    DISPLAY_UNLOCK();
}

void display_show_message(const char *role, const char *content)
{
    if (!s_display_mutex || !role || !content) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }

    /* Store message */
    snprintf(s_message_buffer, sizeof(s_message_buffer), "%s: %s", role, content);

    render_screen();
    display_update();
    DISPLAY_UNLOCK();
}

void display_set_display_status(display_status_t status)
{
    if (!s_display_mutex) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }

    s_status = status;
    render_screen();
    display_update();
    DISPLAY_UNLOCK();
}

void display_set_brightness(uint8_t brightness)
{
    if (!s_display_mutex) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_set_contrast(brightness * 255 / 100);
            break;
        case DISPLAY_TYPE_ST7789:
            st7789_set_brightness(brightness);
            break;
        default:
            break;
    }
    DISPLAY_UNLOCK();
}

void display_set_power(bool on)
{
    if (!s_display_mutex) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306:
            ssd1306_set_power(on);
            break;
        case DISPLAY_TYPE_ST7789:
            st7789_set_power(on);
            break;
        default:
            break;
    }
    DISPLAY_UNLOCK();
}

/* Private functions */

static void notification_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (!s_display_mutex) return;
    DISPLAY_LOCK();
    if (!s_initialized) {
        DISPLAY_UNLOCK();
        return;
    }
    /* Restore normal display */
    render_screen();
    display_update();
    DISPLAY_UNLOCK();
}

static void render_screen(void)
{
    if (!s_initialized) return;

    switch (s_config.type) {
        case DISPLAY_TYPE_SSD1306: {
            ssd1306_clear();
            /* Status bar (top) - use ASCII-only icons */
            const char *status_icon = "*";
            switch (s_status) {
                case DISPLAY_STATUS_CONNECTING: status_icon = "~"; break;
                case DISPLAY_STATUS_CONNECTED:  status_icon = "*"; break;
                case DISPLAY_STATUS_THINKING:   status_icon = "?"; break;
                case DISPLAY_STATUS_SPEAKING:   status_icon = ">"; break;
                case DISPLAY_STATUS_ERROR:      status_icon = "!"; break;
                default: status_icon = "*"; break;
            }

            char status_line[80];  /* Increased buffer size to prevent truncation */
            snprintf(status_line, sizeof(status_line), "%s %s", status_icon, s_status_text);
            ssd1306_draw_text(0, 0, status_line, 1);
            ssd1306_draw_line(0, 10, 127, 10);

            /* Message area */
            if (s_message_buffer[0]) {
                ssd1306_draw_text_wrapped(0, 14, s_message_buffer, 1, 128);
            }
            break;
        }
        case DISPLAY_TYPE_ST7789: {
            const bool has_message = (s_message_buffer[0] != '\0');
            st7789_fill_rect(0, 0, s_config.width, 32, 0x0000);
            if (has_message || s_prev_had_message) {
                st7789_fill_rect(0, 40, s_config.width, s_config.height - 40, 0x0000);
            }

            /* Status icon color based on state */
            uint16_t icon_color = 0xFFFF; /* white */
            const char *status_icon = "*";
            switch (s_status) {
                case DISPLAY_STATUS_CONNECTING: status_icon = "~"; icon_color = 0xFD20; break;
                case DISPLAY_STATUS_CONNECTED:  status_icon = "*"; icon_color = 0x07E0; break;
                case DISPLAY_STATUS_THINKING:   status_icon = "?"; icon_color = 0x001F; break;
                case DISPLAY_STATUS_SPEAKING:   status_icon = ">"; icon_color = 0xFFE0; break;
                case DISPLAY_STATUS_ERROR:      status_icon = "!"; icon_color = 0xF800; break;
                default: break;
            }
            st7789_draw_status_line(status_icon, icon_color, s_status_text, 0xFFFF, 0x0000);

            /* Message area below status bar */
            if (has_message) {
                st7789_draw_text(4, 40, s_message_buffer, 2, 0xFFFF, 0x0000);
            }
            s_prev_had_message = has_message;
            break;
        }
        default:
            break;
    }
}

/* Stubs for upstream display functions (not used with SSD1306 hardware) */
void display_show_banner(void) {}
void display_set_backlight_percent(uint8_t percent) { (void)percent; }
uint8_t display_get_backlight_percent(void) { return 50; }
void display_cycle_backlight(void) {}
bool display_get_banner_center_rgb(uint8_t *r, uint8_t *g, uint8_t *b) {
    if (r) *r = 0;
    if (g) *g = 0;
    if (b) *b = 0;
    return false;
}
void display_show_config_screen(const char *qr_text, const char *ip_text,
                                const char **lines, size_t line_count, size_t scroll,
                                size_t selected, int selected_offset_px) {
    (void)qr_text; (void)ip_text; (void)lines; (void)line_count;
    (void)scroll; (void)selected; (void)selected_offset_px;
}
void display_show_message_card(const char *title, const char *body) {
    (void)title; (void)body;
}
