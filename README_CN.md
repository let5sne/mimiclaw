# MimiClaw: $5 芯片上的口袋 AI 助理

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

**$5 芯片上的 AI 助理（OpenClaw）。没有 Linux，没有 Node.js，纯 C。**

MimiClaw 把一块小小的 ESP32-S3 开发板变成你的私人 AI 助理。插上 USB 供电，连上 WiFi，通过 Telegram 或飞书 Bot 跟它对话 — 它能处理你丢给它的任何任务，还会随时间积累本地记忆不断进化 — 全部跑在一颗拇指大小的芯片上。

## 认识 MimiClaw

- **小巧** — 没有 Linux，没有 Node.js，没有臃肿依赖 — 纯 C
- **好用** — 在 Telegram / 飞书发消息，剩下的它来搞定
- **忠诚** — 从记忆中学习，跨重启也不会忘
- **能干** — USB 供电，0.5W，24/7 运行
- **可爱** — 一块 ESP32-S3 开发板，$5，没了

## 工作原理

![](assets/mimiclaw.png)

你在 Telegram、飞书或局域网 WebSocket 客户端发一条消息，ESP32-S3 通过 WiFi 收到后送进 Agent 循环 — LLM 思考、调用工具、读取记忆 — 再把回复发回来。同时支持 **Anthropic (Claude)** 和 **OpenAI (GPT)** 两种提供商，运行时可切换。一切都跑在一颗 $5 的芯片上，所有数据存在本地 Flash。

## 快速开始

默认快速开始使用 **ESP32-S3-DevKitC-1**，**不需要连接任何外设**。先插对 **USB** 口，再配置 WiFi / Bot / API Key，烧录后通过 Telegram 或飞书验证即可。

### 5 步快跑

1. 准备一块 **ESP32-S3-DevKitC-1**，并插到标有 **USB** 的接口，不要插 **COM**。
2. 安装 ESP-IDF 并克隆仓库。
3. 复制 `main/mimi_secrets.h.example` 为 `main/mimi_secrets.h`，填好 WiFi、Bot 和 API Key。
4. 编译并烧录固件。
5. 打开 Telegram 或飞书，先发 `/start` 或 `hello` 验证完整 Agent 链路。

### 你需要

- 一块 **ESP32-S3-DevKitC-1**（默认快速开始板型，建议选择 16MB Flash + 8MB PSRAM 版本）
- 一根 **USB Type-C 数据线**
- 一个 **Telegram Bot Token**，或一个 **飞书自建应用**（开启机器人能力 + 事件订阅）
- 一个 **Anthropic API Key** — 从 [console.anthropic.com](https://console.anthropic.com) 获取，或一个 **OpenAI API Key** — 从 [platform.openai.com](https://platform.openai.com) 获取
- 默认快速开始不需要麦克风、喇叭或屏幕

### 安装

```bash
# 需要先安装 ESP-IDF v5.5+:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3
```

<details>
<summary>Ubuntu 安装</summary>

建议基线：

- Ubuntu 22.04/24.04
- Python >= 3.10
- CMake >= 3.16
- Ninja >= 1.10
- Git >= 2.34
- flex >= 2.6
- bison >= 3.8
- gperf >= 3.1
- dfu-util >= 0.11
- `libusb-1.0-0`、`libffi-dev`、`libssl-dev`

Ubuntu 安装与构建：

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

./scripts/setup_idf_ubuntu.sh
./scripts/build_ubuntu.sh
```

</details>

<details>
<summary>macOS 安装</summary>

建议基线：

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
- `libusb`、`libffi`、`openssl`

macOS 安装与构建：

```bash
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

./scripts/setup_idf_macos.sh
./scripts/build_macos.sh
```

</details>

### 配置

MimiClaw 使用**两层配置**：`mimi_secrets.h` 提供编译时默认值，串口 CLI 可在运行时覆盖。CLI 设置的值存在 NVS Flash 中，优先级高于编译时值。

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

编辑 `main/mimi_secrets.h`：

```c
#define MIMI_SECRET_WIFI_SSID       "你的WiFi名"
#define MIMI_SECRET_WIFI_PASS       "你的WiFi密码"
#define MIMI_SECRET_TG_TOKEN        ""              // 只用飞书时可留空
#define MIMI_SECRET_FEISHU_APP_ID   ""              // 只用 Telegram 时可留空
#define MIMI_SECRET_FEISHU_APP_SECRET ""
#define MIMI_SECRET_FEISHU_VERIFY_TOKEN ""
#define MIMI_SECRET_FEISHU_ENCRYPT_KEY ""
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"     // "anthropic" 或 "openai"
#define MIMI_SECRET_SEARCH_KEY      ""              // 可选：Brave Search API key
#define MIMI_SECRET_PROXY_HOST      "10.0.0.1"      // 可选：代理地址
#define MIMI_SECRET_PROXY_PORT      "7897"           // 可选：代理端口
```

然后编译烧录：

```bash
# 完整编译（修改 mimi_secrets.h 后必须 fullclean）
get_idf && idf.py fullclean && idf.py build

# 查找串口
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# 烧录并监控（将 PORT 替换为你的串口）
# USB 转接器：大概率是 /dev/cu.usbmodem11401（macOS）或 /dev/ttyACM0（Linux）
idf.py -p PORT flash monitor
```

建议的首次验证动作：

- Telegram：发送 `/start`，确认固件在线
- 飞书：给机器人发送 `hello`，确认回调和出站发送都正常
- 发送 `hello`，验证完整 Agent 链路

> **注意：请插对 USB 口！** 大多数 ESP32-S3 开发板有两个 Type-C 接口，必须插标有 **USB** 的那个口（原生 USB Serial/JTAG），**不要**插标有 **COM** 的口（外部 UART 桥接）。插错口会导致烧录/监控失败。
>
> **快速开始默认参考板型**：本 README 的主路径默认使用 **ESP32-S3-DevKitC-1**。第一次启动请保持最小形态：**只接 USB 线，不接麦克风、不接喇叭、不接屏幕**。
>
> <details>
> <summary>查看参考图片</summary>
>
> <img src="assets/esp32s3-usb-port.jpg" alt="请插 USB 口，不要插 COM 口" width="480" />
>
> </details>

### 代理配置（国内用户）

在国内需要代理才能访问 Telegram 和 Anthropic API。MimiClaw 内置 HTTP CONNECT 隧道支持。

**前提**：局域网内有一个支持 HTTP CONNECT 的代理（Clash Verge、V2Ray 等），并开启了「允许局域网连接」。

可以在 `mimi_secrets.h` 中编译时设置，也可以通过串口 CLI 随时修改：

```
mimi> set_proxy 192.168.1.83 7897   # 设置代理
mimi> clear_proxy                    # 清除代理
```

> **提示**：确保 ESP32-S3 和代理机器在同一局域网。Clash Verge 在「设置 → 允许局域网」中开启。

### 飞书 Bot 接入与联调

这个分支默认是**无外部 voice gateway 版本**。文本对话开箱即用，不需要先启动 `tools/voice_gateway.py`。

#### 1. 填写飞书配置

正常接入飞书 Bot 时，你真正需要关心的是这 6 项：

- **必填**：`App ID`
- **必填**：`App Secret`
- **推荐**：`Receive Mode`
- **webhook 模式强烈建议填写**：`Verify Token`
- **webhook 模式可选**：`Encrypt Key`
- **通常不用改**：`Open API Base`

在 `main/mimi_secrets.h` 中填写：

```c
#define MIMI_SECRET_FEISHU_APP_ID        "cli_xxx"              // 必填：飞书应用 App ID
#define MIMI_SECRET_FEISHU_APP_SECRET    "xxx"                  // 必填：飞书应用 App Secret
#define MIMI_SECRET_FEISHU_RECEIVE_MODE  "websocket"            // 推荐：板载长连接；可改为 webhook
#define MIMI_SECRET_FEISHU_VERIFY_TOKEN  "mimiclaw-feishu"      // webhook 模式建议填写
#define MIMI_SECRET_FEISHU_ENCRYPT_KEY   ""                     // 仅 webhook 加密回调时填写
#define MIMI_SECRET_FEISHU_OPEN_API_BASE "https://open.feishu.cn" // 一般不要改；仅本地 stub 联调时覆盖
```

说明：

- `App ID` 和 `App Secret` 是飞书 Bot 正常收发消息的**必填项**
- `MIMI_SECRET_FEISHU_RECEIVE_MODE` 支持 `websocket` 和 `webhook`
- 推荐优先使用 `websocket`：设备主动连飞书，**不需要公网回调地址**
- 只有 `webhook` 模式才需要 `Verify Token`、`Encrypt Key`、`/feishu/events` 和公网回调
- `Verify Token` 不是飞书开放平台强制项，但 webhook 模式下这个项目**建议一定填写**
- `Encrypt Key` 可以先留空；如果你暂时只想先跑通 webhook 文本回调，建议先不要开加密
- 飞书配置既可写在编译时默认值里，也可通过 CLI 在运行时覆盖
- 如果你在飞书开放平台配置了 `Verify Token` / `Encrypt Key`，这里必须保持一致
- `MIMI_SECRET_FEISHU_OPEN_API_BASE` 默认保持官方地址；只有本地 stub 联调时才需要覆盖

#### 2. 在飞书开放平台配置应用

你在飞书后台至少要做这些配置：

- 创建**自建应用**
- 开启**机器人能力**
- 订阅事件 `im.message.receive_v1`
- 如果接收模式是 `websocket`：
  - 不需要配置 `Request URL`
  - 不需要公网地址
- 如果接收模式是 `webhook`：
  - 请求地址填 `https://<你的公网地址>/feishu/events`
  - `Verify Token` 填成和 `MIMI_SECRET_FEISHU_VERIFY_TOKEN` 一样
  - `Encrypt Key` 可留空；如果启用，就填成和 `MIMI_SECRET_FEISHU_ENCRYPT_KEY` 一样

当前固件已经支持**飞书加密事件体**：

- 这部分只针对 `webhook`
- 未配置 `Encrypt Key` 时，按明文事件体处理
- 配置了 `Encrypt Key` 时，会校验 `X-Lark-Signature`，再解密 `encrypt` 字段
- 仍建议保留 `Verify Token`，这样 URL 校验和普通事件都能多一道来源校验

最小可用配置建议：

1. 先只填 `App ID`、`App Secret`、`Verify Token`
2. `Receive Mode` 先用 `websocket`
3. 如果你必须走回调，再切 `webhook`
4. `Encrypt Key` 只在 webhook 稳定后再开启

#### 3. 接入模式差异

- `websocket`：
  - 设备主动建立飞书长连接
  - 不需要公网回调
  - 真实必填只有 `App ID` / `App Secret`
- `webhook`：
  - 回调路径固定为 `/feishu/events`
  - HTTP 服务默认监听端口 `18789`
  - 如果设备不在公网，需要自己做反向代理、端口映射或内网穿透，把外部请求转到 `http://<设备局域网IP>:18789/feishu/events`

#### 4. 烧录后的联调步骤

- 打开串口监控
- 如果是 `websocket` 模式，确认日志里出现 `Feishu WebSocket long connection enabled` 以及 `Feishu WS connected`
- 如果是 `webhook` 模式，确认日志里出现 `Feishu callback registered at /feishu/events`
- 如果 webhook 启用了 `Encrypt Key`，额外确认 URL 校验和事件推送都返回成功
- 在飞书里给机器人发送 `hello`
- 预期现象：
  - websocket 模式下，设备日志出现 `Feishu WS connected`
  - webhook 模式下，飞书开放平台事件订阅页显示回调成功
  - 设备日志出现 `Feishu text from ...`
  - Bot 返回一条文本回复

#### 4.1 本地回放联调（不依赖飞书后台）

如果你只想验证设备侧的 `/feishu/events` 处理链路，可以直接在开发机执行：

```bash
./tools/run_feishu_replay.sh --scenario all --verify-token mimiclaw-feishu
```

常见用法：

```bash
# 明文 text / duplicate / image / file / audio / sticker 全回放
./tools/run_feishu_replay.sh --scenario all --verify-token mimiclaw-feishu

# 只测加密重复投递
./tools/run_feishu_replay.sh \
  --scenario duplicate \
  --verify-token mimiclaw-feishu \
  --encrypt-key your_encrypt_key \
  --encrypted

# 打印请求与响应体，便于对照签名和事件结构
./tools/run_feishu_replay.sh --scenario text --show-body
```

说明：

- 脚本默认把请求打到 `http://127.0.0.1:18789/feishu/events`
- `duplicate` 场景会复用同一个 `event_id/message_id` 连发两次，用来验证固件去重
- 在默认配置下，`image/file/audio/sticker` 只验证“回调 -> 摘要文本 -> Agent”这条降级链路

#### 4.2 本地验证 `image/file` 真实下载 + gateway 解析

如果你已经启用了 `MIMI_FEISHU_GATEWAY_MEDIA_ENABLED=1`，还可以用本地 OpenAPI stub 把 `image/file` 的真实下载分支也跑通。

1. 在设备 CLI 中把飞书 OpenAPI 基地址指到开发机：

```text
mimi> set_feishu_open_api_base http://<你的电脑IP>:19091
```

2. 在开发机启动本地飞书 OpenAPI stub：

```bash
python3 tools/feishu_openapi_stub.py --host 0.0.0.0 --port 19091
```

3. 另一个终端启动 `voice_gateway.py`

4. 再执行带 gateway 断言的校验：

```bash
./tools/run_feishu_validate.sh \
  --scenario image \
  --verify-token mimiclaw-feishu \
  --log-file ./logs/monitor.log \
  --expect-media-mode gateway
```

说明：

- stub 内置 `image_key=img_replay_demo` 和 `file_key=file_replay_demo`
- `--expect-media-mode gateway` 会要求日志里出现 `gateway_parse from`
- 如果你想回到官方飞书地址，可执行 `mimi> clear_feishu_open_api_base`

如果你已经把串口日志保存到文件，还可以直接做“回放 + 校验”：

```bash
./tools/run_feishu_validate.sh \
  --scenario all \
  --verify-token mimiclaw-feishu \
  --log-file ./logs/monitor.log
```

这个校验脚本会检查：

- HTTP 返回是否符合预期
- `url_verification` 是否返回正确 `challenge`
- `duplicate` 场景是否在日志里出现 `Skip duplicate Feishu event`
- 文本/媒体场景是否在日志里出现对应入站关键字

如果你想把“启动串口 monitor + 回放校验 + 日志保留”一次跑完，可以直接执行：

```bash
./tools/run_feishu_validate_live.sh \
  --port /dev/ttyACM0 \
  --scenario all \
  --verify-token mimiclaw-feishu
```

说明：

- 这个脚本会后台启动 `idf.py -p PORT monitor`
- monitor 输出会自动落到 `logs/feishu-validate-*.log`
- 校验结束后会自动停掉 monitor，并保留日志供复盘

#### 5. 当前边界

- 文本消息会原样进入 Agent
- 默认情况下，图片 / 文件 / 语音 / 贴纸 / 其他飞书媒体消息会退化成摘要文本，再进入 Agent
- 若启用 `MIMI_FEISHU_GATEWAY_MEDIA_ENABLED=1` 并启动 `voice_gateway.py`，飞书 `image/file` 会先下载资源，再走 vision / doc 解析；`audio/sticker/其他` 仍保持摘要模式
- 飞书出站发送使用 `chat_id`
- 飞书重复投递会按 `event_id/message_id` 做轻量去重，避免同一条文本被重复送进 Agent

### 可选：语音/视觉网关启动

这部分是**可选扩展**。默认无网关版本不依赖它；只有你想给 Telegram 增加真实 STT、图片理解、文档解析时，才需要单独启动。

默认快速开始不需要这一部分。如果你只是想先让板子联网并在 Telegram 上跑起来，可以先跳过。

在开发机启动网关（同时提供 STT 与图片解析入口）：

```bash
python3 tools/voice_gateway.py \
  --host 0.0.0.0 --port 8090 --model small --device cpu \
  --vision-enabled
```

- STT 入口：`http://<你的电脑IP>:8091/stt_upload`
- 图片解析入口：`http://<你的电脑IP>:8091/vision_upload`
- 文档解析入口：`http://<你的电脑IP>:8091/doc_upload`
- 默认会尝试从 `main/mimi_secrets.h` 读取视觉 API 配置；如需覆盖，可传 `--vision-endpoint/--vision-api-key/--vision-model`

要让固件真正启用这些扩展，还需要把 `main/mimi_config.h` 中的：

```c
#define MIMI_TELEGRAM_GATEWAY_MEDIA_ENABLED 1
#define MIMI_FEISHU_GATEWAY_MEDIA_ENABLED   1
```

重新编译烧录。默认值都为 `0`，即只保留文本与媒体摘要模式。开启后：

- Telegram 会恢复语音 / 图片 / 文件的真实 STT / vision / doc 解析
- 飞书会为 `image/file` 启用真实下载 + vision / doc 解析，其他媒体类型仍走摘要

### 文档解析回归冒烟测试

网关启动后，可一键回归：

```bash
./tools/run_doc_regression.sh --basic
./tools/run_doc_regression.sh --office
```

也可自定义参数执行：

```bash
python3 tools/doc_regression.py \
  --manifest tools/doc_regression_manifest.example.json \
  --base-url http://127.0.0.1:8091
```

脚本会调用 `/doc_upload`，校验格式、文本长度、关键词、解析器前缀和耗时阈值。
`tools/doc_regression_manifest.office.example.json` 内含可直接运行的 `xlsx` 样本，以及一个可选 `xls` 用例（缺失时自动跳过）。

### CLI 命令

通过串口连接即可配置和调试。**配置命令**让你无需重新编译就能修改设置 — 随时随地插上 USB 线就能改。

**运行时配置**（存入 NVS，覆盖编译时默认值）：

```
mimi> wifi_set MySSID MyPassword   # 换 WiFi
mimi> set_tg_token 123456:ABC...   # 换 Telegram Bot Token
mimi> set_feishu_app cli_xxx secret_xxx   # 设置飞书 app_id / app_secret
mimi> set_feishu_receive_mode websocket   # 切到板载长连接（推荐）
mimi> set_feishu_verify_token token_xxx   # 设置飞书 Verify Token
mimi> set_feishu_encrypt_key key_xxx      # 设置飞书 Encrypt Key
mimi> set_feishu_open_api_base http://127.0.0.1:19091  # 覆盖飞书 OpenAPI 基地址（联调用）
mimi> set_api_key sk-ant-api03-... # 换 API Key（Anthropic 或 OpenAI）
mimi> set_model_provider openai    # 切换提供商（anthropic|openai）
mimi> set_model gpt-4o             # 换模型
mimi> set_proxy 192.168.1.83 7897  # 设置代理
mimi> clear_proxy                  # 清除代理
mimi> set_search_key BSA...        # 设置 Brave Search API Key
mimi> config_show                  # 查看所有配置（脱敏显示）
mimi> config_reset                 # 清除 NVS，恢复编译时默认值
```

说明：

- 现在支持通过 CLI 更新飞书 `app_id/app_secret/receive_mode/verify_token/encrypt_key/open_api_base`
- 已保存到 NVS 的飞书配置会覆盖编译时默认值

**调试与运维：**

```
mimi> wifi_status              # 连上了吗？
mimi> memory_read              # 看看它记住了什么
mimi> memory_write "内容"       # 写入 MEMORY.md
mimi> heap_info                # 还剩多少内存？
mimi> agent_stats              # Agent 成功率/时延/失败计数
mimi> heartbeat_status         # Heartbeat 计数/最后运行时间
mimi> heartbeat_now            # 立即触发一次 Heartbeat
mimi> cron_status              # Cron 调度与计数
mimi> cron_set 30 "任务..."     # 设置每 30 分钟执行
mimi> cron_now                 # 立即触发一次 Cron
mimi> cron_clear               # 清除 Cron 调度
mimi> session_list             # 列出所有会话
mimi> session_clear 12345      # 删除一个会话
mimi> heartbeat_trigger           # 手动触发一次心跳检查
mimi> cron_start                  # 立即启动 cron 调度器
mimi> restart                     # 重启
```

## 记忆

MimiClaw 把所有数据存为纯文本文件，可以直接读取和编辑：

| 文件 | 说明 |
|------|------|
| `SOUL.md` | 机器人的人设 — 编辑它来改变行为方式 |
| `USER.md` | 关于你的信息 — 姓名、偏好、语言 |
| `AGENTS.md` | 行为规范和安全约束 |
| `TOOLS.md` | 工具使用策略和优先级 |
| `SKILLS.md` | 技能路由提示和触发式指令规则 |
| `IDENTITY.md` | 助手身份定位与回复一致性约束 |
| `HEARTBEAT.md` | 周期内部任务说明（仅非注释行生效） |
| `CRON.md` | 默认定时任务文件（`every_minutes` + `task`） |
| `MEMORY.md` | 长期记忆 — 它应该一直记住的事 |
| `daily/2026-02-05.md` | 每日笔记 — 今天发生了什么 |
| `HEARTBEAT.md` | 待办清单 — 机器人定期检查并自主执行 |
| `cron.json` | 定时任务 — AI 创建的周期性或一次性任务 |
| `2026-02-05.md` | 每日笔记 — 今天发生了什么 |
| `tg_12345.jsonl` | 聊天记录 — 你和它的对话 |

## 工具

MimiClaw 同时支持 Anthropic 和 OpenAI 的工具调用 — LLM 在对话中可以调用工具，循环执行直到任务完成（ReAct 模式）。

| 工具 | 说明 |
|------|------|
| `web_search` | 通过 Brave Search API 搜索网页，获取实时信息 |
| `get_current_time` | 通过 HTTP 获取当前日期和时间，并设置系统时钟 |
| `get_device_info` | 读取设备真实运行时硬件信息，包括芯片 / CPU / Flash / PSRAM / GPIO |
| `read_file` | 读取 SPIFFS 文件（路径需以 `/spiffs/` 开头） |
| `write_file` | 写入或覆盖 SPIFFS 文件（默认白名单：`/spiffs/memory/`、`/spiffs/skills/`） |
| `edit_file` | 对 SPIFFS 文件执行查找替换（默认白名单：`/spiffs/memory/`、`/spiffs/skills/`） |
| `list_dir` | 列出 SPIFFS 文件，可按前缀过滤 |
| `memory_write_long_term` | 覆盖长期记忆（`/spiffs/memory/MEMORY.md`） |
| `memory_append_today` | 追加一条今天的 daily 记忆 |
| `cron_add` | 创建定时或一次性任务（LLM 自主创建 cron 任务） |
| `cron_list` | 列出所有已调度的 cron 任务 |
| `cron_remove` | 按 ID 删除 cron 任务 |

启用网页搜索需要在 `mimi_secrets.h` 中设置 [Brave Search API key](https://brave.com/search/api/)（`MIMI_SECRET_SEARCH_KEY`）。

## 定时任务（Cron）

MimiClaw 内置 cron 调度器，让 AI 可以自主安排任务。LLM 可以通过 `cron_add` 工具创建周期性任务（"每 N 秒"）或一次性任务（"在某个时间戳"）。任务触发时，消息会注入到 Agent 循环 — AI 自动醒来、处理任务并回复。

任务持久化存储在 SPIFFS（`cron.json`），重启后不会丢失。典型用途：每日总结、定时提醒、定期巡检。

## 心跳（Heartbeat）

心跳服务会定期读取 SPIFFS 上的 `HEARTBEAT.md`，检查是否有待办事项。如果发现未完成的条目（非空行、非标题、非已勾选的 `- [x]`），就会向 Agent 循环发送提示，让 AI 自主处理。

这让 MimiClaw 变成一个主动型助理 — 把任务写入 `HEARTBEAT.md`，机器人会在下一次心跳周期自动拾取执行（默认每 30 分钟）。

## 其他功能

- **WebSocket 网关** — 端口 18789，局域网内用任意 WebSocket 客户端连接
- **飞书 Bot** — 同时支持板载 WebSocket 长连接和 `/feishu/events` webhook 回调；默认文本直通、媒体摘要，可选为 `image/file` 开启真实 gateway 解析
- **OTA 更新** — WiFi 远程刷固件，无需 USB
- **双核** — 网络 I/O 和 AI 处理分别跑在不同 CPU 核心
- **HTTP 代理** — CONNECT 隧道，适配受限网络
- **工具调用** — ReAct Agent 循环，Anthropic tool use 协议
- **Telegram 媒体处理** — 默认无网关模式下，语音/图片/文件会退化为媒体摘要；启用 `MIMI_TELEGRAM_GATEWAY_MEDIA_ENABLED=1` 并启动 `voice_gateway.py` 后，才会开启真实 STT / vision / doc_upload 扩展
- **飞书媒体处理** — 默认无网关模式下走媒体摘要；启用 `MIMI_FEISHU_GATEWAY_MEDIA_ENABLED=1` 并启动 `voice_gateway.py` 后，`image/file` 会下载资源并走真实 vision / doc_upload 扩展

## 工程化增强路线（P0，进行中）

- [x] 入站安全：Telegram allowlist（`allow_from`）+ WebSocket 鉴权 token
- [x] 文件工具边界：默认只允许写 `/spiffs/memory/`，配置目录默认只读
- [x] 可靠性：LLM/发送链路重试与退避，状态消息可丢、正式回复尽量不丢
- [x] 预算守卫：工具轮次、上下文大小、工具输出大小、总耗时限制
- [x] 记忆治理：统一 daily memory 路径，补齐专用记忆写入工具
- [x] 可观测性：`run_id`、分阶段耗时、`agent_stats` 诊断命令

详细跟踪见 **[docs/TODO.md](docs/TODO.md)**。
- **多提供商** — 同时支持 Anthropic (Claude) 和 OpenAI (GPT)，运行时可切换
- **定时任务** — AI 可自主创建周期性和一次性任务，重启后持久保存
- **心跳服务** — 定期检查任务文件，驱动 AI 自主执行
- **工具调用** — ReAct Agent 循环，两种提供商均支持工具调用

## 开发者

技术细节在 `docs/` 文件夹：

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — 系统设计、模块划分、任务布局、内存分配、协议、Flash 分区
- **[docs/TODO.md](docs/TODO.md)** — 功能差距和路线图

## Contributing

Please read **[docs/CONTRIBUTE.md](docs/CONTRIBUTE.md)** before opening issues or pull requests.

## 许可证

MIT

## 致谢

灵感来自 [OpenClaw](https://github.com/openclaw/openclaw) 和 [Nanobot](https://github.com/HKUDS/nanobot)。MimiClaw 为嵌入式硬件重新实现了核心 AI Agent 架构 — 没有 Linux，没有服务器，只有一颗 $5 的芯片。

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=memovai/mimiclaw&type=Date)](https://star-history.com/#memovai/mimiclaw&Date)
