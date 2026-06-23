#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <Arduino.h>

void debug_log_begin(const String& initial = "Booting...");
void debug_log_append(const String& text, const char* category = "system");
void debug_log_printf(const char* category, const char* fmt, ...);
void debug_log_set_web_ready(bool ready);
void debug_log_render_boot(const char* title = "System Boot");
const String& debug_log_text();

#endif
