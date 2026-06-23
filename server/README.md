# 桌面小猫机器人 — 后端服务

## 环境要求

- Python 3.9+
- Windows / macOS / Linux

## 快速启动

### 1. 安装依赖

```bash
cd server
pip install -r requirements.txt
```

### 2. 配置 API Key

**Windows (CMD):**
```cmd
set DASHSCOPE_API_KEY=你的阿里云DashScope API Key
```

**Windows (PowerShell):**
```powershell
$env:DASHSCOPE_API_KEY="你的阿里云DashScope API Key"
```

**macOS / Linux:**
```bash
export DASHSCOPE_API_KEY=你的阿里云DashScope API Key
```

> 获取 API Key：登录 [阿里云百炼控制台](https://bailian.console.aliyun.com/) → 模型广场 → API Key 管理。

### 3. 启动服务

```bash
python app.py
```

启动成功后会显示：

```
==================================================
  桌面小猫机器人后端服务
==================================================
  ASR:        可用
  API Key:    已配置
  监听地址:   http://0.0.0.0:8765
  ASR 模型:   paraformer-realtime-v2
  对话模型:   qwen3-omni-flash
==================================================
```

## 端点说明

| 端点 | 类型 | 用途 |
|------|------|------|
| `/ws_audio` | WebSocket | ESP32 音频流 + 指令收发 |
| `/ws/ui` | WebSocket | 浏览器调试面板 |
| `/stream.wav` | HTTP | TTS 音频流（浏览器播放） |
| `/health` | HTTP GET | 健康检查 |
| `/` | HTTP GET | 服务信息 |

## 可选配置

通过环境变量设置：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `DASHSCOPE_API_KEY` | (必填) | 阿里云 API Key |
| `SERVER_HOST` | `0.0.0.0` | 监听地址 |
| `SERVER_PORT` | `8765` | 监听端口 |
| `ASR_COOLDOWN_SEC` | `3.0` | ASR 冷却期（秒） |
| `QWEN_MODEL` | `qwen-turbo` | 情绪分析用模型 |

## 通信协议

### ESP32 → 服务器

| 消息 | 说明 |
|------|------|
| `"START"` (文本) | 开始 ASR 识别 |
| `"STOP"` (文本) | 停止 ASR 识别 |
| PCM 音频 (二进制) | 16kHz 16bit mono，640 bytes/帧 |

### 服务器 → ESP32（JSON 文本）

| type | 说明 |
|------|------|
| `{"type":"cmd","command":"CMD_LIGHT_ON"}` | 匹配到设备指令 |
| `{"type":"chat","text":"..."}` | AI 对话回复文本 |
| `{"type":"tts_start"}` | 即将发送 TTS 音频 |
| `{"type":"audio_done"}` | TTS 播放完毕 |
| `{"type":"asr_partial","text":"..."}` | ASR 中间结果 |
| `{"type":"asr_final","text":"..."}` | ASR 最终结果 |
| `{"type":"emotion","emotion":"happy","led":{...}}` | AI 回复情绪 |

TTS 音频以二进制 PCM 帧发送，24kHz 16bit mono，960 bytes/帧（约 20ms）。

## 验证

浏览器访问 `http://你的电脑IP:8765/health`：

```json
{
  "ok": true,
  "asr_available": true,
  "api_key_configured": true,
  "esp32_connected": false,
  "ui_clients": 0
}
```

`esp32_connected` 为 `true` 即表示 ESP32 已成功连接。
