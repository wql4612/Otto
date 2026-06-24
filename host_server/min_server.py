from __future__ import annotations

import json
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from .config import load_settings
from .qwen_host_client import QwenHostClient, QwenRequestError
from .runtime_state import runtime_state


settings = load_settings()
qwen = QwenHostClient(settings)
PANEL_PATH = Path(__file__).resolve().parents[1] / "desktop_debug_panel" / "index.html"


def _audio_mime_from_format(fmt: str) -> str:
    value = (fmt or "").strip().lower()
    if value == "mp3":
        return "audio/mpeg"
    if value == "pcm":
        return "audio/L16"
    return "audio/wav"


def _sanitize_voice_command_result(result: dict) -> dict:
    payload = dict(result)
    tts_audio = payload.pop("tts_audio_bytes", b"")
    action = payload.get("action")
    answer = action.get("answer", "") if isinstance(action, dict) else ""
    tts_format = str(payload.get("tts", {}).get("format", settings.tts_format))
    runtime_state.update_voice_reply(
        transcript=str(payload.get("voice", {}).get("transcript", "")),
        answer=str(answer),
        tts_text=str(payload.get("tts", {}).get("tts_text", answer)),
        action=action if isinstance(action, dict) else {},
        tts_audio=tts_audio if isinstance(tts_audio, bytes) else b"",
        tts_mime=_audio_mime_from_format(tts_format),
    )
    payload["runtime"] = runtime_state.voice_reply_snapshot()
    return payload


class OttoHostHandler(BaseHTTPRequestHandler):
    server_version = "OttoHostServer/0.1"

    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, status: int, body: str, content_type: str) -> None:
        raw = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        self.wfile.write(raw)

    def _send_bytes(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self) -> dict:
        content_len = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(content_len) if content_len > 0 else b"{}"
        if not raw:
            return {}
        try:
            data = json.loads(raw.decode("utf-8"))
            return data if isinstance(data, dict) else {}
        except json.JSONDecodeError:
            raise ValueError("invalid json body")

    def _healthz(self) -> dict:
        return {
            "ok": True,
            "mode": "stdlib-http",
            "qwen_configured": settings.configured,
            "voice_chat_url": settings.voice_chat_url,
            "image_chat_url": settings.image_chat_url,
            "tts_chat_url": settings.tts_chat_url,
            "voice_model": settings.voice_model,
            "image_model": settings.image_model,
            "tts_voice": settings.tts_voice,
            "tts_format": settings.tts_format,
        }

    def _info(self) -> dict:
        return {
            "service": "otto-host-server",
            "mode": "stdlib-http",
            "panel": "/",
            "routes": [
                "GET /",
                "GET /healthz",
                "GET /api/info",
                "GET /api/runtime/status",
                "GET /api/voice/audio/latest.wav",
                "POST /api/qwen/test/sample-asr",
                "POST /api/qwen/test/plan",
                "POST /api/qwen/test/reply",
                "POST /api/qwen/test/connectivity",
                "POST /api/voice/command",
            ],
        }

    def _serve_panel(self) -> None:
        if not PANEL_PATH.exists():
            self._send_text(
                HTTPStatus.NOT_FOUND,
                "Otto host panel file is missing.",
                "text/plain; charset=utf-8",
            )
            return
        self._send_text(
            HTTPStatus.OK,
            PANEL_PATH.read_text(encoding="utf-8"),
            "text/html; charset=utf-8",
        )

    def do_OPTIONS(self) -> None:
        self._send_json(HTTPStatus.NO_CONTENT, {})

    def do_GET(self) -> None:
        route = urlparse(self.path).path
        if route in ("/", "/panel"):
            self._serve_panel()
            return

        if route == "/healthz":
            self._send_json(HTTPStatus.OK, self._healthz())
            return

        if route == "/api/info":
            self._send_json(HTTPStatus.OK, self._info())
            return

        if route == "/api/runtime/status":
            self._send_json(
                HTTPStatus.OK,
                {
                    "ok": True,
                    "runtime": runtime_state.voice_reply_snapshot(),
                },
            )
            return

        if route == "/api/voice/audio/latest.wav":
            reply_id, mime, audio = runtime_state.latest_audio()
            if not reply_id or not audio:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "no audio available"})
                return
            self._send_bytes(HTTPStatus.OK, audio, mime)
            return

        self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})

    def do_POST(self) -> None:
        route = urlparse(self.path).path

        try:
            if route == "/api/qwen/test/sample-asr":
                self._send_json(HTTPStatus.OK, qwen.run_sample_asr())
                return

            if route == "/api/qwen/test/plan":
                body = self._read_json_body()
                transcript = str(body.get("transcript", "")).strip()
                if not transcript:
                    self._send_json(HTTPStatus.BAD_REQUEST, {"error": "transcript is required"})
                    return
                self._send_json(HTTPStatus.OK, qwen.plan_action(transcript))
                return

            if route == "/api/qwen/test/reply":
                body = self._read_json_body()
                transcript = str(body.get("transcript", "")).strip()
                if not transcript:
                    self._send_json(HTTPStatus.BAD_REQUEST, {"error": "transcript is required"})
                    return
                result = qwen.run_text_command(transcript)
                self._send_json(HTTPStatus.OK, _sanitize_voice_command_result(result))
                return

            if route == "/api/qwen/test/connectivity":
                self._send_json(HTTPStatus.OK, qwen.connectivity_report())
                return

            if route == "/api/voice/command":
                body = self._read_json_body()
                audio_base64 = str(body.get("audio_base64", "")).strip()
                audio_format = str(body.get("audio_format", "wav")).strip() or "wav"
                if not audio_base64:
                    self._send_json(HTTPStatus.BAD_REQUEST, {"error": "audio_base64 is required"})
                    return
                import base64

                try:
                    audio_bytes = base64.b64decode(audio_base64, validate=True)
                except Exception:
                    self._send_json(HTTPStatus.BAD_REQUEST, {"error": "invalid audio_base64"})
                    return

                result = qwen.run_audio_command(audio_bytes, fmt=audio_format)
                self._send_json(HTTPStatus.OK, _sanitize_voice_command_result(result))
                return

            self._send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
        except ValueError as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
        except QwenRequestError as exc:
            self._send_json(HTTPStatus.BAD_GATEWAY, {"error": str(exc)})
        except Exception as exc:
            self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})

    def log_message(self, fmt: str, *args) -> None:
        print(f"[host_server] {self.address_string()} - {fmt % args}")


def main() -> None:
    server = ThreadingHTTPServer((settings.host, settings.port), OttoHostHandler)
    print(
        f"Otto host server running on http://{settings.host}:{settings.port} "
        f"(stdlib mode)"
    )
    server.serve_forever()


if __name__ == "__main__":
    main()
