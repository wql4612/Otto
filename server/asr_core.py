# asr_core.py
# -*- coding: utf-8 -*-
"""
DashScope ASR 实时语音识别回调模块

封装 paraformer-realtime-v2 的识别结果处理：
- partial 结果用于 UI 临时展示
- final 结果用于触发 LLM 对话
- 冷却期内忽略识别结果（防止识别到 AI TTS 音频）
"""

import os
import json
import time
import asyncio
from typing import Any, Dict, List, Optional, Callable, Tuple

ASR_DEBUG_RAW = os.getenv("ASR_DEBUG_RAW", "0") == "1"


def _shorten(s: str, limit: int = 200) -> str:
    if not s:
        return ""
    return s if len(s) <= limit else (s[:limit] + "...")


def _safe_to_dict(x: Any) -> Dict[str, Any]:
    if isinstance(x, dict):
        return x
    for attr in ("to_dict", "model_dump", "__dict__"):
        try:
            v = getattr(x, attr, None)
        except Exception:
            v = None
        if callable(v):
            try:
                d = v()
                if isinstance(d, dict):
                    return d
            except Exception:
                pass
        elif isinstance(v, dict):
            return v
    try:
        s = str(x)
        if s and s.lstrip().startswith("{") and s.rstrip().endswith("}"):
            return json.loads(s)
    except Exception:
        pass
    return {"_raw": str(x)}


def _extract_sentence(event_obj: Any) -> Tuple[Optional[str], Optional[bool]]:
    """从 ASR 事件中提取 sentence text 和 sentence_end 标记"""
    d = _safe_to_dict(event_obj)
    cands: List[Dict[str, Any]] = [d]
    for k in ("output", "data", "result"):
        v = d.get(k)
        if isinstance(v, dict):
            cands.append(v)
    for obj in cands:
        sent = obj.get("sentence")
        if isinstance(sent, dict):
            text = sent.get("text")
            is_end = sent.get("sentence_end")
            if is_end is not None:
                is_end = bool(is_end)
            return text, is_end
    for obj in cands:
        if "text" in obj and isinstance(obj.get("text"), str):
            return obj.get("text"), None
    return None, None


# ===== ASR 全局总闸 =====
_current_recognition: Optional[object] = None
_rec_lock = asyncio.Lock()


async def set_current_recognition(r):
    global _current_recognition
    async with _rec_lock:
        _current_recognition = r


async def stop_current_recognition():
    global _current_recognition
    async with _rec_lock:
        r = _current_recognition
        _current_recognition = None
    if r:
        try:
            r.stop()
        except Exception:
            pass


# ===== ASR 回调 =====
class ASRCallback:
    """
    处理 DashScope 实时 ASR 识别结果：

    1. partial 结果 → 仅用于 UI 展示（广播给前端 / ESP32）
    2. final 结果 → 触发 LLM 对话
    3. 冷却期内忽略识别（防止 AI TTS 被 ASR 识别后循环触发）
    4. AI 播放期间忽略识别（防止自己跟自己说话）
    """

    def __init__(
        self,
        on_sdk_error: Callable[[str], None],
        post: Callable[[asyncio.Future], None],
        broadcast_partial: Callable,
        broadcast_final: Callable,
        is_playing_now_fn: Callable[[], bool],
        start_ai_fn: Callable,          # async (text: str)
        interrupt_lock: asyncio.Lock,
    ):
        self._on_sdk_error = on_sdk_error
        self._post = post
        self._last_partial: str = ""
        self._last_final: str = ""

        self._broadcast_partial = broadcast_partial
        self._broadcast_final = broadcast_final
        self._is_playing = is_playing_now_fn
        self._start_ai = start_ai_fn
        self._interrupt_lock = interrupt_lock

        self._session_start_time = time.time()
        self._cooldown_seconds = float(os.getenv("ASR_COOLDOWN_SEC", "3.0"))

    def on_open(self):
        pass

    def on_close(self):
        pass

    def on_complete(self):
        pass

    def reset_state(self):
        """清空内部状态，防止旧识别结果残留"""
        self._last_partial = ""
        self._last_final = ""
        self._session_start_time = time.time()
        print("[ASR] 回调状态已重置", flush=True)

    def on_error(self, err):
        try:
            self._post(self._broadcast_partial(""))
            self._on_sdk_error(str(err))
        except Exception:
            pass

    def on_result(self, result):
        self._handle(result)

    def on_event(self, event):
        self._handle(event)

    def _handle(self, event: Any):
        if ASR_DEBUG_RAW:
            try:
                rawd = _safe_to_dict(event)
                print("[ASR RAW]", json.dumps(rawd, ensure_ascii=False), flush=True)
            except Exception:
                pass

        text, is_end = _extract_sentence(event)
        if text is None:
            return
        text = text.strip()
        if not text:
            return

        # ---- 冷却期：忽略识别（防止 AI TTS 被当作输入） ----
        elapsed = time.time() - self._session_start_time
        if elapsed < self._cooldown_seconds:
            print(f"[ASR COOLDOWN] {elapsed:.1f}s < {self._cooldown_seconds}s, 忽略: '{_shorten(text)}'", flush=True)
            return

        # ---- AI 播放时忽略识别 ----
        if self._is_playing():
            print(f"[ASR IGNORED] AI 正在播放, 忽略: '{_shorten(text)}'", flush=True)
            return

        # ---- partial：仅 UI 展示 ----
        self._last_partial = text
        try:
            self._post(self._broadcast_partial(text))
        except Exception:
            pass

        # ---- final：触发 LLM ----
        if is_end is True:
            final_text = text
            try:
                self._post(self._broadcast_final(final_text))
            except Exception:
                pass

            if not self._is_playing() and final_text:
                async def _run():
                    async with self._interrupt_lock:
                        print(f"[ASR FINAL] → {final_text}", flush=True)
                        await self._start_ai(final_text)
                try:
                    self._post(_run())
                except Exception:
                    pass

            self._last_partial = ""
            self._last_final = ""
