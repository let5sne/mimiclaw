#!/usr/bin/env python3
"""
飞书回调本地回放工具。

用途：
1. 在不依赖飞书开放平台的情况下，直接向设备的 /feishu/events 回放明文回调
2. 验证 Encrypt Key 模式下的签名校验与解密
3. 触发重复投递场景，检查 event_id/message_id 去重
4. 验证图片 / 文件 / 语音等非文本消息的摘要降级路径

示例：
  python3 tools/feishu_callback_replay.py \
    --base-url http://127.0.0.1:18789 \
    --verify-token mimiclaw-feishu \
    --scenario text

  python3 tools/feishu_callback_replay.py \
    --base-url http://127.0.0.1:18789 \
    --verify-token mimiclaw-feishu \
    --encrypt-key your_encrypt_key \
    --encrypted \
    --scenario duplicate

  python3 tools/feishu_callback_replay.py \
    --base-url http://127.0.0.1:18789 \
    --verify-token mimiclaw-feishu \
    --scenario all
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import secrets
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    Cipher = None
    algorithms = None
    modes = None

try:
    from Crypto.Cipher import AES as CryptoAES
except ImportError:
    CryptoAES = None


@dataclass
class ReplayCase:
    name: str
    payload: dict[str, Any]
    encrypted: bool
    event_id: str = ""
    message_id: str = ""


@dataclass
class ReplayResult:
    name: str
    status: int
    response: str
    encrypted: bool
    event_id: str = ""
    message_id: str = ""


def compact_json(data: dict[str, Any]) -> str:
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


def sha256_bytes(text: str) -> bytes:
    return hashlib.sha256(text.encode("utf-8")).digest()


def sha256_hex(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def pkcs7_pad(data: bytes, block_size: int = 16) -> bytes:
    pad = block_size - (len(data) % block_size)
    return data + bytes([pad]) * pad


def aes_cbc_encrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    if Cipher is not None and algorithms is not None and modes is not None:
        encryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
        return encryptor.update(data) + encryptor.finalize()
    if CryptoAES is not None:
        cipher = CryptoAES.new(key, CryptoAES.MODE_CBC, iv)
        return cipher.encrypt(data)
    raise RuntimeError(
        "本机缺少 AES-CBC 实现。请安装 cryptography 或 pycryptodome 后再使用 --encrypted。"
    )


def encrypt_payload(inner_payload: dict[str, Any], encrypt_key: str) -> dict[str, Any]:
    if not encrypt_key:
        raise ValueError("encrypted 模式必须提供 --encrypt-key")

    inner_json = compact_json(inner_payload).encode("utf-8")
    aes_key = sha256_bytes(encrypt_key)
    iv = secrets.token_bytes(16)
    padded = pkcs7_pad(inner_json)
    cipher_text = aes_cbc_encrypt(padded, aes_key, iv)
    return {"encrypt": base64.b64encode(iv + cipher_text).decode("ascii")}


def build_signature_headers(body: dict[str, Any], encrypt_key: str) -> dict[str, str]:
    if not encrypt_key:
        return {}

    timestamp = str(int(time.time()))
    nonce = secrets.token_hex(8)
    body_json = compact_json(body)
    signature = sha256_hex(timestamp + nonce + encrypt_key + body_json)
    return {
        "X-Lark-Request-Timestamp": timestamp,
        "X-Lark-Request-Nonce": nonce,
        "X-Lark-Signature": signature,
    }


def now_ms() -> str:
    return str(int(time.time() * 1000))


def build_url_verification(token: str, app_id: str, challenge: str) -> dict[str, Any]:
    return {
        "token": token,
        "type": "url_verification",
        "challenge": challenge,
        "app_id": app_id,
    }


def build_message_event(
    *,
    token: str,
    app_id: str,
    event_id: str,
    message_id: str,
    chat_id: str,
    sender_id: str,
    message_type: str,
    content: dict[str, Any],
) -> dict[str, Any]:
    ts_ms = now_ms()
    return {
        "schema": "2.0",
        "header": {
            "event_id": event_id,
            "event_type": "im.message.receive_v1",
            "create_time": ts_ms,
            "token": token,
            "app_id": app_id,
        },
        "event": {
            "sender": {
                "sender_id": {
                    "open_id": sender_id,
                },
                "sender_type": "user",
                "tenant_key": "tenant_replay",
            },
            "message": {
                "message_id": message_id,
                "root_id": message_id,
                "parent_id": message_id,
                "create_time": ts_ms,
                "chat_id": chat_id,
                "chat_type": "p2p",
                "message_type": message_type,
                "content": compact_json(content),
            },
        },
    }


def build_cases(args: argparse.Namespace) -> list[ReplayCase]:
    event_seed = int(time.time() * 1000)
    cases: list[ReplayCase] = []

    def make_case(name: str, payload: dict[str, Any], encrypted: bool,
                  event_id: str = "", message_id: str = "") -> None:
        if encrypted:
            payload = encrypt_payload(payload, args.encrypt_key)
        cases.append(
            ReplayCase(
                name=name,
                payload=payload,
                encrypted=encrypted,
                event_id=event_id,
                message_id=message_id,
            )
        )

    def add_text_case(name: str, suffix: str, encrypted: bool, text: str) -> None:
        event_id = f"evt_replay_{event_seed}_{suffix}"
        message_id = f"om_replay_{event_seed}_{suffix}"
        payload = build_message_event(
            token=args.verify_token,
            app_id=args.app_id,
            event_id=event_id,
            message_id=message_id,
            chat_id=args.chat_id,
            sender_id=args.sender_id,
            message_type="text",
            content={"text": text},
        )
        make_case(name, payload, encrypted, event_id, message_id)

    def add_media_case(name: str, suffix: str, encrypted: bool,
                       message_type: str, content: dict[str, Any]) -> None:
        event_id = f"evt_replay_{event_seed}_{suffix}"
        message_id = f"om_replay_{event_seed}_{suffix}"
        payload = build_message_event(
            token=args.verify_token,
            app_id=args.app_id,
            event_id=event_id,
            message_id=message_id,
            chat_id=args.chat_id,
            sender_id=args.sender_id,
            message_type=message_type,
            content=content,
        )
        make_case(name, payload, encrypted, event_id, message_id)

    scenario = args.scenario
    encrypted = bool(args.encrypted)

    if encrypted and not args.encrypt_key:
        raise SystemExit("encrypted 模式缺少 --encrypt-key")

    if scenario in ("url", "all"):
        make_case(
            "url-verification",
            build_url_verification(args.verify_token, args.app_id, args.challenge),
            encrypted,
        )

    if scenario in ("text", "all"):
        add_text_case("text", "text", encrypted, args.text)

    if scenario in ("duplicate", "all"):
        event_id = f"evt_replay_{event_seed}_dup"
        message_id = f"om_replay_{event_seed}_dup"
        payload = build_message_event(
            token=args.verify_token,
            app_id=args.app_id,
            event_id=event_id,
            message_id=message_id,
            chat_id=args.chat_id,
            sender_id=args.sender_id,
            message_type="text",
            content={"text": args.duplicate_text},
        )
        make_case("duplicate-1", payload, encrypted, event_id, message_id)
        make_case("duplicate-2", payload, encrypted, event_id, message_id)

    if scenario in ("image", "all"):
        add_media_case(
            "image",
            "image",
            encrypted,
            "image",
            {"image_key": "img_replay_demo"},
        )

    if scenario in ("file", "all"):
        add_media_case(
            "file",
            "file",
            encrypted,
            "file",
            {
                "file_key": "file_replay_demo",
                "file_name": "demo-report.pdf",
                "file_size": 24576,
            },
        )

    if scenario in ("audio", "all"):
        add_media_case(
            "audio",
            "audio",
            encrypted,
            "audio",
            {
                "file_key": "audio_replay_demo",
                "duration": 7,
            },
        )

    if scenario in ("sticker", "all"):
        add_media_case(
            "sticker",
            "sticker",
            encrypted,
            "sticker",
            {"file_key": "sticker_replay_demo"},
        )

    return cases


def build_url(base_url: str, path: str) -> str:
    root = base_url.rstrip("/")
    if root.endswith(path):
        return root
    if not path.startswith("/"):
        path = "/" + path
    return root + path


def post_json(url: str, body: dict[str, Any], headers: dict[str, str]) -> tuple[int, str]:
    data = compact_json(body).encode("utf-8")
    req = urllib.request.Request(url=url, data=data, method="POST")
    req.add_header("Content-Type", "application/json; charset=utf-8")
    for key, value in headers.items():
        req.add_header(key, value)

    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            status = int(getattr(resp, "status", resp.getcode()))
            text = resp.read().decode("utf-8", errors="replace")
            return status, text
    except urllib.error.HTTPError as e:
        text = e.read().decode("utf-8", errors="replace") if hasattr(e, "read") else str(e)
        return int(e.code), text


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="飞书回调本地回放工具")
    parser.add_argument("--base-url", default="http://127.0.0.1:18789", help="设备 HTTP 服务地址")
    parser.add_argument("--path", default="/feishu/events", help="飞书回调路径")
    parser.add_argument(
        "--scenario",
        default="all",
        choices=["url", "text", "duplicate", "image", "file", "audio", "sticker", "all"],
        help="要回放的场景",
    )
    parser.add_argument("--verify-token", default="mimiclaw-feishu", help="飞书 Verify Token")
    parser.add_argument("--encrypt-key", default="", help="飞书 Encrypt Key")
    parser.add_argument("--encrypted", action="store_true", help="使用加密回调模式")
    parser.add_argument("--app-id", default="cli_replay", help="回放时写入的 app_id")
    parser.add_argument("--chat-id", default="oc_replay_chat", help="回放时写入的 chat_id")
    parser.add_argument("--sender-id", default="ou_replay_user", help="回放时写入的 sender open_id")
    parser.add_argument("--text", default="hello from feishu replay", help="text 场景消息内容")
    parser.add_argument(
        "--duplicate-text",
        default="duplicate delivery check",
        help="duplicate 场景消息内容",
    )
    parser.add_argument(
        "--challenge",
        default="challenge_replay_demo",
        help="url 场景 challenge 内容",
    )
    parser.add_argument(
        "--show-body",
        action="store_true",
        help="打印请求和响应体，便于对照调试",
    )
    parser.add_argument(
        "--json-output",
        default="",
        help="把结构化结果写入 JSON 文件",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    url = build_url(args.base_url, args.path)
    cases = build_cases(args)
    if not cases:
        print("没有可执行回放用例。")
        return 2

    print(f"开始回放: endpoint={url} cases={len(cases)} encrypted={'yes' if args.encrypted else 'no'}")
    failures = 0
    results: list[ReplayResult] = []

    for idx, case in enumerate(cases, start=1):
        headers = build_signature_headers(case.payload, args.encrypt_key if case.encrypted else "")
        status, body = post_json(url, case.payload, headers)
        ok = 200 <= status < 300
        if not ok:
            failures += 1
        results.append(
            ReplayResult(
                name=case.name,
                status=status,
                response=body,
                encrypted=case.encrypted,
                event_id=case.event_id,
                message_id=case.message_id,
            )
        )

        print(
            f"[{idx}/{len(cases)}] {case.name}: "
            f"status={status} event_id={case.event_id or '-'} message_id={case.message_id or '-'}"
        )
        if args.show_body:
            print(f"  request={compact_json(case.payload)}")
            print(f"  response={body}")
        elif body:
            print(f"  response={body}")

    if args.json_output:
        report = {
            "endpoint": url,
            "scenario": args.scenario,
            "encrypted": bool(args.encrypted),
            "failures": failures,
            "total": len(results),
            "results": [
                {
                    "name": item.name,
                    "status": item.status,
                    "response": item.response,
                    "encrypted": item.encrypted,
                    "event_id": item.event_id,
                    "message_id": item.message_id,
                }
                for item in results
            ],
        }
        with open(args.json_output, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)

    print(f"结果: pass={len(cases) - failures}, fail={failures}, total={len(cases)}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
