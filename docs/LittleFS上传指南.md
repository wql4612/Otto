# ESP32 LittleFS 文件上传指南

> 适用：Seeed XIAO ESP32S3 Sense  
> 目的：将表情 JPG 和控制面板 HTML 上传到 ESP32 内部 Flash 的 LittleFS 分区

---

## 一、确认文件已就位

你的 `data/` 目录已经准备好了，位于：

```
AI_Desktop_Assistant/main/data/
├── control_panel.html          ← Web 控制面板
└── faces/
    ├── face_happy.jpg
    ├── face_idle.jpg
    ├── face_listening.jpg
    ├── face_surprised.jpg
    ├── face_sleep.jpg
    ├── face_confused.jpg
    └── face_cute.jpg
```

> ⚠️ **关键**：`data/` 文件夹必须和 `main.ino` 在**同一个目录**下！你的项目结构是正确的，不需要移动。

---

## 二、安装 LittleFS 上传插件

1. 从以下地址下载 `.vsix` 文件：
   - https://github.com/earlephilhower/arduino-littlefs-upload/releases
   - 找到最新版本，下载 `.vsix` 文件（如 `littlefs-upload-1.0.0.vsix`）

2. 将 `.vsix` 文件放入插件目录：
   ```
   C:\Users\你的用户名\.arduinoIDE\plugins\
   ```
   > ⚠️ 如果没有 `plugins` 文件夹，需要**手动新建**，名字必须是 `plugins`

3. **完全关闭** Arduino IDE，再重新打开

4. 按 `Ctrl+Shift+P`，输入 `upload`，出现 **"Upload LittleFS to …"** 即安装成功

---

## 三、配置开发板分区方案

上传前需要选择包含 LittleFS 的分区表：

1. Arduino IDE → **工具 (Tools)**
2. 确认以下设置：

| 选项 | 值 |
|------|-----|
| 开发板 (Board) | **XIAO_ESP32S3** |
| PSRAM | **OPI PSRAM** |
| Flash Size | **8MB** |
| Partition Scheme | **Default 4MB with spiffs** 或 **8M with spiffs** |

> ⚠️ 注意：虽然名称为 "spiffs"，但实际 LittleFS 也兼容此分区。如果有 "LittleFS" 选项则优先选。

---

## 四、上传步骤

### 第一步：先烧录固件

像平常一样，点击 **→ (Upload)** 先烧录 `main.ino`。

### 第二步：上传 LittleFS 数据

1. 确保 ESP32 已通过 USB 连接
2. 按 `Ctrl+Shift+P`，输入 `upload`，选择 **"Upload LittleFS to XIAO_ESP32S3"**（或对应的开发板名）
3. 回车执行

你会看到类似输出：

```
[LittleFS] data   : .../AI_Desktop_Assistant/main/data
[LittleFS] size   : 1472 KB
[LittleFS] page   : 256 B
[LittleFS] block  : 4096 B
/tmp/esp32-littlefs-xxxxx.bin
---------------------------------------------------
LittleFS:    368 KB used
LittleFS:   1088 KB free
Uploading stub...
Running stub...
Stub running...
```

4. 出现 **"Done"** 或 **"Upload complete"** 表示成功

---

## 五、验证

上传完成后，重新上电（拔插 USB），看串口监视器（115200 波特率）：

```
[LittleFS] Total: 1472 KB, Used: 368 KB
```

如果看到这个，说明 LittleFS 挂载成功且文件已上传。

WiFi 连接成功后，屏幕应该会显示 **idle 表情**（而不是booting……）。

---

## 六、常见问题

| 问题 | 解决方法 |
|------|---------|
| `Ctrl+Shift+P` 搜不到 "Upload LittleFS" | 检查 `.vsix` 是否放对目录，重启 IDE |
| 上传报错 "SPIFFS not supported" | 分区方案选错了，换成 **Default 4MB with spiffs** |
| 上传后串口显示 `[Face] File not found` | 文件没正确上传，重新上传一次 |
| `[LittleFS] Mount failed!` | 先烧录固件，再上传 LittleFS；检查 Flash Size 是否为 8MB |
| 上传卡住不动 | 按住 XIAO 的 BOOT 键 + 按一下 RST，松开 RST 再松开 BOOT，进入下载模式后重试 |

---

## 七、后续：WiFi 连接

上传 LittleFS 后，修改 `main.ino` 中的 WiFi 配置：

```cpp
const char* WIFI_SSID     = "你的WiFi名";    // 必须是 2.4GHz！
const char* WIFI_PASSWORD = "你的WiFi密码";
```

推荐先用**手机开 2.4GHz 热点**验证，成功后屏幕会显示 IP 地址。
