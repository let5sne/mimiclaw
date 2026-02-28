#include "st7789.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "font_cjk.h"

static const char *TAG = "st7789";

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static spi_host_device_t s_spi_host = SPI2_HOST;
static int s_width = 240;
static int s_height = 240;
static int s_backlight_pin = -1;
static bool s_inited = false;
static uint16_t *s_fill_buf = NULL;
static SemaphoreHandle_t s_color_done_sem = NULL;
static const int ST7789_SAFE_PCLK_HZ = 10 * 1000 * 1000;
static const int ST7789_SPI_MODE = 3;
static const int ST7789_FILL_LINES = 20;
static const bool ST7789_MIRROR_X = false;
static const bool ST7789_MIRROR_Y = false;

static void st7789_fill_rect_internal(int x_start, int y_start, int x_end, int y_end, uint16_t color);
static void st7789_draw_glyph_to_buffer(char c, int scale, uint16_t fg, uint16_t bg,
                                        uint16_t *dst, int dst_w, int dst_x);
static uint32_t st7789_utf8_decode(const char **pp);
static bool st7789_is_cjk(uint32_t cp);
static void st7789_draw_cjk_to_buffer(const uint8_t *glyph, int target_w, int target_h,
                                      uint16_t fg, uint16_t bg, uint16_t *dst, int dst_w, int dst_x);

static bool st7789_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    BaseType_t high_task_wakeup = pdFALSE;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    if (sem) {
        xSemaphoreGiveFromISR(sem, &high_task_wakeup);
    }
    return high_task_wakeup == pdTRUE;
}

static esp_err_t st7789_draw_bitmap_sync(int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_color_done_sem) {
        while (xSemaphoreTake(s_color_done_sem, 0) == pdTRUE) {
        }
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, (void *)color_data);
    if (err != ESP_OK) {
        return err;
    }
    if (s_color_done_sem) {
        if (xSemaphoreTake(s_color_done_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "wait color trans done timeout");
        }
    }
    return ESP_OK;
}

/* 8x8 ASCII font (chars 32-127), same as SSD1306 */
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Space
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // !
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // #
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // $
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // %
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // &
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // (
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // *
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ,
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // .
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // /
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 9
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // :
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ;
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // <
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // =
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // >
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // ?
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // @
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // A
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // B
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // C
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // D
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // E
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // F
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // G
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // H
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // I
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // J
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // K
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // L
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // M
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // N
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // O
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // P
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // Q
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // R
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // S
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // T
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // U
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // X
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // Y
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // Z
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // [
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ]
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // _
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // a
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // b
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // c
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, // d
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, // e
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, // f
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // g
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // h
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // i
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // j
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // k
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // l
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // m
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // n
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // o
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // p
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // q
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // r
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // s
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // t
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // u
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // v
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // w
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // x
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // y
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // z
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // |
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // }
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // DEL
};

static void st7789_fill_rect_internal(int x_start, int y_start, int x_end, int y_end, uint16_t color)
{
    if (!s_panel || s_width <= 0 || s_height <= 0) {
        return;
    }

    if (!s_fill_buf) {
        ESP_LOGE(TAG, "Failed to allocate fill buffer");
        return;
    }

    if (x_start < 0) x_start = 0;
    if (y_start < 0) y_start = 0;
    if (x_end > s_width) x_end = s_width;
    if (y_end > s_height) y_end = s_height;
    if (x_start >= x_end || y_start >= y_end) {
        return;
    }

    const int lines = ST7789_FILL_LINES;
    const int draw_w = x_end - x_start;
    const int chunk_pixels = draw_w * lines;
    for (int i = 0; i < chunk_pixels; ++i) {
        s_fill_buf[i] = color;
    }

    for (int y = y_start; y < y_end; y += lines) {
        int y2 = y + lines;
        if (y2 > y_end) {
            y2 = y_end;
        }
        st7789_draw_bitmap_sync(x_start, y, x_end, y2, s_fill_buf);
    }
}

static void st7789_fill_color(uint16_t color)
{
    st7789_fill_rect_internal(0, 0, s_width, s_height, color);
}

esp_err_t st7789_init(const display_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_inited) {
        return ESP_OK;
    }

    s_width = config->width > 0 ? config->width : 240;
    s_height = config->height > 0 ? config->height : 240;
    s_backlight_pin = config->backlight_pin;
    s_spi_host = (spi_host_device_t)config->spi_host;

    spi_bus_config_t buscfg = {
        .sclk_io_num = config->sclk_pin,
        .mosi_io_num = config->mosi_pin,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = s_width * ST7789_FILL_LINES * (int)sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(s_spi_host, &buscfg, SPI_DMA_CH_AUTO), TAG,
                        "spi_bus_initialize failed");

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = config->dc_pin,
        .cs_gpio_num = config->cs_pin,
        .pclk_hz = ST7789_SAFE_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = ST7789_SPI_MODE,
        .trans_queue_depth = 1,
        .on_color_trans_done = st7789_color_trans_done,
    };
    s_color_done_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_color_done_sem != NULL, ESP_ERR_NO_MEM, TAG, "create color done semaphore failed");
    io_config.user_ctx = s_color_done_sem;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)s_spi_host, &io_config, &s_io),
                        TAG, "new_panel_io_spi failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = config->rst_pin,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel), TAG,
                        "new_panel_st7789 failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel_reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init failed");
    /* 面包板/杜邦线场景下给控制器一点稳定时间，减少首帧花屏概率 */
    vTaskDelay(pdMS_TO_TICKS(120));
    /* 1.54" 240x240 ST7789V: no mirror, no gap */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "set_gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert_color failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, ST7789_MIRROR_X, ST7789_MIRROR_Y), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, false), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display_on failed");

    const size_t fill_bytes = (size_t)s_width * (size_t)ST7789_FILL_LINES * sizeof(uint16_t);
    s_fill_buf = (uint16_t *)heap_caps_malloc(fill_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_fill_buf != NULL, ESP_ERR_NO_MEM, TAG, "alloc fill buffer failed");

    if (s_backlight_pin >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << s_backlight_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "backlight gpio config failed");
        gpio_set_level(s_backlight_pin, 1);
    }

    st7789_fill_color(0x0000);
    s_inited = true;
    ESP_LOGI(TAG, "ST7789 initialized: %dx%d, SPI host=%d mode=%d pclk=%d mirror=(%d,%d) MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%d",
             s_width, s_height, (int)s_spi_host,
             ST7789_SPI_MODE, ST7789_SAFE_PCLK_HZ,
             (int)ST7789_MIRROR_X, (int)ST7789_MIRROR_Y,
             config->mosi_pin, config->sclk_pin, config->cs_pin,
             config->dc_pin, config->rst_pin, s_backlight_pin);
    return ESP_OK;
}

void st7789_deinit(void)
{
    if (!s_inited) {
        return;
    }

    if (s_backlight_pin >= 0) {
        gpio_set_level(s_backlight_pin, 0);
    }
    if (s_panel) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
    if (s_fill_buf) {
        free(s_fill_buf);
        s_fill_buf = NULL;
    }
    if (s_color_done_sem) {
        vSemaphoreDelete(s_color_done_sem);
        s_color_done_sem = NULL;
    }
    spi_bus_free(s_spi_host);
    s_inited = false;
}

void st7789_clear(void)
{
    st7789_fill_color(0x0000);
}

void st7789_update(void)
{
    /* ST7789 draws immediately in current implementation */
}

void st7789_set_brightness(uint8_t brightness)
{
    if (s_backlight_pin < 0) {
        return;
    }
    gpio_set_level(s_backlight_pin, brightness > 0 ? 1 : 0);
}

void st7789_set_power(bool on)
{
    if (!s_panel) {
        return;
    }
    esp_lcd_panel_disp_on_off(s_panel, on);
    if (s_backlight_pin >= 0) {
        gpio_set_level(s_backlight_pin, on ? 1 : 0);
    }
}

static void st7789_draw_glyph_to_buffer(char c, int scale, uint16_t fg, uint16_t bg,
                                        uint16_t *dst, int dst_w, int dst_x)
{
    if (!dst || dst_w <= 0 || scale <= 0) return;
    if (c < 32 || c > 127) c = '?';

    const uint8_t *glyph = font8x8[c - 32];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint16_t color = (glyph[row] & (1 << col)) ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = dst_x + col * scale + sx;
                    int py = row * scale + sy;
                    dst[py * dst_w + px] = color;
                }
            }
        }
    }
}

/* 解码 1 个 UTF-8 字符，返回码点并推进指针；非法字节返回 U+FFFD。 */
static uint32_t st7789_utf8_decode(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t cp = 0;
    int extra = 0;

    if (!p || *p == '\0') {
        return 0;
    }

    if (*p < 0x80) {
        cp = *p++;
        extra = 0;
    } else if ((*p & 0xE0) == 0xC0) {
        cp = *p++ & 0x1F;
        extra = 1;
    } else if ((*p & 0xF0) == 0xE0) {
        cp = *p++ & 0x0F;
        extra = 2;
    } else if ((*p & 0xF8) == 0xF0) {
        cp = *p++ & 0x07;
        extra = 3;
    } else {
        *pp = (const char *)(p + 1);
        return 0xFFFD;
    }

    for (int i = 0; i < extra; ++i) {
        if ((*p & 0xC0) != 0x80) {
            *pp = (const char *)p;
            return 0xFFFD;
        }
        cp = (cp << 6) | (*p++ & 0x3F);
    }

    *pp = (const char *)p;
    return cp;
}

/* 与 SSD1306 一致：判断常见 CJK/全角符号范围。 */
static bool st7789_is_cjk(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||
           (cp >= 0x3000 && cp <= 0x303F) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) ||
           (cp >= 0xFF00 && cp <= 0xFFEF);
}

/* 把 16x16 单色 CJK 字形映射到任意目标尺寸并写入目标缓冲。 */
static void st7789_draw_cjk_to_buffer(const uint8_t *glyph, int target_w, int target_h,
                                      uint16_t fg, uint16_t bg, uint16_t *dst, int dst_w, int dst_x)
{
    if (!glyph || !dst || dst_w <= 0 || target_w <= 0 || target_h <= 0) {
        return;
    }

    for (int py = 0; py < target_h; ++py) {
        int src_y = (py * 16) / target_h;
        if (src_y > 15) src_y = 15;
        uint8_t hi = glyph[src_y * 2];
        uint8_t lo = glyph[src_y * 2 + 1];

        for (int px = 0; px < target_w; ++px) {
            int src_x = (px * 16) / target_w;
            if (src_x > 15) src_x = 15;
            bool on = (src_x < 8) ? ((hi & (0x80 >> src_x)) != 0)
                                  : ((lo & (0x80 >> (src_x - 8))) != 0);
            dst[py * dst_w + (dst_x + px)] = on ? fg : bg;
        }
    }
}

/* 绘制单个 ASCII 字符。 */
static void st7789_draw_ascii_char(int x, int y, char c, int scale,
                                   uint16_t fg, uint16_t bg)
{
    if (!s_panel || !s_fill_buf || scale <= 0) return;

    const int cw = 8 * scale;
    const int ch = 8 * scale;
    if (x + cw > s_width || y + ch > s_height || x < 0 || y < 0) return;

    for (int i = 0; i < cw * ch; ++i) {
        s_fill_buf[i] = bg;
    }
    st7789_draw_glyph_to_buffer(c, scale, fg, bg, s_fill_buf, cw, 0);
    st7789_draw_bitmap_sync(x, y, x + cw, y + ch, s_fill_buf);
}

/* 绘制单个 CJK 码点（使用 16x16 字库，按目标单元尺寸缩放）。 */
static void st7789_draw_cjk_char(int x, int y, uint32_t cp, int cell_w, int cell_h,
                                 uint16_t fg, uint16_t bg)
{
    if (!s_panel || !s_fill_buf || cell_w <= 0 || cell_h <= 0) return;
    if (x < 0 || y < 0 || x + cell_w > s_width || y + cell_h > s_height) return;

    const int pixels = cell_w * cell_h;
    if (pixels <= 0 || pixels > s_width * ST7789_FILL_LINES) return;

    for (int i = 0; i < pixels; ++i) {
        s_fill_buf[i] = bg;
    }

    const uint8_t *bmp = font_cjk_get_glyph(cp);
    if (!bmp) {
        /* 字库缺失时用 ASCII 占位符，避免出现按字节连串问号。 */
        int fallback_scale = (cell_w >= 16 && cell_h >= 16) ? 2 : 1;
        st7789_draw_glyph_to_buffer('?', fallback_scale, fg, bg, s_fill_buf, cell_w, 0);
    } else {
        st7789_draw_cjk_to_buffer(bmp, cell_w, cell_h, fg, bg, s_fill_buf, cell_w, 0);
    }

    st7789_draw_bitmap_sync(x, y, x + cell_w, y + cell_h, s_fill_buf);
}

void st7789_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    st7789_fill_rect_internal(x, y, x + w, y + h, color);
}

void st7789_draw_status_line(const char *icon, uint16_t icon_color,
                             const char *text, uint16_t text_color,
                             uint16_t bg_color)
{
    if (!s_panel || !s_fill_buf || !text) return;

    const char icon_char = (icon && icon[0] != '\0') ? icon[0] : ' ';
    const int x = 4;
    const int margin_r = 4;
    int scale = 2;
    const int text_len = (int)strlen(text);
    const int max_w = s_width - x - margin_r;

    if ((text_len + 2) * 8 * scale > max_w) {
        scale = 1;
    }

    const int char_w = 8 * scale;
    const int char_h = 8 * scale;
    int max_chars = max_w / char_w;
    if (max_chars < 2) return;

    int draw_text_chars = text_len;
    if (draw_text_chars > max_chars - 2) {
        draw_text_chars = max_chars - 2;
    }

    const int line_chars = 2 + draw_text_chars;
    const int draw_w = line_chars * char_w;
    const int draw_h = char_h;
    const int draw_pixels = draw_w * draw_h;
    if (draw_pixels <= 0 || draw_pixels > s_width * ST7789_FILL_LINES) return;

    for (int i = 0; i < draw_pixels; ++i) {
        s_fill_buf[i] = bg_color;
    }

    st7789_draw_glyph_to_buffer(icon_char, scale, icon_color, bg_color, s_fill_buf, draw_w, 0);
    st7789_draw_glyph_to_buffer(' ', scale, text_color, bg_color, s_fill_buf, draw_w, char_w);
    for (int i = 0; i < draw_text_chars; ++i) {
        st7789_draw_glyph_to_buffer(text[i], scale, text_color, bg_color,
                                    s_fill_buf, draw_w, (i + 2) * char_w);
    }

    const int y = (scale == 2) ? 4 : 8;
    st7789_draw_bitmap_sync(x, y, x + draw_w, y + draw_h, s_fill_buf);
}

/* Draw ASCII string at (x,y) with scale */
void st7789_draw_text(int x, int y, const char *text, int scale,
                      uint16_t fg, uint16_t bg)
{
    if (!text || scale <= 0) return;

    int cx = x;
    int cy = y;
    const int cell_w = 8 * scale;
    const int cell_h = 8 * scale;
    const int line_gap = 2;
    const char *p = text;

    while (*p) {
        if (*p == '\n') {
            ++p;
            cx = x;
            cy += cell_h + line_gap;
            if (cy + cell_h > s_height) break;
            continue;
        }

        uint32_t cp = st7789_utf8_decode(&p);
        if (cp == 0) break;

        if (cx + cell_w > s_width) {
            cx = x;
            cy += cell_h + line_gap;
            if (cy + cell_h > s_height) break;
        }

        if (st7789_is_cjk(cp)) {
            st7789_draw_cjk_char(cx, cy, cp, cell_w, cell_h, fg, bg);
        } else if (cp <= 0x7F) {
            st7789_draw_ascii_char(cx, cy, (char)cp, scale, fg, bg);
        } else {
            st7789_draw_ascii_char(cx, cy, '?', scale, fg, bg);
        }

        cx += cell_w;
    }
}

void st7789_render_status(display_status_t status)
{
    (void)status;
    st7789_fill_rect(0, 0, s_width, 32, 0x0000);
}
