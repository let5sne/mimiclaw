#pragma once

#include <stdint.h>
#include <stdbool.h>
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

/**
 * Initialize display with given configuration
 */
esp_err_t display_init(const display_config_t *config);

/**
 * Deinitialize display
 */
void display_deinit(void);

/**
 * Clear display
 */
void display_clear(void);

/**
 * Update display (flush buffer to screen)
 */
void display_update(void);

/**
 * Set status text (top line)
 */
void display_set_status(const char *status);

/**
 * Show notification (temporary message)
 */
void display_show_notification(const char *text, int duration_ms);

/**
 * Display a message (role: "user" or "assistant")
 */
void display_show_message(const char *role, const char *content);

/**
 * Set display status (changes icon/indicator)
 */
void display_set_display_status(display_status_t status);

/**
 * Set brightness (0-100)
 */
void display_set_brightness(uint8_t brightness);

/**
 * Turn display on/off
 */
void display_set_power(bool on);

#ifdef __cplusplus
}
#endif
