#pragma once

/* MimiClaw Global Configuration */

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("mimi_secrets.h")
#include "mimi_secrets.h"
#endif

#ifndef MIMI_SECRET_WIFI_SSID
#define MIMI_SECRET_WIFI_SSID       ""
#endif
#ifndef MIMI_SECRET_WIFI_PASS
#define MIMI_SECRET_WIFI_PASS       ""
#endif
#ifndef MIMI_SECRET_TG_TOKEN
#define MIMI_SECRET_TG_TOKEN        ""
#endif
#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY         ""
#endif
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL           ""
#endif
#ifndef MIMI_SECRET_API_ENDPOINT
#define MIMI_SECRET_API_ENDPOINT    ""
#endif
#ifndef MIMI_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_HOST      ""
#endif
#ifndef MIMI_SECRET_PROXY_PORT
#define MIMI_SECRET_PROXY_PORT      ""
#endif
#ifndef MIMI_SECRET_SEARCH_KEY
#define MIMI_SECRET_SEARCH_KEY      ""
#endif

/* WiFi */
#define MIMI_WIFI_MAX_RETRY          10
#define MIMI_WIFI_RETRY_BASE_MS      1000
#define MIMI_WIFI_RETRY_MAX_MS       30000

/* Telegram Bot */
#define MIMI_TG_POLL_TIMEOUT_S       30
#define MIMI_TG_MAX_MSG_LEN          4096
#define MIMI_TG_POLL_STACK           (12 * 1024)
#define MIMI_TG_POLL_PRIO            5
#define MIMI_TG_POLL_CORE            0

/* Agent Loop */
#define MIMI_AGENT_STACK             (12 * 1024)
#define MIMI_AGENT_PRIO              6
#define MIMI_AGENT_CORE              1
#define MIMI_AGENT_MAX_HISTORY       20
#define MIMI_AGENT_MAX_TOOL_ITER     10
#define MIMI_MAX_TOOL_CALLS          4

/* Timezone (POSIX TZ format) */
#define MIMI_TIMEZONE                "PST8PDT,M3.2.0,M11.1.0"

/* LLM */
#define MIMI_LLM_DEFAULT_MODEL       "claude-opus-4-5"
#define MIMI_LLM_MAX_TOKENS          4096
#define MIMI_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define MIMI_LLM_API_VERSION         "2023-06-01"
#define MIMI_LLM_STREAM_BUF_SIZE     (32 * 1024)

/* Message Bus */
#define MIMI_BUS_QUEUE_LEN           8
#define MIMI_OUTBOUND_STACK          (8 * 1024)
#define MIMI_OUTBOUND_PRIO           5
#define MIMI_OUTBOUND_CORE           0

/* Memory / SPIFFS */
#define MIMI_SPIFFS_BASE             "/spiffs"
#define MIMI_SPIFFS_CONFIG_DIR       "/spiffs/config"
#define MIMI_SPIFFS_MEMORY_DIR       "/spiffs/memory"
#define MIMI_SPIFFS_SESSION_DIR      "/spiffs/sessions"
#define MIMI_MEMORY_FILE             "/spiffs/memory/MEMORY.md"
#define MIMI_SOUL_FILE               "/spiffs/config/SOUL.md"
#define MIMI_USER_FILE               "/spiffs/config/USER.md"
#define MIMI_CONTEXT_BUF_SIZE        (16 * 1024)
#define MIMI_SESSION_MAX_MSGS        20

/* WebSocket Gateway */
#define MIMI_WS_PORT                 18789
#define MIMI_WS_MAX_CLIENTS          4

/* Serial CLI */
#define MIMI_CLI_STACK               (4 * 1024)
#define MIMI_CLI_PRIO                3
#define MIMI_CLI_CORE                0

/* NVS Namespaces */
#define MIMI_NVS_WIFI                "wifi_config"
#define MIMI_NVS_TG                  "tg_config"
#define MIMI_NVS_LLM                 "llm_config"
#define MIMI_NVS_PROXY               "proxy_config"
#define MIMI_NVS_SEARCH              "search_config"

/* NVS Keys */
#define MIMI_NVS_KEY_SSID            "ssid"
#define MIMI_NVS_KEY_PASS            "password"
#define MIMI_NVS_KEY_TG_TOKEN        "bot_token"
#define MIMI_NVS_KEY_API_KEY         "api_key"
#define MIMI_NVS_KEY_MODEL           "model"
#define MIMI_NVS_KEY_PROXY_HOST      "host"
#define MIMI_NVS_KEY_PROXY_PORT      "port"

/* Display Configuration */
#ifndef MIMI_DISPLAY_ENABLED
#define MIMI_DISPLAY_ENABLED         0  /* Set to 1 to enable display */
#endif

#ifndef MIMI_DISPLAY_TYPE
#define MIMI_DISPLAY_TYPE            1  /* 0=None, 1=SSD1306, 2=ST7789, 3=ILI9341 */
#endif

#ifndef MIMI_DISPLAY_WIDTH
#define MIMI_DISPLAY_WIDTH           128
#endif

#ifndef MIMI_DISPLAY_HEIGHT
#define MIMI_DISPLAY_HEIGHT          64
#endif

/* I2C pins for OLED (SSD1306) */
#ifndef MIMI_DISPLAY_I2C_PORT
#define MIMI_DISPLAY_I2C_PORT        0
#endif

#ifndef MIMI_DISPLAY_SDA_PIN
#define MIMI_DISPLAY_SDA_PIN         21
#endif

#ifndef MIMI_DISPLAY_SCL_PIN
#define MIMI_DISPLAY_SCL_PIN         22
#endif

#ifndef MIMI_DISPLAY_I2C_ADDR
#define MIMI_DISPLAY_I2C_ADDR        0x3C
#endif

/* SPI pins for LCD (ST7789/ILI9341) */
#ifndef MIMI_DISPLAY_SPI_HOST
#define MIMI_DISPLAY_SPI_HOST        1
#endif

#ifndef MIMI_DISPLAY_MOSI_PIN
#define MIMI_DISPLAY_MOSI_PIN        23
#endif

#ifndef MIMI_DISPLAY_SCLK_PIN
#define MIMI_DISPLAY_SCLK_PIN        18
#endif

#ifndef MIMI_DISPLAY_CS_PIN
#define MIMI_DISPLAY_CS_PIN          5
#endif

#ifndef MIMI_DISPLAY_DC_PIN
#define MIMI_DISPLAY_DC_PIN          16
#endif

#ifndef MIMI_DISPLAY_RST_PIN
#define MIMI_DISPLAY_RST_PIN         17
#endif

#ifndef MIMI_DISPLAY_BL_PIN
#define MIMI_DISPLAY_BL_PIN          4
#endif

