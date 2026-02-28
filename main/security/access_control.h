#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * 初始化安全策略（构建时默认值 + NVS 覆盖）。
 */
esp_err_t access_control_init(void);

/**
 * Telegram allowlist 校验。
 * - allowlist 为空或为 "*" 时，默认放行。
 * - 否则 sender_id 必须命中逗号分隔列表。
 */
bool access_control_is_telegram_allowed(const char *sender_id);

/**
 * WebSocket 是否启用 token 鉴权（token 非空即启用）。
 */
bool access_control_ws_token_required(void);

/**
 * 校验 WebSocket token。
 */
bool access_control_validate_ws_token(const char *token);

/**
 * 获取当前 allowlist / ws token（只读）。
 */
const char *access_control_get_allow_from(void);
const char *access_control_get_ws_token(void);

/**
 * 运行时更新配置（写入 NVS）。
 */
esp_err_t access_control_set_allow_from(const char *allow_from);
esp_err_t access_control_clear_allow_from(void);
esp_err_t access_control_set_ws_token(const char *token);
esp_err_t access_control_clear_ws_token(void);

