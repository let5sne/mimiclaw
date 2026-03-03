#include "session_mgr.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "session";
#define SESSION_PATH_MAX 192
#define SESSION_FILE_PREFIX "s"

static const char *session_channel_prefix(const char *channel)
{
    if (!channel || channel[0] == '\0') {
        return "h";
    }
    if (strcmp(channel, "telegram") == 0) {
        return "t";
    }
    if (strcmp(channel, "feishu") == 0) {
        return "f";
    }
    if (strcmp(channel, "websocket") == 0) {
        return "w";
    }
    if (strcmp(channel, "voice") == 0) {
        return "v";
    }
    if (strcmp(channel, "cli") == 0) {
        return "c";
    }
    if (strcmp(channel, "system") == 0) {
        return "y";
    }
    return "h";
}

static uint64_t session_hash_id(const char *channel, const char *chat_id)
{
    const uint64_t fnv_offset = 1469598103934665603ULL;
    const uint64_t fnv_prime = 1099511628211ULL;
    uint64_t hash = fnv_offset;

    const char *prefix = session_channel_prefix(channel);
    for (const unsigned char *p = (const unsigned char *)prefix; p && *p; p++) {
        hash ^= (uint64_t)(*p);
        hash *= fnv_prime;
    }
    hash ^= (uint64_t)':';
    hash *= fnv_prime;

    for (const unsigned char *p = (const unsigned char *)(chat_id ? chat_id : "");
         p && *p; p++) {
        hash ^= (uint64_t)(*p);
        hash *= fnv_prime;
    }
    return hash;
}

static void session_path_ex(const char *channel, const char *chat_id, char *buf, size_t size)
{
    const char *prefix = session_channel_prefix(channel);
    uint64_t hash = session_hash_id(channel, chat_id);
    snprintf(buf, size, "%s/%s%s%016llx.j", MIMI_SPIFFS_BASE,
             SESSION_FILE_PREFIX, prefix, (unsigned long long)hash);
}

esp_err_t session_mgr_init(void)
{
    ESP_LOGI(TAG, "Session manager initialized at %s (%s<channel><hash>.j)",
             MIMI_SPIFFS_BASE, SESSION_FILE_PREFIX);
    return ESP_OK;
}

esp_err_t session_append_ex(const char *channel, const char *chat_id,
                            const char *role, const char *content)
{
    char path[SESSION_PATH_MAX];
    session_path_ex(channel, chat_id, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open session file %s: errno=%d (%s)",
                 path, errno, strerror(errno));
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t session_append(const char *chat_id, const char *role, const char *content)
{
    return session_append_ex("telegram", chat_id, role, content);
}

esp_err_t session_get_history_json_ex(const char *channel, const char *chat_id,
                                      char *buf, size_t size, int max_msgs)
{
    char path[SESSION_PATH_MAX];
    session_path_ex(channel, chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        /* No history yet */
        snprintf(buf, size, "[]");
        return ESP_OK;
    }

    /* Read all lines into a ring buffer of cJSON objects */
    cJSON *messages[MIMI_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        /* Ring buffer: overwrite oldest if full */
        if (count >= max_msgs) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    fclose(f);

    /* Build JSON array with only role + content */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (role && content) {
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
        }
        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (cleanup_start + i) % max_msgs;
        cJSON_Delete(messages[idx]);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    return ESP_OK;
}

esp_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    return session_get_history_json_ex("telegram", chat_id, buf, size, max_msgs);
}

esp_err_t session_clear(const char *chat_id)
{
    const char *channels[] = {"telegram", "feishu", "websocket", "voice",
                              "cli", "system", "chat"};
    const char *legacy_prefixes[] = {"tg", "feishu", "ws", "voice", "cli", "system", "chat"};
    bool removed = false;

    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        char path[SESSION_PATH_MAX];
        session_path_ex(channels[i], chat_id, path, sizeof(path));
        if (remove(path) == 0) {
            ESP_LOGI(TAG, "Session cleared: %s", path);
            removed = true;
        }
    }

    /* 清理旧版未哈希命名，避免历史残留占空间。 */
    for (size_t i = 0; i < sizeof(legacy_prefixes) / sizeof(legacy_prefixes[0]); i++) {
        char path[SESSION_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s_%s.jsonl",
                 MIMI_SPIFFS_SESSION_DIR, legacy_prefixes[i], chat_id ? chat_id : "");
        if (remove(path) == 0) {
            ESP_LOGI(TAG, "Legacy session cleared: %s", path);
            removed = true;
        }
    }

    return removed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void session_list(void)
{
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open SPIFFS directory");
        return;
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, SESSION_FILE_PREFIX, strlen(SESSION_FILE_PREFIX)) == 0 &&
            strstr(entry->d_name, ".j")) {
            ESP_LOGI(TAG, "  Session: %s", entry->d_name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "  No sessions found");
    }
}
