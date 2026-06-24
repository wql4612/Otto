from __future__ import annotations

import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any


@dataclass(slots=True)
class VoiceReplyState:
    reply_id: str = ""
    updated_at: float = 0.0
    transcript: str = ""
    answer: str = ""
    tts_text: str = ""
    tts_mime: str = "audio/wav"
    action: dict[str, Any] = field(default_factory=dict)
    tts_audio: bytes = b""

    @property
    def tts_available(self) -> bool:
        return bool(self.tts_audio)


class RuntimeState:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._voice_reply = VoiceReplyState()

    def update_voice_reply(
        self,
        *,
        transcript: str,
        answer: str,
        tts_text: str,
        action: dict[str, Any],
        tts_audio: bytes,
        tts_mime: str = "audio/wav",
    ) -> VoiceReplyState:
        with self._lock:
            self._voice_reply = VoiceReplyState(
                reply_id=uuid.uuid4().hex[:12],
                updated_at=time.time(),
                transcript=transcript,
                answer=answer,
                tts_text=tts_text,
                tts_mime=tts_mime,
                action=dict(action),
                tts_audio=bytes(tts_audio),
            )
            return self._voice_reply

    def voice_reply_snapshot(self) -> dict[str, Any]:
        with self._lock:
            current = self._voice_reply
            return {
                "reply_id": current.reply_id,
                "updated_at": current.updated_at,
                "transcript": current.transcript,
                "answer": current.answer,
                "tts_text": current.tts_text,
                "tts_available": current.tts_available,
                "tts_mime": current.tts_mime,
                "action": dict(current.action),
            }

    def latest_audio(self) -> tuple[str, str, bytes]:
        with self._lock:
            current = self._voice_reply
            return current.reply_id, current.tts_mime, bytes(current.tts_audio)


runtime_state = RuntimeState()
