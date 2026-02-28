#include "serial_cli.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "bus/message_bus.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "tools/tool_web_search.h"
#include "security/access_control.h"
#include "heartbeat/heartbeat_service.h"
#include "heartbeat/heartbeat.h"
#include "cron/cron_service.h"
#include "bus/message_bus.h"
#include "audio/audio.h"
#include "voice/voice_channel.h"
#include "control/control_plane.h"
#include "skills/skill_loader.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "argtable3/argtable3.h"

static const char *TAG = "cli";

/* --- wifi_set command --- */
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_set_args;

static int cmd_wifi_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_set_args.end, argv[0]);
        return 1;
    }
    wifi_manager_set_credentials(wifi_set_args.ssid->sval[0],
                                  wifi_set_args.password->sval[0]);
    printf("WiFi credentials saved. Restart to apply.\n");
    return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv)
{
    printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
    printf("IP: %s\n", wifi_manager_get_ip());
    return 0;
}

/* --- set_tg_token command --- */
static struct {
    struct arg_str *token;
    struct arg_end *end;
} tg_token_args;

static int cmd_set_tg_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_token_args.end, argv[0]);
        return 1;
    }
    telegram_set_token(tg_token_args.token->sval[0]);
    printf("Telegram bot token saved.\n");
    return 0;
}

/* --- set_api_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, api_key_args.end, argv[0]);
        return 1;
    }
    llm_set_api_key(api_key_args.key->sval[0]);
    printf("API key saved.\n");
    return 0;
}

/* --- set_model command --- */
static struct {
    struct arg_str *model;
    struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, model_args.end, argv[0]);
        return 1;
    }
    llm_set_model(model_args.model->sval[0]);
    printf("Model set.\n");
    return 0;
}

/* --- set_model_provider command --- */
static struct {
    struct arg_str *provider;
    struct arg_end *end;
} provider_args;

static int cmd_set_model_provider(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&provider_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, provider_args.end, argv[0]);
        return 1;
    }
    llm_set_provider(provider_args.provider->sval[0]);
    printf("Model provider set.\n");
    return 0;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv)
{
    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }
    if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
        printf("=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        printf("MEMORY.md is empty or not found.\n");
    }
    free(buf);
    return 0;
}

/* --- memory_write command --- */
static struct {
    struct arg_str *content;
    struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, memory_write_args.end, argv[0]);
        return 1;
    }
    memory_write_long_term(memory_write_args.content->sval[0]);
    printf("MEMORY.md updated.\n");
    return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv)
{
    printf("Sessions:\n");
    session_list();
    return 0;
}

/* --- session_clear command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, session_clear_args.end, argv[0]);
        return 1;
    }
    if (session_clear(session_clear_args.chat_id->sval[0]) == ESP_OK) {
        printf("Session cleared.\n");
    } else {
        printf("Session not found.\n");
    }
    return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv)
{
    printf("Internal free: %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("PSRAM free:    %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Total free:    %d bytes\n",
           (int)esp_get_free_heap_size());
    return 0;
}

/* --- agent_stats command --- */
static int cmd_agent_stats(int argc, char **argv)
{
    agent_stats_t stats;
    esp_err_t err = agent_loop_get_stats(&stats);
    if (err != ESP_OK) {
        printf("Failed to get agent stats: %s\n", esp_err_to_name(err));
        return 1;
    }

    uint32_t success_permille = 0;
    if (stats.total_turns > 0) {
        success_permille = (stats.success_turns * 1000U) / stats.total_turns;
    }

    printf("=== Agent Stats ===\n");
    printf("  Last Run ID        : %" PRIu32 "\n", stats.last_run_id);
    printf("  Total Turns        : %" PRIu32 "\n", stats.total_turns);
    printf("  Success Turns      : %" PRIu32 "\n", stats.success_turns);
    printf("  Failed Turns       : %" PRIu32 "\n", stats.failed_turns);
    printf("  Success Rate       : %" PRIu32 ".%" PRIu32 "%%\n",
           success_permille / 10U, success_permille % 10U);
    printf("  Timeout Turns      : %" PRIu32 "\n", stats.timeout_turns);
    printf("  Context Budget Hit : %" PRIu32 "\n", stats.context_budget_hits);
    printf("  Tool Budget Hit    : %" PRIu32 "\n", stats.tool_budget_hits);
    printf("  Iter Limit Hit     : %" PRIu32 "\n", stats.iter_limit_hits);
    printf("  LLM Error Turns    : %" PRIu32 "\n", stats.llm_error_turns);
    printf("  Outbound Q Fail    : %" PRIu32 "\n", stats.outbound_enqueue_failures);
    printf("  Outbound Send Fail : %" PRIu32 "\n", stats.outbound_send_failures);
    printf("  Last Latency (ms)  : %" PRIu32 "\n", stats.last_turn_latency_ms);
    printf("  Avg Latency (ms)   : %" PRIu32 "\n", stats.avg_turn_latency_ms);
    printf("  Max Latency (ms)   : %" PRIu32 "\n", stats.max_turn_latency_ms);
    printf("  Avg Context (ms)   : %" PRIu32 "\n", stats.avg_context_ms);
    printf("  Avg LLM (ms)       : %" PRIu32 "\n", stats.avg_llm_ms);
    printf("  Avg Tools (ms)     : %" PRIu32 "\n", stats.avg_tools_ms);
    printf("  Avg Outbound (ms)  : %" PRIu32 "\n", stats.avg_outbound_ms);
    printf("===================\n");
    return 0;
}

/* --- heartbeat_status command --- */
static int cmd_heartbeat_status(int argc, char **argv)
{
#if MIMI_HEARTBEAT_ENABLED
    heartbeat_stats_t stats;
    esp_err_t err = heartbeat_service_get_stats(&stats);
    if (err != ESP_OK) {
        printf("Heartbeat not ready: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("=== Heartbeat Status ===\n");
    printf("  Enabled           : yes\n");
    printf("  Interval (sec)    : %d\n", MIMI_HEARTBEAT_INTERVAL_S);
    printf("  File              : %s\n", MIMI_HEARTBEAT_FILE);
    printf("  Total Runs        : %" PRIu32 "\n", stats.total_runs);
    printf("  Triggered Runs    : %" PRIu32 "\n", stats.triggered_runs);
    printf("  Enqueue Success   : %" PRIu32 "\n", stats.enqueue_success);
    printf("  Enqueue Failures  : %" PRIu32 "\n", stats.enqueue_failures);
    printf("  Skip No File      : %" PRIu32 "\n", stats.skipped_no_file);
    printf("  Skip Empty        : %" PRIu32 "\n", stats.skipped_empty);
    printf("  Skip Read Error   : %" PRIu32 "\n", stats.skipped_read_error);
    printf("  Last Run (unix)   : %" PRIu32 "\n", stats.last_run_unix);
    printf("========================\n");
    return 0;
#else
    printf("Heartbeat is disabled. Set MIMI_HEARTBEAT_ENABLED=1 in mimi_config.h\n");
    return 1;
#endif
}

/* --- heartbeat_now command --- */
static int cmd_heartbeat_now(int argc, char **argv)
{
#if MIMI_HEARTBEAT_ENABLED
    esp_err_t err = heartbeat_service_trigger_now();
    if (err != ESP_OK) {
        printf("Heartbeat trigger failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Heartbeat trigger requested.\n");
    return 0;
#else
    printf("Heartbeat is disabled. Set MIMI_HEARTBEAT_ENABLED=1 in mimi_config.h\n");
    return 1;
#endif
}

/* --- cron_set command --- */
static struct {
    struct arg_int *minutes;
    struct arg_str *task;
    struct arg_end *end;
} cron_set_args;

/* 兼容旧版单任务 cron CLI，映射到新版多任务 cron 服务 */
#define CLI_LEGACY_CRON_NAME "cli_schedule"

typedef struct {
    bool enabled;
    uint32_t interval_min;
    uint32_t total_runs;
    uint32_t triggered_runs;
    uint32_t enqueue_success;
    uint32_t enqueue_failures;
    uint32_t skipped_not_configured;
    uint32_t last_run_unix;
} cron_stats_t;

static bool find_legacy_cron_job(const cron_job_t **job_out)
{
    const cron_job_t *jobs = NULL;
    int count = 0;
    cron_list_jobs(&jobs, &count);
    for (int i = 0; i < count; i++) {
        if (strcmp(jobs[i].name, CLI_LEGACY_CRON_NAME) == 0) {
            if (job_out) *job_out = &jobs[i];
            return true;
        }
    }
    return false;
}

static esp_err_t cron_service_clear_schedule(void)
{
    while (true) {
        const cron_job_t *jobs = NULL;
        int count = 0;
        bool removed = false;

        cron_list_jobs(&jobs, &count);
        for (int i = 0; i < count; i++) {
            if (strcmp(jobs[i].name, CLI_LEGACY_CRON_NAME) == 0) {
                esp_err_t err = cron_remove_job(jobs[i].id);
                if (err != ESP_OK) {
                    return err;
                }
                removed = true;
                break;
            }
        }

        if (!removed) {
            return ESP_OK;
        }
    }
}

static esp_err_t cron_service_set_schedule(uint32_t minutes, const char *task)
{
    if (minutes == 0 || !task || !task[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = cron_service_clear_schedule();
    if (err != ESP_OK) {
        return err;
    }

    cron_job_t job;
    memset(&job, 0, sizeof(job));
    strncpy(job.name, CLI_LEGACY_CRON_NAME, sizeof(job.name) - 1);
    job.enabled = true;
    job.kind = CRON_KIND_EVERY;
    job.interval_s = minutes * 60U;
    strncpy(job.message, task, sizeof(job.message) - 1);
    strncpy(job.channel, MIMI_CHAN_SYSTEM, sizeof(job.channel) - 1);
    strncpy(job.chat_id, "cron", sizeof(job.chat_id) - 1);

    return cron_add_job(&job);
}

static esp_err_t cron_service_get_stats(cron_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(stats, 0, sizeof(*stats));
    const cron_job_t *job = NULL;
    if (find_legacy_cron_job(&job)) {
        stats->enabled = job->enabled;
        stats->interval_min = job->interval_s / 60U;
        stats->last_run_unix = (job->last_run > 0) ? (uint32_t)job->last_run : 0;
    }
    return ESP_OK;
}

static esp_err_t cron_service_get_task(char *task, size_t task_size)
{
    if (!task || task_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    task[0] = '\0';
    const cron_job_t *job = NULL;
    if (find_legacy_cron_job(&job)) {
        strncpy(task, job->message, task_size - 1);
    }
    return ESP_OK;
}

static esp_err_t cron_service_trigger_now(void)
{
    const cron_job_t *job = NULL;
    if (!find_legacy_cron_job(&job)) {
        return ESP_ERR_NOT_FOUND;
    }

    mimi_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, job->channel[0] ? job->channel : MIMI_CHAN_SYSTEM,
            sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, job->chat_id[0] ? job->chat_id : "cron",
            sizeof(msg.chat_id) - 1);
    msg.content = strdup(job->message);
    if (!msg.content) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err != ESP_OK) {
        free(msg.content);
    }
    return err;
}

static int cmd_cron_set(int argc, char **argv)
{
#if MIMI_CRON_ENABLED
    int nerrors = arg_parse(argc, argv, (void **)&cron_set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, cron_set_args.end, argv[0]);
        return 1;
    }

    int minutes = cron_set_args.minutes->ival[0];
    const char *task = cron_set_args.task->sval[0];
    if (minutes < (int)MIMI_CRON_MIN_INTERVAL_MIN || minutes > (int)MIMI_CRON_MAX_INTERVAL_MIN) {
        printf("Invalid minutes. Range: %d..%d\n",
               MIMI_CRON_MIN_INTERVAL_MIN, MIMI_CRON_MAX_INTERVAL_MIN);
        return 1;
    }

    esp_err_t err = cron_service_set_schedule((uint32_t)minutes, task);
    if (err != ESP_OK) {
        printf("Failed to set cron schedule: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Cron schedule set: every %d min.\n", minutes);
    return 0;
#else
    printf("Cron is disabled. Set MIMI_CRON_ENABLED=1 in mimi_config.h\n");
    return 1;
#endif
}

/* --- cron_clear command --- */
static int cmd_cron_clear(int argc, char **argv)
{
#if MIMI_CRON_ENABLED
    esp_err_t err = cron_service_clear_schedule();
    if (err != ESP_OK) {
        printf("Failed to clear cron schedule: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Cron schedule cleared.\n");
    return 0;
#else
    printf("Cron is disabled. Set MIMI_CRON_ENABLED=1 in mimi_config.h\n");
    return 1;
#endif
}

/* --- cron_status command --- */
static int cmd_cron_status(int argc, char **argv)
{
#if MIMI_CRON_ENABLED
    cron_stats_t stats;
    esp_err_t err = cron_service_get_stats(&stats);
    if (err != ESP_OK) {
        printf("Cron not ready: %s\n", esp_err_to_name(err));
        return 1;
    }

    char task[MIMI_CRON_TASK_MAX_BYTES + 1];
    err = cron_service_get_task(task, sizeof(task));
    if (err != ESP_OK) {
        printf("Cron task read failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("=== Cron Status ===\n");
    printf("  Enabled           : %s\n", stats.enabled ? "yes" : "no");
    printf("  Interval (min)    : %" PRIu32 "\n", stats.interval_min);
    printf("  Total Runs        : %" PRIu32 "\n", stats.total_runs);
    printf("  Triggered Runs    : %" PRIu32 "\n", stats.triggered_runs);
    printf("  Enqueue Success   : %" PRIu32 "\n", stats.enqueue_success);
    printf("  Enqueue Failures  : %" PRIu32 "\n", stats.enqueue_failures);
    printf("  Skip Not Config   : %" PRIu32 "\n", stats.skipped_not_configured);
    printf("  Last Run (unix)   : %" PRIu32 "\n", stats.last_run_unix);
    printf("  Task              : %s\n", task[0] ? task : "(empty)");
    printf("===================\n");
    return 0;
#else
    printf("Cron is disabled. Set MIMI_CRON_ENABLED=1 in mimi_config.h\n");
    return 1;
#endif
}

/* --- cron_now command --- */
static int cmd_cron_now(int argc, char **argv)
{
#if MIMI_CRON_ENABLED
    esp_err_t err = cron_service_trigger_now();
    if (err != ESP_OK) {
        printf("Cron trigger failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Cron trigger requested.\n");
    return 0;
#else
    printf("Cron is disabled. Set MIMI_CRON_ENABLED=1 in mimi_config.h\n");
    return 1;
#endif
}

/* --- set_proxy command --- */
static struct {
    struct arg_str *host;
    struct arg_int *port;
    struct arg_str *type;
    struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, proxy_args.end, argv[0]);
        return 1;
    }
    const char *proxy_type = "http";
    if (proxy_args.type->count > 0 && proxy_args.type->sval[0] && proxy_args.type->sval[0][0]) {
        proxy_type = proxy_args.type->sval[0];
    }
    if (strcmp(proxy_type, "http") != 0 && strcmp(proxy_type, "socks5") != 0) {
        printf("Invalid proxy type: %s. Use http or socks5.\n", proxy_type);
        return 1;
    }

    http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0], proxy_type);
    printf("Proxy set. Restart to apply.\n");
    return 0;
}

/* --- clear_proxy command --- */
static int cmd_clear_proxy(int argc, char **argv)
{
    http_proxy_clear();
    printf("Proxy cleared. Restart to apply.\n");
    return 0;
}

/* --- set_search_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, search_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_key(search_key_args.key->sval[0]);
    printf("Search API key saved.\n");
    return 0;
}

/* --- set_allow_from command --- */
static struct {
    struct arg_str *allow_from;
    struct arg_end *end;
} allow_from_args;

static int cmd_set_allow_from(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&allow_from_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, allow_from_args.end, argv[0]);
        return 1;
    }

    esp_err_t err = access_control_set_allow_from(allow_from_args.allow_from->sval[0]);
    if (err != ESP_OK) {
        printf("Failed to set allow_from: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Telegram allow_from set: %s\n", allow_from_args.allow_from->sval[0]);
    return 0;
}

/* --- clear_allow_from command --- */
static int cmd_clear_allow_from(int argc, char **argv)
{
    esp_err_t err = access_control_clear_allow_from();
    if (err != ESP_OK) {
        printf("Failed to clear allow_from: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Telegram allow_from cleared (open mode).\n");
    return 0;
}

/* --- set_ws_token command --- */
static struct {
    struct arg_str *token;
    struct arg_end *end;
} ws_token_args;

static int cmd_set_ws_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&ws_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, ws_token_args.end, argv[0]);
        return 1;
    }

    esp_err_t err = access_control_set_ws_token(ws_token_args.token->sval[0]);
    if (err != ESP_OK) {
        printf("Failed to set WS token: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("WS token saved.\n");
    return 0;
}

/* --- clear_ws_token command --- */
static int cmd_clear_ws_token(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = access_control_clear_ws_token();
    if (err != ESP_OK) {
        printf("Failed to clear WS token: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("WS token cleared (open mode).\n");
    return 0;
}

/* --- wifi_scan command --- */
static int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_manager_scan_and_print();
    return 0;
}

/* --- skill_list command --- */
static int cmd_skill_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }

    size_t n = skill_loader_build_summary(buf, 4096);
    if (n == 0) {
        printf("No skills found under " MIMI_SKILLS_PREFIX ".\n");
    } else {
        printf("=== Skills ===\n%s", buf);
    }
    free(buf);
    return 0;
}

/* --- skill_show command --- */
static struct {
    struct arg_str *name;
    struct arg_end *end;
} skill_show_args;

static bool has_md_suffix(const char *name)
{
    size_t len = strlen(name);
    return (len >= 3) && strcmp(name + len - 3, ".md") == 0;
}

static bool build_skill_path(const char *name, char *out, size_t out_size)
{
    if (!name || !name[0]) return false;
    if (strstr(name, "..") != NULL) return false;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return false;

    if (has_md_suffix(name)) {
        snprintf(out, out_size, MIMI_SKILLS_PREFIX "%s", name);
    } else {
        snprintf(out, out_size, MIMI_SKILLS_PREFIX "%s.md", name);
    }
    return true;
}

static int cmd_skill_show(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_show_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_show_args.end, argv[0]);
        return 1;
    }

    char path[128];
    if (!build_skill_path(skill_show_args.name->sval[0], path, sizeof(path))) {
        printf("Invalid skill name.\n");
        return 1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Skill not found: %s\n", path);
        return 1;
    }

    printf("=== %s ===\n", path);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        fputs(line, stdout);
    }
    fclose(f);
    printf("\n============\n");
    return 0;
}

/* --- skill_search command --- */
static struct {
    struct arg_str *keyword;
    struct arg_end *end;
} skill_search_args;

static bool contains_nocase(const char *text, const char *keyword)
{
    if (!text || !keyword || !keyword[0]) return false;

    size_t key_len = strlen(keyword);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < key_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)keyword[i])) {
            i++;
        }
        if (i == key_len) return true;
    }
    return false;
}

static int cmd_skill_search(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_search_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_search_args.end, argv[0]);
        return 1;
    }

    const char *keyword = skill_search_args.keyword->sval[0];
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        printf("Cannot open " MIMI_SPIFFS_BASE ".\n");
        return 1;
    }

    const char *prefix = "skills/";
    const size_t prefix_len = strlen(prefix);
    int matches = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        if (strncmp(name, prefix, prefix_len) != 0) continue;
        if (name_len < prefix_len + 4) continue;
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        char full_path[296];
        snprintf(full_path, sizeof(full_path), MIMI_SPIFFS_BASE "/%s", name);

        bool file_matched = contains_nocase(name, keyword);
        int matched_line = 0;

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        char line[256];
        int line_no = 0;
        while (!file_matched && fgets(line, sizeof(line), f)) {
            line_no++;
            if (contains_nocase(line, keyword)) {
                file_matched = true;
                matched_line = line_no;
            }
        }
        fclose(f);

        if (file_matched) {
            matches++;
            if (matched_line > 0) {
                printf("- %s (matched at line %d)\n", full_path, matched_line);
            } else {
                printf("- %s (matched in filename)\n", full_path);
            }
        }
    }

    closedir(dir);
    if (matches == 0) {
        printf("No skills matched keyword: %s\n", keyword);
    } else {
        printf("Total matches: %d\n", matches);
    }
    return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask)
{
    char nvs_val[128] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    /* NVS takes highest priority */
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(nvs_val);
        if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    /* Fall back to build-time value */
    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
        printf("  %-14s: %.4s****  [%s]\n", label, display, source);
    } else {
        printf("  %-14s: %s  [%s]\n", label, display, source);
    }
}

static int cmd_config_show(int argc, char **argv)
{
    printf("=== Current Configuration ===\n");
    print_config("WiFi SSID",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID,     MIMI_SECRET_WIFI_SSID,  false);
    print_config("WiFi Pass",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS,     MIMI_SECRET_WIFI_PASS,  true);
    print_config("TG Token",   MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN, MIMI_SECRET_TG_TOKEN,   true);
    print_config("API Key",    MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_API_KEY,    true);
    print_config("Model",      MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL,    MIMI_SECRET_MODEL,      false);
    print_config("Provider",   MIMI_NVS_LLM,    MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER, false);
    print_config("Proxy Host", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST, false);
    print_config("Proxy Port", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT, false);
    print_config("Search Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_SEARCH_KEY, true);
    print_config("Allow From", MIMI_NVS_SECURITY, MIMI_NVS_KEY_ALLOW_FROM, MIMI_SECRET_ALLOW_FROM, false);
    print_config("WS Token",  MIMI_NVS_SECURITY, MIMI_NVS_KEY_WS_TOKEN, MIMI_SECRET_WS_TOKEN, true);
    print_config("Voice GW",   MIMI_NVS_VOICE,  MIMI_NVS_KEY_VOICE_GW, MIMI_VOICE_GATEWAY_URL, false);
    printf("  %-14s: %u%%  [runtime]\n", "Volume", (unsigned)audio_get_volume());
    printf("=============================\n");
    return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv)
{
    const char *namespaces[] = {
        MIMI_NVS_WIFI, MIMI_NVS_TG, MIMI_NVS_LLM, MIMI_NVS_PROXY, MIMI_NVS_SEARCH,
        MIMI_NVS_VOICE, MIMI_NVS_SECURITY, MIMI_NVS_AUDIO
    };
    int ns_count = sizeof(namespaces) / sizeof(namespaces[0]);
    for (int i = 0; i < ns_count; i++) {
        nvs_handle_t nvs;
        if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    printf("All NVS config cleared. Build-time defaults will be used on restart.\n");
    return 0;
}

/* --- heartbeat_trigger command --- */
static int cmd_heartbeat_trigger(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Checking HEARTBEAT.md...\n");
    esp_err_t err = heartbeat_service_trigger_now();
    if (err == ESP_OK) {
        printf("Heartbeat trigger requested.\n");
        return 0;
    }
    printf("Heartbeat trigger failed: %s\n", esp_err_to_name(err));
    return 1;
}

/* --- cron_start command --- */
static int cmd_cron_start(int argc, char **argv)
{
    esp_err_t err = cron_service_start();
    if (err == ESP_OK) {
        printf("Cron service started.\n");
        return 0;
    }

    printf("Failed to start cron service: %s\n", esp_err_to_name(err));
    return 1;
}

static int cmd_tool_exec(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: tool_exec <name> [json]\n");
        return 1;
    }

    const char *tool_name = argv[1];
    const char *input_json = (argc >= 3) ? argv[2] : "{}";

    char *output = calloc(1, 4096);
    if (!output) {
        printf("Out of memory.\n");
        return 1;
    }

    esp_err_t err = tool_registry_execute(tool_name, input_json, output, 4096);
    printf("tool_exec status: %s\n", esp_err_to_name(err));
    printf("%s\n", output[0] ? output : "(empty)");
    free(output);
    return (err == ESP_OK) ? 0 : 1;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    esp_restart();
    return 0;  /* unreachable */
}

/* --- set_voice_gw command --- */
static struct {
    struct arg_str *url;
    struct arg_end *end;
} voice_gw_args;

/* --- set_volume command --- */
static struct {
    struct arg_int *volume;
    struct arg_end *end;
} volume_args;

static struct {
    struct arg_int *temp_x10;
    struct arg_end *end;
} temp_event_args;

static struct {
    struct arg_str *query;
    struct arg_end *end;
} music_play_args;

static int cmd_set_voice_gw(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&voice_gw_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, voice_gw_args.end, argv[0]);
        return 1;
    }
    voice_channel_set_gateway(voice_gw_args.url->sval[0]);
    printf("Voice gateway URL saved: %s\n", voice_gw_args.url->sval[0]);
    return 0;
}

static int cmd_set_volume(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&volume_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, volume_args.end, argv[0]);
        return 1;
    }

    int volume = volume_args.volume->ival[0];
    if (volume < 0 || volume > 100) {
        printf("Invalid volume. Range: 0..100\n");
        return 1;
    }

    audio_set_volume((uint8_t)volume);
    printf("Volume set to %d%%\n", volume);
    return 0;
}

static int cmd_get_volume(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Current volume: %u%%\n", (unsigned)audio_get_volume());
    return 0;
}

static int cmd_control_audit(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    control_audit_entry_t entries[10];
    size_t n = control_plane_get_recent_audits(entries, 10);
    if (n == 0) {
        printf("No control audit records.\n");
        return 0;
    }

    printf("=== Control Audit (latest %u) ===\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        const control_audit_entry_t *e = &entries[i];
        printf("[%u] ts=%lld req=%s cap=%s ok=%d dedup=%d\n",
               (unsigned)(i + 1),
               (long long)e->ts_ms,
               e->request_id[0] ? e->request_id : "-",
               e->capability[0] ? e->capability : "-",
               e->success ? 1 : 0,
               e->dedup_hit ? 1 : 0);
        printf("     %s\n", e->summary[0] ? e->summary : "-");
    }
    printf("=================================\n");
    return 0;
}

static int cmd_alarm_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    control_alarm_info_t alarms[MIMI_CONTROL_MAX_ALARMS];
    size_t n = control_plane_get_active_alarms(alarms, MIMI_CONTROL_MAX_ALARMS);
    if (n == 0) {
        printf("No active alarms.\n");
        return 0;
    }

    printf("=== Active Alarms (%u) ===\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        const control_alarm_info_t *a = &alarms[i];
        printf("  #%" PRIu32 "  remaining=%" PRIu32 " ms  target=%s:%s  note=%s\n",
               a->alarm_id,
               a->remaining_ms,
               a->channel[0] ? a->channel : "-",
               a->chat_id[0] ? a->chat_id : "-",
               a->note[0] ? a->note : "-");
    }
    printf("===========================\n");
    return 0;
}

static int cmd_temp_rule_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    control_temp_rule_info_t rules[MIMI_CONTROL_MAX_TEMP_RULES];
    size_t n = control_plane_get_temperature_rules(rules, MIMI_CONTROL_MAX_TEMP_RULES);
    if (n == 0) {
        printf("No temperature rules.\n");
        return 0;
    }

    printf("=== Temperature Rules (%u) ===\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        const control_temp_rule_info_t *r = &rules[i];
        const char *cmp = (r->comparator == 1) ? ">=" : "<=";
        if (r->action_type == 2) {
            printf("  #%" PRIu32 "  when temp %s %d.%d C  -> set_volume=%d%%\n",
                   r->rule_id, cmp, r->threshold_x10 / 10, abs(r->threshold_x10 % 10),
                   r->action_value);
        } else {
            printf("  #%" PRIu32 "  when temp %s %d.%d C  -> remind: %s\n",
                   r->rule_id, cmp, r->threshold_x10 / 10, abs(r->threshold_x10 % 10),
                   r->note[0] ? r->note : "-");
        }
    }
    printf("===============================\n");
    return 0;
}

static int cmd_temp_event(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&temp_event_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, temp_event_args.end, argv[0]);
        return 1;
    }

    int temp_x10 = temp_event_args.temp_x10->ival[0];
    esp_err_t err = control_plane_handle_temperature_event(temp_x10);
    if (err != ESP_OK) {
        printf("Temperature event failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Temperature event injected: %d.%d C\n", temp_x10 / 10, abs(temp_x10 % 10));
    return 0;
}

static int cmd_music_play(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&music_play_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, music_play_args.end, argv[0]);
        return 1;
    }

    const char *query = music_play_args.query->sval[0];
    esp_err_t err = voice_channel_play_music(query);
    if (err != ESP_OK) {
        printf("Music play failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Music playback requested: %s\n", query);
    return 0;
}

static int cmd_music_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t err = voice_channel_stop_music();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("Music stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Music playback stopped.\n");
    return 0;
}

/* --- audio_test command: play a 1kHz sine wave for 1 second --- */
static int cmd_audio_test(int argc, char **argv)
{
#if MIMI_AUDIO_ENABLED
    printf("Playing 1kHz test tone (1 second)...\n");

    const int sample_rate = MIMI_AUDIO_SPK_SAMPLE_RATE;
    const int duration_ms = 1000;
    const int num_samples = sample_rate * duration_ms / 1000;
    const float freq = 1000.0f;
    const float amplitude = 16000.0f;  /* ~50% of int16 max */

    size_t buf_size = num_samples * sizeof(int16_t);
    int16_t *buf = malloc(buf_size);
    if (!buf) {
        printf("Out of memory for audio buffer (%d bytes)\n", (int)buf_size);
        return 1;
    }

    for (int i = 0; i < num_samples; i++) {
        buf[i] = (int16_t)(amplitude * sinf(2.0f * M_PI * freq * i / sample_rate));
    }

    esp_err_t ret = audio_play((const uint8_t *)buf, buf_size);
    free(buf);

    if (ret != ESP_OK) {
        printf("Playback failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Done.\n");
    return 0;
#else
    printf("Audio is disabled. Set MIMI_AUDIO_ENABLED=1 in mimi_config.h\n");
    return 1;
#endif
}

/* --- mic_test command: read mic for 2 seconds and print RMS levels --- */
static int cmd_mic_test(int argc, char **argv)
{
#if MIMI_AUDIO_ENABLED
    printf("Recording from mic for 2 seconds...\n");
    printf("Speak or make noise to see levels.\n\n");

    esp_err_t ret = audio_start_listening();
    if (ret != ESP_OK) {
        printf("Failed to start mic: %s\n", esp_err_to_name(ret));
        return 1;
    }

    const int chunk_samples = 512;
    int16_t *buf = malloc(chunk_samples * sizeof(int16_t));
    if (!buf) {
        audio_stop_listening();
        printf("Out of memory\n");
        return 1;
    }

    const int iterations = (MIMI_AUDIO_MIC_SAMPLE_RATE * 2) / chunk_samples;

    for (int iter = 0; iter < iterations; iter++) {
        size_t bytes_read = 0;
        ret = audio_mic_read(buf, chunk_samples * sizeof(int16_t), &bytes_read, 1000);
        if (ret != ESP_OK) {
            printf("Read error: %s\n", esp_err_to_name(ret));
            break;
        }

        int samples = bytes_read / sizeof(int16_t);
        int64_t sum_sq = 0;
        int16_t peak = 0;
        for (int i = 0; i < samples; i++) {
            sum_sq += (int64_t)buf[i] * buf[i];
            int16_t abs_val = buf[i] < 0 ? -buf[i] : buf[i];
            if (abs_val > peak) peak = abs_val;
        }
        int rms = (int)sqrtf((float)sum_sq / samples);

        int bar_len = rms / 500;
        if (bar_len > 40) bar_len = 40;
        printf("RMS:%5d Peak:%5d |", rms, peak);
        for (int i = 0; i < bar_len; i++) printf("#");
        printf("\n");
    }

    free(buf);
    audio_stop_listening();
    printf("\nMic test done.\n");
    return 0;
#else
    printf("Audio is disabled. Set MIMI_AUDIO_ENABLED=1 in mimi_config.h\n");
    return 1;
#endif
}

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mimi> ";
    repl_config.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    /* Register commands */
    esp_console_register_help_command();

    /* set_wifi */
    wifi_set_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi SSID");
    wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "WiFi password");
    wifi_set_args.end = arg_end(2);
    esp_console_cmd_t wifi_set_cmd = {
        .command = "set_wifi",
        .help = "Set WiFi SSID and password (e.g. set_wifi MySSID MyPass)",
        .func = &cmd_wifi_set,
        .argtable = &wifi_set_args,
    };
    esp_console_cmd_register(&wifi_set_cmd);

    /* wifi_status */
    esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status",
        .help = "Show WiFi connection status",
        .func = &cmd_wifi_status,
    };
    esp_console_cmd_register(&wifi_status_cmd);

    /* wifi_scan */
    esp_console_cmd_t wifi_scan_cmd = {
        .command = "wifi_scan",
        .help = "Scan and list nearby WiFi APs",
        .func = &cmd_wifi_scan,
    };
    esp_console_cmd_register(&wifi_scan_cmd);

    /* set_tg_token */
    tg_token_args.token = arg_str1(NULL, NULL, "<token>", "Telegram bot token");
    tg_token_args.end = arg_end(1);
    esp_console_cmd_t tg_token_cmd = {
        .command = "set_tg_token",
        .help = "Set Telegram bot token",
        .func = &cmd_set_tg_token,
        .argtable = &tg_token_args,
    };
    esp_console_cmd_register(&tg_token_cmd);

    /* set_api_key */
    api_key_args.key = arg_str1(NULL, NULL, "<key>", "LLM API key");
    api_key_args.end = arg_end(1);
    esp_console_cmd_t api_key_cmd = {
        .command = "set_api_key",
        .help = "Set LLM API key",
        .func = &cmd_set_api_key,
        .argtable = &api_key_args,
    };
    esp_console_cmd_register(&api_key_cmd);

    /* set_model */
    model_args.model = arg_str1(NULL, NULL, "<model>", "Model identifier");
    model_args.end = arg_end(1);
    esp_console_cmd_t model_cmd = {
        .command = "set_model",
        .help = "Set LLM model (default: " MIMI_LLM_DEFAULT_MODEL ")",
        .func = &cmd_set_model,
        .argtable = &model_args,
    };
    esp_console_cmd_register(&model_cmd);

    /* set_model_provider */
    provider_args.provider = arg_str1(NULL, NULL, "<provider>", "Model provider (anthropic|openai)");
    provider_args.end = arg_end(1);
    esp_console_cmd_t provider_cmd = {
        .command = "set_model_provider",
        .help = "Set LLM model provider (default: " MIMI_LLM_PROVIDER_DEFAULT ")",
        .func = &cmd_set_model_provider,
        .argtable = &provider_args,
    };
    esp_console_cmd_register(&provider_cmd);

    /* skill_list */
    esp_console_cmd_t skill_list_cmd = {
        .command = "skill_list",
        .help = "List installed skills from " MIMI_SKILLS_PREFIX,
        .func = &cmd_skill_list,
    };
    esp_console_cmd_register(&skill_list_cmd);

    /* skill_show */
    skill_show_args.name = arg_str1(NULL, NULL, "<name>", "Skill name (e.g. weather or weather.md)");
    skill_show_args.end = arg_end(1);
    esp_console_cmd_t skill_show_cmd = {
        .command = "skill_show",
        .help = "Print full content of one skill file",
        .func = &cmd_skill_show,
        .argtable = &skill_show_args,
    };
    esp_console_cmd_register(&skill_show_cmd);

    /* skill_search */
    skill_search_args.keyword = arg_str1(NULL, NULL, "<keyword>", "Keyword to search in skills");
    skill_search_args.end = arg_end(1);
    esp_console_cmd_t skill_search_cmd = {
        .command = "skill_search",
        .help = "Search skill files by keyword (filename + content)",
        .func = &cmd_skill_search,
        .argtable = &skill_search_args,
    };
    esp_console_cmd_register(&skill_search_cmd);

    /* memory_read */
    esp_console_cmd_t mem_read_cmd = {
        .command = "memory_read",
        .help = "Read MEMORY.md",
        .func = &cmd_memory_read,
    };
    esp_console_cmd_register(&mem_read_cmd);

    /* memory_write */
    memory_write_args.content = arg_str1(NULL, NULL, "<content>", "Content to write");
    memory_write_args.end = arg_end(1);
    esp_console_cmd_t mem_write_cmd = {
        .command = "memory_write",
        .help = "Write to MEMORY.md",
        .func = &cmd_memory_write,
        .argtable = &memory_write_args,
    };
    esp_console_cmd_register(&mem_write_cmd);

    /* session_list */
    esp_console_cmd_t sess_list_cmd = {
        .command = "session_list",
        .help = "List all sessions",
        .func = &cmd_session_list,
    };
    esp_console_cmd_register(&sess_list_cmd);

    /* session_clear */
    session_clear_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Chat ID to clear");
    session_clear_args.end = arg_end(1);
    esp_console_cmd_t sess_clear_cmd = {
        .command = "session_clear",
        .help = "Clear a session",
        .func = &cmd_session_clear,
        .argtable = &session_clear_args,
    };
    esp_console_cmd_register(&sess_clear_cmd);

    /* heap_info */
    esp_console_cmd_t heap_cmd = {
        .command = "heap_info",
        .help = "Show heap memory usage",
        .func = &cmd_heap_info,
    };
    esp_console_cmd_register(&heap_cmd);

    /* agent_stats */
    esp_console_cmd_t agent_stats_cmd = {
        .command = "agent_stats",
        .help = "Show agent diagnostics (success rate, latency, failures)",
        .func = &cmd_agent_stats,
    };
    esp_console_cmd_register(&agent_stats_cmd);

    /* heartbeat_status */
    esp_console_cmd_t heartbeat_status_cmd = {
        .command = "heartbeat_status",
        .help = "Show heartbeat diagnostics and counters",
        .func = &cmd_heartbeat_status,
    };
    esp_console_cmd_register(&heartbeat_status_cmd);

    /* heartbeat_now */
    esp_console_cmd_t heartbeat_now_cmd = {
        .command = "heartbeat_now",
        .help = "Trigger heartbeat task immediately",
        .func = &cmd_heartbeat_now,
    };
    esp_console_cmd_register(&heartbeat_now_cmd);

    /* cron_set */
    cron_set_args.minutes = arg_int1(NULL, NULL, "<minutes>", "Run interval in minutes");
    cron_set_args.task = arg_str1(NULL, NULL, "<task>", "Task text for scheduled agent run");
    cron_set_args.end = arg_end(2);
    esp_console_cmd_t cron_set_cmd = {
        .command = "cron_set",
        .help = "Set cron schedule (example: cron_set 30 \"check agent health\")",
        .func = &cmd_cron_set,
        .argtable = &cron_set_args,
    };
    esp_console_cmd_register(&cron_set_cmd);

    /* cron_clear */
    esp_console_cmd_t cron_clear_cmd = {
        .command = "cron_clear",
        .help = "Clear cron schedule",
        .func = &cmd_cron_clear,
    };
    esp_console_cmd_register(&cron_clear_cmd);

    /* cron_status */
    esp_console_cmd_t cron_status_cmd = {
        .command = "cron_status",
        .help = "Show cron schedule and counters",
        .func = &cmd_cron_status,
    };
    esp_console_cmd_register(&cron_status_cmd);

    /* cron_now */
    esp_console_cmd_t cron_now_cmd = {
        .command = "cron_now",
        .help = "Trigger cron task immediately",
        .func = &cmd_cron_now,
    };
    esp_console_cmd_register(&cron_now_cmd);

    /* set_search_key */
    search_key_args.key = arg_str1(NULL, NULL, "<key>", "Brave Search API key");
    search_key_args.end = arg_end(1);
    esp_console_cmd_t search_key_cmd = {
        .command = "set_search_key",
        .help = "Set Brave Search API key for web_search tool",
        .func = &cmd_set_search_key,
        .argtable = &search_key_args,
    };
    esp_console_cmd_register(&search_key_cmd);

    /* set_allow_from */
    allow_from_args.allow_from = arg_str1(NULL, NULL, "<csv>", "Comma-separated sender IDs (or *)");
    allow_from_args.end = arg_end(1);
    esp_console_cmd_t allow_from_cmd = {
        .command = "set_allow_from",
        .help = "Set Telegram allowlist (example: set_allow_from 12345,67890)",
        .func = &cmd_set_allow_from,
        .argtable = &allow_from_args,
    };
    esp_console_cmd_register(&allow_from_cmd);

    /* clear_allow_from */
    esp_console_cmd_t clear_allow_from_cmd = {
        .command = "clear_allow_from",
        .help = "Clear Telegram allowlist (open mode)",
        .func = &cmd_clear_allow_from,
    };
    esp_console_cmd_register(&clear_allow_from_cmd);

    /* set_ws_token */
    ws_token_args.token = arg_str1(NULL, NULL, "<token>", "WebSocket auth token");
    ws_token_args.end = arg_end(1);
    esp_console_cmd_t ws_token_cmd = {
        .command = "set_ws_token",
        .help = "Set WebSocket auth token",
        .func = &cmd_set_ws_token,
        .argtable = &ws_token_args,
    };
    esp_console_cmd_register(&ws_token_cmd);

    /* clear_ws_token */
    esp_console_cmd_t clear_ws_token_cmd = {
        .command = "clear_ws_token",
        .help = "Clear WebSocket auth token (open mode)",
        .func = &cmd_clear_ws_token,
    };
    esp_console_cmd_register(&clear_ws_token_cmd);

    /* set_proxy */
    proxy_args.host = arg_str1(NULL, NULL, "<host>", "Proxy host/IP");
    proxy_args.port = arg_int1(NULL, NULL, "<port>", "Proxy port");
    proxy_args.type = arg_str0(NULL, NULL, "<type>", "Proxy type: http|socks5 (default: http)");
    proxy_args.end = arg_end(3);
    esp_console_cmd_t proxy_cmd = {
        .command = "set_proxy",
        .help = "Set proxy (e.g. set_proxy 192.168.1.83 7897 [http|socks5])",
        .func = &cmd_set_proxy,
        .argtable = &proxy_args,
    };
    esp_console_cmd_register(&proxy_cmd);

    /* clear_proxy */
    esp_console_cmd_t clear_proxy_cmd = {
        .command = "clear_proxy",
        .help = "Remove proxy configuration",
        .func = &cmd_clear_proxy,
    };
    esp_console_cmd_register(&clear_proxy_cmd);

    /* config_show */
    esp_console_cmd_t config_show_cmd = {
        .command = "config_show",
        .help = "Show current configuration (build-time + NVS)",
        .func = &cmd_config_show,
    };
    esp_console_cmd_register(&config_show_cmd);

    /* config_reset */
    esp_console_cmd_t config_reset_cmd = {
        .command = "config_reset",
        .help = "Clear all NVS overrides, revert to build-time defaults",
        .func = &cmd_config_reset,
    };
    esp_console_cmd_register(&config_reset_cmd);

    /* heartbeat_trigger */
    esp_console_cmd_t heartbeat_cmd = {
        .command = "heartbeat_trigger",
        .help = "Manually trigger a heartbeat check",
        .func = &cmd_heartbeat_trigger,
    };
    esp_console_cmd_register(&heartbeat_cmd);

    /* cron_start */
    esp_console_cmd_t cron_start_cmd = {
        .command = "cron_start",
        .help = "Start cron scheduler timer now",
        .func = &cmd_cron_start,
    };
    esp_console_cmd_register(&cron_start_cmd);

    /* tool_exec */
    esp_console_cmd_t tool_exec_cmd = {
        .command = "tool_exec",
        .help = "Execute a registered tool: tool_exec <name> '{...json...}'",
        .func = &cmd_tool_exec,
    };
    esp_console_cmd_register(&tool_exec_cmd);

    /* restart */
    esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Restart the device",
        .func = &cmd_restart,
    };
    esp_console_cmd_register(&restart_cmd);

    /* audio_test */
    esp_console_cmd_t audio_test_cmd = {
        .command = "audio_test",
        .help = "Play a 1kHz sine wave test tone for 1 second",
        .func = &cmd_audio_test,
    };
    esp_console_cmd_register(&audio_test_cmd);

    /* set_volume */
    volume_args.volume = arg_int1(NULL, NULL, "<0-100>", "Speaker volume percent");
    volume_args.end = arg_end(1);
    esp_console_cmd_t set_volume_cmd = {
        .command = "set_volume",
        .help = "Set speaker volume (0-100)",
        .func = &cmd_set_volume,
        .argtable = &volume_args,
    };
    esp_console_cmd_register(&set_volume_cmd);

    /* get_volume */
    esp_console_cmd_t get_volume_cmd = {
        .command = "get_volume",
        .help = "Get current speaker volume",
        .func = &cmd_get_volume,
    };
    esp_console_cmd_register(&get_volume_cmd);

    /* control_audit */
    esp_console_cmd_t control_audit_cmd = {
        .command = "control_audit",
        .help = "Show recent deterministic control audit logs",
        .func = &cmd_control_audit,
    };
    esp_console_cmd_register(&control_audit_cmd);

    /* alarm_list */
    esp_console_cmd_t alarm_list_cmd = {
        .command = "alarm_list",
        .help = "Show active local alarms in control plane",
        .func = &cmd_alarm_list,
    };
    esp_console_cmd_register(&alarm_list_cmd);

    /* temp_rule_list */
    esp_console_cmd_t temp_rule_list_cmd = {
        .command = "temp_rule_list",
        .help = "Show active temperature rules in control plane",
        .func = &cmd_temp_rule_list,
    };
    esp_console_cmd_register(&temp_rule_list_cmd);

    /* temp_event */
    temp_event_args.temp_x10 = arg_int1(NULL, NULL, "<temp_x10>", "Temperature x10 (e.g. 305 = 30.5C)");
    temp_event_args.end = arg_end(1);
    esp_console_cmd_t temp_event_cmd = {
        .command = "temp_event",
        .help = "Inject temperature event to evaluate deterministic temp rules",
        .func = &cmd_temp_event,
        .argtable = &temp_event_args,
    };
    esp_console_cmd_register(&temp_event_cmd);

    /* music_play */
    music_play_args.query = arg_str1(NULL, NULL, "<query>", "Music query/url");
    music_play_args.end = arg_end(1);
    esp_console_cmd_t music_play_cmd = {
        .command = "music_play",
        .help = "Request gateway music playback (example: music_play 周杰伦 稻香)",
        .func = &cmd_music_play,
        .argtable = &music_play_args,
    };
    esp_console_cmd_register(&music_play_cmd);

    /* music_stop */
    esp_console_cmd_t music_stop_cmd = {
        .command = "music_stop",
        .help = "Stop gateway music playback",
        .func = &cmd_music_stop,
    };
    esp_console_cmd_register(&music_stop_cmd);

    /* mic_test */
    esp_console_cmd_t mic_test_cmd = {
        .command = "mic_test",
        .help = "Read microphone for 2 seconds and print RMS levels",
        .func = &cmd_mic_test,
    };
    esp_console_cmd_register(&mic_test_cmd);

    /* set_voice_gw */
    voice_gw_args.url = arg_str1(NULL, NULL, "<url>", "Voice gateway URL (e.g. ws://192.168.1.100:8090)");
    voice_gw_args.end = arg_end(1);
    esp_console_cmd_t voice_gw_cmd = {
        .command = "set_voice_gw",
        .help = "Set voice gateway URL for STT/TTS",
        .func = &cmd_set_voice_gw,
        .argtable = &voice_gw_args,
    };
    esp_console_cmd_register(&voice_gw_cmd);

    /* Start REPL */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial CLI started");

    return ESP_OK;
}
