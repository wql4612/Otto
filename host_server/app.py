from __future__ import annotations

import base64
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, Response
from pydantic import BaseModel

from .config import load_settings
from .qwen_host_client import QwenHostClient, QwenRequestError
from .runtime_state import runtime_state


settings = load_settings()
qwen = QwenHostClient(settings)
app = FastAPI(title="Otto Host Server", version="0.1.0")
PANEL_PATH = Path(__file__).resolve().parents[1] / "desktop_debug_panel" / "index.html"


def _audio_mime_from_format(fmt: str) -> str:
    value = (fmt or "").strip().lower()
    if value == "mp3":
        return "audio/mpeg"
    if value == "pcm":
        return "audio/L16"
    return "audio/wav"


def _sanitize_voice_command_result(result: dict[str, Any]) -> dict[str, Any]:
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


class PlannerRequest(BaseModel):
    transcript: str


class VoiceCommandRequest(BaseModel):
    audio_base64: str
    audio_format: str = "wav"


class ReplyRequest(BaseModel):
    transcript: str


class EspSessionRegistry:
    def __init__(self) -> None:
        self.ws: WebSocket | None = None
        self.last_hello: dict[str, Any] | None = None

    async def attach(self, ws: WebSocket) -> None:
        await ws.accept()
        self.ws = ws

    def detach(self, ws: WebSocket) -> None:
        if self.ws is ws:
            self.ws = None

    @property
    def connected(self) -> bool:
        return self.ws is not None


esp_registry = EspSessionRegistry()


@app.get("/", response_class=HTMLResponse)
def root_panel() -> str:
    if not PANEL_PATH.exists():
        return "Otto host panel file is missing."
    return PANEL_PATH.read_text(encoding="utf-8")


@app.get("/healthz")
def healthz() -> dict[str, Any]:
    return {
        "ok": True,
        "mode": "fastapi",
        "qwen_configured": settings.configured,
        "voice_chat_url": settings.voice_chat_url,
        "image_chat_url": settings.image_chat_url,
        "tts_chat_url": settings.tts_chat_url,
        "voice_model": settings.voice_model,
        "image_model": settings.image_model,
        "tts_voice": settings.tts_voice,
        "tts_format": settings.tts_format,
        "esp_connected": esp_registry.connected,
    }


@app.get("/api/info")
def api_info() -> dict[str, Any]:
    return {
        "service": "otto-host-server",
        "mode": "fastapi",
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
            "WS /ws/esp32",
        ],
    }


@app.get("/api/runtime/status")
def runtime_status() -> dict[str, Any]:
    return {
        "ok": True,
        "runtime": runtime_state.voice_reply_snapshot(),
    }


@app.get("/api/voice/audio/latest.wav")
def latest_voice_audio() -> Response:
    reply_id, mime, audio = runtime_state.latest_audio()
    if not reply_id or not audio:
        raise HTTPException(status_code=404, detail="no audio available")
    return Response(content=audio, media_type=mime, headers={"Cache-Control": "no-store"})


@app.post("/api/qwen/test/sample-asr")
def test_sample_asr() -> dict[str, Any]:
    try:
        return qwen.run_sample_asr()
    except QwenRequestError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc


@app.post("/api/qwen/test/plan")
def test_plan(req: PlannerRequest) -> dict[str, Any]:
    try:
        return qwen.plan_action(req.transcript)
    except QwenRequestError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc


@app.post("/api/qwen/test/reply")
def test_reply(req: ReplyRequest) -> dict[str, Any]:
    try:
        result = qwen.run_text_command(req.transcript)
        return _sanitize_voice_command_result(result)
    except QwenRequestError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc


@app.post("/api/qwen/test/connectivity")
def test_connectivity() -> dict[str, Any]:
    try:
        return qwen.connectivity_report()
    except QwenRequestError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc


@app.post("/api/voice/command")
def voice_command(req: VoiceCommandRequest) -> dict[str, Any]:
    try:
        audio_bytes = base64.b64decode(req.audio_base64, validate=True)
    except Exception as exc:
        raise HTTPException(status_code=400, detail="invalid audio_base64") from exc

    try:
        result = qwen.run_audio_command(audio_bytes, fmt=req.audio_format)
        return _sanitize_voice_command_result(result)
    except QwenRequestError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc


@app.websocket("/ws/esp32")
async def ws_esp32(ws: WebSocket) -> None:
    await esp_registry.attach(ws)
    try:
        await ws.send_json(
            {
                "type": "host_claim",
                "protocol_version": 1,
                "voice_mode": "stream",
                "planner_mode": "host",
                "tts_mode": "host",
            }
        )
        while True:
            message = await ws.receive()
            if "text" in message and message["text"]:
                # Current stage only keeps the channel alive and records board hello/state.
                if '"type":"hello"' in message["text"] or '"type": "hello"' in message["text"]:
                    esp_registry.last_hello = {"raw": message["text"]}
                elif '"type":"state"' in message["text"] or '"type": "state"' in message["text"]:
                    esp_registry.last_hello = {"raw": message["text"]}
            elif message.get("type") == "websocket.disconnect":
                break
    except WebSocketDisconnect:
        pass
    finally:
        esp_registry.detach(ws)
