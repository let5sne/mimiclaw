#include "message_bus.h"
#include "mimi_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/task.h"

static const char *TAG = "bus";

static QueueHandle_t s_inbound_queue;
static QueueHandle_t s_outbound_queue;

static bool outbound_is_status(const mimi_msg_t *msg)
{
    if (!msg || !msg->content) return false;
    return (strncmp(msg->content, "mimi", 4) == 0) && (strstr(msg->content, "...") != NULL);
}

static uint32_t outbound_retry_delay_ms(int attempt)
{
    uint32_t delay = MIMI_OUTBOUND_QUEUE_RETRY_BASE_MS;
    for (int i = 1; i < attempt; i++) {
        delay <<= 1;
        if (delay > 5000) {
            delay = 5000;
            break;
        }
    }
    return delay;
}

esp_err_t message_bus_init(void)
{
    s_inbound_queue = xQueueCreate(MIMI_BUS_QUEUE_LEN, sizeof(mimi_msg_t));
    s_outbound_queue = xQueueCreate(MIMI_BUS_QUEUE_LEN, sizeof(mimi_msg_t));

    if (!s_inbound_queue || !s_outbound_queue) {
        ESP_LOGE(TAG, "Failed to create message queues");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Message bus initialized (queue depth %d)", MIMI_BUS_QUEUE_LEN);
    return ESP_OK;
}

esp_err_t message_bus_push_inbound(const mimi_msg_t *msg)
{
    if (xQueueSend(s_inbound_queue, msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Inbound queue full, dropping message");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t message_bus_pop_inbound(mimi_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_inbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t message_bus_push_outbound(const mimi_msg_t *msg)
{
    bool is_status = outbound_is_status(msg);
    int max_attempts = is_status ? 1 : MIMI_OUTBOUND_QUEUE_RETRY_MAX;
    TickType_t wait_ticks = is_status ? 0 : pdMS_TO_TICKS(MIMI_OUTBOUND_FINAL_WAIT_MS);

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        if (xQueueSend(s_outbound_queue, msg, wait_ticks) == pdTRUE) {
            return ESP_OK;
        }

        if (attempt < max_attempts) {
            uint32_t delay_ms = outbound_retry_delay_ms(attempt);
            ESP_LOGW(TAG, "Outbound queue full, retry enqueue (%d/%d) in %" PRIu32 " ms",
                     attempt, max_attempts, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    ESP_LOGW(TAG, "Outbound queue full, dropping %s message",
             is_status ? "status" : "final");
    return ESP_ERR_NO_MEM;
}

esp_err_t message_bus_pop_outbound(mimi_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_outbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void message_bus_msg_free(mimi_msg_t *msg)
{
    if (!msg) return;
    free(msg->content);
    msg->content = NULL;
    free(msg->meta_json);
    msg->meta_json = NULL;
}
