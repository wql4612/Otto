#ifndef VOICE_DRIVER_H
#define VOICE_DRIVER_H

#include <Arduino.h>

// ── Edge Impulse 模型开关 ──
// 在 Edge Impulse 训练好语音 KWS 模型并导出 Arduino 库后，
// 取消下面这行注释，并安装对应的库:
#define HAS_EI_VOICE_MODEL

// ── 指令 ID（与 command_map.h 对应）──
enum VoiceCommand : int8_t {
    CMD_NONE       = -1,
    CMD_LIGHT_ON   = 0,
    CMD_LIGHT_OFF  = 1,
    CMD_FAN_ON     = 2,
    CMD_FAN_OFF    = 3,
    CMD_MUSIC_ON   = 4,
    CMD_STOP       = 5,
    CMD_BRIGHTER   = 6,
    CMD_DIMMER     = 7,
    CMD_HELLO      = 8,
    CMD_COME_HERE  = 9,
    CMD_RELAX_MODE = 10,
    CMD_WORK_MODE  = 11,
    CMD_WHAT_TIME  = 12,
    CMD_HOME_MODE  = 13,
    CMD_AWAY_MODE  = 14,
    CMD_COUNT      = 15
};

// ── 初始化 ──
bool voice_init();

// ── 主循环调用 ──
void voice_loop();

// ── 状态查询 ──
bool        voice_wake_detected();   // 本周期是否检测到唤醒词
VoiceCommand voice_last_command();   // 最近识别的指令（CMD_NONE = 无）
void        voice_clear_command();   // 消费指令（读取后清零）
bool        voice_run_wake_test(uint32_t timeout_ms, String& detail);
bool        voice_start_wake_listener();
void        voice_stop_wake_listener();
bool        voice_wait_listener_stopped(uint32_t timeout_ms = 2500);
bool        voice_listener_running();
void        voice_set_listener_enabled(bool enabled);
bool        voice_listener_enabled();
uint32_t    voice_last_energy_level();

// ── 调试 ──
const char* voice_command_name(VoiceCommand cmd);

#endif
