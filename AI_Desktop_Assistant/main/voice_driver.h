#ifndef VOICE_DRIVER_H
#define VOICE_DRIVER_H

#include <Arduino.h>

// ── Edge Impulse 模型开关 ──
// 已训练唤醒词模型 otto-wake，取消注释以启用本地 KWS:
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
void        voice_set_command(VoiceCommand cmd);  // 由服务器消息设置指令

// ── 调试 ──
const char* voice_command_name(VoiceCommand cmd);

#endif
