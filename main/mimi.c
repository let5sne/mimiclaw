#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
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
#include "voice/voice_channel.h"
#include "buttons/button_driver.h"
#include "imu/imu_manager.h"
#include "skills/skill_loader.h"

static const char *TAG = "mimi";

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
        return telegram_send_message(msg->chat_id, msg->content);
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
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());

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
