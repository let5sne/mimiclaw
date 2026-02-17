# MimiClaw: Pocket AI Assistant on a $5 Chip

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![DeepWiki](https://img.shields.io/badge/DeepWiki-mimiclaw-blue.svg)](https://deepwiki.com/memovai/mimiclaw)
[![Discord](https://img.shields.io/badge/Discord-mimiclaw-5865F2?logo=discord&logoColor=white)](https://discord.gg/r8ZxSvB8Yr)
[![X](https://img.shields.io/badge/X-@ssslvky-black?logo=x)](https://x.com/ssslvky)

**[English](README.md) | [中文](README_CN.md)**

<p align="center">
  <img src="assets/banner.png" alt="MimiClaw" width="480" />
</p>

**The world's first AI assistant(OpenClaw) on a $5 chip. No Linux. No Node.js. Just pure C**

MimiClaw turns a tiny ESP32-S3 board into a personal AI assistant. Plug it into USB power, connect to WiFi, and talk to it through Telegram — it handles any task you throw at it and evolves over time with local memory — all on a chip the size of a thumb.

## Meet MimiClaw

- **Tiny** — No Linux, no Node.js, no bloat — just pure C
- **Handy** — Message it from Telegram, it handles the rest
- **Loyal** — Learns from memory, remembers across reboots
- **Energetic** — USB power, 0.5 W, runs 24/7
- **Lovable** — One ESP32-S3 board, $5, nothing else

## How It Works

![](assets/mimiclaw.png)

You send a message on Telegram. The ESP32-S3 picks it up over WiFi, feeds it into an agent loop — Claude thinks, calls tools, reads memory — and sends the reply back. Everything runs on a single $5 chip with all your data stored locally on flash.

## Quick Start

### What You Need

- An **ESP32-S3 dev board** with 16 MB flash and 8 MB PSRAM (e.g. Xiaozhi AI board, ~$10)
- A **USB Type-C cable**
- A **Telegram bot token** — talk to [@BotFather](https://t.me/BotFather) on Telegram to create one
- An **Anthropic API key** — from [console.anthropic.com](https://console.anthropic.com)

### Install

```bash
# You need ESP-IDF v5.5+ installed first:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3
```

### Configure

MimiClaw uses a **two-layer config** system: build-time defaults in `mimi_secrets.h`, with runtime overrides via the serial CLI. CLI values are stored in NVS flash and take priority over build-time values.

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

Edit `main/mimi_secrets.h`:

```c
#define MIMI_SECRET_WIFI_SSID       "YourWiFiName"
#define MIMI_SECRET_WIFI_PASS       "YourWiFiPassword"
#define MIMI_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_SEARCH_KEY      ""              // optional: Brave Search API key
#define MIMI_SECRET_PROXY_HOST      ""              // optional: e.g. "10.0.0.1"
#define MIMI_SECRET_PROXY_PORT      ""              // optional: e.g. "7897"
```

Then build and flash:

```bash
# Clean build (required after any mimi_secrets.h change)
idf.py fullclean && idf.py build

# Find your serial port
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# Flash and monitor (replace PORT with your port)
# USB adapter: likely /dev/cu.usbmodem11401 (macOS) or /dev/ttyACM0 (Linux)
idf.py -p PORT flash monitor
```

### Voice/Vision Gateway

Start the local gateway (STT + image analysis endpoint):

```bash
python3 tools/voice_gateway.py \
  --host 0.0.0.0 --port 8090 --model small --device cpu \
  --vision-enabled
```

- STT endpoint: `http://<your-host-ip>:8091/stt_upload`
- Vision endpoint: `http://<your-host-ip>:8091/vision_upload`
- Document endpoint: `http://<your-host-ip>:8091/doc_upload`
- By default, gateway tries loading API defaults from `main/mimi_secrets.h`; you can override via `--vision-endpoint/--vision-api-key/--vision-model`

### Document Regression Smoke Test

After gateway is running, execute:

```bash
./tools/run_doc_regression.sh --basic
./tools/run_doc_regression.sh --office
```

Or run with custom arguments:

```bash
python3 tools/doc_regression.py \
  --manifest tools/doc_regression_manifest.example.json \
  --base-url http://127.0.0.1:8091
```

The script calls `/doc_upload` and validates format, extracted text length, keywords, parser prefix, and latency budget.
`tools/doc_regression_manifest.office.example.json` includes a real `xlsx` sample and an optional `xls` case (`food_legacy.xls`) which is skipped when missing.

### CLI Commands

Connect via serial to configure or debug. **Config commands** let you change settings without recompiling — just plug in a USB cable anywhere.

**Runtime config** (saved to NVS, overrides build-time defaults):

```
mimi> wifi_set MySSID MyPassword   # change WiFi network
mimi> set_tg_token 123456:ABC...   # change Telegram bot token
mimi> set_api_key sk-ant-api03-... # change Anthropic API key
mimi> set_model claude-sonnet-4-5  # change LLM model
mimi> set_proxy 127.0.0.1 7897  # set HTTP proxy
mimi> clear_proxy                  # remove proxy
mimi> set_search_key BSA...        # set Brave Search API key
mimi> config_show                  # show all config (masked)
mimi> config_reset                 # clear NVS, revert to build-time defaults
```

**Debug & maintenance:**

```
mimi> wifi_status              # am I connected?
mimi> memory_read              # see what the bot remembers
mimi> memory_write "content"   # write to MEMORY.md
mimi> heap_info                # how much RAM is free?
mimi> agent_stats              # agent success rate / latency / failures
mimi> heartbeat_status         # heartbeat counters / last run
mimi> heartbeat_now            # trigger heartbeat immediately
mimi> cron_status              # cron schedule + counters
mimi> cron_set 30 "task..."    # run every 30 min
mimi> cron_now                 # trigger cron immediately
mimi> cron_clear               # clear cron schedule
mimi> session_list             # list all chat sessions
mimi> session_clear 12345      # wipe a conversation
mimi> restart                  # reboot
```

## Memory

MimiClaw stores everything as plain text files you can read and edit:

| File | What it is |
|------|------------|
| `SOUL.md` | The bot's personality — edit this to change how it behaves |
| `USER.md` | Info about you — name, preferences, language |
| `AGENTS.md` | Behavior rules and safety constraints |
| `TOOLS.md` | Tool usage policy and priorities |
| `SKILLS.md` | Skill routing hints and trigger-style instruction rules |
| `IDENTITY.md` | Assistant identity and response consistency constraints |
| `HEARTBEAT.md` | Periodic internal task instructions (non-comment lines only) |
| `CRON.md` | Default cron schedule file (`every_minutes` + `task`) |
| `MEMORY.md` | Long-term memory — things the bot should always remember |
| `daily/2026-02-05.md` | Daily notes — what happened today |
| `tg_12345.jsonl` | Chat history — your conversation with the bot |

## Tools

MimiClaw uses Anthropic's tool use protocol — Claude can call tools during a conversation and loop until the task is done (ReAct pattern).

| Tool | Description |
|------|-------------|
| `web_search` | Search the web via Brave Search API for current information |
| `get_current_time` | Fetch current date/time via HTTP and set the system clock |
| `read_file` | Read a SPIFFS file (path must start with `/spiffs/`) |
| `write_file` | Write or overwrite a SPIFFS file (default allowlist: `/spiffs/memory/`) |
| `edit_file` | Find-and-replace in a SPIFFS file (default allowlist: `/spiffs/memory/`) |
| `list_dir` | List SPIFFS files, optionally filtered by prefix |
| `memory_write_long_term` | Overwrite long-term memory (`/spiffs/memory/MEMORY.md`) |
| `memory_append_today` | Append one note to today's daily memory |

To enable web search, set a [Brave Search API key](https://brave.com/search/api/) via `MIMI_SECRET_SEARCH_KEY` in `mimi_secrets.h`.

## Also Included

- **WebSocket gateway** on port 18789 — connect from your LAN with any WebSocket client
- **OTA updates** — flash new firmware over WiFi, no USB needed
- **Dual-core** — network I/O and AI processing run on separate CPU cores
- **HTTP proxy** — CONNECT tunnel support for restricted networks
- **Tool use** — ReAct agent loop with Anthropic tool use protocol
- **Telegram media handling** — `/start` local reply; voice uses real STT via voice gateway HTTP (`/stt_upload`); photos call cloud vision via `vision_upload` with structured output (`caption`/`ocr_text`/`objects`) and `file_id` cache dedupe; documents call `doc_upload` for parsing (`txt/pdf/docx/pptx/xls/xlsx/image-doc`), and when PDF/PPTX text extraction is too short it auto-falls back to page/image OCR via vision before summary fallback

## P0 Hardening Roadmap (In Progress)

- [x] Inbound security: Telegram allowlist (`allow_from`) + WebSocket auth token
- [x] File tool safety boundaries: write default-limited to `/spiffs/memory/`
- [x] Reliability: retries/backoff for LLM and outbound delivery; drop status first, preserve final replies
- [x] Budget guards: tool iterations, context size, tool output size, end-to-end timeout caps
- [x] Memory governance: unify daily memory path and add dedicated memory write tools
- [x] Observability: `run_id`, stage-level latency logs, `agent_stats` diagnostics command

Detailed tracking: **[docs/TODO.md](docs/TODO.md)**.

## For Developers

Technical details live in the `docs/` folder:

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — system design, module map, task layout, memory budget, protocols, flash partitions
- **[docs/TODO.md](docs/TODO.md)** — feature gap tracker and roadmap

## License

MIT

## Acknowledgments

Inspired by [OpenClaw](https://github.com/openclaw/openclaw) and [Nanobot](https://github.com/HKUDS/nanobot). MimiClaw reimplements the core AI agent architecture for embedded hardware — no Linux, no server, just a $5 chip.

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=memovai/mimiclaw&type=Date)](https://star-history.com/#memovai/mimiclaw&Date)
