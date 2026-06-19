/**
 * AI Desktop Assistant — 主固件 v3
 * 集成：麦克风 / 摄像头 / 扬声器 / 屏幕 / WiFi / 舵机 / RF
 *       + 帧差法运动检测 / 语音识别占位 / 指令映射
 *
 * 引脚配置 (Seeed XIAO ESP32S3 Sense):
 *   LCD:      SCLK=2  MOSI=3  DC=4  CS=5  RST=6  BL=7
 *   I2S 功放: BCLK=6  LRCLK=5  DOUT=1
 *   PDM 麦克: CLK=42  DIN=41 (板载)
 *   SG90舵机: GPIO21   MG996R舵机: GPIO44
 *   433M RF:  GPIO43
 *   UART→51:  TX=9  RX=8
 *   Camera:   板载固定
 *
 * 阶段 B (当前):  帧差法 ✓  语音占位 ✓  指令映射 ✓
 * 阶段 C (待做):  51单片机 UART / 433MHz 编码学习 / 继电器控制
 */

#include "mic_driver.h"
#include "camera_driver.h"
#include "speaker_driver.h"
#include "screen_driver.h"
#include "wifi_driver.h"
#include "servo_driver.h"
#include "rf_driver.h"
#include "vision_driver.h"
#include "voice_driver.h"
#include "command_map.h"
#include "uart_driver.h"

// ═══════════════════════════════════════
// 配置
// ═══════════════════════════════════════
#define SAMPLE_RATE   16000
#define SAMPLE_BITS   16

const char* WIFI_SSID     = "WHU-STU-7.4G";
const char* WIFI_PASSWORD = "2842234004";

constexpr int SERVO_180_PIN = 21;
constexpr int SERVO_360_PIN = 44;
constexpr int RF_TX_PIN     = 43;

#define IMAGE_W     240
#define IMAGE_H     284
#define IMAGE_BYTES (IMAGE_W * IMAGE_H * 2)

#define CMD_BUF_SIZE 64

// ═══════════════════════════════════════
// 全局对象
// ═══════════════════════════════════════
Servo180Driver servo_180;
Servo360Driver servo_360;

static String g_last_result = "Idle";
static String g_debug_log   = "Booting...";

// 系统状态（与前端 + command_map 同步）
SystemState g_sys;

FaceIndex g_current_face = FACE_IDLE;

// 缓冲区
static uint8_t* g_image_buf    = nullptr;
static uint8_t* g_loopback_buf = nullptr;
static size_t   g_loopback_len = 0;

// FreeRTOS
static QueueHandle_t g_cmd_queue = nullptr;
static TaskHandle_t  g_task_mic  = nullptr;
static TaskHandle_t  g_task_spk  = nullptr;

// Loopback 状态机
enum SysState { ST_IDLE, ST_LOOPBACK_REC, ST_LOOPBACK_PLAY };
static SysState g_state = ST_IDLE;

// ═══════════════════════════════════════
// 工具函数
// ═══════════════════════════════════════
void set_last_result(const String& text) {
    g_last_result = text;
    if (g_debug_log.length() > 1200)
        g_debug_log.remove(0, g_debug_log.length() - 1200);
    if (g_debug_log.length() > 0) g_debug_log += "\n";
    g_debug_log += text;
    Serial.println(text);
}

void play_tone(int freq, int duration_ms, int amplitude = 16000) {
    speaker_play_tone(freq, duration_ms, amplitude, SAMPLE_RATE);
}

void log_event(const char* category, const char* message) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"log\",\"category\":\"%s\",\"message\":\"%s\"}",
        category, message);
    ws_broadcast(buf);
}

// ═══════════════════════════════════════
// 状态广播
// ═══════════════════════════════════════
String status_provider() {
    char buf[384];
    snprintf(buf, sizeof(buf),
        ",\"last_result\":\"%s\""
        ",\"debug_log\":\"%s\""
        ",\"person\":%s"
        ",\"light\":%s"
        ",\"fan\":%s"
        ",\"speaker\":%s"
        ",\"face\":%d"
        ",\"camera\":\"%s\"",
        json_escape(g_last_result).c_str(),
        json_escape(g_debug_log).c_str(),
        g_sys.person_present ? "true" : "false",
        g_sys.light_on       ? "true" : "false",
        g_sys.fan_on         ? "true" : "false",
        g_sys.speaker_on     ? "true" : "false",
        (int)g_current_face,
        "ready");
    return String(buf);
}

void broadcast_status() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"status\","
        "\"person\":%s,\"light\":%s,\"fan\":%s,\"speaker\":%s,\"face\":%d}",
        g_sys.person_present ? "true" : "false",
        g_sys.light_on       ? "true" : "false",
        g_sys.fan_on         ? "true" : "false",
        g_sys.speaker_on     ? "true" : "false",
        (int)g_current_face);
    ws_broadcast(buf);
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
    if (msg.indexOf("\"type\":\"cmd\"") >= 0) {
        if (msg.indexOf("\"device\":\"light\"") >= 0) {
            g_sys.light_on = (msg.indexOf("\"action\":\"on\"") >= 0);
            log_event("command", g_sys.light_on ? "台灯 · 开启" : "台灯 · 关闭");
        }
        if (msg.indexOf("\"device\":\"fan\"") >= 0) {
            g_sys.fan_on = (msg.indexOf("\"action\":\"on\"") >= 0);
            log_event("command", g_sys.fan_on ? "风扇 · 开启" : "风扇 · 关闭");
            g_sys.fan_on ? rf_send_on() : rf_send_off();
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
    // ── 舵机 180° ──
    if (cmd == "servo180_0") {
        if (!servo_180.attached()) servo_180.attach(SERVO_180_PIN);
        servo_180.set_angle(0);
        message = "Servo180 -> 0 deg";
    } else if (cmd == "servo180_90") {
        if (!servo_180.attached()) servo_180.attach(SERVO_180_PIN);
        servo_180.set_angle(90);
        message = "Servo180 -> 90 deg";
    } else if (cmd == "servo180_180") {
        if (!servo_180.attached()) servo_180.attach(SERVO_180_PIN);
        servo_180.set_angle(180);
        message = "Servo180 -> 180 deg";
    }
    // ── 舵机 360° ──
    else if (cmd == "servo360_rev") {
        if (!servo_360.attached()) servo_360.attach(SERVO_360_PIN);
        servo_360.set_speed_percent(-50);
        message = "Servo360 -> rev 50%";
    } else if (cmd == "servo360_stop") {
        if (!servo_360.attached()) servo_360.attach(SERVO_360_PIN);
        servo_360.stop();
        message = "Servo360 -> stop";
    } else if (cmd == "servo360_fwd") {
        if (!servo_360.attached()) servo_360.attach(SERVO_360_PIN);
        servo_360.set_speed_percent(50);
        message = "Servo360 -> fwd 50%";
    }
    // ── WiFi ──
    else if (cmd == "wifi_status") {
        message = "WiFi: " + String(wifi_is_connected() ? "yes" : "no") +
                  ", IP: " + wifi_ip_string();
    }
    // ── Ping ──
    else if (cmd == "ping") {
        message = "Ping ok, uptime=" + String(millis() / 1000UL) + "s";
    }
    // ── RF ──
    else if (cmd == "rf_on") {
        rf_send_on(); message = "RF: ON sent";
    } else if (cmd == "rf_off") {
        rf_send_off(); message = "RF: OFF sent";
    }
    // ── Camera ──
    else if (cmd == "capture") {
        camera_fb_t* fb = camera_capture();
        if (!fb) { message = "Capture failed"; return false; }
        message = "Captured: " + String(fb->len) + " bytes";
        camera_return_fb(fb);
        play_tone(1200, 50);
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
    Serial.println("\n=== AI Desktop Assistant v3 ===\n");
    set_last_result("System boot");

    // 内核对象
    g_cmd_queue    = xQueueCreate(8, CMD_BUF_SIZE);
    g_image_buf    = (uint8_t*)ps_malloc(IMAGE_BYTES);
    g_loopback_buf = (uint8_t*)ps_malloc(SAMPLE_RATE * SAMPLE_BITS / 8 * 1);
    set_last_result(g_image_buf ? "Buffers: OK" : "Buffer alloc FAILED");

    // 1. 舵机（最先，安全位置）
    if (servo_180.attach(SERVO_180_PIN)) {
        servo_180.set_angle(90);
        set_last_result("Servo180 OK");
    } else { set_last_result("Servo180 FAIL"); }

    if (servo_360.attach(SERVO_360_PIN)) {
        servo_360.stop();
        set_last_result("Servo360 OK");
    } else { set_last_result("Servo360 FAIL"); }

    // 2. 屏幕
    if (!screen_init()) {
        set_last_result("Screen: FAIL");
    } else {
        set_last_result("Screen: OK");
        screen_show_boot();
    }

    // 3. 麦克风
    if (!mic_init()) set_last_result("Mic: FAIL");
    else              set_last_result("Mic: OK");

    // 4. 扬声器
    if (!speaker_init()) set_last_result("Speaker: FAIL");
    else {
        set_last_result("Speaker: OK");
        play_tone(880, 80);
    }

    // 5. RF
    rf_init(RF_TX_PIN);
    set_last_result("RF: OK");

    // 5b. UART → 51 MCU
    uart_init();

    // 6. 摄像头
    if (!camera_init()) set_last_result("Camera: FAIL");
    else                set_last_result("Camera: OK");

    // 7. 视觉检测 (帧差法)
    vision_init();

    // 8. 语音识别 (占位，等待 Edge Impulse 模型)
    voice_init();

    // 9. LittleFS
    if (!wifi_littlefs_init()) set_last_result("LittleFS: FAIL");
    else                       set_last_result("LittleFS: OK");

    // 10. WiFi + WebServer（最后，因为会用屏幕显示 IP）
    wifi_set_command_handler(handle_debug_command);
    wifi_set_status_provider(status_provider);
    wifi_set_image_handler(on_image_data);
    wifi_set_ws_message_handler(on_ws_message);

    if (!wifi_init(WIFI_SSID, WIFI_PASSWORD)) {
        set_last_result("WiFi: FAIL");
        if (screen_is_ready())
            screen_show_message("WiFi Failed", "No network", "Check credentials");
    } else {
        set_last_result("WiFi: OK");
        Serial.println("\n  >>> http://" + wifi_ip_string() + " <<<\n");
        if (screen_is_ready()) {
            screen_show_message("WiFi Ready", wifi_ip_string().c_str(), "Open browser");
            delay(1500);
            screen_show_face_jpeg(FACE_FILES[FACE_IDLE]);
            g_current_face = FACE_IDLE;
        }
    }

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
    if (servo_180.attached() && millis() - last_servo_idle > 3000) {
        last_servo_idle = millis();
        idle_angle = 85 + random(0, 11);  // 85°~95° 微摆
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
            Serial.println("[WiFi] Lost connection, reconnecting...");
            wifi_init(WIFI_SSID, WIFI_PASSWORD);
        }
    }

    // ── 5. 定时状态广播 ──
    static unsigned long last_bcast = 0;
    if (millis() - last_bcast > 2000) {
        broadcast_status();
        last_bcast = millis();
    }

    // ── 6. 视觉检测 ──
    vision_loop();

    // ── 7. 视觉事件 → 动作 ──
    VisionEvent ve = vision_last_event();
    if (ve != VISION_NONE) {
        execute_vision_event(ve);
        broadcast_status();
    }

    // ── 8. 语音识别 ──
    voice_loop();

    // ── 9. 语音指令 → 动作 ──
    VoiceCommand vc = voice_last_command();
    if (vc != CMD_NONE) {
        execute_voice_command(vc);
        voice_clear_command();
        broadcast_status();
    }

    delay(10);
}
