#include "security/access_control.h"
#include "mimi_config.h"

#include <ctype.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "access";

#define ALLOW_FROM_MAX_LEN 256
#define WS_TOKEN_MAX_LEN   128

static char s_allow_from[ALLOW_FROM_MAX_LEN] = {0};
static char s_ws_token[WS_TOKEN_MAX_LEN] = {0};
static SemaphoreHandle_t s_lock = NULL;

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static bool token_equals(const char *a, size_t alen, const char *b)
{
    if (!b) return false;
    size_t blen = strlen(b);
    if (alen != blen) return false;
    return strncmp(a, b, alen) == 0;
}

static bool allow_from_match(const char *allow_from, const char *sender_id)
{
    if (!allow_from || allow_from[0] == '\0') {
        return true; /* 未配置 allowlist，默认放行 */
    }
    bool saw_token = false;
    const char *p = allow_from;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && isspace((unsigned char)*(end - 1))) end--;

        if (end > start) {
            saw_token = true;
            if ((end - start) == 1 && *start == '*') {
                return true;
            }
            if (sender_id && sender_id[0] != '\0' &&
                token_equals(start, (size_t)(end - start), sender_id)) {
                return true;
            }
        }
        if (*p == ',') p++;
    }

    /* 仅包含空白/分隔符时，视为未配置 allowlist（开放模式） */
    if (!saw_token) return true;
    return false;
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_SECURITY, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t nvs_erase_key_local(const char *key)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_SECURITY, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(nvs, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t access_control_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    lock();
    safe_copy(s_allow_from, sizeof(s_allow_from), MIMI_SECRET_ALLOW_FROM);
    safe_copy(s_ws_token, sizeof(s_ws_token), MIMI_SECRET_WS_TOKEN);
    unlock();

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SECURITY, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[ALLOW_FROM_MAX_LEN] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_ALLOW_FROM, tmp, &len) == ESP_OK) {
            lock();
            safe_copy(s_allow_from, sizeof(s_allow_from), tmp);
            unlock();
        }

        char token[WS_TOKEN_MAX_LEN] = {0};
        len = sizeof(token);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_WS_TOKEN, token, &len) == ESP_OK) {
            lock();
            safe_copy(s_ws_token, sizeof(s_ws_token), token);
            unlock();
        }
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Access control initialized (allow_from=%s, ws_token=%s)",
             s_allow_from[0] ? "configured" : "open",
             s_ws_token[0] ? "configured" : "open");
    return ESP_OK;
}

bool access_control_is_telegram_allowed(const char *sender_id)
{
    char allow_from[ALLOW_FROM_MAX_LEN];
    lock();
    safe_copy(allow_from, sizeof(allow_from), s_allow_from);
    unlock();
    return allow_from_match(allow_from, sender_id);
}

bool access_control_ws_token_required(void)
{
    bool required;
    lock();
    required = (s_ws_token[0] != '\0');
    unlock();
    return required;
}

bool access_control_validate_ws_token(const char *token)
{
    if (!token) return false;
    bool ok;
    lock();
    ok = (s_ws_token[0] != '\0' && strcmp(token, s_ws_token) == 0);
    unlock();
    return ok;
}

const char *access_control_get_allow_from(void)
{
    return s_allow_from;
}

const char *access_control_get_ws_token(void)
{
    return s_ws_token;
}

esp_err_t access_control_set_allow_from(const char *allow_from)
{
    if (!allow_from) allow_from = "";
    if (strlen(allow_from) >= sizeof(s_allow_from)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_write_str(MIMI_NVS_KEY_ALLOW_FROM, allow_from);
    if (err != ESP_OK) return err;

    lock();
    safe_copy(s_allow_from, sizeof(s_allow_from), allow_from);
    unlock();
    ESP_LOGI(TAG, "allow_from updated");
    return ESP_OK;
}

esp_err_t access_control_clear_allow_from(void)
{
    esp_err_t err = nvs_erase_key_local(MIMI_NVS_KEY_ALLOW_FROM);
    if (err != ESP_OK) return err;
    lock();
    s_allow_from[0] = '\0';
    unlock();
    ESP_LOGI(TAG, "allow_from cleared");
    return ESP_OK;
}

esp_err_t access_control_set_ws_token(const char *token)
{
    if (!token) token = "";
    if (strlen(token) >= sizeof(s_ws_token)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_write_str(MIMI_NVS_KEY_WS_TOKEN, token);
    if (err != ESP_OK) return err;

    lock();
    safe_copy(s_ws_token, sizeof(s_ws_token), token);
    unlock();
    ESP_LOGI(TAG, "ws token updated");
    return ESP_OK;
}

esp_err_t access_control_clear_ws_token(void)
{
    esp_err_t err = nvs_erase_key_local(MIMI_NVS_KEY_WS_TOKEN);
    if (err != ESP_OK) return err;
    lock();
    s_ws_token[0] = '\0';
    unlock();
    ESP_LOGI(TAG, "ws token cleared");
    return ESP_OK;
}
