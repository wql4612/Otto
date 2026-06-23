# Otto Desktop Debug Panel

这是给电脑端使用的调试面板，配合 ESP32 的轻量 HTTP API 工作。

## 使用方式

1. 先让 ESP32 连上局域网。
2. 进入本目录。
3. 在电脑上启动一个静态文件服务器。

```bash
cd desktop_debug_panel
python3 -m http.server 8080
```

4. 浏览器打开：

```text
http://127.0.0.1:8080
```

5. 在页面顶部填写 ESP32 地址，例如：

```text
http://192.168.1.123
```

也可以直接通过 URL 参数传入：

```text
http://127.0.0.1:8080/?esp=http://192.168.1.123
```

## 当前架构

- 电脑端托管 `index.html`
- ESP32 只提供轻量 API
- 页面通过轮询 `/api/status` 获取状态
- 页面通过 `POST /api/action?cmd=...` 发送测试命令

## 固件侧说明

当前固件默认启用的是混合方案中的“API-only”模式：

- `GET /api/status`
- `POST /api/action`
- `POST /api/upload`

默认不再托管完整调试页，也不依赖 WebSocket。

如果后面需要恢复板端托管页面，可在 `main/main.ino` 中调整：

```cpp
wifi_set_ui_config({ true, true });
```

当前默认值是：

```cpp
wifi_set_ui_config({ false, false });
```
