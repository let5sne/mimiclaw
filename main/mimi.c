#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "feishu/feishu_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "security/access_control.h"
#include "heartbeat/heartbeat_service.h"
#include "cron/cron_service.h"
#include "skills/skill_loader.h"
#include "display/display.h"
#include "display/font_cjk.h"
#include "audio/audio.h"
#include "status/status_led.h"
#include "voice/voice_channel.h"
#include "buttons/button_driver.h"
#include "imu/imu_manager.h"
#include "skills/skill_loader.h"

static const char *TAG = "mimi";

#if MIMI_VOICE_ENABLED && MIMI_AUDIO_ENABLED && MIMI_VOICE_MIRROR_TELEGRAM
static size_t outbound_utf8_char_len(unsigned char c)
{
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static bool outbound_match_utf8_token(const char *s, const char *token)
{
    return s && token && strncmp(s, token, strlen(token)) == 0;
}

static void outbound_append_compact_text(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) return;

    size_t out = 0;
    bool prev_space = false;
    bool at_line_start = true;

    for (const char *p = src; *p != '\0' && out + 1 < dst_size;) {
        unsigned char c = (unsigned char)*p;

        if (c == '\r') {
            p++;
            continue;
        }

        if (c == '\n' || c == '\t' || c == ' ') {
            if (out > 0 && !prev_space) {
                dst[out++] = ' ';
                prev_space = true;
            }
            at_line_start = true;
            p++;
            continue;
        }

        if (c == '#' || c == '*' || c == '`' || c == '_' || c == '~') {
            p++;
            continue;
        }

        if (c == '|') {
            if (out > 0 && !prev_space) {
                dst[out++] = ' ';
                prev_space = true;
            }
            at_line_start = true;
            p++;
            continue;
        }

        if ((c == '-' || c == '+') && at_line_start) {
            p++;
            continue;
        }

        size_t char_len = outbound_utf8_char_len(c);
        if (out + char_len >= dst_size) break;
        memcpy(dst + out, p, char_len);
        out += char_len;
        p += char_len;
        prev_space = false;
        at_line_start = false;
    }

    while (out > 0 && dst[out - 1] == ' ') {
        out--;
    }
    dst[out] = '\0';
}

static bool outbound_is_sentence_break(const char *s)
{
    return outbound_match_utf8_token(s, "。") ||
           outbound_match_utf8_token(s, "！") ||
           outbound_match_utf8_token(s, "？") ||
           outbound_match_utf8_token(s, "!") ||
           outbound_match_utf8_token(s, "?");
}

static bool outbound_is_soft_break(const char *s)
{
    return outbound_is_sentence_break(s) ||
           outbound_match_utf8_token(s, "，") ||
           outbound_match_utf8_token(s, "、") ||
           outbound_match_utf8_token(s, "；") ||
           outbound_match_utf8_token(s, ";") ||
           outbound_match_utf8_token(s, ",") ||
           outbound_match_utf8_token(s, "：") ||
           outbound_match_utf8_token(s, ":") ||
           outbound_match_utf8_token(s, " ");
}

static void outbound_build_voice_summary(const char *src, char *dst, size_t dst_size)
{
    static const char *suffix = "。详细内容已发到 Telegram。";
    char compact[512] = {0};
    size_t out = 0;
    size_t last_sentence_end = 0;
    size_t last_soft_break = 0;
    int sentence_count = 0;
    bool truncated = false;

    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src || src[0] == '\0') return;

    outbound_append_compact_text(src, compact, sizeof(compact));
    if (compact[0] == '\0') return;

    for (const char *p = compact; *p != '\0';) {
        size_t char_len = outbound_utf8_char_len((unsigned char)*p);
        if (out + char_len >= dst_size) {
            truncated = true;
            break;
        }
        if (out + char_len > MIMI_VOICE_SUMMARY_MAX_BYTES) {
            truncated = true;
            break;
        }

        memcpy(dst + out, p, char_len);
        out += char_len;

        if (outbound_is_sentence_break(p)) {
            last_sentence_end = out;
            sentence_count++;
            if (sentence_count >= MIMI_VOICE_SUMMARY_MAX_SENTENCES) {
                p += char_len;
                truncated = (*p != '\0');
                break;
            }
        } else if (outbound_is_soft_break(p)) {
            last_soft_break = out;
        }

        p += char_len;
    }

    dst[out] = '\0';

    if (truncated) {
        size_t cutoff = out;
        size_t suffix_len = strlen(suffix);
        if (last_sentence_end > 0 && last_sentence_end + suffix_len < dst_size) {
            cutoff = last_sentence_end;
        } else if (last_soft_break > 0 && last_soft_break + suffix_len < dst_size) {
            cutoff = last_soft_break;
        }
        while (cutoff > 0 && dst[cutoff - 1] == ' ') {
            cutoff--;
        }
        dst[cutoff] = '\0';
        if (cutoff + suffix_len < dst_size) {
            memcpy(dst + cutoff, suffix, suffix_len + 1);
        }
    }
}

static void outbound_mirror_telegram_to_voice(const mimi_msg_t *msg, bool is_status)
{
    char summary[256] = {0};

    if (!msg || is_status || !msg->content || msg->content[0] == '\0') {
        return;
    }
    if (!voice_channel_is_connected()) {
        return;
    }
    voice_state_t state = voice_channel_get_state();
    if (state != VOICE_STATE_IDLE) {
        ESP_LOGI(TAG, "Voice mirror skipped: channel busy (state=%d)",
                 (int)state);
        return;
    }

    outbound_build_voice_summary(msg->content, summary, sizeof(summary));
    if (summary[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "Voice mirror summary: \"%.*s\"", 160, summary);
    esp_err_t speak_ret = voice_channel_speak(summary);
    if (speak_ret != ESP_OK) {
        ESP_LOGW(TAG, "Voice mirror failed: %s", esp_err_to_name(speak_ret));
    }
}
#endif

static bool outbound_is_status_text(const char *text)
{
    if (!text) return false;
    return (strncmp(text, "mimi", 4) == 0) && (strstr(text, "...") != NULL);
}

static uint32_t outbound_send_retry_delay_ms(int attempt)
{
    uint32_t delay = MIMI_OUTBOUND_SEND_RETRY_BASE_MS;
    for (int i = 1; i < attempt; i++) {
        delay <<= 1;
        if (delay > 5000) {
            delay = 5000;
            break;
        }
    }
    return delay;
}

static esp_err_t outbound_send_once(const mimi_msg_t *msg, bool is_status)
{
    if (strcmp(msg->channel, MIMI_CHAN_TELEGRAM) == 0) {
        esp_err_t ret = telegram_send_message(msg->chat_id, msg->content);
        if (ret != ESP_OK) {
            return ret;
        }
#if MIMI_VOICE_ENABLED && MIMI_AUDIO_ENABLED && MIMI_VOICE_MIRROR_TELEGRAM
        outbound_mirror_telegram_to_voice(msg, is_status);
#endif
        return ESP_OK;
    }

    if (strcmp(msg->channel, MIMI_CHAN_FEISHU) == 0) {
        return feishu_send_message(msg->chat_id, msg->content);
    }

    if (strcmp(msg->channel, MIMI_CHAN_WEBSOCKET) == 0) {
        return ws_server_send(msg->chat_id, msg->content);
    }

    if (strcmp(msg->channel, MIMI_CHAN_VOICE) == 0) {
        if (is_status) {
            ESP_LOGI(TAG, "Voice: skipping status msg");
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Voice outbound: \"%.*s\"", 200, msg->content);
        esp_err_t ret = voice_channel_speak(msg->content);
        ESP_LOGI(TAG, "Voice outbound done: ret=%s", esp_err_to_name(ret));
        return ret;
    }

    if (strcmp(msg->channel, MIMI_CHAN_SYSTEM) == 0) {
        ESP_LOGI(TAG, "System outbound (local-only): \"%.*s\"", 200, msg->content);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Unknown channel: %s", msg->channel);
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t outbound_send_with_retry(const mimi_msg_t *msg)
{
    bool is_status = outbound_is_status_text(msg->content);
    int max_attempts = is_status ? 1 : MIMI_OUTBOUND_SEND_RETRY_MAX;
    esp_err_t last_err = ESP_FAIL;

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        last_err = outbound_send_once(msg, is_status);
        if (last_err == ESP_OK) {
            return ESP_OK;
        }

        if (attempt < max_attempts) {
            uint32_t delay_ms = outbound_send_retry_delay_ms(attempt);
            ESP_LOGW(TAG, "Outbound send failed for %s:%s (%s), retry %d/%d in %" PRIu32 " ms",
                     msg->channel, msg->chat_id, esp_err_to_name(last_err),
                     attempt, max_attempts, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    return last_err;
}

static esp_err_t init_display(void)
{
#if MIMI_DISPLAY_ENABLED
    display_config_t config = {
        .type = MIMI_DISPLAY_TYPE,
        .width = MIMI_DISPLAY_WIDTH,
        .height = MIMI_DISPLAY_HEIGHT,
        .i2c_port = MIMI_DISPLAY_I2C_PORT,
        .sda_pin = MIMI_DISPLAY_SDA_PIN,
        .scl_pin = MIMI_DISPLAY_SCL_PIN,
        .i2c_addr = MIMI_DISPLAY_I2C_ADDR,
        .spi_host = MIMI_DISPLAY_SPI_HOST,
        .mosi_pin = MIMI_DISPLAY_MOSI_PIN,
        .sclk_pin = MIMI_DISPLAY_SCLK_PIN,
        .cs_pin = MIMI_DISPLAY_CS_PIN,
        .dc_pin = MIMI_DISPLAY_DC_PIN,
        .rst_pin = MIMI_DISPLAY_RST_PIN,
        .backlight_pin = MIMI_DISPLAY_BL_PIN,
    };

    esp_err_t ret = display_init(&config);
    if (ret == ESP_OK) {
        display_set_status("Initializing...");
        display_set_display_status(DISPLAY_STATUS_IDLE);
    }
    return ret;
#else
    ESP_LOGI(TAG, "Display disabled");
    return ESP_OK;
#endif
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MIMI_SPIFFS_BASE,
        // 存在 model 与 spiffs 两个 data/spiffs 分区时，必须显式指定业务 SPIFFS 分区
        .partition_label = "spiffs",
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

    return ESP_OK;
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg)
{
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        mimi_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) continue;

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

        esp_err_t send_err = outbound_send_with_retry(&msg);
        if (send_err != ESP_OK) {
            agent_loop_record_outbound_send_failure();
            ESP_LOGE(TAG, "Outbound send failed permanently for %s:%s (%s)",
                     msg.channel, msg.chat_id, esp_err_to_name(send_err));
        }

        message_bus_msg_free(&msg);
    }
}

void app_main(void)
{
    /* Silence noisy components */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("i2c", ESP_LOG_ERROR);
    esp_log_level_set("QRCODE", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MimiClaw - ESP32-S3 AI Agent");
    ESP_LOGI(TAG, "========================================");

    /* Print memory info */
    ESP_LOGI(TAG, "Internal free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "PSRAM free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());

    /* Load timezone from NVS (falls back to MIMI_TIMEZONE default) */
    {
        nvs_handle_t nvs;
        char tz_buf[64] = MIMI_TIMEZONE;
        if (nvs_open(MIMI_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t tz_len = sizeof(tz_buf);
            if (nvs_get_str(nvs, MIMI_NVS_KEY_TIMEZONE, tz_buf, &tz_len) != ESP_OK) {
                strncpy(tz_buf, MIMI_TIMEZONE, sizeof(tz_buf) - 1);
            }
            nvs_close(nvs);
        }
        setenv("TZ", tz_buf, 1);
        tzset();
        ESP_LOGI(TAG, "Timezone: %s", tz_buf);
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());
    {
        esp_err_t led_ret = status_led_init();
        if (led_ret != ESP_OK) {
            ESP_LOGW(TAG, "Status LED init failed: %s", esp_err_to_name(led_ret));
        }
    }

    /* Load CJK font (non-fatal if missing) */
    font_cjk_init("/spiffs/fonts/unifont_cjk.bin");

    /* Initialize display early */
    init_display();

#if MIMI_AUDIO_ENABLED
    /* Initialize audio */
    {
        audio_config_t audio_cfg = {
            .mic_i2s_port = MIMI_AUDIO_MIC_I2S_PORT,
            .mic_ws_pin = MIMI_AUDIO_MIC_WS_PIN,
            .mic_sck_pin = MIMI_AUDIO_MIC_SCK_PIN,
            .mic_sd_pin = MIMI_AUDIO_MIC_SD_PIN,
            .mic_sample_rate = MIMI_AUDIO_MIC_SAMPLE_RATE,
            .mic_bits_per_sample = MIMI_AUDIO_MIC_BITS,
            .spk_i2s_port = MIMI_AUDIO_SPK_I2S_PORT,
            .spk_ws_pin = MIMI_AUDIO_SPK_WS_PIN,
            .spk_sck_pin = MIMI_AUDIO_SPK_SCK_PIN,
            .spk_sd_pin = MIMI_AUDIO_SPK_SD_PIN,
            .spk_sample_rate = MIMI_AUDIO_SPK_SAMPLE_RATE,
            .spk_bits_per_sample = MIMI_AUDIO_SPK_BITS,
            .enable_wake_word = true,
            .wake_word = MIMI_AUDIO_WAKE_WORD,
            .wake_word_threshold = MIMI_AUDIO_WAKE_THRESHOLD,
            .vad_threshold = 50,
            .silence_timeout_ms = 1000,
        };
        esp_err_t audio_ret = audio_init(&audio_cfg);
        if (audio_ret != ESP_OK) {
            ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(audio_ret));
        } else if (audio_is_wake_word_enabled()) {
            /* Start listening for wake word */
            audio_ret = audio_start_listening();
            if (audio_ret == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGI(TAG, "Wake word listening disabled: WakeNet model unavailable");
            } else if (audio_ret != ESP_OK) {
                ESP_LOGW(TAG, "Audio start listening failed: %s", esp_err_to_name(audio_ret));
            }
        }
    }
#endif

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(skill_loader_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(access_control_init());
    ESP_ERROR_CHECK(telegram_bot_init());
    ESP_ERROR_CHECK(feishu_bot_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    ESP_ERROR_CHECK(agent_loop_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    /* Start WiFi */
    display_set_status("Connecting WiFi...");
    display_set_display_status(DISPLAY_STATUS_CONNECTING);

    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());

            display_set_status("WiFi Connected");
            display_set_display_status(DISPLAY_STATUS_CONNECTED);
            vTaskDelay(pdMS_TO_TICKS(1000));

            /* Start network-dependent services */
            ESP_ERROR_CHECK(telegram_bot_start());
            ESP_ERROR_CHECK(agent_loop_start());
            ESP_ERROR_CHECK(ws_server_start());
            ESP_ERROR_CHECK(feishu_bot_start());

#if MIMI_HEARTBEAT_ENABLED
            {
                esp_err_t hb_err = heartbeat_service_init();
                if (hb_err == ESP_OK) {
                    hb_err = heartbeat_service_start();
                }
                if (hb_err != ESP_OK) {
                    ESP_LOGW(TAG, "Heartbeat disabled due to init/start failure: %s",
                             esp_err_to_name(hb_err));
                } else {
                    ESP_LOGI(TAG, "Heartbeat service started");
                }
            }
#endif

#if MIMI_CRON_ENABLED
            {
                esp_err_t cron_err = cron_service_init();
                if (cron_err == ESP_OK) {
                    cron_err = cron_service_start();
                }
                if (cron_err != ESP_OK) {
                    ESP_LOGW(TAG, "Cron disabled due to init/start failure: %s",
                             esp_err_to_name(cron_err));
                } else {
                    ESP_LOGI(TAG, "Cron service started");
                }
            }
#endif

            /* Outbound dispatch task */
            ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
                outbound_dispatch_task, "outbound",
                MIMI_OUTBOUND_STACK, NULL,
                MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE) == pdPASS)
                ? ESP_OK : ESP_FAIL);

            display_set_status("MimiClaw Ready");
            display_set_display_status(DISPLAY_STATUS_IDLE);

#if MIMI_VOICE_ENABLED && MIMI_AUDIO_ENABLED
            /* Initialize and start voice channel */
            {
                voice_channel_config_t voice_cfg = {
                    .button_gpio = MIMI_VOICE_BUTTON_PIN,
                    .max_record_sec = MIMI_VOICE_MAX_RECORD_S,
                };
                strncpy(voice_cfg.gateway_url, MIMI_VOICE_GATEWAY_URL,
                        sizeof(voice_cfg.gateway_url) - 1);

                esp_err_t voice_ret = voice_channel_init(&voice_cfg);
                if (voice_ret == ESP_OK) {
                    voice_channel_start();
                    ESP_LOGI(TAG, "Voice channel started (button GPIO: %d, wake word enabled: %s)", 
                             MIMI_VOICE_BUTTON_PIN, audio_is_wake_word_enabled() ? "true" : "false");
                } else {
                    ESP_LOGW(TAG, "Voice channel init failed: %s",
                             esp_err_to_name(voice_ret));
                }
            }
#endif

            ESP_LOGI(TAG, "All services started!");
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout. Check MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
            display_set_status("WiFi Timeout");
            display_set_display_status(DISPLAY_STATUS_ERROR);
        }
    } else {
        ESP_LOGI(TAG, "No WiFi credentials configured. Set MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
        display_set_status("No WiFi Config");
        display_set_display_status(DISPLAY_STATUS_ERROR);
    }

    ESP_LOGI(TAG, "MimiClaw ready. Type 'help' for CLI commands.");
}
