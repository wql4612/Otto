/**
 * voice_driver — 本地唤醒词检测 (KWS)
 *
 * 使用 Edge Impulse 训练的唤醒词模型（otto-wake），检测"小猫咪"。
 * 不调用 ei_microphone_init()，改用 mic_driver 的 mic_record() 录音，
 * 避免 I2S 冲突，唤醒后可直接切换到音频流模式。
 *
 * PDM 麦克风: CLK=42, DIN=41 (XIAO ESP32S3 Sense 板载)
 */

#include "voice_driver.h"
#include "mic_driver.h"

#ifdef HAS_EI_VOICE_MODEL
#include <otto-wake_inferencing.h>
#endif

namespace {

static bool g_initialized = false;

static VoiceCommand g_last_cmd = CMD_NONE;
static bool         g_wake_detected = false;

// KWS 录音缓冲
#define KWS_BUF_BYTES   (EI_CLASSIFIER_RAW_SAMPLE_COUNT * sizeof(int16_t))  // 32000
static uint8_t g_kws_buf[KWS_BUF_BYTES];

}  // namespace

// ═══════════════════════════════════════
// 指令名映射（保留：供服务器返回的指令使用）
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

#ifdef HAS_EI_VOICE_MODEL
// ═══════════════════════════════════════
// EI signal 回调: int16 PCM → float
// ═══════════════════════════════════════
static const int16_t* g_signal_samples = nullptr;

static int kws_get_data(size_t offset, size_t length, float* out_ptr) {
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = (float)g_signal_samples[offset + i] / 32768.0f;
    }
    return 0;
}
#endif

// ═══════════════════════════════════════
// 公共 API
// ═══════════════════════════════════════

bool voice_init() {
    g_initialized = false;
    g_last_cmd = CMD_NONE;
    g_wake_detected = false;

#ifdef HAS_EI_VOICE_MODEL
    // 初始化 Edge Impulse 推理引擎（不调用 ei_microphone_init，用我们自己的 mic）
    run_classifier_init();
    Serial.println("[Voice] EI KWS model loaded (otto-wake)");
    Serial.println("[Voice] Wake word: 小猫咪");
    g_initialized = true;
    return true;
#else
    // 旁路模式：检测麦克风可用性
    if (!mic_init()) {
        Serial.println("[Voice] Mic init failed (optional)");
    } else {
        Serial.println("[Voice] Mic OK (PDM 16kHz)");
    }
    Serial.println("[Voice] Bypass mode — no EI model loaded");
    g_initialized = true;
    return true;
#endif
}

void voice_loop() {
    if (!g_initialized) return;

    g_wake_detected = false;
    g_last_cmd = CMD_NONE;

#ifdef HAS_EI_VOICE_MODEL
    // 录制 1 秒音频（16kHz 16bit mono = 32000 bytes）
    size_t got = mic_record(g_kws_buf, KWS_BUF_BYTES, NULL);
    if (got < KWS_BUF_BYTES) return;

    // 构建 signal_t 指向录音缓冲
    g_signal_samples = (const int16_t*)g_kws_buf;
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;  // 16000 采样点
    signal.get_data = kws_get_data;

    // 推理
    ei_impulse_result_t result;
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    if (err != EI_IMPULSE_OK) return;

    // 找最高置信度的标签
    float max_val = 0.0f;
    int max_idx = -1;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (result.classification[i].value > max_val) {
            max_val = result.classification[i].value;
            max_idx = (int)i;
        }
    }

    // 唤醒词检测
    if (max_idx >= 0 && max_val > 0.8f) {
        const char* label = ei_classifier_inferencing_categories[max_idx];
        if (strcmp(label, "wake") == 0) {
            g_wake_detected = true;
            Serial.printf("[Wake] 小猫咪 (%.2f)\n", max_val);
        } else {
            Serial.printf("[Voice] %s (%.2f) — ignored\n", label, max_val);
        }
    }
#endif
}

bool voice_wake_detected() {
    bool val = g_wake_detected;
    g_wake_detected = false;
    return val;
}

VoiceCommand voice_last_command() {
    return g_last_cmd;
}

void voice_clear_command() {
    g_last_cmd = CMD_NONE;
}

// ═══════════════════════════════════════
// 供 main.ino 设置服务器返回的指令
// ═══════════════════════════════════════
void voice_set_command(VoiceCommand cmd) {
    g_last_cmd = cmd;
}
