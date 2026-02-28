#include "display.h"
#include "status/status_led.h"
#include "ssd1306.h"
#include "st7789.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
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
static char s_message_role[24] = {0};
static char s_message_content[256] = {0};
static char s_message_buffer[256] = {0};
static TimerHandle_t s_notification_timer = NULL;
static TimerHandle_t s_message_page_timer = NULL;
static SemaphoreHandle_t s_display_mutex = NULL;
static bool s_prev_had_message = false;
static size_t s_message_page_index = 0;
static size_t s_message_visible_pages = 0;
static bool s_message_pages_truncated = false;
static bool s_notification_active = false;

#define DISPLAY_ST7789_MESSAGE_X            4
#define DISPLAY_ST7789_ROLE_Y               40
#define DISPLAY_ST7789_BODY_Y               64
#define DISPLAY_ST7789_FOOTER_H             16
#define DISPLAY_ST7789_BODY_BOTTOM_PAD      4
#define DISPLAY_ST7789_MAX_AUTO_PAGES       3
#define DISPLAY_MESSAGE_PAGE_INTERVAL_MS    2500

#define DISPLAY_LOCK() do { \
    if (s_display_mutex) xSemaphoreTakeRecursive(s_display_mutex, portMAX_DELAY); \
} while (0)

#define DISPLAY_UNLOCK() do { \
    if (s_display_mutex) xSemaphoreGiveRecursive(s_display_mutex); \
} while (0)

/* Forward declarations */
static void notification_timer_callback(TimerHandle_t timer);
static void render_screen(void);
static const char *display_role_label(const char *role);
static uint16_t display_role_color(const char *role);
static void display_sanitize_text(const char *src, char *dst, size_t dst_size);
static void message_page_timer_callback(TimerHandle_t timer);
static void display_stop_message_paging_locked(void);
static void display_refresh_message_paging_locked(bool reset_page);

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

    s_message_page_timer = xTimerCreate("disp_page", pdMS_TO_TICKS(DISPLAY_MESSAGE_PAGE_INTERVAL_MS),
                                        pdTRUE, NULL, message_page_timer_callback);
    if (!s_message_page_timer) {
        ESP_LOGE(TAG, "Failed to create message page timer");
        xTimerDelete(s_notification_timer, 0);
        s_notification_timer = NULL;
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
    s_message_page_index = 0;
    s_message_visible_pages = 0;
    s_message_pages_truncated = false;
    s_notification_active = false;
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
    if (s_message_page_timer) {
        xTimerDelete(s_message_page_timer, 0);
        s_message_page_timer = NULL;
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
    s_message_page_index = 0;
    s_message_visible_pages = 0;
    s_message_pages_truncated = false;
    s_notification_active = false;
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
    display_stop_message_paging_locked();
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
    display_stop_message_paging_locked();
    s_notification_active = true;

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

    /* ST7789 单独保存角色和正文，便于更细的排版控制；其余屏幕继续复用合成文本。 */
    strncpy(s_message_role, role, sizeof(s_message_role) - 1);
    s_message_role[sizeof(s_message_role) - 1] = '\0';
    display_sanitize_text(content, s_message_content, sizeof(s_message_content));
    const char *role_label = display_role_label(role);
    size_t used = 0;
    if (role_label && role_label[0] != '\0') {
        used = snprintf(s_message_buffer, sizeof(s_message_buffer), "%s: ", role_label);
        if (used >= sizeof(s_message_buffer)) {
            used = sizeof(s_message_buffer) - 1;
        }
    } else {
        s_message_buffer[0] = '\0';
    }
    snprintf(s_message_buffer + used, sizeof(s_message_buffer) - used, "%s", s_message_content);
    display_refresh_message_paging_locked(true);

    render_screen();
    display_update();
    DISPLAY_UNLOCK();
}

void display_set_display_status(display_status_t status)
{
    switch (status) {
        case DISPLAY_STATUS_THINKING:
            status_led_set_activity(STATUS_LED_ACTIVITY_THINKING);
            break;
        case DISPLAY_STATUS_SPEAKING:
            status_led_set_activity(STATUS_LED_ACTIVITY_SPEAKING);
            break;
        default:
            status_led_set_activity(STATUS_LED_ACTIVITY_IDLE);
            break;
    }

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
    s_notification_active = false;
    display_refresh_message_paging_locked(false);
    render_screen();
    display_update();
    DISPLAY_UNLOCK();
}

static void message_page_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (!s_display_mutex) return;
    DISPLAY_LOCK();
    if (!s_initialized || s_notification_active || s_message_visible_pages <= 1) {
        DISPLAY_UNLOCK();
        return;
    }

    s_message_page_index = (s_message_page_index + 1) % s_message_visible_pages;
    render_screen();
    display_update();
    DISPLAY_UNLOCK();
}

static const char *display_role_label(const char *role)
{
    if (!role || role[0] == '\0') {
        return "Mimi";
    }
    if (strcmp(role, "assistant") == 0) {
        return "Mimi";
    }
    if (strcmp(role, "user") == 0) {
        return "你";
    }
    return role;
}

static uint16_t display_role_color(const char *role)
{
    if (role && strcmp(role, "user") == 0) {
        return 0x07FF;
    }
    return 0xFFE0;
}

static void display_sanitize_text(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t di = 0;
    bool last_was_space = false;
    bool last_was_newline = false;

    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; ++si) {
        unsigned char ch = (unsigned char)src[si];

        if (ch == '*' || ch == '_' || ch == '`' || ch == '~') {
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            while (di > 0 && dst[di - 1] == ' ') {
                --di;
            }
            if (!last_was_newline) {
                dst[di++] = '\n';
                last_was_newline = true;
            }
            last_was_space = false;
            continue;
        }
        if (isspace(ch)) {
            if (!last_was_space && !last_was_newline) {
                dst[di++] = ' ';
                last_was_space = true;
            }
            continue;
        }

        dst[di++] = (char)ch;
        last_was_space = false;
        last_was_newline = false;
    }

    while (di > 0 && (dst[di - 1] == ' ' || dst[di - 1] == '\n')) {
        --di;
    }
    dst[di] = '\0';
}

static void display_stop_message_paging_locked(void)
{
    if (s_message_page_timer) {
        xTimerStop(s_message_page_timer, 0);
    }
}

static void display_refresh_message_paging_locked(bool reset_page)
{
    if (s_config.type != DISPLAY_TYPE_ST7789 || s_message_content[0] == '\0') {
        s_message_page_index = 0;
        s_message_visible_pages = 0;
        s_message_pages_truncated = false;
        display_stop_message_paging_locked();
        return;
    }

    const int footer_y = s_config.height - DISPLAY_ST7789_FOOTER_H;
    const int body_height = footer_y - DISPLAY_ST7789_BODY_Y - DISPLAY_ST7789_BODY_BOTTOM_PAD;
    int total_pages = st7789_get_text_page_count(
        s_message_content,
        s_config.width - (DISPLAY_ST7789_MESSAGE_X * 2),
        body_height);
    if (total_pages < 1) {
        total_pages = 1;
    }

    s_message_visible_pages = (size_t)total_pages;
    if (s_message_visible_pages > DISPLAY_ST7789_MAX_AUTO_PAGES) {
        s_message_visible_pages = DISPLAY_ST7789_MAX_AUTO_PAGES;
    }
    s_message_pages_truncated = (size_t)total_pages > s_message_visible_pages;

    if (reset_page || s_message_page_index >= s_message_visible_pages) {
        s_message_page_index = 0;
    }

    if (s_message_visible_pages > 1 && !s_notification_active && s_message_page_timer) {
        xTimerChangePeriod(s_message_page_timer, pdMS_TO_TICKS(DISPLAY_MESSAGE_PAGE_INTERVAL_MS), 0);
        xTimerStart(s_message_page_timer, 0);
    } else {
        display_stop_message_paging_locked();
    }
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
            const bool has_message = (s_message_content[0] != '\0');
            const int footer_y = s_config.height - DISPLAY_ST7789_FOOTER_H;
            const int body_height = footer_y - DISPLAY_ST7789_BODY_Y - DISPLAY_ST7789_BODY_BOTTOM_PAD;
            st7789_fill_rect(0, 0, s_config.width, 32, 0x0000);
            if (has_message || s_prev_had_message) {
                st7789_fill_rect(0, DISPLAY_ST7789_ROLE_Y, s_config.width,
                                 s_config.height - DISPLAY_ST7789_ROLE_Y, 0x0000);
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
                const char *role_label = display_role_label(s_message_role);
                const size_t visible_pages = (s_message_visible_pages > 0) ? s_message_visible_pages : 1;
                const size_t page_index = (s_message_page_index < visible_pages) ? s_message_page_index : 0;
                st7789_draw_text(DISPLAY_ST7789_MESSAGE_X, DISPLAY_ST7789_ROLE_Y,
                                 role_label, 1, display_role_color(s_message_role), 0x0000);
                st7789_draw_text_wrapped_page(
                    DISPLAY_ST7789_MESSAGE_X, DISPLAY_ST7789_BODY_Y, s_message_content,
                    s_config.width - (DISPLAY_ST7789_MESSAGE_X * 2), body_height,
                    (int)page_index, 0xFFFF, 0x0000);

                if (visible_pages > 1 || s_message_pages_truncated) {
                    char footer[8];
                    size_t footer_len = 0;
                    footer[footer_len++] = (char)('0' + (int)(page_index + 1));
                    footer[footer_len++] = '/';
                    footer[footer_len++] = (char)('0' + (int)visible_pages);
                    if (s_message_pages_truncated) {
                        footer[footer_len++] = '+';
                    }
                    footer[footer_len] = '\0';
                    const int footer_w = (int)footer_len * 8;
                    int footer_x = s_config.width - footer_w - DISPLAY_ST7789_MESSAGE_X;
                    if (footer_x < DISPLAY_ST7789_MESSAGE_X) {
                        footer_x = DISPLAY_ST7789_MESSAGE_X;
                    }
                    st7789_fill_rect(0, footer_y - 1, s_config.width, 1, 0x2104);
                    st7789_draw_text(footer_x, footer_y, footer, 1, 0x7BEF, 0x0000);
                }
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
