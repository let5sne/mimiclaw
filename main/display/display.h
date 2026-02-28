#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display types */
typedef enum {
    DISPLAY_TYPE_NONE = 0,
    DISPLAY_TYPE_SSD1306,    /* OLED 128x64 I2C */
    DISPLAY_TYPE_ST7789,     /* LCD 240x240 SPI */
    DISPLAY_TYPE_ILI9341,    /* LCD 320x240 SPI */
} display_type_t;

/* Display configuration */
typedef struct {
    display_type_t type;
    int width;
    int height;

    /* I2C config (for OLED) */
    int i2c_port;
    int sda_pin;
    int scl_pin;
    uint8_t i2c_addr;

    /* SPI config (for LCD) */
    int spi_host;
    int mosi_pin;
    int sclk_pin;
    int cs_pin;
    int dc_pin;
    int rst_pin;
    int backlight_pin;
} display_config_t;

/* Display status */
typedef enum {
    DISPLAY_STATUS_IDLE = 0,
    DISPLAY_STATUS_CONNECTING,
    DISPLAY_STATUS_CONNECTED,
    DISPLAY_STATUS_THINKING,
    DISPLAY_STATUS_SPEAKING,
    DISPLAY_STATUS_ERROR,
} display_status_t;

#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 172

esp_err_t display_init(const display_config_t *config);
void display_deinit(void);
void display_clear(void);
void display_update(void);
void display_set_status(const char *status);
void display_show_notification(const char *text, int duration_ms);
void display_show_message(const char *role, const char *content);
void display_set_display_status(display_status_t status);
void display_set_brightness(uint8_t brightness);
void display_set_power(bool on);
void display_show_banner(void);
void display_set_backlight_percent(uint8_t percent);
uint8_t display_get_backlight_percent(void);
void display_cycle_backlight(void);
bool display_get_banner_center_rgb(uint8_t *r, uint8_t *g, uint8_t *b);
void display_show_config_screen(const char *qr_text, const char *ip_text,
                                const char **lines, size_t line_count, size_t scroll,
                                size_t selected, int selected_offset_px);
void display_show_message_card(const char *title, const char *body);

#ifdef __cplusplus
}
#endif
