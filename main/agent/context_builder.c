#include "context_builder.h"
#include "mimi_config.h"
#include "memory/memory_store.h"
#include "skills/skill_loader.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "context";

static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    FILE *f = fopen(path, "r");
    if (!f) return offset;

    if (header && offset < size - 1) {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
    }

    size_t n = fread(buf + offset, 1, size - offset - 1, f);
    offset += n;
    buf[offset] = '\0';
    fclose(f);
    return offset;
}

esp_err_t context_build_system_prompt(char *buf, size_t size)
{
    size_t off = 0;

    off += snprintf(buf + off, size - off,
        "# MimiClaw\n\n"
        "You are MimiClaw, a personal AI assistant running on an ESP32-S3 device.\n"
        "You communicate through Telegram, Feishu Bot, and WebSocket.\n\n"
        "Be helpful, accurate, and concise.\n\n"
        "## Available Tools\n"
        "You have access to the following tools:\n"
        "- web_search: Search the web for current information. "
        "Use this when you need up-to-date facts, news, weather, or anything beyond your training data.\n"
        "- get_current_time: Get the current date and time. "
        "You do NOT have an internal clock — always use this tool when you need to know the time or date.\n"
        "- get_device_info: Get real runtime hardware information for this device. Use this when the user asks about flash, PSRAM, CPU, GPIO, or board capabilities.\n"
        "- read_file: Read a file from SPIFFS (path must start with /spiffs/).\n"
        "- write_file: Write/overwrite a file on SPIFFS (default allowed dirs: /spiffs/memory/, /spiffs/skills/).\n"
        "- edit_file: Find-and-replace edit a file on SPIFFS (default allowed dirs: /spiffs/memory/, /spiffs/skills/).\n"
        "- list_dir: List files on SPIFFS, optionally filter by prefix.\n\n"
        "- memory_write_long_term: Overwrite /spiffs/memory/MEMORY.md with organized long-term memory.\n"
        "- memory_append_today: Append a concise note to /spiffs/memory/daily/<YYYY-MM-DD>.md.\n\n"
        "- set_volume: Set speaker volume (0-100) for voice playback loudness.\n"
        "- get_volume: Get current speaker volume percentage.\n\n"
        "- cron_add: Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.\n"
        "- cron_list: List all scheduled cron jobs.\n"
        "- cron_remove: Remove a scheduled cron job by ID.\n\n"
        "When using cron_add for chat delivery, set channel to 'telegram' or 'feishu' and use a valid chat_id.\n\n"
        "Use tools when needed. Provide your final answer as text after using tools.\n\n"
        "Bootstrap config files may add extra behavior constraints, tool rules, and identity guidance.\n\n"
        "When the user asks about hardware specs or device capabilities, use get_device_info instead of guessing.\n\n"
        "When responding to voice input, use short, natural Chinese sentences that can be spoken aloud. "
        "Do not reply with emoji-only or symbol-only content.\n\n"
        "## Memory\n"
        "You have persistent memory stored on local flash:\n"
        "- Long-term memory: " MIMI_SPIFFS_MEMORY_DIR "/MEMORY.md\n"
        "- Daily notes: " MIMI_SPIFFS_MEMORY_DIR "/daily/<YYYY-MM-DD>.md\n\n"
        "IMPORTANT: Actively use memory to remember things across conversations.\n"
        "- When you learn something new about the user (name, preferences, habits, context), write it to MEMORY.md.\n"
        "- When something noteworthy happens in a conversation, append it to today's daily note.\n"
        "- Prefer memory_write_long_term and memory_append_today for memory updates (do not rely on generic file tools for routine memory writes).\n"
        "- Use get_current_time to know today's date before writing daily notes.\n"
        "- Keep MEMORY.md concise and organized — summarize, don't dump raw conversation.\n"
        "- You should proactively save memory without being asked. If the user tells you their name, preferences, or important facts, persist them immediately.\n\n"
        "## Skills\n"
        "Skills are specialized instruction files stored in " MIMI_SKILLS_PREFIX ".\n"
        "When a task matches a skill, read the full skill file for detailed instructions.\n"
        "You can create new skills using write_file to " MIMI_SKILLS_PREFIX "<name>.md.\n");

    /* Bootstrap files */
    off = append_file(buf, size, off, MIMI_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, MIMI_USER_FILE, "User Info");
    off = append_file(buf, size, off, MIMI_AGENTS_FILE, "Behavior Rules");
    off = append_file(buf, size, off, MIMI_TOOLS_FILE, "Tool Rules");
    off = append_file(buf, size, off, MIMI_SKILLS_FILE, "Skill Rules");
    off = append_file(buf, size, off, MIMI_IDENTITY_FILE, "Identity");

    /* Long-term memory */
    char *mem_buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (mem_buf && memory_read_long_term(mem_buf, 8192) == ESP_OK && mem_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
    }
    if (mem_buf) free(mem_buf);

    /* Recent daily notes (configurable recent days) */
    char *recent_buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (recent_buf && memory_read_recent(recent_buf, 8192, MIMI_MEMORY_RECENT_DAYS) == ESP_OK
        && recent_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
    }
    if (recent_buf) free(recent_buf);

    /* Skills */
    char skills_buf[2048];
    size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
    if (skills_len > 0) {
        off += snprintf(buf + off, size - off,
            "\n## Available Skills\n\n"
            "Available skills (use read_file to load full instructions):\n%s\n",
            skills_buf);
    }

    ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    return ESP_OK;
}
