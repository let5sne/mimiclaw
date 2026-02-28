#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute set_volume tool.
 * Input: {"volume": 0..100}
 */
esp_err_t tool_set_volume_execute(const char *input_json, char *output, size_t output_size);

/**
 * Execute get_volume tool.
 * Input: {}
 */
esp_err_t tool_get_volume_execute(const char *input_json, char *output, size_t output_size);
