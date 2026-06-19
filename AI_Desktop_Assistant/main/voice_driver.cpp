/**
 * voice_driver — 语音识别模块
 *
 * Edge Impulse KWS 占位 (B1):
 *   HAS_EI_VOICE_MODEL 未定义时，模块处于旁路模式：
 *   - voice_init() 检测麦克风可用性
 *   - voice_loop() 空转（不执行推理）
 *   - voice_last_command() 始终返回 CMD_NONE
 *
 * Edge Impulse 模型训练完成后激活步骤:
 *   1. 访问 edgeimpulse.com → 创建项目
 *   2. 用 XIAO ESP32S3 Sense 录制中文语音样本:
 *      - 唤醒词 "小猫咪": 50-100 条
 *      - 15 条中文指令各 50 条
 *      - noise/unknown 各 30 条
 *   3. 创建 Impulse: 1 秒窗口, MFCC 特征, CNN 分类
 *   4. 训练 → 验证准确率 > 85%
 *   5. 导出为 "Arduino library" → 安装到 Arduino IDE
 *   6. 取消 #define HAS_EI_VOICE_MODEL 注释
 *   7. 重新编译上传
 *
 * PDM 麦克风: CLK=42, DIN=41 (XIAO ESP32S3 Sense 板载)
 */

#include "voice_driver.h"
#include "mic_driver.h"

namespace {

static bool g_initialized = false;
static bool g_model_loaded = false;

static VoiceCommand g_last_cmd = CMD_NONE;
static bool         g_wake_detected = false;

// 防抖: 同一个指令不连续触发
static VoiceCommand g_prev_cmd = CMD_NONE;
static unsigned long g_last_cmd_ms = 0;

}  // namespace

// ═══════════════════════════════════════
// 指令名映射
// ═══════════════════════════════════════
const char* voice_command_name(VoiceCommand cmd) {
    switch (cmd) {
        case CMD_LIGHT_ON:   return "开灯";
        case CMD_LIGHT_OFF:  return "关灯";
        case CMD_FAN_ON:     return "开风扇";
        case CMD_FAN_OFF:    return "关风扇";
        case CMD_MUSIC_ON:   return "放音乐";
        case CMD_STOP:       return "停下";
        case CMD_BRIGHTER:   return "亮一点";
        case CMD_DIMMER:     return "暗一点";
        case CMD_HELLO:      return "你好";
        case CMD_COME_HERE:  return "过来";
        case CMD_RELAX_MODE: return "休闲模式";
        case CMD_WORK_MODE:  return "工作模式";
        case CMD_WHAT_TIME:  return "现在几点";
        case CMD_HOME_MODE:  return "回家模式";
        case CMD_AWAY_MODE:  return "离开模式";
        default:             return "未知";
    }
}

// ═══════════════════════════════════════
// 公共 API
// ═══════════════════════════════════════

bool voice_init() {
    g_initialized = false;
    g_model_loaded = false;
    g_last_cmd = CMD_NONE;
    g_wake_detected = false;

#ifdef HAS_EI_VOICE_MODEL
    // ── Edge Impulse KWS 模型初始化 ──
    // TODO: 替换 <your-project_inferencing.h> 为实际的库名
    //
    // #include <otto-voice_inferencing.h>
    //
    // 初始化 Edge Impulse 推理引擎:
    //   run_classifier_init();
    //
    // 配置 PDM 麦克风（Edge Impulse 使用自己的 I2S 实例）:
    //   // PDM 引脚: CLK=42, DIN=41
    //   ei_microphone_init();
    //
    // 加载模型:
    //   g_model_loaded = true;
    //
    // Serial.println("[Voice] Edge Impulse KWS model loaded");
    // Serial.println("[Voice] 唤醒词: 小猫咪, 指令: 15 条中文");

    Serial.println("[Voice] EI model placeholder — needs trained model");
    g_initialized = true;
    return true;

#else
    // ── 旁路模式: 检测麦克风可用性 ──
    if (!mic_init()) {
        Serial.println("[Voice] Mic init failed (optional)");
    } else {
        Serial.println("[Voice] Mic OK (PDM 16kHz)");
    }
    Serial.println("[Voice] Bypass mode — no EI model loaded");
    Serial.println("[Voice] Train KWS model at edgeimpulse.com to enable");
    g_initialized = true;
    return true;
#endif
}

void voice_loop() {
    if (!g_initialized) return;

    g_wake_detected = false;
    g_last_cmd = CMD_NONE;

#ifdef HAS_EI_VOICE_MODEL
    // ── Edge Impulse 推理循环 ──
    // TODO: 集成实际的 EI 推理
    //
    // 持续音频采集 + 推理:
    //   ei_impulse_result_t result;
    //   signal_t signal = audio_signal_from_mic();
    //   run_classifier(&signal, &result, false);
    //
    // 检查结果:
    //   if (result.classification[0].value > 0.8f) {
    //       int idx = result.classification[0].label_index;
    //       const char* label = result.classification[0].label;
    //
    //       // 标签 "wake" 或 high confidence → 唤醒词
    //       // 其他标签 → 指令 ID
    //       // 映射 label → VoiceCommand
    //   }
    //
    // 防抖: 同一指令 2 秒内不重复触发
    //   if (cmd != g_prev_cmd || millis() - g_last_cmd_ms > 2000) {
    //       g_last_cmd = cmd;
    //       g_prev_cmd = cmd;
    //       g_last_cmd_ms = millis();
    //   }

    // 示例: 以下为伪代码框架
    // run_classifier_continuous();
    // if (ei_result_available()) {
    //     auto result = ei_get_result();
    //     if (result.label == "wake" && result.value > 0.8) {
    //         g_wake_detected = true;
    //     } else if (result.value > 0.7) {
    //         g_last_cmd = map_label_to_command(result.label);
    //         Serial.printf("[Voice] Command: %s (%.2f)\n",
    //                       voice_command_name(g_last_cmd), result.value);
    //     }
    // }

#else
    // 旁路: 无操作
    // (mic_driver 不在此处持续录音以避免与 wifi_driver 抢 CPU)
#endif
}

bool voice_wake_detected() {
    bool val = g_wake_detected;
    g_wake_detected = false;
    return val;
}

VoiceCommand voice_last_command() {
    VoiceCommand cmd = g_last_cmd;
    return cmd;
}

void voice_clear_command() {
    g_last_cmd = CMD_NONE;
}
