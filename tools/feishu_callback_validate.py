#!/usr/bin/env python3
"""
飞书回放校验工具。

职责：
1. 调用 feishu_callback_replay.py 执行回放
2. 校验 URL verification / 普通事件 HTTP 返回
3. 可选读取串口日志文件，校验去重与媒体摘要链路
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import time
from typing import Any


def read_text_tail(path: pathlib.Path, start_offset: int) -> str:
    with path.open("r", encoding="utf-8", errors="replace") as f:
        f.seek(start_offset)
        return f.read()


def build_command(args: argparse.Namespace, json_output: str) -> list[str]:
    cmd = [
        sys.executable,
        str(pathlib.Path(args.replay_script)),
        "--base-url", args.base_url,
        "--scenario", args.scenario,
        "--verify-token", args.verify_token,
        "--json-output", json_output,
    ]
    if args.encrypt_key:
        cmd.extend(["--encrypt-key", args.encrypt_key])
    if args.encrypted:
        cmd.append("--encrypted")
    if args.app_id:
        cmd.extend(["--app-id", args.app_id])
    if args.chat_id:
        cmd.extend(["--chat-id", args.chat_id])
    if args.sender_id:
        cmd.extend(["--sender-id", args.sender_id])
    if args.text:
        cmd.extend(["--text", args.text])
    if args.duplicate_text:
        cmd.extend(["--duplicate-text", args.duplicate_text])
    if args.challenge:
        cmd.extend(["--challenge", args.challenge])
    return cmd


def expect_http(report: dict[str, Any], challenge: str) -> list[str]:
    errs: list[str] = []
    results = report.get("results", [])
    for item in results:
        name = str(item.get("name", ""))
        status = int(item.get("status", 0))
        response = str(item.get("response", ""))
        if status != 200:
            errs.append(f"{name}: HTTP 状态码不是 200，而是 {status}")
            continue

        if name == "url-verification":
            try:
                body = json.loads(response or "{}")
            except json.JSONDecodeError:
                errs.append("url-verification: 响应不是合法 JSON")
                continue
            if str(body.get("challenge", "")) != challenge:
                errs.append(
                    f"url-verification: challenge 不匹配 got={body.get('challenge')} expect={challenge}"
                )
        else:
            try:
                body = json.loads(response or "{}")
            except json.JSONDecodeError:
                errs.append(f"{name}: 响应不是合法 JSON")
                continue
            if int(body.get("code", -1)) != 0:
                errs.append(f"{name}: 响应 code 不是 0，而是 {body.get('code')}")
    return errs


def expect_logs(report: dict[str, Any], log_text: str, scenario: str, expect_media_mode: str) -> list[str]:
    errs: list[str] = []
    if not log_text:
        return errs

    case_names = {str(item.get("name", "")) for item in report.get("results", [])}

    def require(substr: str, label: str) -> None:
        if substr not in log_text:
            errs.append(f"日志缺少关键字: {label}")

    def require_any(substrings: tuple[str, ...], label: str) -> None:
        if not any(substr in log_text for substr in substrings):
            errs.append(f"日志缺少关键字: {label}")

    if scenario in ("text", "all"):
        require("Feishu text from", "文本入站日志")
    if scenario in ("duplicate", "all"):
        require("Skip duplicate Feishu event", "重复事件去重日志")

    image_or_file = bool({"image", "file"} & case_names)
    summary_only_media = bool({"audio", "sticker"} & case_names)

    if expect_media_mode == "gateway":
        if image_or_file:
            require("gateway_parse from", "image/file gateway 解析日志")
        if summary_only_media:
            require("summary from", "audio/sticker 摘要日志")
    elif expect_media_mode == "summary":
        if scenario in ("image", "file", "audio", "sticker", "all"):
            require("summary from", "媒体摘要入站日志")
    else:
        if image_or_file:
            require_any(("summary from", "gateway_parse from"), "image/file 入站日志")
        if summary_only_media:
            require("summary from", "audio/sticker 摘要日志")

    return errs


def parse_args() -> argparse.Namespace:
    root = pathlib.Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="飞书回放校验工具")
    parser.add_argument("--base-url", default="http://127.0.0.1:18789", help="设备 HTTP 服务地址")
    parser.add_argument(
        "--scenario",
        default="all",
        choices=["url", "text", "duplicate", "image", "file", "audio", "sticker", "all"],
        help="回放场景",
    )
    parser.add_argument("--verify-token", default="mimiclaw-feishu", help="飞书 Verify Token")
    parser.add_argument("--encrypt-key", default="", help="飞书 Encrypt Key")
    parser.add_argument("--encrypted", action="store_true", help="使用加密回调模式")
    parser.add_argument("--app-id", default="cli_replay", help="回放 app_id")
    parser.add_argument("--chat-id", default="oc_replay_chat", help="回放 chat_id")
    parser.add_argument("--sender-id", default="ou_replay_user", help="回放 sender open_id")
    parser.add_argument("--text", default="hello from feishu replay", help="text 场景消息内容")
    parser.add_argument(
        "--duplicate-text",
        default="duplicate delivery check",
        help="duplicate 场景消息内容",
    )
    parser.add_argument(
        "--challenge",
        default="challenge_replay_demo",
        help="url 场景 challenge",
    )
    parser.add_argument(
        "--log-file",
        default="",
        help="可选：串口日志文件路径。提供后会校验文本/去重/媒体摘要关键日志。",
    )
    parser.add_argument(
        "--expect-media-mode",
        default="auto",
        choices=["auto", "summary", "gateway"],
        help="媒体日志期望模式：auto=接受 summary/gateway，summary=强制摘要，gateway=要求 image/file 走 gateway",
    )
    parser.add_argument(
        "--log-wait-ms",
        type=int,
        default=1500,
        help="请求发送完成后等待日志刷新的毫秒数",
    )
    parser.add_argument(
        "--replay-script",
        default=str(root / "feishu_callback_replay.py"),
        help="回放脚本路径",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    log_start = 0
    log_path = pathlib.Path(args.log_file) if args.log_file else None
    if log_path:
        if not log_path.exists():
            print(f"日志文件不存在: {log_path}", file=sys.stderr)
            return 2
        log_start = log_path.stat().st_size

    with tempfile.NamedTemporaryFile(prefix="feishu_replay_", suffix=".json", delete=False) as tmp:
        json_path = tmp.name

    cmd = build_command(args, json_path)
    print("执行回放校验:")
    print("  " + " ".join(cmd))
    proc = subprocess.run(cmd, text=True)
    if proc.returncode != 0:
        print("回放脚本返回非 0，继续读取 JSON 报告做细化诊断。")

    report_path = pathlib.Path(json_path)
    if not report_path.exists():
        print("缺少 JSON 报告输出，无法继续校验。", file=sys.stderr)
        return 2

    report = json.loads(report_path.read_text(encoding="utf-8"))
    report_path.unlink(missing_ok=True)

    errs = expect_http(report, args.challenge)

    log_text = ""
    if log_path:
        time.sleep(max(args.log_wait_ms, 0) / 1000.0)
        log_text = read_text_tail(log_path, log_start)
        errs.extend(expect_logs(report, log_text, args.scenario, args.expect_media_mode))

    if errs:
        print("校验失败：")
        for err in errs:
            print(f"  - {err}")
        return 1

    print("校验通过。")
    if log_path:
        print(f"已校验日志文件新增内容: {log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
