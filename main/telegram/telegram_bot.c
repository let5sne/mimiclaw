#include "telegram_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"
#include "security/access_control.h"
#include "display/display.h"
#include "ui/config_screen.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "telegram";
static const char *TG_START_HELP =
    "你好，我是 MimiClaw。\n"
    "你可以直接发文字给我；\n"
    "语音/图片/文件消息我也会识别类型并给出处理建议。";

static char s_bot_token[128] = MIMI_SECRET_TG_TOKEN;
static int64_t s_update_offset = 0;
static int64_t s_last_saved_offset = -1;
static int64_t s_last_offset_save_us = 0;

#define TG_OFFSET_NVS_KEY            "update_offset"
#define TG_OFFSET_SAVE_INTERVAL_US   (5LL * 1000 * 1000)
#define TG_OFFSET_SAVE_STEP          10

#define TG_VISION_CACHE_SLOTS 8
#define TG_VISION_TEXT_MAX    768

typedef struct {
    char file_id[96];
    char text[TG_VISION_TEXT_MAX];
    uint32_t stamp;
} tg_vision_cache_entry_t;

static tg_vision_cache_entry_t s_vision_cache[TG_VISION_CACHE_SLOTS];
static uint32_t s_vision_cache_stamp = 0;

/* HTTP response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static void save_update_offset_if_needed(bool force)
{
    if (s_update_offset <= 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    bool should_save = force;
    if (!should_save && s_last_saved_offset >= 0) {
        if ((s_update_offset - s_last_saved_offset) >= TG_OFFSET_SAVE_STEP) {
            should_save = true;
        } else if ((now - s_last_offset_save_us) >= TG_OFFSET_SAVE_INTERVAL_US) {
            should_save = true;
        }
    } else if (!should_save) {
        should_save = true;
    }

    if (!should_save) {
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    if (nvs_set_i64(nvs, TG_OFFSET_NVS_KEY, s_update_offset) == ESP_OK) {
        if (nvs_commit(nvs) == ESP_OK) {
            s_last_saved_offset = s_update_offset;
            s_last_offset_save_us = now;
        }
    }
    nvs_close(nvs);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len >= resp->cap) {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1) {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp) return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static char *tg_api_call_via_proxy(const char *path, const char *post_data)
{
    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443,
                                          (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000);
    if (!conn) return NULL;

    /* Build HTTP request */
    char header[512];
    int hlen;
    if (post_data) {
        hlen = snprintf(header, sizeof(header),
            "POST /bot%s/%s HTTP/1.1\r\n"
            "Host: api.telegram.org\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            s_bot_token, path, (int)strlen(post_data));
    } else {
        hlen = snprintf(header, sizeof(header),
            "GET /bot%s/%s HTTP/1.1\r\n"
            "Host: api.telegram.org\r\n"
            "Connection: close\r\n\r\n",
            s_bot_token, path);
    }

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return NULL;
    }
    if (post_data && proxy_conn_write(conn, post_data, strlen(post_data)) < 0) {
        proxy_conn_close(conn);
        return NULL;
    }

    /* Read response — accumulate until connection close */
    size_t cap = 4096, len = 0;
    char *buf = calloc(1, cap);
    if (!buf) { proxy_conn_close(conn); return NULL; }

    int timeout = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000;
    while (1) {
        if (len + 1024 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        int n = proxy_conn_read(conn, buf + len, cap - len - 1, timeout);
        if (n <= 0) break;
        len += n;
    }
    buf[len] = '\0';
    proxy_conn_close(conn);

    /* Skip HTTP headers — find \r\n\r\n */
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) { free(buf); return NULL; }
    body += 4;

    /* Return just the body */
    char *result = strdup(body);
    free(buf);
    return result;
}

/* ── Direct path: esp_http_client ───────────────────────────── */

static char *tg_api_call_direct(const char *method, const char *post_data)
{
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", s_bot_token, method);

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) return NULL;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return NULL;
    }

    if (post_data) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }

    return resp.buf;
}

static char *tg_api_call(const char *method, const char *post_data)
{
    if (http_proxy_is_enabled()) {
        return tg_api_call_via_proxy(method, post_data);
    }
    return tg_api_call_direct(method, post_data);
}

static int tg_find_http_header_end(const uint8_t *buf, size_t len)
{
    if (!buf || len < 4) return -1;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (int)(i + 4);
        }
    }
    return -1;
}

static int tg_parse_http_status(const uint8_t *buf)
{
    if (!buf) return 0;
    int status = 0;
    sscanf((const char *)buf, "HTTP/%*d.%*d %d", &status);
    return status;
}

typedef enum {
    TG_STT_STAGE_NONE = 0,
    TG_STT_STAGE_GET_FILE,
    TG_STT_STAGE_DOWNLOAD,
    TG_STT_STAGE_UPLOAD,
} tg_stt_stage_t;

typedef enum {
    TG_MEDIA_KIND_NONE = 0,
    TG_MEDIA_KIND_PHOTO,
    TG_MEDIA_KIND_DOCUMENT,
} tg_media_kind_t;

static const char *tg_stt_stage_name(tg_stt_stage_t stage)
{
    switch (stage) {
        case TG_STT_STAGE_GET_FILE: return "get_file";
        case TG_STT_STAGE_DOWNLOAD: return "download";
        case TG_STT_STAGE_UPLOAD: return "stt_upload";
        default: return "unknown";
    }
}

static const char *tg_media_kind_name(tg_media_kind_t kind)
{
    switch (kind) {
        case TG_MEDIA_KIND_PHOTO: return "photo";
        case TG_MEDIA_KIND_DOCUMENT: return "document";
        default: return "unknown";
    }
}

static bool tg_vision_cache_get(const char *file_id, char *out_text, size_t out_size)
{
    if (!file_id || !file_id[0] || !out_text || out_size < 2) return false;
    out_text[0] = '\0';

    for (int i = 0; i < TG_VISION_CACHE_SLOTS; i++) {
        tg_vision_cache_entry_t *e = &s_vision_cache[i];
        if (e->file_id[0] && strcmp(e->file_id, file_id) == 0 && e->text[0]) {
            strncpy(out_text, e->text, out_size - 1);
            out_text[out_size - 1] = '\0';
            e->stamp = ++s_vision_cache_stamp;
            return true;
        }
    }
    return false;
}

static void tg_vision_cache_put(const char *file_id, const char *text)
{
    if (!file_id || !file_id[0] || !text || !text[0]) return;

    int idx = -1;
    uint32_t min_stamp = UINT32_MAX;
    for (int i = 0; i < TG_VISION_CACHE_SLOTS; i++) {
        tg_vision_cache_entry_t *e = &s_vision_cache[i];
        if (e->file_id[0] == '\0') {
            idx = i;
            break;
        }
        if (e->stamp < min_stamp) {
            min_stamp = e->stamp;
            idx = i;
        }
    }
    if (idx < 0) idx = 0;

    tg_vision_cache_entry_t *dst = &s_vision_cache[idx];
    strncpy(dst->file_id, file_id, sizeof(dst->file_id) - 1);
    dst->file_id[sizeof(dst->file_id) - 1] = '\0';
    strncpy(dst->text, text, sizeof(dst->text) - 1);
    dst->text[sizeof(dst->text) - 1] = '\0';
    dst->stamp = ++s_vision_cache_stamp;
}

static const char *tg_guess_image_format_from_path(const char *file_path)
{
    if (!file_path || !file_path[0]) return "jpeg";
    const char *dot = strrchr(file_path, '.');
    if (!dot || !dot[1]) return "jpeg";

    char ext[8] = {0};
    size_t n = strlen(dot + 1);
    if (n >= sizeof(ext)) n = sizeof(ext) - 1;
    for (size_t i = 0; i < n; i++) {
        ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    }
    ext[n] = '\0';

    if (strcmp(ext, "png") == 0) return "png";
    if (strcmp(ext, "webp") == 0) return "webp";
    if (strcmp(ext, "bmp") == 0) return "bmp";
    return "jpeg";
}

static const char *tg_guess_doc_format_from_path(const char *file_path)
{
    if (!file_path || !file_path[0]) return "bin";
    const char *dot = strrchr(file_path, '.');
    if (!dot || !dot[1]) return "bin";

    static char ext[16];
    memset(ext, 0, sizeof(ext));
    size_t n = strlen(dot + 1);
    if (n >= sizeof(ext)) n = sizeof(ext) - 1;
    for (size_t i = 0; i < n; i++) {
        ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    }
    ext[n] = '\0';
    return ext;
}

static esp_err_t tg_get_file_path(const char *file_id, char *out_path, size_t out_size)
{
    if (!file_id || !out_path || out_size < 2) return ESP_ERR_INVALID_ARG;
    out_path[0] = '\0';

    cJSON *body = cJSON_CreateObject();
    if (!body) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "file_id", file_id);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return ESP_ERR_NO_MEM;

    char *resp = tg_api_call("getFile", json);
    free(json);
    if (!resp) return ESP_FAIL;

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return ESP_FAIL;

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *path = result ? cJSON_GetObjectItem(result, "file_path") : NULL;
    if (!cJSON_IsTrue(ok) || !cJSON_IsString(path) || !path->valuestring) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(out_path, path->valuestring, out_size - 1);
    out_path[out_size - 1] = '\0';
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t tg_download_file_direct(const char *file_path, uint8_t **out_data, size_t *out_len)
{
    if (!file_path || !out_data || !out_len) return ESP_ERR_INVALID_ARG;
    *out_data = NULL;
    *out_len = 0;

    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/file/bot%s/%s", s_bot_token, file_path);

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = MIMI_TG_STT_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Download telegram file failed: err=%s status=%d path=%s",
                 esp_err_to_name(err), status, file_path);
        free(resp.buf);
        return ESP_FAIL;
    }

    if (resp.len == 0 || resp.len > MIMI_TG_MEDIA_MAX_BYTES) {
        ESP_LOGW(TAG, "Telegram media size invalid: %d bytes", (int)resp.len);
        free(resp.buf);
        return ESP_ERR_INVALID_SIZE;
    }

    *out_data = (uint8_t *)resp.buf;
    *out_len = resp.len;
    return ESP_OK;
}

static esp_err_t tg_download_file_via_proxy(const char *file_path, uint8_t **out_data, size_t *out_len)
{
    if (!file_path || !out_data || !out_len) return ESP_ERR_INVALID_ARG;
    *out_data = NULL;
    *out_len = 0;

    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443, MIMI_TG_STT_TIMEOUT_MS);
    if (!conn) return ESP_FAIL;

    char header[640];
    int hlen = snprintf(header, sizeof(header),
                        "GET /file/bot%s/%s HTTP/1.1\r\n"
                        "Host: api.telegram.org\r\n"
                        "Connection: close\r\n\r\n",
                        s_bot_token, file_path);
    if (hlen <= 0 || hlen >= (int)sizeof(header)) {
        proxy_conn_close(conn);
        return ESP_FAIL;
    }
    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_FAIL;
    }

    size_t cap = 8192;
    size_t len = 0;
    uint8_t *buf = (uint8_t *)calloc(1, cap + 1);
    if (!buf) {
        proxy_conn_close(conn);
        return ESP_ERR_NO_MEM;
    }

    while (1) {
        if (len + 2048 >= cap) {
            size_t new_cap = cap * 2;
            if (new_cap > MIMI_TG_MEDIA_MAX_BYTES + 16384) {
                new_cap = MIMI_TG_MEDIA_MAX_BYTES + 16384;
            }
            if (new_cap <= cap) break;
            uint8_t *tmp = (uint8_t *)realloc(buf, new_cap + 1);
            if (!tmp) break;
            buf = tmp;
            cap = new_cap;
        }

        int n = proxy_conn_read(conn, (char *)buf + len, cap - len, MIMI_TG_STT_TIMEOUT_MS);
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = '\0'; /* only for status/header parse convenience */
    }
    proxy_conn_close(conn);

    int status = tg_parse_http_status(buf);
    int header_end = tg_find_http_header_end(buf, len);
    if (status != 200 || header_end < 0 || (size_t)header_end >= len) {
        ESP_LOGW(TAG, "Proxy download telegram file failed: status=%d path=%s", status, file_path);
        free(buf);
        return ESP_FAIL;
    }

    size_t body_len = len - (size_t)header_end;
    if (body_len == 0 || body_len > MIMI_TG_MEDIA_MAX_BYTES) {
        ESP_LOGW(TAG, "Proxy telegram media size invalid: %d bytes", (int)body_len);
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *body = (uint8_t *)malloc(body_len);
    if (!body) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(body, buf + header_end, body_len);
    free(buf);

    *out_data = body;
    *out_len = body_len;
    return ESP_OK;
}

static esp_err_t tg_download_file(const char *file_path, uint8_t **out_data, size_t *out_len)
{
    bool proxy_enabled = http_proxy_is_enabled();
    ESP_LOGI(TAG, "Telegram file download path=%s proxy_enabled=%d file=%s",
             proxy_enabled ? "proxy" : "direct", proxy_enabled ? 1 : 0,
             file_path ? file_path : "(null)");

    if (proxy_enabled) {
        return tg_download_file_via_proxy(file_path, out_data, out_len);
    }
    return tg_download_file_direct(file_path, out_data, out_len);
}

static esp_err_t tg_build_gateway_http_url(const char *endpoint_path, char *out_url, size_t out_size)
{
    if (!endpoint_path || !endpoint_path[0] || !out_url || out_size < 16) return ESP_ERR_INVALID_ARG;
    out_url[0] = '\0';

    char gw[160] = {0};
    strncpy(gw, MIMI_VOICE_GATEWAY_URL, sizeof(gw) - 1);

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_VOICE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(gw);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_VOICE_GW, gw, &len) != ESP_OK || gw[0] == '\0') {
            strncpy(gw, MIMI_VOICE_GATEWAY_URL, sizeof(gw) - 1);
            gw[sizeof(gw) - 1] = '\0';
        }
        nvs_close(nvs);
    }

    bool secure = false;
    const char *p = gw;
    if (strncmp(gw, "wss://", 6) == 0) {
        secure = true;
        p = gw + 6;
    } else if (strncmp(gw, "ws://", 5) == 0) {
        p = gw + 5;
    }

    const char *slash = strchr(p, '/');
    size_t hostport_len = slash ? (size_t)(slash - p) : strlen(p);
    if (hostport_len == 0 || hostport_len >= 96) return ESP_FAIL;

    char hostport[96];
    memcpy(hostport, p, hostport_len);
    hostport[hostport_len] = '\0';

    char host[80];
    int port = secure ? 443 : 80;
    const char *colon = strrchr(hostport, ':');
    if (colon && colon[1] != '\0' && isdigit((unsigned char)colon[1])) {
        size_t host_len = (size_t)(colon - hostport);
        if (host_len == 0 || host_len >= sizeof(host)) return ESP_FAIL;
        memcpy(host, hostport, host_len);
        host[host_len] = '\0';
        port = atoi(colon + 1);
    } else {
        strncpy(host, hostport, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }

    int http_port = (port > 0) ? (port + 1) : 8091;
    snprintf(out_url, out_size, "%s://%s:%d/%s",
             secure ? "https" : "http", host, http_port, endpoint_path);
    return ESP_OK;
}

static esp_err_t tg_stt_upload(const uint8_t *audio, size_t audio_len, const char *audio_format,
                               char *out_text, size_t out_size)
{
    if (!audio || audio_len == 0 || !out_text || out_size < 2) return ESP_ERR_INVALID_ARG;
    out_text[0] = '\0';

    char stt_url[192];
    esp_err_t err = tg_build_gateway_http_url("stt_upload", stt_url, sizeof(stt_url));
    if (err != ESP_OK) return err;

    http_resp_t resp = {
        .buf = calloc(1, 2048),
        .len = 0,
        .cap = 2048,
    };
    if (!resp.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = stt_url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = MIMI_TG_STT_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Audio-Format",
                               (audio_format && audio_format[0]) ? audio_format : "ogg");
    esp_http_client_set_post_field(client, (const char *)audio, audio_len);

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "STT upload failed: err=%s status=%d url=%s",
                 esp_err_to_name(err), status, stt_url);
        free(resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) return ESP_FAIL;

    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strncpy(out_text, text->valuestring, out_size - 1);
    out_text[out_size - 1] = '\0';
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t tg_vision_upload(const uint8_t *image, size_t image_len, const char *image_format,
                                  char *out_text, size_t out_size)
{
    if (!image || image_len == 0 || !out_text || out_size < 2) return ESP_ERR_INVALID_ARG;
    out_text[0] = '\0';

    char vision_url[192];
    esp_err_t err = tg_build_gateway_http_url("vision_upload", vision_url, sizeof(vision_url));
    if (err != ESP_OK) return err;

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = vision_url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = MIMI_TG_VISION_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Image-Format",
                               (image_format && image_format[0]) ? image_format : "jpeg");
    esp_http_client_set_post_field(client, (const char *)image, image_len);

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Vision upload failed: err=%s status=%d url=%s",
                 esp_err_to_name(err), status, vision_url);
        free(resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) return ESP_FAIL;

    char merged[TG_VISION_TEXT_MAX] = {0};
    size_t off = 0;
    cJSON *caption = cJSON_GetObjectItem(root, "caption");
    if (cJSON_IsString(caption) && caption->valuestring && caption->valuestring[0]) {
        int n = snprintf(merged + off, sizeof(merged) - off, "描述：%s", caption->valuestring);
        if (n > 0) {
            off += (size_t)n;
            if (off >= sizeof(merged)) off = sizeof(merged) - 1;
        }
    }

    cJSON *ocr_text = cJSON_GetObjectItem(root, "ocr_text");
    if (cJSON_IsString(ocr_text) && ocr_text->valuestring && ocr_text->valuestring[0]) {
        int n = snprintf(merged + off, sizeof(merged) - off, "%s文字：%s",
                         off ? "\n" : "", ocr_text->valuestring);
        if (n > 0) {
            off += (size_t)n;
            if (off >= sizeof(merged)) off = sizeof(merged) - 1;
        }
    }

    cJSON *objects = cJSON_GetObjectItem(root, "objects");
    if (objects && cJSON_IsArray(objects) && cJSON_GetArraySize(objects) > 0) {
        char items[256] = {0};
        size_t item_off = 0;
        cJSON *it = NULL;
        int count = 0;
        cJSON_ArrayForEach(it, objects) {
            if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0]) continue;
            if (count >= 12) break;
            int n = snprintf(items + item_off, sizeof(items) - item_off, "%s%s",
                             count ? "、" : "", it->valuestring);
            if (n <= 0) break;
            item_off += (size_t)n;
            if (item_off >= sizeof(items)) {
                item_off = sizeof(items) - 1;
                break;
            }
            count++;
        }
        if (count > 0) {
            int n = snprintf(merged + off, sizeof(merged) - off, "%s元素：%s",
                             off ? "\n" : "", items);
            if (n > 0) {
                off += (size_t)n;
                if (off >= sizeof(merged)) off = sizeof(merged) - 1;
            }
        }
    }

    if (off == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        strncpy(merged, text->valuestring, sizeof(merged) - 1);
        merged[sizeof(merged) - 1] = '\0';
    }

    strncpy(out_text, merged, out_size - 1);
    out_text[out_size - 1] = '\0';
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t tg_doc_upload(const uint8_t *doc_data, size_t doc_len,
                               const char *doc_name, const char *doc_mime,
                               const char *doc_path,
                               char *out_text, size_t out_size,
                               char *out_meta, size_t meta_size)
{
    if (!doc_data || doc_len == 0 || !out_text || out_size < 2) return ESP_ERR_INVALID_ARG;
    out_text[0] = '\0';
    if (out_meta && meta_size > 0) out_meta[0] = '\0';

    char doc_url[192];
    esp_err_t err = tg_build_gateway_http_url("doc_upload", doc_url, sizeof(doc_url));
    if (err != ESP_OK) return err;

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = doc_url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = MIMI_TG_DOC_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    if (doc_name && doc_name[0]) esp_http_client_set_header(client, "X-Doc-Name", doc_name);
    if (doc_mime && doc_mime[0]) esp_http_client_set_header(client, "X-Doc-Mime", doc_mime);
    if (doc_path && doc_path[0]) {
        esp_http_client_set_header(client, "X-Doc-Path", doc_path);
        esp_http_client_set_header(client, "X-Doc-Format", tg_guess_doc_format_from_path(doc_path));
    }
    esp_http_client_set_post_field(client, (const char *)doc_data, doc_len);

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Doc upload failed: err=%s status=%d url=%s",
                 esp_err_to_name(err), status, doc_url);
        free(resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) return ESP_FAIL;

    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strncpy(out_text, text->valuestring, out_size - 1);
    out_text[out_size - 1] = '\0';

    if (out_meta && meta_size > 0) {
        const char *fmt = "";
        const char *parser = "";
        int text_len = 0;
        bool truncated = false;
        bool from_vision = false;
        cJSON *fmt_item = cJSON_GetObjectItem(root, "doc_format");
        cJSON *parser_item = cJSON_GetObjectItem(root, "parser");
        cJSON *len_item = cJSON_GetObjectItem(root, "text_len");
        cJSON *trunc_item = cJSON_GetObjectItem(root, "truncated");
        cJSON *vision_item = cJSON_GetObjectItem(root, "from_vision");
        if (cJSON_IsString(fmt_item)) fmt = fmt_item->valuestring;
        if (cJSON_IsString(parser_item)) parser = parser_item->valuestring;
        if (cJSON_IsNumber(len_item)) text_len = len_item->valueint;
        if (cJSON_IsBool(trunc_item)) truncated = cJSON_IsTrue(trunc_item);
        if (cJSON_IsBool(vision_item)) from_vision = cJSON_IsTrue(vision_item);
        snprintf(out_meta, meta_size,
                 "{\"doc_parse\":\"ok\",\"format\":\"%.16s\",\"parser\":\"%.24s\","
                 "\"text_len\":%d,\"truncated\":%s,\"from_vision\":%s}",
                 fmt, parser, text_len, truncated ? "true" : "false",
                 from_vision ? "true" : "false");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static bool tg_extract_voice_file(cJSON *message, const char **out_file_id, const char **out_format)
{
    if (!message || !out_file_id || !out_format) return false;

    cJSON *voice = cJSON_GetObjectItem(message, "voice");
    if (voice && cJSON_IsObject(voice)) {
        cJSON *fid = cJSON_GetObjectItem(voice, "file_id");
        if (cJSON_IsString(fid) && fid->valuestring && fid->valuestring[0]) {
            *out_file_id = fid->valuestring;
            *out_format = "ogg";
            return true;
        }
    }

    cJSON *audio = cJSON_GetObjectItem(message, "audio");
    if (audio && cJSON_IsObject(audio)) {
        cJSON *fid = cJSON_GetObjectItem(audio, "file_id");
        if (cJSON_IsString(fid) && fid->valuestring && fid->valuestring[0]) {
            const char *fmt = "mp3";
            cJSON *mime = cJSON_GetObjectItem(audio, "mime_type");
            if (cJSON_IsString(mime) && mime->valuestring) {
                if (strstr(mime->valuestring, "ogg")) fmt = "ogg";
                else if (strstr(mime->valuestring, "wav")) fmt = "wav";
                else if (strstr(mime->valuestring, "mp4")) fmt = "mp4";
            }
            *out_file_id = fid->valuestring;
            *out_format = fmt;
            return true;
        }
    }
    return false;
}

static bool tg_extract_downloadable_media(cJSON *message, tg_media_kind_t *out_kind,
                                          const char **out_file_id)
{
    if (!message || !out_kind || !out_file_id) return false;
    *out_kind = TG_MEDIA_KIND_NONE;
    *out_file_id = NULL;

    cJSON *photo = cJSON_GetObjectItem(message, "photo");
    if (photo && cJSON_IsArray(photo)) {
        int count = cJSON_GetArraySize(photo);
        if (count > 0) {
            cJSON *last = cJSON_GetArrayItem(photo, count - 1);
            cJSON *fid = last ? cJSON_GetObjectItem(last, "file_id") : NULL;
            if (cJSON_IsString(fid) && fid->valuestring && fid->valuestring[0]) {
                *out_kind = TG_MEDIA_KIND_PHOTO;
                *out_file_id = fid->valuestring;
                return true;
            }
        }
    }

    cJSON *document = cJSON_GetObjectItem(message, "document");
    if (document && cJSON_IsObject(document)) {
        cJSON *fid = cJSON_GetObjectItem(document, "file_id");
        if (cJSON_IsString(fid) && fid->valuestring && fid->valuestring[0]) {
            *out_kind = TG_MEDIA_KIND_DOCUMENT;
            *out_file_id = fid->valuestring;
            return true;
        }
    }

    return false;
}

static void tg_extract_document_info(cJSON *message, const char **out_name, const char **out_mime)
{
    if (!out_name || !out_mime) return;
    *out_name = "";
    *out_mime = "";
    if (!message) return;

    cJSON *document = cJSON_GetObjectItem(message, "document");
    if (!document || !cJSON_IsObject(document)) return;

    cJSON *name_item = cJSON_GetObjectItem(document, "file_name");
    cJSON *mime_item = cJSON_GetObjectItem(document, "mime_type");
    if (cJSON_IsString(name_item) && name_item->valuestring) {
        *out_name = name_item->valuestring;
    }
    if (cJSON_IsString(mime_item) && mime_item->valuestring) {
        *out_mime = mime_item->valuestring;
    }
}

static bool tg_extract_sender_id(cJSON *message, char *sender_id, size_t size)
{
    if (!message || !sender_id || size < 2) return false;
    sender_id[0] = '\0';

    cJSON *from = cJSON_GetObjectItem(message, "from");
    if (!from) return false;

    cJSON *from_id = cJSON_GetObjectItem(from, "id");
    if (cJSON_IsNumber(from_id)) {
        snprintf(sender_id, size, "%.0f", from_id->valuedouble);
        return true;
    }
    if (cJSON_IsString(from_id)) {
        strncpy(sender_id, from_id->valuestring, size - 1);
        sender_id[size - 1] = '\0';
        return true;
    }
    return false;
}

static bool tg_extract_chat_id(cJSON *message, char *chat_id, size_t size)
{
    if (!message || !chat_id || size < 2) return false;
    chat_id[0] = '\0';

    cJSON *chat = cJSON_GetObjectItem(message, "chat");
    if (!chat) return false;
    cJSON *chat_id_item = cJSON_GetObjectItem(chat, "id");
    if (!chat_id_item) return false;

    if (cJSON_IsNumber(chat_id_item)) {
        snprintf(chat_id, size, "%.0f", chat_id_item->valuedouble);
        return true;
    }
    if (cJSON_IsString(chat_id_item)) {
        strncpy(chat_id, chat_id_item->valuestring, size - 1);
        chat_id[size - 1] = '\0';
        return true;
    }
    return false;
}

static const char *tg_get_caption(cJSON *message)
{
    cJSON *caption = cJSON_GetObjectItem(message, "caption");
    if (cJSON_IsString(caption) && caption->valuestring && caption->valuestring[0]) {
        return caption->valuestring;
    }
    return NULL;
}

static bool tg_build_media_summary(cJSON *message, char *out, size_t out_size)
{
    if (!message || !out || out_size < 2) return false;
    out[0] = '\0';

    const char *caption = tg_get_caption(message);
    cJSON *voice = cJSON_GetObjectItem(message, "voice");
    if (voice && cJSON_IsObject(voice)) {
        int duration = 0;
        const char *file_id = "";
        cJSON *duration_item = cJSON_GetObjectItem(voice, "duration");
        cJSON *file_id_item = cJSON_GetObjectItem(voice, "file_id");
        if (cJSON_IsNumber(duration_item)) duration = duration_item->valueint;
        if (cJSON_IsString(file_id_item)) file_id = file_id_item->valuestring;

        snprintf(out, out_size,
                 "[Telegram语音消息]\n"
                 "时长: %d 秒\n"
                 "file_id: %.96s\n"
                 "%s%s",
                 duration, file_id,
                 caption ? "caption: " : "",
                 caption ? caption : "");
        return true;
    }

    cJSON *audio = cJSON_GetObjectItem(message, "audio");
    if (audio && cJSON_IsObject(audio)) {
        int duration = 0;
        const char *title = "";
        const char *performer = "";
        cJSON *duration_item = cJSON_GetObjectItem(audio, "duration");
        cJSON *title_item = cJSON_GetObjectItem(audio, "title");
        cJSON *performer_item = cJSON_GetObjectItem(audio, "performer");
        if (cJSON_IsNumber(duration_item)) duration = duration_item->valueint;
        if (cJSON_IsString(title_item)) title = title_item->valuestring;
        if (cJSON_IsString(performer_item)) performer = performer_item->valuestring;

        snprintf(out, out_size,
                 "[Telegram音频消息]\n"
                 "时长: %d 秒\n"
                 "标题: %.64s\n"
                 "作者: %.64s\n"
                 "%s%s",
                 duration, title, performer,
                 caption ? "caption: " : "",
                 caption ? caption : "");
        return true;
    }

    cJSON *photo = cJSON_GetObjectItem(message, "photo");
    if (photo && cJSON_IsArray(photo)) {
        int count = cJSON_GetArraySize(photo);
        int width = 0, height = 0;
        const char *file_id = "";
        if (count > 0) {
            cJSON *last = cJSON_GetArrayItem(photo, count - 1);
            if (last && cJSON_IsObject(last)) {
                cJSON *w = cJSON_GetObjectItem(last, "width");
                cJSON *h = cJSON_GetObjectItem(last, "height");
                cJSON *fid = cJSON_GetObjectItem(last, "file_id");
                if (cJSON_IsNumber(w)) width = w->valueint;
                if (cJSON_IsNumber(h)) height = h->valueint;
                if (cJSON_IsString(fid)) file_id = fid->valuestring;
            }
        }

        snprintf(out, out_size,
                 "[Telegram图片消息]\n"
                 "尺寸: %dx%d\n"
                 "file_id: %.96s\n"
                 "%s%s",
                 width, height,
                 file_id,
                 caption ? "caption: " : "",
                 caption ? caption : "");
        return true;
    }

    cJSON *document = cJSON_GetObjectItem(message, "document");
    if (document && cJSON_IsObject(document)) {
        const char *file_name = "";
        const char *mime_type = "";
        const char *file_id = "";
        int file_size = 0;
        cJSON *name_item = cJSON_GetObjectItem(document, "file_name");
        cJSON *mime_item = cJSON_GetObjectItem(document, "mime_type");
        cJSON *size_item = cJSON_GetObjectItem(document, "file_size");
        cJSON *fid_item = cJSON_GetObjectItem(document, "file_id");
        if (cJSON_IsString(name_item)) file_name = name_item->valuestring;
        if (cJSON_IsString(mime_item)) mime_type = mime_item->valuestring;
        if (cJSON_IsNumber(size_item)) file_size = size_item->valueint;
        if (cJSON_IsString(fid_item)) file_id = fid_item->valuestring;

        snprintf(out, out_size,
                 "[Telegram文件消息]\n"
                 "文件名: %.96s\n"
                 "MIME: %.64s\n"
                 "大小: %d 字节\n"
                 "file_id: %.96s\n"
                 "%s%s",
                 file_name, mime_type, file_size, file_id,
                 caption ? "caption: " : "",
                 caption ? caption : "");
        return true;
    }

    return false;
}

static void tg_push_inbound(const char *chat_id, const char *content,
                            const char *media_type, const char *file_id,
                            const char *file_path, const char *meta_json)
{
    if (!chat_id || !content || !content[0]) return;

    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_TELEGRAM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    if (media_type && media_type[0]) {
        strncpy(msg.media_type, media_type, sizeof(msg.media_type) - 1);
    } else {
        strncpy(msg.media_type, "text", sizeof(msg.media_type) - 1);
    }
    if (file_id && file_id[0]) {
        strncpy(msg.file_id, file_id, sizeof(msg.file_id) - 1);
    }
    if (file_path && file_path[0]) {
        strncpy(msg.file_path, file_path, sizeof(msg.file_path) - 1);
    }
    msg.content = strdup(content);
    if (!msg.content) {
        return;
    }
    if (meta_json && meta_json[0]) {
        msg.meta_json = strdup(meta_json);
        if (!msg.meta_json) {
            message_bus_msg_free(&msg);
            return;
        }
    }

    if (message_bus_push_inbound(&msg) != ESP_OK) {
        message_bus_msg_free(&msg);
    }
}

static bool tg_build_downloaded_media_summary(cJSON *message, tg_media_kind_t kind,
                                              const char *file_path, size_t media_len,
                                              char *out, size_t out_size)
{
    if (!message || !file_path || !out || out_size < 2) return false;
    out[0] = '\0';
    const char *caption = tg_get_caption(message);

    if (kind == TG_MEDIA_KIND_PHOTO) {
        int width = 0;
        int height = 0;
        cJSON *photo = cJSON_GetObjectItem(message, "photo");
        if (photo && cJSON_IsArray(photo)) {
            int count = cJSON_GetArraySize(photo);
            if (count > 0) {
                cJSON *last = cJSON_GetArrayItem(photo, count - 1);
                cJSON *w = last ? cJSON_GetObjectItem(last, "width") : NULL;
                cJSON *h = last ? cJSON_GetObjectItem(last, "height") : NULL;
                if (cJSON_IsNumber(w)) width = w->valueint;
                if (cJSON_IsNumber(h)) height = h->valueint;
            }
        }
        snprintf(out, out_size,
                 "[Telegram图片消息]\n"
                 "尺寸: %dx%d\n"
                 "下载: 成功 %u 字节\n"
                 "file_path: %.120s\n"
                 "%s%s",
                 width, height, (unsigned int)media_len, file_path,
                 caption ? "caption: " : "",
                 caption ? caption : "");
        return true;
    }

    if (kind == TG_MEDIA_KIND_DOCUMENT) {
        const char *file_name = "";
        const char *mime_type = "";
        cJSON *document = cJSON_GetObjectItem(message, "document");
        if (document && cJSON_IsObject(document)) {
            cJSON *name_item = cJSON_GetObjectItem(document, "file_name");
            cJSON *mime_item = cJSON_GetObjectItem(document, "mime_type");
            if (cJSON_IsString(name_item)) file_name = name_item->valuestring;
            if (cJSON_IsString(mime_item)) mime_type = mime_item->valuestring;
        }
        snprintf(out, out_size,
                 "[Telegram文件消息]\n"
                 "文件名: %.96s\n"
                 "MIME: %.64s\n"
                 "下载: 成功 %u 字节\n"
                 "file_path: %.120s\n"
                 "%s%s",
                 file_name, mime_type, (unsigned int)media_len, file_path,
                 caption ? "caption: " : "",
                 caption ? caption : "");
        return true;
    }

    return false;
}

static void process_updates(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *update;
    cJSON_ArrayForEach(update, result) {
        /* Track offset and skip stale/duplicate updates */
        cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
        int64_t uid = -1;
        if (cJSON_IsNumber(update_id)) {
            uid = (int64_t)update_id->valuedouble;
        }
        if (uid >= 0) {
            if (uid < s_update_offset) {
                continue;
            }
            s_update_offset = uid + 1;
            save_update_offset_if_needed(false);
        }

        /* Extract message */
        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!message) continue;

        char sender_id[32] = {0};
        tg_extract_sender_id(message, sender_id, sizeof(sender_id));

        if (!access_control_is_telegram_allowed(sender_id)) {
            ESP_LOGW(TAG, "Blocked telegram message from sender_id=%s",
                     sender_id[0] ? sender_id : "(unknown)");
            continue;
        }

        char chat_id_str[32];
        if (!tg_extract_chat_id(message, chat_id_str, sizeof(chat_id_str))) {
            continue;
        }

        cJSON *text = cJSON_GetObjectItem(message, "text");
        if (text && cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
            ESP_LOGI(TAG, "Text message from chat %s: %.40s...", chat_id_str, text->valuestring);

            if (strcmp(text->valuestring, "/start") == 0) {
                telegram_send_message(chat_id_str, TG_START_HELP);
                continue;
            }

            tg_push_inbound(chat_id_str, text->valuestring, "text", NULL, NULL, NULL);
            continue;
        }

        const char *voice_file_id = NULL;
        const char *voice_format = NULL;
        if (tg_extract_voice_file(message, &voice_file_id, &voice_format)) {
            char file_path[256];
            uint8_t *audio_data = NULL;
            size_t audio_len = 0;
            char stt_text[768];
            tg_stt_stage_t fail_stage = TG_STT_STAGE_NONE;

            esp_err_t stt_err = tg_get_file_path(voice_file_id, file_path, sizeof(file_path));
            if (stt_err != ESP_OK) {
                fail_stage = TG_STT_STAGE_GET_FILE;
            }
            if (stt_err == ESP_OK) {
                stt_err = tg_download_file(file_path, &audio_data, &audio_len);
                if (stt_err != ESP_OK) {
                    fail_stage = TG_STT_STAGE_DOWNLOAD;
                }
            }
            if (stt_err == ESP_OK) {
                stt_err = tg_stt_upload(audio_data, audio_len, voice_format, stt_text, sizeof(stt_text));
                if (stt_err != ESP_OK) {
                    fail_stage = TG_STT_STAGE_UPLOAD;
                }
            }
            free(audio_data);

            if (stt_err == ESP_OK && stt_text[0]) {
                char inbound_text[1024];
                const char *caption = tg_get_caption(message);
                snprintf(inbound_text, sizeof(inbound_text),
                         "[Telegram语音转写]\n%s\n%s%s",
                         stt_text,
                         caption ? "caption: " : "",
                         caption ? caption : "");
                char voice_meta[128];
                snprintf(voice_meta, sizeof(voice_meta), "{\"format\":\"%s\",\"stt\":\"ok\"}",
                         voice_format ? voice_format : "ogg");
                ESP_LOGI(TAG, "Voice STT success chat %s: %.80s", chat_id_str, stt_text);
                tg_push_inbound(chat_id_str, inbound_text, "voice",
                                voice_file_id, file_path, voice_meta);
                continue;
            }

            ESP_LOGW(TAG, "Voice STT failed stage=%s err=%s chat=%s file_id=%.48s; fallback to media summary",
                     tg_stt_stage_name(fail_stage), esp_err_to_name(stt_err), chat_id_str,
                     voice_file_id ? voice_file_id : "");
            if (fail_stage == TG_STT_STAGE_DOWNLOAD && !http_proxy_is_enabled()) {
                ESP_LOGW(TAG, "Voice STT download failed and proxy is disabled. CLI: set_proxy <host> <port>");
            } else if (fail_stage == TG_STT_STAGE_UPLOAD) {
                ESP_LOGW(TAG, "Voice STT upload failed. Check voice gateway and /stt_upload endpoint.");
            } else if (fail_stage == TG_STT_STAGE_GET_FILE) {
                ESP_LOGW(TAG, "Voice STT getFile failed. Check Telegram token/network availability.");
            }
        }

        tg_media_kind_t media_kind = TG_MEDIA_KIND_NONE;
        const char *media_file_id = NULL;
        if (tg_extract_downloadable_media(message, &media_kind, &media_file_id)) {
            char file_path[256];
            uint8_t *media_data = NULL;
            size_t media_len = 0;

            esp_err_t media_err = tg_get_file_path(media_file_id, file_path, sizeof(file_path));
            if (media_err == ESP_OK) {
                media_err = tg_download_file(file_path, &media_data, &media_len);
            }

            if (media_err == ESP_OK && media_len > 0) {
                char inbound_text[1024];
                bool handled = false;
                ESP_LOGI(TAG, "Telegram %s download success chat %s: %u bytes",
                         tg_media_kind_name(media_kind), chat_id_str, (unsigned int)media_len);

                if (media_kind == TG_MEDIA_KIND_PHOTO) {
                    char vision_text[768];
                    bool cache_hit = false;
                    esp_err_t vision_err = ESP_FAIL;

                    if (media_file_id && media_file_id[0]) {
                        cache_hit = tg_vision_cache_get(media_file_id, vision_text, sizeof(vision_text));
                    }
                    if (!cache_hit) {
                        const char *img_fmt = tg_guess_image_format_from_path(file_path);
                        vision_err = tg_vision_upload(media_data, media_len, img_fmt,
                                                      vision_text, sizeof(vision_text));
                        if (vision_err == ESP_OK && vision_text[0] && media_file_id && media_file_id[0]) {
                            tg_vision_cache_put(media_file_id, vision_text);
                        }
                    } else {
                        vision_err = ESP_OK;
                        ESP_LOGI(TAG, "Telegram photo vision cache hit chat %s file_id=%.32s",
                                 chat_id_str, media_file_id);
                    }

                    if (vision_err == ESP_OK && vision_text[0]) {
                        const char *caption = tg_get_caption(message);
                        snprintf(inbound_text, sizeof(inbound_text),
                                 "[Telegram图片解析]\n%s\n\n[下载信息]\n大小: %u 字节\nfile_path: %.120s\n%s%s",
                                 vision_text, (unsigned int)media_len, file_path,
                                 caption ? "caption: " : "",
                                 caption ? caption : "");
                        char photo_meta[160];
                        snprintf(photo_meta, sizeof(photo_meta),
                                 "{\"vision\":\"ok\",\"bytes\":%u,\"cache_hit\":%s}",
                                 (unsigned int)media_len, cache_hit ? "true" : "false");
                        tg_push_inbound(chat_id_str, inbound_text, "photo",
                                        media_file_id, file_path, photo_meta);
                        ESP_LOGI(TAG, "Telegram photo vision success chat %s: %.80s",
                                 chat_id_str, vision_text);
                        handled = true;
                    } else {
                        ESP_LOGW(TAG, "Telegram photo vision failed err=%s chat=%s path=%s",
                                 esp_err_to_name(vision_err), chat_id_str, file_path);
                    }
                }

                if (media_kind == TG_MEDIA_KIND_DOCUMENT) {
                    char doc_text[900];
                    char doc_meta[192];
                    const char *doc_name = "";
                    const char *doc_mime = "";
                    tg_extract_document_info(message, &doc_name, &doc_mime);

                    esp_err_t doc_err = tg_doc_upload(media_data, media_len,
                                                      doc_name, doc_mime, file_path,
                                                      doc_text, sizeof(doc_text),
                                                      doc_meta, sizeof(doc_meta));
                    if (doc_err == ESP_OK && doc_text[0]) {
                        const char *caption = tg_get_caption(message);
                        snprintf(inbound_text, sizeof(inbound_text),
                                 "[Telegram文件解析]\n%.760s\n\n[下载信息]\n大小: %u 字节\nfile_path: %.96s\n%s%.120s",
                                 doc_text, (unsigned int)media_len, file_path,
                                 caption ? "caption: " : "",
                                 caption ? caption : "");
                        tg_push_inbound(chat_id_str, inbound_text, "document",
                                        media_file_id, file_path, doc_meta);
                        ESP_LOGI(TAG, "Telegram document parse success chat %s: %.80s",
                                 chat_id_str, doc_text);
                        handled = true;
                    } else {
                        ESP_LOGW(TAG, "Telegram document parse failed err=%s chat=%s path=%s",
                                 esp_err_to_name(doc_err), chat_id_str, file_path);
                    }
                }

                if (!handled) {
                    if (!tg_build_downloaded_media_summary(message, media_kind, file_path, media_len,
                                                           inbound_text, sizeof(inbound_text))) {
                        snprintf(inbound_text, sizeof(inbound_text),
                                 "[Telegram媒体消息]\n类型: %s\n下载: 成功 %u 字节\nfile_path: %.120s",
                                 tg_media_kind_name(media_kind), (unsigned int)media_len, file_path);
                    }
                    char media_meta[96];
                    snprintf(media_meta, sizeof(media_meta), "{\"download_bytes\":%u}",
                             (unsigned int)media_len);
                    tg_push_inbound(chat_id_str, inbound_text, tg_media_kind_name(media_kind),
                                    media_file_id, file_path, media_meta);
                }
                free(media_data);
                continue;
            }
            free(media_data);
            ESP_LOGW(TAG, "Telegram %s download failed err=%s chat=%s file_id=%.48s; fallback to media summary",
                     tg_media_kind_name(media_kind), esp_err_to_name(media_err), chat_id_str,
                     media_file_id ? media_file_id : "");
        }

        char media_summary[1024];
        if (tg_build_media_summary(message, media_summary, sizeof(media_summary))) {
            ESP_LOGI(TAG, "Media message from chat %s: %.80s", chat_id_str, media_summary);
            tg_push_inbound(chat_id_str, media_summary, "media", NULL, NULL, NULL);
            continue;
        }

        ESP_LOGD(TAG, "Unsupported telegram message ignored for chat %s", chat_id_str);
    }

    cJSON_Delete(root);
}

static void telegram_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Telegram polling task started");

    while (1) {
        if (s_bot_token[0] == '\0') {
            ESP_LOGW(TAG, "No bot token configured, waiting...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        char params[128];
        snprintf(params, sizeof(params),
                 "getUpdates?offset=%" PRId64 "&timeout=%d",
                 s_update_offset, MIMI_TG_POLL_TIMEOUT_S);

        char *resp = tg_api_call(params, NULL);
        if (resp) {
            process_updates(resp);
            free(resp);
        } else {
            /* Back off on error */
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

/* --- Public API --- */

esp_err_t telegram_bot_init(void)
{
    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TG, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TG_TOKEN, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_bot_token, tmp, sizeof(s_bot_token) - 1);
        }

        int64_t offset = 0;
        if (nvs_get_i64(nvs, TG_OFFSET_NVS_KEY, &offset) == ESP_OK && offset > 0) {
            s_update_offset = offset;
            s_last_saved_offset = offset;
            ESP_LOGI(TAG, "Loaded Telegram update offset: %" PRId64, s_update_offset);
        }
        nvs_close(nvs);
    }

    /* s_bot_token is already initialized from MIMI_SECRET_TG_TOKEN as fallback */

    if (s_bot_token[0]) {
        ESP_LOGI(TAG, "Telegram bot token loaded (len=%d)", (int)strlen(s_bot_token));
    } else {
        ESP_LOGI(TAG, "No Telegram bot token configured. Use CLI: set_tg_token <TOKEN>");
    }
    return ESP_OK;
}

esp_err_t telegram_bot_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        telegram_poll_task, "tg_poll",
        MIMI_TG_POLL_STACK, NULL,
        MIMI_TG_POLL_PRIO, NULL, MIMI_TG_POLL_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

static int tg_response_is_ok(const char *resp_json, const char **out_desc)
{
    static char s_desc_buf[160];
    s_desc_buf[0] = '\0';
    if (out_desc) {
        *out_desc = NULL;
    }
    if (!resp_json) {
        return 0;
    }

    cJSON *root = cJSON_Parse(resp_json);
    if (!root) {
        return 0;
    }

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    cJSON *desc = cJSON_GetObjectItem(root, "description");
    int ret = cJSON_IsTrue(ok) ? 1 : 0;
    if (out_desc && cJSON_IsString(desc) && desc->valuestring) {
        strncpy(s_desc_buf, desc->valuestring, sizeof(s_desc_buf) - 1);
        s_desc_buf[sizeof(s_desc_buf) - 1] = '\0';
        *out_desc = s_desc_buf;
    }
    cJSON_Delete(root);
    return ret;
}

esp_err_t telegram_send_message(const char *chat_id, const char *text)
{
    if (s_bot_token[0] == '\0') {
        ESP_LOGW(TAG, "Cannot send: no bot token");
        return ESP_ERR_INVALID_STATE;
    }

    /* Split long messages at 4096-char boundary */
    size_t text_len = strlen(text);
    size_t offset = 0;
    int all_ok = 1;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_TG_MAX_MSG_LEN) {
            chunk = MIMI_TG_MAX_MSG_LEN;
        }

        /* Build JSON body */
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "chat_id", chat_id);

        /* Create null-terminated chunk */
        char *segment = malloc(chunk + 1);
        if (!segment) {
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        cJSON_AddStringToObject(body, "text", segment);
        cJSON_AddStringToObject(body, "parse_mode", "Markdown");

        char *json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        free(segment);

        if (!json_str) {
            all_ok = 0;
            offset += chunk;
            continue;
        }

        ESP_LOGI(TAG, "Sending telegram chunk to %s (%d bytes)", chat_id, (int)chunk);
        char *resp = tg_api_call("sendMessage", json_str);
        free(json_str);

        int sent_ok = 0;
        bool markdown_failed = false;
        if (resp) {
            const char *desc = NULL;
            sent_ok = tg_response_is_ok(resp, &desc);
            if (!sent_ok) {
                markdown_failed = true;
                ESP_LOGI(TAG, "Markdown rejected by Telegram for %s: %s",
                         chat_id, desc ? desc : "unknown");
            }
        }

        if (!sent_ok) {
            /* Retry without parse_mode */
            cJSON *body2 = cJSON_CreateObject();
            cJSON_AddStringToObject(body2, "chat_id", chat_id);
            char *seg2 = malloc(chunk + 1);
            if (seg2) {
                memcpy(seg2, text + offset, chunk);
                seg2[chunk] = '\0';
                cJSON_AddStringToObject(body2, "text", seg2);
                free(seg2);
            }
            char *json2 = cJSON_PrintUnformatted(body2);
            cJSON_Delete(body2);
            if (json2) {
                char *resp2 = tg_api_call("sendMessage", json2);
                free(json2);
                if (resp2) {
                    const char *desc2 = NULL;
                    sent_ok = tg_response_is_ok(resp2, &desc2);
                    if (!sent_ok) {
                        ESP_LOGE(TAG, "Plain send failed: %s", desc2 ? desc2 : "unknown");
                        ESP_LOGE(TAG, "Telegram raw response: %.300s", resp2);
                    }
                    free(resp2);
                } else {
                    ESP_LOGE(TAG, "Plain send failed: no HTTP response");
                }
            } else {
                ESP_LOGE(TAG, "Plain send failed: no JSON body");
            }
        }

        if (!sent_ok) {
            all_ok = 0;
        } else {
            if (markdown_failed) {
                ESP_LOGI(TAG, "Plain-text fallback succeeded for %s", chat_id);
            }
            ESP_LOGI(TAG, "Telegram send success to %s (%d bytes)", chat_id, (int)chunk);
        }

        free(resp);
        offset += chunk;
    }

    return all_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t telegram_set_token(const char *token)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_TG_TOKEN, token));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_bot_token, token, sizeof(s_bot_token) - 1);
    ESP_LOGI(TAG, "Telegram bot token saved");
    return ESP_OK;
}
