# 阶段 B 调试 — 改动总结

> 日期：2026-06-23  
> 问题：烧录后屏幕卡在 "Booting..."，系统不响应

---

## 一、问题定位过程

| 步骤 | 排查方向 | 结果 |
|------|---------|------|
| 1 | 怀疑摄像头初始化卡死 | 串口输出 `Camera: OK`，排除 |
| 2 | 怀疑 WiFi 上电导致掉电复位 | 串口输出 `[WiFi] Connecting...` 无复位，排除 |
| 3 | 换 WiFi 后仍显示 Booting | 串口输出 `WiFi: OK`，系统正常但屏幕不更新 |
| 4 | **发现 GPIO9 引脚冲突** | **根因确认** ✅ |

---

## 二、根因：GPIO9 引脚冲突

LCD 和 UART 共用 **GPIO 9**：

```
LCD MOSI (SPI数据)  → GPIO 9
UART TX (给51单片机)  → GPIO 9   ← 冲突！
```

`setup()` 执行顺序：

```
1. screen_init()  → SPI 占用 GPIO9 → screen_show_boot() 显示 "Booting..."  ✅
2. uart_init()    → Serial1 抢占 GPIO9 → SPI 通信失效                       ❌
3. 之后所有屏幕操作（WiFi Ready、表情显示）全部无效，屏幕永远停在 "Booting..."
```

---

## 三、代码改动

### `main.ino`

```diff
- // 5b. UART → 51 MCU
- uart_init();
+ // 5b. UART → 51 MCU (阶段C再启用，当前 GPIO9 与 LCD MOSI 冲突)
+ // uart_init();
```

> 51 单片机 UART 通信属于**阶段 C** 内容，当前暂未接入硬件，注释掉不影响功能。

### WiFi 凭据

```cpp
// 原（校园网，需网页认证，不可用）
const char* WIFI_SSID     = "WHU-STU-7.4G";
const char* WIFI_PASSWORD = "2842234004";

// 改（手机 2.4GHz 热点，已通过测试）
const char* WIFI_SSID     = "OnePlus Ace 2 Pro";
const char* WIFI_PASSWORD = "chengkangyu144";
```

---

## 四、新增文件

| 文件 | 说明 |
|------|------|
| `LittleFS上传指南.md` | ESP32 LittleFS 插件安装与数据上传教程（.vsix 方式） |

---

## 五、阶段 C 待解决

接入 51 MCU 时需要解决 GPIO9 冲突，可选方案：

| 方案 | 说明 |
|------|------|
| A | UART TX 改用其他空闲 GPIO（如 GPIO4） |
| B | LCD 改用纯软件 SPI，释放 GPIO9 |
| C | 使用 ESP32 的硬串口 Serial1 但重映射到其他引脚 |

---

## 六、当前状态

| 组件 | 状态 |
|------|------|
| 摄像头 | ✅ 正常 |
| 屏幕 / 表情 | ✅ 正常（7 表情全部显示） |
| 麦克风 | ✅ 正常 |
| 扬声器 | ✅ 正常 |
| RF 发射 | ✅ 已初始化 |
| WiFi + WebServer | ✅ 连接成功 |
| 运动检测（帧差法） | ✅ 正常 |
| 语音识别 | ⏳ 占位，等待 Edge Impulse 模型 |
| 51 MCU UART | ⏳ 阶段 C |
