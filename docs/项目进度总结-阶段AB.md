# 桌面机器人项目 — 阶段 A & B 进度总结

> **日期**: 2026-06-19  
> **仓库**: https://github.com/wql4612/Otto.git  
> **分支**: master (c978ceb + 工作区变更)

---

## 一、项目概述

基于 ESP32S3 的桌面机器人，全本地推理（语音+视觉），通过 51 单片机 + 433MHz RF 控制台灯/风扇/音响。核心设计原则：**不上云，不训练大模型，全部用现成的本地分类模型**。

---

## 二、阶段 A：代码整合（已完成）

### 目标
消除代码重复、统一架构、打通硬件驱动层到 Web 控制面板的完整链路。

### 变更文件

| 文件 | 操作 | 说明 |
|------|------|------|
| `wifi_driver.h` | **重写** | 从 ESPAsyncWebServer（外部依赖）迁移到同步 WebServer（内置）；新增 LittleFS、WebSocket 广播、WS 消息回调、`wifi_handle_client()` |
| `wifi_driver.cpp` | **重写** | 统一 WebServer：内嵌默认首页 + MJPEG 摄像头流 `/stream` + LittleFS 静态文件服务 + 完整 WebSocket 协议（握手/帧解析/广播）+ REST API（`/api/status`, `/api/action`, `/api/upload`） |
| `screen_driver.h` | **更新** | 新增 `FaceIndex` 枚举（7 种表情）、`FACE_FILES[]` 路径映射、`screen_show_face_jpeg()` |
| `screen_driver.cpp` | **更新** | 新增 JPEG → RGB565 解码显示（JPEGDEC 回调 + `drawRGBBitmap`），表情从 LittleFS 读取 |
| `main.ino` | **重写 (v3)** | 10 步初始化流程、系统状态机、WebSocket 前端协议兼容、7 表情切换、定时状态广播、FreeRTOS loopback 保留、AI 管线预留 |
| `data/` | **新建** | `control_panel.html` + 7 张 `faces/*.jpg`（LittleFS 上传就绪） |
| `DEPLOY.md` | **更新** | 反映新架构的 API 端点表、调试命令表、库依赖 |
| `esp32_webserver.h` | **废弃标记** | 功能已整合到 wifi_driver |

### 审查确认的关键决策

| 问题 | 决策 | 原因 |
|------|------|------|
| I2S 扬声器引脚 | BCLK=6, LRC=5, DOUT=1 | 板载 PDM 麦克已占用 41/42，需避免冲突 |
| SPI LCD 引脚 | CS=2, DC=3 | 实际接线如此 |
| WiFi 凭据 | 暂时硬编码 | 后续可改为配置文件 |
| 舵机引脚 | SG90=21, MG996R=44 | 与设计方案一致 |
| WebServer 实现 | 同步 WebServer | 无外部依赖，与 esp32_webserver.h 一致 |

### 架构变化

```
之前（碎片化）:
  wifi_driver.cpp → ESPAsyncWebServer（外部依赖）
  esp32_webserver.h → 同步 WebServer（独立模块）
  main.ino → 仅调试命令，无集成

之后（统一）:
  wifi_driver → 同步 WebServer（完整功能）
  screen_driver → 文字 + JPEG 表情
  main.ino → 全集成入口
```

---

## 三、阶段 B：AI 模型集成（已完成）

### 目标
接入帧差法运动检测（立即可用），预留 Edge Impulse 人体检测和语音识别接口。

### 技术路线（调研结论）

| 模块 | 方案 | 原因 |
|------|------|------|
| 运动检测 | 帧差法（纯代码） | 无外部依赖 |
| 人体检测 | Edge Impulse FOMO（占位） | SSCMA 需要外部硬件模块；SenseCraft 模型无法嵌入 Arduino 代码 |
| 语音识别 | Edge Impulse KWS（占位） | Arduino IDE 原生支持；可训练自定义唤醒词"小猫咪" |

### 新建文件

| 文件 | 功能 | 状态 |
|------|------|------|
| `vision_driver.h` | 人体检测 + 运动检测统一接口 | **完整** |
| `vision_driver.cpp` | JPEG 字节流采样帧差法 + Edge Impulse FOMO 占位 | **完整** |
| `voice_driver.h` | 语音识别接口 + `VoiceCommand` 枚举（15 条指令） | **完整** |
| `voice_driver.cpp` | PDM 麦克风检测 + Edge Impulse KWS 占位（含训练指引） | **完整** |
| `command_map.h` | 15 条语音指令 + 3 种视觉事件 → 设备动作映射 | **完整** |

### 核心设计决策

**帧差法实现方式**：JPEG 字节流采样对比
- 每帧均匀采样 200 字节，无需 JPEG 解码
- 单字节差异 > 25 视为"变化"，变化比例 > 15% 触发运动事件
- 速度快（<5ms），不干扰 MJPEG 流

**人体检测降级策略**（无 Edge Impulse 模型时）：
- 帧差比值 > 30% → 视为"有人"（代理判断）
- 连续 10 秒低于阈值 → 视为"离开"

**Edge Impulse 条件编译模式**：
```cpp
// 模型训练完成后取消注释即可激活：
// #define HAS_EI_VOICE_MODEL        // 语音识别
// #define HAS_EI_PERSON_DETECTION   // 人体检测
```

### 系统主循环管线

```
┌────────────────────────────────────────────┐
│ loop() @ ~100Hz                            │
│                                            │
│ 1. FreeRTOS 命令队列处理                    │
│ 2. Loopback 录音→播放状态机                 │
│ 3. WiFi/WebSocket 请求处理                  │
│ 4. 定时状态广播（每 2 秒）                   │
│ 5. vision_loop() → 帧差法检测              │
│ 6. 视觉事件 → execute_vision_event()       │
│    ├─ PERSON_ENTER → 开灯 + 😊 + 欢迎音     │
│    ├─ PERSON_LEAVE → 关灯 + 💤 + 待机       │
│    └─ MOTION       → 😲 + 日志             │
│ 7. voice_loop() → 语音识别（占位）          │
│ 8. 语音指令 → execute_voice_command()      │
│    └─ 15 条指令 → 灯光/风扇/音响/舵机/场景   │
└────────────────────────────────────────────┘
```

### 15 条语音指令映射

| ID | 指令 | 动作 |
|----|------|------|
| 0 | 开灯 | `g_sys.light_on = true` + UART（阶段C） |
| 1 | 关灯 | `g_sys.light_on = false` + UART（阶段C） |
| 2 | 开风扇 | `g_sys.fan_on = true` + RF 发射 |
| 3 | 关风扇 | `g_sys.fan_on = false` + RF 发射 |
| 4 | 放音乐 | `g_sys.speaker_on = true` |
| 5 | 停下 | 全关 + RF 发射 |
| 6-7 | 亮一点/暗一点 | PWM 调光（阶段C） |
| 8 | 你好 | 舵机点头 + 😊 + 提示音 |
| 9 | 过来 | 舵机招手 + 🥺 |
| 10 | 休闲模式 | 开灯 + 🥺 + 音响开 |
| 11 | 工作模式 | 开灯 + 风扇开 + 😐 |
| 12 | 现在几点 | 🤔（阶段C 显示时间） |
| 13 | 回家模式 | 开灯 + 😊 |
| 14 | 离开模式 | 全关 + 💤 + RF 发射 |

---

## 四、完整文件清单

```
AI_Desktop_Assistant/
├── main/
│   ├── main.ino              ← 主固件 v3（全集成入口）
│   ├── command_map.h          ← [新] 指令→动作映射表
│   ├── vision_driver.h/cpp    ← [新] 人体检测 + 运动检测
│   ├── voice_driver.h/cpp     ← [新] 语音识别（占位）
│   ├── wifi_driver.h/cpp      ← [改] WiFi + WebServer + WebSocket
│   ├── screen_driver.h/cpp    ← [改] TFT 显示 + JPEG 表情
│   ├── camera_driver.h/cpp    ← 摄像头驱动
│   ├── mic_driver.h/cpp       ← PDM 麦克风驱动
│   ├── speaker_driver.h/cpp   ← I2S 扬声器驱动
│   ├── servo_driver.h/cpp     ← SG90 + MG996R 舵机
│   ├── rf_driver.h/cpp        ← 433MHz RF 发射
│   ├── sd_driver.h/cpp        ← SD 卡
│   ├── player_driver.h/cpp    ← 音频播放
│   └── data/                  ← [新] LittleFS 上传目录
│       ├── control_panel.html
│       └── faces/ (7 张 JPG)
├── rf_receiver.c              ← 51 单片机 RF 接收
esp32_webserver/
├── esp32_webserver.h          ← [已废弃] 功能已整合
└── DEPLOY.md                  ← [更新] 部署指南 v2
lcd_faces/                     ← 表情源文件（PNG/JPG/.h）
tools/
├── generate_face_sprites.py   ← 表情生成器
└── test_frontend.py           ← 前端测试服务器
control_panel.html             ← 控制面板源文件
设计方案.md                     ← 完整设计文档
```

---

## 五、API 端点（Web 控制）

| 路径 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 控制面板（LittleFS）或内嵌调试页 |
| `/stream` | GET | MJPEG 摄像头实时流 |
| `/ws` | GET | WebSocket 升级（实时状态推送） |
| `/api/status` | GET | JSON 完整状态查询 |
| `/api/action?cmd=xxx` | POST | 调试命令执行 |
| `/api/upload` | POST | RGB565 图片上传到屏幕 |
| `/faces/*` | GET | 表情 JPG 文件 |

### 调试命令速查

`ping` `wifi_status` `capture` `mic_diag` `screen_demo`  
`servo180_0/90/180` `servo360_fwd/rev/stop` `rf_on/off`  
`face_happy/idle/sleep/listening/surprised/cute/confused`  
`mic_record` `loopback` `tone_16k` `tone_44k`

---

## 六、硬件引脚确认

| 功能 | 引脚 | 来源 |
|------|------|------|
| LCD SCLK/MOSI/DC/CS | 2 / 3 / 4 / 5 | screen_driver 默认 |
| LCD RST/BL | 6 / 7 | screen_driver 默认 |
| I2S 功放 BCLK/LRC/DOUT | 6 / 5 / 1 | speaker_driver 默认 |
| PDM 麦克 CLK/DIN | 42 / 41 | mic_driver 硬编码（板载） |
| SG90 舵机 | 21 | main.ino |
| MG996R 舵机 | 44 | main.ino |
| 433MHz RF TX | 43 | main.ino |
| UART → 51 MCU | TX=9, RX=8 | 预留 |

---

## 七、下一步：阶段 C（设备控制链路）

| 序号 | 任务 | 优先级 |
|------|------|--------|
| C1 | ESP32 ↔ 51 MCU UART 协议（`0xAA\|CMD\|LEN\|DATA\|XOR`） | P0 |
| C2 | 51 MCU 继电器控制（台灯 + 扩展） | P0 |
| C3 | 433MHz RF 编码学习 + 发射（替换当前脉冲方案） | P1 |
| C4 | PWM 灯光调光 | P2 |
| C5 | 场景模式完善（含 51 MCU 联动） | P2 |

## 八、后续：Edge Impulse 模型训练

用户需在 [edgeimpulse.com](https://edgeimpulse.com) 完成：

1. **语音 KWS 模型**：录制"小猫咪"唤醒词 + 15 条中文指令各 50-100 条 → 训练 → 导出 Arduino 库
2. **人体检测 FOMO 模型**：拍照采集有人/无人场景 → 标注 → 训练 → 导出 Arduino 库
3. 在 `voice_driver.h` 和 `vision_driver.h` 中取消 `#define HAS_EI_*` 注释即可激活

---

## 九、编译依赖

| 库 | 来源 |
|----|------|
| Adafruit GFX Library | Arduino Library Manager |
| Adafruit ST7789 | Arduino Library Manager |
| JPEGDEC (Larry Bank) | Arduino Library Manager |
| ESP32 Arduino Core | Boards Manager（Seeed XIAO ESP32S3） |

**编译配置**: Partition Scheme → No OTA (Large APP) / PSRAM → OPI PSRAM
