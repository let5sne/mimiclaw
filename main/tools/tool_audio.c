#include "tools/tool_audio.h"
#include "audio/audio.h"

#include <stdio.h>
#include "cJSON.h"

esp_err_t tool_set_volume_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *volume_item = cJSON_GetObjectItem(root, "volume");
    if (!cJSON_IsNumber(volume_item)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing or invalid 'volume' field");
        return ESP_ERR_INVALID_ARG;
    }

    int volume = volume_item->valueint;
    if (volume < 0 || volume > 100) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: volume must be between 0 and 100");
        return ESP_ERR_INVALID_ARG;
    }

    audio_set_volume((uint8_t)volume);
    cJSON_Delete(root);
    snprintf(output, output_size, "OK: volume set to %d%%", volume);
    return ESP_OK;
}

esp_err_t tool_get_volume_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    snprintf(output, output_size, "Current volume: %u%%", (unsigned)audio_get_volume());
    return ESP_OK;
}
