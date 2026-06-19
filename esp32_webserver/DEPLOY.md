# ESP32 Web 控制面板 · 部署指南 v2

> **架构更新 (2026-06-19)**: WebServer 功能已整合到 `wifi_driver.cpp`。
> `esp32_webserver.h` 保留作为参考，新代码请使用 `wifi_driver` 模块。

## 文件清单

```
AI_Desktop_Assistant/main/
├── main.ino                  ← 主固件
├── data/                     ← LittleFS 上传目录
│   ├── control_panel.html    ← 控制面板
│   └── faces/                ← 表情图片 (7张 JPG)
│       ├── face_happy.jpg
│       ├── face_idle.jpg
│       ├── face_listening.jpg
│       ├── face_surprised.jpg
│       ├── face_sleep.jpg
│       ├── face_confused.jpg
│       └── face_cute.jpg
├── wifi_driver.cpp/h         ← WiFi + WebServer + WebSocket
├── screen_driver.cpp/h       ← TFT 显示 + 表情
├── camera_driver.cpp/h       ← OV2640 摄像头
├── mic_driver.cpp/h          ← PDM 麦克风
├── speaker_driver.cpp/h      ← I2S 音频
├── servo_driver.cpp/h        ← SG90 + MG996R 舵机
├── rf_driver.cpp/h           ← 433MHz 发射
├── sd_driver.cpp/h           ← SD 卡
└── player_driver.cpp/h       ← 音频播放
```

## 步骤1: Arduino IDE 配置

### 1.1 安装库
Arduino IDE → 工具 → 管理库:
- `JPEGDEC` (by Larry Bank)
- `Adafruit GFX Library`
- `Adafruit ST7789`

### 1.2 选择分区表
`工具 → Partition Scheme → No OTA (Large APP)` (约 3MB APP, 1.5MB SPIFFS)

或者使用自定义分区表，确保 LittleFS 至少 1.5MB。

### 1.3 开启 PSRAM
`工具 → PSRAM → "OPI PSRAM"`

## 步骤2: 上传 LittleFS 文件

### 方法A: Arduino IDE 插件 (推荐)
1. 安装 `ESP32 LittleFS Upload` 插件
2. `data/` 目录已准备好（在 sketch 目录下）
3. 工具 → `ESP32 LittleFS Upload` → 等待完成

### 方法B: 命令行
```bash
mklittlefs -c data/ -s 1503232 littlefs.bin
esptool.py --port COM3 write_flash 0x290000 littlefs.bin
```

## 步骤3: 编译上传

1. 打开 `AI_Desktop_Assistant/main/main.ino`
2. 修改 `WIFI_SSID` 和 `WIFI_PASSWORD` 为实际值
3. 选择开发板: `Seeed XIAO ESP32S3 Sense`
4. 编译并上传

## 步骤4: 验证

1. 串口监视器 (115200 baud) 查看启动日志
2. 浏览器打开 `http://<ESP32_IP>`
3. 应看到控制面板页面
4. 摄像头应显示实时 MJPEG 流
5. 点击按钮 → 串口打印命令日志
6. 表情切换: 访问 `/api/action?cmd=face_happy` 等

## API 端点

| 路径 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 控制面板 (LittleFS) 或内嵌页面 |
| `/stream` | GET | MJPEG 摄像头流 |
| `/ws` | GET | WebSocket 升级 |
| `/api/status` | GET | JSON 状态查询 |
| `/api/action?cmd=xxx` | POST | 调试命令 |
| `/api/upload` | POST | RGB565 图片上传到屏幕 |
| `/faces/*` | GET | 表情 JPG 文件 |

## 调试命令

| 命令 | 说明 |
|------|------|
| `ping` | 连通测试 |
| `wifi_status` | WiFi 状态 |
| `capture` | 拍一张照片 |
| `mic_diag` | 麦克风诊断 |
| `screen_demo` | 屏幕测试图案 |
| `servo180_0/90/180` | SG90 舵机角度 |
| `servo360_fwd/rev/stop` | MG996R 舵机 |
| `rf_on/off` | RF 发射测试 |
| `face_happy/idle/sleep/listening/surprised/cute` | 表情切换 |
| `mic_record` | 录 1 秒音频 |
| `loopback` | 录放直通测试 |
| `tone_16k/tone_44k` | 提示音测试 |

## 故障排除

| 问题 | 检查 |
|------|------|
| 页面404 | LittleFS 上传成功？串口查看容量 |
| 摄像头无画面 | PSRAM 开启？摄像头初始化日志 |
| WebSocket 断连 | 同 WiFi？IP 正确？ |
| 表情不显示 | JPEG 在 /faces/？JPEGDEC 已安装？ |
| 编译太大 | 关闭调试输出，用 O2 优化 |
| WiFi 连不上 | SSID/密码正确？信号强度？ |
