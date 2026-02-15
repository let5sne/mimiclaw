#!/usr/bin/env python3
"""
MimiClaw Voice Gateway Server

Lightweight HTTP server that bridges ESP32 audio with cloud STT/TTS services.
  POST /stt  — raw PCM 16kHz 16-bit mono → transcribed text (faster-whisper)
  POST /tts  — JSON {"text": "...", "voice": "..."} → raw PCM 16kHz 16-bit mono (edge-tts)

Usage:
  pip install -r requirements.txt
  python voice_gateway.py [--host 0.0.0.0] [--port 8090] [--model small]
"""

import argparse
import asyncio
import io
import struct
import tempfile
import logging

from flask import Flask, request, jsonify, Response
from faster_whisper import WhisperModel
import edge_tts
from pydub import AudioSegment

app = Flask(__name__)
log = logging.getLogger("voice_gw")

whisper_model = None  # initialized in main


def pcm_to_wav_bytes(pcm_data: bytes, sample_rate=16000, sample_width=2, channels=1) -> bytes:
    """Wrap raw PCM in a WAV header for whisper ingestion."""
    buf = io.BytesIO()
    import wave
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sample_width)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_data)
    return buf.getvalue()


@app.route("/stt", methods=["POST"])
def stt():
    """Receive raw PCM audio, return transcribed text."""
    pcm_data = request.get_data()
    if not pcm_data or len(pcm_data) < 320:
        return jsonify({"error": "no audio data"}), 400

    log.info("STT: received %d bytes (%.1fs)", len(pcm_data), len(pcm_data) / 32000)

    # Normalize audio level before transcription
    audio_seg = AudioSegment(data=pcm_data, sample_width=2, frame_rate=16000, channels=1)
    peak_dbfs = audio_seg.max_dBFS
    log.info("STT: input peak=%.1f dBFS", peak_dbfs)
    if peak_dbfs < -6.0:
        # Boost to target -3 dBFS
        gain = -3.0 - peak_dbfs
        audio_seg = audio_seg.apply_gain(gain)
        log.info("STT: applied +%.1f dB gain", gain)
        pcm_data = audio_seg.raw_data

    # Wrap PCM in WAV for whisper
    wav_bytes = pcm_to_wav_bytes(pcm_data)

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=True) as tmp:
        tmp.write(wav_bytes)
        tmp.flush()
        segments, info = whisper_model.transcribe(
            tmp.name,
            language="zh",
            beam_size=5,
            vad_filter=False,
            initial_prompt="以下是普通话的句子。",
        )
        text = "".join(seg.text for seg in segments).strip()

    log.info("STT result [%s]: %s", info.language, text)
    return jsonify({"text": text, "language": info.language})


@app.route("/tts", methods=["POST"])
def tts():
    """Receive text, return raw PCM audio."""
    data = request.get_json(silent=True)
    if not data or not data.get("text"):
        return jsonify({"error": "missing text"}), 400

    text = data["text"]
    voice = data.get("voice", "zh-CN-XiaoxiaoNeural")

    # Strip emoji and non-speakable characters, keep CJK + ASCII + punctuation
    import re
    clean_text = re.sub(
        r'[^\u4e00-\u9fff\u3000-\u303f\uff00-\uffef'
        r'a-zA-Z0-9\s.,!?;:\'\"()\-\u3002\uff0c\uff01\uff1f\uff1b\uff1a\u201c\u201d\u2018\u2019]',
        '', text).strip()

    if not clean_text:
        log.warning("TTS: no speakable text after cleanup, original: %s", repr(text))
        # Return 200 with empty PCM (silence) instead of error
        return Response(b'\x00' * 3200, mimetype="audio/pcm")

    log.info("TTS: voice=%s text=%.80s...", voice, clean_text)

    # edge-tts is async, run in event loop
    mp3_bytes = asyncio.run(_edge_tts_synthesize(clean_text, voice))

    # Convert MP3 → raw PCM 16kHz 16-bit mono via pydub
    audio = AudioSegment.from_mp3(io.BytesIO(mp3_bytes))
    audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
    pcm_data = audio.raw_data

    log.info("TTS: generated %d bytes PCM (%.1fs)", len(pcm_data), len(pcm_data) / 32000)
    return Response(pcm_data, mimetype="audio/pcm")


async def _edge_tts_synthesize(text: str, voice: str) -> bytes:
    """Run edge-tts and collect MP3 bytes."""
    communicate = edge_tts.Communicate(text, voice)
    buf = io.BytesIO()
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            buf.write(chunk["data"])
    return buf.getvalue()


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"})


def main():
    parser = argparse.ArgumentParser(description="MimiClaw Voice Gateway")
    parser.add_argument("--host", default="0.0.0.0", help="Listen address")
    parser.add_argument("--port", type=int, default=8090, help="Listen port")
    parser.add_argument("--model", default="small", help="Whisper model size (tiny/base/small/medium/large-v3)")
    parser.add_argument("--device", default="auto", help="Compute device (cpu/cuda/auto)")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(name)s] %(message)s")

    global whisper_model
    log.info("Loading whisper model '%s' on %s...", args.model, args.device)
    whisper_model = WhisperModel(args.model, device=args.device, compute_type="int8")
    log.info("Whisper model loaded.")

    log.info("Starting voice gateway on %s:%d", args.host, args.port)
    app.run(host=args.host, port=args.port, threaded=True)


if __name__ == "__main__":
    main()
