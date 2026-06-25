/**
 * AI Desktop Assistant — 主固件 v3
 * 集成：麦克风 / 摄像头 / 扬声器 / 屏幕 / WiFi / 舵机 / RF
 *       + 帧差法运动检测 / 语音识别占位 / 指令映射
 *
 * 引脚配置 (Seeed XIAO ESP32S3 Sense):
 *   LCD:      SCLK=7  MOSI=9  DC=3  CS=2
 *   I2S 功放: BCLK=6  LRCLK=5  DOUT=1
 *   PDM 麦克: CLK=42  DIN=41 (板载)
 *   头部俯仰舵机(A): GPIO4    头部左右舵机(B): GPIO8
 *   433M RF:  GPIO43 (当前停用，释放资源)
 *   UART→51:  TX=9  RX=8 (当前未启用)
 *   Camera:   板载固定
 *
 * 阶段 B (当前):  帧差法 ✓  语音占位 ✓  指令映射 ✓
 * 阶段 C (待做):  51单片机 UART / 433MHz 编码学习 / 继电器控制
 */

#include "mic_driver.h"
#include "camera_driver.h"
#include "speaker_driver.h"
#include "player_driver.h"
#include "screen_driver.h"
#include "wifi_driver.h"
#include "servo_driver.h"
#include "rf_driver.h"
#include "vision_driver.h"
#include "voice_driver.h"
#include "ultrasonic_driver.h"
#include "command_map.h"
#include "uart_driver.h"
#include "debug_log.h"
#include "qwen_client.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <base64.h>
#include <esp_system.h>
#include <esp_sleep.h>

// ═══════════════════════════════════════
// 配置
// ═══════════════════════════════════════
#define SAMPLE_RATE   16000
#define SAMPLE_BITS   16
#define VOICE_TEST_MS 2000
#define VOICE_COMMAND_MS 5000
#define WAKE_LISTENER_AUTOSTART_MS 6000
#define DEEP_SLEEP_WAKE_MINUTES 30

constexpr uint16_t HOST_SERVER_PORT = 9000;
const char* HOST_SERVER_PATH_VOICE_COMMAND = "/api/voice/command";

const char* WIFI_SSID     = "WHU-STU-7.4G";
const char* WIFI_PASSWORD = "2842234004";

// Qwen 视觉识别配置。填入后即可通过 Web 调试页触发识别。
const char* QWEN_IMAGE_API_URL = "https://ws-b5ji5wbyhspkl8kn.cn-beijing.maas.aliyuncs.com/compatible-mode/v1";
const char* QWEN_VOICE_API_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1";
const char* QWEN_API_KEY  = "sk-ws-H.RPYEMRH.N8Ic.MEUCIC5GfdDe5KTmP7LLhYvnEOsVUhfef4xNmS7oLDgekYIjAiEAhj-ndg7luLt6b1oAvjYmRa5UZskTYB3Fr0GdCg8ZgUQ";
const char* QWEN_IMAGE_MODEL = "qwen3.5-omni-plus";
const char* QWEN_IMAGE_PROMPT =
    "你是图像识别助手。请只返回一个 JSON 对象，不要返回 Markdown，不要返回额外解释。"
    " JSON 必须只包含两个字段：has_face(boolean) 和 face_count(number)。"
    " 如果画面里没有清晰可见的人脸，has_face 必须为 false，face_count 必须为 0。";
const char* QWEN_VOICE_MODEL = "qwen3-asr-flash";

constexpr bool ENABLE_SERVO_180 = true;
constexpr bool ENABLE_SERVO_HEAD_PAN = true;
constexpr bool ENABLE_RF = false;
constexpr bool ENABLE_SERVO_IDLE_ANIMATION = false;

constexpr int SERVO_180_PIN = 4;
constexpr int SERVO_HEAD_PAN_PIN = 8;
constexpr int RF_TX_PIN     = 43;
constexpr int ULTRASONIC_TRIG_PIN = 44;
constexpr int ULTRASONIC_ECHO_PIN = 43;
constexpr float ULTRASONIC_TRIGGER_CM = 50.0f;
constexpr unsigned long ULTRASONIC_POLL_MS = 1000;

const char* RADIO_ACK_LIGHT_ON  = "/radio/light_on.wav";
const char* RADIO_ACK_LIGHT_OFF = "/radio/light_off.wav";
const char* RADIO_ACK_WAKE      = "/radio/wake.wav";

#define IMAGE_W     184
#define IMAGE_H     184
#define IMAGE_BYTES (IMAGE_W * IMAGE_H * 2)

#define CMD_BUF_SIZE 64

// ═══════════════════════════════════════
// 全局对象
// ═══════════════════════════════════════
Servo180Driver servo_180(500, 2500, 60, 80);
Servo180Driver servo_head_pan(500, 2500, 75, 105);

static String g_last_result = "Idle";

enum BootModuleIndex : uint8_t {
    BOOT_SERVO180 = 0,
    BOOT_SERVO_HEAD_PAN,
    BOOT_SCREEN,
    BOOT_MIC,
    BOOT_SPEAKER,
    BOOT_RF,
    BOOT_AUDIO_FB,
    BOOT_CAMERA,
    BOOT_ULTRASONIC,
    BOOT_LITTLEFS,
    BOOT_WIFI,
    BOOT_MODULE_COUNT
};

static ScreenStatusItem g_boot_items[BOOT_MODULE_COUNT] = {
    { "Servo180", false, false },
    { "HeadPan", false, false },
    { "Screen", false, false },
    { "Mic", false, false },
    { "Speaker", false, false },
    { "RF", false, false },
    { "AudioFB", false, false },
    { "Camera", false, false },
    { "UltraSonic", false, false },
    { "LittleFS", false, false },
    { "WiFi", false, false },
};

// 系统状态（与前端 + command_map 同步）
SystemState g_sys;

FaceIndex g_current_face = FACE_IDLE;

// 缓冲区
static uint8_t* g_image_buf    = nullptr;
static uint8_t* g_loopback_buf = nullptr;
static size_t   g_loopback_len = 0;
static bool     g_voice_waiting_command = false;
static bool     g_voice_ack_pending = false;
static bool     g_host_waiting_action = false;
static bool     g_host_action_pending = false;
static unsigned long g_voice_ack_deadline_ms = 0;
static unsigned long g_host_wait_deadline_ms = 0;
static bool     g_wake_autostart_pending = true;
static unsigned long g_wake_autostart_at_ms = 0;
static uint32_t g_boot_id = 0;
static bool     g_presence_probe_busy = false;
static bool     g_presence_last_object_near = false;
static bool     g_presence_checked_for_current_object = false;
static unsigned long g_presence_last_check_ms = 0;
static bool     g_ultrasonic_last_ok = false;
static bool     g_sleep_pending = false;
static unsigned long g_sleep_at_ms = 0;
static String   g_host_pending_action_json;
static String   g_host_session_id;
static uint32_t g_host_session_seq = 0;
static String   g_host_server_ip;

enum RuntimeMode : uint8_t {
    MODE_STANDALONE = 0,
    MODE_HOST_ENHANCED = 1
};

static RuntimeMode g_runtime_mode = MODE_STANDALONE;
static bool        g_host_claimed = false;

// FreeRTOS
static QueueHandle_t g_cmd_queue = nullptr;
static TaskHandle_t  g_task_mic  = nullptr;
static TaskHandle_t  g_task_spk  = nullptr;

// Loopback 状态机
enum SysState { ST_IDLE, ST_LOOPBACK_REC, ST_LOOPBACK_PLAY };
static SysState g_state = ST_IDLE;

bool apply_qwen_voice_json(const String& json_text, String& answer_out);

// ═══════════════════════════════════════
// 工具函数
// ═══════════════════════════════════════
void set_last_result(const String& text) {
    g_last_result = text;
    debug_log_append(text, "system");
}

void play_tone(int freq, int duration_ms, int amplitude) {
    speaker_play_tone(freq, duration_ms, amplitude, SAMPLE_RATE);
}

void log_event(const char* category, const char* message) {
    debug_log_append(String(message), category);
}

void update_boot_status(BootModuleIndex index, bool ok, const char* log_ok, const char* log_fail, const char* ip_text = nullptr) {
    g_boot_items[index].known = true;
    g_boot_items[index].ok = ok;
    set_last_result(ok ? String(log_ok) : String(log_fail));
    if (screen_is_ready()) {
        screen_show_init_status("System Boot", g_boot_items, BOOT_MODULE_COUNT, ip_text);
    }
}

bool play_feedback_audio(const char* path, int fallback_freq_a, int fallback_freq_b = 0, float gain = 1.0f) {
    if (path && LittleFS.exists(path)) {
        debug_log_printf("system", "[Audio] Play feedback: %s", path);
        if (play_wav_littlefs_gain(path, gain, false)) return true;
        debug_log_printf("system", "[Audio] Playback failed, fallback tone: %s", path);
    } else if (path) {
        debug_log_printf("system", "[Audio] Feedback file missing: %s", path);
    }

    play_tone(fallback_freq_a, 90);
    if (fallback_freq_b > 0) {
        delay(40);
        play_tone(fallback_freq_b, 110);
    }
    return false;
}

bool is_voice_command_stage_active() {
    return g_voice_waiting_command || g_voice_ack_pending ||
           g_host_waiting_action || g_host_action_pending;
}

void enter_voice_listening_face() {
    screen_show_face_jpeg(FACE_FILES[FACE_LISTENING]);
    g_current_face = FACE_LISTENING;
}

void restore_face_after_voice_stage() {
    screen_show_face_jpeg(FACE_FILES[FACE_HAPPY]);
    g_current_face = FACE_HAPPY;
}

const char* runtime_mode_name() {
    return g_runtime_mode == MODE_HOST_ENHANCED ? "host_enhanced" : "standalone";
}

const char* voice_stage_name() {
    if (g_voice_ack_pending) return "ack_pending";
    if (g_host_waiting_action) return "host_wait_action";
    if (g_host_action_pending) return "host_action_pending";
    if (g_voice_waiting_command) return "standalone_command";
    if (voice_listener_running()) return "wake_listening";
    return "idle";
}

void clear_host_voice_state() {
    g_host_waiting_action = false;
    g_host_action_pending = false;
    g_host_wait_deadline_ms = 0;
    g_host_pending_action_json = "";
    g_host_session_id = "";
}

void set_runtime_mode(RuntimeMode mode, const char* reason = nullptr) {
    if (g_runtime_mode == mode) return;

    g_runtime_mode = mode;
    String message = String("[Host] Runtime mode -> ") + runtime_mode_name();
    if (reason && strlen(reason) > 0) {
        message += " (";
        message += reason;
        message += ")";
    }
    debug_log_append(message, "system");
    set_last_result(String("Mode: ") + runtime_mode_name());
}

String next_host_session_id() {
    char buf[32];
    ++g_host_session_seq;
    snprintf(buf, sizeof(buf), "wake-%lu-%lu",
             (unsigned long)(millis() / 1000UL),
             (unsigned long)g_host_session_seq);
    return String(buf);
}

void ws_send_host_message(const String& json) {
    if (!json.length()) return;
    ws_broadcast(json);
}

void notify_host_state() {
    String json = "{\"type\":\"state\",\"mode\":\"";
    json += runtime_mode_name();
    json += "\",\"voice_stage\":\"";
    json += voice_stage_name();
    json += "\",\"host_claimed\":";
    json += (g_host_claimed ? "true" : "false");
    json += ",\"wake_listener\":";
    json += (voice_listener_running() ? "true" : "false");
    json += ",\"person_present\":";
    json += (g_sys.person_present ? "true" : "false");
    json += ",\"light\":";
    json += (g_sys.light_on ? "true" : "false");
    if (g_host_session_id.length()) {
        json += ",\"session_id\":\"";
        json += json_escape(g_host_session_id);
        json += "\"";
    }
    json += "}";
    ws_send_host_message(json);
}

void notify_host_hello() {
    String json = "{\"type\":\"hello\",\"device_id\":\"otto-esp32s3\",";
    json += "\"fw\":\"v3\",\"mode\":\"";
    json += runtime_mode_name();
    json += "\",\"capabilities\":{";
    json += "\"local_wake\":true,";
    json += "\"pcm_uplink\":false,";
    json += "\"screen\":true,";
    json += "\"servo180\":";
    json += (ENABLE_SERVO_180 ? "true" : "false");
    json += ",\"speaker_local\":true,";
    json += "\"tts_stream_pull\":false";
    json += "}}";
    ws_send_host_message(json);
}

bool apply_action_json_with_feedback(const String& json_text, String& message_out) {
    const bool light_before = g_sys.light_on;
    String answer;
    bool ok = apply_qwen_voice_json(json_text, answer);

    if (g_sys.light_on != light_before) {
        if (g_sys.light_on) {
            log_event("command", "动作执行 · 开灯");
            play_feedback_audio(RADIO_ACK_LIGHT_ON, 880, 1180);
        } else {
            log_event("command", "动作执行 · 关灯");
            play_feedback_audio(RADIO_ACK_LIGHT_OFF, 620, 440);
        }
    }

    if (!ok) {
        message_out = answer.length() ? answer : String("Voice action apply failed");
        return false;
    }

    message_out = answer.length() ? answer : String("Voice action applied");
    return true;
}

void resume_wake_listener_after_stage() {
    g_voice_waiting_command = false;
    g_voice_ack_pending = false;
    clear_host_voice_state();
    voice_set_listener_enabled(true);
    if (!voice_listener_running() && !voice_start_wake_listener()) {
        debug_log_append("[Wake] Failed to restart listener", "system");
    } else if (voice_listener_running()) {
        debug_log_append("[Wake] Listener restarted", "system");
    }
    notify_host_state();
}

void notify_host_wake_event() {
    if (!g_host_claimed || g_runtime_mode != MODE_HOST_ENHANCED) return;

    g_host_session_id = next_host_session_id();
    String json = "{\"type\":\"wake\",\"session_id\":\"";
    json += json_escape(g_host_session_id);
    json += "\",\"source\":\"local_wake\",\"energy\":";
    json += String(voice_last_energy_level());
    json += "}";
    ws_send_host_message(json);
    notify_host_state();
}

String host_server_base_url() {
    String host_ip = g_host_server_ip;
    if (!host_ip.length()) {
        host_ip = wifi_last_ws_remote_ip();
    }
    if (!host_ip.length()) return String();

    String url = "http://";
    url += host_ip;
    url += ":";
    url += String(HOST_SERVER_PORT);
    return url;
}

String host_server_voice_url() {
    String base = host_server_base_url();
    if (!base.length()) return String();
    return base + String(HOST_SERVER_PATH_VOICE_COMMAND);
}

bool run_host_voice_command(String& message) {
    if (!wifi_is_connected()) {
        message = "WiFi disconnected";
        return false;
    }

    String host_url = host_server_voice_url();
    if (!host_url.length()) {
        message = "Host server IP unknown";
        return false;
    }

    const size_t max_bytes = SAMPLE_RATE * SAMPLE_BITS / 8 * VOICE_COMMAND_MS / 1000;
    uint8_t* audio_buf = (uint8_t*)ps_malloc(max_bytes);
    if (!audio_buf) {
        message = "Audio buffer alloc failed";
        return false;
    }

    debug_log_append("[Host] Recording voice command for local server...", "system");
    size_t got = mic_record(audio_buf, max_bytes, NULL);
    if (got == 0) {
        free(audio_buf);
        message = mic_is_busy() ? "Mic busy" : "No audio captured";
        return false;
    }

    String audio_b64 = base64::encode(audio_buf, got);
    free(audio_buf);
    audio_b64.replace("\n", "");
    audio_b64.replace("\r", "");

    String payload = "{\"audio_base64\":\"";
    payload += audio_b64;
    payload += "\",\"audio_format\":\"wav\"}";

    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, host_url)) {
        message = "Host HTTP begin failed";
        return false;
    }

    http.setConnectTimeout(15000);
    http.setTimeout(90000);
    http.addHeader("Content-Type", "application/json");

    int http_code = http.POST((uint8_t*)payload.c_str(), payload.length());
    String response = http.getString();
    http.end();

    if (http_code <= 0) {
        message = String("Host HTTP POST failed: ") + http_code;
        return false;
    }
    if (http_code < 200 || http_code >= 300) {
        message = String("Host HTTP ") + http_code + ": " + response.substring(0, 180);
        return false;
    }

    int action_idx = response.indexOf("\"action\"");
    if (action_idx < 0) {
        message = "Host response missing action";
        return false;
    }
    int action_brace = response.indexOf('{', action_idx);
    if (action_brace < 0) {
        message = "Host action JSON missing";
        return false;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    int action_end = -1;
    for (int i = action_brace; i < (int)response.length(); ++i) {
        char c = response[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                action_end = i;
                break;
            }
        }
    }

    if (action_end < 0) {
        message = "Host action parse failed";
        return false;
    }

    String action_json = response.substring(action_brace, action_end + 1);
    debug_log_append(String("[Host] Action JSON: ") + action_json, "system");
    return apply_action_json_with_feedback(action_json, message);
}

bool run_cloud_voice_command(String& message) {
    const size_t max_bytes = SAMPLE_RATE * SAMPLE_BITS / 8 * VOICE_COMMAND_MS / 1000;
    uint8_t* audio_buf = (uint8_t*)ps_malloc(max_bytes);
    if (!audio_buf) {
        message = "Audio buffer alloc failed";
        return false;
    }

    debug_log_append("[Qwen] Recording voice command...", "system");
    size_t got = mic_record(audio_buf, max_bytes, NULL);
    if (got == 0) {
        free(audio_buf);
        message = mic_is_busy() ? "Mic busy" : "No audio captured";
        return false;
    }

    String qwen_json;
    bool ok = qwen_plan_voice_response_json((const int16_t*)audio_buf, got / 2, qwen_json);
    free(audio_buf);
    if (!ok) {
        message = qwen_json;
        return false;
    }

    debug_log_append(String("[Qwen] Voice JSON: ") + qwen_json, "system");
    return apply_action_json_with_feedback(qwen_json, message);
}

bool parse_has_face_result(const String& text, bool& has_face) {
    int idx = text.indexOf("has_face=");
    if (idx < 0) return false;
    idx += (int)strlen("has_face=");
    if (text.startsWith("true", idx)) {
        has_face = true;
        return true;
    }
    if (text.startsWith("false", idx)) {
        has_face = false;
        return true;
    }
    return false;
}

bool extract_json_string_field_local(const String& json, const char* key, String& value) {
    String pattern = String("\"") + key + "\"";
    int key_idx = json.indexOf(pattern);
    if (key_idx < 0) return false;
    int colon = json.indexOf(':', key_idx + pattern.length());
    if (colon < 0) return false;
    int quote = json.indexOf('"', colon + 1);
    if (quote < 0) return false;

    String raw;
    bool escaped = false;
    for (int i = quote + 1; i < (int)json.length(); ++i) {
        char c = json[i];
        if (escaped) {
            switch (c) {
                case 'n': raw += '\n'; break;
                case 'r': raw += '\r'; break;
                case 't': raw += '\t'; break;
                case '\\': raw += '\\'; break;
                case '"': raw += '"'; break;
                default: raw += c; break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            value = raw;
            return true;
        }
        raw += c;
    }
    return false;
}

FaceIndex face_from_name(const String& face_name) {
    if (face_name == "happy") return FACE_HAPPY;
    if (face_name == "confused") return FACE_CONFUSED;
    if (face_name == "surprised") return FACE_SURPRISED;
    if (face_name == "listening") return FACE_LISTENING;
    return FACE_IDLE;
}

void show_face_by_name(const String& face_name) {
    FaceIndex face = face_from_name(face_name);
    screen_show_face_jpeg(FACE_FILES[face]);
    g_current_face = face;
}

void servo180_set_if_ready(int angle_deg) {
    if (!ENABLE_SERVO_180) return;
    if (!servo_180.attached()) servo_180.attach(SERVO_180_PIN);
    servo_180.set_angle(angle_deg);
}

void servo_motion_from_name(const String& servo_name) {
    if (!ENABLE_SERVO_180) return;

    if (servo_name == "confused") {
        servo180_set_if_ready(60);
        return;
    }

    if (servo_name == "surprised") {
        for (int i = 0; i < 3; ++i) {
            servo180_set_if_ready(66);
            delay(130);
            servo180_set_if_ready(80);
            delay(130);
        }
        servo180_set_if_ready(70);
        return;
    }

    if (servo_name == "happy") {
        for (int i = 0; i < 3; ++i) {
            servo180_set_if_ready(62);
            delay(150);
            servo180_set_if_ready(78);
            delay(150);
        }
        servo180_set_if_ready(70);
        return;
    }

    servo180_set_if_ready(70);
}

bool apply_qwen_voice_json(const String& json_text, String& answer_out) {
    String result;
    String face;
    String servo;
    String answer;
    String light;

    if (!extract_json_string_field_local(json_text, "result", result)) return false;
    if (!extract_json_string_field_local(json_text, "face", face)) return false;
    if (!extract_json_string_field_local(json_text, "servo", servo)) return false;
    if (!extract_json_string_field_local(json_text, "answer", answer)) return false;
    extract_json_string_field_local(json_text, "light", light);

    answer_out = answer;

    if (result == "error") {
        show_face_by_name("confused");
        servo_motion_from_name("confused");
        return false;
    }

    if (light == "on") {
        g_sys.light_on = true;
    } else if (light == "off") {
        g_sys.light_on = false;
    }

    show_face_by_name(face);
    servo_motion_from_name(servo);
    return true;
}

bool ultrasonic_pins_conflict(int trig_pin, int echo_pin) {
    const int reserved_pins[] = {
        1,   // I2S DOUT
        2,   // LCD CS
        3,   // LCD DC
        4,   // Servo180
        5,   // I2S LRCLK
        6,   // I2S BCLK
        7,   // LCD SCLK
        8,   // Head pan servo / legacy UART RX
        9,   // LCD MOSI
        41,  // PDM DIN
        42   // PDM CLK
    };

    for (size_t i = 0; i < sizeof(reserved_pins) / sizeof(reserved_pins[0]); ++i) {
        if (trig_pin == reserved_pins[i] || echo_pin == reserved_pins[i]) {
            return true;
        }
    }
    return false;
}

void update_presence_face(bool has_person) {
    g_sys.person_present = has_person;
    if (has_person) {
        screen_show_face_jpeg(FACE_FILES[FACE_HAPPY]);
        g_current_face = FACE_HAPPY;
    } else {
        screen_clear(ST77XX_BLACK);
    }
}

void prepare_and_enter_deep_sleep() {
    debug_log_append("[Sleep] Prepare deep sleep", "system");
    set_last_result("Entering deep sleep");

    voice_set_listener_enabled(false);
    voice_stop_wake_listener();
    voice_wait_listener_stopped(400);
    g_voice_waiting_command = false;
    g_voice_ack_pending = false;
    clear_host_voice_state();
    g_wake_autostart_pending = false;

    if (ENABLE_SERVO_180 && servo_180.attached()) servo_180.detach();
    if (ENABLE_SERVO_HEAD_PAN && servo_head_pan.attached()) servo_head_pan.detach();

    speaker_idle();
    speaker_deinit();
    camera_deinit();
    screen_enter_sleep();

    delay(80);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_WAKE_MINUTES * 60ULL * 1000000ULL);

    delay(120);
    esp_deep_sleep_start();
}

void run_presence_probe_once() {
    if (g_presence_probe_busy || is_voice_command_stage_active()) return;

    g_presence_probe_busy = true;
    camera_fb_t* fb = camera_capture();
    if (!fb) {
        debug_log_append("[Presence] Capture failed", "detect");
        g_presence_probe_busy = false;
        return;
    }

    debug_log_append("[Presence] Object near, capture once for face check", "detect");
    String result_text;
    bool ok = qwen_recognize_image(fb->buf, fb->len, result_text);
    camera_return_fb(fb);
    if (!ok) {
        debug_log_append("[Presence] Qwen check failed: " + result_text, "detect");
        g_presence_probe_busy = false;
        return;
    }

    bool has_face = false;
    if (!parse_has_face_result(result_text, has_face)) {
        debug_log_append("[Presence] Face result parse failed: " + result_text, "detect");
        g_presence_probe_busy = false;
        return;
    }

    debug_log_append(String("[Presence] ") + result_text, "detect");
    update_presence_face(has_face);
    g_presence_probe_busy = false;
}

void presence_loop() {
    if (!ultrasonic_is_ready()) return;
    if (millis() - g_presence_last_check_ms < ULTRASONIC_POLL_MS) return;
    g_presence_last_check_ms = millis();

    float distance_cm = -1.0f;
    const bool got_distance = ultrasonic_read_cm(distance_cm);
    g_ultrasonic_last_ok = got_distance;
    const bool object_near = got_distance && distance_cm > 0.0f && distance_cm <= ULTRASONIC_TRIGGER_CM;

    if (got_distance) {
        debug_log_printf("detect", "[Ultra] distance=%.1f cm", distance_cm);
    }

    const bool edge_trigger = object_near && !g_presence_last_object_near;
    const bool should_probe = edge_trigger && !g_presence_checked_for_current_object;

    if (should_probe) {
        g_presence_checked_for_current_object = true;
        debug_log_append("[Presence] Object entered range, run face check once", "detect");
        run_presence_probe_once();
    } else if (!object_near && g_presence_last_object_near) {
        g_presence_checked_for_current_object = false;
        g_sys.person_present = false;
        screen_clear(ST77XX_BLACK);
        debug_log_append("[Presence] Object left, reset to idle and screen off", "detect");
    }

    g_presence_last_object_near = object_near;
}

// ═══════════════════════════════════════
// 状态广播
// ═══════════════════════════════════════
String status_provider() {
    String json;
    json.reserve(2048);
    json += ",\"boot_id\":";
    json += String(g_boot_id);
    json += ",\"last_result\":\"";
    json += json_escape(g_last_result);
    json += "\"";
    json += ",\"debug_log\":\"";
    json += json_escape(debug_log_text());
    json += "\"";
    json += ",\"person\":";
    json += (g_sys.person_present ? "true" : "false");
    json += ",\"light\":";
    json += (g_sys.light_on ? "true" : "false");
    json += ",\"fan\":";
    json += (g_sys.fan_on ? "true" : "false");
    json += ",\"speaker\":";
    json += (g_sys.speaker_on ? "true" : "false");
    json += ",\"wake_energy\":";
    json += String(voice_last_energy_level());
    json += ",\"wake_listener\":";
    json += (voice_listener_running() ? "true" : "false");
    json += ",\"runtime_mode\":\"";
    json += runtime_mode_name();
    json += "\"";
    json += ",\"host_claimed\":";
    json += (g_host_claimed ? "true" : "false");
    json += ",\"host_waiting_action\":";
    json += (g_host_waiting_action ? "true" : "false");
    json += ",\"host_server_ip\":\"";
    json += json_escape(g_host_server_ip.length() ? g_host_server_ip : wifi_last_ws_remote_ip());
    json += "\"";
    json += ",\"voice_stage\":\"";
    json += voice_stage_name();
    json += "\"";
    json += ",\"ultrasonic_ready\":";
    json += (ultrasonic_is_ready() ? "true" : "false");
    json += ",\"ultrasonic_ok\":";
    json += (g_ultrasonic_last_ok ? "true" : "false");
    json += ",\"ultrasonic_distance_cm\":";
    json += String(ultrasonic_last_distance_cm(), 1);
    json += ",\"face\":";
    json += String((int)g_current_face);
    json += ",\"camera\":\"disabled\"";
    json += ",\"qwen\":\"";
    json += json_escape(qwen_config_status());
    json += "\"";
    return json;
}

void broadcast_status() {
    String json = "{\"type\":\"status\",";
    json += "\"person\":";
    json += (g_sys.person_present ? "true" : "false");
    json += ",\"light\":";
    json += (g_sys.light_on ? "true" : "false");
    json += ",\"fan\":";
    json += (g_sys.fan_on ? "true" : "false");
    json += ",\"speaker\":";
    json += (g_sys.speaker_on ? "true" : "false");
    json += ",\"face\":";
    json += String((int)g_current_face);
    json += ",\"runtime_mode\":\"";
    json += runtime_mode_name();
    json += "\"";
    json += ",\"host_claimed\":";
    json += (g_host_claimed ? "true" : "false");
    json += ",\"voice_stage\":\"";
    json += voice_stage_name();
    json += "\"}";
    ws_broadcast(json);
}

// ═══════════════════════════════════════
// 图片上传
// ═══════════════════════════════════════
static void on_image_data(const uint8_t* data, size_t len, size_t index, size_t total) {
    if (total == 0 || total > IMAGE_BYTES || !g_image_buf) return;
    if (index + len > total) len = total - index;
    memcpy(g_image_buf + index, data, len);
    if (index + len >= total) {
        screen_show_rgb565((const uint16_t*)g_image_buf, IMAGE_W, IMAGE_H);
        set_last_result("Image displayed");
    }
}

// ═══════════════════════════════════════
// WebSocket 消息处理（前端控制面板）
// ═══════════════════════════════════════
static void on_ws_message(const String& msg) {
    if (msg.indexOf("\"type\":\"host_claim\"") >= 0) {
        g_host_claimed = true;
        g_host_server_ip = wifi_last_ws_remote_ip();
        set_runtime_mode(MODE_HOST_ENHANCED, "claimed by host");
        notify_host_hello();
        if (g_host_server_ip.length()) {
            debug_log_printf("system", "[Host] Server IP = %s", g_host_server_ip.c_str());
        }
        notify_host_state();
        broadcast_status();
        return;
    }

    if (msg.indexOf("\"type\":\"host_release\"") >= 0) {
        g_host_claimed = false;
        g_host_server_ip = "";
        resume_wake_listener_after_stage();
        set_runtime_mode(MODE_STANDALONE, "released by host");
        notify_host_state();
        broadcast_status();
        return;
    }

    if (msg.indexOf("\"type\":\"action\"") >= 0) {
        String session_id;
        extract_json_string_field_local(msg, "session_id", session_id);
        if (session_id.length() && g_host_session_id.length() && session_id != g_host_session_id) {
            debug_log_printf("system",
                             "[Host] Ignore action for stale session %s (expect %s)",
                             session_id.c_str(), g_host_session_id.c_str());
            return;
        }
        g_host_pending_action_json = msg;
        g_host_action_pending = true;
        g_host_waiting_action = false;
        g_host_wait_deadline_ms = 0;
        if (session_id.length()) g_host_session_id = session_id;
        debug_log_append("[Host] Action received from host", "system");
        notify_host_state();
        return;
    }

    if (msg.indexOf("\"type\":\"stop\"") >= 0 || msg.indexOf("\"type\":\"reset\"") >= 0) {
        debug_log_append("[Host] Voice stage cancelled by host", "system");
        resume_wake_listener_after_stage();
        set_last_result("Host cancelled current voice stage");
        broadcast_status();
        return;
    }

    if (msg.indexOf("\"type\":\"cmd\"") >= 0) {
        if (msg.indexOf("\"device\":\"light\"") >= 0) {
            g_sys.light_on = (msg.indexOf("\"action\":\"on\"") >= 0);
            log_event("command", g_sys.light_on ? "台灯 · 开启" : "台灯 · 关闭");
        }
        if (msg.indexOf("\"device\":\"fan\"") >= 0) {
            g_sys.fan_on = (msg.indexOf("\"action\":\"on\"") >= 0);
            log_event("command", g_sys.fan_on ? "风扇 · 开启" : "风扇 · 关闭");
            if (ENABLE_RF) {
                g_sys.fan_on ? rf_send_on() : rf_send_off();
            }
        }
        if (msg.indexOf("\"device\":\"speaker\"") >= 0) {
            g_sys.speaker_on = (msg.indexOf("\"action\":\"on\"") >= 0);
            log_event("command", g_sys.speaker_on ? "音响 · 开启" : "音响 · 关闭");
        }
    }
    if (msg.indexOf("\"type\":\"scene\"") >= 0) {
        if (msg.indexOf("\"name\":\"home\"") >= 0) {
            g_sys.light_on = true;
            g_current_face = FACE_HAPPY;
            screen_show_face_jpeg(FACE_FILES[FACE_HAPPY]);
            log_event("scene", "回家模式: 开灯 + 欢迎");
        }
        if (msg.indexOf("\"name\":\"away\"") >= 0) {
            g_sys.light_on   = false;
            g_sys.fan_on     = false;
            g_sys.speaker_on = false;
            g_current_face = FACE_SLEEP;
            screen_show_face_jpeg(FACE_FILES[FACE_SLEEP]);
            log_event("scene", "离开模式: 全关 + 待机");
        }
    }
    broadcast_status();
}

// ═══════════════════════════════════════
// FreeRTOS 任务
// ═══════════════════════════════════════
static void taskMicRecord(void* pv) {
    (void)pv;
    size_t max_bytes = SAMPLE_RATE * SAMPLE_BITS / 8;  // 1 秒
    g_loopback_len = mic_record(g_loopback_buf, max_bytes, NULL);
    g_task_mic = NULL;
    vTaskDelete(NULL);
}

static void taskSpeakerPlay(void* pv) {
    size_t samples = g_loopback_len / 2;
    int16_t* stereo = (int16_t*)ps_malloc(samples * 2 * sizeof(int16_t));
    if (stereo) {
        int16_t* mono = (int16_t*)g_loopback_buf;
        for (size_t i = 0; i < samples; i++) {
            stereo[i * 2]     = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        speaker_play(stereo, samples * 2, true);
        speaker_idle();
        free(stereo);
    }
    g_task_spk = NULL;
    vTaskDelete(NULL);
}

// ═══════════════════════════════════════
// 调试命令处理
// ═══════════════════════════════════════
static bool handle_debug_command(const String& cmd, String& message) {
    // ── 设备开关 ──
    if (cmd == "light_on") {
        g_sys.light_on = true;
        log_event("command", "台灯 · 开启");
        message = "Light: ON";
    } else if (cmd == "light_off") {
        g_sys.light_on = false;
        log_event("command", "台灯 · 关闭");
        message = "Light: OFF";
    } else if (cmd == "fan_on") {
        g_sys.fan_on = true;
        if (ENABLE_RF) rf_send_on();
        log_event("command", "风扇 · 开启");
        message = ENABLE_RF ? "Fan: ON" : "Fan: ON (RF disabled)";
    } else if (cmd == "fan_off") {
        g_sys.fan_on = false;
        if (ENABLE_RF) rf_send_off();
        log_event("command", "风扇 · 关闭");
        message = ENABLE_RF ? "Fan: OFF" : "Fan: OFF (RF disabled)";
    } else if (cmd == "speaker_on") {
        g_sys.speaker_on = true;
        log_event("command", "音响 · 开启");
        message = "Speaker: ON";
    } else if (cmd == "speaker_off") {
        g_sys.speaker_on = false;
        log_event("command", "音响 · 关闭");
        message = "Speaker: OFF";
    } else if (cmd == "scene_home") {
        g_sys.light_on = true;
        g_current_face = FACE_HAPPY;
        screen_show_face_jpeg(FACE_FILES[FACE_HAPPY]);
        log_event("scene", "回家模式: 开灯 + 欢迎");
        message = "Scene: home";
    } else if (cmd == "scene_away") {
        g_sys.light_on   = false;
        g_sys.fan_on     = false;
        g_sys.speaker_on = false;
        if (ENABLE_RF) rf_send_off();
        g_current_face = FACE_SLEEP;
        screen_show_face_jpeg(FACE_FILES[FACE_SLEEP]);
        log_event("scene", "离开模式: 全关 + 待机");
        message = "Scene: away";
    }
    // ── 舵机 180° ──
    else if (cmd == "servo180_45") {
        if (!ENABLE_SERVO_180) { message = "Servo180 disabled"; return false; }
        if (!servo_180.attached()) servo_180.attach(SERVO_180_PIN);
        servo_180.set_angle(60);
        message = "Servo180 -> 60 deg";
    } else if (cmd == "servo180_90") {
        if (!ENABLE_SERVO_180) { message = "Servo180 disabled"; return false; }
        if (!servo_180.attached()) servo_180.attach(SERVO_180_PIN);
        servo_180.set_angle(80);
        message = "Servo180 -> 80 deg";
    }
    // ── 头部左右摆动舵机（原 360° 通道，现为 180° 舵机）──
    else if (cmd == "head_pan_left") {
        if (!ENABLE_SERVO_HEAD_PAN) { message = "HeadPan disabled"; return false; }
        if (!servo_head_pan.attached()) servo_head_pan.attach(SERVO_HEAD_PAN_PIN);
        servo_head_pan.set_angle(75);
        message = "HeadPan -> 75 deg";
    } else if (cmd == "head_pan_center") {
        if (!ENABLE_SERVO_HEAD_PAN) { message = "HeadPan disabled"; return false; }
        if (!servo_head_pan.attached()) servo_head_pan.attach(SERVO_HEAD_PAN_PIN);
        servo_head_pan.set_angle(90);
        message = "HeadPan -> 90 deg";
    } else if (cmd == "head_pan_right") {
        if (!ENABLE_SERVO_HEAD_PAN) { message = "HeadPan disabled"; return false; }
        if (!servo_head_pan.attached()) servo_head_pan.attach(SERVO_HEAD_PAN_PIN);
        servo_head_pan.set_angle(105);
        message = "HeadPan -> 105 deg";
    }
    // ── WiFi ──
    else if (cmd == "wifi_status") {
        message = "WiFi: " + String(wifi_is_connected() ? "yes" : "no") +
                  ", IP: " + wifi_ip_string();
    } else if (cmd == "host_mode_host" || cmd.startsWith("host_mode_host:")) {
        g_host_claimed = true;
        if (cmd.startsWith("host_mode_host:")) {
            g_host_server_ip = cmd.substring(strlen("host_mode_host:"));
            g_host_server_ip.trim();
        }
        if (!g_host_server_ip.length()) {
            g_host_server_ip = wifi_last_ws_remote_ip();
        }
        set_runtime_mode(MODE_HOST_ENHANCED, "local debug");
        notify_host_state();
        message = "Host mode: ON @ " + host_server_base_url();
    } else if (cmd == "host_mode_standalone") {
        g_host_claimed = false;
        g_host_server_ip = "";
        resume_wake_listener_after_stage();
        set_runtime_mode(MODE_STANDALONE, "local debug");
        notify_host_state();
        message = "Host mode: OFF";
    }
    // ── Ping ──
    else if (cmd == "ping") {
        message = "Ping ok, uptime=" + String(millis() / 1000UL) + "s";
    }
    // ── Ultrasonic ──
    else if (cmd == "ultrasonic_test") {
        if (!ultrasonic_is_ready()) {
            message = "Ultrasonic: not ready";
            return false;
        }
        float distance_cm = -1.0f;
        bool ok = ultrasonic_read_cm(distance_cm);
        g_ultrasonic_last_ok = ok;
        if (!ok) {
            message = "Ultrasonic: read timeout";
            return false;
        }
        message = "Ultrasonic: " + String(distance_cm, 1) + " cm";
    } else if (cmd == "sleep_deep") {
        g_sleep_pending = true;
        g_sleep_at_ms = millis() + 800;
        message = "Deep sleep armed: WiFi/API off, wake by timer or reset";
    }
    // ── RF ──
    else if (cmd == "rf_on") {
        if (!ENABLE_RF) { message = "RF disabled"; return false; }
        rf_send_on(); message = "RF: ON sent";
    } else if (cmd == "rf_off") {
        if (!ENABLE_RF) { message = "RF disabled"; return false; }
        rf_send_off(); message = "RF: OFF sent";
    }
    // ── Camera ──
    else if (cmd == "capture") {
        camera_fb_t* fb = camera_capture();
        if (!fb) { message = "Capture failed"; return false; }
        message = "Captured: " + String(fb->len) + " bytes";
        camera_return_fb(fb);
        play_tone(1200, 50);
    } else if (cmd == "qwen_status") {
        message = qwen_config_status();
    } else if (cmd == "qwen_recognize") {
        camera_fb_t* fb = camera_capture();
        if (!fb) { message = "Capture failed"; return false; }

        debug_log_append("[Qwen] Captured frame, starting recognition...", "system");
        bool ok = qwen_recognize_image(fb->buf, fb->len, message);
        camera_return_fb(fb);
        if (!ok) return false;
    } else if (cmd == "qwen_voice_test") {
        enter_voice_listening_face();
        bool ok = run_cloud_voice_command(message);
        return ok;
    } else if (cmd == "qwen_voice_sample") {
        String transcript;
        bool ok = qwen_test_voice_sample(transcript);
        if (!ok) {
            message = transcript;
            return false;
        }
        message = "Sample ASR: " + transcript;
    } else if (cmd == "audio_test_on") {
        bool ok = play_feedback_audio(RADIO_ACK_LIGHT_ON, 880, 1180);
        message = ok ? "Audio test: light_on.wav" : "Audio test: tone fallback";
    } else if (cmd == "audio_test_off") {
        bool ok = play_feedback_audio(RADIO_ACK_LIGHT_OFF, 620, 440);
        message = ok ? "Audio test: light_off.wav" : "Audio test: tone fallback";
    } else if (cmd == "audio_test_wake") {
        bool ok = play_feedback_audio(RADIO_ACK_WAKE, 1040, 1320, 1.35f);
        message = ok ? "Audio test: wake.wav" : "Audio test: wake fallback";
    } else if (cmd == "wake_test") {
        String wake_detail;
        bool ok = voice_run_wake_test(2000, wake_detail);
        if (ok) {
            log_event("command", ("本地唤醒 · " + wake_detail).c_str());
            play_feedback_audio(RADIO_ACK_WAKE, 1040, 1320, 1.35f);
            message = "Wake detected: " + wake_detail;
        } else {
            message = "Wake test: " + wake_detail;
            return false;
        }
    } else if (cmd == "wake_listener_on") {
        voice_set_listener_enabled(true);
        bool ok = voice_start_wake_listener();
        message = ok ? "Wake listener: ON" : "Wake listener start failed";
        return ok;
    } else if (cmd == "wake_listener_off") {
        voice_set_listener_enabled(false);
        voice_stop_wake_listener();
        message = "Wake listener: OFF";
    }
    // ── Mic diag ──
    else if (cmd == "mic_diag") {
        MicDiagResult diag;
        if (!mic_run_diag(&diag, 12, 256)) { message = "Mic diag failed"; return false; }
        message = "Mic: reads=" + String(diag.reads_attempted) +
                  " data=" + String(diag.reads_with_data) +
                  " bytes=" + String(diag.total_bytes) +
                  " nz=" + String(diag.nonzero_samples);
    }
    // ── Screen ──
    else if (cmd == "screen_demo") {
        screen_show_test_pattern();
        delay(1200);
        screen_show_message("Hello!", "XIAO S3", "Offset 36");
        message = "Screen demo done";
    }
    // ── Face ──
    else if (cmd == "face_happy") {
        screen_show_face_jpeg(FACE_FILES[FACE_HAPPY]); g_current_face = FACE_HAPPY;
        message = "Face: happy";
    } else if (cmd == "face_idle") {
        screen_show_face_jpeg(FACE_FILES[FACE_IDLE]); g_current_face = FACE_IDLE;
        message = "Face: idle";
    } else if (cmd == "face_sleep") {
        screen_show_face_jpeg(FACE_FILES[FACE_SLEEP]); g_current_face = FACE_SLEEP;
        message = "Face: sleep";
    } else if (cmd == "face_listening") {
        screen_show_face_jpeg(FACE_FILES[FACE_LISTENING]); g_current_face = FACE_LISTENING;
        message = "Face: listening";
    } else if (cmd == "face_surprised") {
        screen_show_face_jpeg(FACE_FILES[FACE_SURPRISED]); g_current_face = FACE_SURPRISED;
        message = "Face: surprised";
    } else if (cmd == "face_cute") {
        screen_show_face_jpeg(FACE_FILES[FACE_CUTE]); g_current_face = FACE_CUTE;
        message = "Face: cute";
    }
    // ── 慢速命令 → FreeRTOS 队列 ──
    else if (cmd == "mic_record" || cmd == "loopback" ||
             cmd == "tone_16k" || cmd == "tone_44k") {
        if (!g_cmd_queue) { message = "Queue not ready"; return false; }
        char buf[CMD_BUF_SIZE] = {};
        cmd.toCharArray(buf, CMD_BUF_SIZE);
        if (xQueueSend(g_cmd_queue, buf, 0) != pdTRUE) {
            message = "Queue full"; return false;
        }
        message = "Queued: " + cmd;
        return true;
    }
    else {
        message = "Unknown: " + cmd;
        return false;
    }

    set_last_result(message);
    return true;
}

// ═══════════════════════════════════════
// setup
// ═══════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(300);
    randomSeed(millis());  // 舵机随机动画种子
    g_boot_id = (uint32_t)esp_random();
    debug_log_begin("Booting...");
    set_last_result("System boot");

    // 内核对象
    g_cmd_queue    = xQueueCreate(8, CMD_BUF_SIZE);
    g_image_buf    = (uint8_t*)ps_malloc(IMAGE_BYTES);
    g_loopback_buf = (uint8_t*)ps_malloc(SAMPLE_RATE * SAMPLE_BITS / 8 * 1);
    set_last_result(g_image_buf ? "Buffers: OK" : "Buffer alloc FAILED");

    // 1. 舵机（最先，安全位置）
    if (ENABLE_SERVO_180 && servo_180.attach(SERVO_180_PIN)) {
        servo_180.set_angle(70);
        update_boot_status(BOOT_SERVO180, true, "Servo180 OK", "Servo180 FAIL");
    } else { update_boot_status(BOOT_SERVO180, false, "Servo180 OK", ENABLE_SERVO_180 ? "Servo180 FAIL" : "Servo180 OFF"); }

    if (ENABLE_SERVO_HEAD_PAN && servo_head_pan.attach(SERVO_HEAD_PAN_PIN)) {
        servo_head_pan.set_angle(90);
        update_boot_status(BOOT_SERVO_HEAD_PAN, true, "HeadPan OK", "HeadPan FAIL");
    } else { update_boot_status(BOOT_SERVO_HEAD_PAN, false, "HeadPan OK", ENABLE_SERVO_HEAD_PAN ? "HeadPan FAIL" : "HeadPan OFF"); }

    // 2. 屏幕
    if (!screen_init()) {
        update_boot_status(BOOT_SCREEN, false, "Screen: OK", "Screen: FAIL");
    } else {
        update_boot_status(BOOT_SCREEN, true, "Screen: OK", "Screen: FAIL");
    }

    // 3. 麦克风
    if (!mic_init()) update_boot_status(BOOT_MIC, false, "Mic: OK", "Mic: FAIL");
    else             update_boot_status(BOOT_MIC, true, "Mic: OK", "Mic: FAIL");

    // 4. 扬声器
    if (!speaker_init()) update_boot_status(BOOT_SPEAKER, false, "Speaker: OK", "Speaker: FAIL");
    else {
        update_boot_status(BOOT_SPEAKER, true, "Speaker: OK", "Speaker: FAIL");
        play_tone(880, 80);
    }

    // 5. RF
    if (ENABLE_RF) {
        rf_init(RF_TX_PIN);
        update_boot_status(BOOT_RF, true, "RF: OK", "RF: FAIL");
    } else {
        update_boot_status(BOOT_RF, false, "RF: OK", "RF: OFF");
    }

    // 5a. UART → 51 MCU (阶段C再启用，当前 GPIO9 与 LCD MOSI 冲突)
    // uart_init();

    // 6. 摄像头
    if (!camera_init()) update_boot_status(BOOT_CAMERA, false, "Camera: OK", "Camera: FAIL");
    else                update_boot_status(BOOT_CAMERA, true, "Camera: OK", "Camera: FAIL");

    // 7. 超声波测距（引脚待定时可保持未启用）
    if (ultrasonic_pins_conflict(ULTRASONIC_TRIG_PIN, ULTRASONIC_ECHO_PIN)) {
        update_boot_status(BOOT_ULTRASONIC, false, "UltraSonic: OK", "UltraSonic: CONFLICT");
        debug_log_printf("system",
                         "[Ultra] Pin conflict: trig=%d echo=%d overlaps LCD/I2S/mic/RF/servo pins",
                         ULTRASONIC_TRIG_PIN, ULTRASONIC_ECHO_PIN);
    } else if (ultrasonic_init(ULTRASONIC_TRIG_PIN, ULTRASONIC_ECHO_PIN)) {
        update_boot_status(BOOT_ULTRASONIC, true, "UltraSonic: OK", "UltraSonic: OFF");
        debug_log_append("[Ultra] HC-SR04 ready", "system");
    } else {
        update_boot_status(BOOT_ULTRASONIC, false, "UltraSonic: OK", "UltraSonic: OFF");
        debug_log_append("[Ultra] HC-SR04 disabled, waiting for valid GPIO pins", "system");
    }

    // 8. 视觉模块保留初始化，后续可作为本地人脸链路扩展
    vision_init();

    // 9. LittleFS
    if (!wifi_littlefs_init()) update_boot_status(BOOT_LITTLEFS, false, "LittleFS: OK", "LittleFS: FAIL");
    else                       update_boot_status(BOOT_LITTLEFS, true, "LittleFS: OK", "LittleFS: FAIL");

    // 9a. 音频反馈资源（走 LittleFS，与表情同路径策略）
    const bool has_feedback_audio = LittleFS.exists(RADIO_ACK_LIGHT_ON) || LittleFS.exists(RADIO_ACK_LIGHT_OFF);
    update_boot_status(BOOT_AUDIO_FB, has_feedback_audio, "AudioFB: file mode", "AudioFB: tone mode");
    if (has_feedback_audio) {
        debug_log_append("[Audio] LittleFS feedback audio ready", "system");
    } else {
        debug_log_append("[Audio] LittleFS feedback audio missing, using tones", "system");
    }

    // 10. WiFi + WebServer（最后，因为会用屏幕显示 IP）
    qwen_set_config({ QWEN_IMAGE_API_URL, QWEN_VOICE_API_URL, QWEN_API_KEY, QWEN_IMAGE_MODEL, QWEN_IMAGE_PROMPT, QWEN_VOICE_MODEL });
    wifi_set_ui_config({ false, false });  // 混合方案: 板端仅保留轻量 API，不托管完整调试页
    wifi_set_command_handler(handle_debug_command);
    wifi_set_status_provider(status_provider);
    wifi_set_image_handler(on_image_data);
    wifi_set_ws_message_handler(on_ws_message);

    if (!wifi_init(WIFI_SSID, WIFI_PASSWORD)) {
        update_boot_status(BOOT_WIFI, false, "WiFi: OK", "WiFi: FAIL");
        if (screen_is_ready())
            screen_show_init_status("System Boot", g_boot_items, BOOT_MODULE_COUNT, "WiFi failed");
    } else {
        debug_log_set_web_ready(true);
        update_boot_status(BOOT_WIFI, true, "WiFi: OK", "WiFi: FAIL", wifi_ip_string().c_str());
        debug_log_printf("system", "Web UI: http://%s", wifi_ip_string().c_str());
        if (screen_is_ready()) {
            screen_show_init_status("System Ready", g_boot_items, BOOT_MODULE_COUNT, wifi_ip_string().c_str());
            delay(2500);
            screen_show_face_jpeg(FACE_FILES[FACE_IDLE]);
            g_current_face = FACE_IDLE;
        }
    }

    // 11. continuous 唤醒改为延迟自动启动，避开 boot 高峰
    voice_set_listener_enabled(true);
    g_wake_autostart_pending = true;
    g_wake_autostart_at_ms = millis() + WAKE_LISTENER_AUTOSTART_MS;
    debug_log_printf("system", "[Wake] Gate listener auto-start in %u ms", (unsigned)WAKE_LISTENER_AUTOSTART_MS);

    set_last_result("Setup complete");
}

// ═══════════════════════════════════════
// loop
// ═══════════════════════════════════════
void loop() {
    // ── 1. 队列命令 ──
    if (g_state == ST_IDLE) {
        char cmd_buf[CMD_BUF_SIZE];
        if (xQueueReceive(g_cmd_queue, cmd_buf, 0) == pdTRUE) {
            String cmd = String(cmd_buf);
            set_last_result("Run: " + cmd);

            if (cmd == "mic_record") {
                const size_t max_bytes = SAMPLE_RATE * SAMPLE_BITS / 8;
                uint8_t* buf = (uint8_t*)ps_malloc(max_bytes);
                if (buf) {
                    size_t got = mic_record(buf, max_bytes, NULL);
                    set_last_result(got > 0 ? "Mic: got " + String(got) + " bytes"
                                            : "Mic: no data");
                    free(buf);
                }
            } else if (cmd == "loopback") {
                if (g_loopback_buf) {
                    xTaskCreatePinnedToCore(taskMicRecord, "mic_cap",
                                            4096, NULL, 2, &g_task_mic, 0);
                    g_state = ST_LOOPBACK_REC;
                }
            } else if (cmd == "tone_16k") {
                speaker_play_tone(440, 1000, 16000, 16000);
            } else if (cmd == "tone_44k") {
                speaker_play_tone(440, 1000, 16000, 44100);
            }
        }
    }

    // ── 2. 舵机待机动画 ──
    static unsigned long last_servo_idle = 0;
    static int idle_angle = 90;
    if (ENABLE_SERVO_IDLE_ANIMATION &&
        ENABLE_SERVO_180 &&
        servo_180.attached() &&
        millis() - last_servo_idle > 3000) {
        last_servo_idle = millis();
        idle_angle = 60 + random(0, 21);  // 60°~80° 微摆
        servo_180.set_angle(idle_angle);
    }

    // ── 3. Loopback 状态机 ──
    if (g_state == ST_LOOPBACK_REC && g_task_mic == NULL) {
        if (g_loopback_len == 0) {
            set_last_result("Loopback: no data");
            g_state = ST_IDLE;
        } else {
            xTaskCreatePinnedToCore(taskSpeakerPlay, "spk_play",
                                    8192, NULL, 3, &g_task_spk, 1);
            g_state = ST_LOOPBACK_PLAY;
        }
    }
    if (g_state == ST_LOOPBACK_PLAY && g_task_spk == NULL) {
        set_last_result("Loopback: done");
        g_state = ST_IDLE;
    }

    // ── 3. WiFi/WebSocket 处理 ──
    wifi_handle_client();

    // ── 4. WiFi 自动重连 ──
    static unsigned long last_wifi_check = 0;
    if (millis() - last_wifi_check > 10000) {
        last_wifi_check = millis();
        if (!wifi_is_connected()) {
            debug_log_append("[WiFi] Lost connection, reconnecting...", "system");
            debug_log_set_web_ready(false);
            wifi_init(WIFI_SSID, WIFI_PASSWORD);
            if (wifi_is_connected()) debug_log_set_web_ready(true);
        }
    }

    // ── 5. 定时状态广播 ──
    static unsigned long last_bcast = 0;
    if (millis() - last_bcast > 2000) {
        broadcast_status();
        last_bcast = millis();
    }

    // ── 6. 距离触发的人体确认链路 ──
    if (!is_voice_command_stage_active()) {
        presence_loop();
    }

    // ── 7. 语音识别 ──
    if (!is_voice_command_stage_active()) {
        voice_loop();
    }

    if (g_wake_autostart_pending && !is_voice_command_stage_active() &&
        millis() >= g_wake_autostart_at_ms && !voice_listener_running()) {
        if (voice_start_wake_listener()) {
            g_wake_autostart_pending = false;
            debug_log_append("[Wake] Gate listener online", "system");
        }
    }

    // ── 7a. 唤醒后进入云端指令阶段 ──
    if (!g_voice_waiting_command && voice_wake_detected()) {
        g_voice_ack_pending = true;
        g_voice_waiting_command = false;
        g_host_waiting_action = false;
        g_host_action_pending = false;
        voice_stop_wake_listener();
        if (!voice_wait_listener_stopped()) {
            debug_log_append("[Wake] Listener stop timeout, continue anyway", "system");
        }
        enter_voice_listening_face();
        const bool wake_file_ok = LittleFS.exists(RADIO_ACK_WAKE);
        debug_log_printf("system", "[Wake] Stage2 begin, wake file %s", wake_file_ok ? "ready" : "missing");
        const bool audio_ok = play_feedback_audio(RADIO_ACK_WAKE, 1040, 1320, 1.35f);
        log_event("command", audio_ok ? "本地唤醒 · 已播放应答，准备录制云端指令"
                                      : "本地唤醒 · 应答音失败，准备录制云端指令");
        set_last_result(audio_ok ? "Wake detected, ack played, waiting next stage..."
                                 : "Wake detected, ack fallback, waiting next stage...");
        g_voice_ack_deadline_ms = millis() + 450;
        notify_host_state();
    }

    if (g_voice_ack_pending && millis() >= g_voice_ack_deadline_ms) {
        g_voice_ack_pending = false;
        if (g_runtime_mode == MODE_HOST_ENHANCED && g_host_claimed) {
            g_voice_waiting_command = true;
            notify_host_wake_event();
            debug_log_append("[Host] Wake forwarded to host, start host HTTP command", "system");
            set_last_result("Wake ack complete, recording host command...");
        } else {
            g_voice_waiting_command = true;
            debug_log_append("[Wake] Ack window complete, start cloud recording", "system");
            set_last_result("Wake ack complete, recording command...");
        }
        notify_host_state();
    }

    if (g_voice_waiting_command) {
        String voice_message;
        bool ok = false;
        if (g_runtime_mode == MODE_HOST_ENHANCED && g_host_claimed) {
            ok = run_host_voice_command(voice_message);
            if (!ok) {
                debug_log_append("[Host] Host command failed, fallback to standalone cloud command", "system");
                ok = run_cloud_voice_command(voice_message);
            }
        } else {
            ok = run_cloud_voice_command(voice_message);
        }
        set_last_result(voice_message);
        if (!ok) {
            debug_log_append("[Voice] Cloud command failed: " + voice_message, "system");
        }
        resume_wake_listener_after_stage();
        broadcast_status();
    }

    if (!is_voice_command_stage_active() && voice_listener_enabled() && !voice_listener_running()) {
        if (voice_start_wake_listener()) {
            debug_log_append("[Wake] Listener auto-recovered", "system");
        }
    }

    if (g_sleep_pending && millis() >= g_sleep_at_ms) {
        g_sleep_pending = false;
        prepare_and_enter_deep_sleep();
    }

    // ── 8. 语音指令 → 动作 ──
    VoiceCommand vc = voice_last_command();
    if (vc != CMD_NONE) {
        execute_voice_command(vc);
        voice_clear_command();
        broadcast_status();
    }

    delay(10);
}
