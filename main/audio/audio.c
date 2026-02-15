#include "audio.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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

/* Task handles */
static TaskHandle_t s_listen_task = NULL;
static TaskHandle_t s_record_task = NULL;

/* Forward declarations */
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

    /* Enable I2S RX */
    esp_err_t ret = i2s_channel_enable(s_mic_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S mic: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create listening task */
    xTaskCreate(listen_task, "audio_listen", 4096, NULL, 5, &s_listen_task);

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

static void listen_task(void *arg)
{
    ESP_LOGI(TAG, "Listen task started");

    uint8_t *buffer = malloc(1024);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_mic_handle, buffer, 1024, &bytes_read, portMAX_DELAY);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* TODO: Implement wake word detection using ESP-SR */
        /* For now, just log that we're receiving audio */
        // ESP_LOGD(TAG, "Read %d bytes", bytes_read);

        vTaskDelay(pdMS_TO_TICKS(10));
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
