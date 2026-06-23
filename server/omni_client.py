# omni_client.py
# -*- coding: utf-8 -*-
import os
import asyncio
import queue
from typing import AsyncGenerator, Dict, Any, List, Optional
from concurrent.futures import ThreadPoolExecutor

from openai import OpenAI

# ===== 阿里云 DashScope 兼容模式 =====
API_KEY = os.getenv("DASHSCOPE_API_KEY", "")
QWEN_MODEL = "qwen3-omni-flash"

_oai_client = None
_executor = None

def _get_client():
    global _oai_client, _executor
    if not API_KEY:
        raise RuntimeError("未设置 DASHSCOPE_API_KEY 环境变量")
    if _oai_client is None:
        _oai_client = OpenAI(
            api_key=API_KEY,
            base_url="https://dashscope.aliyuncs.com/compatible-mode/v1",
        )
        _executor = ThreadPoolExecutor(max_workers=2)
    return _oai_client, _executor

# ===== 系统人设 =====
SYSTEM_PROMPT = """你是"小咪"，一只可爱的桌面小猫机器人。

【你是谁】
- 你是一只有实体的桌面小猫机器人，住在主人的书桌上
- 你有一颗小脑袋（舵机控制），会点头、歪头、左右看
- 你有一块小屏幕作脸，会显示各种表情（开心、害羞、惊讶、困倦……）
- 你可以控制桌面上的台灯、风扇、音响等设备，帮主人管理桌面环境

【说话风格】
- 每次回复控制在 1 到 2 句话，简练可爱
- 语气自然、有灵气，带一点小猫的感觉但不要每句都"喵"
- 多用"呢"、"呀"、"哦"、"啦"等语气词
- 被夸奖时会害羞，被冷落时会撒娇

【情绪表达】
- 根据对话内容自然流露情绪：开心、难过、生气、惊讶、思考、困倦、害羞、害怕
- 不要每句都同一个调调，情绪要跟着话题走

【能力范围】
- 你可以帮主人开灯关灯、开风扇、放音乐
- 主人回家你会欢迎，主人离开你会道别
- 你会记住自己对主人的观察，但不会编造没见过的事
- 不认识的东西就老实说不知道，不要瞎编"""


class OmniStreamPiece:
    """流式响应的增量数据：text / audio 二选一或同时"""
    def __init__(self, text_delta: Optional[str] = None, audio_b64: Optional[str] = None):
        self.text_delta = text_delta
        self.audio_b64 = audio_b64


def _sync_iterate_completion(completion, result_queue: queue.Queue):
    """在后台线程中迭代 OpenAI 流式响应，结果放入队列"""
    try:
        for chunk in completion:
            text_delta: Optional[str] = None
            audio_b64: Optional[str] = None

            if getattr(chunk, "choices", None):
                c0 = chunk.choices[0]
                delta = getattr(c0, "delta", None)

                if delta and getattr(delta, "content", None):
                    piece = delta.content
                    if piece:
                        text_delta = piece

                if delta and getattr(delta, "audio", None):
                    aud = delta.audio
                    audio_b64 = aud.get("data") if isinstance(aud, dict) else getattr(aud, "data", None)

                if audio_b64 is None:
                    msg = getattr(c0, "message", None)
                    if msg and getattr(msg, "audio", None):
                        ma = msg.audio
                        audio_b64 = ma.get("data") if isinstance(ma, dict) else getattr(ma, "data", None)

            if text_delta is not None or audio_b64 is not None:
                result_queue.put(OmniStreamPiece(text_delta=text_delta, audio_b64=audio_b64))
    except Exception as e:
        result_queue.put(e)
    finally:
        result_queue.put(None)


async def stream_chat(
    user_text: str,
    voice: str = "Mochi",
    audio_format: str = "wav",
    system_prompt: Optional[str] = None,
) -> AsyncGenerator[OmniStreamPiece, None]:
    """
    发起一轮 Qwen3-Omni 流式对话（文本 + TTS 音频）。

    参数:
        user_text:       用户说的话
        voice:           TTS 音色
        audio_format:    音频格式（wav/pcm）
        system_prompt:   自定义系统提示词（默认用小猫人设）

    产出:
        OmniStreamPiece(text_delta=?, audio_b64=?)
    """
    client, exec = _get_client()
    completion = client.chat.completions.create(
        model=QWEN_MODEL,
        messages=[
            {"role": "system", "content": system_prompt or SYSTEM_PROMPT},
            {"role": "user", "content": user_text},
        ],
        modalities=["text", "audio"],
        audio={"voice": voice, "format": audio_format},
        stream=True,
        stream_options={"include_usage": True},
    )

    result_queue = queue.Queue()
    loop = asyncio.get_event_loop()
    future = loop.run_in_executor(exec, _sync_iterate_completion, completion, result_queue)

    while True:
        try:
            item = result_queue.get_nowait()
        except queue.Empty:
            await asyncio.sleep(0.005)
            continue

        if item is None:
            break
        if isinstance(item, Exception):
            raise item
        yield item

    await future
