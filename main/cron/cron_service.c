#include "cron_service.h"
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
#include "nvs.h"

static const char *TAG = "cron";

static TaskHandle_t s_task = NULL;
static bool s_inited = false;
static bool s_started = false;
static cron_stats_t s_stats = {0};
static char s_task_text[MIMI_CRON_TASK_MAX_BYTES + 1] = {0};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

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

static bool is_valid_interval(uint32_t interval_min)
{
    return interval_min >= MIMI_CRON_MIN_INTERVAL_MIN &&
           interval_min <= MIMI_CRON_MAX_INTERVAL_MIN;
}

static void append_line(char *out, size_t out_size, const char *line)
{
    if (!out || !line || out_size < 2) return;

    size_t used = strlen(out);
    size_t line_len = strlen(line);
    if (line_len == 0) return;

    if (used + line_len + 2 >= out_size) return;
    memcpy(out + used, line, line_len);
    used += line_len;
    out[used++] = '\n';
    out[used] = '\0';
}

static esp_err_t parse_cron_file(uint32_t *out_interval_min, char *out_task, size_t out_task_size)
{
    if (!out_interval_min || !out_task || out_task_size < 2) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(MIMI_CRON_FILE, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    char *raw = calloc(1, MIMI_CRON_FILE_MAX_BYTES + 1);
    char *task = calloc(1, MIMI_CRON_TASK_MAX_BYTES + 1);
    if (!raw || !task) {
        free(raw);
        free(task);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t n = fread(raw, 1, MIMI_CRON_FILE_MAX_BYTES, f);
    fclose(f);
    raw[n] = '\0';

    if (n == 0) {
        free(raw);
        free(task);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t interval = MIMI_CRON_DEFAULT_INTERVAL_MIN;

    char *saveptr = NULL;
    char *line = strtok_r(raw, "\n", &saveptr);
    while (line) {
        trim_line(line);
        if (line[0] == '\0' || line[0] == '#') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        const char *k_interval = "every_minutes:";
        const char *k_task = "task:";
        if (strncmp(line, k_interval, strlen(k_interval)) == 0) {
            const char *v = line + strlen(k_interval);
            while (*v && isspace((unsigned char)*v)) v++;
            int parsed = atoi(v);
            if (parsed > 0) interval = (uint32_t)parsed;
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        if (strncmp(line, k_task, strlen(k_task)) == 0) {
            const char *v = line + strlen(k_task);
            while (*v && isspace((unsigned char)*v)) v++;
            append_line(task, sizeof(task), v);
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        append_line(task, sizeof(task), line);
        line = strtok_r(NULL, "\n", &saveptr);
    }

    trim_line(task);
    if (!is_valid_interval(interval) || task[0] == '\0') {
        free(raw);
        free(task);
        return ESP_ERR_INVALID_SIZE;
    }

    *out_interval_min = interval;
    strncpy(out_task, task, out_task_size - 1);
    out_task[out_task_size - 1] = '\0';
    free(raw);
    free(task);
    return ESP_OK;
}

static esp_err_t load_config_from_nvs(uint32_t *out_interval_min, char *out_task, size_t out_task_size)
{
    if (!out_interval_min || !out_task || out_task_size < 2) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_CRON, NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t interval = 0;
    esp_err_t err_interval = nvs_get_u32(nvs, MIMI_NVS_KEY_CRON_INTERVAL, &interval);

    size_t len = out_task_size;
    out_task[0] = '\0';
    esp_err_t err_task = nvs_get_str(nvs, MIMI_NVS_KEY_CRON_TASK, out_task, &len);
    nvs_close(nvs);

    if (err_interval != ESP_OK || err_task != ESP_OK || !is_valid_interval(interval) || out_task[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    *out_interval_min = interval;
    return ESP_OK;
}

static esp_err_t persist_config_to_nvs(uint32_t interval_min, const char *task)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_CRON, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_u32(nvs, MIMI_NVS_KEY_CRON_INTERVAL, interval_min);
    if (err == ESP_OK) err = nvs_set_str(nvs, MIMI_NVS_KEY_CRON_TASK, task);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t clear_config_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_CRON, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_erase_key(nvs, MIMI_NVS_KEY_CRON_INTERVAL);
    nvs_erase_key(nvs, MIMI_NVS_KEY_CRON_TASK);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void copy_state(bool *enabled, uint32_t *interval_min, char *task, size_t task_size)
{
    if (!enabled || !interval_min || !task || task_size < 2) return;

    portENTER_CRITICAL(&s_lock);
    *enabled = s_stats.enabled;
    *interval_min = s_stats.interval_min;
    strncpy(task, s_task_text, task_size - 1);
    task[task_size - 1] = '\0';
    portEXIT_CRITICAL(&s_lock);
}

static void set_state(bool enabled, uint32_t interval_min, const char *task)
{
    portENTER_CRITICAL(&s_lock);
    s_stats.enabled = enabled;
    s_stats.interval_min = interval_min;
    if (task) {
        strncpy(s_task_text, task, sizeof(s_task_text) - 1);
        s_task_text[sizeof(s_task_text) - 1] = '\0';
    } else {
        s_task_text[0] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
}

static void run_once(const char *reason, uint32_t interval_min, const char *task)
{
    if (!task || task[0] == '\0' || !is_valid_interval(interval_min)) {
        portENTER_CRITICAL(&s_lock);
        s_stats.skipped_not_configured++;
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    uint32_t now_unix = (uint32_t)time(NULL);
    char time_buf[32] = {0};
    time_t now_t = (time_t)now_unix;
    struct tm tm_info;
    if (localtime_r(&now_t, &tm_info)) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    } else {
        snprintf(time_buf, sizeof(time_buf), "%" PRIu32, now_unix);
    }

    size_t payload_cap = strlen(task) + 224;
    char *payload = calloc(1, payload_cap);
    if (!payload) {
        portENTER_CRITICAL(&s_lock);
        s_stats.enqueue_failures++;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "No memory for cron payload");
        return;
    }

    snprintf(payload, payload_cap,
             "Cron trigger (%s) at %s, interval=%" PRIu32 " min.\n"
             "Execute the scheduled task below:\n%s",
             reason ? reason : "interval", time_buf, interval_min, task);

    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_SYSTEM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "cron", sizeof(msg.chat_id) - 1);
    strncpy(msg.media_type, "system", sizeof(msg.media_type) - 1);
    msg.content = payload;

    esp_err_t push_err = message_bus_push_inbound(&msg);

    portENTER_CRITICAL(&s_lock);
    s_stats.total_runs++;
    s_stats.last_run_unix = now_unix;
    if (push_err == ESP_OK) {
        s_stats.triggered_runs++;
        s_stats.enqueue_success++;
    } else {
        s_stats.enqueue_failures++;
    }
    portEXIT_CRITICAL(&s_lock);

    if (push_err == ESP_OK) {
        ESP_LOGI(TAG, "Cron triggered (%s), interval=%" PRIu32 " min, payload=%d bytes",
                 reason ? reason : "interval", interval_min, (int)strlen(payload));
    } else {
        ESP_LOGW(TAG, "Cron enqueue failed: %s", esp_err_to_name(push_err));
        free(payload);
    }
}

static void cron_task(void *arg)
{
    ESP_LOGI(TAG, "Cron task started, fallback file=%s", MIMI_CRON_FILE);

    while (1) {
        bool enabled = false;
        uint32_t interval_min = 0;
        char task[MIMI_CRON_TASK_MAX_BYTES + 1] = {0};
        copy_state(&enabled, &interval_min, task, sizeof(task));

        uint32_t wait_sec = MIMI_CRON_DISABLED_POLL_S;
        if (enabled && is_valid_interval(interval_min)) {
            wait_sec = interval_min * 60;
        }

        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_sec * 1000));
        if (notified > 0) {
            copy_state(&enabled, &interval_min, task, sizeof(task));
            run_once("manual", interval_min, task);
        } else if (enabled) {
            run_once("interval", interval_min, task);
        }
    }
}

esp_err_t cron_service_init(void)
{
    if (s_inited) return ESP_OK;

    memset(&s_stats, 0, sizeof(s_stats));
    s_task_text[0] = '\0';

    char *task = calloc(1, MIMI_CRON_TASK_MAX_BYTES + 1);
    if (!task) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t interval_min = 0;
    esp_err_t err = load_config_from_nvs(&interval_min, task, MIMI_CRON_TASK_MAX_BYTES + 1);
    if (err == ESP_OK) {
        set_state(true, interval_min, task);
        ESP_LOGI(TAG, "Cron loaded from NVS: every %" PRIu32 " min", interval_min);
    } else if (parse_cron_file(&interval_min, task, MIMI_CRON_TASK_MAX_BYTES + 1) == ESP_OK) {
        set_state(true, interval_min, task);
        ESP_LOGI(TAG, "Cron loaded from file: every %" PRIu32 " min", interval_min);
    } else {
        set_state(false, 0, NULL);
        ESP_LOGI(TAG, "Cron disabled (no valid config)");
    }

    free(task);
    s_inited = true;
    return ESP_OK;
}

esp_err_t cron_service_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_started) return ESP_OK;

    BaseType_t ret = xTaskCreatePinnedToCore(
        cron_task, "cron",
        MIMI_CRON_STACK, NULL, MIMI_CRON_PRIO, &s_task, MIMI_AGENT_CORE);
    if (ret != pdPASS) return ESP_FAIL;

    s_started = true;
    return ESP_OK;
}

esp_err_t cron_service_trigger_now(void)
{
    if (!s_started || !s_task) return ESP_ERR_INVALID_STATE;

    bool enabled = false;
    uint32_t interval_min = 0;
    char task[MIMI_CRON_TASK_MAX_BYTES + 1] = {0};
    copy_state(&enabled, &interval_min, task, sizeof(task));
    if (!enabled || !is_valid_interval(interval_min) || task[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t cron_service_set_schedule(uint32_t interval_min, const char *task)
{
    if (!task) return ESP_ERR_INVALID_ARG;
    if (!is_valid_interval(interval_min)) return ESP_ERR_INVALID_ARG;

    char task_copy[MIMI_CRON_TASK_MAX_BYTES + 1];
    strncpy(task_copy, task, sizeof(task_copy) - 1);
    task_copy[sizeof(task_copy) - 1] = '\0';
    trim_line(task_copy);
    if (task_copy[0] == '\0') return ESP_ERR_INVALID_ARG;

    esp_err_t err = persist_config_to_nvs(interval_min, task_copy);
    if (err != ESP_OK) return err;

    set_state(true, interval_min, task_copy);
    ESP_LOGI(TAG, "Cron schedule set: every %" PRIu32 " min", interval_min);

    if (s_started && s_task) {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

esp_err_t cron_service_clear_schedule(void)
{
    esp_err_t err = clear_config_from_nvs();
    if (err != ESP_OK) return err;

    set_state(false, 0, NULL);
    ESP_LOGI(TAG, "Cron schedule cleared");

    if (s_started && s_task) {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

esp_err_t cron_service_get_stats(cron_stats_t *out_stats)
{
    if (!out_stats) return ESP_ERR_INVALID_ARG;
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&s_lock);
    *out_stats = s_stats;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t cron_service_get_task(char *out_task, size_t out_size)
{
    if (!out_task || out_size < 2) return ESP_ERR_INVALID_ARG;
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    portENTER_CRITICAL(&s_lock);
    strncpy(out_task, s_task_text, out_size - 1);
    out_task[out_size - 1] = '\0';
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
