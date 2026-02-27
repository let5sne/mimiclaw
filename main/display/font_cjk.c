#include "font_cjk.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "font_cjk";

/* MCFN file format:
 *   [0..3]   magic "MCFN"
 *   [4..7]   uint32 LE  glyph_count
 *   [8 .. 8+glyph_count*4-1]   sorted uint32 LE codepoints (index table)
 *   [8+glyph_count*4 .. ]      32-byte bitmaps, same order as index
 */
#define MCFN_MAGIC  0x4E46434D  /* "MCFN" little-endian */

/* Index table in PSRAM */
static uint32_t *s_index = NULL;
static uint32_t  s_count = 0;

/* Font file handle – kept open for on-demand glyph reads */
static FILE *s_fp = NULL;
static uint32_t s_bitmap_offset = 0;  /* byte offset where bitmaps start */

/* LRU cache */
typedef struct {
    uint32_t codepoint;
    uint8_t  bitmap[FONT_CJK_GLYPH_BYTES];
    uint32_t age;
} cache_slot_t;

static cache_slot_t *s_cache = NULL;
static uint32_t s_age_counter = 0;

static bool s_ready = false;

/* Binary search the index table */
static int index_search(uint32_t cp)
{
    int lo = 0, hi = (int)s_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (s_index[mid] == cp) return mid;
        if (s_index[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* Read a single glyph bitmap from the file */
static bool read_glyph(int idx, uint8_t *out)
{
    if (!s_fp) return false;
    long offset = s_bitmap_offset + (long)idx * FONT_CJK_GLYPH_BYTES;
    if (fseek(s_fp, offset, SEEK_SET) != 0) return false;
    return fread(out, 1, FONT_CJK_GLYPH_BYTES, s_fp) == FONT_CJK_GLYPH_BYTES;
}

esp_err_t font_cjk_init(const char *path)
{
    if (s_ready) return ESP_OK;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "Font file not found: %s (CJK disabled)", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Read & verify header */
    uint32_t magic = 0, count = 0;
    if (fread(&magic, 4, 1, fp) != 1 || fread(&count, 4, 1, fp) != 1) {
        ESP_LOGE(TAG, "Failed to read font header");
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }
    if (magic != MCFN_MAGIC) {
        ESP_LOGE(TAG, "Bad font magic: 0x%08X", (unsigned)magic);
        fclose(fp);
        return ESP_ERR_INVALID_STATE;
    }
    if (count == 0 || count > 100000) {
        ESP_LOGE(TAG, "Invalid glyph count: %u", (unsigned)count);
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Allocate index table in PSRAM */
    size_t idx_size = count * sizeof(uint32_t);
    s_index = heap_caps_malloc(idx_size, MALLOC_CAP_SPIRAM);
    if (!s_index) {
        ESP_LOGE(TAG, "Failed to alloc index (%u bytes)", (unsigned)idx_size);
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    if (fread(s_index, sizeof(uint32_t), count, fp) != count) {
        ESP_LOGE(TAG, "Failed to read index table");
        heap_caps_free(s_index);
        s_index = NULL;
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    s_count = count;
    s_bitmap_offset = 8 + count * 4;

    /* Allocate LRU cache in PSRAM */
    s_cache = heap_caps_calloc(FONT_CJK_CACHE_SLOTS, sizeof(cache_slot_t), MALLOC_CAP_SPIRAM);
    if (!s_cache) {
        ESP_LOGE(TAG, "Failed to alloc glyph cache");
        heap_caps_free(s_index);
        s_index = NULL;
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    s_fp = fp;  /* Keep file open for glyph reads */
    s_ready = true;

    ESP_LOGI(TAG, "CJK font loaded: %u glyphs, index=%uKB",
             (unsigned)s_count, (unsigned)(idx_size / 1024));
    return ESP_OK;
}

const uint8_t *font_cjk_get_glyph(uint32_t codepoint)
{
    if (!s_ready) return NULL;

    /* Check cache first */
    int oldest_slot = 0;
    uint32_t oldest_age = UINT32_MAX;

    for (int i = 0; i < FONT_CJK_CACHE_SLOTS; i++) {
        if (s_cache[i].codepoint == codepoint && s_cache[i].age > 0) {
            s_cache[i].age = ++s_age_counter;
            return s_cache[i].bitmap;
        }
        if (s_cache[i].age < oldest_age) {
            oldest_age = s_cache[i].age;
            oldest_slot = i;
        }
    }

    /* Cache miss – look up in index */
    int idx = index_search(codepoint);
    if (idx < 0) return NULL;

    /* Read bitmap from file into LRU slot */
    if (!read_glyph(idx, s_cache[oldest_slot].bitmap)) {
        ESP_LOGW(TAG, "Failed to read glyph U+%04X", (unsigned)codepoint);
        return NULL;
    }

    s_cache[oldest_slot].codepoint = codepoint;
    s_cache[oldest_slot].age = ++s_age_counter;
    return s_cache[oldest_slot].bitmap;
}

bool font_cjk_is_ready(void)
{
    return s_ready;
}
