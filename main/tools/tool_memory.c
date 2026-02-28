#include "tools/tool_memory.h"
#include "memory/memory_store.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_memory";

#define MEMORY_LONG_TERM_MAX_BYTES  (16 * 1024)
#define MEMORY_NOTE_MAX_BYTES       1024

esp_err_t tool_memory_write_long_term_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));
    if (!content) {
        snprintf(output, output_size, "Error: missing 'content' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(content);
    if (len > MEMORY_LONG_TERM_MAX_BYTES) {
        snprintf(output, output_size, "Error: content too large (%d > %d bytes)",
                 (int)len, MEMORY_LONG_TERM_MAX_BYTES);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = memory_write_long_term(content);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to write MEMORY.md (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size, "OK: long-term memory updated (%d bytes)", (int)len);
    ESP_LOGI(TAG, "memory_write_long_term: %d bytes", (int)len);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_memory_append_today_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *note = cJSON_GetStringValue(cJSON_GetObjectItem(root, "note"));
    if (!note) {
        snprintf(output, output_size, "Error: missing 'note' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(note);
    if (len > MEMORY_NOTE_MAX_BYTES) {
        snprintf(output, output_size, "Error: note too large (%d > %d bytes)",
                 (int)len, MEMORY_NOTE_MAX_BYTES);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = memory_append_today(note);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to append daily note (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size, "OK: appended to today's note (%d bytes)", (int)len);
    ESP_LOGI(TAG, "memory_append_today: %d bytes", (int)len);
    cJSON_Delete(root);
    return ESP_OK;
}
