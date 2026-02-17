#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * 写入长期记忆（覆盖 MEMORY.md）。
 * 输入 JSON: {"content":"..."}
 */
esp_err_t tool_memory_write_long_term_execute(const char *input_json, char *output, size_t output_size);

/**
 * 追加一条今日记忆到 daily 文件。
 * 输入 JSON: {"note":"..."}
 */
esp_err_t tool_memory_append_today_execute(const char *input_json, char *output, size_t output_size);

