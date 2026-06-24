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
static int16_t* g_infer_buffer_b = nullptr;
static int16_t* g_infer_window_buffer = nullptr;
static int16_t* g_active_infer_buffer = nullptr;
static size_t g_infer_buffer_samples = 0;
static size_t g_chunk_buffer_samples = 0;
static constexpr float kWakeThreshold = EI_CLASSIFIER_THRESHOLD;
static constexpr const char* kWakeLabel = "wake";
static constexpr const char* kOnLabel = "on";
static constexpr const char* kOffLabel = "off";
static TaskHandle_t g_wake_task = nullptr;
static bool g_wake_task_running = false;
static volatile uint32_t g_last_energy = 0;
static volatile uint32_t g_prev_energy = 0;
static constexpr uint32_t kGateChunkMs = 500;
static constexpr uint32_t kWakeInferenceCooldownMs = 80;

static int wake_signal_get_data(size_t offset, size_t length, float *out_ptr) {
    if (!g_active_infer_buffer || offset + length > g_infer_buffer_samples) return -1;
    numpy::int16_to_float(&g_active_infer_buffer[offset], out_ptr, length);
    return 0;
}

static bool is_wake_label(const char* label) {
    return label && strcmp(label, kWakeLabel) == 0;
}

static bool ensure_wake_buffers(size_t infer_samples, size_t chunk_samples, String* err = nullptr) {
    const size_t infer_bytes = infer_samples * sizeof(int16_t);
    const size_t chunk_bytes = chunk_samples * sizeof(int16_t);

    if (!g_infer_window_buffer || g_infer_buffer_samples != infer_samples) {
        if (g_infer_window_buffer) {
            free(g_infer_window_buffer);
            g_infer_window_buffer = nullptr;
        }
        g_infer_window_buffer = (int16_t*)ps_malloc(infer_bytes);
        if (!g_infer_window_buffer) {
            if (err) *err = "Wake window alloc failed";
            return false;
        }
        g_infer_buffer_samples = infer_samples;
    }

    if (!g_infer_buffer_a || !g_infer_buffer_b || g_chunk_buffer_samples != chunk_samples) {
        if (g_infer_buffer_a) {
            free(g_infer_buffer_a);
            g_infer_buffer_a = nullptr;
        }
        if (g_infer_buffer_b) {
            free(g_infer_buffer_b);
            g_infer_buffer_b = nullptr;
        }
        g_infer_buffer_a = (int16_t*)ps_malloc(chunk_bytes);
        g_infer_buffer_b = (int16_t*)ps_malloc(chunk_bytes);
        if (!g_infer_buffer_a || !g_infer_buffer_b) {
            if (g_infer_buffer_a) {
                free(g_infer_buffer_a);
                g_infer_buffer_a = nullptr;
            }
            if (g_infer_buffer_b) {
                free(g_infer_buffer_b);
                g_infer_buffer_b = nullptr;
            }
            if (err) *err = "Wake chunk alloc failed";
            return false;
        }
        g_chunk_buffer_samples = chunk_samples;
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

static bool record_audio_chunk(int16_t* chunk_buffer, size_t chunk_samples,
                               uint32_t& avg_abs, uint32_t& peak_abs,
                               String* detail = nullptr) {
    if (!chunk_buffer) {
        if (detail) *detail = "Wake chunk buffer missing";
        return false;
    }

    const size_t chunk_bytes = chunk_samples * sizeof(int16_t);
    size_t got = mic_record((uint8_t*)chunk_buffer, chunk_bytes, nullptr);
    if (got != chunk_bytes) {
        if (detail) *detail = "Wake chunk capture incomplete";
        return false;
    }

    update_energy_metrics(chunk_buffer, chunk_samples, avg_abs, peak_abs);
    return true;
}

static bool run_wake_inference_window(const int16_t* first_chunk, size_t first_chunk_samples,
                                      const int16_t* second_chunk, size_t second_chunk_samples,
                                      String* detail = nullptr) {
    const size_t infer_samples = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    if (!g_infer_window_buffer || !first_chunk || !second_chunk) {
        if (detail) *detail = "Wake window buffer missing";
        return false;
    }
    if (first_chunk_samples + second_chunk_samples != infer_samples) {
        if (detail) *detail = "Wake chunk/window mismatch";
        return false;
    }

    memcpy(g_infer_window_buffer, first_chunk, first_chunk_samples * sizeof(int16_t));
    memcpy(g_infer_window_buffer + first_chunk_samples, second_chunk,
           second_chunk_samples * sizeof(int16_t));

    uint32_t avg_abs = 0;
    uint32_t peak_abs = 0;
    update_energy_metrics(g_infer_window_buffer, infer_samples, avg_abs, peak_abs);
    g_last_energy = avg_abs;
    g_active_infer_buffer = g_infer_window_buffer;

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
                  " chunk_ms=" + String(kGateChunkMs) +
                  " energy=" + String(avg_abs);
    }

    if (wake_score >= kWakeThreshold) {
        g_wake_detected = true;
    }
    return true;
}

static void wake_listener_task(void* pv) {
    (void)pv;
    g_wake_task_running = true;
    debug_log_printf("system", "[Wake] Listener task started, stride=%lu ms, overlap=%lu ms",
                     (unsigned long)kGateChunkMs, (unsigned long)kGateChunkMs);
    unsigned long last_trigger_ms = 0;
    unsigned long last_heartbeat_ms = 0;
    const size_t infer_samples = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    const size_t chunk_samples = infer_samples / 2;
    bool have_prev_chunk = false;

    String init_detail;
    if ((infer_samples % 2) != 0 || !ensure_wake_buffers(infer_samples, chunk_samples, &init_detail)) {
        if ((infer_samples % 2) != 0) init_detail = "Wake model window must be even for sliding mode";
        debug_log_append("[Wake] Listener init failed: " + init_detail, "system");
        g_wake_task_running = false;
        g_wake_task = nullptr;
        vTaskDelete(NULL);
        return;
    }

    while (g_listener_enabled) {
        if (g_wake_detected) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t avg_abs = 0;
        String chunk_detail;
        uint32_t ignored_peak = 0;
        if (!record_audio_chunk(g_infer_buffer_b, chunk_samples, avg_abs, ignored_peak, &chunk_detail)) {
            debug_log_append("[Wake] Chunk read failed: " + chunk_detail, "system");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        g_last_energy = avg_abs;

        if (millis() - last_heartbeat_ms >= 1500) {
            last_heartbeat_ms = millis();
            debug_log_printf("system",
                             "[Wake] sliding energy=%lu prev=%lu listener=%s",
                             (unsigned long)avg_abs,
                             (unsigned long)g_prev_energy,
                             g_listener_enabled ? "on" : "off");
        }

        if (have_prev_chunk && (millis() - last_trigger_ms >= kWakeInferenceCooldownMs)) {
                last_trigger_ms = millis();

                String infer_detail;
                debug_log_printf("system",
                                 "[Wake] Sliding infer start, prev=%lu curr=%lu",
                                 (unsigned long)g_prev_energy,
                                 (unsigned long)avg_abs);
                if (!run_wake_inference_window(g_infer_buffer_a, chunk_samples,
                                               g_infer_buffer_b, chunk_samples,
                                               &infer_detail)) {
                    debug_log_append("[Wake] Listener inference failed: " + infer_detail, "system");
                    vTaskDelay(pdMS_TO_TICKS(120));
                } else {
                    debug_log_append("[Wake] " + infer_detail, "system");
                    if (g_wake_detected) {
                        debug_log_append("[Wake] Detected in background", "system");
                    }
                }
        }

        memcpy(g_infer_buffer_a, g_infer_buffer_b, chunk_samples * sizeof(int16_t));
        g_prev_energy = avg_abs;
        have_prev_chunk = true;

        vTaskDelay(pdMS_TO_TICKS(10));
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
    g_prev_energy = 0;

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
    const size_t chunk_samples = infer_samples / 2;
    const uint32_t model_window_ms = (uint32_t)((infer_samples * 1000ULL) / EI_CLASSIFIER_FREQUENCY);

    if ((infer_samples % 2) != 0) {
        detail = "Wake model window must be even for sliding mode";
        return false;
    }
    if (!ensure_wake_buffers(infer_samples, chunk_samples, &detail)) return false;

    debug_log_printf("system", "[Wake] Capture start, requested=%lu ms, model=%lu ms, stride=%lu ms, threshold=%.2f",
                     timeout_ms, model_window_ms, (unsigned long)kGateChunkMs, kWakeThreshold);

    uint32_t avg_a = 0;
    uint32_t avg_b = 0;
    uint32_t ignored_peak_a = 0;
    uint32_t ignored_peak_b = 0;
    if (!record_audio_chunk(g_infer_buffer_a, chunk_samples, avg_a, ignored_peak_a, &detail)) {
        return false;
    }
    if (!record_audio_chunk(g_infer_buffer_b, chunk_samples, avg_b, ignored_peak_b, &detail)) {
        return false;
    }

    g_prev_energy = avg_a;
    g_last_energy = avg_b;

    if (!run_wake_inference_window(g_infer_buffer_a, chunk_samples,
                                   g_infer_buffer_b, chunk_samples, &detail)) {
        return false;
    }
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

uint32_t voice_last_energy_level() {
#ifdef HAS_EI_VOICE_MODEL
    return g_last_energy;
#else
    return 0;
#endif
}
