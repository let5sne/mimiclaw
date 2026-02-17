#pragma once

#include "esp_err.h"
#include <stdint.h>

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
    uint32_t avg_turn_latency_ms;
    uint32_t max_turn_latency_ms;
    uint32_t last_turn_latency_ms;
    uint32_t avg_context_ms;
    uint32_t avg_llm_ms;
    uint32_t avg_tools_ms;
    uint32_t avg_outbound_ms;
    uint32_t last_run_id;
} agent_stats_t;

/**
 * Initialize the agent loop.
 */
esp_err_t agent_loop_init(void);

/**
 * Start the agent loop task (runs on Core 1).
 * Consumes from inbound queue, calls Claude API, pushes to outbound queue.
 */
esp_err_t agent_loop_start(void);

/**
 * 读取 Agent 运行统计信息（线程安全快照）。
 */
esp_err_t agent_loop_get_stats(agent_stats_t *out_stats);

/**
 * 记录出站发送永久失败（由 outbound_dispatch 调用）。
 */
void agent_loop_record_outbound_send_failure(void);
