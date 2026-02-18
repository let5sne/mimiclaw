#include "audio.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <ctype.h>
#include <string.h>

static const char *TAG = "audio";

/* Audio state */
static bool s_initialized = false;
static audio_config_t s_config = {0};
static i2s_chan_handle_t s_mic_handle = NULL;
static i2s_chan_handle_t s_spk_handle = NULL;
static audio_event_cb_t s_event_callback = NULL;
static void *s_event_user_data = NULL;
static uint8_t s_volume = 80;
static bool s_muted = false;

/* WakeNet state */
static srmodel_list_t *s_sr_models = NULL;
static const esp_wn_iface_t *s_wakenet = NULL;
static model_iface_data_t *s_wakenet_data = NULL;
static int s_wakenet_chunk_samples = 0;
static char s_wakenet_model_name[MODEL_NAME_MAX_LENGTH] = {0};
static i2s_std_slot_mask_t s_mic_slot_mask = I2S_STD_SLOT_LEFT;

/* Task handles */
static TaskHandle_t s_listen_task = NULL;
static TaskHandle_t s_record_task = NULL;

/* Forward declarations */
static const char *wake_word_keyword_from_config(const char *wake_word,
                                                 char *out, size_t out_size);
static esp_err_t audio_wakenet_init(void);
static void audio_wakenet_deinit(void);
static esp_err_t audio_set_mic_slot_mask(i2s_std_slot_mask_t slot_mask);
static void listen_task(void *arg);
static void record_task(void *arg) __attribute__((unused));

esp_err_t audio_init(const audio_config_t *config)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Audio already initialized");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(audio_config_t));

    /* Initialize I2S for microphone (RX) */
    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        (i2s_port_t)s_config.mic_i2s_port, I2S_ROLE_MASTER);

    esp_err_t ret = i2s_new_channel(&mic_chan_cfg, NULL, &s_mic_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S mic channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t mic_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_config.mic_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            (i2s_data_bit_width_t)s_config.mic_bits_per_sample, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)s_config.mic_sck_pin,
            .ws = (gpio_num_t)s_config.mic_ws_pin,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)s_config.mic_sd_pin,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    mic_std_cfg.slot_cfg.slot_mask = s_mic_slot_mask;

    ret = i2s_channel_init_std_mode(s_mic_handle, &mic_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S mic: %s", esp_err_to_name(ret));
        i2s_del_channel(s_mic_handle);
        return ret;
    }

    /* Initialize I2S for speaker (TX) */
    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        (i2s_port_t)s_config.spk_i2s_port, I2S_ROLE_MASTER);

    ret = i2s_new_channel(&spk_chan_cfg, &s_spk_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S speaker channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_mic_handle);
        return ret;
    }

    i2s_std_config_t spk_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_config.spk_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            (i2s_data_bit_width_t)s_config.spk_bits_per_sample, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)s_config.spk_sck_pin,
            .ws = (gpio_num_t)s_config.spk_ws_pin,
            .dout = (gpio_num_t)s_config.spk_sd_pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_spk_handle, &spk_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S speaker: %s", esp_err_to_name(ret));
        i2s_del_channel(s_mic_handle);
        i2s_del_channel(s_spk_handle);
        return ret;
    }

    /* WakeNet threshold is a probability in [0.4, 0.9999]. */
    if (s_config.enable_wake_word) {
        if (s_config.wake_word_threshold > 0.0f &&
            (s_config.wake_word_threshold < 0.4f || s_config.wake_word_threshold > 0.9999f)) {
            ESP_LOGW(TAG, "Wake threshold %.3f out of range, use model default",
                     s_config.wake_word_threshold);
        }
        ESP_LOGI(TAG, "Wake word configured: \"%s\"",
                 s_config.wake_word ? s_config.wake_word : "");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Audio initialized: mic=%dHz, spk=%dHz",
             s_config.mic_sample_rate, s_config.spk_sample_rate);

    return ESP_OK;
}

void audio_deinit(void)
{
    if (!s_initialized) return;

    audio_stop_listening();
    audio_stop_recording();
    audio_stop_playback();

    if (s_mic_handle) {
        i2s_del_channel(s_mic_handle);
        s_mic_handle = NULL;
    }

    if (s_spk_handle) {
        i2s_del_channel(s_spk_handle);
        s_spk_handle = NULL;
    }

    audio_wakenet_deinit();

    s_initialized = false;
    ESP_LOGI(TAG, "Audio deinitialized");
}

esp_err_t audio_start_listening(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Audio not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_listen_task) {
        ESP_LOGW(TAG, "Already listening");
        return ESP_OK;
    }

    if (!s_config.enable_wake_word) {
        ESP_LOGW(TAG, "Wake word is disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = audio_wakenet_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WakeNet init failed");
        return ret;
    }

    /* Enable I2S RX */
    ret = i2s_channel_enable(s_mic_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S mic: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create listening task */
    BaseType_t xret = xTaskCreate(listen_task, "audio_listen", 6144, NULL, 5, &s_listen_task);
    if (xret != pdPASS) {
        i2s_channel_disable(s_mic_handle);
        s_listen_task = NULL;
        ESP_LOGE(TAG, "Failed to create listen task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Started listening for wake word");
    return ESP_OK;
}

void audio_stop_listening(void)
{
    if (s_listen_task) {
        vTaskDelete(s_listen_task);
        s_listen_task = NULL;
    }

    if (s_mic_handle) {
        i2s_channel_disable(s_mic_handle);
    }

    ESP_LOGI(TAG, "Stopped listening");
}

esp_err_t audio_start_recording(void (*data_cb)(const uint8_t *data, size_t len))
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Audio not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* TODO: Implement recording with callback */
    ESP_LOGW(TAG, "Recording not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_stop_recording(void)
{
    if (s_record_task) {
        vTaskDelete(s_record_task);
        s_record_task = NULL;
    }
}

esp_err_t audio_play(const uint8_t *data, size_t len)
{
    if (!s_initialized || !s_spk_handle) {
        ESP_LOGE(TAG, "Audio not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_muted) {
        ESP_LOGD(TAG, "Audio muted, skipping playback");
        return ESP_OK;
    }

    /* Enable speaker */
    esp_err_t ret = i2s_channel_enable(s_spk_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable speaker: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Write audio data */
    size_t bytes_written = 0;
    ret = i2s_channel_write(s_spk_handle, data, len, &bytes_written, portMAX_DELAY);

    /* Disable speaker */
    i2s_channel_disable(s_spk_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write audio: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Played %d bytes", bytes_written);
    return ESP_OK;
}

void audio_stop_playback(void)
{
    if (s_spk_handle) {
        i2s_channel_disable(s_spk_handle);
    }
}

esp_err_t audio_spk_enable(void)
{
    if (!s_initialized || !s_spk_handle) return ESP_ERR_INVALID_STATE;
    return i2s_channel_enable(s_spk_handle);
}

void audio_spk_disable(void)
{
    if (s_spk_handle) {
        i2s_channel_disable(s_spk_handle);
    }
}

esp_err_t audio_spk_write(const uint8_t *data, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (!s_initialized || !s_spk_handle) return ESP_ERR_INVALID_STATE;
    if (s_muted) { *bytes_written = len; return ESP_OK; }
    return i2s_channel_write(s_spk_handle, data, len, bytes_written, pdMS_TO_TICKS(timeout_ms));
}

void audio_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_volume = volume;
    ESP_LOGI(TAG, "Volume set to %d%%", volume);
}

uint8_t audio_get_volume(void)
{
    return s_volume;
}

void audio_set_mute(bool mute)
{
    s_muted = mute;
    ESP_LOGI(TAG, "Audio %s", mute ? "muted" : "unmuted");
}

void audio_set_event_callback(audio_event_cb_t callback, void *user_data)
{
    s_event_callback = callback;
    s_event_user_data = user_data;
}

bool audio_is_listening(void)
{
    return s_listen_task != NULL;
}

bool audio_is_wake_word_enabled(void)
{
    return s_config.enable_wake_word;
}

int audio_get_vad_threshold(void)
{
    return s_config.vad_threshold;
}

int audio_get_silence_timeout_ms(void)
{
    return s_config.silence_timeout_ms;
}

esp_err_t audio_mic_enable(void)
{
    if (!s_initialized || !s_mic_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_enable(s_mic_handle);
}

void audio_mic_disable(void)
{
    if (s_mic_handle) {
        i2s_channel_disable(s_mic_handle);
    }
}

esp_err_t audio_mic_read(void *buf, size_t buf_size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!s_initialized || !s_mic_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_read(s_mic_handle, buf, buf_size, bytes_read, pdMS_TO_TICKS(timeout_ms));
}

/* Private functions */

static const char *wake_word_keyword_from_config(const char *wake_word,
                                                 char *out, size_t out_size)
{
    if (!wake_word || !out || out_size < 8) {
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; wake_word[i] != '\0' && j + 1 < out_size; ++i) {
        unsigned char c = (unsigned char)wake_word[i];
        if (isalnum(c)) {
            out[j++] = (char)tolower(c);
        }
    }
    out[j] = '\0';
    if (j == 0) {
        return NULL;
    }

    if (strstr(out, "hiesp") || strstr(out, "esp")) {
        return "hiesp";
    }
    if (strstr(out, "hilexin") || strstr(out, "lexin")) {
        return "hilexin";
    }
    if (strstr(out, "alexa")) {
        return "alexa";
    }
    if (strstr(out, "xiaozhi")) {
        return "nihaoxiaozhi";
    }
    return out;
}

static esp_err_t audio_wakenet_init(void)
{
    if (s_wakenet_data) {
        return ESP_OK;
    }

    s_sr_models = esp_srmodel_init("model");
    if (!s_sr_models || s_sr_models->num <= 0) {
        ESP_LOGE(TAG, "No WakeNet model found in \"model\" partition");
        audio_wakenet_deinit();
        return ESP_FAIL;
    }

    char keyword_buf[64] = {0};
    const char *keyword = wake_word_keyword_from_config(
        s_config.wake_word, keyword_buf, sizeof(keyword_buf));
    char *model_name = NULL;
    if (keyword) {
        model_name = esp_srmodel_filter(s_sr_models, ESP_WN_PREFIX, keyword);
    }
    if (!model_name) {
        model_name = esp_srmodel_filter(s_sr_models, ESP_WN_PREFIX, NULL);
    }
    if (!model_name) {
        ESP_LOGE(TAG, "WakeNet model filter failed");
        audio_wakenet_deinit();
        return ESP_FAIL;
    }

    s_wakenet = esp_wn_handle_from_name(model_name);
    if (!s_wakenet) {
        ESP_LOGE(TAG, "WakeNet handle not found for model: %s", model_name);
        audio_wakenet_deinit();
        return ESP_FAIL;
    }

    s_wakenet_data = s_wakenet->create(model_name, DET_MODE_95);
    if (!s_wakenet_data) {
        ESP_LOGE(TAG, "WakeNet create failed for model: %s", model_name);
        audio_wakenet_deinit();
        return ESP_FAIL;
    }

    s_wakenet_chunk_samples = s_wakenet->get_samp_chunksize(s_wakenet_data);
    if (s_wakenet_chunk_samples <= 0) {
        ESP_LOGE(TAG, "Invalid WakeNet chunk size");
        audio_wakenet_deinit();
        return ESP_FAIL;
    }

    strncpy(s_wakenet_model_name, model_name, sizeof(s_wakenet_model_name) - 1);
    s_wakenet_model_name[sizeof(s_wakenet_model_name) - 1] = '\0';

    if (s_config.wake_word_threshold >= 0.4f && s_config.wake_word_threshold <= 0.9999f) {
        int set_ok = s_wakenet->set_det_threshold(s_wakenet_data, s_config.wake_word_threshold, 1);
        if (set_ok != 1) {
            ESP_LOGW(TAG, "Set WakeNet threshold failed, use default");
        }
    }

    char *wake_words = esp_srmodel_get_wake_words(s_sr_models, s_wakenet_model_name);
    ESP_LOGI(TAG, "WakeNet ready: model=%s wake_words=%s chunk=%d",
             s_wakenet_model_name, wake_words ? wake_words : "unknown", s_wakenet_chunk_samples);
    free(wake_words);
    return ESP_OK;
}

static void audio_wakenet_deinit(void)
{
    if (s_wakenet && s_wakenet_data) {
        s_wakenet->destroy(s_wakenet_data);
    }
    s_wakenet_data = NULL;
    s_wakenet = NULL;
    s_wakenet_chunk_samples = 0;
    s_wakenet_model_name[0] = '\0';

    if (s_sr_models) {
        esp_srmodel_deinit(s_sr_models);
        s_sr_models = NULL;
    }
}

static esp_err_t audio_set_mic_slot_mask(i2s_std_slot_mask_t slot_mask)
{
    if (!s_mic_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mic_slot_mask == slot_mask) {
        return ESP_OK;
    }

    esp_err_t disable_ret = i2s_channel_disable(s_mic_handle);
    bool should_reenable = (disable_ret == ESP_OK);
    if (disable_ret != ESP_OK && disable_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Disable mic before slot switch failed: %s", esp_err_to_name(disable_ret));
    }

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        (i2s_data_bit_width_t)s_config.mic_bits_per_sample, I2S_SLOT_MODE_MONO);
    slot_cfg.slot_mask = slot_mask;

    esp_err_t ret = i2s_channel_reconfig_std_slot(s_mic_handle, &slot_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reconfig mic slot failed: %s", esp_err_to_name(ret));
        if (should_reenable) {
            i2s_channel_enable(s_mic_handle);
        }
        return ret;
    }

    s_mic_slot_mask = slot_mask;
    if (should_reenable) {
        ret = i2s_channel_enable(s_mic_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Re-enable mic after slot switch failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "Mic slot switched to %s", slot_mask == I2S_STD_SLOT_RIGHT ? "RIGHT" : "LEFT");
    return ESP_OK;
}

static void listen_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Listen task started");

    size_t buffer_bytes = (size_t)s_wakenet_chunk_samples * sizeof(int16_t);
    int16_t *buffer = (int16_t *)malloc(buffer_bytes);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        vTaskDelete(NULL);
        return;
    }

    int low_signal_frames = 0;
    uint32_t debug_frames = 0;

    while (1) {
        if (!s_wakenet || !s_wakenet_data) {
            ESP_LOGE(TAG, "WakeNet runtime not ready");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_mic_handle, buffer, buffer_bytes, &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK || bytes_read != buffer_bytes) {
            ESP_LOGW(TAG, "I2S read failed ret=%s bytes=%d/%d",
                     esp_err_to_name(ret), (int)bytes_read, (int)buffer_bytes);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int16_t peak = 0;
        for (int i = 0; i < s_wakenet_chunk_samples; ++i) {
            int16_t v = buffer[i];
            int16_t abs_v = (v < 0) ? (int16_t)(-v) : v;
            if (abs_v > peak) {
                peak = abs_v;
            }
        }

        /* 低频率打印输入峰值，方便定位“能采到音但不触发”与“根本没采到音”。 */
        debug_frames++;
        if ((debug_frames % 64U) == 0U) {
            ESP_LOGI(TAG, "Wake input: slot=%s peak=%d",
                     s_mic_slot_mask == I2S_STD_SLOT_RIGHT ? "RIGHT" : "LEFT",
                     (int)peak);
        }

        /* INMP441 常见左右声道接线差异，持续近静音时自动在左右声道间切换探测。 */
        if (peak < 64) {
            low_signal_frames++;
        } else {
            low_signal_frames = 0;
        }
        if (low_signal_frames >= 160) {
            i2s_std_slot_mask_t next_slot = (s_mic_slot_mask == I2S_STD_SLOT_LEFT)
                                                ? I2S_STD_SLOT_RIGHT
                                                : I2S_STD_SLOT_LEFT;
            (void)audio_set_mic_slot_mask(next_slot);
            low_signal_frames = 0;
            continue;
        }

        /* 对低音量输入做轻量增益，提升唤醒触发率。 */
        if (peak > 0 && peak < 10000) {
            int gain = 10000 / peak;
            if (gain > 32) {
                gain = 32;
            }
            if (gain > 1) {
                for (int i = 0; i < s_wakenet_chunk_samples; ++i) {
                    int32_t scaled = (int32_t)buffer[i] * gain;
                    if (scaled > 32767) scaled = 32767;
                    if (scaled < -32768) scaled = -32768;
                    buffer[i] = (int16_t)scaled;
                }
            }
        }

        int word_index = (int)s_wakenet->detect(s_wakenet_data, buffer);
        if (word_index > 0) {
            const char *word = s_wakenet->get_word_name(s_wakenet_data, word_index);
            ESP_LOGI(TAG, "Wake word detected: idx=%d word=%s model=%s",
                     word_index, word ? word : "unknown", s_wakenet_model_name);
            if (s_event_callback) {
                s_event_callback(AUDIO_EVENT_WAKE_WORD_DETECTED, s_event_user_data);
            }
            /* 防止连续触发导致重复启动录音 */
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    free(buffer);
    vTaskDelete(NULL);
}

static void record_task(void *arg)
{
    /* TODO: Implement recording task with VAD */
    (void)arg;
    vTaskDelete(NULL);
}
