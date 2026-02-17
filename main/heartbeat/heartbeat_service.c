#include "heartbeat_service.h"
#include "mimi_config.h"
#include "bus/message_bus.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "heartbeat";

static TaskHandle_t s_task = NULL;
static bool s_inited = false;
static bool s_started = false;
static heartbeat_stats_t s_stats = {0};
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static void stats_inc(uint32_t *field)
{
    portENTER_CRITICAL(&s_stats_lock);
    (*field)++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stats_set_last_run(uint32_t ts_unix)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.last_run_unix = ts_unix;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void trim_line(char *s)
{
    if (!s) return;

    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

static bool build_actionable_text(char *raw, char *out, size_t out_size)
{
    if (!raw || !out || out_size < 2) return false;

    out[0] = '\0';
    size_t used = 0;

    char *saveptr = NULL;
    char *line = strtok_r(raw, "\n", &saveptr);
    while (line) {
        trim_line(line);
        if (line[0] != '\0' && line[0] != '#') {
            size_t line_len = strlen(line);
            if (used + line_len + 2 >= out_size) {
                break;
            }
            memcpy(out + used, line, line_len);
            used += line_len;
            out[used++] = '\n';
            out[used] = '\0';
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    return used > 0;
}

static esp_err_t load_heartbeat_text(char *out, size_t out_size)
{
    if (!out || out_size < 2) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(MIMI_HEARTBEAT_FILE, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    char raw[MIMI_HEARTBEAT_MAX_BYTES + 1];
    size_t n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    raw[n] = '\0';

    if (n == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!build_actionable_text(raw, out, out_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void run_once(const char *reason)
{
    uint32_t now_unix = (uint32_t)time(NULL);
    stats_inc(&s_stats.total_runs);
    stats_set_last_run(now_unix);

    char tasks[MIMI_HEARTBEAT_MAX_BYTES + 1];
    esp_err_t read_err = load_heartbeat_text(tasks, sizeof(tasks));
    if (read_err == ESP_ERR_NOT_FOUND) {
        stats_inc(&s_stats.skipped_no_file);
        ESP_LOGD(TAG, "Heartbeat skip: file missing (%s)", MIMI_HEARTBEAT_FILE);
        return;
    }
    if (read_err == ESP_ERR_INVALID_SIZE) {
        stats_inc(&s_stats.skipped_empty);
        ESP_LOGD(TAG, "Heartbeat skip: no actionable content");
        return;
    }
    if (read_err != ESP_OK) {
        stats_inc(&s_stats.skipped_read_error);
        ESP_LOGW(TAG, "Heartbeat read error: %s", esp_err_to_name(read_err));
        return;
    }

    char time_buf[32] = {0};
    time_t now_t = (time_t)now_unix;
    struct tm tm_info;
    if (localtime_r(&now_t, &tm_info)) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    } else {
        snprintf(time_buf, sizeof(time_buf), "%" PRIu32, now_unix);
    }

    size_t payload_cap = strlen(tasks) + 192;
    char *payload = calloc(1, payload_cap);
    if (!payload) {
        stats_inc(&s_stats.enqueue_failures);
        ESP_LOGE(TAG, "No memory for heartbeat payload");
        return;
    }

    snprintf(payload, payload_cap,
             "Heartbeat trigger (%s) at %s.\n"
             "Follow tasks below; keep updates concise:\n%s",
             reason ? reason : "interval", time_buf, tasks);

    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_SYSTEM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "heartbeat", sizeof(msg.chat_id) - 1);
    strncpy(msg.media_type, "system", sizeof(msg.media_type) - 1);
    msg.content = payload;

    esp_err_t push_err = message_bus_push_inbound(&msg);
    if (push_err == ESP_OK) {
        stats_inc(&s_stats.triggered_runs);
        stats_inc(&s_stats.enqueue_success);
        ESP_LOGI(TAG, "Heartbeat triggered (%s), payload=%d bytes",
                 reason ? reason : "interval", (int)strlen(payload));
    } else {
        stats_inc(&s_stats.enqueue_failures);
        ESP_LOGW(TAG, "Heartbeat enqueue failed: %s", esp_err_to_name(push_err));
        free(payload);
    }
}

static void heartbeat_task(void *arg)
{
    ESP_LOGI(TAG, "Heartbeat task started, interval=%d s, file=%s",
             MIMI_HEARTBEAT_INTERVAL_S, MIMI_HEARTBEAT_FILE);

    while (1) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MIMI_HEARTBEAT_INTERVAL_S * 1000));
        if (notified > 0) {
            run_once("manual");
        } else {
            run_once("interval");
        }
    }
}

esp_err_t heartbeat_service_init(void)
{
    if (s_inited) return ESP_OK;
    memset(&s_stats, 0, sizeof(s_stats));
    s_inited = true;
    return ESP_OK;
}

esp_err_t heartbeat_service_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_started) return ESP_OK;

    BaseType_t ret = xTaskCreatePinnedToCore(
        heartbeat_task, "heartbeat",
        MIMI_HEARTBEAT_STACK, NULL, MIMI_HEARTBEAT_PRIO, &s_task, MIMI_AGENT_CORE);
    if (ret != pdPASS) {
        return ESP_FAIL;
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t heartbeat_service_trigger_now(void)
{
    if (!s_started || !s_task) return ESP_ERR_INVALID_STATE;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t heartbeat_service_get_stats(heartbeat_stats_t *out_stats)
{
    if (!out_stats) return ESP_ERR_INVALID_ARG;
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&s_stats_lock);
    *out_stats = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
    return ESP_OK;
}
