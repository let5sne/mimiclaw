#pragma once

#include "display.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t st7789_init(const display_config_t *config);
void st7789_deinit(void);
void st7789_clear(void);
void st7789_update(void);
void st7789_set_brightness(uint8_t brightness);
void st7789_set_power(bool on);
void st7789_render_status(display_status_t status);
void st7789_fill_rect(int x, int y, int w, int h, uint16_t color);
void st7789_draw_status_line(const char *icon, uint16_t icon_color,
                             const char *text, uint16_t text_color,
                             uint16_t bg_color);
void st7789_draw_text(int x, int y, const char *text, int scale,
                      uint16_t fg, uint16_t bg);

#ifdef __cplusplus
}
#endif
