#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_BASE_URL="${FEISHU_REPLAY_BASE_URL:-http://127.0.0.1:18789}"

BASE_URL="${DEFAULT_BASE_URL}"
SCENARIO="all"
VERIFY_TOKEN="${FEISHU_REPLAY_VERIFY_TOKEN:-mimiclaw-feishu}"
ENCRYPT_KEY="${FEISHU_REPLAY_ENCRYPT_KEY:-}"
ENCRYPTED=0
SHOW_BODY=0

print_help() {
  cat <<EOF
用法:
  ./tools/run_feishu_replay.sh [--url URL] [--scenario NAME] [--verify-token TOKEN]
                               [--encrypt-key KEY] [--encrypted] [--show-body]

参数:
  --url URL           设备 HTTP 服务地址，默认: ${DEFAULT_BASE_URL}
  --scenario NAME     url|text|duplicate|image|file|audio|sticker|all，默认 all
  --verify-token TOK  飞书 Verify Token，默认: ${VERIFY_TOKEN}
  --encrypt-key KEY   飞书 Encrypt Key
  --encrypted         使用加密回调模式
  --show-body         打印请求和响应体
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
    --show-body)
      SHOW_BODY=1
      shift
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
  python3 "${ROOT_DIR}/tools/feishu_callback_replay.py"
  --base-url "${BASE_URL}"
  --scenario "${SCENARIO}"
  --verify-token "${VERIFY_TOKEN}"
)

if [[ -n "${ENCRYPT_KEY}" ]]; then
  CMD+=(--encrypt-key "${ENCRYPT_KEY}")
fi
if [[ ${ENCRYPTED} -eq 1 ]]; then
  CMD+=(--encrypted)
fi
if [[ ${SHOW_BODY} -eq 1 ]]; then
  CMD+=(--show-body)
fi

echo "运行飞书回放: scenario=${SCENARIO} base_url=${BASE_URL} encrypted=${ENCRYPTED}"
"${CMD[@]}"
