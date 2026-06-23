#ifndef COMMAND_MAP_H
#define COMMAND_MAP_H

/**
 * command_map — 指令→动作映射表
 *
 * 将语音指令 ID 和视觉事件映射为设备动作:
 *   - 更新系统状态 (g_sys)
 *   - 控制 LCD 表情 (screen_show_face_jpeg)
 *   - RF 发射 (rf_send_on/off)
 *   - 舵机动作 (servo_180/360)
 *   - 音频反馈 (play_tone)
 *   - WebSocket 广播 (ws_broadcast / log_event)
 *
 * 由 main.ino 在主循环中调用，根据 voice_driver 和 vision_driver
 * 的输出触发对应的 execute_* 函数。
 */

#include <Arduino.h>
#include "screen_driver.h"
#include "voice_driver.h"
#include "vision_driver.h"
#include "rf_driver.h"
#include "servo_driver.h"
#include "speaker_driver.h"
#include "wifi_driver.h"
#include "uart_driver.h"
#include "debug_log.h"

constexpr int COMMAND_SERVO_180_PIN = 4;

// ── 外部引用（定义在 main.ino）──
struct SystemState {
    bool person_present;
    bool light_on;
    bool fan_on;
    bool speaker_on;
    unsigned long last_person_change;
};
extern SystemState g_sys;
extern FaceIndex g_current_face;
extern Servo180Driver servo_180;
extern Servo360Driver servo_360;

void log_event(const char* category, const char* message);
void play_tone(int freq, int duration_ms, int amplitude = 16000);

// ── 语音指令执行 ──
inline void execute_voice_command(VoiceCommand cmd) {
    if (cmd == CMD_NONE) return;

    const char* name = voice_command_name(cmd);
    debug_log_printf("command", "[Cmd] Voice: %s (id=%d)", name, (int)cmd);

    switch (cmd) {
        // ── 灯光 ──
        case CMD_LIGHT_ON:
            g_sys.light_on = true;
            uart_relay_ctrl(1, true);
            break;
        case CMD_LIGHT_OFF:
            g_sys.light_on = false;
            uart_relay_ctrl(1, false);
            break;

        // ── 风扇 ──
        case CMD_FAN_ON:
            g_sys.fan_on = true;
            rf_send_on();
            break;
        case CMD_FAN_OFF:
            g_sys.fan_on = false;
            rf_send_off();
            break;

        // ── 音响 ──
        case CMD_MUSIC_ON:
            g_sys.speaker_on = true;
            uart_relay_ctrl(2, true);  // 扩展通道 → 音响电源
            break;
        case CMD_STOP:
            g_sys.fan_on = false;
            g_sys.speaker_on = false;
            uart_relay_ctrl(1, false);
            uart_relay_ctrl(2, false);
            rf_send_off();
            break;

        // ── 亮度 ──
        case CMD_BRIGHTER:
            // TODO 阶段C: PWM 调光
            break;
        case CMD_DIMMER:
            // TODO 阶段C: PWM 调光
            break;

        // ── 互动 ──
        case CMD_HELLO:
            if (!servo_180.attached()) servo_180.attach(COMMAND_SERVO_180_PIN);
            servo_180.set_angle(30);
            delay(400);
            servo_180.set_angle(90);
            g_current_face = FACE_HAPPY;
            screen_show_face_jpeg(FACE_FILES[FACE_HAPPY]);
            play_tone(880, 100);
            play_tone(1100, 100);
            break;
        case CMD_COME_HERE:
            if (!servo_180.attached()) servo_180.attach(COMMAND_SERVO_180_PIN);
            servo_180.set_angle(30);
            delay(300);
            servo_180.set_angle(150);
            delay(300);
            servo_180.set_angle(90);
            g_current_face = FACE_CUTE;
            screen_show_face_jpeg(FACE_FILES[FACE_CUTE]);
            break;

        // ── 模式 ──
        case CMD_RELAX_MODE:
            g_sys.light_on = true;
            g_sys.fan_on = false;
            g_sys.speaker_on = true;
            g_current_face = FACE_CUTE;
            screen_show_face_jpeg(FACE_FILES[FACE_CUTE]);
            break;
        case CMD_WORK_MODE:
            g_sys.light_on = true;
            g_sys.fan_on = true;
            g_sys.speaker_on = false;
            rf_send_on();
            g_current_face = FACE_IDLE;
            screen_show_face_jpeg(FACE_FILES[FACE_IDLE]);
            break;
        case CMD_HOME_MODE:
            g_sys.light_on = true;
            g_current_face = FACE_HAPPY;
            screen_show_face_jpeg(FACE_FILES[FACE_HAPPY]);
            break;
        case CMD_AWAY_MODE:
            g_sys.light_on = false;
            g_sys.fan_on = false;
            g_sys.speaker_on = false;
            rf_send_off();
            g_current_face = FACE_SLEEP;
            screen_show_face_jpeg(FACE_FILES[FACE_SLEEP]);
            break;

        // ── 查询 ──
        case CMD_WHAT_TIME: {
            unsigned long sec = millis() / 1000;
            char time_buf[32];
            snprintf(time_buf, sizeof(time_buf), "%lu:%02lu:%02lu",
                     sec / 3600, (sec % 3600) / 60, sec % 60);
            screen_show_message("Running Time", time_buf, "uptime");
            g_current_face = FACE_LISTENING;
            screen_show_face_jpeg(FACE_FILES[FACE_LISTENING]);
            break;
        }

        default: break;
    }

    log_event("command", name);
}

// ── 视觉事件执行 ──
inline void execute_vision_event(VisionEvent event) {
    switch (event) {
        case VISION_PERSON_ENTER:
            g_sys.person_present = true;
            g_sys.last_person_change = millis();
            g_sys.light_on = true;
            uart_relay_ctrl(1, true);
            g_current_face = FACE_HAPPY;
            screen_show_face_jpeg(FACE_FILES[FACE_HAPPY]);
            if (!servo_180.attached()) servo_180.attach(COMMAND_SERVO_180_PIN);
            servo_180.set_angle(30);
            delay(300);
            servo_180.set_angle(90);
            play_tone(660, 80);
            play_tone(880, 120);
            log_event("detect", "检测到人进入 → 自动开灯");
            debug_log_append("[Vision] Person ENTER -> 开灯 + 欢迎", "detect");
            break;

        case VISION_PERSON_LEAVE:
            g_sys.person_present = false;
            g_sys.light_on = false;
            uart_relay_ctrl(1, false);
            g_current_face = FACE_SLEEP;
            screen_show_face_jpeg(FACE_FILES[FACE_SLEEP]);
            log_event("detect", "人已离开 → 延时关灯");
            debug_log_append("[Vision] Person LEAVE -> 关灯 + 待机", "detect");
            break;

        case VISION_MOTION:
            // 挥手控灯: 每次检测到运动 → 切换台灯状态
            g_sys.light_on = !g_sys.light_on;
            uart_relay_ctrl(1, g_sys.light_on);
            g_current_face = FACE_SURPRISED;
            screen_show_face_jpeg(FACE_FILES[FACE_SURPRISED]);
            log_event("detect", g_sys.light_on ? "挥手 → 开灯" : "挥手 → 关灯");
            debug_log_printf("detect", "[Vision] Motion -> Light %s", g_sys.light_on ? "ON" : "OFF");
            break;

        default: break;
    }
}

#endif
