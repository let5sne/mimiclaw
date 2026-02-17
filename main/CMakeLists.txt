idf_component_register(
    SRCS
        "mimi.c"
        "bus/message_bus.c"
        "wifi/wifi_manager.c"
        "telegram/telegram_bot.c"
        "llm/llm_proxy.c"
        "agent/agent_loop.c"
        "agent/context_builder.c"
        "memory/memory_store.c"
        "memory/session_mgr.c"
        "gateway/ws_server.c"
        "cli/serial_cli.c"
        "ota/ota_manager.c"
        "proxy/http_proxy.c"
        "security/access_control.c"
        "heartbeat/heartbeat_service.c"
        "cron/cron_service.c"
        "display/display.c"
        "display/ssd1306.c"
        "display/font_cjk.c"
        "audio/audio.c"
        "voice/voice_channel.c"
        "tools/tool_registry.c"
        "tools/tool_web_search.c"
        "tools/tool_get_time.c"
        "tools/tool_files.c"
        "tools/tool_memory.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client esp_http_server
        esp_https_ota esp_event json spiffs console vfs app_update esp-tls driver
        esp_websocket_client
)

# 将 spiffs_data 打包到 spiffs 分区，并在 `idf.py flash` 时自动烧录
set(MIMI_SPIFFS_IMAGE_DIR "${CMAKE_SOURCE_DIR}/spiffs_data")
if (EXISTS "${MIMI_SPIFFS_IMAGE_DIR}")
    spiffs_create_partition_image(spiffs "${MIMI_SPIFFS_IMAGE_DIR}" FLASH_IN_PROJECT)
endif()
