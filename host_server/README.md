# Otto Host Server

这是“主机增强模式”的第一版本地服务骨架。

当前目标不是一次性做完整语音闭环，而是先完成两件事：

1. 在主机侧建立独立的 Qwen 调用入口
2. 验证本地服务可以稳定访问 Qwen，并拿到可解析结果

当前已经进一步补上了一条可直接给 ESP32 使用的闭环接口：

3. ESP32 将一段录音通过 HTTP 发给主机
4. 主机完成 ASR + planner 后直接返回动作 JSON

## 当前能力

- `GET /healthz`
- `POST /api/qwen/test/sample-asr`
- `POST /api/qwen/test/plan`
- `POST /api/qwen/test/connectivity`
- `POST /api/voice/command`
- `WS /ws/esp32`

其中：

- `sample-asr` 会下载官方样例音频并调用 `qwen3-asr-flash`
- `plan` 会把文本送给动作规划模型，要求返回结构化 JSON
- `connectivity` 会连续执行一次 ASR 和一次动作规划
- `voice/command` 会接收开发板上传的 Base64 音频，返回最终动作 JSON
- `ws/esp32` 是后续给开发板接入主机增强模式时使用的预留通道

## 配置来源

服务默认按下面顺序读取配置：

1. 环境变量
2. `AI_Desktop_Assistant/main/main.ino` 中现有的 Qwen 常量

优先使用的环境变量：

```bash
export DASHSCOPE_API_KEY="sk-xxx"
export QWEN_IMAGE_API_URL="https://.../compatible-mode/v1"
export QWEN_VOICE_API_URL="https://dashscope.aliyuncs.com/compatible-mode/v1"
export QWEN_IMAGE_MODEL="qwen3.5-omni-plus"
export QWEN_VOICE_MODEL="qwen3-asr-flash"
export QWEN_TTS_API_URL="https://dashscope.aliyuncs.com/compatible-mode/v1"
export QWEN_TTS_VOICE="Tina"
export QWEN_TTS_FORMAT="wav"
```

## 安装与启动

建议在项目根目录创建虚拟环境：

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r host_server/requirements.txt
```

如果本机已经有 `fastapi/uvicorn`，可以直接使用：

```bash
uvicorn host_server.app:app --reload --host 127.0.0.1 --port 9000
```

如果从项目根目录启动失败，可改成：

```bash
PYTHONPATH=. uvicorn host_server.app:app --reload --host 127.0.0.1 --port 9000
```

如果像当前这台机器一样暂时没有 `pip/venv`，可以直接用标准库版本启动：

```bash
python3 -m host_server.min_server
```

注意：

- 这条命令需要在项目根目录 `Otto/` 下执行
- 如果你当前人在别的目录，例如 `desktop_debug_panel/`，请改用下面这条，它不依赖当前工作目录：

```bash
python3 /home/thd/CS/Project/Otto/run_host_server.py
```

如果 `9000` 端口已被旧服务占用，可以临时换端口：

```bash
HOST_SERVER_PORT=9001 python3 /home/thd/CS/Project/Otto/run_host_server.py
```

## 快速测试

浏览器控制台：

```bash
xdg-open http://127.0.0.1:9000/
```

或者直接在浏览器访问：

```text
http://127.0.0.1:9000/
```

健康检查：

```bash
curl http://127.0.0.1:9000/healthz
```

官方样例 ASR：

```bash
curl -X POST http://127.0.0.1:9000/api/qwen/test/sample-asr
```

动作规划测试：

```bash
curl -X POST http://127.0.0.1:9000/api/qwen/test/plan \
  -H 'Content-Type: application/json' \
  -d '{"transcript":"请帮我开灯"}'
```

完整联通性测试：

```bash
curl -X POST http://127.0.0.1:9000/api/qwen/test/connectivity
```

开发板语音命令接口测试：

```bash
curl -X POST http://127.0.0.1:9000/api/voice/command \
  -H 'Content-Type: application/json' \
  -d '{"audio_base64":"<base64>","audio_format":"wav"}'
```

## 下一步

当前版本通过后，下一步就可以开始接：

1. ESP32 `host_claim / host_release`
2. 板端唤醒后通过 HTTP 上传语音命令
3. 主机返回统一动作 JSON
4. 再升级到真正的 WebSocket / PCM 流式模式
5. 主机 TTS 与板端播放
