#include "feishu/feishu_bot.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "gateway/ws_server.h"
#include "security/access_control.h"

#include <stdbool.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "feishu";
static const char *FEISHU_START_HELP =
    "MimiClaw 已连接飞书 Bot。\n"
    "直接发送文本消息即可开始对话。";
#define FEISHU_VISION_TEXT_MAX 768
#define FEISHU_ERR_UNAUTHORIZED ((esp_err_t)(ESP_ERR_HTTP_BASE + 0x101))

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

typedef struct {
    char event_id[MIMI_FEISHU_EVENT_ID_MAX_LEN];
    int64_t seen_at_us;
} feishu_event_dedup_entry_t;

typedef struct {
    char message_id[96];
    char file_key[96];
    char file_name[128];
    char mime_type[96];
} feishu_media_ref_t;

static char s_app_id[96] = MIMI_SECRET_FEISHU_APP_ID;
static char s_app_secret[128] = MIMI_SECRET_FEISHU_APP_SECRET;
static char s_verify_token[128] = MIMI_SECRET_FEISHU_VERIFY_TOKEN;
static char s_encrypt_key[128] = MIMI_SECRET_FEISHU_ENCRYPT_KEY;
static char s_tenant_token[256] = {0};
static int64_t s_tenant_token_expire_us = 0;
static bool s_started = false;
static feishu_event_dedup_entry_t s_event_dedup[MIMI_FEISHU_EVENT_DEDUP_SIZE] = {0};
static portMUX_TYPE s_event_dedup_lock = portMUX_INITIALIZER_UNLOCKED;

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static bool feishu_is_configured(void)
{
    return s_app_id[0] != '\0' && s_app_secret[0] != '\0';
}

static void feishu_invalidate_tenant_token(void)
{
    s_tenant_token[0] = '\0';
    s_tenant_token_expire_us = 0;
}

static esp_err_t feishu_nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, key, value ? value : "");
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t feishu_nvs_erase_key(const char *key)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void feishu_load_str_from_nvs(const char *key, char *dst, size_t dst_size)
{
    nvs_handle_t nvs;
    if (!dst || dst_size == 0) {
        return;
    }
    if (nvs_open(MIMI_NVS_FEISHU, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = dst_size;
    char tmp[256] = {0};
    if (dst_size > sizeof(tmp)) {
        len = sizeof(tmp);
    }
    if (nvs_get_str(nvs, key, tmp, &len) == ESP_OK) {
        safe_copy(dst, dst_size, tmp);
    }
    nvs_close(nvs);
}

static bool feishu_get_header(httpd_req_t *req, const char *name, char *out, size_t out_size)
{
    if (!req || !name || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len >= out_size) {
        return false;
    }
    return httpd_req_get_hdr_value_str(req, name, out, out_size) == ESP_OK && out[0] != '\0';
}

static bool feishu_get_signature_headers(httpd_req_t *req,
                                         char *timestamp, size_t timestamp_size,
                                         char *nonce, size_t nonce_size,
                                         char *signature, size_t signature_size)
{
    bool got_timestamp = feishu_get_header(req, "X-Lark-Request-Timestamp",
                                           timestamp, timestamp_size) ||
                         feishu_get_header(req, "x-lark-request-timestamp",
                                           timestamp, timestamp_size);
    bool got_nonce = feishu_get_header(req, "X-Lark-Request-Nonce",
                                       nonce, nonce_size) ||
                     feishu_get_header(req, "x-lark-request-nonce",
                                       nonce, nonce_size);
    bool got_signature = feishu_get_header(req, "X-Lark-Signature",
                                           signature, signature_size) ||
                         feishu_get_header(req, "x-lark-signature",
                                           signature, signature_size);
    return got_timestamp && got_nonce && got_signature;
}

static bool feishu_sha256_bytes(const unsigned char *input, size_t input_len, unsigned char *digest)
{
    if (!input || !digest) {
        return false;
    }
    return mbedtls_sha256(input, input_len, digest, 0) == 0;
}

static bool feishu_signature_matches(httpd_req_t *req, const char *body_json)
{
    if (s_encrypt_key[0] == '\0') {
        return true;
    }
    if (!req || !body_json) {
        return false;
    }

    char timestamp[64] = {0};
    char nonce[64] = {0};
    char signature[80] = {0};
    if (!feishu_get_signature_headers(req, timestamp, sizeof(timestamp),
                                      nonce, sizeof(nonce),
                                      signature, sizeof(signature))) {
        ESP_LOGW(TAG, "Missing Feishu signature headers");
        return false;
    }

    size_t content_len = strlen(timestamp) + strlen(nonce) + strlen(s_encrypt_key) + strlen(body_json);
    char *content = calloc(1, content_len + 1);
    if (!content) {
        return false;
    }
    snprintf(content, content_len + 1, "%s%s%s%s", timestamp, nonce, s_encrypt_key, body_json);

    unsigned char digest[32] = {0};
    char hex[65] = {0};
    bool ok = false;
    if (feishu_sha256_bytes((const unsigned char *)content, strlen(content), digest)) {
        for (size_t i = 0; i < sizeof(digest); i++) {
            snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", digest[i]);
        }
        ok = strcasecmp(hex, signature) == 0;
    }
    free(content);
    return ok;
}

static esp_err_t feishu_decrypt_payload(const char *encrypted_b64, char **out_json)
{
    if (!encrypted_b64 || !encrypted_b64[0] || !out_json || s_encrypt_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    unsigned char hashed_key[32] = {0};
    if (!feishu_sha256_bytes((const unsigned char *)s_encrypt_key, strlen(s_encrypt_key), hashed_key)) {
        return ESP_FAIL;
    }

    size_t enc_len = 0;
    if (mbedtls_base64_decode(NULL, 0, &enc_len,
                              (const unsigned char *)encrypted_b64,
                              strlen(encrypted_b64)) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }

    unsigned char *enc_buf = calloc(1, enc_len + 1);
    if (!enc_buf) {
        return ESP_ERR_NO_MEM;
    }
    if (mbedtls_base64_decode(enc_buf, enc_len, &enc_len,
                              (const unsigned char *)encrypted_b64,
                              strlen(encrypted_b64)) != 0) {
        free(enc_buf);
        return ESP_FAIL;
    }
    if (enc_len <= 16 || ((enc_len - 16) % 16) != 0) {
        free(enc_buf);
        return ESP_FAIL;
    }

    unsigned char iv[16] = {0};
    memcpy(iv, enc_buf, 16);
    size_t cipher_len = enc_len - 16;
    unsigned char *plain = calloc(1, cipher_len + 1);
    if (!plain) {
        free(enc_buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(plain, enc_buf + 16, cipher_len);
    free(enc_buf);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int ret = mbedtls_aes_setkey_dec(&aes, hashed_key, 256);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipher_len, iv, plain, plain);
    }
    mbedtls_aes_free(&aes);
    if (ret != 0) {
        free(plain);
        return ESP_FAIL;
    }

    unsigned char pad = plain[cipher_len - 1];
    if (pad == 0 || pad > 16 || pad > cipher_len) {
        free(plain);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < pad; i++) {
        if (plain[cipher_len - 1 - i] != pad) {
            free(plain);
            return ESP_FAIL;
        }
    }
    plain[cipher_len - pad] = '\0';

    *out_json = (char *)plain;
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (!evt || !evt->user_data) return ESP_OK;

    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    if (resp->len + (size_t)evt->data_len + 1 > resp->cap) {
        size_t new_cap = resp->cap ? resp->cap : 1024;
        while (resp->len + (size_t)evt->data_len + 1 > new_cap) {
            new_cap *= 2;
        }
        char *new_buf = realloc(resp->buf, new_cap);
        if (!new_buf) {
            return ESP_ERR_NO_MEM;
        }
        resp->buf = new_buf;
        resp->cap = new_cap;
    }

    memcpy(resp->buf + resp->len, evt->data, (size_t)evt->data_len);
    resp->len += (size_t)evt->data_len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static esp_err_t feishu_post_json(const char *url, const char *auth_bearer,
                                  const char *post_data, int timeout_ms,
                                  int *out_status, char **out_body)
{
    if (!url || !post_data || !out_status || !out_body) {
        return ESP_ERR_INVALID_ARG;
    }

    http_resp_t resp = {
        .buf = calloc(1, 2048),
        .len = 0,
        .cap = 2048,
    };
    if (!resp.buf) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    if (auth_bearer && auth_bearer[0]) {
        char auth[320];
        snprintf(auth, sizeof(auth), "Bearer %s", auth_bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP POST failed: %s url=%s", esp_err_to_name(err), url);
        free(resp.buf);
        return err;
    }

    *out_body = resp.buf;
    return ESP_OK;
}

static esp_err_t feishu_refresh_tenant_token(void)
{
    if (!feishu_is_configured()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "app_id", s_app_id);
    cJSON_AddStringToObject(body, "app_secret", s_app_secret);

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    char *resp = NULL;
    esp_err_t err = feishu_post_json(
        "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
        NULL,
        post_data,
        MIMI_FEISHU_HTTP_TIMEOUT_MS,
        &status,
        &resp);
    free(post_data);
    if (err != ESP_OK) {
        return err;
    }
    if (status != 200 || !resp) {
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
    cJSON *expire = cJSON_GetObjectItem(root, "expire");
    cJSON *expire_in = cJSON_GetObjectItem(root, "expire_in");
    if ((cJSON_IsNumber(code) && code->valueint != 0) ||
        !cJSON_IsString(token) || !token->valuestring || token->valuestring[0] == '\0') {
        cJSON *msg = cJSON_GetObjectItem(root, "msg");
        ESP_LOGW(TAG, "Fetch tenant token failed: code=%d msg=%s",
                 cJSON_IsNumber(code) ? code->valueint : -1,
                 cJSON_IsString(msg) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int expire_s = 7200;
    if (cJSON_IsNumber(expire) && expire->valueint > 0) {
        expire_s = expire->valueint;
    } else if (cJSON_IsNumber(expire_in) && expire_in->valueint > 0) {
        expire_s = expire_in->valueint;
    }

    safe_copy(s_tenant_token, sizeof(s_tenant_token), token->valuestring);
    s_tenant_token_expire_us = esp_timer_get_time() + ((int64_t)expire_s - 60) * 1000000LL;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Tenant token refreshed, expires in %d seconds", expire_s);
    return ESP_OK;
}

static esp_err_t feishu_ensure_tenant_token(void)
{
    if (s_tenant_token[0] != '\0' && esp_timer_get_time() < s_tenant_token_expire_us) {
        return ESP_OK;
    }
    return feishu_refresh_tenant_token();
}

static size_t feishu_choose_chunk_size(const char *text, size_t remaining)
{
    size_t chunk = remaining > MIMI_FEISHU_TEXT_MAX_BYTES ? MIMI_FEISHU_TEXT_MAX_BYTES : remaining;
    while (chunk > 0 && ((unsigned char)text[chunk] & 0xC0) == 0x80) {
        chunk--;
    }
    return chunk > 0 ? chunk : remaining;
}

static esp_err_t feishu_send_text_once(const char *chat_id, const char *text, bool allow_refresh_retry)
{
    if (!chat_id || !chat_id[0] || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = feishu_ensure_tenant_token();
    if (err != ESP_OK) {
        return err;
    }

    cJSON *content = cJSON_CreateObject();
    cJSON *body = cJSON_CreateObject();
    if (!content || !body) {
        cJSON_Delete(content);
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(content, "text", text);
    char *content_json = cJSON_PrintUnformatted(content);
    cJSON_Delete(content);
    if (!content_json) {
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "receive_id", chat_id);
    cJSON_AddStringToObject(body, "msg_type", "text");
    cJSON_AddStringToObject(body, "content", content_json);
    free(content_json);

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    char *resp = NULL;
    err = feishu_post_json(
        "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id",
        s_tenant_token,
        post_data,
        MIMI_FEISHU_HTTP_TIMEOUT_MS,
        &status,
        &resp);
    free(post_data);
    if (err != ESP_OK) {
        return err;
    }

    if (status == 401) {
        feishu_invalidate_tenant_token();
        free(resp);
        err = feishu_ensure_tenant_token();
        if (err != ESP_OK) {
            return err;
        }
        if (!allow_refresh_retry) {
            return ESP_FAIL;
        }
        return feishu_send_text_once(chat_id, text, false);
    }

    if (status != 200 || !resp) {
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    bool ok = cJSON_IsNumber(code) && code->valueint == 0;
    if (!ok) {
        cJSON *msg = cJSON_GetObjectItem(root, "msg");
        ESP_LOGW(TAG, "Send Feishu message failed: code=%d msg=%s",
                 cJSON_IsNumber(code) ? code->valueint : -1,
                 cJSON_IsString(msg) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static bool feishu_extract_payload_token(cJSON *root, char *out_token, size_t out_size)
{
    if (!root || !out_token || out_size == 0) {
        return false;
    }
    out_token[0] = '\0';

    cJSON *token = cJSON_GetObjectItem(root, "token");
    if (cJSON_IsString(token) && token->valuestring && token->valuestring[0]) {
        safe_copy(out_token, out_size, token->valuestring);
        return true;
    }

    cJSON *header = cJSON_GetObjectItem(root, "header");
    if (!cJSON_IsObject(header)) {
        return false;
    }
    cJSON *header_token = cJSON_GetObjectItem(header, "token");
    if (cJSON_IsString(header_token) && header_token->valuestring && header_token->valuestring[0]) {
        safe_copy(out_token, out_size, header_token->valuestring);
        return true;
    }
    return false;
}

static esp_err_t feishu_send_http_json(httpd_req_t *req, const char *status, const char *body)
{
    if (!req || !body) {
        return ESP_ERR_INVALID_ARG;
    }
    if (status && status[0]) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t feishu_read_http_body(httpd_req_t *req, char **out_body)
{
    if (!req || !out_body) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_body = NULL;

    if (req->content_len <= 0 || req->content_len > MIMI_FEISHU_EVENT_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';
    *out_body = buf;
    return ESP_OK;
}

static bool feishu_extract_text_from_content(const char *content_json, char *out_text, size_t out_size)
{
    if (!content_json || !out_text || out_size == 0) {
        return false;
    }
    out_text[0] = '\0';

    cJSON *root = cJSON_Parse(content_json);
    if (!root) {
        return false;
    }

    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
        safe_copy(out_text, out_size, text->valuestring);
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);
    return false;
}

static const char *feishu_json_get_string(cJSON *root, const char *key)
{
    if (!root || !key) {
        return NULL;
    }
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        return item->valuestring;
    }
    return NULL;
}

static bool feishu_json_get_int(cJSON *root, const char *key, int *out_value)
{
    if (!root || !key || !out_value) {
        return false;
    }
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out_value = item->valueint;
    return true;
}

static const char *feishu_get_bus_media_type(const char *message_type)
{
    if (!message_type || !message_type[0]) {
        return "media";
    }
    if (strcmp(message_type, "image") == 0 || strcmp(message_type, "sticker") == 0) {
        return "photo";
    }
    if (strcmp(message_type, "file") == 0) {
        return "document";
    }
    if (strcmp(message_type, "audio") == 0) {
        return "voice";
    }
    if (strcmp(message_type, "media") == 0) {
        return "media";
    }
    return "media";
}

#if MIMI_FEISHU_GATEWAY_MEDIA_ENABLED
static const char *feishu_guess_image_format(const char *file_name, const char *mime_type)
{
    if (file_name && file_name[0]) {
        const char *dot = strrchr(file_name, '.');
        if (dot && dot[1]) {
            char ext[8] = {0};
            size_t n = strlen(dot + 1);
            if (n >= sizeof(ext)) {
                n = sizeof(ext) - 1;
            }
            for (size_t i = 0; i < n; i++) {
                ext[i] = (char)tolower((unsigned char)dot[1 + i]);
            }
            ext[n] = '\0';

            if (strcmp(ext, "png") == 0) return "png";
            if (strcmp(ext, "webp") == 0) return "webp";
            if (strcmp(ext, "bmp") == 0) return "bmp";
            if (strcmp(ext, "gif") == 0) return "gif";
        }
    }

    if (mime_type && mime_type[0]) {
        if (strcasestr(mime_type, "png")) return "png";
        if (strcasestr(mime_type, "webp")) return "webp";
        if (strcasestr(mime_type, "bmp")) return "bmp";
        if (strcasestr(mime_type, "gif")) return "gif";
    }
    return "jpeg";
}

static void feishu_guess_doc_format(const char *file_name, const char *mime_type,
                                    char *out_format, size_t out_size)
{
    if (!out_format || out_size < 2) {
        return;
    }
    safe_copy(out_format, out_size, "bin");

    if (file_name && file_name[0]) {
        const char *dot = strrchr(file_name, '.');
        if (dot && dot[1]) {
            size_t n = strlen(dot + 1);
            if (n >= out_size) {
                n = out_size - 1;
            }
            for (size_t i = 0; i < n; i++) {
                out_format[i] = (char)tolower((unsigned char)dot[1 + i]);
            }
            out_format[n] = '\0';
            if (out_format[0] != '\0') {
                return;
            }
        }
    }

    if (!mime_type || !mime_type[0]) {
        return;
    }
    if (strcasestr(mime_type, "pdf")) {
        safe_copy(out_format, out_size, "pdf");
    } else if (strcasestr(mime_type, "markdown")) {
        safe_copy(out_format, out_size, "md");
    } else if (strcasestr(mime_type, "plain")) {
        safe_copy(out_format, out_size, "txt");
    } else if (strcasestr(mime_type, "msword")) {
        safe_copy(out_format, out_size, "doc");
    } else if (strcasestr(mime_type, "wordprocessingml")) {
        safe_copy(out_format, out_size, "docx");
    } else if (strcasestr(mime_type, "spreadsheetml")) {
        safe_copy(out_format, out_size, "xlsx");
    } else if (strcasestr(mime_type, "presentationml")) {
        safe_copy(out_format, out_size, "pptx");
    } else if (strcasestr(mime_type, "ms-excel")) {
        safe_copy(out_format, out_size, "xls");
    } else if (strcasestr(mime_type, "ms-powerpoint")) {
        safe_copy(out_format, out_size, "ppt");
    } else if (strcasestr(mime_type, "json")) {
        safe_copy(out_format, out_size, "json");
    } else if (strcasestr(mime_type, "xml")) {
        safe_copy(out_format, out_size, "xml");
    } else if (strcasestr(mime_type, "zip")) {
        safe_copy(out_format, out_size, "zip");
    }
}

static esp_err_t feishu_build_gateway_http_url(const char *endpoint_path, char *out_url, size_t out_size)
{
    if (!endpoint_path || !endpoint_path[0] || !out_url || out_size < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    out_url[0] = '\0';

    char gw[160] = {0};
    safe_copy(gw, sizeof(gw), MIMI_VOICE_GATEWAY_URL);

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_VOICE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(gw);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_VOICE_GW, gw, &len) != ESP_OK || gw[0] == '\0') {
            safe_copy(gw, sizeof(gw), MIMI_VOICE_GATEWAY_URL);
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
    if (hostport_len == 0 || hostport_len >= 96) {
        return ESP_FAIL;
    }

    char hostport[96] = {0};
    memcpy(hostport, p, hostport_len);
    hostport[hostport_len] = '\0';

    char host[80] = {0};
    int port = secure ? 443 : 80;
    const char *colon = strrchr(hostport, ':');
    if (colon && colon[1] != '\0' && isdigit((unsigned char)colon[1])) {
        size_t host_len = (size_t)(colon - hostport);
        if (host_len == 0 || host_len >= sizeof(host)) {
            return ESP_FAIL;
        }
        memcpy(host, hostport, host_len);
        host[host_len] = '\0';
        port = atoi(colon + 1);
    } else {
        safe_copy(host, sizeof(host), hostport);
    }

    int http_port = (port > 0) ? (port + 1) : 8091;
    snprintf(out_url, out_size, "%s://%s:%d/%s",
             secure ? "https" : "http", host, http_port, endpoint_path);
    return ESP_OK;
}

static esp_err_t feishu_vision_upload(const uint8_t *image, size_t image_len, const char *image_format,
                                      char *out_text, size_t out_size)
{
    if (!image || image_len == 0 || !out_text || out_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    char vision_url[192] = {0};
    esp_err_t err = feishu_build_gateway_http_url("vision_upload", vision_url, sizeof(vision_url));
    if (err != ESP_OK) {
        return err;
    }

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) {
        return ESP_ERR_NO_MEM;
    }

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
        ESP_LOGW(TAG, "Feishu vision upload failed: err=%s status=%d url=%s",
                 esp_err_to_name(err), status, vision_url);
        free(resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) {
        return ESP_FAIL;
    }

    char merged[FEISHU_VISION_TEXT_MAX] = {0};
    size_t off = 0;
    cJSON *caption = cJSON_GetObjectItem(root, "caption");
    if (cJSON_IsString(caption) && caption->valuestring && caption->valuestring[0]) {
        int n = snprintf(merged + off, sizeof(merged) - off, "描述：%s", caption->valuestring);
        if (n > 0) {
            off += (size_t)n;
            if (off >= sizeof(merged)) {
                off = sizeof(merged) - 1;
            }
        }
    }

    cJSON *ocr_text = cJSON_GetObjectItem(root, "ocr_text");
    if (cJSON_IsString(ocr_text) && ocr_text->valuestring && ocr_text->valuestring[0]) {
        int n = snprintf(merged + off, sizeof(merged) - off, "%s文字：%s",
                         off ? "\n" : "", ocr_text->valuestring);
        if (n > 0) {
            off += (size_t)n;
            if (off >= sizeof(merged)) {
                off = sizeof(merged) - 1;
            }
        }
    }

    cJSON *objects = cJSON_GetObjectItem(root, "objects");
    if (objects && cJSON_IsArray(objects) && cJSON_GetArraySize(objects) > 0) {
        char items[256] = {0};
        size_t item_off = 0;
        cJSON *it = NULL;
        int count = 0;
        cJSON_ArrayForEach(it, objects) {
            if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0]) {
                continue;
            }
            if (count >= 12) {
                break;
            }
            int n = snprintf(items + item_off, sizeof(items) - item_off, "%s%s",
                             count ? "、" : "", it->valuestring);
            if (n <= 0) {
                break;
            }
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
                if (off >= sizeof(merged)) {
                    off = sizeof(merged) - 1;
                }
            }
        }
    }

    if (off == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        safe_copy(merged, sizeof(merged), text->valuestring);
    }

    safe_copy(out_text, out_size, merged);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t feishu_doc_upload(const uint8_t *doc_data, size_t doc_len,
                                   const char *doc_name, const char *doc_mime,
                                   const char *doc_path, const char *doc_format,
                                   char *out_text, size_t out_size,
                                   char *out_meta, size_t meta_size)
{
    if (!doc_data || doc_len == 0 || !out_text || out_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';
    if (out_meta && meta_size > 0) {
        out_meta[0] = '\0';
    }

    char doc_url[192] = {0};
    esp_err_t err = feishu_build_gateway_http_url("doc_upload", doc_url, sizeof(doc_url));
    if (err != ESP_OK) {
        return err;
    }

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) {
        return ESP_ERR_NO_MEM;
    }

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
    if (doc_name && doc_name[0]) {
        esp_http_client_set_header(client, "X-Doc-Name", doc_name);
    }
    if (doc_mime && doc_mime[0]) {
        esp_http_client_set_header(client, "X-Doc-Mime", doc_mime);
    }
    if (doc_path && doc_path[0]) {
        esp_http_client_set_header(client, "X-Doc-Path", doc_path);
    }
    if (doc_format && doc_format[0]) {
        esp_http_client_set_header(client, "X-Doc-Format", doc_format);
    }
    esp_http_client_set_post_field(client, (const char *)doc_data, doc_len);

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Feishu doc upload failed: err=%s status=%d url=%s",
                 esp_err_to_name(err), status, doc_url);
        free(resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    safe_copy(out_text, out_size, text->valuestring);

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
        if (cJSON_IsString(fmt_item)) {
            fmt = fmt_item->valuestring;
        }
        if (cJSON_IsString(parser_item)) {
            parser = parser_item->valuestring;
        }
        if (cJSON_IsNumber(len_item)) {
            text_len = len_item->valueint;
        }
        if (cJSON_IsBool(trunc_item)) {
            truncated = cJSON_IsTrue(trunc_item);
        }
        if (cJSON_IsBool(vision_item)) {
            from_vision = cJSON_IsTrue(vision_item);
        }
        snprintf(out_meta, meta_size,
                 "{\"doc_parse\":\"ok\",\"format\":\"%.16s\",\"parser\":\"%.24s\","
                 "\"text_len\":%d,\"truncated\":%s,\"from_vision\":%s}",
                 fmt, parser, text_len,
                 truncated ? "true" : "false",
                 from_vision ? "true" : "false");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static bool feishu_extract_media_ref(cJSON *message, const char *message_type, feishu_media_ref_t *out_ref)
{
    if (!message || !out_ref) {
        return false;
    }
    memset(out_ref, 0, sizeof(*out_ref));

    cJSON *message_id_item = cJSON_GetObjectItem(message, "message_id");
    if (cJSON_IsString(message_id_item) && message_id_item->valuestring) {
        safe_copy(out_ref->message_id, sizeof(out_ref->message_id), message_id_item->valuestring);
    }

    cJSON *content_item = cJSON_GetObjectItem(message, "content");
    if (!cJSON_IsString(content_item) || !content_item->valuestring || !content_item->valuestring[0]) {
        return false;
    }

    cJSON *content_root = cJSON_Parse(content_item->valuestring);
    if (!content_root) {
        return false;
    }

    const char *key = NULL;
    if (message_type && strcmp(message_type, "image") == 0) {
        key = feishu_json_get_string(content_root, "image_key");
    } else if (message_type && strcmp(message_type, "file") == 0) {
        key = feishu_json_get_string(content_root, "file_key");
    }
    if (!key) {
        key = feishu_json_get_string(content_root, "file_key");
    }
    if (!key) {
        key = feishu_json_get_string(content_root, "image_key");
    }
    if (!key) {
        key = feishu_json_get_string(content_root, "media_key");
    }
    if (!key) {
        key = feishu_json_get_string(content_root, "audio_key");
    }

    const char *file_name = feishu_json_get_string(content_root, "file_name");
    if (!file_name) {
        file_name = feishu_json_get_string(content_root, "title");
    }
    const char *mime_type = feishu_json_get_string(content_root, "mime_type");

    if (key && key[0]) {
        safe_copy(out_ref->file_key, sizeof(out_ref->file_key), key);
    }
    if (file_name && file_name[0]) {
        safe_copy(out_ref->file_name, sizeof(out_ref->file_name), file_name);
    }
    if (mime_type && mime_type[0]) {
        safe_copy(out_ref->mime_type, sizeof(out_ref->mime_type), mime_type);
    }

    cJSON_Delete(content_root);
    return out_ref->message_id[0] != '\0' && out_ref->file_key[0] != '\0';
}

static esp_err_t feishu_download_message_resource_once(const feishu_media_ref_t *ref,
                                                       uint8_t **out_data, size_t *out_len)
{
    if (!ref || !ref->message_id[0] || !ref->file_key[0] || !out_data || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0;

    esp_err_t err = feishu_ensure_tenant_token();
    if (err != ESP_OK) {
        return err;
    }

    char url[512] = {0};
    snprintf(url, sizeof(url),
             "https://open.feishu.cn/open-apis/im/v1/messages/%s/resources/%s",
             ref->message_id, ref->file_key);

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = MIMI_FEISHU_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return ESP_FAIL;
    }

    char auth[320] = {0};
    snprintf(auth, sizeof(auth), "Bearer %s", s_tenant_token);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", auth);

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Feishu media download failed: err=%s url=%s",
                 esp_err_to_name(err), url);
        free(resp.buf);
        return err;
    }
    if (status == 401) {
        free(resp.buf);
        return FEISHU_ERR_UNAUTHORIZED;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "Feishu media download status=%d message=%s file_key=%.48s",
                 status, ref->message_id, ref->file_key);
        free(resp.buf);
        return ESP_FAIL;
    }
    if (resp.len == 0 || resp.len > MIMI_FEISHU_MEDIA_MAX_BYTES) {
        ESP_LOGW(TAG, "Feishu media size invalid: %u bytes file_key=%.48s",
                 (unsigned int)resp.len, ref->file_key);
        free(resp.buf);
        return ESP_ERR_INVALID_SIZE;
    }

    *out_data = (uint8_t *)resp.buf;
    *out_len = resp.len;
    return ESP_OK;
}

static esp_err_t feishu_download_message_resource(const feishu_media_ref_t *ref,
                                                  uint8_t **out_data, size_t *out_len)
{
    esp_err_t err = feishu_download_message_resource_once(ref, out_data, out_len);
    if (err == FEISHU_ERR_UNAUTHORIZED) {
        feishu_invalidate_tenant_token();
        return feishu_download_message_resource_once(ref, out_data, out_len);
    }
    return err;
}

static void feishu_merge_json_object(cJSON *dst, cJSON *src)
{
    if (!cJSON_IsObject(dst) || !cJSON_IsObject(src)) {
        return;
    }
    for (cJSON *item = src->child; item; item = item->next) {
        if (!item->string || !item->string[0]) {
            continue;
        }
        cJSON *dup = cJSON_Duplicate(item, true);
        if (!dup) {
            continue;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(dst, item->string);
        cJSON_AddItemToObject(dst, item->string, dup);
    }
}

static bool feishu_try_build_gateway_media_text(cJSON *message,
                                                const char *message_type,
                                                char *out_text,
                                                size_t out_text_size,
                                                char *out_file_id,
                                                size_t out_file_id_size,
                                                char *out_file_path,
                                                size_t out_file_path_size,
                                                char **out_meta_json)
{
    if (!message || !message_type || !out_text || out_text_size < 2) {
        return false;
    }
    out_text[0] = '\0';
    if (out_file_id && out_file_id_size > 0) {
        out_file_id[0] = '\0';
    }
    if (out_file_path && out_file_path_size > 0) {
        out_file_path[0] = '\0';
    }
    if (out_meta_json) {
        *out_meta_json = NULL;
    }

    if (strcmp(message_type, "image") != 0 && strcmp(message_type, "file") != 0) {
        return false;
    }

    feishu_media_ref_t ref = {0};
    if (!feishu_extract_media_ref(message, message_type, &ref)) {
        return false;
    }

    uint8_t *media_data = NULL;
    size_t media_len = 0;
    esp_err_t media_err = feishu_download_message_resource(&ref, &media_data, &media_len);
    if (media_err != ESP_OK || !media_data || media_len == 0) {
        free(media_data);
        ESP_LOGW(TAG, "Feishu %s media download failed: err=%s message_id=%s file_key=%.48s",
                 message_type, esp_err_to_name(media_err), ref.message_id, ref.file_key);
        return false;
    }

    char parsed_text[1024] = {0};
    char doc_meta[192] = {0};
    char resolved_path[128] = {0};
    safe_copy(resolved_path, sizeof(resolved_path), ref.file_name);

    if (strcmp(message_type, "image") == 0) {
        const char *img_fmt = feishu_guess_image_format(ref.file_name, ref.mime_type);
        if (!resolved_path[0]) {
            snprintf(resolved_path, sizeof(resolved_path), "feishu_image.%s", img_fmt);
        }
        media_err = feishu_vision_upload(media_data, media_len, img_fmt,
                                         parsed_text, sizeof(parsed_text));
        if (media_err == ESP_OK && parsed_text[0]) {
            snprintf(out_text, out_text_size,
                     "[飞书图片解析]\n%s\n\n[下载信息]\n大小: %u 字节\nfile_path: %.96s",
                     parsed_text, (unsigned int)media_len, resolved_path);
        }
    } else {
        char doc_format[16] = {0};
        feishu_guess_doc_format(ref.file_name, ref.mime_type, doc_format, sizeof(doc_format));
        if (!resolved_path[0]) {
            snprintf(resolved_path, sizeof(resolved_path), "feishu_file.%s",
                     doc_format[0] ? doc_format : "bin");
        }
        media_err = feishu_doc_upload(media_data, media_len,
                                      ref.file_name, ref.mime_type,
                                      resolved_path, doc_format,
                                      parsed_text, sizeof(parsed_text),
                                      doc_meta, sizeof(doc_meta));
        if (media_err == ESP_OK && parsed_text[0]) {
            snprintf(out_text, out_text_size,
                     "[飞书文件解析]\n%.760s\n\n[下载信息]\n大小: %u 字节\nfile_path: %.96s",
                     parsed_text, (unsigned int)media_len, resolved_path);
        }
    }
    free(media_data);

    if (media_err != ESP_OK || !out_text[0]) {
        ESP_LOGW(TAG, "Feishu %s gateway parse failed: err=%s message_id=%s file_key=%.48s",
                 message_type, esp_err_to_name(media_err), ref.message_id, ref.file_key);
        return false;
    }

    if (out_file_id && out_file_id_size > 0) {
        safe_copy(out_file_id, out_file_id_size, ref.file_key);
    }
    if (out_file_path && out_file_path_size > 0) {
        safe_copy(out_file_path, out_file_path_size, resolved_path);
    }
    if (out_meta_json) {
        cJSON *meta = cJSON_CreateObject();
        if (meta) {
            cJSON_AddStringToObject(meta, "source_type", message_type);
            cJSON_AddStringToObject(meta, "gateway_parse",
                                    strcmp(message_type, "image") == 0 ? "vision" : "doc");
            cJSON_AddNumberToObject(meta, "bytes", (double)media_len);
            if (ref.message_id[0]) {
                cJSON_AddStringToObject(meta, "message_id", ref.message_id);
            }
            if (ref.file_key[0]) {
                cJSON_AddStringToObject(meta, "file_key", ref.file_key);
            }
            if (ref.file_name[0]) {
                cJSON_AddStringToObject(meta, "name", ref.file_name);
            }
            if (ref.mime_type[0]) {
                cJSON_AddStringToObject(meta, "mime_type", ref.mime_type);
            }
            if (resolved_path[0]) {
                cJSON_AddStringToObject(meta, "file_path", resolved_path);
            }
            if (doc_meta[0]) {
                cJSON *doc_meta_root = cJSON_Parse(doc_meta);
                if (doc_meta_root) {
                    feishu_merge_json_object(meta, doc_meta_root);
                    cJSON_Delete(doc_meta_root);
                }
            }
            *out_meta_json = cJSON_PrintUnformatted(meta);
            cJSON_Delete(meta);
        }
    }

    ESP_LOGI(TAG, "Feishu %s gateway parse success message=%s bytes=%u",
             message_type, ref.message_id, (unsigned int)media_len);
    return true;
}
#else
static bool feishu_try_build_gateway_media_text(cJSON *message,
                                                const char *message_type,
                                                char *out_text,
                                                size_t out_text_size,
                                                char *out_file_id,
                                                size_t out_file_id_size,
                                                char *out_file_path,
                                                size_t out_file_path_size,
                                                char **out_meta_json)
{
    (void)message;
    (void)message_type;
    (void)out_text;
    (void)out_text_size;
    (void)out_file_id;
    (void)out_file_id_size;
    (void)out_file_path;
    (void)out_file_path_size;
    if (out_meta_json) {
        *out_meta_json = NULL;
    }
    return false;
}
#endif

static bool feishu_build_media_summary(cJSON *message,
                                       const char *message_type,
                                       char *summary,
                                       size_t summary_size,
                                       char *out_file_id,
                                       size_t file_id_size,
                                       char **out_meta_json)
{
    if (!message || !message_type || !summary || summary_size < 2) {
        return false;
    }
    summary[0] = '\0';
    if (out_file_id && file_id_size > 0) {
        out_file_id[0] = '\0';
    }
    if (out_meta_json) {
        *out_meta_json = NULL;
    }

    const char *message_id = "";
    cJSON *message_id_item = cJSON_GetObjectItem(message, "message_id");
    if (cJSON_IsString(message_id_item) && message_id_item->valuestring) {
        message_id = message_id_item->valuestring;
    }

    const char *content_json = NULL;
    cJSON *content_item = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content_item) && content_item->valuestring) {
        content_json = content_item->valuestring;
    }

    cJSON *content_root = NULL;
    if (content_json && content_json[0]) {
        content_root = cJSON_Parse(content_json);
    }

    const char *key = NULL;
    const char *name = NULL;
    const char *mime = NULL;
    int duration = 0;
    int file_size = 0;
    bool has_duration = false;
    bool has_file_size = false;

    if (content_root) {
        key = feishu_json_get_string(content_root, "file_key");
        if (!key) key = feishu_json_get_string(content_root, "image_key");
        if (!key) key = feishu_json_get_string(content_root, "media_key");
        if (!key) key = feishu_json_get_string(content_root, "audio_key");
        name = feishu_json_get_string(content_root, "file_name");
        if (!name) name = feishu_json_get_string(content_root, "title");
        mime = feishu_json_get_string(content_root, "mime_type");
        has_duration = feishu_json_get_int(content_root, "duration", &duration);
        has_file_size = feishu_json_get_int(content_root, "file_size", &file_size);
    }

    if (out_file_id && file_id_size > 0 && key && key[0]) {
        safe_copy(out_file_id, file_id_size, key);
    }

    if (strcmp(message_type, "image") == 0) {
        snprintf(summary, summary_size,
                 "[飞书图片消息]\n"
                 "image_key: %.96s\n"
                 "message_id: %.96s",
                 key ? key : "",
                 message_id);
    } else if (strcmp(message_type, "file") == 0) {
        snprintf(summary, summary_size,
                 "[飞书文件消息]\n"
                 "文件名: %.96s\n"
                 "大小: %d 字节\n"
                 "file_key: %.96s\n"
                 "message_id: %.96s",
                 name ? name : "",
                 has_file_size ? file_size : 0,
                 key ? key : "",
                 message_id);
    } else if (strcmp(message_type, "audio") == 0) {
        snprintf(summary, summary_size,
                 "[飞书语音消息]\n"
                 "时长: %d\n"
                 "file_key: %.96s\n"
                 "message_id: %.96s",
                 has_duration ? duration : 0,
                 key ? key : "",
                 message_id);
    } else if (strcmp(message_type, "media") == 0) {
        snprintf(summary, summary_size,
                 "[飞书媒体消息]\n"
                 "标题: %.96s\n"
                 "时长: %d\n"
                 "file_key: %.96s\n"
                 "message_id: %.96s",
                 name ? name : "",
                 has_duration ? duration : 0,
                 key ? key : "",
                 message_id);
    } else if (strcmp(message_type, "sticker") == 0) {
        snprintf(summary, summary_size,
                 "[飞书表情消息]\n"
                 "file_key: %.96s\n"
                 "message_id: %.96s",
                 key ? key : "",
                 message_id);
    } else {
        snprintf(summary, summary_size,
                 "[飞书%s消息]\n"
                 "message_id: %.96s\n"
                 "content: %.320s",
                 message_type,
                 message_id,
                 content_json ? content_json : "{}");
    }

    if (out_meta_json) {
        cJSON *meta = cJSON_CreateObject();
        if (meta) {
            cJSON_AddStringToObject(meta, "source_type", message_type);
            if (message_id[0]) {
                cJSON_AddStringToObject(meta, "message_id", message_id);
            }
            if (key && key[0]) {
                cJSON_AddStringToObject(meta, "file_key", key);
            }
            if (name && name[0]) {
                cJSON_AddStringToObject(meta, "name", name);
            }
            if (mime && mime[0]) {
                cJSON_AddStringToObject(meta, "mime_type", mime);
            }
            if (has_duration) {
                cJSON_AddNumberToObject(meta, "duration", duration);
            }
            if (has_file_size) {
                cJSON_AddNumberToObject(meta, "file_size", file_size);
            }
            *out_meta_json = cJSON_PrintUnformatted(meta);
            cJSON_Delete(meta);
        }
    }

    cJSON_Delete(content_root);
    return summary[0] != '\0';
}

static bool feishu_extract_event_unique_id(cJSON *root, char *out_id, size_t out_size)
{
    if (!root || !out_id || out_size == 0) {
        return false;
    }
    out_id[0] = '\0';

    cJSON *header = cJSON_GetObjectItem(root, "header");
    if (cJSON_IsObject(header)) {
        cJSON *event_id = cJSON_GetObjectItem(header, "event_id");
        if (cJSON_IsString(event_id) && event_id->valuestring && event_id->valuestring[0]) {
            snprintf(out_id, out_size, "event:%s", event_id->valuestring);
            return true;
        }
    }

    cJSON *event = cJSON_GetObjectItem(root, "event");
    if (!cJSON_IsObject(event)) {
        return false;
    }
    cJSON *message = cJSON_GetObjectItem(event, "message");
    if (!cJSON_IsObject(message)) {
        return false;
    }
    cJSON *message_id = cJSON_GetObjectItem(message, "message_id");
    if (cJSON_IsString(message_id) && message_id->valuestring && message_id->valuestring[0]) {
        snprintf(out_id, out_size, "msg:%s", message_id->valuestring);
        return true;
    }
    return false;
}

static bool feishu_is_duplicate_event(const char *event_id)
{
    if (!event_id || !event_id[0]) {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t ttl_us = (int64_t)MIMI_FEISHU_EVENT_DEDUP_TTL_MS * 1000LL;
    bool is_duplicate = false;
    int replace_idx = -1;
    int64_t oldest_seen_at = LLONG_MAX;

    portENTER_CRITICAL(&s_event_dedup_lock);
    for (int i = 0; i < MIMI_FEISHU_EVENT_DEDUP_SIZE; i++) {
        feishu_event_dedup_entry_t *entry = &s_event_dedup[i];
        bool expired = entry->seen_at_us <= 0 || (now_us - entry->seen_at_us) > ttl_us;

        if (!expired && strcmp(entry->event_id, event_id) == 0) {
            entry->seen_at_us = now_us;
            is_duplicate = true;
            break;
        }

        if (replace_idx < 0 && (entry->event_id[0] == '\0' || expired)) {
            replace_idx = i;
        }
        if (entry->seen_at_us < oldest_seen_at) {
            oldest_seen_at = entry->seen_at_us;
            if (replace_idx < 0) {
                replace_idx = i;
            }
        }
    }

    if (!is_duplicate && replace_idx >= 0) {
        safe_copy(s_event_dedup[replace_idx].event_id,
                  sizeof(s_event_dedup[replace_idx].event_id),
                  event_id);
        s_event_dedup[replace_idx].seen_at_us = now_us;
    }
    portEXIT_CRITICAL(&s_event_dedup_lock);

    return is_duplicate;
}

static void feishu_push_inbound(const char *chat_id,
                                const char *content,
                                const char *media_type,
                                const char *file_id,
                                const char *file_path,
                                const char *meta_json)
{
    if (!chat_id || !chat_id[0] || !content || !content[0]) {
        return;
    }

    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_FEISHU, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    strncpy(msg.media_type,
            (media_type && media_type[0]) ? media_type : "text",
            sizeof(msg.media_type) - 1);
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

static esp_err_t feishu_events_handler(httpd_req_t *req)
{
    if (!feishu_is_configured()) {
        ESP_LOGW(TAG, "Ignore Feishu callback: app credentials are not configured");
        return feishu_send_http_json(req, "503 Service Unavailable", "{\"code\":503}");
    }

    char *body = NULL;
    esp_err_t err = feishu_read_http_body(req, &body);
    if (err != ESP_OK) {
        return feishu_send_http_json(req, "400 Bad Request", "{\"code\":400}");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        free(body);
        return feishu_send_http_json(req, "400 Bad Request", "{\"code\":400}");
    }

    char *canonical_body = cJSON_PrintUnformatted(root);
    if (!canonical_body) {
        cJSON_Delete(root);
        free(body);
        return feishu_send_http_json(req, "500 Internal Server Error", "{\"code\":500}");
    }

    if (!feishu_signature_matches(req, canonical_body)) {
        free(canonical_body);
        cJSON_Delete(root);
        free(body);
        ESP_LOGW(TAG, "Feishu callback signature mismatch");
        return feishu_send_http_json(req, "401 Unauthorized", "{\"code\":401}");
    }
    free(canonical_body);
    free(body);

    if (cJSON_GetObjectItem(root, "encrypt")) {
        cJSON *encrypt = cJSON_GetObjectItem(root, "encrypt");
        if (!cJSON_IsString(encrypt) || !encrypt->valuestring || s_encrypt_key[0] == '\0') {
            cJSON_Delete(root);
            ESP_LOGW(TAG, "Encrypted Feishu callback received but encrypt_key is missing");
            return feishu_send_http_json(req, "401 Unauthorized", "{\"code\":401}");
        }

        char *decrypted_json = NULL;
        esp_err_t decrypt_err = feishu_decrypt_payload(encrypt->valuestring, &decrypted_json);
        cJSON_Delete(root);
        if (decrypt_err != ESP_OK || !decrypted_json) {
            ESP_LOGW(TAG, "Decrypt Feishu callback failed: %s", esp_err_to_name(decrypt_err));
            return feishu_send_http_json(req, "400 Bad Request", "{\"code\":400}");
        }

        root = cJSON_Parse(decrypted_json);
        free(decrypted_json);
        if (!root) {
            ESP_LOGW(TAG, "Parse decrypted Feishu callback failed");
            return feishu_send_http_json(req, "400 Bad Request", "{\"code\":400}");
        }
    }

    char payload_token[128] = {0};
    feishu_extract_payload_token(root, payload_token, sizeof(payload_token));
    if (s_verify_token[0] && strcmp(payload_token, s_verify_token) != 0) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Feishu callback token mismatch");
        return feishu_send_http_json(req, "401 Unauthorized", "{\"code\":401}");
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *challenge = cJSON_GetObjectItem(root, "challenge");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "url_verification") == 0 &&
        cJSON_IsString(challenge) && challenge->valuestring) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            cJSON_Delete(root);
            return feishu_send_http_json(req, "500 Internal Server Error", "{\"code\":500}");
        }
        cJSON_AddStringToObject(resp, "challenge", challenge->valuestring);
        char *resp_json = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        cJSON_Delete(root);
        if (!resp_json) {
            return feishu_send_http_json(req, "500 Internal Server Error", "{\"code\":500}");
        }
        esp_err_t send_err = feishu_send_http_json(req, NULL, resp_json);
        free(resp_json);
        return send_err;
    }

    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *event = cJSON_GetObjectItem(root, "event");
    cJSON *event_type = header ? cJSON_GetObjectItem(header, "event_type") : NULL;
    if (!cJSON_IsString(event_type) || strcmp(event_type->valuestring, "im.message.receive_v1") != 0 ||
        !cJSON_IsObject(event)) {
        cJSON_Delete(root);
        return feishu_send_http_json(req, NULL, "{\"code\":0}");
    }

    char event_unique_id[MIMI_FEISHU_EVENT_ID_MAX_LEN] = {0};
    if (feishu_extract_event_unique_id(root, event_unique_id, sizeof(event_unique_id)) &&
        feishu_is_duplicate_event(event_unique_id)) {
        ESP_LOGI(TAG, "Skip duplicate Feishu event %s", event_unique_id);
        cJSON_Delete(root);
        return feishu_send_http_json(req, NULL, "{\"code\":0}");
    }

    char sender_id[96] = {0};
    cJSON *sender = cJSON_GetObjectItem(event, "sender");
    if (cJSON_IsObject(sender)) {
        cJSON *sender_id_obj = cJSON_GetObjectItem(sender, "sender_id");
        if (cJSON_IsObject(sender_id_obj)) {
            cJSON *open_id = cJSON_GetObjectItem(sender_id_obj, "open_id");
            cJSON *user_id = cJSON_GetObjectItem(sender_id_obj, "user_id");
            cJSON *union_id = cJSON_GetObjectItem(sender_id_obj, "union_id");
            if (cJSON_IsString(open_id) && open_id->valuestring) {
                safe_copy(sender_id, sizeof(sender_id), open_id->valuestring);
            } else if (cJSON_IsString(user_id) && user_id->valuestring) {
                safe_copy(sender_id, sizeof(sender_id), user_id->valuestring);
            } else if (cJSON_IsString(union_id) && union_id->valuestring) {
                safe_copy(sender_id, sizeof(sender_id), union_id->valuestring);
            }
        }
    }

    if (!access_control_is_sender_allowed(sender_id)) {
        ESP_LOGW(TAG, "Blocked Feishu message from sender_id=%s",
                 sender_id[0] ? sender_id : "(unknown)");
        cJSON_Delete(root);
        return feishu_send_http_json(req, NULL, "{\"code\":0}");
    }

    cJSON *message = cJSON_GetObjectItem(event, "message");
    char chat_id[MIMI_CHAT_ID_MAX_LEN] = {0};
    char message_type[16] = {0};
    char text[1024] = {0};
    char media_text[1024] = {0};
    char media_file_id[96] = {0};
    char media_file_path[128] = {0};
    char *media_meta_json = NULL;
    bool media_from_gateway = false;
    if (cJSON_IsObject(message)) {
        cJSON *chat_id_item = cJSON_GetObjectItem(message, "chat_id");
        cJSON *message_type_item = cJSON_GetObjectItem(message, "message_type");
        cJSON *content_item = cJSON_GetObjectItem(message, "content");
        if (cJSON_IsString(chat_id_item) && chat_id_item->valuestring) {
            safe_copy(chat_id, sizeof(chat_id), chat_id_item->valuestring);
        }
        if (cJSON_IsString(message_type_item) && message_type_item->valuestring) {
            safe_copy(message_type, sizeof(message_type), message_type_item->valuestring);
        }
        if (cJSON_IsString(content_item) && content_item->valuestring) {
            feishu_extract_text_from_content(content_item->valuestring, text, sizeof(text));
        }
        if (message_type[0] && strcmp(message_type, "text") != 0) {
            media_from_gateway = feishu_try_build_gateway_media_text(message, message_type,
                                                                     media_text, sizeof(media_text),
                                                                     media_file_id, sizeof(media_file_id),
                                                                     media_file_path, sizeof(media_file_path),
                                                                     &media_meta_json);
            if (!media_from_gateway) {
                feishu_build_media_summary(message, message_type,
                                           media_text, sizeof(media_text),
                                           media_file_id, sizeof(media_file_id),
                                           &media_meta_json);
            }
        }
    }

    cJSON_Delete(root);

    if (!chat_id[0]) {
        free(media_meta_json);
        return feishu_send_http_json(req, NULL, "{\"code\":0}");
    }

    if (strcmp(message_type, "text") != 0) {
        if (media_text[0]) {
            ESP_LOGI(TAG, "Feishu %s %s from %s: %.60s",
                     message_type[0] ? message_type : "media",
                     media_from_gateway ? "gateway_parse" : "summary",
                     chat_id,
                     media_text);
            feishu_push_inbound(chat_id,
                                media_text,
                                feishu_get_bus_media_type(message_type),
                                media_file_id,
                                media_file_path,
                                media_meta_json);
        } else {
            ESP_LOGI(TAG, "Ignore unsupported Feishu message type=%s chat=%s",
                     message_type[0] ? message_type : "(empty)", chat_id);
        }
        free(media_meta_json);
        return feishu_send_http_json(req, NULL, "{\"code\":0}");
    }

    if (strcmp(text, "/start") == 0) {
        free(media_meta_json);
        feishu_send_message(chat_id, FEISHU_START_HELP);
        return feishu_send_http_json(req, NULL, "{\"code\":0}");
    }

    ESP_LOGI(TAG, "Feishu text from %s: %.60s", chat_id, text);
    feishu_push_inbound(chat_id, text, "text", NULL, NULL, NULL);
    free(media_meta_json);
    return feishu_send_http_json(req, NULL, "{\"code\":0}");
}

esp_err_t feishu_bot_init(void)
{
    safe_copy(s_app_id, sizeof(s_app_id), MIMI_SECRET_FEISHU_APP_ID);
    safe_copy(s_app_secret, sizeof(s_app_secret), MIMI_SECRET_FEISHU_APP_SECRET);
    safe_copy(s_verify_token, sizeof(s_verify_token), MIMI_SECRET_FEISHU_VERIFY_TOKEN);
    safe_copy(s_encrypt_key, sizeof(s_encrypt_key), MIMI_SECRET_FEISHU_ENCRYPT_KEY);
    feishu_load_str_from_nvs(MIMI_NVS_KEY_FEISHU_APP_ID, s_app_id, sizeof(s_app_id));
    feishu_load_str_from_nvs(MIMI_NVS_KEY_FEISHU_SECRET, s_app_secret, sizeof(s_app_secret));
    feishu_load_str_from_nvs(MIMI_NVS_KEY_FEISHU_VERIFY, s_verify_token, sizeof(s_verify_token));
    feishu_load_str_from_nvs(MIMI_NVS_KEY_FEISHU_ENCRYPT, s_encrypt_key, sizeof(s_encrypt_key));
    feishu_invalidate_tenant_token();

    if (!feishu_is_configured()) {
        ESP_LOGI(TAG, "Feishu bot disabled: app_id/app_secret not configured");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Feishu bot configured (verify_token=%s, encrypt_key=%s)",
             s_verify_token[0] ? "configured" : "open",
             s_encrypt_key[0] ? "configured" : "open");
    return ESP_OK;
}

esp_err_t feishu_bot_start(void)
{
#if !MIMI_FEISHU_ENABLED
    ESP_LOGI(TAG, "Feishu bot disabled by build flag");
    return ESP_OK;
#else
    if (s_started || !feishu_is_configured()) {
        return ESP_OK;
    }

    httpd_uri_t uri = {
        .uri = MIMI_FEISHU_EVENTS_PATH,
        .method = HTTP_POST,
        .handler = feishu_events_handler,
        .user_ctx = NULL,
    };
    esp_err_t err = ws_server_register_uri(&uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register Feishu callback failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "Feishu callback registered at %s", MIMI_FEISHU_EVENTS_PATH);
    return ESP_OK;
#endif
}

esp_err_t feishu_send_message(const char *chat_id, const char *text)
{
    if (!feishu_is_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!chat_id || !chat_id[0] || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t text_len = strlen(text);
    size_t offset = 0;
    while (offset < text_len) {
        size_t remaining = text_len - offset;
        size_t chunk_len = feishu_choose_chunk_size(text + offset, remaining);
        char *segment = calloc(1, chunk_len + 1);
        if (!segment) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + offset, chunk_len);
        segment[chunk_len] = '\0';

        esp_err_t err = feishu_send_text_once(chat_id, segment, true);
        free(segment);
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk_len;
    }
    return ESP_OK;
}

esp_err_t feishu_set_app_credentials(const char *app_id, const char *app_secret)
{
    if (!app_id || !app_secret ||
        strlen(app_id) >= sizeof(s_app_id) ||
        strlen(app_secret) >= sizeof(s_app_secret)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = feishu_nvs_write_str(MIMI_NVS_KEY_FEISHU_APP_ID, app_id);
    if (err != ESP_OK) {
        return err;
    }
    err = feishu_nvs_write_str(MIMI_NVS_KEY_FEISHU_SECRET, app_secret);
    if (err != ESP_OK) {
        return err;
    }

    safe_copy(s_app_id, sizeof(s_app_id), app_id);
    safe_copy(s_app_secret, sizeof(s_app_secret), app_secret);
    feishu_invalidate_tenant_token();

    err = feishu_bot_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_LOGI(TAG, "Feishu app credentials updated");
    return ESP_OK;
}

esp_err_t feishu_clear_app_credentials(void)
{
    esp_err_t err = feishu_nvs_erase_key(MIMI_NVS_KEY_FEISHU_APP_ID);
    if (err != ESP_OK) {
        return err;
    }
    err = feishu_nvs_erase_key(MIMI_NVS_KEY_FEISHU_SECRET);
    if (err != ESP_OK) {
        return err;
    }

    safe_copy(s_app_id, sizeof(s_app_id), MIMI_SECRET_FEISHU_APP_ID);
    safe_copy(s_app_secret, sizeof(s_app_secret), MIMI_SECRET_FEISHU_APP_SECRET);
    feishu_invalidate_tenant_token();
    if (feishu_is_configured()) {
        err = feishu_bot_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }
    ESP_LOGI(TAG, "Feishu app credentials cleared");
    return ESP_OK;
}

esp_err_t feishu_set_verify_token(const char *token)
{
    if (!token || strlen(token) >= sizeof(s_verify_token)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = feishu_nvs_write_str(MIMI_NVS_KEY_FEISHU_VERIFY, token);
    if (err != ESP_OK) {
        return err;
    }
    safe_copy(s_verify_token, sizeof(s_verify_token), token);
    ESP_LOGI(TAG, "Feishu verify token updated");
    return ESP_OK;
}

esp_err_t feishu_clear_verify_token(void)
{
    esp_err_t err = feishu_nvs_erase_key(MIMI_NVS_KEY_FEISHU_VERIFY);
    if (err != ESP_OK) {
        return err;
    }
    safe_copy(s_verify_token, sizeof(s_verify_token), MIMI_SECRET_FEISHU_VERIFY_TOKEN);
    ESP_LOGI(TAG, "Feishu verify token cleared");
    return ESP_OK;
}

esp_err_t feishu_set_encrypt_key(const char *key)
{
    if (!key || strlen(key) >= sizeof(s_encrypt_key)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = feishu_nvs_write_str(MIMI_NVS_KEY_FEISHU_ENCRYPT, key);
    if (err != ESP_OK) {
        return err;
    }
    safe_copy(s_encrypt_key, sizeof(s_encrypt_key), key);
    ESP_LOGI(TAG, "Feishu encrypt key updated");
    return ESP_OK;
}

esp_err_t feishu_clear_encrypt_key(void)
{
    esp_err_t err = feishu_nvs_erase_key(MIMI_NVS_KEY_FEISHU_ENCRYPT);
    if (err != ESP_OK) {
        return err;
    }
    safe_copy(s_encrypt_key, sizeof(s_encrypt_key), MIMI_SECRET_FEISHU_ENCRYPT_KEY);
    ESP_LOGI(TAG, "Feishu encrypt key cleared");
    return ESP_OK;
}
