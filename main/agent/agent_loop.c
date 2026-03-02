#include "agent_loop.h"
#include "agent/context_builder.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "tools/tool_registry.h"
#include "control/control_plane.h"
#include "display/display.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "cJSON.h"

static const char *TAG = "agent";
static const char *const WORKING_PHRASES[] = {
    "我在处理，请稍等一下…",
    "收到，正在帮你查…",
    "正在执行中，马上给你结果…",
};

#define TOOL_OUTPUT_SIZE  (12 * 1024)
#define TOOL_BUDGET_EXCEEDED_MSG "Error: tool result budget exceeded on device"
#define ROUTE_HINT_VALUE_MAX_LEN 192
#define SKILL_RULE_MAX 12
#define SKILL_HINTS_BLOCK_MAX 768
#define SKILL_HINT_MAX_SELECTED 4
#define SKILL_RULE_DEFAULT_PRIO_MEDIA 70
#define SKILL_RULE_DEFAULT_PRIO_CHANNEL 60
#define SKILL_RULE_PRIORITY_MIN 0
#define SKILL_RULE_PRIORITY_MAX 100

typedef struct {
    uint32_t total_turns;
    uint32_t success_turns;
    uint32_t failed_turns;
    uint32_t timeout_turns;
    uint32_t context_budget_hits;
    uint32_t tool_budget_hits;
    uint32_t iter_limit_hits;
    uint32_t llm_error_turns;
    uint32_t outbound_enqueue_failures;
    uint32_t outbound_send_failures;
    uint32_t max_turn_latency_ms;
    uint32_t last_turn_latency_ms;
    uint32_t last_run_id;
    uint64_t sum_turn_latency_ms;
    uint64_t sum_context_ms;
    uint64_t sum_llm_ms;
    uint64_t sum_tools_ms;
    uint64_t sum_outbound_ms;
} agent_stats_state_t;

static agent_stats_state_t s_stats = {0};
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_run_seq = 0;

typedef struct {
    char text[ROUTE_HINT_VALUE_MAX_LEN];
    char system[ROUTE_HINT_VALUE_MAX_LEN];
    char voice[ROUTE_HINT_VALUE_MAX_LEN];
    char photo[ROUTE_HINT_VALUE_MAX_LEN];
    char document[ROUTE_HINT_VALUE_MAX_LEN];
    char media[ROUTE_HINT_VALUE_MAX_LEN];
} route_hint_cfg_t;

static route_hint_cfg_t s_route_hint_cfg = {0};
static TickType_t s_route_hint_loaded_at = 0;
static bool s_route_hint_cfg_ready = false;

typedef struct {
    char trigger_type[16];  /* media_type / channel */
    char trigger_value[32];
    char instruction[ROUTE_HINT_VALUE_MAX_LEN];
    int priority;
    int order;
} skill_rule_t;

typedef struct {
    skill_rule_t rules[SKILL_RULE_MAX];
    int count;
} skill_rule_cfg_t;

typedef enum {
    VOLUME_INTENT_NONE = 0,
    VOLUME_INTENT_QUERY,
    VOLUME_INTENT_ADJUST,
} volume_intent_t;

static skill_rule_cfg_t s_skill_rule_cfg = {0};
static TickType_t s_skill_rule_loaded_at = 0;
static bool s_skill_rule_cfg_ready = false;

static uint32_t elapsed_ms(TickType_t start, TickType_t end)
{
    return (uint32_t)((end - start) * portTICK_PERIOD_MS);
}

static uint32_t next_run_id(void)
{
    uint32_t run_id = 0;
    portENTER_CRITICAL(&s_stats_lock);
    s_run_seq++;
    run_id = s_run_seq;
    portEXIT_CRITICAL(&s_stats_lock);
    return run_id;
}

static void record_turn_stats(uint32_t run_id,
                              bool success,
                              uint32_t total_ms,
                              uint32_t context_ms,
                              uint32_t llm_ms,
                              uint32_t tools_ms,
                              uint32_t outbound_ms,
                              bool hit_timeout,
                              bool hit_context_budget,
                              bool hit_tool_budget,
                              bool hit_iter_limit,
                              bool hit_llm_error,
                              bool outbound_enqueue_failed)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.total_turns++;
    if (success) {
        s_stats.success_turns++;
    } else {
        s_stats.failed_turns++;
    }

    if (hit_timeout) s_stats.timeout_turns++;
    if (hit_context_budget) s_stats.context_budget_hits++;
    if (hit_tool_budget) s_stats.tool_budget_hits++;
    if (hit_iter_limit) s_stats.iter_limit_hits++;
    if (hit_llm_error) s_stats.llm_error_turns++;
    if (outbound_enqueue_failed) s_stats.outbound_enqueue_failures++;

    s_stats.last_run_id = run_id;
    s_stats.last_turn_latency_ms = total_ms;
    if (total_ms > s_stats.max_turn_latency_ms) {
        s_stats.max_turn_latency_ms = total_ms;
    }

    s_stats.sum_turn_latency_ms += total_ms;
    s_stats.sum_context_ms += context_ms;
    s_stats.sum_llm_ms += llm_ms;
    s_stats.sum_tools_ms += tools_ms;
    s_stats.sum_outbound_ms += outbound_ms;
    portEXIT_CRITICAL(&s_stats_lock);
}

/* Build the assistant content array from llm_response_t for the messages history.
 * Returns a cJSON array with text and tool_use blocks. */
static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    /* Text block */
    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    /* Tool use blocks */
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id);
        cJSON_AddStringToObject(tool_block, "name", call->name);

        cJSON *input = cJSON_Parse(call->input);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

static void append_turn_context_prompt(char *prompt, size_t size, const mimi_msg_t *msg)
{
    if (!prompt || size == 0 || !msg) {
        return;
    }

    size_t off = strnlen(prompt, size - 1);
    if (off >= size - 1) {
        return;
    }

    int n = snprintf(
        prompt + off, size - off,
        "\n## Current Turn Context\n"
        "- source_channel: %s\n"
        "- source_chat_id: %s\n"
        "- If using cron_add for Telegram in this turn, set channel='telegram' and chat_id to source_chat_id.\n"
        "- Never use chat_id 'cron' for Telegram messages.\n",
        msg->channel[0] ? msg->channel : "(unknown)",
        msg->chat_id[0] ? msg->chat_id : "(empty)");

    if (n < 0 || (size_t)n >= (size - off)) {
        prompt[size - 1] = '\0';
    }
}

/* Build the user message with tool_result blocks */
static bool agent_turn_timed_out(TickType_t start_tick)
{
    uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS);
    return elapsed_ms > MIMI_AGENT_TURN_TIMEOUT_MS;
}

static esp_err_t get_context_bytes(const char *system_prompt, cJSON *messages, size_t *out_bytes)
{
    if (!system_prompt || !messages || !out_bytes) return ESP_ERR_INVALID_ARG;

    char *messages_json = cJSON_PrintUnformatted(messages);
    if (!messages_json) {
        return ESP_ERR_NO_MEM;
    }

    *out_bytes = strlen(system_prompt) + strlen(messages_json);
    free(messages_json);
    return ESP_OK;
}

static void truncate_tool_output_if_needed(char *tool_output, size_t tool_output_size)
{
    const char *suffix = "\n...[tool output truncated by budget]";
    size_t suffix_len = strlen(suffix);
    size_t len = strlen(tool_output);
    if (len <= MIMI_AGENT_TOOL_RESULT_MAX_BYTES) return;

    size_t hard_limit = MIMI_AGENT_TOOL_RESULT_MAX_BYTES;
    if (hard_limit >= tool_output_size) {
        hard_limit = tool_output_size - 1;
    }
    if (hard_limit <= suffix_len + 1) {
        tool_output[hard_limit] = '\0';
        return;
    }

    size_t keep = hard_limit - suffix_len;
    tool_output[keep] = '\0';
    strncat(tool_output, suffix, tool_output_size - strlen(tool_output) - 1);
}

static void copy_route_hint(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void route_hint_set_defaults(route_hint_cfg_t *cfg)
{
    if (!cfg) return;
    copy_route_hint(cfg->text, sizeof(cfg->text), "");
    copy_route_hint(cfg->system, sizeof(cfg->system),
                    "这是系统触发任务，直接执行任务并给出结果，不要寒暄。");
    copy_route_hint(cfg->voice, sizeof(cfg->voice),
                    "这是语音转写输入，优先用简短自然中文回复；信息缺失时先提一个澄清问题。");
    copy_route_hint(cfg->photo, sizeof(cfg->photo),
                    "这是图片解析输入，优先基于描述/文字/元素回答；不要复述原始元数据。");
    copy_route_hint(cfg->document, sizeof(cfg->document),
                    "这是文件输入，先提炼关键信息与结论；不确定处明确说明。");
    copy_route_hint(cfg->media, sizeof(cfg->media),
                    "这是媒体摘要输入，先基于现有信息回答，并说明可继续补充解析。");
}

static char *trim_ascii_spaces(char *s)
{
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static void route_hint_apply_line(route_hint_cfg_t *cfg, char *line)
{
    if (!cfg || !line) return;

    char *p = trim_ascii_spaces(line);
    if (*p == '-' || *p == '*') {
        p++;
        p = trim_ascii_spaces(p);
    }
    if (strncmp(p, "route.", 6) != 0) return;

    char *colon = strchr(p, ':');
    if (!colon) return;

    *colon = '\0';
    char *key = trim_ascii_spaces(p + 6);
    char *value = trim_ascii_spaces(colon + 1);
    if (!key || !value || value[0] == '\0') return;

    if (strcmp(key, "text") == 0) {
        copy_route_hint(cfg->text, sizeof(cfg->text), value);
    } else if (strcmp(key, "system") == 0) {
        copy_route_hint(cfg->system, sizeof(cfg->system), value);
    } else if (strcmp(key, "voice") == 0) {
        copy_route_hint(cfg->voice, sizeof(cfg->voice), value);
    } else if (strcmp(key, "photo") == 0) {
        copy_route_hint(cfg->photo, sizeof(cfg->photo), value);
    } else if (strcmp(key, "document") == 0) {
        copy_route_hint(cfg->document, sizeof(cfg->document), value);
    } else if (strcmp(key, "media") == 0) {
        copy_route_hint(cfg->media, sizeof(cfg->media), value);
    }
}

static void route_hint_reload_if_needed(void)
{
    TickType_t now = xTaskGetTickCount();
    if (s_route_hint_cfg_ready &&
        elapsed_ms(s_route_hint_loaded_at, now) < MIMI_AGENT_ROUTE_HINT_RELOAD_MS) {
        return;
    }

    route_hint_cfg_t next_cfg = {0};
    route_hint_set_defaults(&next_cfg);

    FILE *fp = fopen(MIMI_TOOLS_FILE, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            route_hint_apply_line(&next_cfg, line);
        }
        fclose(fp);
    } else {
        ESP_LOGW(TAG, "Route hint config not found: %s, using defaults", MIMI_TOOLS_FILE);
    }

    s_route_hint_cfg = next_cfg;
    s_route_hint_loaded_at = now;
    s_route_hint_cfg_ready = true;
}

static bool skill_rule_add(skill_rule_cfg_t *cfg,
                           const char *trigger_type,
                           const char *trigger_value,
                           const char *instruction,
                           int priority)
{
    if (!cfg || !trigger_type || !trigger_value || !instruction) return false;
    if (cfg->count >= SKILL_RULE_MAX) return false;
    if (trigger_type[0] == '\0' || trigger_value[0] == '\0' || instruction[0] == '\0') return false;

    if (priority < SKILL_RULE_PRIORITY_MIN) priority = SKILL_RULE_PRIORITY_MIN;
    if (priority > SKILL_RULE_PRIORITY_MAX) priority = SKILL_RULE_PRIORITY_MAX;

    skill_rule_t *r = &cfg->rules[cfg->count];
    strncpy(r->trigger_type, trigger_type, sizeof(r->trigger_type) - 1);
    strncpy(r->trigger_value, trigger_value, sizeof(r->trigger_value) - 1);
    strncpy(r->instruction, instruction, sizeof(r->instruction) - 1);
    r->priority = priority;
    r->order = cfg->count;
    cfg->count++;
    return true;
}

static int skill_rule_default_priority(const char *field)
{
    if (!field) return SKILL_RULE_DEFAULT_PRIO_CHANNEL;
    if (strcmp(field, "media_type") == 0) return SKILL_RULE_DEFAULT_PRIO_MEDIA;
    if (strcmp(field, "channel") == 0) return SKILL_RULE_DEFAULT_PRIO_CHANNEL;
    return SKILL_RULE_DEFAULT_PRIO_CHANNEL;
}

static void skill_rule_apply_line(skill_rule_cfg_t *cfg, char *line)
{
    if (!cfg || !line) return;

    char *p = trim_ascii_spaces(line);
    if (*p == '-' || *p == '*') {
        p++;
        p = trim_ascii_spaces(p);
    }
    if (strncmp(p, "when.", 5) != 0) return;

    char *arrow = strstr(p, "->");
    if (!arrow) return;
    *arrow = '\0';
    char *lhs = trim_ascii_spaces(p + 5);
    char *rhs = trim_ascii_spaces(arrow + 2);
    if (rhs[0] == '\0') return;

    char *field = NULL;
    char *value = NULL;
    int priority = -1;

    char *saveptr = NULL;
    for (char *token = strtok_r(lhs, ",", &saveptr);
         token;
         token = strtok_r(NULL, ",", &saveptr)) {
        token = trim_ascii_spaces(token);
        if (token[0] == '\0') continue;

        char *eq = strchr(token, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim_ascii_spaces(token);
        char *v = trim_ascii_spaces(eq + 1);
        if (k[0] == '\0' || v[0] == '\0') continue;

        if (!field && (strcmp(k, "media_type") == 0 || strcmp(k, "channel") == 0)) {
            field = k;
            value = v;
            continue;
        }
        if (strcmp(k, "priority") == 0) {
            char *endptr = NULL;
            long pval = strtol(v, &endptr, 10);
            if (endptr) {
                endptr = trim_ascii_spaces(endptr);
            }
            if (endptr && endptr[0] == '\0') {
                priority = (int)pval;
            }
        }
    }

    if (!field || !value) return;
    if (priority < 0) {
        priority = skill_rule_default_priority(field);
    }
    skill_rule_add(cfg, field, value, rhs, priority);
}

static void skill_rule_reload_if_needed(void)
{
    TickType_t now = xTaskGetTickCount();
    if (s_skill_rule_cfg_ready &&
        elapsed_ms(s_skill_rule_loaded_at, now) < MIMI_AGENT_SKILL_RULE_RELOAD_MS) {
        return;
    }

    skill_rule_cfg_t next_cfg = {0};

    FILE *fp = fopen(MIMI_SKILLS_FILE, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            skill_rule_apply_line(&next_cfg, line);
        }
        fclose(fp);
    } else {
        ESP_LOGW(TAG, "Skill rule config not found: %s", MIMI_SKILLS_FILE);
    }

    s_skill_rule_cfg = next_cfg;
    s_skill_rule_loaded_at = now;
    s_skill_rule_cfg_ready = true;
}

static int collect_skill_hints(const mimi_msg_t *msg, char *buf, size_t buf_size)
{
    if (!msg || !buf || buf_size == 0) return 0;

    skill_rule_reload_if_needed();
    buf[0] = '\0';

    const char *media_type = msg->media_type[0] ? msg->media_type : "text";
    typedef struct {
        char instruction[ROUTE_HINT_VALUE_MAX_LEN];
        int priority;
        int order;
    } matched_hint_t;

    matched_hint_t matched[SKILL_RULE_MAX] = {0};
    int matched_count = 0;

    for (int i = 0; i < s_skill_rule_cfg.count; i++) {
        const skill_rule_t *r = &s_skill_rule_cfg.rules[i];
        bool ok = false;
        if (strcmp(r->trigger_type, "media_type") == 0) {
            ok = (strcmp(media_type, r->trigger_value) == 0);
        } else if (strcmp(r->trigger_type, "channel") == 0) {
            ok = (strcmp(msg->channel, r->trigger_value) == 0);
        }
        if (!ok) continue;

        int existing = -1;
        for (int k = 0; k < matched_count; k++) {
            if (strcmp(matched[k].instruction, r->instruction) == 0) {
                existing = k;
                break;
            }
        }
        if (existing >= 0) {
            if (r->priority > matched[existing].priority ||
                (r->priority == matched[existing].priority && r->order < matched[existing].order)) {
                matched[existing].priority = r->priority;
                matched[existing].order = r->order;
            }
            continue;
        }
        if (matched_count >= SKILL_RULE_MAX) break;
        strncpy(matched[matched_count].instruction, r->instruction,
                sizeof(matched[matched_count].instruction) - 1);
        matched[matched_count].priority = r->priority;
        matched[matched_count].order = r->order;
        matched_count++;
    }

    for (int i = 0; i < matched_count; i++) {
        for (int j = i + 1; j < matched_count; j++) {
            bool better = false;
            if (matched[j].priority > matched[i].priority) {
                better = true;
            } else if (matched[j].priority == matched[i].priority &&
                       matched[j].order < matched[i].order) {
                better = true;
            }
            if (better) {
                matched_hint_t tmp = matched[i];
                matched[i] = matched[j];
                matched[j] = tmp;
            }
        }
    }

    int selected = matched_count;
    if (selected > SKILL_HINT_MAX_SELECTED) {
        selected = SKILL_HINT_MAX_SELECTED;
    }

    size_t off = 0;
    int emitted = 0;
    for (int i = 0; i < selected; i++) {
        int n = snprintf(buf + off, buf_size - off, "- %s\n", matched[i].instruction);
        if (n <= 0 || (size_t)n >= (buf_size - off)) break;
        off += (size_t)n;
        emitted++;
    }
    return emitted;
}

static const char *infer_route_hint(const mimi_msg_t *msg)
{
    if (!msg) return "";

    route_hint_reload_if_needed();

    const char *media_type = msg->media_type[0] ? msg->media_type : "text";
    if (strcmp(msg->channel, MIMI_CHAN_SYSTEM) == 0 || strcmp(media_type, "system") == 0) {
        return s_route_hint_cfg.system;
    }
    if (strcmp(media_type, "voice") == 0) {
        return s_route_hint_cfg.voice;
    }
    if (strcmp(media_type, "photo") == 0) {
        return s_route_hint_cfg.photo;
    }
    if (strcmp(media_type, "document") == 0) {
        return s_route_hint_cfg.document;
    }
    if (strcmp(media_type, "media") == 0) {
        return s_route_hint_cfg.media;
    }
    return s_route_hint_cfg.text;
}

static bool text_contains_any(const char *text, const char *const keywords[], size_t count)
{
    if (!text || !text[0]) return false;
    for (size_t i = 0; i < count; i++) {
        if (keywords[i] && keywords[i][0] && strstr(text, keywords[i])) {
            return true;
        }
    }
    return false;
}

static volume_intent_t detect_voice_volume_intent(const mimi_msg_t *msg)
{
    if (!msg || !msg->content) return VOLUME_INTENT_NONE;

    const char *media_type = msg->media_type[0] ? msg->media_type : "text";
    if (strcmp(media_type, "voice") != 0) {
        return VOLUME_INTENT_NONE;
    }
    if (!strstr(msg->content, "音量")) {
        return VOLUME_INTENT_NONE;
    }

    static const char *const adjust_keywords[] = {
        "调", "调整", "设置", "设为", "改成", "改到", "变成",
        "增大", "增加", "调大", "大一点", "开大",
        "减小", "减少", "调小", "小一点", "开小", "降低",
        "静音", "mute", "unmute", "%"
    };
    if (text_contains_any(msg->content, adjust_keywords,
                          sizeof(adjust_keywords) / sizeof(adjust_keywords[0]))) {
        return VOLUME_INTENT_ADJUST;
    }

    static const char *const query_keywords[] = {
        "多少", "几", "多大", "当前", "现在", "查询", "查看",
        "是多少", "是什么", "啥", "吗", "？", "?"
    };
    if (text_contains_any(msg->content, query_keywords,
                          sizeof(query_keywords) / sizeof(query_keywords[0]))) {
        return VOLUME_INTENT_QUERY;
    }

    /* 未命中明显关键词时，保守按查询处理，避免直接口胡音量值。 */
    return VOLUME_INTENT_QUERY;
}

static char *build_user_content_with_meta(const mimi_msg_t *msg)
{
    if (!msg || !msg->content || !msg->content[0]) return NULL;

    const char *media_type = msg->media_type[0] ? msg->media_type : "text";
    const char *route_hint = infer_route_hint(msg);
    volume_intent_t volume_intent = detect_voice_volume_intent(msg);
    const char *runtime_hint = "";
    if (volume_intent == VOLUME_INTENT_QUERY) {
        runtime_hint = "这是音量查询问题。必须先调用 get_volume 获取实时音量，再回答用户。禁止凭上下文记忆直接给出音量数值。";
    } else if (volume_intent == VOLUME_INTENT_ADJUST) {
        runtime_hint = "这是音量调节问题。必须调用 set_volume 执行调整；如果用户说“增大/减小X%”这类相对变化，先调用 get_volume，再计算后调用 set_volume。";
    }
    char skill_hints[SKILL_HINTS_BLOCK_MAX] = {0};
    int skill_hint_count = collect_skill_hints(msg, skill_hints, sizeof(skill_hints));
    bool has_skills = skill_hint_count > 0;
    if (has_skills) {
        ESP_LOGI(TAG, "Skill hints matched: %d (channel=%s, media_type=%s)",
                 skill_hint_count, msg->channel, media_type);
    }
    bool has_hint = route_hint[0] != '\0';
    bool has_runtime_hint = runtime_hint[0] != '\0';
    bool has_meta = (strcmp(media_type, "text") != 0) ||
                    (msg->file_id[0] != '\0') ||
                    (msg->file_path[0] != '\0') ||
                    (msg->meta_json && msg->meta_json[0] != '\0');
    if (!has_meta && !has_hint && !has_runtime_hint && !has_skills) {
        return strdup(msg->content);
    }

    const char *file_id = msg->file_id[0] ? msg->file_id : "-";
    const char *file_path = msg->file_path[0] ? msg->file_path : "-";
    const char *meta_json = (msg->meta_json && msg->meta_json[0]) ? msg->meta_json : "{}";

    size_t cap = strlen(msg->content) + strlen(media_type) + strlen(file_id) +
                 strlen(file_path) + strlen(meta_json) + strlen(route_hint) +
                 strlen(runtime_hint) + strlen(skill_hints) + 384;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    size_t off = 0;
    int n = snprintf(buf + off, cap - off, "%s", msg->content);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    off += (size_t)n;
    if (off >= cap) off = cap - 1;
    if (has_hint) {
        n = snprintf(buf + off, cap - off, "\n\n[route_hint]\n%s", route_hint);
        if (n < 0) {
            free(buf);
            return NULL;
        }
        off += (size_t)n;
        if (off >= cap) off = cap - 1;
    }
    if (has_runtime_hint) {
        n = snprintf(buf + off, cap - off, "\n\n[route_hint_runtime]\n%s", runtime_hint);
        if (n < 0) {
            free(buf);
            return NULL;
        }
        off += (size_t)n;
        if (off >= cap) off = cap - 1;
    }
    if (has_skills) {
        n = snprintf(buf + off, cap - off, "\n\n[skill_hints]\n%s", skill_hints);
        if (n < 0) {
            free(buf);
            return NULL;
        }
        off += (size_t)n;
        if (off >= cap) off = cap - 1;
    }
    if (has_meta) {
        snprintf(buf + off, cap - off,
                 "\n\n[message_meta]\nchannel=%s\nmedia_type=%s\nfile_id=%s\nfile_path=%s\nmeta=%s",
                 msg->channel, media_type, file_id, file_path, meta_json);
    }
    return buf;
}

static cJSON *build_tool_results(const llm_response_t *resp, const mimi_msg_t *msg,
                                 char *tool_output, size_t tool_output_size,
                                 bool *budget_exceeded)
{
    cJSON *content = cJSON_CreateArray();
    size_t total_bytes = 0;
    bool exhausted = false;

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        if (exhausted) {
            snprintf(tool_output, tool_output_size, "%s", TOOL_BUDGET_EXCEEDED_MSG);
        } else {
            /* Execute tool */
            tool_output[0] = '\0';
            tool_registry_execute(call->name, call->input, tool_output, tool_output_size);
            truncate_tool_output_if_needed(tool_output, tool_output_size);

            size_t cur_len = strlen(tool_output);
            if (total_bytes + cur_len > MIMI_AGENT_TOOL_RESULTS_TOTAL_MAX) {
                exhausted = true;
                snprintf(tool_output, tool_output_size, "%s", TOOL_BUDGET_EXCEEDED_MSG);
            } else {
                total_bytes += cur_len;
            }
        }

        ESP_LOGI(TAG, "Tool %s result: %d bytes", call->name, (int)strlen(tool_output));

        /* Build tool_result block */
        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);

    }

    if (budget_exceeded) {
        *budget_exceeded = exhausted;
    }

    return content;
}

static void agent_loop_task(void *arg)
{
    ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

    /* Allocate large buffers from PSRAM */
    char *system_prompt = heap_caps_calloc(1, MIMI_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *history_json = heap_caps_calloc(1, MIMI_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *tool_output = heap_caps_calloc(1, TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);

    if (!system_prompt || !history_json || !tool_output) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        vTaskDelete(NULL);
        return;
    }

    const char *tools_json = tool_registry_get_tools_json();

    while (1) {
        mimi_msg_t msg;
        esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
        if (err != ESP_OK) continue;

        uint32_t run_id = next_run_id();
        TickType_t turn_start_tick = xTaskGetTickCount();
        uint32_t context_ms = 0;
        uint32_t llm_ms = 0;
        uint32_t tools_ms = 0;
        uint32_t outbound_ms = 0;
        bool hit_timeout = false;
        bool hit_context_budget = false;
        bool hit_tool_budget = false;
        bool hit_iter_limit = false;
        bool hit_llm_error = false;
        bool outbound_enqueue_failed = false;
        bool produced_final_response = false;
        char *user_content = NULL;
        const char *user_text_for_llm = msg.content;
        const char *media_type = msg.media_type[0] ? msg.media_type : "text";
        bool has_runtime_meta = (strcmp(media_type, "text") != 0) ||
                                (msg.file_id[0] != '\0') ||
                                (msg.file_path[0] != '\0') ||
                                (msg.meta_json && msg.meta_json[0] != '\0');

        ESP_LOGI(TAG, "run=%" PRIu32 " ingress %s:%s", run_id, msg.channel, msg.chat_id);
        if (has_runtime_meta) {
            ESP_LOGI(TAG,
                     "run=%" PRIu32 " ingress meta media_type=%s file_id=%.32s file_path=%.48s",
                     run_id,
                     media_type,
                     msg.file_id[0] ? msg.file_id : "-",
                     msg.file_path[0] ? msg.file_path : "-");
        }

        /* Update display with user message */
        display_show_message("user", msg.content);
        display_set_display_status(DISPLAY_STATUS_THINKING);

        /* 规则优先：高确定性控制命令直接执行，跳过 LLM。 */
        control_result_t control_result = {0};
        err = control_plane_try_handle_message(&msg, &control_result);
        if (err == ESP_OK && control_result.handled) {
            TickType_t outbound_stage_start = xTaskGetTickCount();
            if (control_result.response_text[0]) {
                session_append(msg.chat_id, "user", msg.content);
                session_append(msg.chat_id, "assistant", control_result.response_text);

                mimi_msg_t out = {0};
                strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
                strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
                out.content = strdup(control_result.response_text);
                if (!out.content || message_bus_push_outbound(&out) != ESP_OK) {
                    outbound_enqueue_failed = true;
                    message_bus_msg_free(&out);
                } else {
                    produced_final_response = true;
                    display_show_message("assistant", control_result.response_text);
                    display_set_display_status(control_result.success
                                               ? DISPLAY_STATUS_SPEAKING
                                               : DISPLAY_STATUS_ERROR);
                }
            } else if (control_result.success) {
                /* 某些确定性动作（如音乐播放）采用静默返回，视为已完成。 */
                produced_final_response = true;
                display_set_display_status(DISPLAY_STATUS_IDLE);
            }
            outbound_ms = elapsed_ms(outbound_stage_start, xTaskGetTickCount());

            message_bus_msg_free(&msg);
            display_set_display_status(DISPLAY_STATUS_IDLE);

            uint32_t total_ms = elapsed_ms(turn_start_tick, xTaskGetTickCount());
            bool response_ready = control_result.response_text[0] ? produced_final_response : true;
            bool success = control_result.success && response_ready && !outbound_enqueue_failed;
            record_turn_stats(run_id, success, total_ms, context_ms, llm_ms, tools_ms, outbound_ms,
                              hit_timeout, hit_context_budget, hit_tool_budget, hit_iter_limit,
                              hit_llm_error, outbound_enqueue_failed);
            ESP_LOGI(TAG,
                     "run=%" PRIu32 " done via control plane success=%d total=%" PRIu32
                     "ms outbound=%" PRIu32 "ms capability=%s",
                     run_id, success ? 1 : 0, total_ms, outbound_ms,
                     control_result.capability[0] ? control_result.capability : "-");
            continue;
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "Control plane error: %s", esp_err_to_name(err));
        }

        TickType_t context_stage_start = xTaskGetTickCount();

        /* 1. Build system prompt */
        context_build_system_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE);
        append_turn_context_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE, &msg);
        ESP_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel, msg.chat_id);

        /* 2. Load session history into cJSON array */
        session_get_history_json(msg.chat_id, history_json,
                                 MIMI_LLM_STREAM_BUF_SIZE, MIMI_AGENT_MAX_HISTORY);

        cJSON *messages = cJSON_Parse(history_json);
        if (!messages) messages = cJSON_CreateArray();

        /* 3. Append current user message */
        user_content = build_user_content_with_meta(&msg);
        if (user_content && user_content[0]) {
            user_text_for_llm = user_content;
        }
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", user_text_for_llm);
        cJSON_AddItemToArray(messages, user_msg);

        context_ms = elapsed_ms(context_stage_start, xTaskGetTickCount());
        ESP_LOGI(TAG, "run=%" PRIu32 " stage=context %" PRIu32 " ms", run_id, context_ms);

        /* 4. ReAct loop */
        char *final_text = NULL;
        int iteration = 0;
        bool sent_working_status = false;

        while (iteration < MIMI_AGENT_MAX_TOOL_ITER) {
            size_t context_bytes = 0;
            if (agent_turn_timed_out(turn_start_tick)) {
                ESP_LOGW(TAG, "Turn timeout exceeded (%d ms)", MIMI_AGENT_TURN_TIMEOUT_MS);
                hit_timeout = true;
                final_text = strdup("这次处理超时了，请把问题拆小一点再试。");
                break;
            }

            err = get_context_bytes(system_prompt, messages, &context_bytes);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to compute context size: %s", esp_err_to_name(err));
                hit_context_budget = true;
                final_text = strdup("设备内存紧张，暂时无法继续处理。");
                break;
            }
            if (context_bytes > MIMI_AGENT_MAX_CONTEXT_BYTES) {
                ESP_LOGW(TAG, "Context budget exceeded: %d > %d",
                         (int)context_bytes, MIMI_AGENT_MAX_CONTEXT_BYTES);
                hit_context_budget = true;
                final_text = strdup("上下文太长了，请精简后再问我。");
                break;
            }

            /* Send "working" indicator before each API call */
#if MIMI_AGENT_SEND_WORKING_STATUS
            static const char *working_phrases[] = {
                "mimi\xF0\x9F\x98\x97is working...",
                "mimi\xF0\x9F\x90\xBE is thinking...",
                "mimi\xF0\x9F\x92\xAD is pondering...",
                "mimi\xF0\x9F\x8C\x99 is on it...",
                "mimi\xE2\x9C\xA8 is cooking...",
            };
            static const int phrase_count = sizeof(working_phrases) / sizeof(working_phrases[0]);
#endif
#if MIMI_AGENT_SEND_WORKING_STATUS
            if (!sent_working_status && strcmp(msg.channel, MIMI_CHAN_SYSTEM) != 0) {
                mimi_msg_t status = {0};
                strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                size_t phrase_count = sizeof(WORKING_PHRASES) / sizeof(WORKING_PHRASES[0]);
                status.content = strdup(WORKING_PHRASES[esp_random() % phrase_count]);
                if (status.content && message_bus_push_outbound(&status) != ESP_OK) {
                    message_bus_msg_free(&status);
                } else {
                    sent_working_status = true;
                }
            }
#endif

            display_set_display_status(DISPLAY_STATUS_THINKING);

            llm_response_t resp;
            TickType_t llm_stage_start = xTaskGetTickCount();
            err = llm_chat_tools(system_prompt, messages, tools_json, &resp);
            llm_ms += elapsed_ms(llm_stage_start, xTaskGetTickCount());

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LLM call failed: %s", esp_err_to_name(err));
                int http_status = llm_get_last_http_status();
                const char *llm_err = llm_get_last_error_message();
                if (http_status == 401 ||
                    (llm_err && (strstr(llm_err, "invalid x-api-key") ||
                                 strstr(llm_err, "authentication_error") ||
                                 strstr(llm_err, "invalid_api_key")))) {
                    final_text = strdup("LLM 鉴权失败：API Key 无效或与当前 provider 不匹配。请执行 set_api_key <KEY>，必要时执行 set_model_provider openai 或 set_model_provider anthropic。");
                } else {
                    final_text = strdup("LLM 调用失败，请稍后重试。");
                }
                hit_llm_error = true;
                display_set_display_status(DISPLAY_STATUS_ERROR);
                break;
            }

            if (!resp.tool_use) {
                /* Normal completion — save final text and break */
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                    produced_final_response = true;
                    display_show_message("assistant", resp.text);
                    display_set_display_status(DISPLAY_STATUS_SPEAKING);
                }
                llm_response_free(&resp);
                break;
            }

            ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1, resp.call_count);

            /* Append assistant message with content array */
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            /* Execute tools and append results */
            bool tool_budget_exceeded = false;
            TickType_t tools_stage_start = xTaskGetTickCount();
            cJSON *tool_results = build_tool_results(&resp, &msg, tool_output, TOOL_OUTPUT_SIZE,
                                                     &tool_budget_exceeded);
            tools_ms += elapsed_ms(tools_stage_start, xTaskGetTickCount());
            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results);
            cJSON_AddItemToArray(messages, result_msg);

            llm_response_free(&resp);
            if (tool_budget_exceeded) {
                ESP_LOGW(TAG, "Tool result budget exceeded (%d bytes total cap)",
                         MIMI_AGENT_TOOL_RESULTS_TOTAL_MAX);
                hit_tool_budget = true;
                final_text = strdup("工具返回内容太大了，请把任务范围缩小一点。");
                break;
            }
            iteration++;
        }

        if (!final_text && iteration >= MIMI_AGENT_MAX_TOOL_ITER) {
            ESP_LOGW(TAG, "Tool iteration limit reached (%d)", MIMI_AGENT_MAX_TOOL_ITER);
            hit_iter_limit = true;
            final_text = strdup("工具调用次数到上限了，请换个更简短的问法。");
        }

        cJSON_Delete(messages);

        /* 5. Send response */
        TickType_t outbound_stage_start = xTaskGetTickCount();
        if (final_text && final_text[0]) {
            /* Save to session (only user text + final assistant text) */
            session_append(msg.chat_id, "user", user_text_for_llm);
            session_append(msg.chat_id, "assistant", final_text);

            /* Push response to outbound */
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = final_text;  /* transfer ownership */
            if (message_bus_push_outbound(&out) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to enqueue final response for %s:%s", out.channel, out.chat_id);
                outbound_enqueue_failed = true;
                message_bus_msg_free(&out);
            }
        } else {
            /* Error or empty response */
            free(final_text);
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup("Sorry, I encountered an error.");
            if (out.content) {
                if (message_bus_push_outbound(&out) != ESP_OK) {
                    outbound_enqueue_failed = true;
                    message_bus_msg_free(&out);
                }
            } else {
                outbound_enqueue_failed = true;
            }
            display_set_display_status(DISPLAY_STATUS_ERROR);
        }
        outbound_ms = elapsed_ms(outbound_stage_start, xTaskGetTickCount());

        /* Free inbound message content */
        free(user_content);
        message_bus_msg_free(&msg);

        /* Reset display to idle after processing */
        display_set_display_status(DISPLAY_STATUS_IDLE);

        uint32_t total_ms = elapsed_ms(turn_start_tick, xTaskGetTickCount());
        bool success = produced_final_response &&
                       !hit_timeout &&
                       !hit_context_budget &&
                       !hit_tool_budget &&
                       !hit_iter_limit &&
                       !hit_llm_error &&
                       !outbound_enqueue_failed;

        record_turn_stats(run_id, success, total_ms, context_ms, llm_ms, tools_ms, outbound_ms,
                          hit_timeout, hit_context_budget, hit_tool_budget, hit_iter_limit,
                          hit_llm_error, outbound_enqueue_failed);

        ESP_LOGI(TAG,
                 "run=%" PRIu32 " done success=%d total=%" PRIu32 "ms context=%" PRIu32 "ms llm=%" PRIu32
                 "ms tools=%" PRIu32 "ms outbound=%" PRIu32 "ms iter=%d",
                 run_id, success ? 1 : 0, total_ms, context_ms, llm_ms, tools_ms, outbound_ms, iteration);

        /* Log memory status */
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

esp_err_t agent_loop_init(void)
{
    ESP_ERROR_CHECK(control_plane_init());
    ESP_LOGI(TAG, "Agent loop initialized");
    return ESP_OK;
}

esp_err_t agent_loop_start(void)
{
    const uint32_t stack_candidates[] = {
        MIMI_AGENT_STACK,
        20 * 1024,
        16 * 1024,
        14 * 1024,
        12 * 1024,
    };

    for (size_t i = 0; i < (sizeof(stack_candidates) / sizeof(stack_candidates[0])); i++) {
        uint32_t stack_size = stack_candidates[i];
        BaseType_t ret = xTaskCreatePinnedToCore(
            agent_loop_task, "agent_loop",
            stack_size, NULL,
            MIMI_AGENT_PRIO, NULL, MIMI_AGENT_CORE);

        if (ret == pdPASS) {
            ESP_LOGI(TAG, "agent_loop task created with stack=%u bytes", (unsigned)stack_size);
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "agent_loop create failed (stack=%u, free_internal=%u, largest_internal=%u), retrying...",
                 (unsigned)stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    return ESP_FAIL;
}

esp_err_t agent_loop_get_stats(agent_stats_t *out_stats)
{
    if (!out_stats) return ESP_ERR_INVALID_ARG;

    memset(out_stats, 0, sizeof(*out_stats));

    portENTER_CRITICAL(&s_stats_lock);
    out_stats->total_turns = s_stats.total_turns;
    out_stats->success_turns = s_stats.success_turns;
    out_stats->failed_turns = s_stats.failed_turns;
    out_stats->timeout_turns = s_stats.timeout_turns;
    out_stats->context_budget_hits = s_stats.context_budget_hits;
    out_stats->tool_budget_hits = s_stats.tool_budget_hits;
    out_stats->iter_limit_hits = s_stats.iter_limit_hits;
    out_stats->llm_error_turns = s_stats.llm_error_turns;
    out_stats->outbound_enqueue_failures = s_stats.outbound_enqueue_failures;
    out_stats->outbound_send_failures = s_stats.outbound_send_failures;
    out_stats->max_turn_latency_ms = s_stats.max_turn_latency_ms;
    out_stats->last_turn_latency_ms = s_stats.last_turn_latency_ms;
    out_stats->last_run_id = s_stats.last_run_id;

    if (s_stats.total_turns > 0) {
        out_stats->avg_turn_latency_ms = (uint32_t)(s_stats.sum_turn_latency_ms / s_stats.total_turns);
        out_stats->avg_context_ms = (uint32_t)(s_stats.sum_context_ms / s_stats.total_turns);
        out_stats->avg_llm_ms = (uint32_t)(s_stats.sum_llm_ms / s_stats.total_turns);
        out_stats->avg_tools_ms = (uint32_t)(s_stats.sum_tools_ms / s_stats.total_turns);
        out_stats->avg_outbound_ms = (uint32_t)(s_stats.sum_outbound_ms / s_stats.total_turns);
    }
    portEXIT_CRITICAL(&s_stats_lock);

    return ESP_OK;
}

void agent_loop_record_outbound_send_failure(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.outbound_send_failures++;
    portEXIT_CRITICAL(&s_stats_lock);
}
