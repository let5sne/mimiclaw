#pragma once

#include "esp_err.h"

/**
 * 初始化飞书 Bot 配置。
 */
esp_err_t feishu_bot_init(void);

/**
 * 启动飞书接收端。根据配置走 webhook 或板载 WebSocket 长连接。
 */
esp_err_t feishu_bot_start(void);

/**
 * 发送文本消息到飞书会话。
 */
esp_err_t feishu_send_message(const char *chat_id, const char *text);

/**
 * 运行时更新飞书 App ID / Secret。
 */
esp_err_t feishu_set_app_credentials(const char *app_id, const char *app_secret);
esp_err_t feishu_clear_app_credentials(void);

/**
 * 运行时更新飞书 Verify Token。
 */
esp_err_t feishu_set_verify_token(const char *token);
esp_err_t feishu_clear_verify_token(void);

/**
 * 运行时更新飞书 Encrypt Key。
 */
esp_err_t feishu_set_encrypt_key(const char *key);
esp_err_t feishu_clear_encrypt_key(void);

/**
 * 运行时更新飞书 OpenAPI 基地址。
 */
esp_err_t feishu_set_open_api_base(const char *base_url);
esp_err_t feishu_clear_open_api_base(void);

/**
 * 运行时更新飞书接收模式（webhook / websocket）。
 * 已启动后修改通常需要重启才会完全生效。
 */
esp_err_t feishu_set_receive_mode(const char *mode);
esp_err_t feishu_clear_receive_mode(void);
