# MimiClaw: Pocket AI Assistant on a $5 Chip

<p align="center">
  <img src="assets/banner.png" alt="MimiClaw" width="500" />
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
  <a href="https://deepwiki.com/memovai/mimiclaw"><img src="https://img.shields.io/badge/DeepWiki-mimiclaw-blue.svg" alt="DeepWiki"></a>
  <a href="https://discord.gg/r8ZxSvB8Yr"><img src="https://img.shields.io/badge/Discord-mimiclaw-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="https://x.com/ssslvky"><img src="https://img.shields.io/badge/X-@ssslvky-black?logo=x" alt="X"></a>
</p>

<p align="center">
  <strong><a href="README.md">English</a> | <a href="README_CN.md">中文</a> | <a href="README_JA.md">日本語</a></strong>
</p>

**The world's first AI assistant(OpenClaw) on a $5 chip. No Linux. No Node.js. Just pure C**

MimiClaw turns a tiny ESP32-S3 board into a personal AI assistant. Plug it into USB power, connect to WiFi, and talk to it through Telegram or a Feishu bot — it handles any task you throw at it and evolves over time with local memory — all on a chip the size of a thumb.

## Meet MimiClaw

- **Tiny** — No Linux, no Node.js, no bloat — just pure C
- **Handy** — Message it from Telegram or Feishu, it handles the rest
- **Loyal** — Learns from memory, remembers across reboots
- **Energetic** — USB power, 0.5 W, runs 24/7
- **Lovable** — One ESP32-S3 board, $5, nothing else

## How It Works

![](assets/mimiclaw.png)

You send a message on Telegram, Feishu, or a LAN WebSocket client. The ESP32-S3 picks it up over WiFi, feeds it into an agent loop — the LLM thinks, calls tools, reads memory — and sends the reply back. Supports both **Anthropic (Claude)** and **OpenAI (GPT)** as providers, switchable at runtime. Everything runs on a single $5 chip with all your data stored locally on flash.

## Quick Start

The default quick start uses **ESP32-S3-DevKitC-1** with **no external peripherals** attached. Plug into the correct **USB** port, configure WiFi/Bot/API key, flash the firmware, and verify from Telegram or Feishu.

### Fast Path (5 Steps)

1. Prepare an **ESP32-S3-DevKitC-1** and plug into the port labeled **USB** (not **COM**).
2. Install ESP-IDF and clone this repository.
3. Copy `main/mimi_secrets.h.example` to `main/mimi_secrets.h` and fill in WiFi, bot, and API key.
4. Build and flash the firmware.
5. Open Telegram or Feishu and send `/start` or `hello` to test the full agent path.

### What You Need

- An **ESP32-S3-DevKitC-1** (default quick-start board; use a variant with 16 MB flash and 8 MB PSRAM)
- A **USB Type-C cable**
- A **Telegram bot token**, or a **Feishu self-built app** with bot capability and event subscription
- An **Anthropic API key** — from [console.anthropic.com](https://console.anthropic.com), or an **OpenAI API key** — from [platform.openai.com](https://platform.openai.com)
- No microphone, speaker, or display is required for the default quick start

### Install

```bash
# You need ESP-IDF v5.5+ installed first:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3
```

<details>
<summary>Ubuntu Install</summary>

Recommended baseline:

- Ubuntu 22.04/24.04
- Python >= 3.10
- CMake >= 3.16
- Ninja >= 1.10
- Git >= 2.34
- flex >= 2.6
- bison >= 3.8
- gperf >= 3.1
- dfu-util >= 0.11
- `libusb-1.0-0`, `libffi-dev`, `libssl-dev`

Install and build on Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

./scripts/setup_idf_ubuntu.sh
./scripts/build_ubuntu.sh
```

</details>

<details>
<summary>macOS Install</summary>

Recommended baseline:

- macOS 12/13/14
- Xcode Command Line Tools
- Homebrew
- Python >= 3.10
- CMake >= 3.16
- Ninja >= 1.10
- Git >= 2.34
- flex >= 2.6
- bison >= 3.8
- gperf >= 3.1
- dfu-util >= 0.11
- `libusb`, `libffi`, `openssl`

Install and build on macOS:

```bash
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

./scripts/setup_idf_macos.sh
./scripts/build_macos.sh
```

</details>

### Configure

MimiClaw uses a **two-layer config** system: build-time defaults in `mimi_secrets.h`, with runtime overrides via the serial CLI. CLI values are stored in NVS flash and take priority over build-time values.

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

Edit `main/mimi_secrets.h`:

```c
#define MIMI_SECRET_WIFI_SSID       "YourWiFiName"
#define MIMI_SECRET_WIFI_PASS       "YourWiFiPassword"
#define MIMI_SECRET_TG_TOKEN        ""              // leave empty if you only use Feishu
#define MIMI_SECRET_FEISHU_APP_ID   ""
#define MIMI_SECRET_FEISHU_APP_SECRET ""
#define MIMI_SECRET_FEISHU_VERIFY_TOKEN ""
#define MIMI_SECRET_FEISHU_ENCRYPT_KEY ""
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"     // "anthropic" or "openai"
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

Recommended first check:

- Telegram: send `/start` to confirm connectivity
- Feishu: send `hello` to confirm callback ingress and outbound messaging
- Send `hello` to test the full agent path

> **Important: Plug into the correct USB port!** Most ESP32-S3 boards have two USB-C ports. You must use the one labeled **USB** (native USB Serial/JTAG), **not** the one labeled **COM** (external UART bridge). Plugging into the wrong port will cause flash/monitor failures.
>
> **Quick-start reference board**: the default path in this README assumes **ESP32-S3-DevKitC-1**. For the first boot, keep the board in its minimum form: **USB cable only, no microphone, no speaker, no display**.
>
> <details>
> <summary>Show reference photo</summary>
>
> <img src="assets/esp32s3-usb-port.jpg" alt="Plug into the USB port, not COM" width="480" />
>
> </details>

### Feishu Bot Setup

This branch defaults to a **no external voice gateway** build. Text chat works without running `tools/voice_gateway.py`.

1. Fill these build-time secrets in `main/mimi_secrets.h`:

```c
#define MIMI_SECRET_FEISHU_APP_ID        "cli_xxx"
#define MIMI_SECRET_FEISHU_APP_SECRET    "xxx"
#define MIMI_SECRET_FEISHU_VERIFY_TOKEN  "mimiclaw-feishu"
#define MIMI_SECRET_FEISHU_ENCRYPT_KEY   ""   // optional; must match Feishu if encrypted callbacks are enabled
```

2. In the Feishu Open Platform:

- create a **self-built app**
- enable **bot capability**
- subscribe to event `im.message.receive_v1`
- set request URL to `https://<public-address>/feishu/events`
- set `Verify Token` to the same value as `MIMI_SECRET_FEISHU_VERIFY_TOKEN`
- leave `Encrypt Key` empty for plaintext callbacks, or set it to match `MIMI_SECRET_FEISHU_ENCRYPT_KEY`

This firmware now supports **encrypted Feishu event payloads**:

- if `Encrypt Key` is empty, callbacks are handled as plaintext events
- if `Encrypt Key` is configured, the firmware validates `X-Lark-Signature` and decrypts the `encrypt` field
- keeping `Verify Token` enabled is still recommended for an extra source check on URL verification and normal events

3. Make the device reachable from Feishu:

- callback path is fixed at `/feishu/events`
- the shared HTTP service listens on port `18789`
- if your board is not public, add reverse proxy / port forwarding / tunnel to `http://<device-lan-ip>:18789/feishu/events`

4. Smoke test after flashing:

- check serial logs for `Feishu callback registered at /feishu/events`
- if `Encrypt Key` is enabled, confirm both URL verification and event delivery succeed in the Feishu console
- send `hello` to the bot in Feishu
- expect a text reply from MimiClaw

4.1 Local replay without the Feishu console

If you only want to validate the device-side `/feishu/events` path, run this from your dev machine:

```bash
./tools/run_feishu_replay.sh --scenario all --verify-token mimiclaw-feishu
```

Common examples:

```bash
# replay plaintext text / duplicate / image / file / audio / sticker callbacks
./tools/run_feishu_replay.sh --scenario all --verify-token mimiclaw-feishu

# replay encrypted duplicate delivery
./tools/run_feishu_replay.sh \
  --scenario duplicate \
  --verify-token mimiclaw-feishu \
  --encrypt-key your_encrypt_key \
  --encrypted

# print request and response bodies for debugging
./tools/run_feishu_replay.sh --scenario text --show-body
```

Notes:

- the script targets `http://127.0.0.1:18789/feishu/events` by default
- the `duplicate` scenario sends the same `event_id/message_id` twice to verify firmware deduplication
- `image/file/audio/sticker` scenarios do not download real media; they only validate the callback -> summary text -> Agent downgrade path

Current limits:

- text messages go to the Agent as-is
- image / file / audio / sticker / other non-text Feishu messages are downgraded into summary text before entering the Agent
- outbound Feishu delivery uses `chat_id`
- repeated Feishu deliveries are lightly deduplicated by `event_id/message_id` before entering the Agent

### Optional: Voice/Vision Gateway

This is an optional extension. The default no-gateway build does not depend on it.

Not required for the default quick start. If you only want to get the board online, skip this section for now.

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

To actually enable these Telegram media features in firmware, also set this in `main/mimi_config.h` and rebuild:

```c
#define MIMI_TELEGRAM_GATEWAY_MEDIA_ENABLED 1
```

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
mimi> set_feishu_app cli_xxx secret_xxx   # set Feishu app_id / app_secret
mimi> set_feishu_verify_token token_xxx   # set Feishu Verify Token
mimi> set_feishu_encrypt_key key_xxx      # set Feishu Encrypt Key
mimi> set_api_key sk-ant-api03-... # change API key (Anthropic or OpenAI)
mimi> set_model_provider openai    # switch provider (anthropic|openai)
mimi> set_model gpt-4o             # change LLM model
mimi> set_proxy 127.0.0.1 7897  # set HTTP proxy
mimi> clear_proxy                  # remove proxy
mimi> set_search_key BSA...        # set Brave Search API key
mimi> config_show                  # show all config (masked)
mimi> config_reset                 # clear NVS, revert to build-time defaults
```

Note:

- Feishu `app_id/app_secret/verify_token/encrypt_key` can now be updated via CLI
- NVS-stored Feishu config overrides build-time defaults

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
mimi> heartbeat_trigger           # manually trigger a heartbeat check
mimi> cron_start                  # start cron scheduler now
mimi> restart                     # reboot
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
| `HEARTBEAT.md` | Task list the bot checks periodically and acts on autonomously |
| `cron.json` | Scheduled jobs — recurring or one-shot tasks created by the AI |
| `2026-02-05.md` | Daily notes — what happened today |
| `tg_12345.jsonl` | Chat history — your conversation with the bot |

## Tools

MimiClaw supports tool calling for both Anthropic and OpenAI — the LLM can call tools during a conversation and loop until the task is done (ReAct pattern).

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
| `cron_add` | Schedule a recurring or one-shot task (the LLM creates cron jobs on its own) |
| `cron_list` | List all scheduled cron jobs |
| `cron_remove` | Remove a cron job by ID |

To enable web search, set a [Brave Search API key](https://brave.com/search/api/) via `MIMI_SECRET_SEARCH_KEY` in `mimi_secrets.h`.

## Cron Tasks

MimiClaw has a built-in cron scheduler that lets the AI schedule its own tasks. The LLM can create recurring jobs ("every N seconds") or one-shot jobs ("at unix timestamp") via the `cron_add` tool. When a job fires, its message is injected into the agent loop — so the AI wakes up, processes the task, and responds.

Jobs are persisted to SPIFFS (`cron.json`) and survive reboots. Example use cases: daily summaries, periodic reminders, scheduled check-ins.

## Heartbeat

The heartbeat service periodically reads `HEARTBEAT.md` from SPIFFS and checks for actionable tasks. If uncompleted items are found (anything that isn't an empty line, a header, or a checked `- [x]` box), it sends a prompt to the agent loop so the AI can act on them autonomously.

This turns MimiClaw into a proactive assistant — write tasks to `HEARTBEAT.md` and the bot will pick them up on the next heartbeat cycle (default: every 30 minutes).

## Also Included

- **WebSocket gateway** on port 18789 — connect from your LAN with any WebSocket client
- **Feishu Bot** — text callback endpoint at `/feishu/events`, sharing the same HTTP service
- **OTA updates** — flash new firmware over WiFi, no USB needed
- **Dual-core** — network I/O and AI processing run on separate CPU cores
- **HTTP proxy** — CONNECT tunnel support for restricted networks
- **Tool use** — ReAct agent loop with Anthropic tool use protocol
- **Telegram media handling** — in the default no-gateway mode, voice/photos/documents fall back to media summaries; if you enable `MIMI_TELEGRAM_GATEWAY_MEDIA_ENABLED=1` and run `voice_gateway.py`, Telegram can use real STT / vision / doc parsing again

## P0 Hardening Roadmap (In Progress)

- [x] Inbound security: Telegram allowlist (`allow_from`) + WebSocket auth token
- [x] File tool safety boundaries: write default-limited to `/spiffs/memory/`
- [x] Reliability: retries/backoff for LLM and outbound delivery; drop status first, preserve final replies
- [x] Budget guards: tool iterations, context size, tool output size, end-to-end timeout caps
- [x] Memory governance: unify daily memory path and add dedicated memory write tools
- [x] Observability: `run_id`, stage-level latency logs, `agent_stats` diagnostics command

Detailed tracking: **[docs/TODO.md](docs/TODO.md)**.
- **Multi-provider** — supports both Anthropic (Claude) and OpenAI (GPT), switchable at runtime
- **Cron scheduler** — the AI can schedule its own recurring and one-shot tasks, persisted across reboots
- **Heartbeat** — periodically checks a task file and prompts the AI to act autonomously
- **Tool use** — ReAct agent loop with tool calling for both providers

## For Developers

Technical details live in the `docs/` folder:

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — system design, module map, task layout, memory budget, protocols, flash partitions
- **[docs/TODO.md](docs/TODO.md)** — feature gap tracker and roadmap

## Contributing

Please read **[docs/CONTRIBUTE.md](docs/CONTRIBUTE.md)** before opening issues or pull requests.

## License

MIT

## Acknowledgments

Inspired by [OpenClaw](https://github.com/openclaw/openclaw) and [Nanobot](https://github.com/HKUDS/nanobot). MimiClaw reimplements the core AI agent architecture for embedded hardware — no Linux, no server, just a $5 chip.

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=memovai/mimiclaw&type=Date)](https://star-history.com/#memovai/mimiclaw&Date)
