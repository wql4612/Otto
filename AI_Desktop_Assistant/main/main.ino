/**
 * AI Desktop Assistant - Main Program
 * 集成：麦克风、摄像头、扬声器、显示屏、WiFi、舵机
 * 功能：独立外设测试、提示音、显示屏测试
 */

#include "mic_driver.h"
#include "camera_driver.h"
#include "speaker_driver.h"
#include "screen_driver.h"
#include "wifi_driver.h"
#include "servo_driver.h"
#include "rf_driver.h"

// ===== 录音参数 =====
#define SAMPLE_RATE 16000
#define SAMPLE_BITS 16

// ===== 图像上传缓冲 =====
#define IMAGE_W 240
#define IMAGE_H 284
#define IMAGE_BYTES (IMAGE_W * IMAGE_H * 2)

// ===== WiFi 参数 =====
const char* WIFI_SSID = "WHU-STU-7.4G";
const char* WIFI_PASSWORD = "2842234004";

// ===== 舵机参数 =====
constexpr int SERVO_180_PIN = 4;
constexpr int SERVO_360_PIN = 44;
// GPIO43 在不少 ESP32-S3 板上会复用到串口/USB 调试, 若 RF 仍异常建议优先改到普通 GPIO.
constexpr int RF_TX_PIN = 43;

// ===== 全局变量 =====
Servo180Driver servo_180;
Servo360Driver servo_360;
static String g_last_result = "Idle";
static String g_debug_log = "Booting...";

// 图像上传缓冲
static uint8_t* g_image_buf = nullptr;

// ===== FreeRTOS 异步命令队列 =====
#define CMD_BUF_SIZE 64
static QueueHandle_t g_cmd_queue = NULL;
static TaskHandle_t g_task_mic = NULL;
static TaskHandle_t g_task_spk = NULL;

// Loopback 跨任务传递
static uint8_t* g_loopback_buf = NULL;
static size_t g_loopback_len = 0;

// 简易状态机
typedef enum { ST_IDLE, ST_LOOPBACK_REC, ST_LOOPBACK_PLAY } SystemState;
static SystemState g_state = ST_IDLE;

static void on_image_data(const uint8_t* data, size_t len, size_t index, size_t total) {
    if (total == 0 || total > IMAGE_BYTES) { set_last_result("Image too large: "+String(total)); return; }
    if (!g_image_buf) return;
    if (index + len > total) len = total - index;
    memcpy(g_image_buf + index, data, len);

    if (index + len >= total) {
        screen_show_rgb565((const uint16_t*)g_image_buf, IMAGE_W, IMAGE_H);
        set_last_result("Image displayed on screen");
    }
}

// loopback test 停止条件
static unsigned long loopback_deadline = 0;
static bool loopback_stop() {
    return millis() >= loopback_deadline;
}

static unsigned long mic_record_deadline = 0;
static bool mic_record_stop() {
    return millis() >= mic_record_deadline;
}

void play_tone(int freq, int duration_ms, int amplitude = 16000) {
    speaker_play_tone(freq, duration_ms, amplitude, 16000);
}

void set_last_result(const String& text) {
    g_last_result = text;
    if (g_debug_log.length() > 1200) {
        g_debug_log.remove(0, g_debug_log.length() - 1200);
    }
    if (g_debug_log.length() > 0) {
        g_debug_log += "\n";
    }
    g_debug_log += text;
    Serial.println(text);
}

bool ensure_servo_180_ready() {
    if (servo_180.attached()) {
        return true;
    }
    if (!servo_180.attach(SERVO_180_PIN)) {
        Serial.printf("Servo180 attach failed on GPIO%d\n", SERVO_180_PIN);
        return false;
    }
    Serial.printf("Servo180 attached on GPIO%d\n", SERVO_180_PIN);
    return true;
}

bool ensure_servo_360_ready() {
    if (servo_360.attached()) {
        return true;
    }
    if (!servo_360.attach(SERVO_360_PIN)) {
        Serial.printf("Servo360 attach failed on GPIO%d\n", SERVO_360_PIN);
        return false;
    }
    Serial.printf("Servo360 attached on GPIO%d\n", SERVO_360_PIN);
    return true;
}

// ===== FreeRTOS 任务函数 =====

void taskMicRecord(void* pv) {
    (void)pv;
    size_t max_bytes = SAMPLE_RATE * SAMPLE_BITS / 8;  // 1 秒
    g_loopback_len = mic_record(g_loopback_buf, max_bytes, NULL);
    g_task_mic = NULL;
    vTaskDelete(NULL);
}

void taskSpeakerPlay(void* pv) {
    size_t samples = g_loopback_len / 2;
    int16_t* stereo = (int16_t*)ps_malloc(samples * 2 * sizeof(int16_t));
    if (stereo) {
        int16_t* mono = (int16_t*)g_loopback_buf;
        for (size_t i = 0; i < samples; i++) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        speaker_play(stereo, samples * 2, true);
        speaker_idle();
        free(stereo);
    }
    g_task_spk = NULL;
    vTaskDelete(NULL);
}

// ===== 麦克风→扬声器直通测试 =====
void loopback_test() {
    const int duration_sec = 1;
    size_t max_bytes = SAMPLE_RATE * SAMPLE_BITS / 8 * duration_sec;

    uint8_t* buf = (uint8_t*)ps_malloc(max_bytes);
    if (!buf) {
        set_last_result("Loopback: malloc failed");
        play_tone(300, 50);
        return;
    }

    set_last_result("Loopback: recording 3 sec");
    play_tone(800, 80);

    loopback_deadline = millis() + (unsigned long)(duration_sec * 1000);
    size_t len = mic_record(buf, max_bytes, loopback_stop);

    if (len == 0) {
        set_last_result("Loopback: got 0 bytes");
        play_tone(300, 40);
        free(buf);
        return;
    }

    size_t samples = len / 2;
    int16_t* stereo = (int16_t*)ps_malloc(samples * 2 * sizeof(int16_t));
    if (!stereo) {
        set_last_result("Loopback: stereo alloc failed");
        free(buf);
        return;
    }

    int16_t* mono = (int16_t*)buf;
    for (size_t i = 0; i < samples; i++) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }

    set_last_result("Loopback: playing back");
    play_tone(600, 80);
    speaker_play(stereo, samples * 2, true);
    speaker_idle();

    free(stereo);
    free(buf);
    set_last_result("Loopback: done");
}

void mic_test_record() {
    const int duration_sec = 1;
    const size_t max_bytes = SAMPLE_RATE * SAMPLE_BITS / 8 * duration_sec;

    uint8_t* buf = (uint8_t*)ps_malloc(max_bytes);
    if (!buf) {
        set_last_result("Mic: malloc failed");
        return;
    }

    set_last_result("Mic: recording 3 sec");

    mic_record_deadline = millis() + (unsigned long)(duration_sec * 1000);
    size_t bytes = mic_record(buf, max_bytes, mic_record_stop);

    if (bytes == 0) {
        set_last_result("Mic: no data");
    } else {
        set_last_result("Mic: got valid data, bytes=" + String(bytes));
    }

    free(buf);
}

void mic_diag_test() {
    MicDiagResult diag;
    if (!mic_run_diag(&diag, 12, 256)) {
        set_last_result("Mic diag failed to start");
        return;
    }

    set_last_result("Mic diag: err=" + String((int)diag.last_err) +
                    ", reads=" + String((unsigned)diag.reads_attempted) +
                    ", data_reads=" + String((unsigned)diag.reads_with_data) +
                    ", bytes=" + String((unsigned)diag.total_bytes) +
                    ", nonzero=" + String((unsigned)diag.nonzero_samples) +
                    ", min=" + String((int)diag.min_sample) +
                    ", max=" + String((int)diag.max_sample));
}

// ===== 拍照测试 =====
bool capture_photo_test() {
    camera_fb_t* fb = camera_capture();
    if (!fb) {
        set_last_result("Capture failed");
        return false;
    }

    set_last_result("Captured JPEG: " + String((unsigned)fb->len) + " bytes");
    camera_return_fb(fb);
    return true;
}

bool handle_debug_command(const String& cmd, String& message) {
    // -- 快速命令：直接执行 --
    if (cmd == "servo180_0") {
        if (!ensure_servo_180_ready()) { message = "Servo attach failed"; return false; }
        servo_180.set_angle(0);
        message = g_last_result = "Servo180 -> 0 deg";
        return true;
    }
    if (cmd == "servo180_90") {
        if (!ensure_servo_180_ready()) { message = "Servo attach failed"; return false; }
        servo_180.set_angle(90);
        message = g_last_result = "Servo180 -> 90 deg";
        return true;
    }
    if (cmd == "servo180_180") {
        if (!ensure_servo_180_ready()) { message = "Servo attach failed"; return false; }
        servo_180.set_angle(180);
        message = g_last_result = "Servo180 -> 180 deg";
        return true;
    }
    if (cmd == "servo360_rev") {
        if (!ensure_servo_360_ready()) { message = "Servo attach failed"; return false; }
        servo_360.set_speed_percent(-50);
        message = g_last_result = "Servo360 -> reverse 50%";
        return true;
    }
    if (cmd == "servo360_stop") {
        if (!ensure_servo_360_ready()) { message = "Servo attach failed"; return false; }
        servo_360.stop();
        message = g_last_result = "Servo360 -> stop";
        return true;
    }
    if (cmd == "servo360_fwd") {
        if (!ensure_servo_360_ready()) { message = "Servo attach failed"; return false; }
        servo_360.set_speed_percent(50);
        message = g_last_result = "Servo360 -> forward 50%";
        return true;
    }
    if (cmd == "wifi_status") {
        wifi_print_status(Serial);
        message = g_last_result = "WiFi: " + String(wifi_is_connected() ? "yes" : "no") + ", ip=" + wifi_ip_string();
        return true;
    }
    if (cmd == "ping") {
        message = g_last_result = "Ping ok, uptime=" + String(millis() / 1000UL) + "s";
        return true;
    }
    if (cmd == "rf_on") {
        rf_send_on();
        message = g_last_result = "RF: ON sent";
        return true;
    }
    if (cmd == "rf_off") {
        rf_send_off();
        message = g_last_result = "RF: OFF sent";
        return true;
    }
    if (cmd == "capture") {
        if (!capture_photo_test()) { message = g_last_result; return false; }
        play_tone(1200, 50);
        message = g_last_result;
        return true;
    }
    if (cmd == "mic_diag") {
        mic_diag_test();
        message = g_last_result;
        return true;
    }

    // -- 慢速命令：通过队列异步执行 --
    if (!g_cmd_queue) { message = "Cmd queue not ready"; return false; }

    char buf[CMD_BUF_SIZE] = {};
    cmd.toCharArray(buf, CMD_BUF_SIZE);
    if (xQueueSend(g_cmd_queue, buf, 0) != pdTRUE) {
        message = "Cmd queue full";
        return false;
    }
    Serial.printf("[%u] queued: %s\n", (unsigned)millis(), cmd.c_str());
    message = "Queued: " + cmd;
    return true;
}

// ===== setup =====
void setup() {
    Serial.begin(115200);
    Serial.println("=== AI Desktop Assistant ===");
    set_last_result("System boot");

    g_cmd_queue = xQueueCreate(8, CMD_BUF_SIZE);
    g_loopback_buf = (uint8_t*)ps_malloc(SAMPLE_RATE * SAMPLE_BITS / 8 * 1);  // 1 秒
    if (g_loopback_buf) set_last_result("Loopback buf: ok");

    // -- 1. PSRAM 缓冲区提前分配 --
    g_image_buf = (uint8_t*)ps_malloc(IMAGE_BYTES);
    if (g_image_buf) {
        set_last_result("Image buf alloc: " + String(IMAGE_BYTES) + " bytes");
    } else {
        set_last_result("Image buf alloc FAILED");
    }

    // -- 2. 舵机最先挂载，置于安全位置（参考：舵机→屏幕→I2S→摄像头的顺序） --
    if (ensure_servo_180_ready()) {
        servo_180.set_angle(90);
        set_last_result("Servo180 @ 90 deg");
    } else {
        set_last_result("Servo180 attach failed");
    }
    if (ensure_servo_360_ready()) {
        servo_360.stop();
        set_last_result("Servo360 stopped");
    } else {
        set_last_result("Servo360 attach failed");
    }

    // -- 3. 屏幕（参考：屏幕在 I2S 之前） --
    if (!screen_init()) {
        Serial.println("Screen init failed (optional)");
        set_last_result("Screen init failed");
    } else {
        set_last_result("Screen ready");
        screen_show_boot();
    }

    // -- 4. 麦克风 → 扬声器 → 摄像头（参考：I2S 在摄像头之前） --
    if (!mic_init()) {
        Serial.println("Mic init failed (optional)");
        set_last_result("Mic init failed");
    } else {
        set_last_result("Mic ready");
    }

    if (!speaker_init()) {
        Serial.println("Speaker init failed (optional)");
        set_last_result("Speaker init failed");
    } else {
        set_last_result("Speaker ready");
        play_tone(880, 100);
    }

    rf_init(RF_TX_PIN);
    set_last_result("RF ready");

    if (!camera_init()) {
        Serial.println("Camera init failed (optional)");
        set_last_result("Camera init failed");
    } else {
        set_last_result("Camera ready");
    }

    // -- 5. WiFi（最后，可用屏幕显示 IP） --
    if (!wifi_init(WIFI_SSID, WIFI_PASSWORD)) {
        Serial.println("WiFi init failed (optional)");
        set_last_result("WiFi init failed — no web control available");
    } else {
        wifi_set_command_handler(handle_debug_command);
        wifi_set_status_provider([]() -> String {
            return String(",\"last_result\":\"") + json_escape(g_last_result) +
                   "\",\"debug_log\":\"" + json_escape(g_debug_log) + "\"";
        });
        wifi_set_image_handler(on_image_data);
        set_last_result("WiFi ready");
        wifi_print_status(Serial);
        if (screen_is_ready()) {
            screen_show_message("WiFi Ready", wifi_ip_string().c_str(), "Status page @ /");
        }
        Serial.println("\nWeb control ready. Open http://" + wifi_ip_string() + " in browser.");
        set_last_result("Web control ready");
    }
}

// ===== loop =====
void loop() {
    char cmd[CMD_BUF_SIZE];

    if (g_state == ST_IDLE && xQueueReceive(g_cmd_queue, cmd, 0) == pdTRUE) {
        String s = String(cmd);
        Serial.printf("[%u] loop exec: %s\n", (unsigned)millis(), cmd);
        set_last_result("Run: " + s);

        if (s == "mic_record") {
            mic_test_record();
        } else if (s == "loopback") {
            if (g_loopback_buf) {
                xTaskCreatePinnedToCore(taskMicRecord, "mic_cap", 4096, NULL, 2, &g_task_mic, 0);
                g_state = ST_LOOPBACK_REC;
            }
        } else if (s == "tone_16k") {
            if (!speaker_play_tone(440, 1000, 16000, 16000))
                set_last_result("Tone 16k failed");
        } else if (s == "tone_44k") {
            if (!speaker_play_tone(440, 1000, 16000, 44100))
                set_last_result("Tone 44k failed");
        } else if (s == "screen_demo") {
            screen_show_test_pattern();
            delay(1200);
            screen_show_message("Hello!", "XIAO S3", "Offset 36");
            set_last_result("Screen demo done");
        }
    }

    // 2. 状态机：loopback 录→播过渡
    if (g_state == ST_LOOPBACK_REC && g_task_mic == NULL) {
        if (g_loopback_len == 0) {
            set_last_result("Loopback: no data");
            g_state = ST_IDLE;
        } else {
            xTaskCreatePinnedToCore(taskSpeakerPlay, "spk_play", 8192, NULL, 3, &g_task_spk, 1);
            g_state = ST_LOOPBACK_PLAY;
        }
    }
    if (g_state == ST_LOOPBACK_PLAY && g_task_spk == NULL) {
        set_last_result("Loopback: done");
        g_state = ST_IDLE;
    }

    delay(10);
}
