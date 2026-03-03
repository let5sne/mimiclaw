#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_BASE_URL="${FEISHU_REPLAY_BASE_URL:-http://127.0.0.1:18789}"
DEFAULT_LOG_DIR="${ROOT_DIR}/logs"

BASE_URL="${DEFAULT_BASE_URL}"
SCENARIO="all"
VERIFY_TOKEN="${FEISHU_REPLAY_VERIFY_TOKEN:-mimiclaw-feishu}"
ENCRYPT_KEY="${FEISHU_REPLAY_ENCRYPT_KEY:-}"
PORT="${ESPPORT:-}"
LOG_FILE=""
MONITOR_COMMAND=""
ENCRYPTED=0
MONITOR_WARMUP=3

print_help() {
  cat <<EOF
用法:
  ./tools/run_feishu_validate_live.sh [--port PORT] [--url URL] [--scenario NAME]
                                      [--verify-token TOKEN] [--encrypt-key KEY]
                                      [--encrypted] [--log-file PATH]
                                      [--monitor-command CMD]

流程:
  1. 后台启动串口 monitor 并把输出写入日志文件
  2. 调用飞书回放校验脚本
  3. 结束 monitor 并保留日志文件

参数:
  --port PORT         串口号，如 /dev/ttyACM0 或 /dev/cu.usbmodem11401
  --url URL           设备 HTTP 服务地址，默认: ${DEFAULT_BASE_URL}
  --scenario NAME     url|text|duplicate|image|file|audio|sticker|all，默认 all
  --verify-token TOK  飞书 Verify Token，默认: ${VERIFY_TOKEN}
  --encrypt-key KEY   飞书 Encrypt Key
  --encrypted         使用加密回调模式
  --log-file PATH     monitor 日志输出路径，默认自动生成到 ${DEFAULT_LOG_DIR}
  --monitor-command   可选：自定义 monitor 命令字符串，用于离线测试脚本编排
  --warmup SEC        monitor 启动后等待秒数，默认: ${MONITOR_WARMUP}
  -h, --help          显示帮助
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="$2"
      shift 2
      ;;
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
    --monitor-command)
      MONITOR_COMMAND="$2"
      shift 2
      ;;
    --warmup)
      MONITOR_WARMUP="$2"
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

if [[ -z "${PORT}" && -z "${MONITOR_COMMAND}" ]]; then
  echo "缺少 --port，或环境变量 ESPPORT 未设置。" >&2
  exit 2
fi

mkdir -p "${DEFAULT_LOG_DIR}"
if [[ -z "${LOG_FILE}" ]]; then
  ts="$(date +%Y%m%d-%H%M%S)"
  LOG_FILE="${DEFAULT_LOG_DIR}/feishu-validate-${ts}.log"
fi

MONITOR_CMD=(
  python3 "${ROOT_DIR}/tools/serial_monitor_log.py"
  --log-file "${LOG_FILE}"
  --quiet
)
if [[ -n "${MONITOR_COMMAND}" ]]; then
  MONITOR_CMD+=(--command sh -c "${MONITOR_COMMAND}")
else
  MONITOR_CMD+=(--port "${PORT}")
fi

VALIDATE_CMD=(
  python3 "${ROOT_DIR}/tools/feishu_callback_validate.py"
  --base-url "${BASE_URL}"
  --scenario "${SCENARIO}"
  --verify-token "${VERIFY_TOKEN}"
  --log-file "${LOG_FILE}"
)

if [[ -n "${ENCRYPT_KEY}" ]]; then
  VALIDATE_CMD+=(--encrypt-key "${ENCRYPT_KEY}")
fi
if [[ ${ENCRYPTED} -eq 1 ]]; then
  VALIDATE_CMD+=(--encrypted)
fi

MON_PID=""
cleanup() {
  if [[ -n "${MON_PID}" ]] && kill -0 "${MON_PID}" 2>/dev/null; then
    kill -INT "${MON_PID}" 2>/dev/null || true
    wait "${MON_PID}" 2>/dev/null || true
  fi
  echo "monitor 日志保留在: ${LOG_FILE}"
}
trap cleanup EXIT

echo "启动后台 monitor: port=${PORT:-custom} log=${LOG_FILE}"
"${MONITOR_CMD[@]}" &
MON_PID="$!"
sleep "${MONITOR_WARMUP}"

echo "开始飞书回放校验: scenario=${SCENARIO} base_url=${BASE_URL}"
"${VALIDATE_CMD[@]}"
