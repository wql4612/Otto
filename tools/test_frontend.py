#!/usr/bin/env python3
"""
前端控制面板独立测试服务器
用法: python test_frontend.py
然后在浏览器打开 http://localhost:8080

用途: 组员C无需ESP32硬件即可调试前端页面
      服务器模拟ESP32的WebSocket消息、MJPEG流、设备状态
"""

import asyncio
import json
import time
import random
import struct
import io
import os
import sys
from http import HTTPStatus
from pathlib import Path

# ────────── 配置 ──────────
HOST = "0.0.0.0"
PORT = 8080
PANEL_FILE = Path(__file__).parent.parent / "control_panel.html"

# ────────── 模拟设备状态 ──────────
class MockState:
    def __init__(self):
        self.person = False
        self.light = False
        self.fan = False
        self.speaker = False
        self.connected_clients = set()

state = MockState()

# ────────── WebSocket 帧处理 ──────────
def ws_read_frame(data: bytes) -> tuple:
    """读取 WebSocket 帧, 返回 (opcode, payload)"""
    if len(data) < 2:
        return None, None
    opcode = data[0] & 0x0F
    masked = (data[1] & 0x80) != 0
    length = data[1] & 0x7F
    offset = 2
    if length == 126:
        if len(data) < 4: return None, None
        length = struct.unpack('>H', data[2:4])[0]
        offset = 4
    elif length == 127:
        if len(data) < 10: return None, None
        length = struct.unpack('>Q', data[2:10])[0]
        offset = 10

    if not masked:
        return opcode, data[offset:offset+length]

    mask = data[offset:offset+4]
    offset += 4
    payload = bytearray(length)
    for i in range(length):
        payload[i] = data[offset + i] ^ mask[i % 4]
    return opcode, bytes(payload)

def ws_make_frame(payload: bytes, opcode: int = 0x01) -> bytes:
    """构造 WebSocket 文本帧 (unmasked)"""
    length = len(payload)
    frame = bytearray()
    frame.append(0x80 | opcode)
    if length <= 125:
        frame.append(length)
    elif length <= 65535:
        frame.append(126)
        frame.extend(struct.pack('>H', length))
    else:
        frame.append(127)
        frame.extend(struct.pack('>Q', length))
    frame.extend(payload)
    return bytes(frame)

# ────────── 模拟相机帧 ──────────
def make_mock_jpeg() -> bytes:
    """生成一个简单的模拟 JPEG 帧 (1×1 灰色像素)"""
    # 最小的有效 JPEG: 1×1 灰色像素
    # 这是 JPEG baseline DCT, 仅供参考; 实际会显示为灰点
    fake = bytearray()
    # SOI
    fake.extend(b'\xff\xd8')
    # APP0
    fake.extend(b'\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00')
    # DQT
    fake.extend(b'\xff\xdb\x00\x43\x00\x10\x0b\x0c\x0e\x0c\x0a\x10\x0e\x0d\x0e\x12\x11\x10\x13\x18\x28\x1a\x18\x16\x16\x18\x31\x23\x25\x1d\x28\x3a\x33\x3d\x3c\x39\x33\x38\x37\x40\x48\x5c\x4e\x40\x44\x57\x45\x37\x38\x50\x6d\x51\x57\x5f\x62\x67\x68\x67\x3e\x4d\x71\x79\x70\x64\x78\x5c\x65\x67\x63')
    # SOF0: 1×1
    fake.extend(b'\xff\xc0\x00\x0b\x08\x00\x01\x00\x01\x01\x01\x11\x00')
    # DHT
    fake.extend(b'\xff\xc4\x00\x1f\x00\x00\x01\x05\x01\x01\x01\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b')
    fake.extend(b'\xff\xc4\x00\xb5\x10\x00\x02\x01\x03\x03\x02\x04\x03\x05\x05\x04\x04\x00\x00\x01\x7d\x01\x02\x03\x00\x04\x11\x05\x12\x21\x31\x41\x06\x13\x51\x61\x07\x22\x71\x14\x32\x81\x91\xa1\x08\x23\x42\xb1\xc1\x15\x52\xd1\xf0\x24\x33\x62\x72\x82\x09\x0a\x16\x17\x18\x19\x1a\x25\x26\x27\x28\x29\x2a\x34\x35\x36\x37\x38\x39\x3a\x43\x44\x45\x46\x47\x48\x49\x4a\x53\x54\x55\x56\x57\x58\x59\x5a\x63\x64\x65\x66\x67\x68\x69\x6a\x73\x74\x75\x76\x77\x78\x79\x7a\x83\x84\x85\x86\x87\x88\x89\x8a\x92\x93\x94\x95\x96\x97\x98\x99\x9a\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa')
    # SOS
    fake.extend(b'\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00\x7f\x00')
    # EOI
    fake.extend(b'\xff\xd9')
    return bytes(fake)

# ────────── HTTP 请求处理 ──────────
async def handle_http(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    try:
        request_line = await asyncio.wait_for(reader.readline(), timeout=10)
    except asyncio.TimeoutError:
        writer.close()
        return

    if not request_line:
        writer.close()
        return

    parts = request_line.decode('utf-8', errors='ignore').strip().split()
    if len(parts) < 2:
        writer.close()
        return

    method, path = parts[0], parts[1]

    # 读取所有 headers
    headers = {}
    while True:
        line = await reader.readline()
        if not line or line == b'\r\n':
            break
        try:
            k, v = line.decode('utf-8', errors='ignore').strip().split(':', 1)
            headers[k.lower().strip()] = v.strip()
        except ValueError:
            pass

    # WebSocket 升级
    if headers.get('upgrade', '').lower() == 'websocket':
        await handle_websocket(reader, writer, headers)
        return

    # ── HTTP Routes ──
    if method == 'GET':
        if path == '/' or path == '/index.html':
            serve_file(writer, PANEL_FILE, "text/html; charset=utf-8")
        elif path == '/stream':
            await stream_mjpeg(reader, writer)
            return
        else:
            writer.write(b"HTTP/1.1 404 Not Found\r\n\r\n")
    else:
        writer.write(b"HTTP/1.1 405 Method Not Allowed\r\n\r\n")

    await writer.drain()
    writer.close()

def serve_file(writer, path: Path, content_type: str):
    if not path.exists():
        writer.write(b"HTTP/1.1 404 Not Found\r\n\r\n")
        return
    content = path.read_bytes()
    resp = (
        f"HTTP/1.1 200 OK\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(content)}\r\n"
        f"Access-Control-Allow-Origin: *\r\n"
        f"Cache-Control: no-cache\r\n"
        f"\r\n"
    ).encode()
    writer.write(resp + content)

# ────────── MJPEG 模拟流 ──────────
async def stream_mjpeg(reader, writer):
    boundary = "FRAME_BOUNDARY"
    writer.write(
        f"HTTP/1.1 200 OK\r\n"
        f"Content-Type: multipart/x-mixed-replace; boundary={boundary}\r\n"
        f"Access-Control-Allow-Origin: *\r\n"
        f"\r\n"
    ).encode()

    counter = 0
    while True:
        try:
            # 生成一个简单的彩色帧 (模拟变化)
            img_data = make_mock_jpeg()
            frame = (
                f"--{boundary}\r\n"
                f"Content-Type: image/jpeg\r\n"
                f"Content-Length: {len(img_data)}\r\n"
                f"\r\n"
            ).encode() + img_data + b"\r\n"

            writer.write(frame)
            await writer.drain()
            await asyncio.sleep(0.1)  # ~10 FPS
            counter += 1
        except (ConnectionError, BrokenPipeError):
            break
    writer.close()

# ────────── WebSocket 处理 ──────────
async def handle_websocket(reader, writer, headers):
    """WebSocket 升级 + 双向通信"""
    # 计算 accept key
    import hashlib, base64
    ws_key = headers.get('sec-websocket-key', '')
    accept = base64.b64encode(
        hashlib.sha1((ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
    ).decode()

    # 发送升级响应
    resp = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n"
        "\r\n"
    ).encode()
    writer.write(resp)
    await writer.drain()

    state.connected_clients.add(writer)
    client_id = id(writer)
    print(f"[WS] Client {client_id} connected ({len(state.connected_clients)} total)")

    # 发送初始状态
    send_ws(writer, make_status_json())
    send_ws(writer, json.dumps({"type":"camera","on":True}))

    # 发送欢迎日志
    send_ws(writer, json.dumps({
        "type": "log", "category": "system",
        "message": "测试服务器已连接 · 可用遥控设备模拟"
    }))

    try:
        while True:
            # 读取 WebSocket 帧头 (2 bytes minimum)
            header = await asyncio.wait_for(reader.readexactly(2), timeout=60)
            opcode_byte = header[0]
            len_byte = header[1]
            opcode = opcode_byte & 0x0F
            masked = (len_byte & 0x80) != 0
            length = len_byte & 0x7F

            if length == 126:
                ext = await reader.readexactly(2)
                length = struct.unpack('>H', ext)[0]
            elif length == 127:
                ext = await reader.readexactly(8)
                length = struct.unpack('>Q', ext)[0]

            if masked:
                mask = await reader.readexactly(4)
                payload = bytearray(length)
                raw = await reader.readexactly(length)
                for i in range(length):
                    payload[i] = raw[i] ^ mask[i % 4]
            else:
                payload = await reader.readexactly(length)

            msg = bytes(payload).decode('utf-8', errors='ignore')

            if opcode == 0x08:  # Close
                print(f"[WS] Client {client_id} closing")
                break

            if opcode == 0x09:  # Ping
                pong = bytearray(header[:2])
                pong[0] = 0x8A  # Pong
                writer.write(pong)
                await writer.drain()
                continue

            if opcode == 0x01:  # Text
                await process_cmd(msg)

    except asyncio.TimeoutError:
        pass
    except (ConnectionError, BrokenPipeError, asyncio.IncompleteReadError):
        pass
    finally:
        state.connected_clients.discard(writer)
        print(f"[WS] Client {client_id} disconnected ({len(state.connected_clients)} remaining)")
        writer.close()

async def process_cmd(msg: str):
    """处理前端发来的控制指令"""
    try:
        data = json.loads(msg)
    except json.JSONDecodeError:
        return

    cmd_type = data.get("type", "")
    ts = time.strftime("%H:%M:%S")

    if cmd_type == "cmd":
        dev = data.get("device", "")
        action = data.get("action", "")
        on = action == "on"

        if dev == "light":
            state.light = on
        elif dev == "fan":
            state.fan = on
        elif dev == "speaker":
            state.speaker = on

        name_map = {"light": "台灯", "fan": "风扇", "speaker": "音响"}
        print(f"[Cmd] {name_map.get(dev, dev)} → {'开' if on else '关'} ({ts})")

    elif cmd_type == "scene":
        scene = data.get("name", "")
        if scene == "home":
            state.light = True
            print(f"[Scene] 回家模式: 开灯 ({ts})")
        elif scene == "away":
            state.light = False
            state.fan = False
            state.speaker = False
            print(f"[Scene] 离开模式: 全关 ({ts})")

    # 广播状态更新
    broadcast(make_status_json())

def send_ws(writer, msg: str):
    """发送 WebSocket 文本帧"""
    try:
        frame = ws_make_frame(msg.encode('utf-8'))
        writer.write(frame)
    except Exception:
        pass

def broadcast(msg: str):
    """广播给所有 WebSocket 客户端"""
    dead = set()
    for writer in state.connected_clients:
        try:
            send_ws(writer, msg)
        except Exception:
            dead.add(writer)
    state.connected_clients -= dead

def make_status_json() -> str:
    return json.dumps({
        "type": "status",
        "person": state.person,
        "light": state.light,
        "fan": state.fan,
        "speaker": state.speaker
    })

# ────────── 模拟人体检测 ──────────
async def mock_person_detection():
    """每隔 10-20 秒随机模拟人进入/离开"""
    while True:
        await asyncio.sleep(random.uniform(12, 22))
        state.person = not state.person
        ts = time.strftime("%H:%M:%S")
        if state.person:
            state.light = True  # 自动开灯
            print(f"[Mock] 检测到人进入 → 自动开灯 ({ts})")
            broadcast(json.dumps({
                "type": "log", "category": "detect",
                "message": "检测到人进入 → 自动开灯"
            }))
        else:
            state.light = False  # 自动关灯
            print(f"[Mock] 人已离开 → 延时关灯 ({ts})")
            broadcast(json.dumps({
                "type": "log", "category": "detect",
                "message": "人已离开 → 延时关灯"
            }))
        broadcast(make_status_json())

# ────────── 主入口 ──────────
async def main():
    if not PANEL_FILE.exists():
        print(f"[Error] 找不到控制面板文件: {PANEL_FILE}")
        print(f"       请确保 control_panel.html 在项目根目录")
        sys.exit(1)

    server = await asyncio.start_server(handle_http, HOST, PORT)

    print("=" * 58)
    print("  🖥  桌面机器人 · 前端控制面板 · 测试服务器")
    print("=" * 58)
    print(f"  📁 面板文件: {PANEL_FILE}")
    print(f"  🌐 浏览器打开: http://localhost:{PORT}")
    print(f"  🔌 WebSocket:   ws://localhost:{PORT}/ws")
    print(f"  📷 MJPEG:       http://localhost:{PORT}/stream")
    print("-" * 58)
    print("  功能说明:")
    print("  · 自动模拟人进入/离开 (检测→开关灯)")
    print("  · 点击设备按钮 → WebSocket → 模拟状态更新")
    print("  · 回家模式/离开模式 一键切换")
    print("  · 事件日志滚动显示")
    print("-" * 58)
    print("  按 Ctrl+C 停止服务器")
    print("=" * 58)

    # 启动模拟器
    asyncio.create_task(mock_person_detection())

    async with server:
        await server.serve_forever()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[Server] 已停止")
