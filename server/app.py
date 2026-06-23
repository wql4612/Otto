# app.py - 桌面小猫机器人后端服务
# 功能：接收 ESP32 音频流 → DashScope ASR → Qwen3-Omni 对话 → 回传指令/TTS
# -*- coding: utf-8 -*-

import os
import sys
import json
import time
import base64
import asyncio
from typing import Optional, List, Dict, Set

# audioop 在 Python 3.13 被移除，用自定义实现替代
try:
    import audioop as _audioop
    def pcm_scale(data: bytes, factor: float) -> bytes:
        return _audioop.mul(data, 2, factor)
except ModuleNotFoundError:
    import array
    def pcm_scale(data: bytes, factor: float) -> bytes:
        """16-bit PCM 音量缩放"""
        samples = array.array('h', data)
        for i in range(len(samples)):
            v = int(samples[i] * factor)
            samples[i] = max(-32768, min(32767, v))
        return samples.tobytes()

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.responses import PlainTextResponse
from starlette.websockets import WebSocketState
import uvicorn

# Windows 事件循环
if sys.platform.startswith("win"):
    try:
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    except Exception:
        pass

# 加载 .env 文件（如果存在）
try:
    from dotenv import load_dotenv
    load_dotenv()
except Exception:
    pass

# ===== DashScope ASR =====
try:
    from dashscope import audio as dash_audio
    ASR_AVAILABLE = True
except ImportError:
    ASR_AVAILABLE = False
    print("[WARNING] dashscope 未安装，ASR 不可用")

from asr_core import ASRCallback, set_current_recognition, stop_current_recognition
from omni_client import stream_chat
from audio_stream import (
    register_stream_route,
    broadcast_pcm16_realtime,
    send_pcm_to_esp32,
    hard_reset_audio,
    is_playing_now,
    stream_clients,
    STREAM_SR,
)
from emotion_parser import analyze_emotion, EMOTION_LED_MAP
from voice_command import match_command

# ===== 配置 =====
API_KEY = os.getenv("DASHSCOPE_API_KEY", "")
if not API_KEY:
    print("[WARNING] DASHSCOPE_API_KEY 未设置！语音识别和对话将不可用。")
    print("         请设置环境变量: set DASHSCOPE_API_KEY=你的key")

ASR_MODEL = "paraformer-realtime-v2"
SAMPLE_RATE = 16000
AUDIO_FMT = "pcm"
CHUNK_MS = 20
BYTES_CHUNK = SAMPLE_RATE * CHUNK_MS // 1000 * 2   # 640 bytes
SILENCE_20MS = bytes(BYTES_CHUNK)

SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.getenv("SERVER_PORT", "8765"))

app = FastAPI(title="桌面小猫机器人后端")

# ===== 状态容器 =====
ui_clients: Dict[int, WebSocket] = {}
_next_ui_id = 1

esp32_audio_ws: Optional[WebSocket] = None
current_partial: str = ""
recent_finals: List[str] = []
RECENT_MAX = 20

interrupt_lock = asyncio.Lock()

# ===== 注册音频流路由 =====
register_stream_route(app)


# ===== 通用广播 =====
async def broadcast_to_ui(msg: str):
    """广播文本消息到所有浏览器 UI 客户端"""
    dead = []
    for cid, ws in list(ui_clients.items()):
        try:
            if ws.client_state == WebSocketState.CONNECTED:
                await ws.send_text(msg)
            else:
                dead.append(cid)
        except Exception:
            dead.append(cid)
    for cid in dead:
        ui_clients.pop(cid, None)


async def broadcast_to_esp32(msg: str):
    """发送文本消息到 ESP32"""
    if esp32_audio_ws and esp32_audio_ws.client_state == WebSocketState.CONNECTED:
        try:
            await esp32_audio_ws.send_text(msg)
        except Exception:
            pass


async def ui_broadcast_partial(text: str):
    """广播 ASR partial 结果（仅 UI 展示）"""
    global current_partial
    current_partial = text
    await broadcast_to_ui("PARTIAL:" + text)
    await broadcast_to_esp32(json.dumps({"type": "asr_partial", "text": text}))


async def ui_broadcast_final(text: str):
    """广播 ASR final 结果"""
    global current_partial
    current_partial = ""
    recent_finals.append(text)
    if len(recent_finals) > RECENT_MAX:
        recent_finals = recent_finals[-RECENT_MAX:]
    await broadcast_to_ui("FINAL:" + text)
    await broadcast_to_esp32(json.dumps({"type": "asr_final", "text": text}))
    print(f"[ASR FINAL] {text}", flush=True)


async def on_sdk_error(err: str):
    """ASR SDK 出错回调"""
    print(f"[ASR ERROR] {err}", flush=True)
    await broadcast_to_ui("ERROR:" + err)
    await broadcast_to_esp32(json.dumps({"type": "error", "message": err}))


# ===== 系统重置 =====
async def full_system_reset(reason: str = ""):
    """清空当前状态，重置 ASR"""
    await hard_reset_audio(reason or "full_system_reset")
    await stop_current_recognition()
    global current_partial
    current_partial = ""
    try:
        await broadcast_to_esp32(json.dumps({"type": "reset"}))
    except Exception:
        pass
    print(f"[SYSTEM] Reset done. reason={reason}", flush=True)


# ===== AI 对话核心 =====
async def start_ai_with_text(user_text: str):
    """
    收到用户语音文本后，先检查是否匹配设备控制指令。
    匹配 → 直接发送指令给 ESP32 + 简短 TTS 确认
    不匹配 → 走 Qwen3-Omni 对话 + TTS 回复
    """
    txt_buf: List[str] = []
    emotion_analyzed = False

    # ── ① 先检查是否匹配设备控制指令 ──
    cmd_result = match_command(user_text)
    if cmd_result:
        cmd_id, matched_kw = cmd_result
        print(f"[CMD] 匹配指令: '{matched_kw}' → {cmd_id}", flush=True)

        # 发送指令给 ESP32
        await broadcast_to_esp32(json.dumps({
            "type": "cmd",
            "command": cmd_id,
            "keyword": matched_kw,
        }))

        # 简短 TTS 确认（不走完整对话，只做语气确认）
        confirm_text = f"好的，{matched_kw}~"
        await _send_tts_confirm(confirm_text)

        # 广播确认文本
        await broadcast_to_ui(f"[CMD] {matched_kw} → {cmd_id}")
        await broadcast_to_esp32(json.dumps({
            "type": "chat",
            "text": confirm_text,
        }))
        return

    # ── ② 非指令 → Qwen3-Omni 对话 ──
    async def _runner():
        nonlocal txt_buf, emotion_analyzed

        try:
            async for piece in stream_chat(user_text, voice="Mochi", audio_format="wav"):
                # 文本增量
                if piece.text_delta:
                    txt_buf.append(piece.text_delta)
                    full_text = "".join(txt_buf)

                    # 情绪分析（一次就够了）
                    if not emotion_analyzed and len(full_text) >= 10:
                        emotion_analyzed = True
                        try:
                            loop = asyncio.get_event_loop()
                            emotion = await loop.run_in_executor(None, analyze_emotion, full_text)

                            # 发送情绪 + 对应的 LED 颜色给 ESP32（如果 ESP32 有 LED）
                            led = EMOTION_LED_MAP.get(emotion, (255, 255, 255, 120))
                            await broadcast_to_esp32(json.dumps({
                                "type": "emotion",
                                "emotion": emotion,
                                "led": {"r": led[0], "g": led[1], "b": led[2]},
                            }))
                            print(f"[EMOTION] {emotion}", flush=True)
                        except Exception as e:
                            print(f"[EMOTION] 分析失败: {e}", flush=True)

                    # 实时广播 AI 增量文本
                    await broadcast_to_ui(f"[AI] {full_text}")
                    await broadcast_to_esp32(json.dumps({
                        "type": "chat_partial",
                        "text": full_text,
                    }))

                # 音频增量
                if piece.audio_b64:
                    try:
                        pcm24 = base64.b64decode(piece.audio_b64)
                    except Exception:
                        pcm24 = b""
                    if pcm24:
                        # TTS 为 24kHz，稍降音量后发送
                        pcm16 = pcm_scale(pcm24, 0.80)
                        if pcm16:
                            # 发送到 ESP32（二进制 PCM）
                            await send_pcm_to_esp32(esp32_audio_ws, pcm16)
                            # 同时发送到浏览器流客户端
                            if stream_clients:
                                await broadcast_pcm16_realtime(pcm16)

        except asyncio.CancelledError:
            raise
        except Exception as e:
            error_text = f"抱歉，我好像卡住了…（{e}）"
            await broadcast_to_esp32(json.dumps({"type": "chat", "text": error_text}))
        finally:
            # 通知流客户端结束
            for sc in list(stream_clients):
                if not sc.abort_event.is_set():
                    try:
                        sc.q.put_nowait(b"\x00" * 64)
                    except Exception:
                        pass
                    try:
                        sc.q.put_nowait(None)
                    except Exception:
                        pass

            # 广播最终 AI 文本
            final_text = "".join(txt_buf).strip() or "……"
            await broadcast_to_ui(f"[AI] {final_text}")
            await broadcast_to_esp32(json.dumps({
                "type": "chat",
                "text": final_text,
            }))

            # 短暂停顿后通知 ESP32 可以重新开始 ASR
            await asyncio.sleep(0.5)
            await broadcast_to_esp32(json.dumps({
                "type": "audio_done",
            }))
            print(f"[AI] 回复完成", flush=True)

    # 清场后启动 AI 任务
    await hard_reset_audio("start_ai")
    await stop_current_recognition()
    global current_partial
    current_partial = ""

    # 通知 ESP32 停止发音频，准备收 TTS
    await broadcast_to_esp32(json.dumps({"type": "tts_start"}))

    loop = asyncio.get_running_loop()
    from audio_stream import __dict__ as _ad
    task = loop.create_task(_runner())
    _ad["current_ai_task"] = task


async def _send_tts_confirm(text: str):
    """发送简短的 TTS 确认语音"""
    content_list = [{"type": "text", "text": text}]
    try:
        async for piece in stream_chat(text, voice="Mochi", audio_format="wav"):
            if piece.audio_b64:
                try:
                    pcm24 = base64.b64decode(piece.audio_b64)
                except Exception:
                    pcm24 = b""
                if pcm24:
                    pcm16 = pcm_scale(pcm24, 0.80)
                    if pcm16:
                        await send_pcm_to_esp32(esp32_audio_ws, pcm16)
                # 忽略文本输出（确认消息不需要显示）
    except Exception:
        pass
    await asyncio.sleep(0.3)
    await broadcast_to_esp32(json.dumps({"type": "audio_done"}))


async def stop_rec(send_notice: str = ""):
    """停止当前 ASR 识别会话"""
    r = await stop_current_recognition()
    if r:
        # 喂一段静音让 ASR 结束当前句子
        for _ in range(10):
            try:
                r.send_audio_frame(SILENCE_20MS)
            except Exception:
                break
    if send_notice:
        await broadcast_to_esp32(send_notice)


# ===== WebSocket: ESP32 音频入口 =====
@app.websocket("/ws_audio")
async def ws_audio(ws: WebSocket):
    global esp32_audio_ws

    # 拒绝重复连接
    if esp32_audio_ws is not None:
        try:
            if esp32_audio_ws.client_state == WebSocketState.CONNECTED:
                await ws.close(code=1013, reason="Another ESP32 already connected")
                return
        except Exception:
            pass
        esp32_audio_ws = None

    await ws.accept()
    esp32_audio_ws = ws
    print("[AUDIO] ESP32 已连接", flush=True)

    recognition = None
    streaming = False
    last_ts = time.monotonic()

    async def stop_ws_rec():
        nonlocal streaming, recognition
        streaming = False
        if recognition:
            try:
                for _ in range(10):
                    try:
                        recognition.send_audio_frame(SILENCE_20MS)
                    except Exception:
                        break
            except Exception:
                pass
            try:
                recognition.stop()
            except Exception:
                pass
            recognition = None
        await set_current_recognition(None)

    async def keepalive_loop():
        nonlocal last_ts, streaming, recognition
        try:
            while streaming and recognition is not None:
                await asyncio.sleep(0.10)
                now = time.monotonic()
                if now - last_ts > 2.0:
                    try:
                        recognition.send_audio_frame(SILENCE_20MS)
                        last_ts = time.monotonic()
                    except Exception:
                        await on_sdk_error("keepalive 发送失败")
                        return
        except asyncio.CancelledError:
            return

    try:
        while True:
            if ws.client_state != WebSocketState.CONNECTED:
                break
            try:
                msg = await ws.receive()
            except WebSocketDisconnect:
                break
            except RuntimeError as e:
                if "Cannot call" in str(e):
                    break
                raise

            # ── 文本消息（控制命令）──
            if "text" in msg and msg["text"]:
                raw = (msg["text"] or "").strip()
                cmd = raw.upper()

                if cmd == "START":
                    print("[AUDIO] START 收到，开启 ASR", flush=True)
                    await stop_ws_rec()
                    global current_partial
                    current_partial = ""
                    await broadcast_to_ui("")
                    await asyncio.sleep(0.3)

                    if ASR_AVAILABLE and API_KEY:
                        loop = asyncio.get_running_loop()

                        def post(coro):
                            asyncio.run_coroutine_threadsafe(coro, loop)

                        cb = ASRCallback(
                            on_sdk_error=lambda s: post(on_sdk_error(s)),
                            post=post,
                            broadcast_partial=ui_broadcast_partial,
                            broadcast_final=ui_broadcast_final,
                            is_playing_now_fn=is_playing_now,
                            start_ai_fn=start_ai_with_text,
                            interrupt_lock=interrupt_lock,
                        )

                        try:
                            print("[AUDIO] 创建 DashScope ASR Recognition...", flush=True)
                            recognition = dash_audio.asr.Recognition(
                                api_key=API_KEY,
                                model=ASR_MODEL,
                                format=AUDIO_FMT,
                                sample_rate=SAMPLE_RATE,
                                callback=cb,
                            )
                            recognition.start()
                            await set_current_recognition(recognition)
                            streaming = True
                            last_ts = time.monotonic()
                            asyncio.create_task(keepalive_loop())
                            print("[AUDIO] ASR 启动成功", flush=True)
                        except Exception as e:
                            print(f"[AUDIO] ASR 启动失败: {e}", flush=True)
                            import traceback
                            traceback.print_exc()
                    else:
                        print(f"[AUDIO] ASR 不可用! ASR_AVAILABLE={ASR_AVAILABLE}, API_KEY={'***' if API_KEY else 'None'}", flush=True)

                    await ws.send_text("OK:STARTED")

                elif cmd == "STOP":
                    await stop_ws_rec()
                    await ws.send_text("OK:STOPPED")

                else:
                    # 尝试解析 JSON（ESP32 发来的其他消息）
                    try:
                        data = json.loads(raw)
                        if data.get("type") == "ping":
                            await ws.send_text(json.dumps({"type": "pong"}))
                    except json.JSONDecodeError:
                        pass

            # ── 二进制消息（PCM 音频）──
            elif "bytes" in msg and msg["bytes"]:
                if is_playing_now():
                    continue  # AI 播放时丢弃音频
                if streaming and recognition:
                    try:
                        recognition.send_audio_frame(msg["bytes"])
                        last_ts = time.monotonic()
                    except Exception:
                        await on_sdk_error("send_audio_frame 失败")

    except Exception as e:
        print(f"[AUDIO] 错误: {e}", flush=True)
    finally:
        await stop_ws_rec()
        if esp32_audio_ws is ws:
            esp32_audio_ws = None
        print("[AUDIO] ESP32 已断开", flush=True)


# ===== WebSocket: 浏览器 UI =====
@app.websocket("/ws/ui")
async def ws_ui(ws: WebSocket):
    global _next_ui_id
    await ws.accept()
    cid = _next_ui_id
    _next_ui_id += 1
    ui_clients[cid] = ws
    print(f"[UI] 浏览器已连接 (client={cid})", flush=True)

    try:
        while True:
            try:
                msg = await ws.receive()
                if "text" in msg and msg["text"]:
                    data = json.loads((msg["text"] or "").strip())
                    if data.get("type") == "prompt":
                        # 浏览器主动发送对话文本
                        text = data.get("text", "")
                        if text:
                            async with interrupt_lock:
                                await start_ai_with_text(text)
            except WebSocketDisconnect:
                break
    except Exception:
        pass
    finally:
        ui_clients.pop(cid, None)
        print(f"[UI] 浏览器已断开 (client={cid})", flush=True)


# ===== HTTP 端点 =====
@app.get("/health")
async def health_check():
    """健康检查"""
    return {
        "ok": True,
        "asr_available": ASR_AVAILABLE,
        "api_key_configured": bool(API_KEY),
        "esp32_connected": esp32_audio_ws is not None and esp32_audio_ws.client_state == WebSocketState.CONNECTED,
        "ui_clients": len(ui_clients),
    }


@app.get("/")
async def index():
    """简单首页"""
    return PlainTextResponse(
        "桌面小猫机器人后端服务\n"
        f"ASR: {'可用' if ASR_AVAILABLE else '不可用'}\n"
        f"API Key: {'已配置' if API_KEY else '未配置'}\n"
        f"ESP32: {'已连接' if esp32_audio_ws else '未连接'}\n"
        "\n端点:\n"
        "  /ws_audio  - ESP32 音频 WebSocket\n"
        "  /ws/ui     - 浏览器 UI WebSocket\n"
        "  /stream.wav  - TTS 音频流 (HTTP)\n"
        "  /health    - 健康检查\n"
    )


# ===== 启动 =====
if __name__ == "__main__":
    print("=" * 50)
    print("  桌面小猫机器人后端服务")
    print("=" * 50)
    print(f"  ASR:        {'可用' if ASR_AVAILABLE else '不可用 (pip install dashscope)'}")
    print(f"  API Key:    {'已配置' if API_KEY else '未配置!'}")
    print(f"  监听地址:   http://{SERVER_HOST}:{SERVER_PORT}")
    print(f"  ASR 模型:   {ASR_MODEL}")
    print(f"  对话模型:   qwen3-omni-flash")
    print("=" * 50)

    uvicorn.run(app, host=SERVER_HOST, port=SERVER_PORT, log_level="info")
