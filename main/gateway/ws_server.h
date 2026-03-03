#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Initialize and start the WebSocket server on MIMI_WS_PORT.
 * Allows external clients to interact with the Agent via JSON messages.
 *
 * Protocol:
 *   Inbound:  {"type":"message","content":"hello","chat_id":"ws_client1"}
 *   Outbound: {"type":"response","content":"Hi!","chat_id":"ws_client1"}
 *
 * Optional auth:
 *   If WS token is configured, client must provide:
 *   - Header: X-WS-Token: <token>
 *   - or query string: ws://host:18789/?token=<token>
 */
esp_err_t ws_server_start(void);

/**
 * Send a text message to a specific WebSocket client by chat_id.
 * @param chat_id  Client identifier (assigned on connection)
 * @param text     Message text
 */
esp_err_t ws_server_send(const char *chat_id, const char *text);

/**
 * 在已启动的 HTTP 服务器上注册额外 URI。
 */
esp_err_t ws_server_register_uri(const httpd_uri_t *uri);

/**
 * Stop the WebSocket server.
 */
esp_err_t ws_server_stop(void);
