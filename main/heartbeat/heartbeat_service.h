#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    uint32_t total_runs;
    uint32_t triggered_runs;
    uint32_t enqueue_success;
    uint32_t enqueue_failures;
    uint32_t skipped_no_file;
    uint32_t skipped_empty;
    uint32_t skipped_read_error;
    uint32_t last_run_unix;
} heartbeat_stats_t;

esp_err_t heartbeat_service_init(void);
esp_err_t heartbeat_service_start(void);
esp_err_t heartbeat_service_trigger_now(void);
esp_err_t heartbeat_service_get_stats(heartbeat_stats_t *out_stats);
