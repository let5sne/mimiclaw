#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "bus/message_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONTROL_CMD_NONE = 0,
    CONTROL_CMD_GET_VOLUME,
    CONTROL_CMD_SET_VOLUME,
    CONTROL_CMD_REBOOT,
    CONTROL_CMD_CREATE_ALARM,
    CONTROL_CMD_LIST_ALARM,
    CONTROL_CMD_CLEAR_ALARM,
    CONTROL_CMD_CREATE_TEMP_RULE,
    CONTROL_CMD_LIST_TEMP_RULE,
    CONTROL_CMD_CLEAR_TEMP_RULE,
} control_cmd_type_t;

typedef struct {
    control_cmd_type_t type;
    int target_value;
    bool relative;
    int delta_value;
    char capability[24];
    char request_id[40];
    char source_channel[16];
    char source_chat_id[32];
    uint32_t delay_ms;
    uint32_t alarm_id;
    char note[96];
    uint32_t temp_rule_id;
    int temp_threshold_x10;
    int temp_comparator;   /* 1: >=, -1: <= */
    int temp_action_type;  /* 1: 提醒, 2: 音量 */
    int temp_action_value; /* action_type=2 时有效 */
} control_command_t;

typedef struct {
    bool handled;
    bool success;
    bool from_rule;
    bool dedup_hit;
    bool pending_action;
    char capability[24];
    char request_id[40];
    char response_text[192];
    int before_value;
    int after_value;
} control_result_t;

typedef struct {
    int64_t ts_ms;
    char request_id[40];
    char capability[24];
    bool success;
    bool dedup_hit;
    char summary[128];
} control_audit_entry_t;

typedef struct {
    uint32_t alarm_id;
    uint32_t remaining_ms;
    char channel[16];
    char chat_id[32];
    char note[96];
} control_alarm_info_t;

typedef struct {
    uint32_t rule_id;
    int threshold_x10;
    int comparator;     /* 1: >=, -1: <= */
    int action_type;    /* 1: 提醒, 2: 音量 */
    int action_value;   /* action_type=2 时为目标音量(0~100) */
    char note[96];
} control_temp_rule_info_t;

/**
 * 初始化确定性控制平面（能力注册表）。
 */
esp_err_t control_plane_init(void);

/**
 * 规则优先入口：尝试把消息解析为确定性控制命令并执行。
 * - handled=false: 当前消息不属于控制命令，交由 LLM 流程处理
 * - handled=true: 已处理（success 决定执行是否成功）
 */
esp_err_t control_plane_try_handle_message(const mimi_msg_t *msg, control_result_t *out);

/**
 * 获取最近控制审计记录（按时间倒序，最多 max_entries 条）。
 */
size_t control_plane_get_recent_audits(control_audit_entry_t *out_entries, size_t max_entries);

/**
 * 获取当前活动闹钟列表（按创建顺序，最多 max_entries 条）。
 */
size_t control_plane_get_active_alarms(control_alarm_info_t *out_entries, size_t max_entries);

/**
 * 获取当前温度规则（按创建顺序，最多 max_entries 条）。
 */
size_t control_plane_get_temperature_rules(control_temp_rule_info_t *out_entries, size_t max_entries);

/**
 * 温度事件入口（temp_x10: 摄氏度 x10，例如 305 表示 30.5°C）。
 */
esp_err_t control_plane_handle_temperature_event(int temp_x10);

#ifdef __cplusplus
}
#endif
