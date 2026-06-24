from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_MAIN = PROJECT_ROOT / "AI_Desktop_Assistant" / "main" / "main.ino"


def _read_firmware_text() -> str:
    try:
        return FIRMWARE_MAIN.read_text(encoding="utf-8")
    except OSError:
        return ""


def _extract_c_string(source: str, name: str) -> str | None:
    pattern = rf'const char\* {re.escape(name)}\s*=\s*"([^"]*)";'
    match = re.search(pattern, source)
    return match.group(1) if match else None


@dataclass(slots=True)
class Settings:
    dashscope_api_key: str
    image_api_url: str
    voice_api_url: str
    tts_api_url: str
    image_model: str
    voice_model: str
    tts_voice: str
    tts_format: str
    host: str = "127.0.0.1"
    port: int = 9000

    @property
    def configured(self) -> bool:
        return all(
            [
                self.dashscope_api_key,
                self.image_api_url,
                self.voice_api_url,
                self.image_model,
                self.voice_model,
            ]
        )

    @property
    def image_chat_url(self) -> str:
        return normalize_chat_url(self.image_api_url)

    @property
    def voice_chat_url(self) -> str:
        return normalize_chat_url(self.voice_api_url)

    @property
    def tts_chat_url(self) -> str:
        return normalize_chat_url(self.tts_api_url)


def normalize_chat_url(url: str) -> str:
    url = (url or "").strip()
    if not url:
        return ""
    if url.endswith("/chat/completions"):
        return url
    if url.endswith("/"):
        url = url[:-1]
    if url.endswith("/compatible-mode/v1"):
        return url + "/chat/completions"
    return url


def load_settings() -> Settings:
    firmware_text = _read_firmware_text()

    def pick(env_name: str, firmware_name: str) -> str:
        env_value = os.getenv(env_name, "").strip()
        if env_value:
            return env_value
        return (_extract_c_string(firmware_text, firmware_name) or "").strip()

    host = os.getenv("HOST_SERVER_BIND", "127.0.0.1").strip() or "127.0.0.1"
    port_text = os.getenv("HOST_SERVER_PORT", "9000").strip() or "9000"
    try:
        port = int(port_text)
    except ValueError:
        port = 9000

    return Settings(
        dashscope_api_key=pick("DASHSCOPE_API_KEY", "QWEN_API_KEY"),
        image_api_url=pick("QWEN_IMAGE_API_URL", "QWEN_IMAGE_API_URL"),
        voice_api_url=pick("QWEN_VOICE_API_URL", "QWEN_VOICE_API_URL"),
        tts_api_url=os.getenv("QWEN_TTS_API_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1").strip()
        or "https://dashscope.aliyuncs.com/compatible-mode/v1",
        image_model=pick("QWEN_IMAGE_MODEL", "QWEN_IMAGE_MODEL"),
        voice_model=pick("QWEN_VOICE_MODEL", "QWEN_VOICE_MODEL"),
        tts_voice=os.getenv("QWEN_TTS_VOICE", "Tina").strip() or "Tina",
        tts_format=os.getenv("QWEN_TTS_FORMAT", "wav").strip() or "wav",
        host=host,
        port=port,
    )
