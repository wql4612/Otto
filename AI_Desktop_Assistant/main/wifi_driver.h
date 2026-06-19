#ifndef WIFI_DRIVER_H
#define WIFI_DRIVER_H

#include <Arduino.h>

typedef bool (*WifiCommandHandler)(const String& cmd, String& message);
typedef String (*WifiStatusProvider)();
typedef void (*WifiImageHandler)(const uint8_t* data, size_t len, size_t index, size_t total);

bool wifi_init(const char* ssid, const char* password, const char* device_name = "ai-desktop-assistant");
bool wifi_is_connected();
String wifi_ip_string();
void wifi_print_status(Stream& out);
void wifi_set_command_handler(WifiCommandHandler handler);
void wifi_set_status_provider(WifiStatusProvider provider);
void wifi_set_image_handler(WifiImageHandler handler);

String json_escape(const String& input);

#endif
