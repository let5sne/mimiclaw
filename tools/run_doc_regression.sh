#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_BASE_URL="${DOC_REGRESSION_BASE_URL:-http://127.0.0.1:8091}"
MANIFEST_BASIC="${ROOT_DIR}/tools/doc_regression_manifest.example.json"
MANIFEST_OFFICE="${ROOT_DIR}/tools/doc_regression_manifest.office.example.json"

BASE_URL="${DEFAULT_BASE_URL}"
MANIFEST="${MANIFEST_BASIC}"

print_help() {
  cat <<EOF
用法:
  ./tools/run_doc_regression.sh [--basic|--office] [--manifest PATH] [--base-url URL]

参数:
  --basic            使用基础样本清单（txt/csv），默认
  --office           使用 Office 清单（xlsx + 可选 xls）
  --manifest PATH    自定义 manifest 路径
  --base-url URL     指定网关地址，默认: ${DEFAULT_BASE_URL}
  -h, --help         显示帮助
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --basic)
      MANIFEST="${MANIFEST_BASIC}"
      shift
      ;;
    --office)
      MANIFEST="${MANIFEST_OFFICE}"
      shift
      ;;
    --manifest)
      MANIFEST="$2"
      shift 2
      ;;
    --base-url)
      BASE_URL="$2"
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

if [[ ! -f "${MANIFEST}" ]]; then
  echo "manifest 不存在: ${MANIFEST}" >&2
  exit 2
fi

echo "运行文档回归: manifest=${MANIFEST} base_url=${BASE_URL}"
python3 "${ROOT_DIR}/tools/doc_regression.py" \
  --manifest "${MANIFEST}" \
  --base-url "${BASE_URL}"
