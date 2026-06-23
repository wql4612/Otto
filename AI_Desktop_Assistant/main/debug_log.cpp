#include "debug_log.h"

#include "screen_driver.h"
#include "wifi_driver.h"

#include <stdarg.h>

namespace {

String g_debug_log = "Booting...";
bool g_web_ready = false;
constexpr size_t MAX_DEBUG_LOG_LEN = 1600;

void trim_log_if_needed() {
    if (g_debug_log.length() <= MAX_DEBUG_LOG_LEN) return;

    size_t overflow = g_debug_log.length() - MAX_DEBUG_LOG_LEN;
    int cut = g_debug_log.indexOf('\n', overflow);
    if (cut < 0) {
        g_debug_log.remove(0, overflow);
    } else {
        g_debug_log.remove(0, cut + 1);
    }
}

String nth_line_from_end(int index_from_end) {
    if (g_debug_log.isEmpty()) return String();

    int end = g_debug_log.length();
    for (int i = 0; i < index_from_end; ++i) {
        end = g_debug_log.lastIndexOf('\n', end - 1);
        if (end < 0) return String();
    }

    int start = g_debug_log.lastIndexOf('\n', end - 1);
    if (start < 0) start = 0;
    else start += 1;

    return g_debug_log.substring(start, end);
}

void broadcast_log(const String& text, const char* category) {
    if (!g_web_ready) return;

    String json = "{\"type\":\"log\",\"category\":\"";
    json += category ? category : "system";
    json += "\",\"message\":\"";
    json += json_escape(text);
    json += "\"}";
    ws_broadcast(json);
}

}  // namespace

void debug_log_begin(const String& initial) {
    g_debug_log = initial;
    trim_log_if_needed();
}

void debug_log_append(const String& text, const char* category) {
    if (text.isEmpty()) return;

    if (!g_debug_log.isEmpty()) g_debug_log += "\n";
    g_debug_log += text;
    trim_log_if_needed();

    broadcast_log(text, category);
}

void debug_log_printf(const char* category, const char* fmt, ...) {
    if (!fmt) return;

    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    debug_log_append(String(buf), category);
}

void debug_log_set_web_ready(bool ready) {
    g_web_ready = ready;
}

void debug_log_render_boot(const char* title) {
    if (!screen_is_ready()) return;

    String latest = nth_line_from_end(0);
    String previous = nth_line_from_end(1);

    if (previous.isEmpty()) {
        screen_show_message(title, latest.c_str(), nullptr);
    } else {
        screen_show_message(title, previous.c_str(), latest.c_str());
    }
}

const String& debug_log_text() {
    return g_debug_log;
}
