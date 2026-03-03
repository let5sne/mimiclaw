#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_WIFI_UNKNOWN = 0,
    STATUS_LED_WIFI_CONNECTING,
    STATUS_LED_WIFI_CONNECTED,
    STATUS_LED_WIFI_ERROR,
} status_led_wifi_state_t;

typedef enum {
    STATUS_LED_SERVICE_UNKNOWN = 0,
    STATUS_LED_SERVICE_OK,
    STATUS_LED_SERVICE_AUTH_ERROR,
    STATUS_LED_SERVICE_ERROR,
} status_led_service_state_t;

typedef enum {
    STATUS_LED_ACTIVITY_BOOT = 0,
    STATUS_LED_ACTIVITY_IDLE,
    STATUS_LED_ACTIVITY_THINKING,
    STATUS_LED_ACTIVITY_SPEAKING,
} status_led_activity_t;

esp_err_t status_led_init(void);
void status_led_set_wifi_state(status_led_wifi_state_t state);
void status_led_set_telegram_state(status_led_service_state_t state);
void status_led_set_llm_state(status_led_service_state_t state);
void status_led_set_activity(status_led_activity_t activity);

#ifdef __cplusplus
}
#endif
