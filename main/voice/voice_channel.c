#include "voice_channel.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "audio/audio.h"
#include "display/display.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_timer.h"

static const char *TAG = "voice";

/* Event group bits */
#define EVT_WS_CONNECTED   BIT0
#define EVT_STT_DONE       BIT1
#define EVT_TTS_DONE       BIT2
#define EVT_WAKE_WORD      BIT3
#define EVT_BUTTON_PRESS   BIT4

/* ---------- State ---------- */
static voice_state_t s_state = VOICE_STATE_IDLE;
static voice_channel_config_t s_config;
static TaskHandle_t s_task_handle = NULL;
static EventGroupHandle_t s_events = NULL;
static volatile bool s_btn_pressed = false;

/* WebSocket client */
static esp_websocket_client_handle_t s_ws_client = NULL;

/* STT result (set by WS handler, read by voice task) */
static char *s_stt_text = NULL;

/* JSON receive buffer for fragmented text frames */
static char *s_json_buf = NULL;
static size_t s_json_len = 0;
static size_t s_json_cap = 0;
static bool s_ws_recv_binary_frag = false;
static bool s_ws_recv_text_frag = false;
static volatile int64_t s_followup_deadline_ms = 0;
static volatile int64_t s_playback_started_ms = 0;

static esp_err_t normalize_gateway_url(const char *input, char *output, size_t output_size)
{
    if (!input || !output || output_size < 8) {
        return ESP_ERR_INVALID_ARG;
    }

    while (*input && isspace((unsigned char)*input)) {
        input++;
    }
    if (*input == '\0') {
        output[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }

    size_t in_len = strlen(input);
    while (in_len > 0 && isspace((unsigned char)input[in_len - 1])) {
        in_len--;
    }
    if (in_len == 0) {
        output[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }

    char tmp[160] = {0};
    if (in_len >= sizeof(tmp)) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(tmp, input, in_len);
    tmp[in_len] = '\0';

    if (strncmp(tmp, "ws://", 5) != 0 && strncmp(tmp, "wss://", 6) != 0) {
        char with_scheme[160] = {0};
        int n = snprintf(with_scheme, sizeof(with_scheme), "ws://%s", tmp);
        if (n <= 0 || (size_t)n >= sizeof(with_scheme)) {
            output[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        strncpy(tmp, with_scheme, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    }

    const char *scheme_end = strstr(tmp, "://");
    if (!scheme_end) {
        output[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }
    const char *host_begin = scheme_end + 3;
    const char *sep = strpbrk(host_begin, "/?#");

    if (!sep) {
        int n = snprintf(output, output_size, "%s/", tmp);
        if (n <= 0 || (size_t)n >= output_size) {
            output[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    if (*sep == '/') {
        if (strlen(tmp) + 1 > output_size) {
            output[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        strncpy(output, tmp, output_size - 1);
        output[output_size - 1] = '\0';
        return ESP_OK;
    }

    size_t prefix_len = (size_t)(sep - tmp);
    if (prefix_len + 1 + strlen(sep) + 1 > output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(output, tmp, prefix_len);
    output[prefix_len] = '/';
    strcpy(output + prefix_len + 1, sep);
    return ESP_OK;
}

/* ---------- Audio event callback ---------- */
static void audio_event_handler(audio_event_type_t event, void *user_data)
{
    switch (event) {
    case AUDIO_EVENT_WAKE_WORD_DETECTED:
        ESP_LOGI(TAG, "Wake word detected event received");
        if (s_events) {
            xEventGroupSetBits(s_events, EVT_WAKE_WORD);
        }
        break;
    case AUDIO_EVENT_SPEECH_START:
        if (s_events) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool barge_in = (s_state == VOICE_STATE_PLAYING)
                            && (now_ms - s_playback_started_ms > 800);
            bool in_followup_window = (s_followup_deadline_ms > now_ms);
            if (barge_in || in_followup_window) {
                ESP_LOGI(TAG, "Speech start -> trigger capture (barge_in=%d followup=%d)",
                         barge_in ? 1 : 0, in_followup_window ? 1 : 0);
                xEventGroupSetBits(s_events, EVT_WAKE_WORD);
            }
        }
        break;
    case AUDIO_EVENT_SPEECH_END:
        ESP_LOGI(TAG, "Speech end event received");
        break;
    default:
        break;
    }
}

/* ---------- Helpers ---------- */

static void set_state(voice_state_t st)
{
    s_state = st;
    switch (st) {
    case VOICE_STATE_IDLE:
        display_set_status("MimiClaw Ready");
        display_set_display_status(DISPLAY_STATUS_IDLE);
        break;
    case VOICE_STATE_CONNECTING:
        display_set_status("Connecting...");
        display_set_display_status(DISPLAY_STATUS_CONNECTING);
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

static void load_gateway_url(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_VOICE, NVS_READONLY, &nvs) == ESP_OK) {
        char raw[sizeof(s_config.gateway_url)] = {0};
        size_t len = sizeof(raw);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_VOICE_GW, raw, &len) == ESP_OK && raw[0]) {
            char normalized[sizeof(s_config.gateway_url)] = {0};
            if (normalize_gateway_url(raw, normalized, sizeof(normalized)) == ESP_OK) {
                strncpy(s_config.gateway_url, normalized, sizeof(s_config.gateway_url) - 1);
                s_config.gateway_url[sizeof(s_config.gateway_url) - 1] = '\0';
                ESP_LOGI(TAG, "Gateway URL from NVS: %s", s_config.gateway_url);
            } else {
                ESP_LOGW(TAG, "Invalid gateway URL in NVS, keep current config: %s", raw);
            }
        }
        nvs_close(nvs);
    }
}

/* Send a JSON control message over WebSocket */
static esp_err_t ws_send_json(const char *type, const cJSON *extra)
{
    if (!s_ws_client || !esp_websocket_client_is_connected(s_ws_client)) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", type);

    /* Merge extra fields if provided */
    if (extra) {
        cJSON *item = extra->child;
        while (item) {
            cJSON_AddItemToObject(msg, item->string, cJSON_Duplicate(item, 1));
            item = item->next;
        }
    }

    char *json_str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json_str) return ESP_ERR_NO_MEM;

    int ret = esp_websocket_client_send_text(s_ws_client, json_str, strlen(json_str), pdMS_TO_TICKS(5000));
    free(json_str);
    return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

/* Send raw binary PCM data over WebSocket */
static esp_err_t ws_send_binary(const uint8_t *data, size_t len)
{
    if (!s_ws_client || !esp_websocket_client_is_connected(s_ws_client)) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = esp_websocket_client_send_bin(s_ws_client, (const char *)data, len, pdMS_TO_TICKS(5000));
    return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

static void audio_spk_write_all(const uint8_t *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        size_t written = 0;
        esp_err_t ret = audio_spk_write(data + offset, len - offset, &written, 1000);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Speaker write failed: %s (written=%d/%d)",
                     esp_err_to_name(ret), (int)offset, (int)len);
            break;
        }
        if (written == 0) {
            ESP_LOGW(TAG, "Speaker write stalled (written=0)");
            break;
        }
        offset += written;
    }
}

/* ---------- JSON message handler ---------- */

static void handle_json_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "WS: invalid JSON");
        return;
    }

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "stt_result") == 0) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        ESP_LOGI(TAG, "STT result: \"%s\"", text ? text : "");

        /* Store result for voice task */
        free(s_stt_text);
        s_stt_text = (text && text[0]) ? strdup(text) : NULL;
        xEventGroupSetBits(s_events, EVT_STT_DONE);

    } else if (strcmp(type, "tts_start") == 0) {
        ESP_LOGI(TAG, "TTS stream starting");
        if (s_state == VOICE_STATE_PLAYING) {
            audio_spk_enable();
        }

    } else if (strcmp(type, "tts_end") == 0) {
        ESP_LOGI(TAG, "TTS stream ended");
        if (s_state == VOICE_STATE_PLAYING) {
            audio_spk_disable();
        }
        xEventGroupSetBits(s_events, EVT_TTS_DONE);

    } else if (strcmp(type, "error") == 0) {
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
        ESP_LOGE(TAG, "Gateway error: %s", msg ? msg : "unknown");
        /* Unblock any waiters */
        xEventGroupSetBits(s_events, EVT_STT_DONE | EVT_TTS_DONE);
    }

    cJSON_Delete(root);
}

/* ---------- WebSocket event handler ---------- */

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Voice WS connected");
        xEventGroupSetBits(s_events, EVT_WS_CONNECTED);
        set_state(VOICE_STATE_IDLE);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Voice WS disconnected");
        xEventGroupClearBits(s_events, EVT_WS_CONNECTED);
        s_followup_deadline_ms = 0;
        s_ws_recv_binary_frag = false;
        s_ws_recv_text_frag = false;
        s_json_len = 0;
        s_playback_started_ms = 0;
        if (s_state == VOICE_STATE_PLAYING) {
            audio_spk_disable();
        }
        set_state(VOICE_STATE_CONNECTING);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (evt->op_code == 0x02) {
            s_ws_recv_binary_frag = true;
            s_ws_recv_text_frag = false;
        } else if (evt->op_code == 0x01) {
            s_ws_recv_text_frag = true;
            s_ws_recv_binary_frag = false;
        }

        bool is_binary = (evt->op_code == 0x02) || (evt->op_code == 0x00 && s_ws_recv_binary_frag);
        bool is_text = (evt->op_code == 0x01) || (evt->op_code == 0x00 && s_ws_recv_text_frag);

        if (is_binary) {
            /* Binary frame: PCM audio chunk during TTS playback, including continuation frames */
            if (s_state == VOICE_STATE_PLAYING && evt->data_len > 0) {
                audio_spk_write_all((const uint8_t *)evt->data_ptr, evt->data_len);
            }
        } else if (is_text) {
            /* Text frame (possibly fragmented) */
            size_t needed = s_json_len + evt->data_len;
            if (needed > s_json_cap) {
                size_t new_cap = needed + 256;
                char *tmp = realloc(s_json_buf, new_cap);
                if (!tmp) {
                    ESP_LOGE(TAG, "JSON buf alloc failed");
                    s_json_len = 0;
                    break;
                }
                s_json_buf = tmp;
                s_json_cap = new_cap;
            }
            memcpy(s_json_buf + s_json_len, evt->data_ptr, evt->data_len);
            s_json_len += evt->data_len;

            if (evt->payload_offset + evt->data_len >= evt->payload_len) {
                handle_json_message(s_json_buf, s_json_len);
                s_json_len = 0;
            }
        }

        if (evt->payload_offset + evt->data_len >= evt->payload_len) {
            s_ws_recv_binary_frag = false;
            s_ws_recv_text_frag = false;
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Voice WS error");
        break;

    default:
        break;
    }
}

/* ---------- GPIO ISR ---------- */

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    int level = gpio_get_level(s_config.button_gpio);
    s_btn_pressed = (level == 0);  /* active LOW */

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_btn_pressed && s_events) {
        xEventGroupSetBitsFromISR(s_events, EVT_BUTTON_PRESS, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

/* ---------- Voice task ---------- */

static void voice_task(void *arg)
{
    ESP_LOGI(TAG, "Voice task started (button GPIO %d)", s_config.button_gpio);

    /* Wait for WebSocket connection */
    xEventGroupWaitBits(s_events, EVT_WS_CONNECTED, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Voice WS ready, listening for button or wake word");

    while (1) {
        /* Wait for button event or wake word event */
        EventBits_t bits = xEventGroupWaitBits(
            s_events, EVT_WAKE_WORD | (s_config.button_gpio >= 0 ? EVT_BUTTON_PRESS : 0),
            pdTRUE, pdFALSE, portMAX_DELAY);

        bool wake_word_detected = (bits & EVT_WAKE_WORD) != 0;
        bool button_pressed = (bits & EVT_BUTTON_PRESS) != 0;
        if (wake_word_detected || button_pressed) {
            s_followup_deadline_ms = 0;
        }

        /* If button pressed, debounce */
        if (button_pressed && s_config.button_gpio >= 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(s_config.button_gpio) != 0) continue;
        }

        /* Check if we're connected */
        if (!(xEventGroupGetBits(s_events) & EVT_WS_CONNECTED)) {
            ESP_LOGW(TAG, "WS not connected, ignoring event");
            continue;
        }

        /* === INTERRUPT: event during playback === */
        if (s_state == VOICE_STATE_PLAYING) {
            ESP_LOGI(TAG, "Interrupt: stopping playback");
            ws_send_json("interrupt", NULL);
            audio_spk_disable();
            /* Fall through to start recording */
        } else if (s_state != VOICE_STATE_IDLE) {
            continue;
        }

        /* === RECORDING === */
        set_state(VOICE_STATE_RECORDING);
        xEventGroupClearBits(s_events, EVT_STT_DONE);

        /* Send audio_start */
        ws_send_json("audio_start", NULL);

        bool resume_wake_listening = false;
        if (audio_is_wake_word_enabled() && audio_is_listening()) {
            audio_stop_listening();
            resume_wake_listening = true;
        }

        esp_err_t ret = audio_mic_enable();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable mic: %s", esp_err_to_name(ret));
            if (resume_wake_listening) {
                esp_err_t listen_ret = audio_start_listening();
                if (listen_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to resume wake listening: %s", esp_err_to_name(listen_ret));
                }
            }
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        const size_t chunk_bytes = 1024;
        uint8_t *chunk_buf = malloc(chunk_bytes);
        if (!chunk_buf) {
            audio_mic_disable();
            if (resume_wake_listening) {
                esp_err_t listen_ret = audio_start_listening();
                if (listen_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to resume wake listening: %s", esp_err_to_name(listen_ret));
                }
            }
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        const size_t max_bytes = (size_t)s_config.max_record_sec
                                 * MIMI_AUDIO_MIC_SAMPLE_RATE * 2;
        size_t total_sent = 0;

        ESP_LOGI(TAG, "Recording... (max %d s)", s_config.max_record_sec);

        /* For wake word, use VAD to detect speech end; for button, use button release */
        bool recording = true;
        uint32_t silence_start_ms = 0;
        int vad_threshold = audio_get_vad_threshold();
        if (vad_threshold <= 0) {
            vad_threshold = 500;
        }
        uint32_t silence_timeout_ms = (uint32_t)audio_get_silence_timeout_ms();
        if (silence_timeout_ms == 0) {
            silence_timeout_ms = 1500;
        }

        while (recording && total_sent < max_bytes) {
            if (wake_word_detected) {
                /* For wake word, use simple silence detection */
                size_t bytes_read = 0;
                ret = audio_mic_read(chunk_buf, chunk_bytes, &bytes_read, 500);
                if (ret == ESP_OK && bytes_read > 0) {
                    /* Apply software gain inline */
                    int16_t *samples = (int16_t *)chunk_buf;
                    int num_samples = bytes_read / 2;
                    int16_t peak = 0;
                    for (int i = 0; i < num_samples; i++) {
                        int16_t abs_val = samples[i] < 0 ? -samples[i] : samples[i];
                        if (abs_val > peak) peak = abs_val;
                    }
                    if (peak > 0 && peak < 20000) {
                        int gain = 20000 / peak;
                        if (gain > 32) gain = 32;
                        if (gain > 1) {
                            for (int i = 0; i < num_samples; i++) {
                                int32_t amplified = (int32_t)samples[i] * gain;
                                if (amplified > 32767) amplified = 32767;
                                if (amplified < -32768) amplified = -32768;
                                samples[i] = (int16_t)amplified;
                            }
                        }
                    }

                    /* Check for silence */
                    int16_t max_val = 0;
                    for (int i = 0; i < num_samples; i++) {
                        int16_t abs_val = samples[i] < 0 ? -samples[i] : samples[i];
                        if (abs_val > max_val) max_val = abs_val;
                    }

                    if (max_val < vad_threshold) {
                        if (silence_start_ms == 0) {
                            silence_start_ms = esp_timer_get_time() / 1000;
                        } else if (esp_timer_get_time() / 1000 - silence_start_ms > silence_timeout_ms) {
                            recording = false;
                        }
                    } else {
                        silence_start_ms = 0;
                    }

                    /* Stream chunk to gateway */
                    ws_send_binary(chunk_buf, bytes_read);
                    total_sent += bytes_read;
                }
            } else if (s_config.button_gpio >= 0) {
                /* For button, check if button released */
                if (gpio_get_level(s_config.button_gpio) != 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    if (gpio_get_level(s_config.button_gpio) != 0) {
                        recording = false;
                        continue;
                    }
                }

                size_t bytes_read = 0;
                ret = audio_mic_read(chunk_buf, chunk_bytes, &bytes_read, 500);
                if (ret == ESP_OK && bytes_read > 0) {
                    /* Apply software gain inline */
                    int16_t *samples = (int16_t *)chunk_buf;
                    int num_samples = bytes_read / 2;
                    int16_t peak = 0;
                    for (int i = 0; i < num_samples; i++) {
                        int16_t abs_val = samples[i] < 0 ? -samples[i] : samples[i];
                        if (abs_val > peak) peak = abs_val;
                    }
                    if (peak > 0 && peak < 20000) {
                        int gain = 20000 / peak;
                        if (gain > 32) gain = 32;
                        if (gain > 1) {
                            for (int i = 0; i < num_samples; i++) {
                                int32_t amplified = (int32_t)samples[i] * gain;
                                if (amplified > 32767) amplified = 32767;
                                if (amplified < -32768) amplified = -32768;
                                samples[i] = (int16_t)amplified;
                            }
                        }
                    }

                    /* Stream chunk to gateway */
                    ws_send_binary(chunk_buf, bytes_read);
                    total_sent += bytes_read;
                }
            }
        }

        audio_mic_disable();
        if (resume_wake_listening) {
            esp_err_t listen_ret = audio_start_listening();
            if (listen_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to resume wake listening: %s", esp_err_to_name(listen_ret));
            }
        }
        free(chunk_buf);

        ESP_LOGI(TAG, "Recorded %d bytes (%.1f s)", (int)total_sent, total_sent / 32000.0f);

        if (total_sent < 3200) {
            ESP_LOGW(TAG, "Recording too short, discarding");
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        /* Send audio_end → triggers STT on gateway */
        set_state(VOICE_STATE_PROCESSING);
        ws_send_json("audio_end", NULL);

        /* Wait for STT result (timeout 30s) */
        bits = xEventGroupWaitBits(
            s_events, EVT_STT_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(30000));

        if (!(bits & EVT_STT_DONE)) {
            ESP_LOGW(TAG, "STT timeout");
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        if (!s_stt_text || s_stt_text[0] == '\0') {
            ESP_LOGW(TAG, "STT returned empty text");
            free(s_stt_text);
            s_stt_text = NULL;
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        ESP_LOGI(TAG, "STT: \"%s\"", s_stt_text);
        display_show_message("user", s_stt_text);

        /* Push to message bus → agent loop processes → outbound dispatch calls voice_channel_speak() */
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_VOICE, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice", sizeof(msg.chat_id) - 1);
        strncpy(msg.media_type, "voice", sizeof(msg.media_type) - 1);
        msg.content = s_stt_text;  /* bus takes ownership */
        s_stt_text = NULL;

        if (message_bus_push_inbound(&msg) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to push to message bus");
            message_bus_msg_free(&msg);
            set_state(VOICE_STATE_IDLE);
            continue;
        }

        /* Wait for agent response (voice_channel_speak sets PLAYING → TTS → IDLE) */
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

/* ---------- WebSocket connection management ---------- */

static esp_err_t ws_connect(void)
{
    if (s_ws_client) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Connecting to %s", s_config.gateway_url);
    set_state(VOICE_STATE_CONNECTING);

    esp_websocket_client_config_t ws_cfg = {
        .uri = s_config.gateway_url,
        .buffer_size = 4096,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 20,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (!s_ws_client) {
        ESP_LOGE(TAG, "WS client init failed");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    esp_err_t ret = esp_websocket_client_start(s_ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS client start failed: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }
    return ret;
}

/* ---------- Public API ---------- */

esp_err_t voice_channel_init(const voice_channel_config_t *config)
{
    memcpy(&s_config, config, sizeof(s_config));
    char normalized[sizeof(s_config.gateway_url)] = {0};
    if (normalize_gateway_url(s_config.gateway_url, normalized, sizeof(normalized)) == ESP_OK) {
        strncpy(s_config.gateway_url, normalized, sizeof(s_config.gateway_url) - 1);
        s_config.gateway_url[sizeof(s_config.gateway_url) - 1] = '\0';
    }
    load_gateway_url();

    if (s_config.gateway_url[0] == '\0') {
        ESP_LOGW(TAG, "No voice gateway URL configured");
    }

    /* Create event group */
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    /* Configure button GPIO if enabled */
    if (s_config.button_gpio >= 0) {
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
    }

    /* Register audio event callback for wake word detection */
    audio_set_event_callback(audio_event_handler, NULL);

    ESP_LOGI(TAG, "Voice channel initialized (GPIO %d, gateway %s)",
             s_config.button_gpio, s_config.gateway_url);
    return ESP_OK;
}

esp_err_t voice_channel_start(void)
{
    if (s_task_handle) return ESP_ERR_INVALID_STATE;

    /* Start WebSocket connection */
    esp_err_t ret = ws_connect();
    if (ret != ESP_OK) return ret;

    /* Start voice task */
    BaseType_t xret = xTaskCreatePinnedToCore(
        voice_task, "voice",
        MIMI_VOICE_TASK_STACK, NULL,
        MIMI_VOICE_TASK_PRIO, &s_task_handle,
        MIMI_VOICE_TASK_CORE);

    return (xret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void voice_channel_stop(void)
{
    audio_set_event_callback(NULL, NULL);

    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }
    if (s_config.button_gpio >= 0) {
        gpio_isr_handler_remove(s_config.button_gpio);
    }
    free(s_stt_text);
    s_stt_text = NULL;
    free(s_json_buf);
    s_json_buf = NULL;
    s_json_len = 0;
    s_json_cap = 0;
    s_followup_deadline_ms = 0;
    s_playback_started_ms = 0;
    if (s_events) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    s_state = VOICE_STATE_IDLE;
}

esp_err_t voice_channel_speak(const char *text)
{
    if (!text || text[0] == '\0') {
        set_state(VOICE_STATE_IDLE);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ws_client || !esp_websocket_client_is_connected(s_ws_client)) {
        ESP_LOGE(TAG, "WS not connected, cannot TTS");
        set_state(VOICE_STATE_IDLE);
        return ESP_ERR_INVALID_STATE;
    }

    set_state(VOICE_STATE_PLAYING);
    s_playback_started_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "TTS speak: \"%.*s\"", 200, text);
    display_show_message("assistant", text);

    /* Clear TTS done event */
    xEventGroupClearBits(s_events, EVT_TTS_DONE);

    /* Send TTS request — gateway will stream PCM back via binary frames */
    cJSON *extra = cJSON_CreateObject();
    cJSON_AddStringToObject(extra, "text", text);
    cJSON_AddStringToObject(extra, "voice", "zh-CN-XiaoxiaoNeural");
    cJSON_AddStringToObject(extra, "rate", MIMI_VOICE_TTS_RATE);
    ws_send_json("tts_request", extra);
    cJSON_Delete(extra);

    /* Wait for TTS to complete (tts_end or interrupt) */
    EventBits_t bits = xEventGroupWaitBits(
        s_events, EVT_TTS_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(60000));

    if (!(bits & EVT_TTS_DONE)) {
        ESP_LOGW(TAG, "TTS timeout");
        audio_spk_disable();
    }

    s_followup_deadline_ms = (esp_timer_get_time() / 1000) + MIMI_VOICE_FOLLOWUP_WINDOW_MS;
    s_playback_started_ms = 0;
    ESP_LOGI(TAG, "Follow-up window opened for %d ms", MIMI_VOICE_FOLLOWUP_WINDOW_MS);
    set_state(VOICE_STATE_IDLE);
    return ESP_OK;
}

voice_state_t voice_channel_get_state(void)
{
    return s_state;
}

bool voice_channel_is_connected(void)
{
    return s_ws_client && esp_websocket_client_is_connected(s_ws_client);
}

esp_err_t voice_channel_set_gateway(const char *url)
{
    if (!url || strlen(url) >= sizeof(s_config.gateway_url)) {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized[sizeof(s_config.gateway_url)] = {0};
    esp_err_t norm_ret = normalize_gateway_url(url, normalized, sizeof(normalized));
    if (norm_ret != ESP_OK) {
        return norm_ret;
    }

    strncpy(s_config.gateway_url, normalized, sizeof(s_config.gateway_url) - 1);
    s_config.gateway_url[sizeof(s_config.gateway_url) - 1] = '\0';

    /* Persist to NVS */
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MIMI_NVS_VOICE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        nvs_set_str(nvs, MIMI_NVS_KEY_VOICE_GW, s_config.gateway_url);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    /* Reconnect with new URL */
    if (s_ws_client) {
        ESP_LOGI(TAG, "Reconnecting to new gateway: %s", s_config.gateway_url);
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        xEventGroupClearBits(s_events, EVT_WS_CONNECTED);
        ws_connect();
    }

    ESP_LOGI(TAG, "Gateway URL set: %s", s_config.gateway_url);
    return ESP_OK;
}
