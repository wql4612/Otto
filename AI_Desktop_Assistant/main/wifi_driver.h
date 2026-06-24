#ifndef WIFI_DRIVER_H
#define WIFI_DRIVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

// ── 回调类型 ──
typedef bool (*WifiCommandHandler)(const String& cmd, String& message);
typedef String (*WifiStatusProvider)();
typedef void (*WifiImageHandler)(const uint8_t* data, size_t len, size_t index, size_t total);
typedef void (*WifiWSMessageHandler)(const String& msg);

struct WifiUiConfig {
    bool serve_embedded_ui;
    bool serve_static_files;
};

// ── WiFi 连接 ──
bool wifi_init(const char* ssid, const char* password,
               const char* device_name = "ai-desktop-assistant");
bool wifi_is_connected();
String wifi_ip_string();
void wifi_print_status(Stream& out);
String wifi_last_ws_remote_ip();

// ── LittleFS ──
bool wifi_littlefs_init();

// ── UI 托管模式 ──
void wifi_set_ui_config(const WifiUiConfig& config);

// ── 回调设置 ──
void wifi_set_command_handler(WifiCommandHandler handler);
void wifi_set_status_provider(WifiStatusProvider provider);
void wifi_set_image_handler(WifiImageHandler handler);
void wifi_set_ws_message_handler(WifiWSMessageHandler handler);

// ── WebSocket 广播 ──
void ws_broadcast(const String& msg);
void ws_broadcast_status_json(const String& json);

// ── 主循环调用 ──
void wifi_handle_client();

// ── 工具 ──
String json_escape(const String& input);

#endif
