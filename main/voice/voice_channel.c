#include "voice_channel.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "audio/audio.h"
#include "display/display.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "voice";

/* ---------- State ---------- */
static voice_state_t s_state = VOICE_STATE_IDLE;
static voice_channel_config_t s_config;
static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_btn_sem = NULL;   /* posted by GPIO ISR */
static volatile bool s_btn_pressed = false;

/* PCM record buffer in PSRAM */
static uint8_t *s_rec_buf = NULL;
static size_t   s_rec_len = 0;  /* bytes recorded so far */

/* HTTP response accumulator */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} http_buf_t;

/* ---------- Helpers ---------- */

static void set_state(voice_state_t st)
{
    s_state = st;
    switch (st) {
    case VOICE_STATE_IDLE:
        display_set_status("MimiClaw Ready");
        display_set_display_status(DISPLAY_STATUS_IDLE);
        break;
    case VOICE_STATE_RECORDING:
        display_set_status("Recording...");
        display_set_display_status(DISPLAY_STATUS_CONNECTED);
        break;
    case VOICE_STATE_PROCESSING:
        display_set_status("Thinking...");
        display_set_display_status(DISPLAY_STATUS_THINKING);
        break;
    case VOICE_STATE_PLAYING:
        display_set_status("Speaking...");
        display_set_display_status(DISPLAY_STATUS_SPEAKING);
        break;
    }
}

/* Load gateway URL from NVS, fall back to config default */
static void load_gateway_url(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_VOICE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_config.gateway_url);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_VOICE_GW, s_config.gateway_url, &len) == ESP_OK
            && s_config.gateway_url[0]) {
            ESP_LOGI(TAG, "Gateway URL from NVS: %s", s_config.gateway_url);
        }
        nvs_close(nvs);
    }
}

/* ---------- HTTP helpers ---------- */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *acc = (http_buf_t *)evt->user_data;
    if (!acc) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (acc->len + evt->data_len > acc->cap) {
            /* Grow buffer (PSRAM) */
            size_t new_cap = acc->cap ? acc->cap * 2 : 32768;
            while (new_cap < acc->len + evt->data_len) new_cap *= 2;
            uint8_t *tmp = heap_caps_realloc(acc->buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!tmp) {
                ESP_LOGE(TAG, "HTTP buf realloc failed (%d)", (int)new_cap);
                return ESP_FAIL;
            }
            acc->buf = tmp;
            acc->cap = new_cap;
        }
        memcpy(acc->buf + acc->len, evt->data, evt->data_len);
        acc->len += evt->data_len;
    }
    return ESP_OK;
}

/**
 * POST raw PCM to /stt, parse JSON response, return heap-allocated text.
 * Caller must free() the returned string.
 */
static char *do_stt(const uint8_t *pcm, size_t pcm_len)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/stt", s_config.gateway_url);

    http_buf_t acc = {0};

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_event_handler,
        .user_data = &acc,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;

    esp_http_client_set_header(client, "Content-Type", "audio/pcm");
    esp_http_client_set_post_field(client, (const char *)pcm, (int)pcm_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "STT request failed: err=%s status=%d", esp_err_to_name(err), status);
        free(acc.buf);
        return NULL;
    }

    /* Log raw response */
    if (acc.buf && acc.len > 0) {
        acc.buf[acc.len < 511 ? acc.len : 511] = '\0';
        ESP_LOGI(TAG, "STT response: %s", (char *)acc.buf);
    }

    /* Parse JSON {"text": "..."} */
    char *text = NULL;
    if (acc.buf && acc.len > 0) {
        /* Null-terminate */
        uint8_t *tmp = realloc(acc.buf, acc.len + 1);
        if (tmp) acc.buf = tmp;
        acc.buf[acc.len] = '\0';

        cJSON *root = cJSON_Parse((char *)acc.buf);
        if (root) {
            cJSON *t = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(t) && t->valuestring[0]) {
                text = strdup(t->valuestring);
            }
            cJSON_Delete(root);
        }
    }
    free(acc.buf);
    return text;
}

/**
 * POST text to /tts, receive raw PCM.
 * Returns PSRAM-allocated buffer, caller must free().
 */
static uint8_t *do_tts(const char *text, size_t *out_len)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/tts", s_config.gateway_url);

    /* Build JSON body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "text", text);
    cJSON_AddStringToObject(body, "voice", "zh-CN-XiaoxiaoNeural");
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json_str) return NULL;

    http_buf_t acc = {0};

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_event_handler,
        .user_data = &acc,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(json_str); return NULL; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, (int)strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(json_str);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "TTS request failed: err=%s status=%d", esp_err_to_name(err), status);
        free(acc.buf);
        return NULL;
    }

    *out_len = acc.len;
    return acc.buf;
}

/* ---------- GPIO ISR ---------- */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    int level = gpio_get_level(s_config.button_gpio);
    s_btn_pressed = (level == 0);  /* active LOW */

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(s_btn_sem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

/* ---------- Voice task ---------- */

static void voice_task(void *arg)
{
    ESP_LOGI(TAG, "Voice task started (button GPIO %d)", s_config.button_gpio);

    while (1) {
        /* Wait for button press */
        xSemaphoreTake(s_btn_sem, portMAX_DELAY);

        /* Debounce */
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(s_config.button_gpio) != 0) continue;  /* spurious */

        if (s_state != VOICE_STATE_IDLE) continue;

        /* === RECORDING === */
        set_state(VOICE_STATE_RECORDING);
        s_rec_len = 0;

        esp_err_t ret = audio_mic_enable();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable mic: %s", esp_err_to_name(ret));
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        const size_t chunk_bytes = 1024;
        const size_t max_bytes = (size_t)s_config.max_record_sec
                                 * MIMI_AUDIO_MIC_SAMPLE_RATE * 2;  /* 16-bit = 2 bytes/sample */

        ESP_LOGI(TAG, "Recording... (max %d s)", s_config.max_record_sec);

        int err_count = 0;
        while (s_rec_len < max_bytes) {
            /* Check if button released */
            if (gpio_get_level(s_config.button_gpio) != 0) {
                /* Debounce release */
                vTaskDelay(pdMS_TO_TICKS(50));
                if (gpio_get_level(s_config.button_gpio) != 0) break;
            }

            size_t bytes_read = 0;
            ret = audio_mic_read(s_rec_buf + s_rec_len, chunk_bytes, &bytes_read, 500);
            if (ret == ESP_OK && bytes_read > 0) {
                s_rec_len += bytes_read;
                err_count = 0;
            } else if (ret != ESP_OK) {
                err_count++;
                if (err_count <= 3) {
                    ESP_LOGW(TAG, "mic_read error: %s", esp_err_to_name(ret));
                }
            }
        }

        audio_mic_disable();
        ESP_LOGI(TAG, "Recorded %d bytes (%.1f s)", (int)s_rec_len, s_rec_len / 32000.0f);

        /* Log RMS level to verify mic is capturing real audio */
        if (s_rec_len >= 64) {
            int16_t *samples = (int16_t *)s_rec_buf;
            int num_samples = s_rec_len / 2;
            int64_t sum_sq = 0;
            int16_t peak = 0;
            for (int i = 0; i < num_samples; i++) {
                sum_sq += (int64_t)samples[i] * samples[i];
                int16_t abs_val = samples[i] < 0 ? -samples[i] : samples[i];
                if (abs_val > peak) peak = abs_val;
            }
            int rms = (int)__builtin_sqrtf((float)sum_sq / num_samples);
            ESP_LOGI(TAG, "Audio RMS: %d, Peak: %d (samples: %d)", rms, peak, num_samples);

            /* Software gain: amplify to use more of the 16-bit range.
             * Target peak ~20000 (60% of int16 max). */
            if (peak > 0 && peak < 20000) {
                int gain = 20000 / peak;
                if (gain > 32) gain = 32;  /* cap at 32x */
                if (gain > 1) {
                    ESP_LOGI(TAG, "Applying %dx software gain", gain);
                    for (int i = 0; i < num_samples; i++) {
                        int32_t amplified = (int32_t)samples[i] * gain;
                        if (amplified > 32767) amplified = 32767;
                        if (amplified < -32768) amplified = -32768;
                        samples[i] = (int16_t)amplified;
                    }
                }
            }
        }

        if (s_rec_len < 3200) {  /* < 0.1s, too short */
            ESP_LOGW(TAG, "Recording too short, discarding");
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        /* === PROCESSING (STT → agent → TTS) === */
        set_state(VOICE_STATE_PROCESSING);

        char *text = do_stt(s_rec_buf, s_rec_len);
        if (!text || text[0] == '\0') {
            ESP_LOGW(TAG, "STT returned empty text");
            free(text);
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        ESP_LOGI(TAG, "STT result: \"%s\"", text);
        display_show_message("user", text);

        /* Push to message bus → agent loop will process and push outbound on "voice" channel */
        ESP_LOGI(TAG, "Pushing to message bus: channel=%s chat_id=voice content=\"%.*s\"",
                 MIMI_CHAN_VOICE, 200, text);
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_VOICE, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice", sizeof(msg.chat_id) - 1);
        msg.content = text;  /* bus takes ownership */

        if (message_bus_push_inbound(&msg) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to push to message bus");
            free(text);
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        /* State stays PROCESSING until outbound dispatch calls voice_channel_speak() */
        /* which will transition to PLAYING → IDLE */

        /* Timeout: if no response in 30s, go back to idle */
        int wait_count = 0;
        while (s_state == VOICE_STATE_PROCESSING && wait_count < 300) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count++;
        }
        if (s_state == VOICE_STATE_PROCESSING) {
            ESP_LOGW(TAG, "Voice response timeout");
            set_state(VOICE_STATE_IDLE);
        }
    }
}

/* ---------- Public API ---------- */

esp_err_t voice_channel_init(const voice_channel_config_t *config)
{
    memcpy(&s_config, config, sizeof(s_config));
    load_gateway_url();

    if (s_config.gateway_url[0] == '\0') {
        ESP_LOGW(TAG, "No voice gateway URL configured");
    }

    /* Allocate record buffer in PSRAM */
    size_t buf_size = (size_t)s_config.max_record_sec
                      * MIMI_AUDIO_MIC_SAMPLE_RATE * 2;
    s_rec_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!s_rec_buf) {
        ESP_LOGE(TAG, "Failed to allocate record buffer (%d bytes)", (int)buf_size);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Record buffer: %d bytes in PSRAM", (int)buf_size);

    /* Create button semaphore */
    s_btn_sem = xSemaphoreCreateBinary();
    if (!s_btn_sem) {
        heap_caps_free(s_rec_buf);
        s_rec_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Configure GPIO with internal pull-up, interrupt on any edge */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_config.button_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(s_config.button_gpio, gpio_isr_handler, NULL);

    ESP_LOGI(TAG, "Voice channel initialized (GPIO %d, gateway %s)",
             s_config.button_gpio, s_config.gateway_url);
    return ESP_OK;
}

esp_err_t voice_channel_start(void)
{
    if (s_task_handle) return ESP_ERR_INVALID_STATE;

    BaseType_t ret = xTaskCreatePinnedToCore(
        voice_task, "voice",
        MIMI_VOICE_TASK_STACK, NULL,
        MIMI_VOICE_TASK_PRIO, &s_task_handle,
        MIMI_VOICE_TASK_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void voice_channel_stop(void)
{
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
    gpio_isr_handler_remove(s_config.button_gpio);
    if (s_rec_buf) {
        heap_caps_free(s_rec_buf);
        s_rec_buf = NULL;
    }
    if (s_btn_sem) {
        vSemaphoreDelete(s_btn_sem);
        s_btn_sem = NULL;
    }
    s_state = VOICE_STATE_IDLE;
}

esp_err_t voice_channel_speak(const char *text)
{
    if (!text || text[0] == '\0') {
        set_state(VOICE_STATE_IDLE);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_config.gateway_url[0] == '\0') {
        ESP_LOGE(TAG, "No gateway URL, cannot TTS");
        set_state(VOICE_STATE_IDLE);
        return ESP_ERR_INVALID_STATE;
    }

    set_state(VOICE_STATE_PLAYING);
    ESP_LOGI(TAG, "TTS speak: \"%.*s\"", 200, text);
    display_show_message("assistant", text);

    size_t pcm_len = 0;
    uint8_t *pcm = do_tts(text, &pcm_len);
    if (!pcm || pcm_len == 0) {
        ESP_LOGE(TAG, "TTS failed");
        free(pcm);
        set_state(VOICE_STATE_IDLE);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Playing TTS audio: %d bytes (%.1f s)", (int)pcm_len, pcm_len / 32000.0f);
    esp_err_t ret = audio_play(pcm, pcm_len);
    free(pcm);

    set_state(VOICE_STATE_IDLE);
    return ret;
}

voice_state_t voice_channel_get_state(void)
{
    return s_state;
}

esp_err_t voice_channel_set_gateway(const char *url)
{
    if (!url || strlen(url) >= sizeof(s_config.gateway_url)) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_config.gateway_url, url, sizeof(s_config.gateway_url) - 1);
    s_config.gateway_url[sizeof(s_config.gateway_url) - 1] = '\0';

    /* Persist to NVS */
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MIMI_NVS_VOICE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        nvs_set_str(nvs, MIMI_NVS_KEY_VOICE_GW, url);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Gateway URL set: %s", url);
    return ESP_OK;
}
