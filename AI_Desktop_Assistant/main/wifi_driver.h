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

// 云桥接回调（Python 服务器 → ESP32）
typedef void (*CloudTextHandler)(const String& msg);
typedef void (*CloudBinaryHandler)(const uint8_t* data, size_t len);

// ── WiFi 连接 ──
bool wifi_init(const char* ssid, const char* password,
               const char* device_name = "ai-desktop-assistant");
bool wifi_is_connected();
String wifi_ip_string();
void wifi_print_status(Stream& out);

// ── LittleFS ──
bool wifi_littlefs_init();

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

// ── 云桥接 (ESP32 → Python 服务器 WebSocket 客户端) ──
bool cloud_bridge_init(const char* server_ip, uint16_t port = 8765);
bool cloud_bridge_connected();
void cloud_bridge_loop();
bool cloud_bridge_send_text(const String& msg);
bool cloud_bridge_send_binary(const uint8_t* data, size_t len);
void cloud_bridge_on_text(CloudTextHandler handler);
void cloud_bridge_on_binary(CloudBinaryHandler handler);

#endif
