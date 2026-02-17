#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Voice channel states */
typedef enum {
    VOICE_STATE_IDLE = 0,
    VOICE_STATE_CONNECTING,
    VOICE_STATE_RECORDING,
    VOICE_STATE_PROCESSING,
    VOICE_STATE_PLAYING,
} voice_state_t;

/* Voice channel configuration */
typedef struct {
    char gateway_url[128];  /* e.g. "ws://192.168.1.100:8090" */
    int button_gpio;        /* Push-to-talk button GPIO (active LOW) */
    int max_record_sec;     /* Max recording duration in seconds */
} voice_channel_config_t;

/**
 * Initialize voice channel (configure GPIO, allocate buffers).
 * Must be called after audio_init() and message_bus_init().
 */
esp_err_t voice_channel_init(const voice_channel_config_t *config);

/**
 * Start voice channel task (button listener + voice pipeline).
 * Requires WiFi to be connected.
 */
esp_err_t voice_channel_start(void);

/**
 * Stop voice channel and free resources.
 */
void voice_channel_stop(void);

/**
 * Speak text via TTS gateway and play through speaker.
 * Called from outbound dispatch when channel == "voice".
 */
esp_err_t voice_channel_speak(const char *text);

/**
 * Get current voice channel state.
 */
voice_state_t voice_channel_get_state(void);

/**
 * Check if WebSocket is connected to gateway.
 */
bool voice_channel_is_connected(void);

/**
 * Update gateway URL at runtime (persisted to NVS).
 */
esp_err_t voice_channel_set_gateway(const char *url);

#ifdef __cplusplus
}
#endif
