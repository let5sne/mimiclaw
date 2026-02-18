#include "control/control_plane.h"
#include "mimi_config.h"
#include "audio/audio.h"
#include "voice/voice_channel.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "control";

typedef esp_err_t (*cap_validate_fn)(const control_command_t *cmd, char *err, size_t err_size);
typedef esp_err_t (*cap_execute_fn)(const control_command_t *cmd, control_result_t *out,
                                    char *err, size_t err_size);

typedef struct {
    control_cmd_type_t cmd_type;
    const char *name;
    uint32_t timeout_ms;
    uint8_t retry_max;
    cap_validate_fn validate;
    cap_execute_fn execute;
} control_capability_t;

typedef struct {
    bool used;
    int64_t ts_ms;
    char request_id[40];
    control_result_t cached_result;
} idemp_entry_t;

typedef struct {
    bool active;
    uint32_t alarm_id;
    int64_t due_ms;
    TimerHandle_t timer;
    char channel[16];
    char chat_id[32];
    char note[96];
} alarm_slot_t;

typedef struct {
    bool active;
    uint32_t rule_id;
    int threshold_x10;
    int comparator;    /* 1: >=, -1: <= */
    int action_type;   /* 1: remind, 2: set_volume */
    int action_value;  /* action_type=2 */
    int64_t last_trigger_ms;
    char note[96];
} temp_rule_slot_t;

static bool s_initialized = false;
static uint32_t s_next_alarm_id = 1;
static uint32_t s_next_temp_rule_id = 1;
static TimerHandle_t s_reboot_timer = NULL;
static idemp_entry_t s_idemp_cache[MIMI_CONTROL_IDEMP_CACHE_SIZE] = {0};
static control_audit_entry_t s_audits[MIMI_CONTROL_AUDIT_SIZE] = {0};
static size_t s_audit_head = 0;
static size_t s_audit_count = 0;
static alarm_slot_t s_alarms[MIMI_CONTROL_MAX_ALARMS] = {0};
static temp_rule_slot_t s_temp_rules[MIMI_CONTROL_MAX_TEMP_RULES] = {0};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static inline int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool contains_any(const char *text, const char *const keywords[], size_t count)
{
    if (!text || !text[0]) return false;
    for (size_t i = 0; i < count; i++) {
        if (keywords[i] && keywords[i][0] && strstr(text, keywords[i])) {
            return true;
        }
    }
    return false;
}

static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static void append_audit(const control_result_t *result, const char *summary)
{
    if (!result) return;

    control_audit_entry_t entry = {0};
    entry.ts_ms = now_ms();
    entry.success = result->success;
    entry.dedup_hit = result->dedup_hit;
    strncpy(entry.request_id, result->request_id, sizeof(entry.request_id) - 1);
    strncpy(entry.capability, result->capability, sizeof(entry.capability) - 1);
    if (summary && summary[0]) {
        strncpy(entry.summary, summary, sizeof(entry.summary) - 1);
    } else {
        snprintf(entry.summary, sizeof(entry.summary), "handled=%d success=%d",
                 result->handled ? 1 : 0, result->success ? 1 : 0);
    }

    portENTER_CRITICAL(&s_lock);
    s_audits[s_audit_head] = entry;
    s_audit_head = (s_audit_head + 1U) % MIMI_CONTROL_AUDIT_SIZE;
    if (s_audit_count < MIMI_CONTROL_AUDIT_SIZE) {
        s_audit_count++;
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool decode_utf8_cp(const char *s, int *cp, int *bytes)
{
    if (!s || !s[0]) return false;
    unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) {
        *cp = c0;
        *bytes = 1;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0) {
        unsigned char c1 = (unsigned char)s[1];
        if ((c1 & 0xC0) != 0x80) return false;
        *cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
        *bytes = 2;
        return true;
    }
    if ((c0 & 0xF0) == 0xE0) {
        unsigned char c1 = (unsigned char)s[1];
        unsigned char c2 = (unsigned char)s[2];
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
        *cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
        *bytes = 3;
        return true;
    }
    if ((c0 & 0xF8) == 0xF0) {
        unsigned char c1 = (unsigned char)s[1];
        unsigned char c2 = (unsigned char)s[2];
        unsigned char c3 = (unsigned char)s[3];
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
        *cp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        *bytes = 4;
        return true;
    }
    return false;
}

static int zh_digit_value(int cp)
{
    switch (cp) {
    case 0x96F6: /* 零 */
    case 0x3007: /* 〇 */
        return 0;
    case 0x4E00: /* 一 */
        return 1;
    case 0x4E8C: /* 二 */
    case 0x4E24: /* 两 */
        return 2;
    case 0x4E09: /* 三 */
        return 3;
    case 0x56DB: /* 四 */
        return 4;
    case 0x4E94: /* 五 */
        return 5;
    case 0x516D: /* 六 */
        return 6;
    case 0x4E03: /* 七 */
        return 7;
    case 0x516B: /* 八 */
        return 8;
    case 0x4E5D: /* 九 */
        return 9;
    default:
        return -1;
    }
}

static int zh_unit_value(int cp)
{
    if (cp == 0x5341) return 10;   /* 十 */
    if (cp == 0x767E) return 100;  /* 百 */
    return 0;
}

static bool parse_int_ascii(const char *s, int *value, int *consumed)
{
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    int start = i;
    int v = 0;
    while (isdigit((unsigned char)s[i])) {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    if (i == start) return false;
    *value = v;
    *consumed = i;
    return true;
}

static bool parse_int_zh(const char *s, int *value, int *consumed)
{
    int i = 0;
    int result = 0;
    int current = 0;
    bool seen = false;

    while (s[i]) {
        int cp = 0, bytes = 0;
        if (!decode_utf8_cp(s + i, &cp, &bytes)) break;

        int digit = zh_digit_value(cp);
        if (digit >= 0) {
            current = digit;
            seen = true;
            i += bytes;
            continue;
        }

        int unit = zh_unit_value(cp);
        if (unit > 0) {
            if (!seen || current == 0) current = 1;
            result += current * unit;
            current = 0;
            seen = true;
            i += bytes;
            continue;
        }
        break;
    }

    if (!seen) return false;
    result += current;
    *value = result;
    *consumed = i;
    return true;
}

static bool parse_number_token(const char *s, int *value, int *consumed)
{
    int v = 0, n = 0;
    if (parse_int_ascii(s, &v, &n)) {
        *value = v;
        *consumed = n;
        return true;
    }
    if (parse_int_zh(s, &v, &n)) {
        *value = v;
        *consumed = n;
        return true;
    }
    return false;
}

static int parse_last_number_before(const char *text, const char *keyword)
{
    if (!text || !keyword) return -1;
    const char *pos = strstr(text, keyword);
    if (!pos) return -1;

    int last = -1;
    for (const char *p = text; p < pos; p++) {
        int v = 0, n = 0;
        if (!parse_number_token(p, &v, &n)) continue;
        if (p + n <= pos) {
            last = v;
            p += (n - 1);
        }
    }
    return last;
}

static bool parse_percent_value(const char *text, int *value)
{
    const char *p = strstr(text, "百分之");
    if (p) {
        p += strlen("百分之");
        int v = 0, n = 0;
        if (parse_number_token(p, &v, &n)) {
            *value = v;
            return true;
        }
    }

    for (const char *cur = text; cur && *cur; cur++) {
        int v = 0, n = 0;
        if (!parse_int_ascii(cur, &v, &n)) continue;
        const char *after = cur + n;
        while (*after == ' ') after++;
        if (*after == '%') {
            *value = v;
            return true;
        }
    }

    for (const char *cur = text; cur && *cur; cur++) {
        int v = 0, n = 0;
        if (parse_int_ascii(cur, &v, &n)) {
            *value = v;
            return true;
        }
    }
    return false;
}

static void trim_ascii_inplace(char *s)
{
    if (!s || s[0] == '\0') return;
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
            c == '.' || c == '!' || c == '?') {
            s[n - 1] = '\0';
            n--;
        } else {
            break;
        }
    }
    while (n >= 3) {
        if (strcmp(s + n - 3, "。") == 0 ||
            strcmp(s + n - 3, "！") == 0 ||
            strcmp(s + n - 3, "？") == 0) {
            s[n - 3] = '\0';
            n -= 3;
            continue;
        }
        break;
    }
}

static bool parse_temperature_threshold_x10(const char *text, int *threshold_x10)
{
    if (!text || !threshold_x10) return false;
    int celsius = parse_last_number_before(text, "摄氏度");
    if (celsius < 0) celsius = parse_last_number_before(text, "度");
    if (celsius < 0) celsius = parse_last_number_before(text, "℃");
    if (celsius < 0) return false;
    *threshold_x10 = celsius * 10;
    return true;
}

static void build_request_id(const mimi_msg_t *msg, char *out_id, size_t out_size)
{
    if (!msg || !out_id || out_size == 0) return;
    out_id[0] = '\0';

    if (msg->meta_json && msg->meta_json[0]) {
        cJSON *meta = cJSON_Parse(msg->meta_json);
        if (meta) {
            const char *rid = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "request_id"));
            if (rid && rid[0]) {
                strncpy(out_id, rid, out_size - 1);
                out_id[out_size - 1] = '\0';
            }
            cJSON_Delete(meta);
        }
    }
    if (out_id[0]) return;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s|%s|%s|%s",
             msg->channel, msg->chat_id,
             msg->media_type[0] ? msg->media_type : "text",
             msg->content ? msg->content : "");
    uint32_t hash = fnv1a32(buf);
    snprintf(out_id, out_size, "auto-%08" PRIx32, hash);
}

static bool idemp_lookup(const char *request_id, control_result_t *out)
{
    if (!request_id || !request_id[0] || !out) return false;
    int64_t now = now_ms();

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < MIMI_CONTROL_IDEMP_CACHE_SIZE; i++) {
        const idemp_entry_t *e = &s_idemp_cache[i];
        if (!e->used) continue;
        if (strcmp(e->request_id, request_id) != 0) continue;
        if (now - e->ts_ms > MIMI_CONTROL_IDEMP_WINDOW_MS) continue;
        *out = e->cached_result;
        out->dedup_hit = true;
        portEXIT_CRITICAL(&s_lock);
        return true;
    }
    portEXIT_CRITICAL(&s_lock);
    return false;
}

static void idemp_store(const char *request_id, const control_result_t *result)
{
    if (!request_id || !request_id[0] || !result) return;
    size_t slot = 0;
    int64_t oldest = INT64_MAX;
    int64_t now = now_ms();

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < MIMI_CONTROL_IDEMP_CACHE_SIZE; i++) {
        if (!s_idemp_cache[i].used) {
            slot = i;
            oldest = INT64_MIN;
            break;
        }
        if (s_idemp_cache[i].ts_ms < oldest) {
            oldest = s_idemp_cache[i].ts_ms;
            slot = i;
        }
    }

    s_idemp_cache[slot].used = true;
    s_idemp_cache[slot].ts_ms = now;
    strncpy(s_idemp_cache[slot].request_id, request_id, sizeof(s_idemp_cache[slot].request_id) - 1);
    s_idemp_cache[slot].cached_result = *result;
    portEXIT_CRITICAL(&s_lock);
}

static void reboot_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGW(TAG, "Reboot timer fired");
    esp_restart();
}

static void alarm_timer_cb(TimerHandle_t timer)
{
    alarm_slot_t *slot = (alarm_slot_t *)pvTimerGetTimerID(timer);
    if (!slot) return;

    char channel[16] = {0};
    char chat_id[32] = {0};
    char note[96] = {0};
    uint32_t alarm_id = 0;

    portENTER_CRITICAL(&s_lock);
    if (slot->active) {
        alarm_id = slot->alarm_id;
        strncpy(channel, slot->channel, sizeof(channel) - 1);
        strncpy(chat_id, slot->chat_id, sizeof(chat_id) - 1);
        strncpy(note, slot->note, sizeof(note) - 1);
        slot->active = false;
        slot->alarm_id = 0;
        slot->due_ms = 0;
        slot->timer = NULL;
        slot->channel[0] = '\0';
        slot->chat_id[0] = '\0';
        slot->note[0] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);

    if (alarm_id == 0) {
        return;
    }

    mimi_msg_t msg = {0};
    strncpy(msg.channel, channel[0] ? channel : MIMI_CHAN_SYSTEM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id[0] ? chat_id : "alarm", sizeof(msg.chat_id) - 1);
    msg.content = malloc(192);
    if (!msg.content) {
        ESP_LOGE(TAG, "Alarm(%u) malloc failed", alarm_id);
        return;
    }
    snprintf(msg.content, 192, "闹钟提醒：%s", note[0] ? note : "时间到了。");

    if (message_bus_push_outbound(&msg) != ESP_OK) {
        ESP_LOGW(TAG, "Alarm(%u) outbound enqueue failed", alarm_id);
        message_bus_msg_free(&msg);
    } else {
        ESP_LOGI(TAG, "Alarm fired: id=%u channel=%s chat=%s", alarm_id, msg.channel, msg.chat_id);
    }
    xTimerDelete(timer, 0);
}

static esp_err_t validate_get_volume(const control_command_t *cmd, char *err, size_t err_size)
{
    (void)cmd;
    (void)err;
    (void)err_size;
    return ESP_OK;
}

static esp_err_t execute_get_volume(const control_command_t *cmd, control_result_t *out,
                                    char *err, size_t err_size)
{
    (void)cmd;
    (void)err;
    (void)err_size;
    out->before_value = audio_get_volume();
    out->after_value = out->before_value;
    snprintf(out->response_text, sizeof(out->response_text), "当前音量是百分之%d。", out->after_value);
    return ESP_OK;
}

static esp_err_t validate_set_volume(const control_command_t *cmd, char *err, size_t err_size)
{
    if (cmd->target_value < 0 || cmd->target_value > 100) {
        snprintf(err, err_size, "目标音量超出范围(0-100): %d", cmd->target_value);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t execute_set_volume(const control_command_t *cmd, control_result_t *out,
                                    char *err, size_t err_size)
{
    out->before_value = audio_get_volume();
    audio_set_volume((uint8_t)cmd->target_value);
    out->after_value = audio_get_volume();
    if (out->after_value != cmd->target_value) {
        snprintf(err, err_size, "写入后回读不一致: expect=%d actual=%d",
                 cmd->target_value, out->after_value);
        return ESP_FAIL;
    }

    if (cmd->relative) {
        const char *verb = cmd->delta_value >= 0 ? "增大" : "减小";
        int delta = cmd->delta_value >= 0 ? cmd->delta_value : -cmd->delta_value;
        snprintf(out->response_text, sizeof(out->response_text),
                 "已将音量%s百分之%d，当前为百分之%d。", verb, delta, out->after_value);
    } else {
        snprintf(out->response_text, sizeof(out->response_text),
                 "音量已设置为百分之%d。", out->after_value);
    }
    return ESP_OK;
}

static esp_err_t validate_reboot(const control_command_t *cmd, char *err, size_t err_size)
{
    if (cmd->delay_ms < 500 || cmd->delay_ms > 3600U * 1000U) {
        snprintf(err, err_size, "重启延迟非法: %" PRIu32 "ms", cmd->delay_ms);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t execute_reboot(const control_command_t *cmd, control_result_t *out,
                                char *err, size_t err_size)
{
    (void)err;
    (void)err_size;
    if (s_reboot_timer) {
        xTimerStop(s_reboot_timer, 0);
        xTimerDelete(s_reboot_timer, 0);
        s_reboot_timer = NULL;
    }

    TickType_t ticks = pdMS_TO_TICKS(cmd->delay_ms);
    if (ticks == 0) ticks = 1;
    s_reboot_timer = xTimerCreate("ctrl_reboot", ticks, pdFALSE, NULL, reboot_timer_cb);
    if (!s_reboot_timer) {
        return ESP_ERR_NO_MEM;
    }
    if (xTimerStart(s_reboot_timer, 0) != pdPASS) {
        xTimerDelete(s_reboot_timer, 0);
        s_reboot_timer = NULL;
        return ESP_FAIL;
    }

    out->pending_action = true;
    snprintf(out->response_text, sizeof(out->response_text),
             "设备将在%.1f秒后重启。", cmd->delay_ms / 1000.0f);
    return ESP_OK;
}

static esp_err_t validate_alarm_create(const control_command_t *cmd, char *err, size_t err_size)
{
    if (cmd->delay_ms < 1000 || cmd->delay_ms > 24U * 3600U * 1000U) {
        snprintf(err, err_size, "闹钟延迟非法: %" PRIu32 "ms", cmd->delay_ms);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t execute_alarm_create(const control_command_t *cmd, control_result_t *out,
                                      char *err, size_t err_size)
{
    alarm_slot_t *slot = NULL;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < MIMI_CONTROL_MAX_ALARMS; i++) {
        if (!s_alarms[i].active) {
            slot = &s_alarms[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (!slot) {
        snprintf(err, err_size, "闹钟已满，最多%d个", MIMI_CONTROL_MAX_ALARMS);
        return ESP_ERR_NO_MEM;
    }

    TickType_t ticks = pdMS_TO_TICKS(cmd->delay_ms);
    if (ticks == 0) ticks = 1;
    TimerHandle_t timer = xTimerCreate("ctrl_alarm", ticks, pdFALSE, slot, alarm_timer_cb);
    if (!timer) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lock);
    uint32_t alarm_id = s_next_alarm_id++;
    if (s_next_alarm_id == 0) s_next_alarm_id = 1;
    slot->active = true;
    slot->alarm_id = alarm_id;
    slot->due_ms = now_ms() + cmd->delay_ms;
    slot->timer = timer;
    strncpy(slot->channel, cmd->source_channel, sizeof(slot->channel) - 1);
    strncpy(slot->chat_id, cmd->source_chat_id, sizeof(slot->chat_id) - 1);
    strncpy(slot->note, cmd->note, sizeof(slot->note) - 1);
    portEXIT_CRITICAL(&s_lock);

    if (xTimerStart(timer, 0) != pdPASS) {
        xTimerDelete(timer, 0);
        portENTER_CRITICAL(&s_lock);
        slot->active = false;
        slot->alarm_id = 0;
        slot->due_ms = 0;
        slot->timer = NULL;
        slot->channel[0] = '\0';
        slot->chat_id[0] = '\0';
        slot->note[0] = '\0';
        portEXIT_CRITICAL(&s_lock);
        return ESP_FAIL;
    }

    out->pending_action = true;
    snprintf(out->response_text, sizeof(out->response_text),
             "已创建闹钟#%" PRIu32 "，%.1f秒后提醒你。", alarm_id, cmd->delay_ms / 1000.0f);
    return ESP_OK;
}

static esp_err_t validate_alarm_list(const control_command_t *cmd, char *err, size_t err_size)
{
    (void)cmd;
    (void)err;
    (void)err_size;
    return ESP_OK;
}

static esp_err_t execute_alarm_list(const control_command_t *cmd, control_result_t *out,
                                    char *err, size_t err_size)
{
    (void)cmd;
    (void)err;
    (void)err_size;
    control_alarm_info_t infos[MIMI_CONTROL_MAX_ALARMS];
    size_t count = control_plane_get_active_alarms(infos, MIMI_CONTROL_MAX_ALARMS);
    if (count == 0) {
        snprintf(out->response_text, sizeof(out->response_text), "当前没有活动闹钟。");
        return ESP_OK;
    }

    int written = snprintf(out->response_text, sizeof(out->response_text),
                           "当前有%u个闹钟：", (unsigned)count);
    for (size_t i = 0; i < count && written > 0 && (size_t)written < sizeof(out->response_text); i++) {
        int sec = (int)((infos[i].remaining_ms + 999U) / 1000U);
        int n = snprintf(out->response_text + written, sizeof(out->response_text) - (size_t)written,
                         "#%" PRIu32 "(%ds)", infos[i].alarm_id, sec);
        if (n <= 0) break;
        written += n;
        if (i + 1 < count && (size_t)written < sizeof(out->response_text) - 2U) {
            out->response_text[written++] = ' ';
            out->response_text[written] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t validate_alarm_clear(const control_command_t *cmd, char *err, size_t err_size)
{
    (void)cmd;
    (void)err;
    (void)err_size;
    return ESP_OK;
}

static esp_err_t execute_alarm_clear(const control_command_t *cmd, control_result_t *out,
                                     char *err, size_t err_size)
{
    uint32_t target_id = cmd->alarm_id;
    size_t cleared = 0;

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < MIMI_CONTROL_MAX_ALARMS; i++) {
        alarm_slot_t *slot = &s_alarms[i];
        if (!slot->active) continue;
        if (target_id != 0 && slot->alarm_id != target_id) continue;

        TimerHandle_t timer = slot->timer;
        slot->active = false;
        slot->alarm_id = 0;
        slot->due_ms = 0;
        slot->timer = NULL;
        slot->channel[0] = '\0';
        slot->chat_id[0] = '\0';
        slot->note[0] = '\0';
        cleared++;
        if (timer) {
            xTimerStop(timer, 0);
            xTimerDelete(timer, 0);
        }
        if (target_id != 0) {
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (target_id != 0 && cleared == 0) {
        snprintf(err, err_size, "未找到闹钟#%" PRIu32, target_id);
        return ESP_ERR_NOT_FOUND;
    }
    if (target_id == 0 && cleared == 0) {
        snprintf(out->response_text, sizeof(out->response_text), "当前没有活动闹钟。");
        return ESP_OK;
    }
    if (target_id != 0) {
        snprintf(out->response_text, sizeof(out->response_text), "已取消闹钟#%" PRIu32 "。", target_id);
    } else {
        snprintf(out->response_text, sizeof(out->response_text), "已取消全部闹钟（%u个）。",
                 (unsigned)cleared);
    }
    return ESP_OK;
}

static esp_err_t validate_temp_rule_create(const control_command_t *cmd, char *err, size_t err_size)
{
    if (cmd->temp_threshold_x10 < -500 || cmd->temp_threshold_x10 > 1200) {
        snprintf(err, err_size, "温度阈值超出范围(-50.0~120.0°C): %d.%d",
                 cmd->temp_threshold_x10 / 10, abs(cmd->temp_threshold_x10 % 10));
        return ESP_ERR_INVALID_ARG;
    }
    if (cmd->temp_comparator != 1 && cmd->temp_comparator != -1) {
        snprintf(err, err_size, "温度比较符无效");
        return ESP_ERR_INVALID_ARG;
    }
    if (cmd->temp_action_type != 1 && cmd->temp_action_type != 2) {
        snprintf(err, err_size, "温度动作类型无效");
        return ESP_ERR_INVALID_ARG;
    }
    if (cmd->temp_action_type == 2 &&
        (cmd->temp_action_value < 0 || cmd->temp_action_value > 100)) {
        snprintf(err, err_size, "目标音量无效: %d", cmd->temp_action_value);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t execute_temp_rule_create(const control_command_t *cmd, control_result_t *out,
                                          char *err, size_t err_size)
{
    temp_rule_slot_t *slot = NULL;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < MIMI_CONTROL_MAX_TEMP_RULES; i++) {
        if (!s_temp_rules[i].active) {
            slot = &s_temp_rules[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (!slot) {
        snprintf(err, err_size, "温度规则已满，最多%d条", MIMI_CONTROL_MAX_TEMP_RULES);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lock);
    uint32_t rule_id = s_next_temp_rule_id++;
    if (s_next_temp_rule_id == 0) s_next_temp_rule_id = 1;
    slot->active = true;
    slot->rule_id = rule_id;
    slot->threshold_x10 = cmd->temp_threshold_x10;
    slot->comparator = cmd->temp_comparator;
    slot->action_type = cmd->temp_action_type;
    slot->action_value = cmd->temp_action_value;
    slot->last_trigger_ms = 0;
    strncpy(slot->note, cmd->note, sizeof(slot->note) - 1);
    portEXIT_CRITICAL(&s_lock);

    const char *cmp = (slot->comparator == 1) ? ">=" : "<=";
    if (slot->action_type == 2) {
        snprintf(out->response_text, sizeof(out->response_text),
                 "已创建温度规则#%" PRIu32 "：温度%s%d.%d°C时，音量设为%d%%。",
                 rule_id, cmp, slot->threshold_x10 / 10, abs(slot->threshold_x10 % 10),
                 slot->action_value);
    } else {
        snprintf(out->response_text, sizeof(out->response_text),
                 "已创建温度规则#%" PRIu32 "：温度%s%d.%d°C时提醒“%s”。",
                 rule_id, cmp, slot->threshold_x10 / 10, abs(slot->threshold_x10 % 10),
                 slot->note[0] ? slot->note : "温度事件触发");
    }
    return ESP_OK;
}

static esp_err_t validate_temp_rule_list(const control_command_t *cmd, char *err, size_t err_size)
{
    (void)cmd;
    (void)err;
    (void)err_size;
    return ESP_OK;
}

static esp_err_t execute_temp_rule_list(const control_command_t *cmd, control_result_t *out,
                                        char *err, size_t err_size)
{
    (void)cmd;
    (void)err;
    (void)err_size;

    control_temp_rule_info_t rules[MIMI_CONTROL_MAX_TEMP_RULES];
    size_t count = control_plane_get_temperature_rules(rules, MIMI_CONTROL_MAX_TEMP_RULES);
    if (count == 0) {
        snprintf(out->response_text, sizeof(out->response_text), "当前没有温度规则。");
        return ESP_OK;
    }

    int written = snprintf(out->response_text, sizeof(out->response_text),
                           "当前有%u条温度规则：", (unsigned)count);
    for (size_t i = 0; i < count && written > 0 && (size_t)written < sizeof(out->response_text); i++) {
        const control_temp_rule_info_t *r = &rules[i];
        const char *cmp = (r->comparator == 1) ? ">=" : "<=";
        int n = 0;
        if (r->action_type == 2) {
            n = snprintf(out->response_text + written, sizeof(out->response_text) - (size_t)written,
                         "#%" PRIu32 "(%s%d.%d°C->%d%%)",
                         r->rule_id, cmp, r->threshold_x10 / 10, abs(r->threshold_x10 % 10),
                         r->action_value);
        } else {
            n = snprintf(out->response_text + written, sizeof(out->response_text) - (size_t)written,
                         "#%" PRIu32 "(%s%d.%d°C->提醒)",
                         r->rule_id, cmp, r->threshold_x10 / 10, abs(r->threshold_x10 % 10));
        }
        if (n <= 0) break;
        written += n;
        if (i + 1 < count && (size_t)written < sizeof(out->response_text) - 2U) {
            out->response_text[written++] = ' ';
            out->response_text[written] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t validate_temp_rule_clear(const control_command_t *cmd, char *err, size_t err_size)
{
    (void)cmd;
    (void)err;
    (void)err_size;
    return ESP_OK;
}

static esp_err_t execute_temp_rule_clear(const control_command_t *cmd, control_result_t *out,
                                         char *err, size_t err_size)
{
    uint32_t target_id = cmd->temp_rule_id;
    size_t cleared = 0;

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < MIMI_CONTROL_MAX_TEMP_RULES; i++) {
        temp_rule_slot_t *slot = &s_temp_rules[i];
        if (!slot->active) continue;
        if (target_id != 0 && slot->rule_id != target_id) continue;

        slot->active = false;
        slot->rule_id = 0;
        slot->threshold_x10 = 0;
        slot->comparator = 0;
        slot->action_type = 0;
        slot->action_value = 0;
        slot->last_trigger_ms = 0;
        slot->note[0] = '\0';
        cleared++;
        if (target_id != 0) break;
    }
    portEXIT_CRITICAL(&s_lock);

    if (target_id != 0 && cleared == 0) {
        snprintf(err, err_size, "未找到温度规则#%" PRIu32, target_id);
        return ESP_ERR_NOT_FOUND;
    }
    if (target_id == 0 && cleared == 0) {
        snprintf(out->response_text, sizeof(out->response_text), "当前没有温度规则。");
        return ESP_OK;
    }
    if (target_id != 0) {
        snprintf(out->response_text, sizeof(out->response_text), "已删除温度规则#%" PRIu32 "。", target_id);
    } else {
        snprintf(out->response_text, sizeof(out->response_text), "已清空温度规则（%u条）。",
                 (unsigned)cleared);
    }
    return ESP_OK;
}

static esp_err_t validate_play_music(const control_command_t *cmd, char *err, size_t err_size)
{
    if (!cmd->note[0]) {
        snprintf(err, err_size, "音乐内容为空");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t execute_play_music(const control_command_t *cmd, control_result_t *out,
                                    char *err, size_t err_size)
{
    esp_err_t ret = voice_channel_play_music(cmd->note);
    if (ret != ESP_OK) {
        snprintf(err, err_size, "播放音乐失败: %s", esp_err_to_name(ret));
        return ret;
    }
    out->pending_action = true;
    out->response_text[0] = '\0';  /* 语音通道静默返回，避免打断音乐播放 */
    return ESP_OK;
}

static esp_err_t validate_stop_music(const control_command_t *cmd, char *err, size_t err_size)
{
    (void)cmd;
    (void)err;
    (void)err_size;
    return ESP_OK;
}

static esp_err_t execute_stop_music(const control_command_t *cmd, control_result_t *out,
                                    char *err, size_t err_size)
{
    (void)cmd;
    esp_err_t ret = voice_channel_stop_music();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        snprintf(err, err_size, "停止音乐失败: %s", esp_err_to_name(ret));
        return ret;
    }
    snprintf(out->response_text, sizeof(out->response_text), "已停止音乐播放。");
    return ESP_OK;
}

static const control_capability_t s_capabilities[] = {
    {
        .cmd_type = CONTROL_CMD_GET_VOLUME,
        .name = "get_volume",
        .timeout_ms = 500,
        .retry_max = 0,
        .validate = validate_get_volume,
        .execute = execute_get_volume,
    },
    {
        .cmd_type = CONTROL_CMD_SET_VOLUME,
        .name = "set_volume",
        .timeout_ms = 500,
        .retry_max = 0,
        .validate = validate_set_volume,
        .execute = execute_set_volume,
    },
    {
        .cmd_type = CONTROL_CMD_REBOOT,
        .name = "reboot",
        .timeout_ms = 1000,
        .retry_max = 0,
        .validate = validate_reboot,
        .execute = execute_reboot,
    },
    {
        .cmd_type = CONTROL_CMD_CREATE_ALARM,
        .name = "alarm_create",
        .timeout_ms = 1000,
        .retry_max = 0,
        .validate = validate_alarm_create,
        .execute = execute_alarm_create,
    },
    {
        .cmd_type = CONTROL_CMD_LIST_ALARM,
        .name = "alarm_list",
        .timeout_ms = 500,
        .retry_max = 0,
        .validate = validate_alarm_list,
        .execute = execute_alarm_list,
    },
    {
        .cmd_type = CONTROL_CMD_CLEAR_ALARM,
        .name = "alarm_clear",
        .timeout_ms = 1000,
        .retry_max = 0,
        .validate = validate_alarm_clear,
        .execute = execute_alarm_clear,
    },
    {
        .cmd_type = CONTROL_CMD_CREATE_TEMP_RULE,
        .name = "temp_rule_create",
        .timeout_ms = 1000,
        .retry_max = 0,
        .validate = validate_temp_rule_create,
        .execute = execute_temp_rule_create,
    },
    {
        .cmd_type = CONTROL_CMD_LIST_TEMP_RULE,
        .name = "temp_rule_list",
        .timeout_ms = 500,
        .retry_max = 0,
        .validate = validate_temp_rule_list,
        .execute = execute_temp_rule_list,
    },
    {
        .cmd_type = CONTROL_CMD_CLEAR_TEMP_RULE,
        .name = "temp_rule_clear",
        .timeout_ms = 1000,
        .retry_max = 0,
        .validate = validate_temp_rule_clear,
        .execute = execute_temp_rule_clear,
    },
    {
        .cmd_type = CONTROL_CMD_PLAY_MUSIC,
        .name = "play_music",
        .timeout_ms = 1000,
        .retry_max = 0,
        .validate = validate_play_music,
        .execute = execute_play_music,
    },
    {
        .cmd_type = CONTROL_CMD_STOP_MUSIC,
        .name = "stop_music",
        .timeout_ms = 1000,
        .retry_max = 0,
        .validate = validate_stop_music,
        .execute = execute_stop_music,
    },
};

static const control_capability_t *find_capability(control_cmd_type_t type)
{
    for (size_t i = 0; i < sizeof(s_capabilities) / sizeof(s_capabilities[0]); i++) {
        if (s_capabilities[i].cmd_type == type) {
            return &s_capabilities[i];
        }
    }
    return NULL;
}

static void init_command_common(const mimi_msg_t *msg, control_command_t *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    strncpy(cmd->source_channel, msg->channel, sizeof(cmd->source_channel) - 1);
    strncpy(cmd->source_chat_id, msg->chat_id, sizeof(cmd->source_chat_id) - 1);
    build_request_id(msg, cmd->request_id, sizeof(cmd->request_id));
}

static bool parse_volume_command(const mimi_msg_t *msg, control_command_t *out,
                                 char *reason, size_t reason_size)
{
    if (!msg || !msg->content || !out) return false;
    const char *text = msg->content;
    if (!strstr(text, "音量")) return false;

    static const char *const conceptual_keywords[] = {
        "什么是音量", "音量是什么", "音量原理", "音量单位", "音量概念"
    };
    if (contains_any(text, conceptual_keywords, sizeof(conceptual_keywords) / sizeof(conceptual_keywords[0]))) {
        return false;
    }

    static const char *const query_keywords[] = {
        "多少", "几", "当前", "现在", "查询", "查看", "告诉我", "是多少", "啥", "?"
    };
    static const char *const absolute_keywords[] = {
        "调到", "调成", "设置", "设为", "改到", "改成", "变成", "开到"
    };
    static const char *const increase_keywords[] = {
        "增大", "增加", "调大", "大一点", "提高", "升高"
    };
    static const char *const decrease_keywords[] = {
        "减小", "减少", "调小", "小一点", "降低", "调低"
    };

    bool ask_query = contains_any(text, query_keywords, sizeof(query_keywords) / sizeof(query_keywords[0]));
    bool is_absolute = contains_any(text, absolute_keywords, sizeof(absolute_keywords) / sizeof(absolute_keywords[0]));
    bool is_increase = contains_any(text, increase_keywords, sizeof(increase_keywords) / sizeof(increase_keywords[0]));
    bool is_decrease = contains_any(text, decrease_keywords, sizeof(decrease_keywords) / sizeof(decrease_keywords[0]));
    bool has_adjust_verb = is_absolute || is_increase || is_decrease ||
                           strstr(text, "静音") || strstr(text, "最大") || strstr(text, "最小");

    init_command_common(msg, out);
    if (!has_adjust_verb && ask_query) {
        out->type = CONTROL_CMD_GET_VOLUME;
        strncpy(out->capability, "get_volume", sizeof(out->capability) - 1);
        return true;
    }
    if (!has_adjust_verb) {
        return false;
    }

    out->type = CONTROL_CMD_SET_VOLUME;
    strncpy(out->capability, "set_volume", sizeof(out->capability) - 1);

    if (strstr(text, "静音") || strstr(text, "最小")) {
        out->target_value = 0;
        return true;
    }
    if (strstr(text, "最大")) {
        out->target_value = 100;
        return true;
    }

    int value = 0;
    bool has_value = parse_percent_value(text, &value);
    if (!has_value && (is_increase || is_decrease)) {
        value = 10;
        has_value = true;
    }
    if (!has_value) {
        snprintf(reason, reason_size, "未识别到目标音量，请说例如“调到30%%”或“减小10%%”。");
        return true;
    }

    if (is_increase || is_decrease) {
        int base = audio_get_volume();
        int delta = clamp_int(value, 0, 100);
        int target = is_increase ? (base + delta) : (base - delta);
        out->relative = true;
        out->delta_value = is_increase ? delta : -delta;
        out->target_value = clamp_int(target, 0, 100);
    } else {
        out->target_value = clamp_int(value, 0, 100);
    }
    return true;
}

static bool parse_reboot_command(const mimi_msg_t *msg, control_command_t *out)
{
    if (!msg || !msg->content || !out) return false;
    const char *text = msg->content;
    if (!strstr(text, "重启")) return false;
    if (strstr(text, "不要重启")) return false;

    init_command_common(msg, out);
    out->type = CONTROL_CMD_REBOOT;
    strncpy(out->capability, "reboot", sizeof(out->capability) - 1);

    int minutes = parse_last_number_before(text, "分钟后");
    int seconds = parse_last_number_before(text, "秒后");
    if (minutes > 0) {
        out->delay_ms = (uint32_t)minutes * 60U * 1000U;
    } else if (seconds > 0) {
        out->delay_ms = (uint32_t)seconds * 1000U;
    } else {
        out->delay_ms = 2000U;
    }
    return true;
}

static bool parse_alarm_command(const mimi_msg_t *msg, control_command_t *out)
{
    if (!msg || !msg->content || !out) return false;
    const char *text = msg->content;
    bool has_alarm = strstr(text, "闹钟") || strstr(text, "提醒");
    if (!has_alarm) return false;

    init_command_common(msg, out);
    strncpy(out->capability, "alarm", sizeof(out->capability) - 1);

    if (strstr(text, "查看闹钟") || strstr(text, "闹钟列表") || strstr(text, "还有几个闹钟")) {
        out->type = CONTROL_CMD_LIST_ALARM;
        return true;
    }
    if (strstr(text, "取消闹钟") || strstr(text, "清空闹钟") || strstr(text, "删除闹钟")) {
        out->type = CONTROL_CMD_CLEAR_ALARM;
        int alarm_id = parse_last_number_before(text, "闹钟");
        if (alarm_id > 0) {
            out->alarm_id = (uint32_t)alarm_id;
        } else {
            out->alarm_id = 0;
        }
        return true;
    }

    int minutes = parse_last_number_before(text, "分钟后");
    int seconds = parse_last_number_before(text, "秒后");
    if (minutes <= 0 && seconds <= 0) {
        return false;
    }

    out->type = CONTROL_CMD_CREATE_ALARM;
    strncpy(out->capability, "alarm_create", sizeof(out->capability) - 1);
    if (minutes > 0) {
        out->delay_ms = (uint32_t)minutes * 60U * 1000U;
    } else {
        out->delay_ms = (uint32_t)seconds * 1000U;
    }

    const char *p = strstr(text, "提醒");
    if (p) {
        p += strlen("提醒");
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "我", strlen("我")) == 0) {
            p += strlen("我");
        }
        while (*p == ' ' || *p == '\t') p++;
    } else {
        p = text;
    }
    if (p && *p) {
        strncpy(out->note, p, sizeof(out->note) - 1);
    } else {
        strncpy(out->note, "时间到了。", sizeof(out->note) - 1);
    }
    return true;
}

static bool parse_temp_rule_command(const mimi_msg_t *msg, control_command_t *out,
                                    char *reason, size_t reason_size)
{
    if (!msg || !msg->content || !out) return false;
    const char *text = msg->content;
    if (!strstr(text, "温度")) return false;

    bool list_rule = strstr(text, "温度规则") &&
                     (strstr(text, "查看") || strstr(text, "列表") || strstr(text, "多少"));
    bool clear_rule = strstr(text, "温度规则") &&
                      (strstr(text, "清空") || strstr(text, "删除") || strstr(text, "取消"));
    bool set_rule = (strstr(text, "规则") || strstr(text, "温度")) &&
                    (strstr(text, "高于") || strstr(text, "超过") || strstr(text, "大于") ||
                     strstr(text, "低于") || strstr(text, "小于") || strstr(text, "不高于") ||
                     strstr(text, "不低于")) &&
                    (strstr(text, "提醒") || strstr(text, "音量"));
    if (!list_rule && !clear_rule && !set_rule) {
        return false;
    }

    init_command_common(msg, out);
    if (list_rule) {
        out->type = CONTROL_CMD_LIST_TEMP_RULE;
        strncpy(out->capability, "temp_rule_list", sizeof(out->capability) - 1);
        return true;
    }
    if (clear_rule) {
        out->type = CONTROL_CMD_CLEAR_TEMP_RULE;
        strncpy(out->capability, "temp_rule_clear", sizeof(out->capability) - 1);
        int rule_id = parse_last_number_before(text, "规则");
        out->temp_rule_id = (rule_id > 0) ? (uint32_t)rule_id : 0;
        return true;
    }

    out->type = CONTROL_CMD_CREATE_TEMP_RULE;
    strncpy(out->capability, "temp_rule_create", sizeof(out->capability) - 1);

    int threshold_x10 = 0;
    if (!parse_temperature_threshold_x10(text, &threshold_x10)) {
        snprintf(reason, reason_size, "未识别到温度阈值，请说例如“温度高于30度时音量调到40%%”。");
        return true;
    }
    out->temp_threshold_x10 = threshold_x10;

    if (strstr(text, "高于") || strstr(text, "超过") || strstr(text, "大于") || strstr(text, "不低于")) {
        out->temp_comparator = 1;
    } else if (strstr(text, "低于") || strstr(text, "小于") || strstr(text, "不高于")) {
        out->temp_comparator = -1;
    } else {
        snprintf(reason, reason_size, "未识别到比较条件，请使用“高于/低于”。");
        return true;
    }

    if (strstr(text, "音量")) {
        int volume = 0;
        if (!parse_percent_value(text, &volume)) {
            snprintf(reason, reason_size, "未识别到目标音量，请说例如“音量调到40%%”。");
            return true;
        }
        out->temp_action_type = 2;
        out->temp_action_value = clamp_int(volume, 0, 100);
        return true;
    }

    out->temp_action_type = 1;
    const char *p = strstr(text, "提醒");
    if (p) {
        p += strlen("提醒");
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "我", strlen("我")) == 0) {
            p += strlen("我");
        }
        while (*p == ' ' || *p == '\t') p++;
    } else {
        p = text;
    }
    if (p && *p) {
        strncpy(out->note, p, sizeof(out->note) - 1);
    } else {
        strncpy(out->note, "温度事件触发", sizeof(out->note) - 1);
    }
    return true;
}

static bool parse_music_command(const mimi_msg_t *msg, control_command_t *out)
{
    if (!msg || !msg->content || !out) return false;
    const char *text = msg->content;

    static const char *const stop_keywords[] = {
        "停止音乐", "暂停音乐", "关闭音乐", "停掉音乐", "停歌", "别放了"
    };
    static const char *const play_keywords[] = {
        "播放音乐", "放音乐", "来点音乐", "来首歌", "放首歌", "播一首"
    };
    bool is_stop = contains_any(text, stop_keywords, sizeof(stop_keywords) / sizeof(stop_keywords[0]));
    bool is_play = contains_any(text, play_keywords, sizeof(play_keywords) / sizeof(play_keywords[0]));
    if (!is_stop && !is_play) {
        return false;
    }

    init_command_common(msg, out);
    if (is_stop) {
        out->type = CONTROL_CMD_STOP_MUSIC;
        strncpy(out->capability, "stop_music", sizeof(out->capability) - 1);
        return true;
    }

    out->type = CONTROL_CMD_PLAY_MUSIC;
    strncpy(out->capability, "play_music", sizeof(out->capability) - 1);

    const char *p = NULL;
    if ((p = strstr(text, "播放音乐")) != NULL) {
        p += strlen("播放音乐");
    } else if ((p = strstr(text, "放音乐")) != NULL) {
        p += strlen("放音乐");
    } else if ((p = strstr(text, "来点音乐")) != NULL) {
        p += strlen("来点音乐");
    } else if ((p = strstr(text, "来首歌")) != NULL) {
        p += strlen("来首歌");
    } else if ((p = strstr(text, "放首歌")) != NULL) {
        p += strlen("放首歌");
    } else if ((p = strstr(text, "播一首")) != NULL) {
        p += strlen("播一首");
    } else {
        p = text;
    }

    strncpy(out->note, p, sizeof(out->note) - 1);
    trim_ascii_inplace(out->note);
    if (out->note[0] == '\0') {
        strncpy(out->note, "轻音乐", sizeof(out->note) - 1);
    }
    return true;
}

static esp_err_t execute_with_capability(const control_command_t *cmd, control_result_t *out,
                                         char *err, size_t err_size)
{
    const control_capability_t *cap = find_capability(cmd->type);
    if (!cap) {
        snprintf(err, err_size, "未注册能力: %d", (int)cmd->type);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = cap->validate(cmd, err, err_size);
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint8_t i = 0; i <= cap->retry_max; i++) {
        ret = cap->execute(cmd, out, err, err_size);
        if (ret == ESP_OK) {
            strncpy(out->capability, cap->name, sizeof(out->capability) - 1);
            return ESP_OK;
        }
    }
    return ret;
}

esp_err_t control_plane_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Control plane initialized (%d capabilities)",
             (int)(sizeof(s_capabilities) / sizeof(s_capabilities[0])));
    return ESP_OK;
}

esp_err_t control_plane_try_handle_message(const mimi_msg_t *msg, control_result_t *out)
{
    if (!msg || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    const char *media_type = msg->media_type[0] ? msg->media_type : "text";
    if (strcmp(media_type, "voice") != 0) {
        return ESP_OK;
    }

    control_command_t cmd = {0};
    char reason[128] = {0};
    bool recognized = parse_reboot_command(msg, &cmd) ||
                      parse_alarm_command(msg, &cmd) ||
                      parse_temp_rule_command(msg, &cmd, reason, sizeof(reason)) ||
                      parse_music_command(msg, &cmd) ||
                      parse_volume_command(msg, &cmd, reason, sizeof(reason));
    if (!recognized) {
        return ESP_OK;
    }

    out->handled = true;
    out->from_rule = true;
    strncpy(out->request_id, cmd.request_id, sizeof(out->request_id) - 1);

    if (idemp_lookup(cmd.request_id, out)) {
        out->handled = true;
        out->from_rule = true;
        append_audit(out, "幂等命中，返回缓存结果");
        ESP_LOGI(TAG, "Idempotency hit: request_id=%s capability=%s",
                 out->request_id, out->capability);
        return ESP_OK;
    }

    if (reason[0] != '\0') {
        out->success = false;
        snprintf(out->response_text, sizeof(out->response_text), "%s", reason);
        idemp_store(cmd.request_id, out);
        append_audit(out, reason);
        return ESP_OK;
    }

    char err[128] = {0};
    esp_err_t ret = execute_with_capability(&cmd, out, err, sizeof(err));
    if (ret != ESP_OK) {
        out->success = false;
        snprintf(out->response_text, sizeof(out->response_text),
                 "操作失败：%s。", err[0] ? err : esp_err_to_name(ret));
        idemp_store(cmd.request_id, out);
        append_audit(out, out->response_text);
        ESP_LOGW(TAG, "Command execute failed: request_id=%s type=%d err=%s",
                 cmd.request_id, (int)cmd.type, err[0] ? err : esp_err_to_name(ret));
        return ESP_OK;
    }

    out->success = true;
    idemp_store(cmd.request_id, out);
    append_audit(out, out->response_text);
    ESP_LOGI(TAG, "Rule command handled: request_id=%s capability=%s success=1",
             out->request_id, out->capability);
    return ESP_OK;
}

size_t control_plane_get_recent_audits(control_audit_entry_t *out_entries, size_t max_entries)
{
    if (!out_entries || max_entries == 0) return 0;

    size_t count = 0;
    portENTER_CRITICAL(&s_lock);
    size_t available = (s_audit_count < max_entries) ? s_audit_count : max_entries;
    for (size_t i = 0; i < available; i++) {
        size_t idx = (s_audit_head + MIMI_CONTROL_AUDIT_SIZE - 1U - i) % MIMI_CONTROL_AUDIT_SIZE;
        out_entries[count++] = s_audits[idx];
    }
    portEXIT_CRITICAL(&s_lock);
    return count;
}

size_t control_plane_get_active_alarms(control_alarm_info_t *out_entries, size_t max_entries)
{
    if (!out_entries || max_entries == 0) return 0;
    size_t count = 0;
    int64_t cur = now_ms();

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < MIMI_CONTROL_MAX_ALARMS && count < max_entries; i++) {
        const alarm_slot_t *slot = &s_alarms[i];
        if (!slot->active) continue;
        control_alarm_info_t *dst = &out_entries[count++];
        memset(dst, 0, sizeof(*dst));
        dst->alarm_id = slot->alarm_id;
        dst->remaining_ms = (slot->due_ms > cur) ? (uint32_t)(slot->due_ms - cur) : 0;
        strncpy(dst->channel, slot->channel, sizeof(dst->channel) - 1);
        strncpy(dst->chat_id, slot->chat_id, sizeof(dst->chat_id) - 1);
        strncpy(dst->note, slot->note, sizeof(dst->note) - 1);
    }
    portEXIT_CRITICAL(&s_lock);
    return count;
}

size_t control_plane_get_temperature_rules(control_temp_rule_info_t *out_entries, size_t max_entries)
{
    if (!out_entries || max_entries == 0) return 0;
    size_t count = 0;

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < MIMI_CONTROL_MAX_TEMP_RULES && count < max_entries; i++) {
        const temp_rule_slot_t *slot = &s_temp_rules[i];
        if (!slot->active) continue;
        control_temp_rule_info_t *dst = &out_entries[count++];
        memset(dst, 0, sizeof(*dst));
        dst->rule_id = slot->rule_id;
        dst->threshold_x10 = slot->threshold_x10;
        dst->comparator = slot->comparator;
        dst->action_type = slot->action_type;
        dst->action_value = slot->action_value;
        strncpy(dst->note, slot->note, sizeof(dst->note) - 1);
    }
    portEXIT_CRITICAL(&s_lock);
    return count;
}

esp_err_t control_plane_handle_temperature_event(int temp_x10)
{
    typedef struct {
        uint32_t rule_id;
        int threshold_x10;
        int comparator;
        int action_type;
        int action_value;
        char note[96];
    } temp_hit_t;

    temp_hit_t hits[MIMI_CONTROL_MAX_TEMP_RULES];
    size_t hit_count = 0;
    int64_t now = now_ms();

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < MIMI_CONTROL_MAX_TEMP_RULES; i++) {
        temp_rule_slot_t *rule = &s_temp_rules[i];
        if (!rule->active) continue;
        if (now - rule->last_trigger_ms < MIMI_CONTROL_TEMP_RULE_COOLDOWN_MS) continue;

        bool matched = false;
        if (rule->comparator == 1) {
            matched = temp_x10 >= rule->threshold_x10;
        } else if (rule->comparator == -1) {
            matched = temp_x10 <= rule->threshold_x10;
        }
        if (!matched) continue;
        if (hit_count >= MIMI_CONTROL_MAX_TEMP_RULES) break;

        temp_hit_t *hit = &hits[hit_count++];
        hit->rule_id = rule->rule_id;
        hit->threshold_x10 = rule->threshold_x10;
        hit->comparator = rule->comparator;
        hit->action_type = rule->action_type;
        hit->action_value = rule->action_value;
        strncpy(hit->note, rule->note, sizeof(hit->note) - 1);
        rule->last_trigger_ms = now;
    }
    portEXIT_CRITICAL(&s_lock);

    for (size_t i = 0; i < hit_count; i++) {
        const temp_hit_t *hit = &hits[i];
        if (hit->action_type == 2) {
            control_command_t cmd = {0};
            cmd.type = CONTROL_CMD_SET_VOLUME;
            cmd.target_value = clamp_int(hit->action_value, 0, 100);
            strncpy(cmd.capability, "set_volume", sizeof(cmd.capability) - 1);
            snprintf(cmd.request_id, sizeof(cmd.request_id), "temp-%" PRIu32 "-%" PRId64,
                     hit->rule_id, now);

            control_result_t result = {0};
            result.handled = true;
            result.from_rule = true;
            strncpy(result.request_id, cmd.request_id, sizeof(result.request_id) - 1);
            char err[128] = {0};
            esp_err_t ret = execute_with_capability(&cmd, &result, err, sizeof(err));
            if (ret == ESP_OK) {
                result.success = true;
                append_audit(&result, "温度规则触发：执行音量调整");
                ESP_LOGI(TAG, "Temp rule hit: id=%" PRIu32 " temp=%d.%dC action=set_volume(%d)",
                         hit->rule_id, temp_x10 / 10, abs(temp_x10 % 10), cmd.target_value);
            } else {
                result.success = false;
                snprintf(result.response_text, sizeof(result.response_text),
                         "温度规则执行失败：%s", err[0] ? err : esp_err_to_name(ret));
                append_audit(&result, result.response_text);
                ESP_LOGW(TAG, "Temp rule execute failed: id=%" PRIu32 " err=%s",
                         hit->rule_id, err[0] ? err : esp_err_to_name(ret));
            }
            continue;
        }

        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_SYSTEM, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "temp_rule", sizeof(msg.chat_id) - 1);
        msg.content = malloc(192);
        if (!msg.content) {
            ESP_LOGE(TAG, "Temp rule message malloc failed");
            continue;
        }
        control_result_t result = {0};
        result.handled = true;
        result.from_rule = true;
        strncpy(result.capability, "temp_rule_notify", sizeof(result.capability) - 1);
        snprintf(result.request_id, sizeof(result.request_id), "temp-%" PRIu32 "-%" PRId64 "-n",
                 hit->rule_id, now);

        snprintf(msg.content, 192, "温度触发提醒：当前%d.%d°C，%s",
                 temp_x10 / 10, abs(temp_x10 % 10),
                 hit->note[0] ? hit->note : "请注意温度变化。");
        if (message_bus_push_outbound(&msg) != ESP_OK) {
            message_bus_msg_free(&msg);
            ESP_LOGW(TAG, "Temp rule outbound enqueue failed: id=%" PRIu32, hit->rule_id);
            result.success = false;
            snprintf(result.response_text, sizeof(result.response_text), "温度规则提醒入队失败");
            append_audit(&result, result.response_text);
        } else {
            ESP_LOGI(TAG, "Temp rule hit: id=%" PRIu32 " temp=%d.%dC action=remind",
                     hit->rule_id, temp_x10 / 10, abs(temp_x10 % 10));
            result.success = true;
            snprintf(result.response_text, sizeof(result.response_text), "温度规则触发：执行提醒");
            append_audit(&result, result.response_text);
        }
    }

    return ESP_OK;
}
