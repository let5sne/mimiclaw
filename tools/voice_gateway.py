#!/usr/bin/env python3
"""
MimiClaw Voice Gateway Server (WebSocket Streaming)

WebSocket server bridging ESP32 audio with STT/TTS services.
Protocol: JSON control messages + binary PCM frames over a single connection.

Client → Server:
  {"type":"audio_start"}          — begin STT session
  [binary frames]                 — raw PCM 16kHz 16-bit mono chunks
  {"type":"audio_end"}            — end recording, trigger STT
  {"type":"tts_request","text":"..."}  — request streaming TTS
  {"type":"interrupt"}            — cancel ongoing TTS

Server → Client:
  {"type":"stt_result","text":"...","language":"zh"}
  {"type":"tts_start"}
  [binary frames]                 — PCM 16kHz 16-bit mono chunks
  {"type":"tts_end"}
  {"type":"error","message":"..."}

Usage:
  pip install -r requirements.txt
  python voice_gateway.py [--host 0.0.0.0] [--port 8090] [--stt-port 8091] [--model small]
"""

import argparse
import asyncio
import base64
import html
import io
import json
import logging
import os
import posixpath
import re
import subprocess
import tempfile
import threading
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import websockets
from faster_whisper import WhisperModel
from pydub import AudioSegment

log = logging.getLogger("voice_gw")

whisper_model = None  # initialized in main
vision_cfg = {
    "enabled": False,
    "endpoint": "",
    "api_key": "",
    "model": "",
    "api_version": "2023-06-01",
    "timeout_s": 45,
    "prompt": (
        "请分析图片并严格输出一个 JSON 对象，不要输出 markdown 代码块。"
        "JSON 键固定为：caption、ocr_text、objects。"
        "caption 为一句中文描述；ocr_text 为图中文字（没有则空字符串）；"
        "objects 为关键元素中文短语数组。"
    ),
    "http_proxy": "",
}

DOC_MAX_TEXT = 12000
DOC_OCR_FALLBACK_MIN_LEN = 80
DOC_OCR_MAX_PAGES = 4
DOC_OCR_PROMPT = (
    "你正在做文档OCR。请严格输出一个 JSON 对象，不要输出 markdown。"
    "键固定为：caption、ocr_text、objects。"
    "caption 用一句中文概括该页；ocr_text 提取该页可见文字；objects 可留空数组。"
)


def _read_define(path: str, name: str) -> str:
    if not path or not os.path.exists(path):
        return ""
    pattern = re.compile(rf'^\s*#define\s+{re.escape(name)}\s+"(.*)"\s*$')
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                m = pattern.match(line.rstrip("\n"))
                if m:
                    return m.group(1).strip()
    except Exception:
        return ""
    return ""


def load_vision_defaults_from_secrets(path: str) -> dict:
    return {
        "api_key": _read_define(path, "MIMI_SECRET_API_KEY"),
        "endpoint": _read_define(path, "MIMI_SECRET_API_ENDPOINT"),
        "model": _read_define(path, "MIMI_SECRET_MODEL"),
    }


def pcm_to_wav_bytes(pcm_data: bytes, sample_rate=16000, sample_width=2, channels=1) -> bytes:
    """Wrap raw PCM in a WAV header for whisper ingestion."""
    import wave
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sample_width)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_data)
    return buf.getvalue()


def normalize_audio(pcm_data: bytes) -> bytes:
    """Normalize audio level before transcription (target -3 dBFS)."""
    audio_seg = AudioSegment(data=pcm_data, sample_width=2, frame_rate=16000, channels=1)
    peak_dbfs = audio_seg.max_dBFS
    log.info("STT: input peak=%.1f dBFS", peak_dbfs)
    if peak_dbfs < -6.0:
        gain = -3.0 - peak_dbfs
        audio_seg = audio_seg.apply_gain(gain)
        log.info("STT: applied +%.1f dB gain", gain)
        return audio_seg.raw_data
    return pcm_data


def clean_text_for_tts(text: str) -> str:
    """Strip emoji and non-speakable characters, keep CJK + ASCII + punctuation."""
    import re
    return re.sub(
        r'[^\u4e00-\u9fff\u3000-\u303f\uff00-\uffef'
        r'a-zA-Z0-9\s.,!?;:\'\"()\-\u3002\uff0c\uff01\uff1f\uff1b\uff1a\u201c\u201d\u2018\u2019]',
        '', text).strip()


def do_stt(pcm_data: bytes) -> tuple[str, str]:
    """Run whisper on PCM data. Returns (text, language)."""
    pcm_data = normalize_audio(pcm_data)
    wav_bytes = pcm_to_wav_bytes(pcm_data)

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=True) as tmp:
        tmp.write(wav_bytes)
        tmp.flush()
        # 第一轮：启用 VAD，减少噪声误识别
        segments, info = whisper_model.transcribe(
            tmp.name,
            language="zh",
            beam_size=5,
            vad_filter=True,
            initial_prompt="以下是普通话的句子。",
        )
        text = "".join(seg.text for seg in segments).strip()
        lang = info.language

        # 兜底：若 VAD 把整段语音都裁掉，回退到无 VAD 再识别一次
        if not text:
            log.warning("STT empty with VAD enabled, retrying without VAD")
            segments2, info2 = whisper_model.transcribe(
                tmp.name,
                language="zh",
                beam_size=5,
                vad_filter=False,
                initial_prompt="以下是普通话的句子。",
            )
            text = "".join(seg.text for seg in segments2).strip()
            lang = info2.language

    if not text:
        text = "我刚才没听清，请再说一遍。"
        lang = "zh"
        log.warning("STT still empty after retry, using fallback text")

    log.info("STT result [%s]: %s", lang, text)
    return text, lang


def do_stt_encoded(audio_data: bytes, audio_format: str) -> tuple[str, str]:
    """Decode compressed audio bytes to PCM16k and run STT."""
    fmt = (audio_format or "ogg").lower()
    if fmt == "oga":
        fmt = "ogg"

    audio_seg = AudioSegment.from_file(io.BytesIO(audio_data), format=fmt)
    audio_seg = audio_seg.set_frame_rate(16000).set_channels(1).set_sample_width(2)
    return do_stt(audio_seg.raw_data)


def infer_image_format(image_data: bytes, declared: str) -> str:
    fmt = (declared or "").strip().lower()
    if fmt in ("jpg", "jpeg", "png", "webp", "bmp"):
        return "jpeg" if fmt == "jpg" else fmt
    if image_data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "png"
    if image_data.startswith(b"\xff\xd8\xff"):
        return "jpeg"
    if image_data.startswith(b"RIFF") and b"WEBP" in image_data[:16]:
        return "webp"
    if image_data.startswith(b"BM"):
        return "bmp"
    return "jpeg"


def extract_response_text(resp_json: dict) -> str:
    content = resp_json.get("content")
    if not isinstance(content, list):
        return ""
    parts = []
    for block in content:
        if not isinstance(block, dict):
            continue
        if block.get("type") == "text":
            text = block.get("text")
            if isinstance(text, str) and text.strip():
                parts.append(text.strip())
    return "\n".join(parts).strip()


def parse_vision_structured_text(text: str) -> dict:
    raw = (text or "").strip()
    if not raw:
        return {"caption": "", "ocr_text": "", "objects": []}

    cleaned = re.sub(r"^```(?:json)?\s*", "", raw, flags=re.IGNORECASE)
    cleaned = re.sub(r"\s*```$", "", cleaned, flags=re.IGNORECASE)
    cleaned = cleaned.strip()

    obj = None
    start = cleaned.find("{")
    end = cleaned.rfind("}")
    candidates = []
    if start != -1 and end != -1 and end > start:
        candidates.append(cleaned[start:end + 1])
    candidates.append(cleaned)

    for cand in candidates:
        try:
            parsed = json.loads(cand)
            if isinstance(parsed, dict):
                obj = parsed
                break
        except Exception:
            continue

    if obj is None:
        return {"caption": raw[:300], "ocr_text": "", "objects": []}

    caption = obj.get("caption", "")
    ocr_text = obj.get("ocr_text", "")
    objects = obj.get("objects", [])

    if not isinstance(caption, str):
        caption = str(caption or "")
    if not isinstance(ocr_text, str):
        ocr_text = str(ocr_text or "")
    if not isinstance(objects, list):
        objects = []
    objects = [str(x).strip() for x in objects if str(x).strip()]

    return {
        "caption": caption.strip(),
        "ocr_text": ocr_text.strip(),
        "objects": objects,
    }


def build_vision_text(result: dict) -> str:
    parts = []
    caption = result.get("caption", "").strip()
    ocr_text = result.get("ocr_text", "").strip()
    objects = result.get("objects", []) or []
    if caption:
        parts.append(f"描述：{caption}")
    if ocr_text:
        parts.append(f"文字：{ocr_text}")
    if objects:
        parts.append("元素：" + "、".join(objects[:12]))
    if not parts:
        return "已收到图片，但未提取到可用信息。"
    return "\n".join(parts)


def do_vision(image_data: bytes, image_format: str, user_prompt: str = "") -> dict:
    if not vision_cfg["enabled"]:
        raise RuntimeError("vision disabled")
    if not vision_cfg["endpoint"] or not vision_cfg["api_key"] or not vision_cfg["model"]:
        raise RuntimeError("vision config incomplete")

    fmt = infer_image_format(image_data, image_format)
    media_type = "image/jpeg" if fmt == "jpeg" else f"image/{fmt}"
    prompt = (user_prompt or "").strip() or vision_cfg["prompt"]
    b64_data = base64.b64encode(image_data).decode("ascii")

    payload = {
        "model": vision_cfg["model"],
        "max_tokens": 512,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "text", "text": prompt},
                {
                    "type": "image",
                    "source": {
                        "type": "base64",
                        "media_type": media_type,
                        "data": b64_data,
                    },
                },
            ],
        }],
    }

    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        vision_cfg["endpoint"], data=data, method="POST",
        headers={
            "Content-Type": "application/json",
            "x-api-key": vision_cfg["api_key"],
            "anthropic-version": vision_cfg["api_version"],
        },
    )
    opener = urllib.request.build_opener()
    proxy = (vision_cfg["http_proxy"] or "").strip()
    if proxy:
        opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({"http": proxy, "https": proxy})
        )

    try:
        with opener.open(req, timeout=vision_cfg["timeout_s"]) as resp:
            status = getattr(resp, "status", resp.getcode())
            body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="replace") if hasattr(e, "read") else str(e)
        raise RuntimeError(f"vision http {e.code}: {detail[:300]}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"vision url error: {e}")

    if status != 200:
        raise RuntimeError(f"vision api status={status} body={body[:300]}")

    try:
        parsed = json.loads(body)
    except json.JSONDecodeError:
        raise RuntimeError(f"vision api invalid json: {body[:120]}")

    text = extract_response_text(parsed)
    if not text:
        raise RuntimeError("vision api empty text result")
    result = parse_vision_structured_text(text)
    if not result.get("caption") and not result.get("ocr_text") and not result.get("objects"):
        result["caption"] = text[:300]
    return result


def infer_doc_format(doc_name: str, doc_mime: str, doc_path: str, declared: str = "") -> str:
    fmt = (declared or "").strip().lower().lstrip(".")
    if fmt:
        return fmt

    def _ext(s: str) -> str:
        base = (s or "").strip().lower()
        if "." not in base:
            return ""
        return base.rsplit(".", 1)[-1]

    for cand in (doc_name, doc_path):
        ext = _ext(cand)
        if ext:
            return ext

    mime = (doc_mime or "").strip().lower()
    if "pdf" in mime:
        return "pdf"
    if "wordprocessingml" in mime:
        return "docx"
    if "msword" in mime:
        return "doc"
    if "spreadsheetml" in mime:
        return "xlsx"
    if "ms-excel.sheet.macroenabled.12" in mime:
        return "xlsm"
    if "ms-excel" in mime:
        return "xls"
    if "json" in mime:
        return "json"
    if "csv" in mime:
        return "csv"
    if "markdown" in mime:
        return "md"
    if "plain" in mime:
        return "txt"
    if "image/jpeg" in mime:
        return "jpeg"
    if "image/png" in mime:
        return "png"
    if "image/webp" in mime:
        return "webp"
    return "bin"


def decode_text_bytes(data: bytes) -> str:
    for enc in ("utf-8", "utf-16", "utf-16le", "utf-16be", "gb18030", "gbk"):
        try:
            return data.decode(enc)
        except Exception:
            continue
    return data.decode("utf-8", errors="replace")


def is_probably_binary(data: bytes) -> bool:
    if not data:
        return False
    sample = data[:4096]
    if b"\x00" in sample:
        return True
    ctrl = 0
    for b in sample:
        if b in (9, 10, 13):  # \t \n \r
            continue
        if b < 32:
            ctrl += 1
    return (ctrl / max(len(sample), 1)) > 0.30


def normalize_doc_text(text: str) -> str:
    if not text:
        return ""
    s = text.replace("\r\n", "\n").replace("\r", "\n")
    s = re.sub(r"\n{3,}", "\n\n", s)
    lines = [ln.strip() for ln in s.split("\n")]
    return "\n".join(lines).strip()


def doc_text_len(text: str) -> int:
    return len(normalize_doc_text(text))


def extract_pdf_text(data: bytes) -> tuple[str, str]:
    try:
        from pypdf import PdfReader  # optional dependency
    except Exception as e:
        raise RuntimeError(f"pdf parser unavailable: {e}")

    reader = PdfReader(io.BytesIO(data))
    out = []
    total = 0
    for page in reader.pages:
        txt = page.extract_text() or ""
        txt = txt.strip()
        if not txt:
            continue
        out.append(txt)
        total += len(txt)
        if total >= DOC_MAX_TEXT:
            break
    return "\n\n".join(out), "pypdf"


def extract_docx_text(data: bytes) -> tuple[str, str]:
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        if "word/document.xml" not in zf.namelist():
            raise RuntimeError("docx missing word/document.xml")
        xml_text = zf.read("word/document.xml").decode("utf-8", errors="replace")

    # 仅提取文本节点，避免引入额外依赖
    chunks = re.findall(r"<w:t[^>]*>(.*?)</w:t>", xml_text, flags=re.DOTALL)
    plain = "".join(html.unescape(x) for x in chunks)
    return plain, "docx-xml"


def extract_pptx_text(data: bytes) -> tuple[str, str]:
    def slide_sort_key(path: str):
        m = re.search(r"slide(\d+)\.xml$", path)
        return int(m.group(1)) if m else 10**9

    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        slide_paths = [n for n in zf.namelist()
                       if n.startswith("ppt/slides/slide") and n.endswith(".xml")]
        slide_paths.sort(key=slide_sort_key)

        if not slide_paths:
            raise RuntimeError("pptx has no slide xml")

        blocks = []
        total = 0
        for idx, path in enumerate(slide_paths, start=1):
            xml_text = zf.read(path).decode("utf-8", errors="replace")
            chunks = re.findall(r"<a:t[^>]*>(.*?)</a:t>", xml_text, flags=re.DOTALL)
            slide_text = "".join(html.unescape(x) for x in chunks).strip()
            if not slide_text:
                continue
            blocks.append(f"[slide {idx}] {slide_text}")
            total += len(slide_text)
            if total >= DOC_MAX_TEXT:
                break

    return "\n\n".join(blocks), "pptx-xml"


def zip_resolve_path(base_path: str, target: str) -> str:
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(base_path), target))
    return resolved.lstrip("/")


def extract_attr_by_suffix(node: ET.Element, suffix: str) -> str:
    for k, v in node.attrib.items():
        if k.endswith(suffix):
            return str(v)
    return ""


def pypdf_image_to_bytes(image_obj) -> tuple[bytes, str]:
    name = ""
    data = None
    if hasattr(image_obj, "name"):
        name = str(getattr(image_obj, "name") or "")
    if hasattr(image_obj, "data"):
        data = getattr(image_obj, "data")
        if callable(data):
            data = data()

    if data is None and isinstance(image_obj, tuple):
        for item in image_obj:
            if isinstance(item, (bytes, bytearray)):
                data = bytes(item)
            elif isinstance(item, str) and not name:
                name = item

    if isinstance(data, bytearray):
        data = bytes(data)
    if not isinstance(data, bytes):
        return b"", name
    return data, name


def extract_pdf_images_for_ocr(data: bytes) -> list[tuple[str, bytes, str]]:
    try:
        from pypdf import PdfReader  # optional dependency
    except Exception:
        return []

    images: list[tuple[str, bytes, str]] = []
    try:
        reader = PdfReader(io.BytesIO(data))
    except Exception:
        return []

    for pidx, page in enumerate(reader.pages, start=1):
        if len(images) >= DOC_OCR_MAX_PAGES:
            break
        page_images = []
        try:
            img_iter = getattr(page, "images", None)
            if img_iter:
                for iidx, image_obj in enumerate(img_iter, start=1):
                    payload, name = pypdf_image_to_bytes(image_obj)
                    if not payload:
                        continue
                    fmt = infer_image_format(payload, name)
                    page_images.append((f"page {pidx} image {iidx}", payload, fmt))
        except Exception:
            continue

        if page_images:
            images.append(page_images[0])
    return images


def extract_pptx_images_for_ocr(data: bytes) -> list[tuple[str, bytes, str]]:
    def slide_sort_key(path: str):
        m = re.search(r"slide(\d+)\.xml$", path)
        return int(m.group(1)) if m else 10**9

    out: list[tuple[str, bytes, str]] = []
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        slide_paths = [n for n in zf.namelist()
                       if n.startswith("ppt/slides/slide") and n.endswith(".xml")]
        slide_paths.sort(key=slide_sort_key)

        for sidx, slide_path in enumerate(slide_paths, start=1):
            if len(out) >= DOC_OCR_MAX_PAGES:
                break

            rels_path = (
                posixpath.dirname(slide_path) + "/_rels/" +
                posixpath.basename(slide_path) + ".rels"
            )
            if rels_path not in zf.namelist():
                continue

            try:
                rels_root = ET.fromstring(zf.read(rels_path))
                slide_root = ET.fromstring(zf.read(slide_path))
            except ET.ParseError:
                continue

            rel_map = {}
            for rel in rels_root.iter():
                if xml_local_name(rel.tag) != "Relationship":
                    continue
                rid = str(rel.attrib.get("Id") or "").strip()
                target = str(rel.attrib.get("Target") or "").strip()
                if rid and target:
                    rel_map[rid] = zip_resolve_path(slide_path, target)

            image_count = 0
            for node in slide_root.iter():
                if xml_local_name(node.tag) != "blip":
                    continue
                rid = extract_attr_by_suffix(node, "embed")
                if not rid:
                    continue
                target = rel_map.get(rid, "")
                if not target or target not in zf.namelist():
                    continue
                try:
                    payload = zf.read(target)
                except Exception:
                    continue
                if not payload:
                    continue
                image_count += 1
                fmt = infer_image_format(payload, target)
                out.append((f"slide {sidx} image {image_count}", payload, fmt))
                break
    return out


def ocr_images_via_vision(images: list[tuple[str, bytes, str]], doc_kind: str) -> tuple[str, str]:
    if not images:
        raise RuntimeError(f"{doc_kind} ocr: no image chunks")
    if not vision_cfg["enabled"]:
        raise RuntimeError("vision disabled")

    blocks = []
    total = 0
    for label, payload, fmt in images[:DOC_OCR_MAX_PAGES]:
        prompt = f"{DOC_OCR_PROMPT}\n当前页标签：{label}"
        result = do_vision(payload, fmt, user_prompt=prompt)
        ocr_text = (result.get("ocr_text") or "").strip()
        caption = (result.get("caption") or "").strip()
        page_text = ocr_text if ocr_text else caption
        if not page_text:
            continue
        block = f"[{label}] {page_text}"
        blocks.append(block)
        total += len(block)
        if total >= DOC_MAX_TEXT:
            break

    if not blocks:
        raise RuntimeError(f"{doc_kind} ocr empty")
    return "\n\n".join(blocks), f"{doc_kind}-ocr-vision"


def xml_local_name(tag: str) -> str:
    if "}" in tag:
        return tag.rsplit("}", 1)[-1]
    return tag


def xml_child(node: ET.Element, name: str) -> ET.Element | None:
    for child in list(node):
        if xml_local_name(child.tag) == name:
            return child
    return None


def xml_desc_text(node: ET.Element | None, name: str) -> str:
    if node is None:
        return ""
    parts = []
    for child in node.iter():
        if xml_local_name(child.tag) == name and child.text:
            parts.append(child.text)
    return "".join(parts)


def excel_col_name(idx0: int) -> str:
    # 0-based 列号转 Excel 列名：0->A, 25->Z, 26->AA
    n = max(0, idx0) + 1
    out = []
    while n > 0:
        n, rem = divmod(n - 1, 26)
        out.append(chr(ord("A") + rem))
    return "".join(reversed(out))


def normalize_table_cell_text(value) -> str:
    s = html.unescape(str(value or "")).strip()
    if not s:
        return ""
    s = re.sub(r"\s+", " ", s)
    if len(s) > 120:
        return s[:120] + "..."
    return s


def is_header_like_value(value: str) -> bool:
    if not value:
        return False
    if len(value) > 40:
        return False
    up = value.upper()
    if up in ("TRUE", "FALSE", "NULL", "N/A", "NA"):
        return False
    if re.fullmatch(r"[-+]?\d+(\.\d+)?", value):
        return False
    if re.fullmatch(r"\d{2,4}[-/]\d{1,2}[-/]\d{1,2}", value):
        return False
    return True


def detect_header_map(rows: list[list[tuple[str, str]]]) -> dict[str, str]:
    if not rows:
        return {}
    first = rows[0]
    if len(first) < 2:
        return {}

    non_empty = [v for _, v in first if v]
    if len(non_empty) < 2:
        return {}

    header_like = sum(1 for v in non_empty if is_header_like_value(v))
    if header_like / max(len(non_empty), 1) < 0.6:
        return {}

    out = {}
    used = set()
    for col, raw in first:
        val = normalize_table_cell_text(raw)
        if not val:
            continue
        name = val
        suffix = 2
        while name in used:
            name = f"{val}_{suffix}"
            suffix += 1
        used.add(name)
        out[col] = name
    return out


def format_sheet_rows(sheet_title: str, rows: list[list[tuple[str, str]]]) -> str:
    if not rows:
        return ""
    header_map = detect_header_map(rows)
    lines = [f"[{sheet_title}]"]

    data_rows = rows
    if header_map:
        ordered_cols = [col for col, _ in rows[0] if col in header_map]
        header_names = [header_map[col] for col in ordered_cols]
        if header_names:
            lines.append("表头: " + " | ".join(header_names))
        data_rows = rows[1:]

    for pairs in data_rows:
        parts = []
        for col, raw in pairs:
            val = normalize_table_cell_text(raw)
            if not val:
                continue
            label = header_map.get(col, col) if header_map else col
            if header_map and label == val:
                continue
            parts.append(f"{label}:{val}")
        if parts:
            lines.append(" | ".join(parts))

    # 仅有标题时说明该 sheet 没有有效内容
    if len(lines) <= 1:
        return ""
    return "\n".join(lines)


def parse_xlsx_shared_strings(zf: zipfile.ZipFile) -> list[str]:
    if "xl/sharedStrings.xml" not in zf.namelist():
        return []
    try:
        root = ET.fromstring(zf.read("xl/sharedStrings.xml"))
    except ET.ParseError:
        return []

    out = []
    for node in root.iter():
        if xml_local_name(node.tag) != "si":
            continue
        text = html.unescape(xml_desc_text(node, "t")).strip()
        out.append(text)
    return out


def xlsx_cell_text(cell: ET.Element, shared: list[str]) -> str:
    ctype = (cell.attrib.get("t") or "").strip()
    value_node = xml_child(cell, "v")

    if ctype == "inlineStr":
        text = html.unescape(xml_desc_text(xml_child(cell, "is"), "t"))
        return text.strip()

    if ctype == "s":
        raw = (value_node.text if value_node is not None and value_node.text else "").strip()
        if not raw:
            return ""
        try:
            idx = int(raw)
        except ValueError:
            return ""
        if idx < 0 or idx >= len(shared):
            return ""
        return shared[idx].strip()

    raw = (value_node.text if value_node is not None and value_node.text else "").strip()
    if ctype == "b":
        if raw == "1":
            return "TRUE"
        if raw == "0":
            return "FALSE"
    return html.unescape(raw)


def extract_xlsx_text(data: bytes) -> tuple[str, str]:
    def sheet_sort_key(path: str):
        m = re.search(r"sheet(\d+)\.xml$", path)
        return int(m.group(1)) if m else 10**9

    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        sheet_paths = [n for n in zf.namelist()
                       if n.startswith("xl/worksheets/sheet") and n.endswith(".xml")]
        sheet_paths.sort(key=sheet_sort_key)
        if not sheet_paths:
            raise RuntimeError("xlsx has no worksheet xml")

        shared = parse_xlsx_shared_strings(zf)
        blocks = []
        total = 0
        for sidx, path in enumerate(sheet_paths, start=1):
            try:
                root = ET.fromstring(zf.read(path))
            except ET.ParseError:
                continue

            rows: list[list[tuple[str, str]]] = []
            for row in root.iter():
                if xml_local_name(row.tag) != "row":
                    continue
                pairs: list[tuple[str, str]] = []
                for cidx, cell in enumerate(list(row), start=1):
                    if xml_local_name(cell.tag) != "c":
                        continue
                    value = xlsx_cell_text(cell, shared)
                    if not value:
                        continue
                    ref = (cell.attrib.get("r") or "").upper()
                    m = re.match(r"^([A-Z]+)\d+$", ref)
                    key = m.group(1) if m else excel_col_name(cidx - 1)
                    pairs.append((key, value))
                if not pairs:
                    continue
                rows.append(pairs)
                if len(rows) >= 100:
                    break

            if rows:
                block = format_sheet_rows(f"sheet {sidx}", rows)
                if block:
                    blocks.append(block)
                    total += len(block)
            if total >= 12000:
                break

    if not blocks:
        raise RuntimeError("xlsx text empty")
    return "\n\n".join(blocks), "xlsx-xml"


def xlrd_cell_text(cell, datemode: int) -> str:
    try:
        import xlrd
    except Exception:
        return ""

    ctype = cell.ctype
    value = cell.value
    if ctype in (xlrd.XL_CELL_EMPTY, xlrd.XL_CELL_BLANK):
        return ""
    if ctype == xlrd.XL_CELL_TEXT:
        return normalize_table_cell_text(value)
    if ctype == xlrd.XL_CELL_NUMBER:
        if float(value).is_integer():
            return str(int(value))
        return ("%0.6f" % value).rstrip("0").rstrip(".")
    if ctype == xlrd.XL_CELL_DATE:
        try:
            dt = xlrd.xldate_as_datetime(value, datemode)
            if dt.hour == 0 and dt.minute == 0 and dt.second == 0:
                return dt.strftime("%Y-%m-%d")
            return dt.strftime("%Y-%m-%d %H:%M:%S")
        except Exception:
            return ""
    if ctype == xlrd.XL_CELL_BOOLEAN:
        return "TRUE" if bool(value) else "FALSE"
    if ctype == xlrd.XL_CELL_ERROR:
        return ""
    return normalize_table_cell_text(value)


def extract_xls_text(data: bytes) -> tuple[str, str]:
    try:
        import xlrd
    except Exception as e:
        raise RuntimeError(f"xls parser unavailable: {e}")

    try:
        book = xlrd.open_workbook(file_contents=data, on_demand=True)
    except Exception as e:
        raise RuntimeError(f"xls parse failed: {e}")

    blocks = []
    total = 0
    try:
        for sidx in range(book.nsheets):
            sheet = book.sheet_by_index(sidx)
            rows: list[list[tuple[str, str]]] = []
            max_rows = min(sheet.nrows, 120)
            max_cols = min(sheet.ncols, 64)
            for ridx in range(max_rows):
                pairs: list[tuple[str, str]] = []
                for cidx in range(max_cols):
                    text = xlrd_cell_text(sheet.cell(ridx, cidx), book.datemode)
                    if not text:
                        continue
                    pairs.append((excel_col_name(cidx), text))
                if pairs:
                    rows.append(pairs)
                if len(rows) >= 100:
                    break

            if rows:
                block = format_sheet_rows(f"sheet {sidx + 1}: {sheet.name}", rows)
                if block:
                    blocks.append(block)
                    total += len(block)
            if total >= 12000:
                break
    finally:
        try:
            book.release_resources()
        except Exception:
            pass

    if not blocks:
        raise RuntimeError("xls text empty")
    return "\n\n".join(blocks), "xls-xlrd"


def build_doc_result(text: str, doc_format: str, parser: str, truncated: bool) -> dict:
    clean = normalize_doc_text(text)
    if not clean:
        raise RuntimeError("document text empty")

    excerpt_limit = 900
    excerpt = clean[:excerpt_limit]
    if len(clean) > excerpt_limit:
        excerpt += "..."

    lines = [ln for ln in clean.split("\n") if ln]
    summary = "；".join(lines[:3])[:240]
    if not summary:
        summary = excerpt[:120]

    response_text = (
        f"文档解析结果\n"
        f"格式：{doc_format}\n"
        f"摘要：{summary}\n"
        f"摘录：\n{excerpt}"
    )
    return {
        "text": response_text,
        "summary": summary,
        "excerpt": excerpt,
        "doc_format": doc_format,
        "parser": parser,
        "text_len": len(clean),
        "truncated": bool(truncated),
    }


def do_document(doc_data: bytes, doc_name: str = "", doc_mime: str = "",
                doc_path: str = "", doc_format: str = "") -> dict:
    if not doc_data:
        raise RuntimeError("empty document")

    fmt = infer_doc_format(doc_name, doc_mime, doc_path, doc_format)
    parser = "bytes"
    text = ""
    truncated = False
    from_vision = False

    if fmt in ("jpeg", "jpg", "png", "webp", "bmp"):
        if not vision_cfg["enabled"]:
            raise RuntimeError("image document requires vision endpoint")
        vision_result = do_vision(doc_data, fmt)
        doc_text = build_vision_text(vision_result)
        result = build_doc_result(doc_text, fmt, "vision", False)
        result["from_vision"] = True
        return result

    if fmt in ("txt", "md", "csv", "json", "yaml", "yml", "xml", "html", "htm", "log"):
        text = decode_text_bytes(doc_data)
        parser = "text-decode"
    elif fmt == "pdf":
        text, parser = extract_pdf_text(doc_data)
        if doc_text_len(text) < DOC_OCR_FALLBACK_MIN_LEN and vision_cfg["enabled"]:
            try:
                chunks = extract_pdf_images_for_ocr(doc_data)
                ocr_text, ocr_parser = ocr_images_via_vision(chunks, "pdf")
                if doc_text_len(ocr_text) >= doc_text_len(text):
                    text, parser = ocr_text, ocr_parser
                    from_vision = True
                    log.info("doc_ocr: pdf fallback applied (chunks=%d)", len(chunks))
            except Exception as e:
                log.warning("doc_ocr: pdf fallback skipped: %s", e)
    elif fmt == "docx":
        text, parser = extract_docx_text(doc_data)
    elif fmt == "pptx":
        text, parser = extract_pptx_text(doc_data)
        if doc_text_len(text) < DOC_OCR_FALLBACK_MIN_LEN and vision_cfg["enabled"]:
            try:
                chunks = extract_pptx_images_for_ocr(doc_data)
                ocr_text, ocr_parser = ocr_images_via_vision(chunks, "pptx")
                if doc_text_len(ocr_text) >= doc_text_len(text):
                    text, parser = ocr_text, ocr_parser
                    from_vision = True
                    log.info("doc_ocr: pptx fallback applied (chunks=%d)", len(chunks))
            except Exception as e:
                log.warning("doc_ocr: pptx fallback skipped: %s", e)
    elif fmt in ("xlsx", "xlsm"):
        text, parser = extract_xlsx_text(doc_data)
    elif fmt == "xls":
        text, parser = extract_xls_text(doc_data)
    else:
        # 对未知格式做二进制拦截，避免把 ZIP/Office 头误判为文本（如 PK）
        if is_probably_binary(doc_data):
            raise RuntimeError(f"unsupported binary document format: {fmt}")
        text = decode_text_bytes(doc_data)
        parser = "text-fallback"

    if len(text) > DOC_MAX_TEXT:
        text = text[:DOC_MAX_TEXT]
        truncated = True

    result = build_doc_result(text, fmt, parser, truncated)
    result["from_vision"] = bool(from_vision)
    return result


class STTUploadHandler(BaseHTTPRequestHandler):
    """HTTP handler for STT and optional vision uploads."""

    def do_POST(self):
        if self.path not in ("/stt_upload", "/vision_upload", "/doc_upload"):
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"error":"empty body"}')
            return

        body = self.rfile.read(length)
        if self.path == "/vision_upload":
            if not vision_cfg["enabled"]:
                self.send_response(503)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.end_headers()
                self.wfile.write(b'{"error":"vision disabled"}')
                return

            image_format = self.headers.get("X-Image-Format", "")
            user_prompt = self.headers.get("X-Image-Prompt", "")
            try:
                result = do_vision(body, image_format, user_prompt)
                text = build_vision_text(result)
                resp = json.dumps({
                    "text": text,
                    "caption": result.get("caption", ""),
                    "ocr_text": result.get("ocr_text", ""),
                    "objects": result.get("objects", []),
                    "model": vision_cfg["model"],
                    "format": infer_image_format(body, image_format),
                }, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(resp)))
                self.end_headers()
                self.wfile.write(resp)
                log.info("HTTP vision ok len=%d text=%.80s", len(body), text)
            except Exception as e:
                log.exception("HTTP vision failed")
                resp = json.dumps({"error": str(e)}, ensure_ascii=False).encode("utf-8")
                self.send_response(500)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(resp)))
                self.end_headers()
                self.wfile.write(resp)
            return

        if self.path == "/doc_upload":
            doc_name = self.headers.get("X-Doc-Name", "")
            doc_mime = self.headers.get("X-Doc-Mime", "")
            doc_path = self.headers.get("X-Doc-Path", "")
            doc_format = self.headers.get("X-Doc-Format", "")
            try:
                result = do_document(
                    body,
                    doc_name=doc_name,
                    doc_mime=doc_mime,
                    doc_path=doc_path,
                    doc_format=doc_format,
                )
                resp = json.dumps(result, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(resp)))
                self.end_headers()
                self.wfile.write(resp)
                log.info("HTTP doc ok fmt=%s parser=%s text_len=%s",
                         result.get("doc_format", ""),
                         result.get("parser", ""),
                         result.get("text_len", 0))
            except Exception as e:
                log.exception("HTTP doc failed")
                resp = json.dumps({"error": str(e)}, ensure_ascii=False).encode("utf-8")
                self.send_response(500)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(resp)))
                self.end_headers()
                self.wfile.write(resp)
            return

        audio_format = self.headers.get("X-Audio-Format", "ogg")

        try:
            text, language = do_stt_encoded(body, audio_format)
            resp = json.dumps({
                "text": text,
                "language": language,
            }, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
            log.info("HTTP STT ok format=%s len=%d text=%.80s", audio_format, len(body), text)
        except Exception as e:
            log.exception("HTTP STT failed")
            resp = json.dumps({"error": str(e)}, ensure_ascii=False).encode("utf-8")
            self.send_response(500)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)

    def do_GET(self):
        if self.path == "/health":
            payload = json.dumps({
                "status": "ok",
                "vision_enabled": bool(vision_cfg["enabled"]),
                "vision_model": vision_cfg["model"] if vision_cfg["enabled"] else "",
            }).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        self.send_response(404)
        self.end_headers()

    def log_message(self, format, *args):
        log.info("http: " + format, *args)


async def stream_tts_pcm(ws, text: str, voice: str, rate: str, cancel_event: asyncio.Event):
    """Stream TTS as PCM chunks over WebSocket using ffmpeg for MP3→PCM conversion."""
    import edge_tts

    await ws.send(json.dumps({"type": "tts_start"}))

    # Use ffmpeg subprocess to convert streaming MP3 → PCM in real-time
    ffmpeg = subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "error",
         "-f", "mp3", "-i", "pipe:0",
         "-f", "s16le", "-ar", "16000", "-ac", "1", "pipe:1"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        bufsize=0,
    )

    bytes_sent = 0

    async def feed_mp3():
        """Feed MP3 chunks from edge-tts into ffmpeg stdin."""
        communicate = edge_tts.Communicate(text, voice, rate=rate)
        try:
            async for chunk in communicate.stream():
                if cancel_event.is_set():
                    break
                if chunk["type"] == "audio":
                    ffmpeg.stdin.write(chunk["data"])
                    ffmpeg.stdin.flush()
        finally:
            try:
                ffmpeg.stdin.close()
            except BrokenPipeError:
                pass

    async def read_pcm():
        """Read PCM from ffmpeg stdout and send as binary WS frames."""
        nonlocal bytes_sent
        loop = asyncio.get_event_loop()
        while True:
            if cancel_event.is_set():
                break
            try:
                pcm_chunk = await loop.run_in_executor(None, ffmpeg.stdout.read, 4096)
            except Exception:
                break
            if not pcm_chunk:
                break
            try:
                await ws.send(pcm_chunk)
                bytes_sent += len(pcm_chunk)
            except websockets.exceptions.ConnectionClosed:
                break

    # Run MP3 feeding and PCM reading concurrently
    feed_task = asyncio.create_task(feed_mp3())
    read_task = asyncio.create_task(read_pcm())

    await asyncio.gather(feed_task, read_task, return_exceptions=True)

    ffmpeg.terminate()
    ffmpeg.wait()

    if not cancel_event.is_set():
        try:
            await ws.send(json.dumps({"type": "tts_end"}))
        except websockets.exceptions.ConnectionClosed:
            pass

    log.info("TTS: sent %d bytes PCM (%.1fs)", bytes_sent, bytes_sent / 32000)


async def handle_client(ws):
    """Handle a single WebSocket client connection."""
    remote = ws.remote_address
    log.info("Client connected: %s", remote)

    pcm_buffer = bytearray()
    recording = False
    tts_cancel = asyncio.Event()
    tts_task = None

    try:
        async for message in ws:
            if isinstance(message, bytes):
                # Binary frame: PCM audio chunk during recording
                if recording:
                    pcm_buffer.extend(message)
                continue

            # JSON control message
            try:
                msg = json.loads(message)
            except json.JSONDecodeError:
                log.warning("Invalid JSON from %s: %s", remote, message[:100])
                continue

            msg_type = msg.get("type", "")

            if msg_type == "audio_start":
                pcm_buffer.clear()
                recording = True
                log.info("Recording started from %s", remote)

            elif msg_type == "audio_end":
                recording = False
                pcm_len = len(pcm_buffer)
                log.info("Recording ended: %d bytes (%.1fs)", pcm_len, pcm_len / 32000)

                if pcm_len < 320:
                    await ws.send(json.dumps({
                        "type": "error", "message": "audio too short"
                    }))
                    continue

                # Run STT in executor to avoid blocking
                loop = asyncio.get_event_loop()
                try:
                    text, lang = await loop.run_in_executor(
                        None, do_stt, bytes(pcm_buffer))
                except Exception as e:
                    log.error("STT error: %s", e)
                    await ws.send(json.dumps({
                        "type": "error", "message": f"STT failed: {e}"
                    }))
                    continue

                await ws.send(json.dumps({
                    "type": "stt_result", "text": text, "language": lang
                }))

            elif msg_type == "tts_request":
                text = msg.get("text", "")
                voice = msg.get("voice", "zh-CN-XiaoxiaoNeural")
                rate = msg.get("rate", "+0%")

                clean = clean_text_for_tts(text)
                if not clean:
                    log.warning("TTS: no speakable text after cleanup, fallback to default reply")
                    clean = "收到"

                log.info("TTS request: voice=%s rate=%s text=%.80s...", voice, rate, clean)

                # Cancel any ongoing TTS
                if tts_task and not tts_task.done():
                    tts_cancel.set()
                    await tts_task

                tts_cancel = asyncio.Event()
                tts_task = asyncio.create_task(
                    stream_tts_pcm(ws, clean, voice, rate, tts_cancel))

            elif msg_type == "interrupt":
                log.info("Interrupt from %s", remote)
                if tts_task and not tts_task.done():
                    tts_cancel.set()
                    await tts_task
                    tts_task = None

            else:
                log.warning("Unknown message type: %s", msg_type)

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        # Cleanup on disconnect
        if tts_task and not tts_task.done():
            tts_cancel.set()
            try:
                await tts_task
            except Exception:
                pass
        log.info("Client disconnected: %s", remote)


async def health_handler(path, request_headers):
    """Respond to HTTP /health requests on the same port."""
    if path == "/health":
        return (200, [("Content-Type", "application/json")], b'{"status":"ok"}')
    return None


async def run_server(host: str, port: int):
    """Start the WebSocket server."""
    log.info("Starting voice gateway on ws://%s:%d", host, port)
    async with websockets.serve(
        handle_client, host, port,
        process_request=health_handler,
        max_size=1024 * 1024,  # 1MB max message
        ping_interval=20,
        ping_timeout=20,
    ):
        await asyncio.Future()  # run forever


def main():
    parser = argparse.ArgumentParser(description="MimiClaw Voice Gateway (WebSocket)")
    parser.add_argument("--host", default="0.0.0.0", help="Listen address")
    parser.add_argument("--port", type=int, default=8090, help="Listen port")
    parser.add_argument("--model", default="small",
                        help="Whisper model size (tiny/base/small/medium/large-v3)")
    parser.add_argument("--device", default="auto", help="Compute device (cpu/cuda/auto)")
    parser.add_argument("--stt-port", type=int, default=0,
                        help="HTTP STT upload port (default: ws_port+1)")
    parser.add_argument("--vision-enabled", action="store_true",
                        help="Enable /vision_upload image analysis")
    parser.add_argument("--vision-endpoint", default="",
                        help="Vision API endpoint (Anthropic Messages compatible)")
    parser.add_argument("--vision-api-key", default="", help="Vision API key")
    parser.add_argument("--vision-model", default="", help="Vision model name")
    parser.add_argument("--vision-api-version", default="2023-06-01",
                        help="Vision API version header")
    parser.add_argument("--vision-timeout", type=int, default=45,
                        help="Vision API timeout seconds")
    parser.add_argument("--vision-prompt",
                        default="请用中文简洁描述图片内容，并提取图中可见文字。",
                        help="Default vision prompt")
    parser.add_argument("--vision-proxy", default="",
                        help="HTTP/HTTPS proxy URL for vision API (optional)")
    parser.add_argument("--vision-secrets", default="main/mimi_secrets.h",
                        help="Path to mimi_secrets.h for default vision config")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(name)s] %(message)s")

    global whisper_model
    log.info("Loading whisper model '%s' on %s...", args.model, args.device)
    whisper_model = WhisperModel(args.model, device=args.device, compute_type="int8")
    log.info("Whisper model loaded.")

    defaults = load_vision_defaults_from_secrets(args.vision_secrets)
    vision_cfg["endpoint"] = args.vision_endpoint or defaults.get("endpoint", "")
    vision_cfg["api_key"] = args.vision_api_key or defaults.get("api_key", "")
    vision_cfg["model"] = args.vision_model or defaults.get("model", "")
    vision_cfg["api_version"] = args.vision_api_version
    vision_cfg["timeout_s"] = args.vision_timeout
    vision_cfg["prompt"] = args.vision_prompt
    vision_cfg["http_proxy"] = args.vision_proxy
    vision_cfg["enabled"] = bool(
        args.vision_enabled or
        (vision_cfg["endpoint"] and vision_cfg["api_key"] and vision_cfg["model"])
    )

    if vision_cfg["enabled"]:
        log.info("Vision enabled: endpoint=%s model=%s", vision_cfg["endpoint"], vision_cfg["model"])
    else:
        log.info("Vision disabled (provide --vision-enabled or complete vision config)")

    stt_port = args.stt_port if args.stt_port > 0 else (args.port + 1)
    http_server = ThreadingHTTPServer((args.host, stt_port), STTUploadHandler)
    http_thread = threading.Thread(target=http_server.serve_forever, daemon=True)
    http_thread.start()
    log.info("HTTP STT server listening on http://%s:%d/stt_upload", args.host, stt_port)
    log.info("HTTP vision endpoint: http://%s:%d/vision_upload (enabled=%s)",
             args.host, stt_port, "yes" if vision_cfg["enabled"] else "no")
    log.info("HTTP document endpoint: http://%s:%d/doc_upload", args.host, stt_port)

    try:
        asyncio.run(run_server(args.host, args.port))
    finally:
        http_server.shutdown()


if __name__ == "__main__":
    main()
