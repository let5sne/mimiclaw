#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

esp_err_t cron_service_init(void);
esp_err_t cron_service_start(void);
esp_err_t cron_service_trigger_now(void);
esp_err_t cron_service_set_schedule(uint32_t interval_min, const char *task);
esp_err_t cron_service_clear_schedule(void);
esp_err_t cron_service_get_stats(cron_stats_t *out_stats);
esp_err_t cron_service_get_task(char *out_task, size_t out_size);
