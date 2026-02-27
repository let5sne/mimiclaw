#!/usr/bin/env python3
"""
文档解析回归测试脚本（调用 voice_gateway 的 /doc_upload）。

用法示例：
  python3 tools/doc_regression.py \
    --manifest tools/doc_regression_manifest.example.json \
    --base-url http://127.0.0.1:8091

manifest 示例字段：
[
  {
    "file": "tests/data/food.xlsx",
    "expect_format": "xlsx",
    "min_text_len": 80,
    "must_contain": ["店名", "区域"],
    "max_latency_ms": 6000,
    "optional": false
  }
]
"""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import pathlib
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


@dataclass
class CaseSpec:
    file: str
    expect_format: str = ""
    min_text_len: int = 20
    must_contain: list[str] | None = None
    max_latency_ms: int = 15000
    doc_name: str = ""
    doc_mime: str = ""
    doc_format: str = ""
    expect_parser_prefix: str = ""
    optional: bool = False


def infer_doc_format(path: str) -> str:
    ext = pathlib.Path(path).suffix.lower().lstrip(".")
    return ext if ext else "bin"


def infer_doc_mime(path: str) -> str:
    mime, _ = mimetypes.guess_type(path)
    return mime or "application/octet-stream"


def load_manifest(path: str) -> list[CaseSpec]:
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    if not isinstance(raw, list):
        raise ValueError("manifest 顶层必须是数组")

    out: list[CaseSpec] = []
    for idx, item in enumerate(raw, start=1):
        if not isinstance(item, dict):
            raise ValueError(f"manifest 第 {idx} 项必须是对象")
        file_path = str(item.get("file", "")).strip()
        if not file_path:
            raise ValueError(f"manifest 第 {idx} 项缺少 file")
        must = item.get("must_contain", [])
        if must is None:
            must = []
        if not isinstance(must, list):
            raise ValueError(f"manifest 第 {idx} 项 must_contain 必须是数组")
        out.append(
            CaseSpec(
                file=file_path,
                expect_format=str(item.get("expect_format", "")).strip().lower(),
                min_text_len=int(item.get("min_text_len", 20)),
                must_contain=[str(x) for x in must],
                max_latency_ms=int(item.get("max_latency_ms", 15000)),
                doc_name=str(item.get("doc_name", "")).strip(),
                doc_mime=str(item.get("doc_mime", "")).strip(),
                doc_format=str(item.get("doc_format", "")).strip().lower(),
                expect_parser_prefix=str(item.get("expect_parser_prefix", "")).strip().lower(),
                optional=bool(item.get("optional", False)),
            )
        )
    return out


def post_doc_upload(base_url: str, case: CaseSpec) -> tuple[int, dict[str, Any], int]:
    doc_path = pathlib.Path(case.file)
    if not doc_path.exists():
        raise FileNotFoundError(f"文件不存在: {case.file}")
    payload = doc_path.read_bytes()

    doc_name = case.doc_name or doc_path.name
    doc_mime = case.doc_mime or infer_doc_mime(str(doc_path))
    doc_format = case.doc_format or infer_doc_format(str(doc_path))

    url = base_url.rstrip("/") + "/doc_upload"
    req = urllib.request.Request(url=url, data=payload, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    req.add_header("X-Doc-Name", doc_name)
    req.add_header("X-Doc-Mime", doc_mime)
    req.add_header("X-Doc-Path", str(doc_path).replace("\\", "/"))
    req.add_header("X-Doc-Format", doc_format)

    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=90) as resp:
            status = int(getattr(resp, "status", resp.getcode()))
            body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        status = int(e.code)
        body = e.read().decode("utf-8", errors="replace") if hasattr(e, "read") else str(e)
    latency_ms = int((time.perf_counter() - t0) * 1000)

    try:
        data = json.loads(body) if body else {}
    except json.JSONDecodeError:
        data = {"_raw": body}
    return status, data, latency_ms


def validate_case(case: CaseSpec, status: int, data: dict[str, Any], latency_ms: int) -> list[str]:
    errs: list[str] = []
    if status != 200:
        errs.append(f"HTTP 状态码异常: {status}")
        return errs

    if not isinstance(data, dict):
        errs.append("返回不是 JSON 对象")
        return errs

    if "error" in data:
        errs.append(f"接口返回 error: {data.get('error')}")
        return errs

    doc_format = str(data.get("doc_format", "")).strip().lower()
    parser = str(data.get("parser", "")).strip().lower()
    text_len = int(data.get("text_len", 0) or 0)
    text_blob = (
        str(data.get("text", "")) + "\n" +
        str(data.get("summary", "")) + "\n" +
        str(data.get("excerpt", ""))
    )

    if case.expect_format and doc_format != case.expect_format:
        errs.append(f"doc_format 不匹配: got={doc_format}, expect={case.expect_format}")

    if text_len < case.min_text_len:
        errs.append(f"text_len 太短: got={text_len}, expect>={case.min_text_len}")

    if case.expect_parser_prefix and not parser.startswith(case.expect_parser_prefix):
        errs.append(
            f"parser 前缀不匹配: got={parser}, expect_prefix={case.expect_parser_prefix}"
        )

    for kw in (case.must_contain or []):
        if kw and kw not in text_blob:
            errs.append(f"缺少关键词: {kw}")

    if case.max_latency_ms > 0 and latency_ms > case.max_latency_ms:
        errs.append(
            f"耗时超阈值: got={latency_ms}ms, expect<={case.max_latency_ms}ms"
        )

    return errs


def run(manifest: str, base_url: str) -> int:
    cases = load_manifest(manifest)
    if not cases:
        print("⚠️ manifest 为空，没有可执行用例")
        return 2

    print(f"开始回归: cases={len(cases)}, endpoint={base_url.rstrip('/')}/doc_upload")
    failed = 0
    skipped = 0

    for i, case in enumerate(cases, start=1):
        print(f"\n[{i}/{len(cases)}] {case.file}")
        if not pathlib.Path(case.file).exists():
            if case.optional:
                skipped += 1
                print("  ⏭️ SKIP (optional 且文件不存在)")
                continue
            failed += 1
            print("  ❌ FAIL (文件不存在)")
            continue

        try:
            status, data, latency_ms = post_doc_upload(base_url, case)
        except Exception as e:
            failed += 1
            print(f"  ❌ 请求失败: {e}")
            continue

        errs = validate_case(case, status, data, latency_ms)
        if errs:
            failed += 1
            print(f"  ❌ FAIL ({latency_ms}ms)")
            for e in errs:
                print(f"     - {e}")
        else:
            fmt = data.get("doc_format", "")
            parser = data.get("parser", "")
            tlen = data.get("text_len", 0)
            print(f"  ✅ PASS ({latency_ms}ms) format={fmt} parser={parser} text_len={tlen}")

    passed = len(cases) - failed - skipped
    print(f"\n结果: pass={passed}, fail={failed}, skip={skipped}, total={len(cases)}")
    return 0 if failed == 0 else 1


def main():
    parser = argparse.ArgumentParser(description="doc_upload 文档解析回归测试")
    parser.add_argument(
        "--manifest",
        default="tools/doc_regression_manifest.example.json",
        help="回归用例 JSON 清单路径",
    )
    parser.add_argument(
        "--base-url",
        default=os.environ.get("DOC_REGRESSION_BASE_URL", "http://127.0.0.1:8091"),
        help="voice_gateway HTTP 基地址（默认 http://127.0.0.1:8091）",
    )
    args = parser.parse_args()

    rc = run(args.manifest, args.base_url)
    raise SystemExit(rc)


if __name__ == "__main__":
    main()
