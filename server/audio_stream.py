# audio_stream.py
# -*- coding: utf-8 -*-
"""
TTS 音频流管理

- 管理 AI TTS 播放的当前任务
- 向 ESP32 WebSocket 推送二进制 PCM 音频
- 可选 /stream.wav HTTP 端点供浏览器播放
"""

import asyncio
from dataclasses import dataclass
from typing import Optional, Set
from fastapi import Request
from fastapi.responses import StreamingResponse

# ===== 音频参数 =====
# TTS 输出为 24kHz 16bit mono PCM → WAV 封装
STREAM_SR = 24000
STREAM_CH = 1
STREAM_SW = 2
BYTES_PER_20MS_16K = STREAM_SR * STREAM_SW * 20 // 1000   # 960 bytes / 20ms

# ===== AI 播放任务总闸 =====
current_ai_task: Optional[asyncio.Task] = None


async def cancel_current_ai():
    """取消当前 AI 语音播放任务"""
    global current_ai_task
    task = current_ai_task
    current_ai_task = None
    if task and not task.done():
        task.cancel()
        try:
            await task
        except (asyncio.CancelledError, Exception):
            pass


def is_playing_now() -> bool:
    t = current_ai_task
    return (t is not None) and (not t.done())


# ===== HTTP WAV 流客户端管理 =====
@dataclass(frozen=True)
class StreamClient:
    q: asyncio.Queue
    abort_event: asyncio.Event

stream_clients: Set[StreamClient] = set()
STREAM_QUEUE_MAX = 96


def _wav_header_unknown_size(sr=24000, ch=1, sw=2) -> bytes:
    import struct
    byte_rate = sr * ch * sw
    block_align = ch * sw
    data_size = 0x7FFFFFF0
    riff_size = 36 + data_size
    return struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", riff_size, b"WAVE",
        b"fmt ", 16,
        1, ch, sr, byte_rate, block_align, sw * 8,
        b"data", data_size,
    )


async def hard_reset_audio(reason: str = ""):
    """一键清场：断开所有播放连接 + 取消 AI 任务"""
    for sc in list(stream_clients):
        try:
            sc.abort_event.set()
        except Exception:
            pass
    stream_clients.clear()
    await cancel_current_ai()
    if reason:
        print(f"[AUDIO RESET] {reason}", flush=True)


async def send_pcm_to_esp32(esp32_ws, pcm16: bytes):
    """
    以 20ms 节拍把 PCM 音频分片发送给 ESP32。

    ESP32 端会把这些二进制帧排队到播放缓冲区。
    """
    if not esp32_ws:
        return

    off = 0
    total = len(pcm16)
    while off < total:
        take = min(BYTES_PER_20MS_16K, total - off)
        piece = pcm16[off: off + take]
        try:
            await esp32_ws.send_bytes(piece)
        except Exception:
            break
        off += take
        await asyncio.sleep(0.018)  # 略小于 20ms，保证播放平滑


async def broadcast_pcm16_realtime(pcm16: bytes):
    """以 20ms 节拍把 PCM 音频发送给所有 HTTP 流客户端"""
    loop = asyncio.get_event_loop()
    next_tick = loop.time()
    off = 0
    while off < len(pcm16):
        take = min(BYTES_PER_20MS_16K, len(pcm16) - off)
        piece = pcm16[off: off + take]

        dead = []
        for sc in list(stream_clients):
            if sc.abort_event.is_set():
                dead.append(sc)
                continue
            try:
                if sc.q.full():
                    try:
                        sc.q.get_nowait()
                    except Exception:
                        pass
                sc.q.put_nowait(piece)
            except Exception:
                dead.append(sc)
        for sc in dead:
            try:
                stream_clients.discard(sc)
            except Exception:
                pass

        next_tick += 0.020
        now = loop.time()
        if now < next_tick:
            await asyncio.sleep(next_tick - now)
        else:
            next_tick = now
        off += take


def register_stream_route(app):
    """注册 /stream.wav HTTP 端点（供浏览器播放 TTS）"""

    @app.get("/stream.wav")
    async def stream_wav(_: Request):
        # 断开旧连接
        for sc in list(stream_clients):
            try:
                sc.abort_event.set()
            except Exception:
                pass
        stream_clients.clear()

        q: asyncio.Queue = asyncio.Queue(maxsize=STREAM_QUEUE_MAX)
        abort_event = asyncio.Event()
        sc = StreamClient(q=q, abort_event=abort_event)
        stream_clients.add(sc)

        async def gen():
            yield _wav_header_unknown_size(STREAM_SR, STREAM_CH, STREAM_SW)
            try:
                while True:
                    if abort_event.is_set():
                        break
                    try:
                        chunk = await asyncio.wait_for(q.get(), timeout=0.5)
                    except asyncio.TimeoutError:
                        continue
                    if abort_event.is_set():
                        break
                    if chunk is None:
                        break
                    if chunk:
                        yield chunk
            finally:
                stream_clients.discard(sc)

        return StreamingResponse(gen(), media_type="audio/wav")
