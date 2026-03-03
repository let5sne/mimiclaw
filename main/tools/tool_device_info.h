#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute get_device_info tool.
 * Input: {}
 */
esp_err_t tool_get_device_info_execute(const char *input_json, char *output, size_t output_size);
