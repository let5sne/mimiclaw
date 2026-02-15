#pragma once

#include "esp_err.h"
#include "display.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SSD1306 Commands */
#define SSD1306_CMD_SET_CONTRAST        0x81
#define SSD1306_CMD_DISPLAY_ALL_ON      0xA5
#define SSD1306_CMD_DISPLAY_NORMAL      0xA6
#define SSD1306_CMD_DISPLAY_OFF         0xAE
#define SSD1306_CMD_DISPLAY_ON          0xAF
#define SSD1306_CMD_SET_DISPLAY_OFFSET  0xD3
#define SSD1306_CMD_SET_COM_PINS        0xDA
#define SSD1306_CMD_SET_VCOM_DETECT     0xDB
#define SSD1306_CMD_SET_DISPLAY_CLK_DIV 0xD5
#define SSD1306_CMD_SET_PRECHARGE       0xD9
#define SSD1306_CMD_SET_MULTIPLEX       0xA8
#define SSD1306_CMD_SET_LOW_COLUMN      0x00
#define SSD1306_CMD_SET_HIGH_COLUMN     0x10
#define SSD1306_CMD_SET_START_LINE      0x40
#define SSD1306_CMD_MEMORY_MODE         0x20
#define SSD1306_CMD_COLUMN_ADDR         0x21
#define SSD1306_CMD_PAGE_ADDR           0x22
#define SSD1306_CMD_COM_SCAN_DEC        0xC8
#define SSD1306_CMD_SEG_REMAP           0xA1
#define SSD1306_CMD_CHARGE_PUMP         0x8D
#define SSD1306_CMD_ACTIVATE_SCROLL     0x2F
#define SSD1306_CMD_DEACTIVATE_SCROLL   0x2E

/**
 * Initialize SSD1306 OLED display
 */
esp_err_t ssd1306_init(const display_config_t *config);

/**
 * Deinitialize SSD1306
 */
void ssd1306_deinit(void);

/**
 * Clear display buffer
 */
void ssd1306_clear(void);

/**
 * Update display (send buffer to screen)
 */
void ssd1306_update(void);

/**
 * Draw a pixel
 */
void ssd1306_draw_pixel(int x, int y, bool on);

/**
 * Draw a line
 */
void ssd1306_draw_line(int x0, int y0, int x1, int y1);

/**
 * Draw a rectangle
 */
void ssd1306_draw_rect(int x, int y, int w, int h, bool fill);

/**
 * Draw text at position (scale: 1=8px, 2=16px)
 */
void ssd1306_draw_text(int x, int y, const char *text, int scale);

/**
 * Draw text with word wrapping
 */
void ssd1306_draw_text_wrapped(int x, int y, const char *text, int scale, int max_width_px);

/**
 * Set contrast (0-255)
 */
void ssd1306_set_contrast(uint8_t contrast);

/**
 * Set display power
 */
void ssd1306_set_power(bool on);

#ifdef __cplusplus
}
#endif
