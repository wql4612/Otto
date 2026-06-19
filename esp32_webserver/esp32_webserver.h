// ============================================================
// ESP32 WebServer + WebSocket 服务端模块（已废弃 / DEPRECATED）
//
// 此模块的功能已整合到 AI_Desktop_Assistant/main/wifi_driver.cpp。
// 新代码请使用 wifi_driver 模块的 API:
//   wifi_init() / wifi_handle_client() / ws_broadcast() 等。
//
// 本文件保留作为参考实现。
// 负责人: 组员A+C 集成（原）
// 依赖: WiFi.h, WebServer.h, LittleFS.h, esp_camera.h
// ============================================================

#ifndef AICAT_WEBSERVER_H
#define AICAT_WEBSERVER_H

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_camera.h>
#include <JPEGDEC.h>

// ────────────── 配置 ──────────────
#define WS_PORT          80
#define WEBSOCKET_PATH   "/ws"
#define STREAM_PATH      "/stream"
#define MAX_WS_CLIENTS   4

// ────────────── 表情定义 ──────────────
enum FaceIndex : uint8_t {
    FACE_HAPPY      = 0,
    FACE_IDLE       = 1,
    FACE_LISTENING  = 2,
    FACE_SURPRISED  = 3,
    FACE_SLEEP      = 4,
    FACE_CONFUSED   = 5,
    FACE_CUTE       = 6,
    FACE_COUNT      = 7
};

static const char* FACE_FILES[FACE_COUNT] PROGMEM = {
    "/faces/face_happy.jpg",
    "/faces/face_idle.jpg",
    "/faces/face_listening.jpg",
    "/faces/face_surprised.jpg",
    "/faces/face_sleep.jpg",
    "/faces/face_confused.jpg",
    "/faces/face_cute.jpg",
};

// ────────────── 全局状态 ──────────────
struct SystemState {
    bool person_present = false;
    bool light_on       = false;
    bool fan_on         = false;
    bool speaker_on     = false;
    uint8_t current_face = FACE_IDLE;
    unsigned long last_person_change = 0;
};
static SystemState g_state;

// ────────────── WebServer & WebSocket ──────────────
static WebServer g_server(WS_PORT);
static WiFiClient g_ws_clients[MAX_WS_CLIENTS];
static uint8_t g_ws_count = 0;
static JPEGDEC g_jpeg;
static uint16_t* g_fb = nullptr;  // 全帧缓冲 (128×160)
static const int FB_W = 240, FB_H = 280;

// ────────────── 前向声明 ──────────────
void ws_broadcast(const String& msg);
void ws_handle_message(const String& msg);
void log_event(const char* category, const char* message);

// ────────────── WiFi 连接 ──────────────
void wifi_connect(const char* ssid, const char* pass) {
    WiFi.begin(ssid, pass);
    Serial.printf("[WiFi] Connecting to %s", ssid);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 30) {
        delay(500); Serial.print("."); retry++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WiFi] Failed to connect, will retry in loop");
    }
}

// ────────────── LittleFS 初始化 ──────────────
bool littlefs_init() {
    if (!LittleFS.begin(true)) {
        Serial.println("[LittleFS] Mount failed!");
        return false;
    }
    Serial.printf("[LittleFS] Total: %d KB, Used: %d KB\n",
        LittleFS.totalBytes() / 1024, LittleFS.usedBytes() / 1024);
    return true;
}

// ────────────── JPEG 解码回调 ──────────────
static int _jpeg_draw_cb(JPEGDRAW* pDraw) {
    // 逐行写入帧缓冲
    uint16_t* src = pDraw->pPixels;
    for (int y = 0; y < pDraw->iHeight; y++) {
        int destY = pDraw->y + y;
        if (destY >= 0 && destY < FB_H) {
            memcpy(&g_fb[destY * FB_W + pDraw->x],
                   &src[y * pDraw->iWidth],
                   pDraw->iWidth * sizeof(uint16_t));
        }
    }
    return 1;
}

// ────────────── 表情显示 ──────────────
bool face_display(FaceIndex face, TFT_eSPI& tft) {
    if (face >= FACE_COUNT || !g_fb) return false;

    const char* path = FACE_FILES[face];
    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("[Face] File not found: %s\n", path);
        return false;
    }

    size_t fsize = f.size();
    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { f.close(); return false; }

    f.read(buf, fsize);
    f.close();

    // 清帧缓冲为黑色
    memset(g_fb, 0, FB_W * FB_H * sizeof(uint16_t));

    if (g_jpeg.openRAM(buf, fsize, _jpeg_draw_cb)) {
        g_jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        g_jpeg.decode(0, 0, 0);
        g_jpeg.close();
    }

    free(buf);

    // 输出到 TFT
    tft.drawRGBBitmap(0, 0, g_fb, FB_W, FB_H);
    g_state.current_face = face;
    return true;
}

// ────────────── MJPEG 相机流 ──────────────
void handle_stream() {
    WiFiClient client = g_server.client();
    if (!client.connected()) return;

    String boundary = "FRAME_BOUNDARY";
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: multipart/x-mixed-replace; boundary=" + boundary);
    client.println("Access-Control-Allow-Origin: *");
    client.println();

    while (client.connected()) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { delay(10); continue; }

        client.printf("--%s\r\n", boundary.c_str());
        client.printf("Content-Type: image/jpeg\r\n");
        client.printf("Content-Length: %u\r\n\r\n", fb->len);
        client.write(fb->buf, fb->len);
        client.print("\r\n");

        esp_camera_fb_return(fb);
        delay(1);  // ~30 FPS max
    }
}

// ────────────── WebSocket 广播 ──────────────
void ws_broadcast(const String& msg) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (g_ws_clients[i] && g_ws_clients[i].connected()) {
            // WebSocket frame: 0x81 = text, no mask from server
            uint8_t header[2];
            size_t len = msg.length();
            header[0] = 0x81;
            if (len <= 125) {
                header[1] = len;
                g_ws_clients[i].write(header, 2);
            } else if (len <= 65535) {
                header[1] = 126;
                g_ws_clients[i].write(header, 2);
                uint8_t ext[2] = {(uint8_t)(len >> 8), (uint8_t)(len & 0xFF)};
                g_ws_clients[i].write(ext, 2);
            }
            g_ws_clients[i].print(msg);
        }
    }
}

// ────────────── WebSocket 消息处理 ──────────────
void ws_handle_message(const String& msg) {
    // 简单 JSON 解析 (避免引入 ArduinoJson 依赖)
    // 支持格式: {"type":"cmd","device":"light","action":"on"}
    //           {"type":"scene","name":"home"}

    if (msg.indexOf("\"type\":\"cmd\"") >= 0) {
        if (msg.indexOf("\"device\":\"light\"") >= 0) {
            bool on = msg.indexOf("\"action\":\"on\"") >= 0;
            g_state.light_on = on;
            // UART 发送给 51 单片机
            uint8_t data[] = {1, (uint8_t)on};
            // uart_send_cmd(0x01, data, 2);  // 需外部实现
            Serial.printf("[Cmd] Light %s\n", on ? "ON" : "OFF");
        }
        if (msg.indexOf("\"device\":\"fan\"") >= 0) {
            bool on = msg.indexOf("\"action\":\"on\"") >= 0;
            g_state.fan_on = on;
            Serial.printf("[Cmd] Fan %s\n", on ? "ON" : "OFF");
        }
        if (msg.indexOf("\"device\":\"speaker\"") >= 0) {
            bool on = msg.indexOf("\"action\":\"on\"") >= 0;
            g_state.speaker_on = on;
            Serial.printf("[Cmd] Speaker %s\n", on ? "ON" : "OFF");
        }
    }

    if (msg.indexOf("\"type\":\"scene\"") >= 0) {
        if (msg.indexOf("\"name\":\"home\"") >= 0) {
            // 回家模式: 开灯 + LCD开心
            g_state.light_on = true;
            // face_display(FACE_HAPPY, tft);
            log_event("scene", "执行回家模式: 开灯+欢迎");
        }
        if (msg.indexOf("\"name\":\"away\"") >= 0) {
            // 离开模式: 全关 + LCD睡觉
            g_state.light_on = false;
            g_state.fan_on = false;
            g_state.speaker_on = false;
            // face_display(FACE_SLEEP, tft);
            log_event("scene", "执行离开模式: 全部关闭");
        }
    }

    // 广播状态更新
    char status[256];
    snprintf(status, sizeof(status),
        "{\"type\":\"status\",\"person\":%s,\"light\":%s,\"fan\":%s,\"speaker\":%s}",
        g_state.person_present ? "true" : "false",
        g_state.light_on ? "true" : "false",
        g_state.fan_on ? "true" : "false",
        g_state.speaker_on ? "true" : "false");
    ws_broadcast(status);
}

// ────────────── 事件日志 ──────────────
void log_event(const char* category, const char* message) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"log\",\"category\":\"%s\",\"message\":\"%s\"}", category, message);
    ws_broadcast(buf);
    Serial.printf("[%s] %s\n", category, message);
}

// ────────────── WebSocket 轮询 (在主 loop 中调用) ──────────────
void ws_loop() {
    // 检查新客户端连接
    WiFiClient newClient = g_server.client();
    // WebSocket 握手在 handle_client 中处理
    g_server.handleClient();
}

// ────────────── 状态广播 (定期调用) ──────────────
void ws_broadcast_status() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"status\",\"person\":%s,\"light\":%s,\"fan\":%s,\"speaker\":%s}",
        g_state.person_present ? "true" : "false",
        g_state.light_on ? "true" : "false",
        g_state.fan_on ? "true" : "false",
        g_state.speaker_on ? "true" : "false");
    ws_broadcast(buf);
}

// ────────────── 服务静态文件 ──────────────
void handle_static_file(const char* path, const char* contentType) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        g_server.send(404, "text/plain", "File not found");
        return;
    }
    g_server.streamFile(f, contentType);
    f.close();
}

// ────────────── 注册路由 ──────────────
void webserver_setup() {
    // 帧缓冲分配
    if (!g_fb) {
        g_fb = (uint16_t*)malloc(FB_W * FB_H * sizeof(uint16_t));
        memset(g_fb, 0, FB_W * FB_H * sizeof(uint16_t));
    }

    // 首页 → 控制面板
    g_server.on("/", HTTP_GET, []() {
        handle_static_file("/control_panel.html", "text/html; charset=utf-8");
    });

    // 摄像头流
    g_server.on(STREAM_PATH, HTTP_GET, handle_stream);

    // 摄像头开关 (JSON API)
    g_server.on("/api/camera", HTTP_POST, []() {
        g_server.send(200, "application/json", "{\"ok\":true}");
    });

    // LittleFS 静态文件通配 (CSS/JS/fonts 等, 一般不需要)
    g_server.onNotFound([]() {
        String path = g_server.uri();
        if (path == "/ws") return;  // WebSocket 不走 HTTP
        if (path.startsWith("/faces/")) {
            handle_static_file(path.c_str(), "image/jpeg");
        } else {
            // 回退到控制面板 (SPA)
            handle_static_file("/control_panel.html", "text/html; charset=utf-8");
        }
    });

    g_server.begin();
    Serial.printf("[Server] Started on port %d\n", WS_PORT);
    Serial.printf("[Server] Open http://%s in browser\n", WiFi.localIP().toString().c_str());
}

#endif // AICAT_WEBSERVER_H
