#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_CJK_GLYPH_BYTES  32   /* 16x16 monochrome: 2 bytes/row * 16 rows */
#define FONT_CJK_CACHE_SLOTS  64

/**
 * Initialize CJK font subsystem.
 * Loads the index table from SPIFFS into PSRAM for fast binary search.
 *
 * @param path  Path to the .bin font file on SPIFFS (e.g. "/spiffs/fonts/unifont_cjk.bin")
 * @return ESP_OK on success
 */
esp_err_t font_cjk_init(const char *path);

/**
 * Look up a glyph bitmap for a Unicode codepoint.
 *
 * @param codepoint  Unicode codepoint (e.g. 0x4F60 for '你')
 * @return Pointer to 32-byte bitmap (row-major, 2 bytes/row, MSB-left), or NULL if not found.
 *         The pointer is valid until the next call that evicts this cache slot.
 */
const uint8_t *font_cjk_get_glyph(uint32_t codepoint);

/**
 * Check if the CJK font subsystem is initialized and ready.
 */
bool font_cjk_is_ready(void);

#ifdef __cplusplus
}
#endif
