#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio configuration */
typedef struct {
    /* I2S Microphone (input) */
    int mic_i2s_port;
    int mic_ws_pin;      /* Word Select / LRCK */
    int mic_sck_pin;     /* Serial Clock / BCLK */
    int mic_sd_pin;      /* Serial Data / DIN */
    int mic_sample_rate;
    int mic_bits_per_sample;

    /* I2S Speaker (output) */
    int spk_i2s_port;
    int spk_ws_pin;
    int spk_sck_pin;
    int spk_sd_pin;
    int spk_sample_rate;
    int spk_bits_per_sample;

    /* Wake word detection */
    bool enable_wake_word;
    const char *wake_word;  /* e.g., "Hi Mimi" */
    float wake_word_threshold;

    /* Audio processing */
    int vad_threshold;      /* Voice Activity Detection threshold */
    int silence_timeout_ms; /* Silence duration to stop recording */
} audio_config_t;

/* Audio event types */
typedef enum {
    AUDIO_EVENT_WAKE_WORD_DETECTED,
    AUDIO_EVENT_SPEECH_START,
    AUDIO_EVENT_SPEECH_END,
    AUDIO_EVENT_PLAYBACK_START,
    AUDIO_EVENT_PLAYBACK_END,
} audio_event_type_t;

/* Audio event callback */
typedef void (*audio_event_cb_t)(audio_event_type_t event, void *user_data);

/**
 * Initialize audio system
 */
esp_err_t audio_init(const audio_config_t *config);

/**
 * Deinitialize audio system
 */
void audio_deinit(void);

/**
 * Start listening for wake word
 */
esp_err_t audio_start_listening(void);

/**
 * Stop listening
 */
void audio_stop_listening(void);

/**
 * Start recording audio (after wake word detected)
 * Returns audio data via callback
 */
esp_err_t audio_start_recording(void (*data_cb)(const uint8_t *data, size_t len));

/**
 * Stop recording
 */
void audio_stop_recording(void);

/**
 * Play audio data (TTS output)
 */
esp_err_t audio_play(const uint8_t *data, size_t len);

/**
 * Stop playback
 */
void audio_stop_playback(void);

/**
 * Set volume (0-100)
 */
void audio_set_volume(uint8_t volume);

/**
 * Get current volume
 */
uint8_t audio_get_volume(void);

/**
 * Mute/unmute
 */
void audio_set_mute(bool mute);

/**
 * Register event callback
 */
void audio_set_event_callback(audio_event_cb_t callback, void *user_data);

/**
 * Enable speaker I2S channel for streaming playback.
 * Call audio_spk_write() to feed PCM chunks, then audio_spk_disable() when done.
 */
esp_err_t audio_spk_enable(void);
void audio_spk_disable(void);

/**
 * Write PCM data to speaker (blocking, use between spk_enable/spk_disable).
 * @return bytes actually written
 */
esp_err_t audio_spk_write(const uint8_t *data, size_t len, size_t *bytes_written, uint32_t timeout_ms);

/**
 * Enable/disable mic I2S channel directly (no listen task).
 * Use these for manual recording via audio_mic_read().
 */
esp_err_t audio_mic_enable(void);
void audio_mic_disable(void);

/**
 * Read raw samples from microphone (blocking).
 * Caller must enable mic channel first or call audio_start_listening().
 *
 * @param buf       Output buffer for int16_t samples
 * @param buf_size  Buffer size in bytes
 * @param bytes_read  Actual bytes read
 * @param timeout_ms  Timeout in milliseconds
 * @return ESP_OK on success
 */
esp_err_t audio_mic_read(void *buf, size_t buf_size, size_t *bytes_read, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
