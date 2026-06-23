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
#include "debug_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef HAS_EI_VOICE_MODEL
#include <Audio_inferencing.h>
#endif

namespace {

static bool g_initialized = false;
static bool g_model_loaded = false;

static VoiceCommand g_last_cmd = CMD_NONE;
static bool         g_wake_detected = false;

// 防抖: 同一个指令不连续触发
static VoiceCommand g_prev_cmd = CMD_NONE;
static unsigned long g_last_cmd_ms = 0;
static bool g_listener_enabled = false;

#ifdef HAS_EI_VOICE_MODEL
static int16_t* g_infer_buffer_a = nullptr;
static int16_t* g_active_infer_buffer = nullptr;
static size_t g_infer_buffer_samples = 0;
static constexpr float kWakeThreshold = EI_CLASSIFIER_THRESHOLD;
static constexpr const char* kWakeLabel = "wake";
static constexpr const char* kOnLabel = "on";
static constexpr const char* kOffLabel = "off";
static TaskHandle_t g_wake_task = nullptr;
static bool g_wake_task_running = false;
static volatile uint16_t g_gate_threshold = 1800;
static volatile uint32_t g_last_energy = 0;
static volatile uint32_t g_last_peak = 0;
static constexpr uint32_t kGateChunkMs = 160;
static constexpr uint32_t kGateCooldownMs = 350;
static constexpr uint8_t kGateHitRequired = 2;

static int wake_signal_get_data(size_t offset, size_t length, float *out_ptr) {
    if (!g_active_infer_buffer || offset + length > g_infer_buffer_samples) return -1;
    numpy::int16_to_float(&g_active_infer_buffer[offset], out_ptr, length);
    return 0;
}

static bool is_wake_label(const char* label) {
    return label && strcmp(label, kWakeLabel) == 0;
}

static bool ensure_wake_buffer(size_t infer_samples, String* err = nullptr) {
    const size_t infer_bytes = infer_samples * sizeof(int16_t);
    if (!g_infer_buffer_a || g_infer_buffer_samples != infer_samples) {
        if (g_infer_buffer_a) {
            free(g_infer_buffer_a);
            g_infer_buffer_a = nullptr;
        }
        g_infer_buffer_a = (int16_t*)ps_malloc(infer_bytes);
        if (!g_infer_buffer_a) {
            if (g_infer_buffer_a) free(g_infer_buffer_a);
            g_infer_buffer_a = nullptr;
            if (err) *err = "Wake buffer alloc failed";
            return false;
        }
        g_infer_buffer_samples = infer_samples;
    }
    return true;
}

static void update_energy_metrics(const int16_t* samples, size_t sample_count, uint32_t& avg_abs, uint32_t& peak_abs) {
    uint64_t sum_abs = 0;
    uint32_t peak = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t v = samples[i];
        uint32_t mag = (uint32_t)(v < 0 ? -v : v);
        sum_abs += mag;
        if (mag > peak) peak = mag;
    }
    avg_abs = sample_count ? (uint32_t)(sum_abs / sample_count) : 0;
    peak_abs = peak;
}

static bool sample_gate_energy(uint32_t& avg_abs, uint32_t& peak_abs, String* detail = nullptr) {
    const size_t gate_samples = (EI_CLASSIFIER_FREQUENCY * kGateChunkMs) / 1000;
    const size_t gate_bytes = gate_samples * sizeof(int16_t);
    if (!ensure_wake_buffer(gate_samples, detail)) return false;

    size_t got = mic_record((uint8_t*)g_infer_buffer_a, gate_bytes, nullptr);
    if (got != gate_bytes) {
        if (detail) *detail = "Gate audio capture incomplete";
        return false;
    }

    update_energy_metrics(g_infer_buffer_a, gate_samples, avg_abs, peak_abs);
    g_last_energy = avg_abs;
    g_last_peak = peak_abs;
    return true;
}

static bool run_wake_inference_window(String* detail = nullptr) {
    const size_t infer_samples = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    const size_t infer_bytes = infer_samples * sizeof(int16_t);

    if (!ensure_wake_buffer(infer_samples, detail)) return false;

    size_t got = mic_record((uint8_t*)g_infer_buffer_a, infer_bytes, nullptr);
    if (got != infer_bytes) {
        if (detail) *detail = "Wake audio capture incomplete";
        return false;
    }

    uint32_t avg_abs = 0;
    uint32_t peak_abs = 0;
    update_energy_metrics(g_infer_buffer_a, infer_samples, avg_abs, peak_abs);
    g_last_energy = avg_abs;
    g_last_peak = peak_abs;
    g_active_infer_buffer = g_infer_buffer_a;

    signal_t signal;
    signal.total_length = infer_samples;
    signal.get_data = &wake_signal_get_data;

    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
    if (r != EI_IMPULSE_OK) {
        if (detail) *detail = "Wake infer failed: " + String((int)r);
        return false;
    }

    size_t best_ix = 0;
    float best_score = 0.0f;
    float wake_score = 0.0f;
    float on_score = 0.0f;
    float off_score = 0.0f;
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        const char* label = result.classification[ix].label;
        const float score = result.classification[ix].value;
        if (score > best_score) {
            best_score = score;
            best_ix = ix;
        }
        if (is_wake_label(label)) wake_score = score;
        if (label && strcmp(label, kOnLabel) == 0) on_score = score;
        if (label && strcmp(label, kOffLabel) == 0) off_score = score;
    }

    const char* best_label = result.classification[best_ix].label;
    if (detail) {
        *detail = String("top=") + (best_label ? best_label : "?") +
                  " score=" + String(best_score, 3) +
                  " wake=" + String(wake_score, 3) +
                  " on=" + String(on_score, 3) +
                  " off=" + String(off_score, 3) +
                  " energy=" + String(avg_abs) +
                  " peak=" + String(peak_abs);
    }

    if (wake_score >= kWakeThreshold) {
        g_wake_detected = true;
    }
    return true;
}

static void wake_listener_task(void* pv) {
    (void)pv;
    g_wake_task_running = true;
    debug_log_append("[Wake] Listener task started", "system");
    uint8_t gate_hits = 0;
    unsigned long last_trigger_ms = 0;
    unsigned long last_heartbeat_ms = 0;

    while (g_listener_enabled) {
        if (g_wake_detected) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t avg_abs = 0;
        uint32_t peak_abs = 0;
        String gate_detail;
        if (!sample_gate_energy(avg_abs, peak_abs, &gate_detail)) {
            debug_log_append("[Wake] Gate read failed: " + gate_detail, "system");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        const bool gate_open = (avg_abs >= g_gate_threshold) ||
                               (peak_abs >= (uint32_t)g_gate_threshold * 3UL);
        gate_hits = gate_open ? (uint8_t)(gate_hits + 1) : 0;

        if (millis() - last_heartbeat_ms >= 1500) {
            last_heartbeat_ms = millis();
            debug_log_printf("system",
                             "[Wake] gate energy=%lu peak=%lu threshold=%u hits=%u",
                             (unsigned long)avg_abs,
                             (unsigned long)peak_abs,
                             (unsigned)g_gate_threshold,
                             (unsigned)gate_hits);
        }

        if (gate_hits < kGateHitRequired) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (millis() - last_trigger_ms < kGateCooldownMs) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        last_trigger_ms = millis();
        gate_hits = 0;

        String infer_detail;
        debug_log_printf("system", "[Wake] Gate opened, start model window, energy=%lu peak=%lu",
                         (unsigned long)avg_abs, (unsigned long)peak_abs);
        if (!run_wake_inference_window(&infer_detail)) {
            debug_log_append("[Wake] Listener inference failed: " + infer_detail, "system");
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        debug_log_append("[Wake] " + infer_detail, "system");
        if (g_wake_detected) {
            debug_log_append("[Wake] Detected in background", "system");
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }

    g_wake_task_running = false;
    g_wake_task = nullptr;
    g_active_infer_buffer = nullptr;
    debug_log_append("[Wake] Listener task stopped", "system");
    vTaskDelete(NULL);
}
#endif

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
    if (!mic_init()) {
        debug_log_append("[Voice] Mic init failed for wake model", "system");
        return false;
    }

    g_model_loaded = true;
    debug_log_append("[Voice] Local wake model ready", "system");
    g_initialized = true;
    return true;

#else
    // ── 旁路模式: 检测麦克风可用性 ──
    if (!mic_init()) {
        debug_log_append("[Voice] Mic init failed (optional)", "system");
    } else {
        debug_log_append("[Voice] Mic OK (PDM 16kHz)", "system");
    }
    debug_log_append("[Voice] Bypass mode - no EI model loaded", "system");
    debug_log_append("[Voice] Train KWS model at edgeimpulse.com to enable", "system");
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

bool voice_run_wake_test(uint32_t timeout_ms, String& detail) {
    detail = "";

#ifdef HAS_EI_VOICE_MODEL
    if (!g_initialized && !voice_init()) {
        detail = "Wake model init failed";
        return false;
    }
    if (!g_model_loaded) {
        detail = "Wake model not loaded";
        return false;
    }

    const size_t infer_samples = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    const uint32_t model_window_ms = (uint32_t)((infer_samples * 1000ULL) / EI_CLASSIFIER_FREQUENCY);

    if (!g_infer_buffer_a || g_infer_buffer_samples != infer_samples) {
        if (g_infer_buffer_a) {
            free(g_infer_buffer_a);
            g_infer_buffer_a = nullptr;
        }
        const size_t infer_bytes = infer_samples * sizeof(int16_t);
        g_infer_buffer_a = (int16_t*)ps_malloc(infer_bytes);
        if (!g_infer_buffer_a) {
            detail = "Wake buffer alloc failed";
            return false;
        }
        g_infer_buffer_samples = infer_samples;
    }

    debug_log_printf("system", "[Wake] Capture start, requested=%lu ms, model=%lu ms, threshold=%.2f",
                     timeout_ms, model_window_ms, kWakeThreshold);
    size_t got = mic_record((uint8_t*)g_infer_buffer_a, infer_samples * sizeof(int16_t), nullptr);
    if (got != infer_samples * sizeof(int16_t)) {
        detail = "Wake audio capture incomplete";
        return false;
    }

    signal_t signal;
    signal.total_length = infer_samples;
    g_active_infer_buffer = g_infer_buffer_a;
    signal.get_data = &wake_signal_get_data;

    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
    if (r != EI_IMPULSE_OK) {
        detail = "Wake infer failed: " + String((int)r);
        return false;
    }

    size_t best_ix = 0;
    float best_score = 0.0f;
    float wake_score = 0.0f;
    float on_score = 0.0f;
    float off_score = 0.0f;
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        const char* label = result.classification[ix].label;
        const float score = result.classification[ix].value;
        if (score > best_score) {
            best_score = score;
            best_ix = ix;
        }
        if (is_wake_label(label)) wake_score = score;
        if (label && strcmp(label, kOnLabel) == 0) on_score = score;
        if (label && strcmp(label, kOffLabel) == 0) off_score = score;
    }

    const char* best_label = result.classification[best_ix].label;
    detail = String("top=") + (best_label ? best_label : "?") +
             " score=" + String(best_score, 3) +
             " wake=" + String(wake_score, 3) +
             " on=" + String(on_score, 3) +
             " off=" + String(off_score, 3) +
             " energy=" + String(g_last_energy) +
             " peak=" + String(g_last_peak);
    const bool infer_ok = true;
    if (wake_score >= kWakeThreshold) g_wake_detected = true;
    if (!infer_ok) return false;
    if (g_wake_detected) return true;
    detail = "No wake detected, " + detail;
    return false;
#else
    detail = "Wake model disabled";
    return false;
#endif
}

bool voice_start_wake_listener() {
#ifdef HAS_EI_VOICE_MODEL
    if (!g_initialized && !voice_init()) return false;
    if (g_wake_task_running) return true;
    g_wake_detected = false;
    g_listener_enabled = true;
    BaseType_t ok = xTaskCreatePinnedToCore(wake_listener_task, "wake_listen", 1024 * 16, NULL, 2, &g_wake_task, 0);
    return ok == pdPASS;
#else
    return false;
#endif
}

void voice_stop_wake_listener() {
    g_listener_enabled = false;
}

bool voice_wait_listener_stopped(uint32_t timeout_ms) {
#ifdef HAS_EI_VOICE_MODEL
    const uint32_t start = millis();
    while (g_wake_task_running && (millis() - start < timeout_ms)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return !g_wake_task_running;
#else
    return true;
#endif
}

bool voice_listener_running() {
#ifdef HAS_EI_VOICE_MODEL
    return g_wake_task_running;
#else
    return false;
#endif
}

void voice_set_listener_enabled(bool enabled) {
    g_listener_enabled = enabled;
}

bool voice_listener_enabled() {
    return g_listener_enabled;
}

void voice_set_gate_threshold(uint16_t threshold) {
#ifdef HAS_EI_VOICE_MODEL
    if (threshold < 200) threshold = 200;
    if (threshold > 12000) threshold = 12000;
    g_gate_threshold = threshold;
#else
    (void)threshold;
#endif
}

uint16_t voice_gate_threshold() {
#ifdef HAS_EI_VOICE_MODEL
    return g_gate_threshold;
#else
    return 0;
#endif
}

uint32_t voice_last_energy_level() {
#ifdef HAS_EI_VOICE_MODEL
    return g_last_energy;
#else
    return 0;
#endif
}

uint32_t voice_last_peak_level() {
#ifdef HAS_EI_VOICE_MODEL
    return g_last_peak;
#else
    return 0;
#endif
}
