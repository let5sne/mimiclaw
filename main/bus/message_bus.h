#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Channel identifiers */
#define MIMI_CHAN_TELEGRAM   "telegram"
#define MIMI_CHAN_WEBSOCKET  "websocket"
#define MIMI_CHAN_CLI        "cli"
#define MIMI_CHAN_VOICE      "voice"
#define MIMI_CHAN_SYSTEM     "system"

/* Message types on the bus */
typedef struct {
    char channel[16];       /* "telegram", "websocket", "cli" */
    char chat_id[32];       /* Telegram chat_id or WS client id */
    char media_type[16];    /* "text"/"voice"/"photo"/"document" */
    char file_id[96];       /* Source media file id (if any) */
    char file_path[128];    /* Source media path (if any) */
    char *content;          /* Heap-allocated message text (caller must free) */
    char *meta_json;        /* Optional JSON metadata (caller must free) */
} mimi_msg_t;

/**
 * Initialize the message bus (inbound + outbound FreeRTOS queues).
 */
esp_err_t message_bus_init(void);

/**
 * Push a message to the inbound queue (towards Agent Loop).
 * 成功时 bus 接管 msg->content；失败时调用方仍需负责释放。
 */
esp_err_t message_bus_push_inbound(const mimi_msg_t *msg);

/**
 * Pop a message from the inbound queue (blocking).
 * Caller must release heap fields via message_bus_msg_free() when done.
 */
esp_err_t message_bus_pop_inbound(mimi_msg_t *msg, uint32_t timeout_ms);

/**
 * Push a message to the outbound queue (towards channels).
 * 成功时 bus 接管 msg->content；失败时调用方仍需负责释放。
 */
esp_err_t message_bus_push_outbound(const mimi_msg_t *msg);

/**
 * Pop a message from the outbound queue (blocking).
 * Caller must release heap fields via message_bus_msg_free() when done.
 */
esp_err_t message_bus_pop_outbound(mimi_msg_t *msg, uint32_t timeout_ms);

/**
 * Free heap fields in a message and clear pointers.
 * Safe to call with partially-filled message.
 */
void message_bus_msg_free(mimi_msg_t *msg);
