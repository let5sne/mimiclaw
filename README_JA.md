# MimiClaw: $5チップで動くポケットAIアシスタント

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

**$5チップ上で動く世界初のAIアシスタント（OpenClaw）。Linuxなし、Node.jsなし、純粋なCのみ。**

MimiClawは小さなESP32-S3ボードをパーソナルAIアシスタントに変えます。USB電源に接続し、WiFiにつなげて、Telegram または Feishu Bot から話しかけるだけ — どんなタスクも処理し、ローカルメモリで時間とともに成長します — すべて親指サイズのチップ上で。

## MimiClawの特徴

- **超小型** — Linux不要、Node.js不要、無駄なし — 純粋なCのみ
- **便利** — Telegram または Feishu でメッセージを送るだけ、あとはお任せ
- **忠実** — メモリから学習し、再起動しても忘れない
- **省エネ** — USB給電、0.5W、24時間365日稼働
- **お手頃** — ESP32-S3ボード1枚、$5、それだけ

## 仕組み

![](assets/mimiclaw.png)

Telegram、Feishu、またはLAN内のWebSocketクライアントからメッセージを送ると、ESP32-S3がWiFi経由で受信し、エージェントループに送ります — LLMが思考し、ツールを呼び出し、メモリを読み取り — 返答を送り返します。**Anthropic (Claude)** と **OpenAI (GPT)** の両方をサポートし、実行時に切り替え可能です。すべてが$5のチップ上で動作し、データはすべてローカルのFlashに保存されます。

## クイックスタート

デフォルトのクイックスタートは **ESP32-S3-DevKitC-1** を使用し、**外部周辺機器は接続しません**。まず正しい **USB** ポートに接続し、WiFi / Bot / APIキーを設定してファームウェアを書き込み、Telegram または Feishu から動作確認します。

### 5ステップ最短経路

1. **ESP32-S3-DevKitC-1** を用意し、**COM** ではなく **USB** と書かれたポートに接続します。
2. ESP-IDF をインストールし、このリポジトリをクローンします。
3. `main/mimi_secrets.h.example` を `main/mimi_secrets.h` にコピーし、WiFi、Bot、APIキーを設定します。
4. ファームウェアをビルドして書き込みます。
5. Telegram または Feishu で `/start` または `hello` を送り、完全な Agent 経路を確認します。

### 必要なもの

- **ESP32-S3-DevKitC-1**（デフォルトのクイックスタート用ボード。16MB Flash + 8MB PSRAM のバリアントを推奨）
- **USB Type-Cケーブル**
- **Telegram Botトークン**、または **Feishu 自作アプリ**（Bot機能 + イベント購読を有効化）
- **Anthropic APIキー** — [console.anthropic.com](https://console.anthropic.com)から取得、または **OpenAI APIキー** — [platform.openai.com](https://platform.openai.com)から取得
- デフォルトのクイックスタートでは、マイク、スピーカー、ディスプレイは不要です

### インストール

```bash
# まずESP-IDF v5.5+をインストールしてください:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3
```

<details>
<summary>Ubuntu インストール</summary>

推奨ベースライン:

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

Ubuntu でのインストールとビルド:

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

./scripts/setup_idf_ubuntu.sh
./scripts/build_ubuntu.sh
```

</details>

<details>
<summary>macOS インストール</summary>

推奨ベースライン:

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

macOS でのインストールとビルド:

```bash
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

./scripts/setup_idf_macos.sh
./scripts/build_macos.sh
```

</details>

### 設定

MimiClawは**2層設定**を採用しています：`mimi_secrets.h`でビルド時のデフォルト値を設定し、シリアルCLIで実行時にオーバーライドできます。CLI設定値はNVS Flashに保存され、ビルド時の値より優先されます。

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

`main/mimi_secrets.h`を編集：

```c
#define MIMI_SECRET_WIFI_SSID       "WiFi名"
#define MIMI_SECRET_WIFI_PASS       "WiFiパスワード"
#define MIMI_SECRET_TG_TOKEN        ""              // Feishu だけ使うなら空でよい
#define MIMI_SECRET_FEISHU_APP_ID   ""
#define MIMI_SECRET_FEISHU_APP_SECRET ""
#define MIMI_SECRET_FEISHU_VERIFY_TOKEN ""
#define MIMI_SECRET_FEISHU_ENCRYPT_KEY ""
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"     // "anthropic" または "openai"
#define MIMI_SECRET_SEARCH_KEY      ""              // 任意：Brave Search APIキー
#define MIMI_SECRET_PROXY_HOST      ""              // 任意：例 "10.0.0.1"
#define MIMI_SECRET_PROXY_PORT      ""              // 任意：例 "7897"
```

ビルドとフラッシュ：

```bash
# フルビルド（mimi_secrets.h変更後はfullclean必須）
idf.py fullclean && idf.py build

# シリアルポートを確認
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# フラッシュとモニター（PORTをあなたのポートに置き換え）
# USBアダプタ：おそらく /dev/cu.usbmodem11401（macOS）または /dev/ttyACM0（Linux）
idf.py -p PORT flash monitor
```

最初の確認手順:

- Telegram: `/start` を送って接続を確認
- Feishu: `hello` を送って入站と返信を確認
- `hello` を送って完全な Agent 経路を確認

> **重要：正しいUSBポートに接続してください！** ほとんどのESP32-S3ボードには2つのUSB-Cポートがあります。**USB**（ネイティブUSB Serial/JTAG）と書かれたポートを使用してください。**COM**（外部UARTブリッジ）と書かれたポートは使わないでください。間違ったポートに接続するとフラッシュ/モニターが失敗します。
>
> **クイックスタートの基準ボード**：この README の主経路は **ESP32-S3-DevKitC-1** を前提にしています。初回起動は最小構成のままにしてください：**USBケーブルのみ接続し、マイク、スピーカー、ディスプレイは接続しません**。
>
> <details>
> <summary>参考画像を表示</summary>
>
> <img src="assets/esp32s3-usb-port.jpg" alt="USBポートに接続、COMポートではありません" width="480" />
>
> </details>

### Feishu Bot セットアップ

このブランチの既定値は **外部 voice gateway なし** の構成です。`tools/voice_gateway.py` を起動しなくてもテキスト会話は動作します。

1. `main/mimi_secrets.h` に次の Feishu 設定を入れます。

通常の Feishu Bot 接続で重要なのは次の項目です。

- 必須: `App ID`
- 必須: `App Secret`
- 推奨: `Receive Mode`
- `webhook` モードで強く推奨: `Verify Token`
- `webhook` モードで任意: `Encrypt Key`
- 通常は変更不要: `Open API Base`

```c
#define MIMI_SECRET_FEISHU_APP_ID        "cli_xxx"              // 必須: Feishu App ID
#define MIMI_SECRET_FEISHU_APP_SECRET    "xxx"                  // 必須: Feishu App Secret
#define MIMI_SECRET_FEISHU_RECEIVE_MODE  "websocket"            // 推奨: 板載長接続。必要なら webhook
#define MIMI_SECRET_FEISHU_VERIFY_TOKEN  "mimiclaw-feishu"      // webhook モードで推奨
#define MIMI_SECRET_FEISHU_ENCRYPT_KEY   ""                     // 暗号化 webhook callback 時のみ必要
#define MIMI_SECRET_FEISHU_OPEN_API_BASE "https://open.feishu.cn" // 通常は既定値のまま。ローカル stub 検証時のみ上書き
```

- `App ID` と `App Secret` は必須です
- `MIMI_SECRET_FEISHU_RECEIVE_MODE` は `websocket` と `webhook` をサポートします
- 推奨デフォルトは `websocket` です。ボード自身が長接続を張るので公開 callback URL は不要です
- `Verify Token`、`Encrypt Key`、`/feishu/events`、公開 callback パスが必要なのは `webhook` モードだけです
- `Verify Token` は Feishu の必須項目ではありませんが、このプロジェクトでは `webhook` モードで設定を強く推奨します
- `Encrypt Key` は平文 webhook が安定してから有効にしてください

2. Feishu Open Platform 側で最低限これらを設定します。

- **自作アプリ** を作成
- **Bot capability** を有効化
- イベント `im.message.receive_v1` を購読
- 受信モードが `websocket` の場合:
  - `Request URL` は不要
  - 公開 callback アドレスは不要
- 受信モードが `webhook` の場合:
  - Request URL を `https://<public-address>/feishu/events` に設定
  - `Verify Token` を `MIMI_SECRET_FEISHU_VERIFY_TOKEN` と同じ値にする
  - 平文 callback なら `Encrypt Key` は空、暗号化 callback なら `MIMI_SECRET_FEISHU_ENCRYPT_KEY` と一致させる
- ローカル OpenAPI stub 検証時を除き、`MIMI_SECRET_FEISHU_OPEN_API_BASE` は公式ホストのままにしてください

このファームウェアは **暗号化された Feishu イベント payload** もサポートします。

- 対象は `webhook` モードのみです
- `Encrypt Key` が空なら callback は平文イベントとして扱われます
- `Encrypt Key` を設定すると、ファームウェアは `X-Lark-Signature` を検証し、`encrypt` フィールドを復号します
- URL 検証と通常イベントの双方で、`Verify Token` を有効にしておくことを推奨します

推奨の最小セットアップ:

1. まず `App ID` と `App Secret` を設定
2. `Receive Mode` は `websocket`
3. callback モードが必要な時だけ `webhook` に切り替える
4. `Encrypt Key` は webhook のテキスト受信が安定してから有効化

3. 必要な受信モードを選びます。

- `websocket`:
  - デバイス自身が Feishu に長接続します
  - 公開 callback は不要です
  - 実質必須なのは `App ID` と `App Secret` だけです
- `webhook`:
  - callback パスは固定で `/feishu/events`
  - 共通 HTTP サービスは `18789` 番ポートで待ち受けます
  - ボードが公開されていない場合は `http://<device-lan-ip>:18789/feishu/events` へ reverse proxy / port forwarding / tunnel を追加してください

4. 書き込み後のスモークテスト:

- シリアルログを監視します
- `websocket` モードでは `Feishu WebSocket long connection enabled` と `Feishu WS connected` を確認します
- `webhook` モードでは `Feishu callback registered at /feishu/events` を確認します
- `Encrypt Key` を有効にした `webhook` モードでは、Feishu コンソールで URL 検証とイベント配送の両方が成功することを確認します
- Feishu Bot に `hello` を送り、MimiClaw からテキスト返信が返ることを確認します

4.1 Feishu コンソールを使わないローカル回放

デバイス側の `/feishu/events` だけを検証したい場合は、開発機から次を実行します。

```bash
./tools/run_feishu_replay.sh --scenario all --verify-token mimiclaw-feishu
```

よく使う例:

```bash
# 平文 text / duplicate / image / file / audio / sticker callback を一括回放
./tools/run_feishu_replay.sh --scenario all --verify-token mimiclaw-feishu

# 暗号化 duplicate 配送を回放
./tools/run_feishu_replay.sh \
  --scenario duplicate \
  --verify-token mimiclaw-feishu \
  --encrypt-key your_encrypt_key \
  --encrypted

# デバッグ用に request / response body を表示
./tools/run_feishu_replay.sh --scenario text --show-body
```

補足:

- 既定では `http://127.0.0.1:18789/feishu/events` を対象にします
- `duplicate` シナリオは同じ `event_id/message_id` を2回送って重複排除を検証します
- 既定構成では `image/file/audio/sticker` は callback -> 要約テキスト -> Agent のダウングレード経路を検証します

#### 4.2 ローカル `image/file` ダウンロード + gateway 検証

`MIMI_FEISHU_GATEWAY_MEDIA_ENABLED=1` を有効にしたなら、ローカル OpenAPI stub で `image/file` の実ダウンロード分岐も検証できます。

1. シリアル CLI でデバイスを開発機に向けます:

```text
mimi> set_feishu_open_api_base http://<your-host-ip>:19091
```

2. ローカル Feishu OpenAPI stub を起動します:

```bash
python3 tools/feishu_openapi_stub.py --host 0.0.0.0 --port 19091
```

3. 別ターミナルで `voice_gateway.py` を起動します

4. gateway 期待値付きで検証を実行します:

```bash
./tools/run_feishu_validate.sh \
  --scenario image \
  --verify-token mimiclaw-feishu \
  --log-file ./logs/monitor.log \
  --expect-media-mode gateway
```

補足:

- stub には `image_key=img_replay_demo` と `file_key=file_replay_demo` が含まれます
- `--expect-media-mode gateway` はログ内に `gateway_parse from` が出ることを要求します
- 公式 Feishu ホストに戻すには `mimi> clear_feishu_open_api_base` を実行します

シリアルログをファイルに保存している場合は、回放と検証をまとめて実行できます。

```bash
./tools/run_feishu_validate.sh \
  --scenario all \
  --verify-token mimiclaw-feishu \
  --log-file ./logs/monitor.log
```

validator が確認する項目:

- 各 replay callback の HTTP response
- `url_verification` の `challenge` 返却
- `duplicate` 実行時にログへ `Skip duplicate Feishu event` が出るか
- text / media シナリオで期待した入站ログマーカーが出るか

シリアル monitor + replay 検証 + ログ収集を一括で回したい場合:

```bash
./tools/run_feishu_validate_live.sh \
  --port /dev/ttyACM0 \
  --scenario all \
  --verify-token mimiclaw-feishu
```

補足:

- このスクリプトは裏で `idf.py -p PORT monitor` を起動します
- monitor 出力は `logs/feishu-validate-*.log` に保存されます
- 検証終了後、monitor プロセスは停止し、ログファイルは保持されます

現在の制限:

- テキストメッセージはそのまま Agent に入ります
- 既定では、`image / file / audio / sticker / その他の非テキスト Feishu メッセージ` は要約テキストにダウングレードされてから Agent に入ります
- `MIMI_FEISHU_GATEWAY_MEDIA_ENABLED=1` を有効にして `voice_gateway.py` を動かすと、Feishu `image/file` は実ダウンロード + vision / doc 解析に進みます。`audio/sticker/other` は要約モードのままです
- Feishu の送信先は `chat_id` を使います
- Feishu の重複配送は `event_id/message_id` で軽量 dedup されてから Agent に入ります

### 任意: 音声 / 視覚 gateway

これは任意の拡張です。既定の no-gateway ビルドはこれに依存しません。

最短クイックスタートでは不要です。まずボードをオンラインにしたいだけなら、この節は今は飛ばして構いません。

ローカル gateway（STT + 画像解析 endpoint）を起動:

```bash
python3 tools/voice_gateway.py \
  --host 0.0.0.0 --port 8090 --model small --device cpu \
  --vision-enabled
```

- STT endpoint: `http://<your-host-ip>:8091/stt_upload`
- Vision endpoint: `http://<your-host-ip>:8091/vision_upload`
- Document endpoint: `http://<your-host-ip>:8091/doc_upload`
- 既定では gateway は `main/mimi_secrets.h` の API 既定値を読み込みます。`--vision-endpoint/--vision-api-key/--vision-model` で上書き可能です

ファームウェア側でこのメディア拡張を有効化するには、`main/mimi_config.h` に次を設定して再ビルドします。

```c
#define MIMI_TELEGRAM_GATEWAY_MEDIA_ENABLED 1
#define MIMI_FEISHU_GATEWAY_MEDIA_ENABLED   1
```

どちらも既定値は `0` で、no-gateway のテキスト + 要約動作を維持します。有効化後は:

- Telegram は音声 / 写真 / 文書で実 STT / vision / doc 解析を再び使えます
- Feishu は `image/file` で実ダウンロード + vision / doc 解析が有効になり、それ以外のメディアは要約にフォールバックします

### 文書解析回帰スモークテスト

gateway 起動後に次を実行します。

```bash
./tools/run_doc_regression.sh --basic
./tools/run_doc_regression.sh --office
```

またはカスタム引数で:

```bash
python3 tools/doc_regression.py \
  --manifest tools/doc_regression_manifest.example.json \
  --base-url http://127.0.0.1:8091
```

このスクリプトは `/doc_upload` を呼び、フォーマット、抽出テキスト長、キーワード、parser prefix、遅延 budget を検証します。`tools/doc_regression_manifest.office.example.json` には実際の `xlsx` サンプルと、存在しない場合は自動で skip される `xls` ケース (`food_legacy.xls`) が含まれています。

### CLIコマンド（UART/COMポート経由）

シリアル接続で設定やデバッグができます。**設定コマンド**により再コンパイル不要で設定変更可能 — USBケーブルを挿すだけ。

**実行時設定**（NVSに保存、ビルド時のデフォルト値をオーバーライド）：

```
mimi> wifi_set MySSID MyPassword   # WiFiネットワークを変更
mimi> set_tg_token 123456:ABC...   # Telegram Botトークンを変更
mimi> set_feishu_app cli_xxx secret_xxx   # Feishu app_id / app_secret を設定
mimi> set_feishu_receive_mode websocket   # 板載長接続に切り替え
mimi> set_feishu_verify_token token_xxx   # Feishu Verify Token を設定
mimi> set_feishu_encrypt_key key_xxx      # Feishu Encrypt Key を設定
mimi> set_feishu_open_api_base http://127.0.0.1:19091  # Feishu OpenAPI ベースURLを上書き
mimi> set_api_key sk-ant-api03-... # APIキーを変更（AnthropicまたはOpenAI）
mimi> set_model_provider openai    # プロバイダーを切替（anthropic|openai）
mimi> set_model gpt-4o             # LLMモデルを変更
mimi> set_proxy 127.0.0.1 7897    # HTTPプロキシを設定
mimi> clear_proxy                  # プロキシを削除
mimi> set_search_key BSA...        # Brave Search APIキーを設定
mimi> config_show                  # 全設定を表示（マスク付き）
mimi> config_reset                 # NVSをクリア、ビルド時デフォルトに戻す
```

補足:

- Feishu の `app_id/app_secret/receive_mode/verify_token/encrypt_key/open_api_base` は CLI から更新できます
- NVS に保存された Feishu 設定はビルド時デフォルトを上書きします

**デバッグ・メンテナンス：**

```
mimi> wifi_status              # 接続されていますか？
mimi> memory_read              # ボットが何を覚えているか確認
mimi> memory_write "内容"       # MEMORY.mdに書き込み
mimi> heap_info                # 空きRAMはどれくらい？
mimi> session_list             # 全チャットセッションを一覧
mimi> session_clear 12345      # 会話を削除
mimi> heartbeat_trigger           # ハートビートチェックを手動トリガー
mimi> cron_start                  # cronスケジューラを今すぐ開始
mimi> restart                     # 再起動
```

### USB（JTAG）vs UART：どのポートで何をするか

ほとんどの ESP32-S3 開発ボードには **2つの USB-C ポート**があります：

| ポート | 用途 |
|--------|------|
| **USB**（JTAG） | `idf.py flash`、JTAGデバッグ |
| **COM**（UART） | **REPL CLI**、シリアルコンソール |

> **REPLにはUART（COM）ポートが必要です。** USB（JTAG）ポートは対話的なREPL入力をサポートしません。

<details>
<summary>ポート詳細と推奨ワークフロー</summary>

| ポート | ラベル | プロトコル |
|--------|--------|------------|
| **USB** | USB / JTAG | ネイティブ USB Serial/JTAG |
| **COM** | UART / COM | 外部 UART ブリッジ（CP2102/CH340） |

ESP-IDFコンソールはデフォルトでUART出力に設定されています（`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`）。

**両方のポートを同時に接続している場合：**

- USB（JTAG）ポートはフラッシュ/ダウンロードを処理し、補助シリアル出力を提供
- UART（COM）ポートはREPL用のメインインタラクティブコンソールを提供
- macOS では両ポートとも `/dev/cu.usbmodem*` または `/dev/cu.usbserial-*` として表示 — `ls /dev/cu.usb*` で確認
- Linux では USB（JTAG）は通常 `/dev/ttyACM0`、UART は通常 `/dev/ttyUSB0`

**推奨ワークフロー：**

```bash
# USB（JTAG）ポートでフラッシュ
idf.py -p /dev/cu.usbmodem11401 flash

# UART（COM）ポートでREPLを開く
idf.py -p /dev/cu.usbserial-110 monitor
# または任意のシリアルターミナル：screen、minicom、PuTTY（ボーレート 115200）
```

</details>

## メモリ

MimiClawはすべてのデータをプレーンテキストファイルとして保存します。直接読み取り・編集可能です：

| ファイル | 説明 |
|----------|------|
| `SOUL.md` | ボットの性格 — 編集して振る舞いを変更 |
| `USER.md` | あなたの情報 — 名前、好み、言語 |
| `AGENTS.md` | 振る舞いルールと安全制約 |
| `TOOLS.md` | ツール利用ポリシーと優先順位 |
| `SKILLS.md` | skill ルーティングのヒントと発火ルール |
| `IDENTITY.md` | アシスタントのアイデンティティと応答整合性の制約 |
| `HEARTBEAT.md` | 周期的に読む内部タスク指示（コメント行以外） |
| `CRON.md` | 既定の cron スケジュールテンプレート (`every_minutes` + `task`) |
| `MEMORY.md` | 長期記憶 — ボットが常に覚えておくべきこと |
| `daily/2026-02-05.md` | 日次メモ — 今日あったこと |
| `cron.json` | スケジュールジョブ — AIが作成した定期・単発タスク |
| `sf0123456789abcdef.j` | 現在の短ハッシュ形式セッション履歴 (`s<channel><hash>.j`) |

## ツール

MimiClawはAnthropicとOpenAI両方のツール呼び出しをサポート — LLMは会話中にツールを呼び出し、タスクが完了するまでループします（ReActパターン）。

| ツール | 説明 |
|--------|------|
| `web_search` | Brave Search API でウェブ検索し、最新情報を取得 |
| `get_current_time` | HTTP経由で現在日時を取得し、システムクロックを設定 |
| `get_device_info` | チップ / CPU / flash / PSRAM / GPIO を含む実行時ハードウェア情報を読む |
| `read_file` | SPIFFS ファイルを読む（パスは `/spiffs/` で始まる必要あり） |
| `write_file` | SPIFFS ファイルを書き込みまたは上書き（既定 allowlist: `/spiffs/memory/`, `/spiffs/skills/`） |
| `edit_file` | SPIFFS ファイルで find-and-replace を行う（既定 allowlist: `/spiffs/memory/`, `/spiffs/skills/`） |
| `list_dir` | SPIFFS ファイル一覧を prefix フィルタ付きで列挙 |
| `memory_write_long_term` | 長期記憶 (`/spiffs/memory/MEMORY.md`) を上書き |
| `memory_append_today` | 今日の日次メモに1件追記 |
| `cron_add` | 定期または単発タスクをスケジュール（LLM が自律的に cron ジョブを作成） |
| `cron_list` | スケジュール済み cron ジョブを一覧表示 |
| `cron_remove` | IDで cron ジョブを削除 |

ウェブ検索を有効にするには、`mimi_secrets.h`で[Brave Search APIキー](https://brave.com/search/api/)（`MIMI_SECRET_SEARCH_KEY`）を設定してください。

## Cronタスク

MimiClawにはcronスケジューラが内蔵されており、AIが自律的にタスクをスケジュールできます。LLMは`cron_add`ツールで定期ジョブ（「N秒ごと」）や単発ジョブ（「UNIXタイムスタンプで指定」）を作成できます。ジョブが発火すると、メッセージがエージェントループに注入され、AIが起動してタスクを処理・応答します。

ジョブはSPIFFS（`cron.json`）に永続化され、再起動後も保持されます。活用例：日次サマリー、定期リマインダー、スケジュールチェック。

## ハートビート

ハートビートサービスはSPIFFS上の`HEARTBEAT.md`を定期的に読み取り、アクション可能なタスクがあるかチェックします。未完了の項目（空行、見出し、チェック済み`- [x]`以外）が見つかると、エージェントループにプロンプトを送信し、AIが自律的に処理します。

これによりMimiClawはプロアクティブなアシスタントになります — `HEARTBEAT.md`にタスクを書き込めば、次のハートビートサイクルで自動的に拾い上げて実行します（デフォルト：30分ごと）。

## その他の機能

- **WebSocketゲートウェイ** — ポート18789、LAN内から任意のWebSocketクライアントで接続
- **Feishu Bot** — 板載 WebSocket 長接続と `/feishu/events` webhook 受信の両方をサポート。既定はテキスト直通 + 要約フォールバック、`image/file` には任意で実 gateway 解析を有効化可能
- **OTAアップデート** — WiFi経由でファームウェア更新、USB不要
- **デュアルコア** — ネットワークI/OとAI処理が別々のCPUコアで動作
- **HTTPプロキシ** — CONNECTトンネル対応、制限付きネットワークに対応
- **マルチプロバイダー** — Anthropic (Claude) と OpenAI (GPT) の両方をサポート、実行時に切り替え可能
- **ツール呼び出し** — ReAct エージェントループ。Anthropic / OpenAI の両方に対応
- **Telegram メディア処理** — 既定の no-gateway モードでは音声 / 写真 / 文書は要約にフォールバック。`MIMI_TELEGRAM_GATEWAY_MEDIA_ENABLED=1` と `voice_gateway.py` で実 STT / vision / doc 解析を再有効化可能
- **Feishu メディア処理** — 既定の no-gateway モードではメディアは要約にフォールバック。`MIMI_FEISHU_GATEWAY_MEDIA_ENABLED=1` と `voice_gateway.py` で `image/file` は実ダウンロード + vision / doc 解析を使えます

## 開発者向け

技術的な詳細は`docs/`フォルダにあります：

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — システム設計、モジュール構成、タスクレイアウト、メモリバジェット、プロトコル、Flashパーティション
- **[docs/TODO.md](docs/TODO.md)** — 機能ギャップとロードマップ

## 貢献

Issue や Pull Request を作成する前に、**[CONTRIBUTING.md](CONTRIBUTING.md)** をご確認ください。

## コントリビューター

MimiClaw に貢献してくれた皆さんに感謝します。

<a href="https://github.com/memovai/mimiclaw/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=memovai/mimiclaw" alt="MimiClaw contributors" />
</a>

## ライセンス

MIT

## 謝辞

[OpenClaw](https://github.com/openclaw/openclaw)と[Nanobot](https://github.com/HKUDS/nanobot)にインスパイアされました。MimiClawはコアAIエージェントアーキテクチャを組み込みハードウェア向けに再実装しました — Linuxなし、サーバーなし、$5のチップだけ。

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=memovai/mimiclaw&type=Date)](https://star-history.com/#memovai/mimiclaw&Date)
