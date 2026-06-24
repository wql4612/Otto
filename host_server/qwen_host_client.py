from __future__ import annotations

import base64
import json
from dataclasses import dataclass
from typing import Any

import requests

from .config import Settings


OFFICIAL_SAMPLE_AUDIO_URL = "https://dashscope.oss-cn-beijing.aliyuncs.com/audios/welcome.mp3"


class QwenRequestError(RuntimeError):
    pass


@dataclass(slots=True)
class QwenHostClient:
    settings: Settings
    timeout_s: int = 60

    def _headers(self) -> dict[str, str]:
        return {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self.settings.dashscope_api_key}",
        }

    def _post_chat(self, url: str, payload: dict[str, Any]) -> dict[str, Any]:
        response = requests.post(
            url,
            headers=self._headers(),
            json=payload,
            timeout=self.timeout_s,
        )
        if response.status_code < 200 or response.status_code >= 300:
            raise QwenRequestError(
                f"Qwen HTTP {response.status_code}: {response.text[:300]}"
            )
        return response.json()

    def _post_chat_stream(self, url: str, payload: dict[str, Any]) -> list[dict[str, Any]]:
        response = requests.post(
            url,
            headers=self._headers(),
            json=payload,
            timeout=(15, self.timeout_s),
            stream=True,
        )
        if response.status_code < 200 or response.status_code >= 300:
            raise QwenRequestError(
                f"Qwen HTTP {response.status_code}: {response.text[:300]}"
            )

        events: list[dict[str, Any]] = []
        try:
            for raw_line in response.iter_lines(decode_unicode=True):
                if not raw_line:
                    continue
                line = raw_line.strip()
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if not data or data == "[DONE]":
                    continue
                try:
                    parsed = json.loads(data)
                except json.JSONDecodeError as exc:
                    raise QwenRequestError(f"Invalid stream event: {data[:200]}") from exc
                events.append(parsed)
        finally:
            response.close()

        return events

    @staticmethod
    def _extract_text(response_json: dict[str, Any]) -> str:
        try:
            content = response_json["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as exc:
            raise QwenRequestError(f"Unexpected Qwen response: {response_json}") from exc

        if isinstance(content, str):
            return content.strip()

        if isinstance(content, list):
            text_parts: list[str] = []
            for item in content:
                if isinstance(item, dict):
                    text = item.get("text")
                    if isinstance(text, str):
                        text_parts.append(text)
            merged = "".join(text_parts).strip()
            if merged:
                return merged

        raise QwenRequestError(f"Unable to extract text from response: {response_json}")

    @staticmethod
    def _extract_json_object_text(text: str) -> str:
        text = (text or "").strip()
        if not text:
            raise QwenRequestError("Empty planner response")

        start = text.find("{")
        if start < 0:
            raise QwenRequestError(f"Planner response is not JSON: {text}")

        depth = 0
        in_string = False
        escaped = False
        for index in range(start, len(text)):
            char = text[index]
            if in_string:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    in_string = False
                continue

            if char == '"':
                in_string = True
                continue
            if char == "{":
                depth += 1
                continue
            if char == "}":
                depth -= 1
                if depth == 0:
                    return text[start:index + 1]

        raise QwenRequestError(f"Incomplete planner JSON: {text}")

    @staticmethod
    def _extract_delta_text(delta: Any) -> str:
        if not isinstance(delta, dict):
            return ""

        content = delta.get("content")
        if isinstance(content, str):
            return content
        if isinstance(content, list):
            parts: list[str] = []
            for item in content:
                if isinstance(item, dict):
                    text = item.get("text")
                    if isinstance(text, str):
                        parts.append(text)
            return "".join(parts)
        return ""

    @staticmethod
    def _extract_delta_audio_chunk(delta: Any) -> bytes:
        if not isinstance(delta, dict):
            return b""
        audio = delta.get("audio")
        if not isinstance(audio, dict):
            return b""
        data = audio.get("data")
        if not isinstance(data, str) or not data:
            return b""
        try:
            return base64.b64decode(data)
        except Exception as exc:
            raise QwenRequestError("Invalid audio chunk in stream response") from exc

    def synthesize_answer_audio(self, answer_text: str) -> dict[str, Any]:
        answer_text = (answer_text or "").strip()
        if not answer_text:
            raise QwenRequestError("Empty answer text for TTS")

        payload = {
            "model": self.settings.image_model,
            "messages": [
                {
                    "role": "system",
                    "content": "请把用户提供的文本直接朗读出来，不要改写，不要补充，不要省略。",
                },
                {"role": "user", "content": answer_text},
            ],
            "modalities": ["text", "audio"],
            "audio": {
                "voice": self.settings.tts_voice,
                "format": self.settings.tts_format,
            },
            "stream": True,
        }

        stream_events = self._post_chat_stream(self.settings.tts_chat_url, payload)
        text_parts: list[str] = []
        audio_parts: list[bytes] = []

        for event in stream_events:
            choices = event.get("choices")
            if not isinstance(choices, list) or not choices:
                continue
            delta = choices[0].get("delta")
            if not isinstance(delta, dict):
                continue
            text_delta = self._extract_delta_text(delta)
            if text_delta:
                text_parts.append(text_delta)
            audio_chunk = self._extract_delta_audio_chunk(delta)
            if audio_chunk:
                audio_parts.append(audio_chunk)

        audio_bytes = b"".join(audio_parts)
        tts_text = "".join(text_parts).strip() or answer_text
        if not audio_bytes:
            raise QwenRequestError("TTS stream returned no audio bytes")

        return {
            "model": self.settings.image_model,
            "voice": self.settings.tts_voice,
            "format": self.settings.tts_format,
            "tts_text": tts_text,
            "audio_bytes": audio_bytes,
        }

    @staticmethod
    def _audio_data_url(audio_bytes: bytes, fmt: str) -> str:
        encoded = base64.b64encode(audio_bytes).decode("utf-8")
        return f"data:audio/{fmt};base64,{encoded}"

    def recognize_audio_bytes(self, audio_bytes: bytes, fmt: str = "wav") -> dict[str, Any]:
        if not audio_bytes:
            raise QwenRequestError("Empty audio bytes")

        payload = {
            "model": self.settings.voice_model,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "input_audio",
                            "input_audio": {
                                "data": self._audio_data_url(audio_bytes, fmt)
                            },
                        }
                    ],
                }
            ],
            "extra_body": {"asr_options": {"language": "zh", "enable_itn": False}},
        }

        raw = self._post_chat(self.settings.voice_chat_url, payload)
        transcript = self._extract_text(raw)
        return {
            "audio_bytes": len(audio_bytes),
            "model": self.settings.voice_model,
            "transcript": transcript,
            "raw": raw,
        }

    def run_sample_asr(self) -> dict[str, Any]:
        sample_resp = requests.get(OFFICIAL_SAMPLE_AUDIO_URL, timeout=self.timeout_s)
        if sample_resp.status_code != 200:
            raise QwenRequestError(
                f"Sample audio download failed: HTTP {sample_resp.status_code}"
            )

        payload = {
            "model": self.settings.voice_model,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "input_audio",
                            "input_audio": {
                                "data": self._audio_data_url(sample_resp.content, "mp3")
                            },
                        }
                    ],
                }
            ],
            "extra_body": {"asr_options": {"language": "zh", "enable_itn": False}},
        }

        raw = self._post_chat(self.settings.voice_chat_url, payload)
        transcript = self._extract_text(raw)
        return {
            "sample_url": OFFICIAL_SAMPLE_AUDIO_URL,
            "audio_bytes": len(sample_resp.content),
            "model": self.settings.voice_model,
            "transcript": transcript,
            "raw": raw,
        }

    def plan_action(self, transcript: str) -> dict[str, Any]:
        system_prompt = (
            "你是桌面助手的动作规划器。"
            "你必须只返回一个 JSON 对象，不要返回 Markdown，不要返回额外解释。"
            "JSON 至少包含字段 result, face, servo, answer, light。"
            "result 只能是 succeed 或 error。"
            "face 只能是 happy、confused、surprised、idle、listening 之一。"
            "servo 只能是 happy、confused、surprised、none 之一。"
            "light 只能是 on、off、keep 之一。"
            "answer 必须是给用户的一句简短中文回复。"
            "如果用户要求开灯、打开灯、亮灯，则 light=on。"
            "如果用户要求关灯、关闭灯、熄灯，则 light=off。"
            "如果没有提到灯，则 light=keep。"
            "如果用户表达困惑、没听懂、疑问，face=confused, servo=confused。"
            "如果用户表达震惊、惊讶，face=surprised, servo=surprised。"
            "如果用户表达开心、夸奖、欢迎、积极肯定，face=happy, servo=happy。"
            "如果只是普通控制指令且没有明显情绪，face=idle, servo=none。"
            "如果文本无法理解，result=error, face=confused, servo=confused, light=keep。"
        )

        payload = {
            "model": self.settings.image_model,
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": f"用户语音转写：{transcript}"},
            ],
            "response_format": {"type": "json_object"},
            "max_tokens": 220,
        }

        raw = self._post_chat(self.settings.image_chat_url, payload)
        action_text = self._extract_text(raw)
        action_json = self._extract_json_object_text(action_text)
        return {
            "transcript": transcript,
            "model": self.settings.image_model,
            "action_text": action_json,
            "action_json": json.loads(action_json),
            "raw": raw,
        }

    def run_audio_command(self, audio_bytes: bytes, fmt: str = "wav") -> dict[str, Any]:
        asr = self.recognize_audio_bytes(audio_bytes, fmt=fmt)
        return self.run_text_command(asr["transcript"], audio_bytes=len(audio_bytes))

    def run_text_command(self, transcript: str, audio_bytes: int = 0) -> dict[str, Any]:
        planner = self.plan_action(transcript)
        action = planner["action_json"]
        answer = str(action.get("answer", "")).strip()
        tts = self.synthesize_answer_audio(answer)
        return {
            "voice": {
                "model": self.settings.voice_model,
                "transcript": transcript,
                "audio_bytes": audio_bytes,
            },
            "planner": {
                "model": planner["model"],
                "transcript": planner["transcript"],
                "action_text": planner["action_text"],
                "action_json": planner["action_json"],
            },
            "tts": {
                "model": tts["model"],
                "voice": tts["voice"],
                "format": tts["format"],
                "tts_text": tts["tts_text"],
                "audio_bytes": len(tts["audio_bytes"]),
            },
            "action": action,
            "tts_audio_bytes": tts["audio_bytes"],
        }

    def connectivity_report(self) -> dict[str, Any]:
        sample = self.run_sample_asr()
        planner = self.plan_action("请帮我开灯")
        return {
            "voice": {
                "model": sample["model"],
                "transcript": sample["transcript"],
                "audio_bytes": sample["audio_bytes"],
            },
            "planner": {
                "model": planner["model"],
                "transcript": planner["transcript"],
                "action_text": planner["action_text"],
            },
        }
