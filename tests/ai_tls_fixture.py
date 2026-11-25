"""Loopback-only HTTPS fixture for the real Rust AI HTTP clients.

The caller creates a short-lived test certificate/key in its temporary directory.
Only the listening port is printed. Request bodies and credentials are never logged.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import ssl
import threading
import time
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit


FAKE_KEY = "fixture-key-DO-NOT-PRINT"
PRIVATE_MARKER = "SERVER_PRIVATE_BODY"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--wav-sha", default="")
    args = parser.parse_args()
    report: dict = {"requests": 0, "checks": {}, "stage": "starting"}
    report_lock = threading.Lock()

    def record(**values) -> None:
        with report_lock:
            report.update(values)
            temporary = args.report.with_suffix(".tmp")
            temporary.write_text(json.dumps(report), encoding="utf-8")
            os.replace(temporary, args.report)

    def gate() -> bool:
        record(stage="paused")
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if args.release.exists():
                record(gateTimedOut=False)
                return True
            time.sleep(0.005)
        record(gateTimedOut=True)
        return False

    def chat_checks(body: dict, path: str, headers) -> dict:
        messages = body.get("messages", [])
        system = "\n".join(str(item.get("content", "")) for item in messages if item.get("role") == "system")
        user = "\n".join(str(item.get("content", "")) for item in messages if item.get("role") == "user")
        user_fields = json.loads(user)
        return {
            "endpoint": path.endswith("/v1/chat/completions"),
            "authorization": headers.get("Authorization") == "Bearer " + FAKE_KEY,
            "stream": body.get("stream") is True,
            "model": body.get("model") == "fixture-writing-model",
            "chinese_policy": "中文" in system,
            "chinese_target": user_fields.get("targetLanguage") == "简体中文",
            "user_source": "用户原文" in user,
        }

    def gemini_text_checks(body: dict, path: str, query: str, headers) -> dict:
        system = body.get("systemInstruction", body.get("system_instruction", {}))
        contents = body.get("contents", [])
        source = "\n".join(str(part.get("text", "")) for item in contents for part in item.get("parts", []))
        user_fields = json.loads(source)
        return {
            "endpoint": path.endswith("/models/gemini-3.5-flash-lite:streamGenerateContent") and query == "alt=sse",
            "authorization": headers.get("x-goog-api-key") == FAKE_KEY,
            "translation_target": user_fields.get("targetLanguage") == "日语",
            "user_source": "用户原文" in source,
        }

    def multipart_checks(raw: bytes, path: str, headers) -> dict:
        content_type = headers.get("Content-Type", "")
        envelope = b"Content-Type: " + content_type.encode("ascii") + b"\r\nMIME-Version: 1.0\r\n\r\n" + raw
        message = BytesParser(policy=policy.default).parsebytes(envelope)
        fields = {}
        audio = b""
        filename = None
        audio_type = None
        for part in message.iter_parts():
            name = part.get_param("name", header="content-disposition")
            if name == "file":
                audio = part.get_payload(decode=True)
                filename = part.get_filename()
                audio_type = part.get_content_type()
            elif name:
                fields[name] = part.get_payload(decode=True).decode("utf-8")
        return {
            "endpoint": path.endswith("/v1/audio/transcriptions"),
            "authorization": headers.get("Authorization") == "Bearer " + FAKE_KEY,
            "multipart": content_type.startswith("multipart/form-data;"),
            "filename": filename == "speech.wav",
            "audio_type": audio_type == "audio/wav",
            "model": fields.get("model") == "gpt-transcribe",
            "language": fields.get("languages[]") == "zh-cn",
            "json_format": fields.get("response_format") == "json",
            "no_legacy_language": "language" not in fields,
            "wav_exact": hashlib.sha256(audio).hexdigest() == args.wav_sha,
        }

    def gemini_audio_checks(body: dict, path: str, headers) -> dict:
        inputs = body.get("input", [])
        audio = inputs[0] if len(inputs) == 1 else {}
        try:
            wav = base64.b64decode(audio.get("data", ""), validate=True)
        except (ValueError, TypeError):
            wav = b""
        transcription = body.get("generation_config", {}).get("transcription_config", {})
        return {
            "endpoint": path.endswith("/v1beta/interactions"),
            "authorization": headers.get("x-goog-api-key") == FAKE_KEY,
            "model": body.get("model") == "gemini-3.5-transcribe",
            "store_false": body.get("store") is False,
            "audio_type": audio.get("type") == "audio" and audio.get("mime_type") == "audio/wav",
            "mode": transcription.get("mode") == {"type": "verbatim"},
            "language": transcription.get("language_codes") == ["cmn-Hans-CN"],
            "wav_exact": hashlib.sha256(wav).hexdigest() == args.wav_sha,
        }

    class Server(ThreadingHTTPServer):
        daemon_threads = True
        def handle_error(self, request, client_address):
            # A client cancel legitimately closes TLS while a test response is paused.
            pass

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def reply(self, code: int, value: dict) -> None:
            data = json.dumps(value, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
            self.wfile.flush()

        def begin_chunks(self, content_type: str) -> None:
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()

        def chunk(self, data: bytes) -> None:
            self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
            self.wfile.flush()

        def split(self, data: bytes) -> None:
            # Split UTF-8 and SSE/JSON boundaries rather than serving complete frames.
            sizes = (1, 2, 5, 3, 11, 7)
            offset = 0
            index = 0
            while offset < len(data):
                size = sizes[index % len(sizes)]
                self.chunk(data[offset:offset + size])
                offset += size
                index += 1

        def finish_chunks(self) -> None:
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            record(stage="finished")

        def do_POST(self) -> None:
            with report_lock:
                total = report["requests"] + 1
            record(requests=total)
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length < 0 or length > 4 * 1024 * 1024:
                    self.reply(413, {"error": "fixture request bound"})
                    return
                raw = self.rfile.read(length)
                url = urlsplit(self.path)
                scenario = args.scenario
                if scenario == "redirect":
                    self.send_response(302)
                    self.send_header("Location", f"https://127.0.0.1:{self.server.server_port}/should-not-follow")
                    self.send_header("Content-Length", "0")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    record(stage="finished")
                    return
                if scenario == "error":
                    self.reply(401, {"error": {"message": PRIVATE_MARKER + ": " + FAKE_KEY}})
                    record(stage="finished")
                    return
                if scenario == "oversized_asr":
                    self.reply(200, {"text": "长" * (128 * 1024)})
                    record(stage="finished")
                    return
                if scenario in ("gpt_asr", "cancel_asr"):
                    checks = multipart_checks(raw, url.path, self.headers)
                    record(checks=checks)
                    if not all(checks.values()):
                        self.reply(422, {"error": "fixture request mismatch"})
                        return
                    if scenario == "gpt_asr":
                        self.reply(200, {"text": "今天的转写。"})
                        record(stage="finished")
                    else:
                        self.begin_chunks("application/json")
                        self.chunk(b'{"text":"')
                        gate()
                        self.chunk("迟到的转写不应返回".encode() + b'"}')
                        self.finish_chunks()
                    return
                body = json.loads(raw)
                if scenario in ("gemini_asr", "gemini_pending"):
                    checks = gemini_audio_checks(body, url.path, self.headers)
                    record(checks=checks)
                    if not all(checks.values()):
                        self.reply(422, {"error": "fixture request mismatch"})
                        return
                    self.reply(200, {
                        "status": "completed" if scenario == "gemini_asr" else "in_progress",
                        "steps": [
                            {"type": "thought", "content": [{"type": "text", "text": "不返回思考步骤"}]},
                            {"type": "model_output", "content": [
                                {"type": "text", "text": "不返回thought", "thought": True},
                                {"type": "text", "text": "不返回is_thought", "is_thought": True},
                                {"type": "text", "text": "第一段"},
                                {"type": "image", "text": "不是文字"},
                                {"type": "text", "text": "第二段"},
                            ]},
                        ],
                    })
                    record(stage="finished")
                    return
                gemini = scenario == "gemini_sse"
                checks = gemini_text_checks(body, url.path, url.query, self.headers) if gemini else chat_checks(body, url.path, self.headers)
                record(checks=checks)
                if not all(checks.values()):
                    self.reply(422, {"error": "fixture request mismatch"})
                    return
                if scenario in ("json_text", "json_length"):
                    self.reply(200, {"choices": [{"message": {"role": "assistant", "content": "完整的中文结果。"},
                                                  "finish_reason": "stop" if scenario == "json_text" else "length"}]})
                    record(stage="finished")
                    return
                if scenario in ("truncated_sse", "length_sse", "oversized_event"):
                    self.begin_chunks("text/event-stream")
                    if scenario == "oversized_event":
                        self.chunk(b"data: " + b"x" * (256 * 1024 + 1))
                    else:
                        self.chunk('data: {"choices":[{"delta":{"content":"仅有一半"}}]}\n\n'.encode())
                        if scenario == "length_sse":
                            self.chunk(b'data: {"choices":[{"delta":{},"finish_reason":"length"}]}\n\n')
                    self.finish_chunks()
                    return
                self.begin_chunks("text/event-stream; charset=utf-8")
                self.split(b": keepalive\r\n\r\n")
                if gemini:
                    def event(text, final=False):
                        candidate = {"content": {"parts": [{"thought": True, "text": "不可见思考"}, {"text": text}]}}
                        if final:
                            candidate["finishReason"] = "STOP"
                        return b"data: " + json.dumps({"candidates": [candidate]}, ensure_ascii=False).encode() + b"\n\n"
                    self.split(event("你"))
                    self.split(event("好"))
                    gate()
                    self.split(event("，世界！", True))
                else:
                    # A legal multiline SSE data event: newline is JSON whitespace.
                    self.split('event: message\r\ndata: {"choices":[{"delta":\r\ndata: {"content":"你"}}]}\r\n\r\n'.encode())
                    self.split('data: {"choices":[{"delta":{"content":"好"}}]}\n\n'.encode())
                    gate()
                    self.split('data: {"choices":[{"delta":{"content":"，世界！"},"finish_reason":"stop"}]}\n\n'.encode())
                    self.split(b"data: [DONE]\n\n")
                self.finish_chunks()
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                record(clientDisconnected=True)
            except Exception as error:
                record(fixtureError=type(error).__name__)
                try:
                    self.reply(500, {"error": "fixture internal error"})
                except OSError:
                    pass

        def do_GET(self) -> None:
            # A redirected request must never arrive here with an AI credential.
            with report_lock:
                total = report["requests"] + 1
            record(requests=total, followedRedirect=True)
            self.reply(500, {"error": "unexpected redirected request"})

    server = Server(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(args.cert, args.key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    record(stage="ready")
    print(json.dumps({"port": server.server_port}), flush=True)
    server.serve_forever(poll_interval=0.05)


if __name__ == "__main__":
    main()
