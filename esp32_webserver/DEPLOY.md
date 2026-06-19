# ESP32 Web 控制面板 · 部署指南

## 文件清单

```
control_panel.html          → 上传到 LittleFS 根目录 (/control_panel.html)
lcd_faces/*.jpg             → 上传到 LittleFS (/faces/face_*.jpg)
esp32_webserver/esp32_webserver.h  → 放入 Arduino sketch 目录
```

## 步骤1: Arduino IDE 配置

### 1.1 安装库
在 Arduino IDE → 工具 → 管理库:
- `JPEGDEC` (by Larry Bank)
- `Adafruit GFX Library`
- `Adafruit ST7789`
- `TFT_eSPI` (by Bodmer)

### 1.2 选择分区表
`工具 → Partition Scheme → No OTA (Large APP)` (约 3MB APP, 1.5MB SPIFFS)

### 1.3 开启 PSRAM
`工具 → PSRAM → "OPI PSRAM"`

## 步骤2: 上传 LittleFS 文件

### 方法A: Arduino IDE 插件 (推荐)
1. 安装 `ESP32 LittleFS Upload` 插件
   - Arduino IDE → 文件 → 首选项 → 附加开发板管理器网址
   - 下载: https://github.com/lorol/arduino-esp32fs-plugin
2. 在 sketch 目录下创建 `data/` 文件夹
3. 将 `control_panel.html` 放入 `data/`
4. 创建 `data/faces/` 文件夹，放入所有 `face_*.jpg`
5. 工具 → `ESP32 LittleFS Upload` → 等待上传完成

### 方法B: 命令行 (esptool)
```bash
# 1. 创建 LittleFS 镜像
mklittlefs -c data/ -s 1503232 littlefs.bin
# 2. 烧录
esptool.py --port COM3 write_flash 0x290000 littlefs.bin
```

## 步骤3: 集成到主固件

```cpp
// integrated.ino 主文件中加入:

#include "esp32_webserver.h"
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

void setup() {
    Serial.begin(115200);

    // 1. 初始化 LittleFS
    littlefs_init();

    // 2. 初始化 TFT
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    // 3. 连接 WiFi
    wifi_connect("YOUR_SSID", "YOUR_PASSWORD");

    // 4. 启动 WebServer
    webserver_setup();

    // 5. 显示开机表情
    face_display(FACE_IDLE, tft);

    Serial.println("[Setup] Done!");
}

void loop() {
    g_server.handleClient();
    // ... 其他逻辑 (人体检测/语音识别/舵机等)
}
```

## 步骤4: 验证

1. ESP32 启动后，串口监视器会打印 IP 地址
2. 浏览器打开 `http://<ESP32_IP>`
3. 应看到控制面板页面
4. 摄像头应显示实时画面
5. 点击按钮 → 串口打印命令日志

## 故障排除

| 问题 | 检查 |
|------|------|
| 页面404 | LittleFS 上传是否成功? 串口查看 LittleFS 容量 |
| 摄像头无画面 | PSRAM 是否开启? 摄像头引脚是否对? |
| WebSocket 断连 | ESP32 和电脑在同一 WiFi? IP 地址对? |
| 表情不显示 | JPEG 文件是否在 /faces/ 下? JPEGDEC 库已安装? |
| 编译太大 | 关闭调试输出、减少 WiFi 缓冲区、用 O2 优化 |
