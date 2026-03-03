#include "tools/tool_device_info.h"
#include "mimi_config.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_psram.h"
#include "esp_system.h"

static const char *TAG = "tool_device";

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
        case CHIP_ESP32:
            return "ESP32";
        case CHIP_ESP32S2:
            return "ESP32-S2";
        case CHIP_ESP32S3:
            return "ESP32-S3";
        case CHIP_ESP32C3:
            return "ESP32-C3";
        case CHIP_ESP32C2:
            return "ESP32-C2";
        case CHIP_ESP32C6:
            return "ESP32-C6";
        case CHIP_ESP32H2:
            return "ESP32-H2";
        case CHIP_ESP32P4:
            return "ESP32-P4";
        case CHIP_ESP32C61:
            return "ESP32-C61";
        case CHIP_ESP32C5:
            return "ESP32-C5";
        case CHIP_ESP32H21:
            return "ESP32-H21";
        case CHIP_ESP32H4:
            return "ESP32-H4";
        case CHIP_POSIX_LINUX:
            return "POSIX/Linux";
        default:
            return "unknown";
    }
}

static const char *display_type_name(int type)
{
    switch (type) {
        case 0:
            return "none";
        case 1:
            return "ssd1306";
        case 2:
            return "st7789";
        case 3:
            return "ili9341";
        default:
            return "unknown";
    }
}

static void add_feature_flags(cJSON *obj, uint32_t features)
{
    cJSON_AddBoolToObject(obj, "wifi_bgn", (features & CHIP_FEATURE_WIFI_BGN) != 0);
    cJSON_AddBoolToObject(obj, "ble", (features & CHIP_FEATURE_BLE) != 0);
    cJSON_AddBoolToObject(obj, "bt", (features & CHIP_FEATURE_BT) != 0);
    cJSON_AddBoolToObject(obj, "embedded_flash", (features & CHIP_FEATURE_EMB_FLASH) != 0);
    cJSON_AddBoolToObject(obj, "embedded_psram", (features & CHIP_FEATURE_EMB_PSRAM) != 0);
}

esp_err_t tool_get_device_info_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_bytes = 0;
    uint32_t flash_configured_bytes = 0;
    esp_err_t flash_err = esp_flash_get_physical_size(NULL, &flash_bytes);
    esp_err_t flash_cfg_err = esp_flash_get_size(NULL, &flash_configured_bytes);

    bool psram_initialized = esp_psram_is_initialized();
    size_t psram_bytes = psram_initialized ? esp_psram_get_size() : 0;

    cJSON *root = cJSON_CreateObject();
    cJSON *chip_obj = cJSON_AddObjectToObject(root, "chip");
    cJSON_AddStringToObject(chip_obj, "model", chip_model_name(chip.model));
    cJSON_AddNumberToObject(chip_obj, "cores", chip.cores);
    cJSON_AddNumberToObject(chip_obj, "revision", chip.revision);
    cJSON *features_obj = cJSON_AddObjectToObject(chip_obj, "features");
    add_feature_flags(features_obj, chip.features);

    cJSON *cpu_obj = cJSON_AddObjectToObject(root, "cpu");
    int cpu_hz = esp_clk_cpu_freq();
    cJSON_AddNumberToObject(cpu_obj, "freq_hz", cpu_hz);
    cJSON_AddNumberToObject(cpu_obj, "freq_mhz", cpu_hz / 1000000);

    cJSON *flash_obj = cJSON_AddObjectToObject(root, "flash");
    if (flash_err == ESP_OK) {
        cJSON_AddNumberToObject(flash_obj, "bytes", (double)flash_bytes);
        cJSON_AddNumberToObject(flash_obj, "mib", (double)flash_bytes / (1024.0 * 1024.0));
        if (flash_cfg_err == ESP_OK) {
            cJSON_AddNumberToObject(flash_obj, "configured_bytes",
                                    (double)flash_configured_bytes);
            cJSON_AddNumberToObject(flash_obj, "configured_mib",
                                    (double)flash_configured_bytes / (1024.0 * 1024.0));
        }
    } else {
        cJSON_AddStringToObject(flash_obj, "error", esp_err_to_name(flash_err));
    }

    cJSON *psram_obj = cJSON_AddObjectToObject(root, "psram");
    cJSON_AddBoolToObject(psram_obj, "initialized", psram_initialized);
    cJSON_AddNumberToObject(psram_obj, "bytes", (double)psram_bytes);
    cJSON_AddNumberToObject(psram_obj, "mib", (double)psram_bytes / (1024.0 * 1024.0));
    cJSON_AddNumberToObject(psram_obj, "free_bytes",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    cJSON *memory_obj = cJSON_AddObjectToObject(root, "memory");
    cJSON_AddNumberToObject(memory_obj, "free_heap_bytes", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(memory_obj, "min_free_heap_bytes",
                            (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(memory_obj, "free_internal_bytes",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    cJSON *gpio_obj = cJSON_AddObjectToObject(root, "gpio");
    cJSON_AddNumberToObject(gpio_obj, "status_led", MIMI_STATUS_LED_PIN);
    cJSON_AddNumberToObject(gpio_obj, "voice_button", MIMI_VOICE_BUTTON_PIN);

    cJSON *display_obj = cJSON_AddObjectToObject(root, "display");
    cJSON_AddBoolToObject(display_obj, "enabled", MIMI_DISPLAY_ENABLED);
    cJSON_AddStringToObject(display_obj, "type", display_type_name(MIMI_DISPLAY_TYPE));
    cJSON_AddNumberToObject(display_obj, "width", MIMI_DISPLAY_WIDTH);
    cJSON_AddNumberToObject(display_obj, "height", MIMI_DISPLAY_HEIGHT);
    cJSON_AddNumberToObject(display_obj, "mosi", MIMI_DISPLAY_MOSI_PIN);
    cJSON_AddNumberToObject(display_obj, "sclk", MIMI_DISPLAY_SCLK_PIN);
    cJSON_AddNumberToObject(display_obj, "cs", MIMI_DISPLAY_CS_PIN);
    cJSON_AddNumberToObject(display_obj, "dc", MIMI_DISPLAY_DC_PIN);
    cJSON_AddNumberToObject(display_obj, "rst", MIMI_DISPLAY_RST_PIN);
    cJSON_AddNumberToObject(display_obj, "bl", MIMI_DISPLAY_BL_PIN);

    cJSON *audio_obj = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddBoolToObject(audio_obj, "enabled", MIMI_AUDIO_ENABLED);
    cJSON *mic_obj = cJSON_AddObjectToObject(audio_obj, "mic");
    cJSON_AddNumberToObject(mic_obj, "ws", MIMI_AUDIO_MIC_WS_PIN);
    cJSON_AddNumberToObject(mic_obj, "sck", MIMI_AUDIO_MIC_SCK_PIN);
    cJSON_AddNumberToObject(mic_obj, "sd", MIMI_AUDIO_MIC_SD_PIN);
    cJSON *spk_obj = cJSON_AddObjectToObject(audio_obj, "speaker");
    cJSON_AddNumberToObject(spk_obj, "ws", MIMI_AUDIO_SPK_WS_PIN);
    cJSON_AddNumberToObject(spk_obj, "sck", MIMI_AUDIO_SPK_SCK_PIN);
    cJSON_AddNumberToObject(spk_obj, "sd", MIMI_AUDIO_SPK_SD_PIN);

    cJSON *voice_obj = cJSON_AddObjectToObject(root, "voice");
    cJSON_AddBoolToObject(voice_obj, "enabled", MIMI_VOICE_ENABLED);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        snprintf(output, output_size, "Error: failed to encode device info");
        return ESP_ERR_NO_MEM;
    }

    strncpy(output, json, output_size - 1);
    output[output_size - 1] = '\0';
    free(json);

    ESP_LOGI(TAG, "Device info collected");
    return ESP_OK;
}
