#pragma once

#include "esp_err.h"

/**
 * 初始化飞书 Bot 配置。
 */
esp_err_t feishu_bot_init(void);

/**
 * 注册飞书事件回调 HTTP 入口。
 */
esp_err_t feishu_bot_start(void);

/**
 * 发送文本消息到飞书会话。
 */
esp_err_t feishu_send_message(const char *chat_id, const char *text);
