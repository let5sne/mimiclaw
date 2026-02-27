# MimiClaw vs Nanobot — Feature Gap Tracker

> Comparing against `nanobot/` reference implementation. Tracks features MimiClaw has not yet aligned with.
> Priority: P0 = Core missing, P1 = Important enhancement, P2 = Nice to have

---

## P0 — Engineering Hardening Plan (Approved)

### [x] ~~Inbound Security (Telegram allowlist + WS auth token)~~
- Add Telegram sender allowlist (`allow_from`) check before enqueueing inbound messages.
- Add WebSocket auth token verification during handshake/message accept.
- Add CLI + NVS config commands for allowlist and token management.

### [x] ~~File Tool Safety Boundaries~~
- Keep read access for `/spiffs/`.
- Restrict write/edit by default to `/spiffs/memory/` (config and session paths opt-in only).
- Return explicit policy errors for denied paths.

### [x] ~~Reliability and Backpressure~~
- Add retry/backoff for LLM requests on transient errors (network/429/5xx).
- Add outbound retry policy per channel for final replies.
- Differentiate message priorities: status updates can drop first, final replies should be preserved.

### [x] ~~Agent Budget Guards~~
- Enforce guardrails for tool iterations, context bytes, tool result bytes, and turn timeout.
- Add explicit degradation path when budget is exceeded (safe fallback response).

### [x] ~~Memory Governance~~
- Unify daily memory path convention in prompt and storage implementation.
- Add dedicated memory tools (`memory_write_long_term`, `memory_append_today`) to reduce unsafe free-form file edits.

### [x] ~~Observability~~
- Add `run_id` and stage-level latency logging (ingress/context/llm/tools/outbound).
- Add CLI diagnostics command (e.g. `agent_stats`) for success rate, latency, and failure counters.

---

## P0 — Core Agent Capabilities

### [x] ~~Tool Use Loop (multi-turn agent iteration)~~
- Implemented: `agent_loop.c` ReAct loop with `llm_chat_tools()`, max 10 iterations, non-streaming JSON parsing

### [x] ~~Memory Write via Tool Use (agent-driven memory persistence)~~
- **openclaw**: Agent uses standard `write`/`edit` tools to write `MEMORY.md` and `memory/YYYY-MM-DD.md`; system prompt instructs agent to persist important information; pre-compaction memory flush triggers a silent agent turn to save durable memories before context window limit
- **MimiClaw**: Exposed `memory_write_long_term` and `memory_append_today` in tool registry; system prompt now guides agent to prefer dedicated memory tools.
- **Scope (remaining optional)**: pre-compaction flush when session history nears `MIMI_SESSION_MAX_MSGS`
- **Depends on**: Tool Use Loop

### [x] ~~Tool Registry + web_search Tool~~
- Implemented: `tools/tool_registry.c` — tool registration, JSON schema builder, dispatch by name
- Implemented: `tools/tool_web_search.c` — Brave Search API via HTTPS (direct + proxy support)

### [x] ~~Core File Tools (SPIFFS read/write/edit/list)~~
- Implemented: `tool_registry.c` + `tool_files.c` now provide `read_file`, `write_file`, `edit_file`, `list_dir`.
- Remaining optional enhancement: dedicated `message` helper tool for cross-channel routing.

### [ ] Subagent / Spawn Background Tasks
- **nanobot**: `subagent.py` — SubagentManager spawns independent agent instances with isolated tool sets and system prompts, announces results back to main agent via system channel
- **MimiClaw**: Not implemented
- **Recommendation**: ESP32 memory is limited; simplify to a single background FreeRTOS task for long-running work, inject result into inbound queue on completion

---

## P1 — Important Features

### [x] ~~Telegram User Allowlist (allow_from)~~
- Completed in P0 inbound security: allowlist + WS token + CLI/NVS runtime config.

### [ ] Telegram Markdown to HTML Conversion
- **nanobot**: `channels/telegram.py` L16-76 — `_markdown_to_telegram_html()` full converter: code blocks, inline code, bold, italic, links, strikethrough, lists
- **MimiClaw**: Uses `parse_mode: Markdown` directly; special characters can cause send failures (has fallback to plain text)
- **Recommendation**: Implement simplified Markdown-to-HTML converter, or switch to `parse_mode: HTML`

### [x] ~~Telegram /start Command~~
- **nanobot**: `telegram.py` L183-192 — handles `/start` command, replies with welcome message
- **MimiClaw**: Implemented in `telegram_bot.c`; `/start` now returns a local welcome/help message directly

### [ ] Telegram Media Handling (photos/voice/files) (partially done)
- **nanobot**: `telegram.py` L194-289 — handles photo, voice, audio, document; downloads files; transcribes voice
- **MimiClaw**: `voice/audio` supports `getFile` + proxy-aware file download + upload to local STT endpoint (`/stt_upload`) for real transcription; `photo` can call local gateway `/vision_upload` for cloud vision analysis; `document` now calls `/doc_upload` for structured parsing (text/pdf/docx/pptx/xls/xlsx/image-doc), and for low-text PDF/PPTX will auto-apply page/image OCR chunks via vision before media summary fallback.
- **Diagnostics**: STT path now emits stage-level failure logs (`get_file` / `download` / `stt_upload`) for faster troubleshooting.
- **Regression**: added `tools/doc_regression.py` with manifest-driven smoke checks against `/doc_upload` (format/text_len/keywords/parser/latency), plus `tools/run_doc_regression.sh` for one-command basic/office runs.
- **Remaining gap**: 暂不支持复杂版面文档的高保真重建（图表/形状/跨页表格）与坐标级结构化 OCR；vision/document quality depends on gateway model and host deps.
- **Next recommendation**: wire regression script into CI and add real sample set for pdf/pptx/xls/xlsx scanned docs.

### [ ] Skills System (pluggable capabilities)
- **nanobot**: `agent/skills.py` — loads skills from SKILL.md files, supports always-loaded and on-demand, frontmatter metadata, requirements checking
- **MimiClaw**: Partially done. Added lightweight route hints in agent loop based on `media_type/channel` (`voice/photo/document/system`) and injected into user turn as `[route_hint]`; route rules are configurable via `/spiffs/config/TOOLS.md` (`route.*`, with default fallback and reload cache). Added bootstrap `SKILLS.md` and minimal rule parser (`when.media_type` / `when.channel`) to inject matched `[skill_hints]` at runtime, with priority排序、重复去重和输出上限控制。
- **Remaining gap**: no standalone skill loader/executor or SKILL.md runtime composition yet.
- **Recommendation**: Next step is a minimal skill registry on SPIFFS with explicit trigger rules.

### [x] ~~Full Bootstrap File Alignment~~
- **nanobot**: Loads `AGENTS.md`, `SOUL.md`, `USER.md`, `TOOLS.md`, `IDENTITY.md` (5 files)
- **MimiClaw**: Loads `SOUL.md`, `USER.md`, `AGENTS.md`, `TOOLS.md`, `SKILLS.md`, `IDENTITY.md`
- **Status**: Bootstrap file set aligned and extended for behavior/tool/skill/identity guidance

### [ ] Longer Memory Lookback
- **nanobot**: `memory.py` L56-80 — `get_recent_memories(days=7)` defaults to 7 days
- **MimiClaw**: `context_builder.c` only reads last 3 days
- **Recommendation**: Make configurable, but mind token budget

### [x] ~~System Prompt Tool Guidance~~
- Implemented: `context_builder.c` includes tool usage guidance in system prompt

### [ ] Message Metadata (media, reply_to, metadata) (partially done)
- **nanobot**: `bus/events.py` — InboundMessage has media, metadata fields; OutboundMessage has reply_to
- **MimiClaw**: `mimi_msg_t` now includes `media_type` + `file_id` + `file_path` + `meta_json`; Telegram/Voice/System ingress already populates base metadata, and agent injects `[message_meta]` into LLM user turn.
- **Remaining gap**: `reply_to` and richer structured metadata schema are not yet standardized across all channels/tools.

### [ ] Outbound Subscription Pattern
- **nanobot**: `bus/queue.py` L41-49 — supports `subscribe_outbound(channel, callback)` subscription model
- **MimiClaw**: Hardcoded if-else dispatch
- **Recommendation**: Current approach is simple and reliable; not worth changing with few channels

---

## P2 — Advanced Features

### [x] ~~Cron Scheduled Task Service (simplified)~~
- **nanobot**: `cron/service.py` — full cron scheduler supporting at/every/cron expressions, persistent storage, timed agent triggers
- **MimiClaw**: Implemented `cron/cron_service.c` simplified scheduler (every N minutes only)
- **Scope**: Supports NVS/file config (`/spiffs/config/CRON.md`), enqueues internal `system:cron` turn, CLI controls (`cron_set`/`cron_clear`/`cron_status`/`cron_now`)

### [x] ~~Heartbeat Service~~
- **nanobot**: `heartbeat/service.py` — reads HEARTBEAT.md every 30 minutes, triggers agent if tasks are found
- **MimiClaw**: Implemented `heartbeat/heartbeat_service.c` with FreeRTOS periodic timer/task
- **Scope**: Reads `/spiffs/config/HEARTBEAT.md`, ignores comment lines, enqueues internal `system:heartbeat` turn, supports CLI `heartbeat_status` and `heartbeat_now`

### [ ] Multi-LLM Provider Support
- **nanobot**: `providers/litellm_provider.py` — supports OpenRouter, Anthropic, OpenAI, Gemini, DeepSeek, Groq, Zhipu, vLLM via LiteLLM
- **MimiClaw**: Hardcoded to Anthropic Messages API
- **Recommendation**: Abstract LLM interface, support OpenAI-compatible API (most providers are compatible)

### [x] ~~Voice Transcription~~
- **nanobot**: `providers/transcription.py` — Groq Whisper API
- **MimiClaw**: Telegram `voice/audio` path implemented with local voice gateway (`/stt_upload`), including Telegram media download and transcription fallback behavior.
- **Note**: This does not yet include image/document semantic understanding.

### [x] ~~Build-time Config File + Runtime NVS Override~~
- Implemented: `mimi_secrets.h` as build-time defaults, NVS as runtime override via CLI
- Two-layer config: build-time secrets → NVS fallback, CLI commands to set/show/reset

### [ ] WebSocket Gateway Protocol Enhancement
- **nanobot**: Gateway port 18790 + richer protocol
- **MimiClaw**: Basic JSON protocol, lacks streaming token push
- **Recommendation**: Add `{"type":"token","content":"..."}` streaming push

### [ ] Multi-Channel Manager
- **nanobot**: `channels/manager.py` — unified lifecycle management for multiple channels
- **MimiClaw**: Hardcoded in app_main()
- **Recommendation**: Not worth abstracting with few channels

### [ ] WhatsApp / Feishu Channels
- **nanobot**: `channels/whatsapp.py`, `channels/feishu.py`
- **MimiClaw**: Only Telegram + WebSocket
- **Recommendation**: Low priority, Telegram is sufficient

### [x] ~~Telegram Proxy Support (HTTP CONNECT)~~
- Implemented: HTTP CONNECT tunnel via `proxy/http_proxy.c`, configurable via `mimi_secrets.h` (`MIMI_SECRET_PROXY_HOST`/`MIMI_SECRET_PROXY_PORT`)

### [ ] Session Metadata Persistence
- **nanobot**: `session/manager.py` L136-153 — session file includes metadata line (created_at, updated_at)
- **MimiClaw**: JSONL only stores role/content/ts, no metadata header
- **Recommendation**: Low priority

---

## Completed Alignment

- [x] Telegram Bot long polling (getUpdates)
- [x] Message Bus (inbound/outbound queues)
- [x] Agent Loop with ReAct tool use (multi-turn, max 10 iterations)
- [x] Claude API (Anthropic Messages API, non-streaming, tool_use protocol)
- [x] Tool Registry + web_search tool (Brave Search API)
- [x] Context Builder (system prompt + bootstrap files + memory + tool guidance)
- [x] Memory Store (MEMORY.md + daily notes)
- [x] Session Manager (JSONL per chat_id, ring buffer history)
- [x] WebSocket Gateway (port 18789, JSON protocol)
- [x] Serial CLI (esp_console, debug/maintenance commands)
- [x] HTTP CONNECT Proxy (Telegram + Claude API + Brave Search via proxy tunnel)
- [x] OTA Update
- [x] WiFi Manager (build-time credentials, exponential backoff)
- [x] SPIFFS storage
- [x] Build-time config (`mimi_secrets.h`) + runtime NVS override via CLI
- [x] Inbound security (Telegram allowlist + WS token auth)
- [x] File tool write/edit safety boundaries (default `/spiffs/memory/`)
- [x] LLM/outbound reliability retry + outbound status-first drop policy
- [x] Agent budget guards (turn timeout/context/tool-result caps + fallback)
- [x] Memory governance (daily path unified + dedicated memory tools exposed)
- [x] Observability (`run_id` + stage latency logs + `agent_stats` CLI)
- [x] Bootstrap files `AGENTS.md` + `TOOLS.md` + `SKILLS.md` + `IDENTITY.md` loaded into system prompt
- [x] Heartbeat service (periodic HEARTBEAT.md trigger + CLI diagnostics)
- [x] Cron service (simplified every-N-min schedule + CLI diagnostics)
- [x] Telegram `/start` command + media summary fallback for non-text messages
- [x] Telegram voice transcription path (`getFile` + media download + local STT upload)
- [x] Telegram voice STT stage diagnostics (`get_file` / `download` / `stt_upload`)
- [x] Telegram photo/document download path (proxy-aware) + enriched summary
- [x] Telegram photo vision path (`vision_upload`, structured fields + `file_id` cache + auto fallback)

---

## Suggested Implementation Order

```
1. [done] Tool Use Loop + Tool Registry + web_search
2. [done] Inbound Security (allow_from + WS auth token)
3. [done] File Tool Safety Boundaries
4. [done] Reliability + Backpressure + Retry
5. [done] Agent Budget Guards
6. [done] Memory Governance (path consistency + dedicated memory tools)
7. [done] Observability (`run_id` + stage latency logs + `agent_stats`)
8. [done] Bootstrap File Completion (AGENTS.md, TOOLS.md, SKILLS.md, IDENTITY.md)
9. Telegram Markdown -> HTML
10. Media Handling
```
