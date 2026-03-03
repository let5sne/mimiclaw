#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_BASE_URL="${FEISHU_REPLAY_BASE_URL:-http://127.0.0.1:18789}"

BASE_URL="${DEFAULT_BASE_URL}"
SCENARIO="all"
VERIFY_TOKEN="${FEISHU_REPLAY_VERIFY_TOKEN:-mimiclaw-feishu}"
ENCRYPT_KEY="${FEISHU_REPLAY_ENCRYPT_KEY:-}"
LOG_FILE="${FEISHU_REPLAY_LOG_FILE:-}"
ENCRYPTED=0
EXPECT_MEDIA_MODE="${FEISHU_EXPECT_MEDIA_MODE:-auto}"

print_help() {
  cat <<EOF
用法:
  ./tools/run_feishu_validate.sh [--url URL] [--scenario NAME] [--verify-token TOKEN]
                                 [--encrypt-key KEY] [--encrypted] [--log-file PATH]
                                 [--expect-media-mode MODE]

参数:
  --url URL           设备 HTTP 服务地址，默认: ${DEFAULT_BASE_URL}
  --scenario NAME     url|text|duplicate|image|file|audio|sticker|all，默认 all
  --verify-token TOK  飞书 Verify Token，默认: ${VERIFY_TOKEN}
  --encrypt-key KEY   飞书 Encrypt Key
  --encrypted         使用加密回调模式
  --log-file PATH     可选：串口日志文件，用于校验去重和媒体摘要日志
  --expect-media-mode 媒体模式期望：auto|summary|gateway，默认: ${EXPECT_MEDIA_MODE}
  -h, --help          显示帮助
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --url)
      BASE_URL="$2"
      shift 2
      ;;
    --scenario)
      SCENARIO="$2"
      shift 2
      ;;
    --verify-token)
      VERIFY_TOKEN="$2"
      shift 2
      ;;
    --encrypt-key)
      ENCRYPT_KEY="$2"
      shift 2
      ;;
    --encrypted)
      ENCRYPTED=1
      shift
      ;;
    --log-file)
      LOG_FILE="$2"
      shift 2
      ;;
    --expect-media-mode)
      EXPECT_MEDIA_MODE="$2"
      shift 2
      ;;
    -h|--help)
      print_help
      exit 0
      ;;
    *)
      echo "未知参数: $1" >&2
      print_help
      exit 2
      ;;
  esac
done

CMD=(
  python3 "${ROOT_DIR}/tools/feishu_callback_validate.py"
  --base-url "${BASE_URL}"
  --scenario "${SCENARIO}"
  --verify-token "${VERIFY_TOKEN}"
  --expect-media-mode "${EXPECT_MEDIA_MODE}"
)

if [[ -n "${ENCRYPT_KEY}" ]]; then
  CMD+=(--encrypt-key "${ENCRYPT_KEY}")
fi
if [[ ${ENCRYPTED} -eq 1 ]]; then
  CMD+=(--encrypted)
fi
if [[ -n "${LOG_FILE}" ]]; then
  CMD+=(--log-file "${LOG_FILE}")
fi

echo "运行飞书回放校验: scenario=${SCENARIO} base_url=${BASE_URL} encrypted=${ENCRYPTED} expect_media_mode=${EXPECT_MEDIA_MODE}"
"${CMD[@]}"
