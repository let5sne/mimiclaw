#!/usr/bin/env python3
"""
飞书 OpenAPI 本地桩服务。

用途：
1. 为设备提供 tenant_access_token 获取接口
2. 为 image/file 回放场景提供消息资源下载接口
3. 可选接收设备的飞书出站 send_message 请求，便于本地联调

默认资源：
- image_key=img_replay_demo  -> 1x1 PNG
- file_key=file_replay_demo  -> 内置 PDF
"""

from __future__ import annotations

import argparse
import base64
import json
import re
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse


PNG_BYTES = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4////fwAJ+wP9KobjigAAAABJRU5ErkJggg=="
)


def build_pdf_bytes(text: str) -> bytes:
    stream = f"BT /F1 18 Tf 24 96 Td ({text}) Tj ET".encode("latin-1", errors="replace")
    objects = [
        b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
        b"2 0 obj\n<< /Type /Pages /Count 1 /Kids [3 0 R] >>\nendobj\n",
        b"3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 140] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n",
        b"4 0 obj\n<< /Length %d >>\nstream\n%s\nendstream\nendobj\n" % (len(stream), stream),
        b"5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n",
    ]

    out = [b"%PDF-1.4\n"]
    offsets = [0]
    current = len(out[0])
    for obj in objects:
        offsets.append(current)
        out.append(obj)
        current += len(obj)

    xref_offset = current
    xref = [f"xref\n0 {len(offsets)}\n".encode("ascii"), b"0000000000 65535 f \n"]
    for offset in offsets[1:]:
        xref.append(f"{offset:010d} 00000 n \n".encode("ascii"))
    trailer = (
        f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\n"
        f"startxref\n{xref_offset}\n%%EOF\n"
    ).encode("ascii")
    out.extend(xref)
    out.append(trailer)
    return b"".join(out)


DEFAULT_FILE_BYTES = build_pdf_bytes("Hello Feishu PDF")


class StubState:
    def __init__(self, token: str) -> None:
        self.token = token
        self.resources: dict[str, tuple[bytes, str]] = {
            "img_replay_demo": (PNG_BYTES, "image/png"),
            "file_replay_demo": (DEFAULT_FILE_BYTES, "application/pdf"),
        }
        self.outbound_messages: list[dict[str, Any]] = []


class FeishuStubHandler(BaseHTTPRequestHandler):
    server_version = "FeishuOpenAPIStub/0.1"

    @property
    def state(self) -> StubState:
        return self.server.state  # type: ignore[attr-defined]

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"[stub] {self.address_string()} - {fmt % args}")

    def _send_json(self, body: dict[str, Any], status: int = 200) -> None:
        data = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length) if length > 0 else b"{}"
        return json.loads(raw.decode("utf-8", errors="replace") or "{}")

    def _require_auth(self) -> bool:
        auth = self.headers.get("Authorization", "")
        expected = f"Bearer {self.state.token}"
        if auth != expected:
            self._send_json({"code": 99991663, "msg": "unauthorized"}, status=401)
            return False
        return True

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/open-apis/auth/v3/tenant_access_token/internal":
            _ = self._read_json()
            self._send_json(
                {
                    "code": 0,
                    "msg": "ok",
                    "tenant_access_token": self.state.token,
                    "expire": 7200,
                }
            )
            return

        if parsed.path == "/open-apis/im/v1/messages":
            if not self._require_auth():
                return
            query = parse_qs(parsed.query)
            body = self._read_json()
            self.state.outbound_messages.append(
                {
                    "query": query,
                    "body": body,
                }
            )
            self._send_json(
                {
                    "code": 0,
                    "msg": "ok",
                    "data": {"message_id": "om_stub_outbound"},
                }
            )
            return

        self._send_json({"code": 404, "msg": f"unknown path: {parsed.path}"}, status=404)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/healthz":
            self._send_json({"ok": True, "resources": sorted(self.state.resources)})
            return

        match = re.fullmatch(r"/open-apis/im/v1/messages/([^/]+)/resources/([^/]+)", parsed.path)
        if match:
            if not self._require_auth():
                return
            _message_id, file_key = match.groups()
            data = self.state.resources.get(file_key)
            if not data:
                self._send_json({"code": 404, "msg": f"resource not found: {file_key}"}, status=404)
                return
            payload, mime = data
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        self._send_json({"code": 404, "msg": f"unknown path: {parsed.path}"}, status=404)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="飞书 OpenAPI 本地桩服务")
    parser.add_argument("--host", default="127.0.0.1", help="监听地址")
    parser.add_argument("--port", type=int, default=19091, help="监听端口")
    parser.add_argument("--token", default="stub_tenant_token", help="返回给设备的 tenant_access_token")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    server = ThreadingHTTPServer((args.host, args.port), FeishuStubHandler)
    server.state = StubState(token=args.token)  # type: ignore[attr-defined]
    print(f"飞书 OpenAPI stub 已启动: http://{args.host}:{args.port}")
    print("内置资源: image_key=img_replay_demo, file_key=file_replay_demo")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n收到中断，正在退出。")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
