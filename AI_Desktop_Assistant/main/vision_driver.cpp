/**
 * vision_driver — 人体检测 + 运动检测
 *
 * 运动检测 (B3, 完整实现):
 *   基于 JPEG 字节流采样的帧差法 — 捕获 JPEG 帧，均匀采样 N 个字节，
 *   对比上一帧的采样值，变化比例 > 阈值即触发运动事件。
 *   无需 JPEG 解码，速度快（<5ms），不干扰 MJPEG 流。
 *
 * 人体检测 (B2, Edge Impulse 占位):
 *   HAS_EI_PERSON_DETECTION 未定义时，用帧差法比值 > 30% 作为"有人"代理判断。
 *   Edge Impulse FOMO 模型训练完成后，取消宏注释即可切换到真实 AI 推理。
 */

#include "vision_driver.h"
#include "camera_driver.h"

// ── JPEG 采样参数 ──
#define SAMPLE_COUNT        200     // 每帧采样字节数
#define SAMPLE_DIFF_MIN     25      // 单字节差异 > 此值视为"变化"
#define MOTION_SAMPLE_RATIO 0.15f   // 变化采样比例 > 此值 → 运动事件

namespace {

// 帧采样缓冲
static uint8_t g_prev_samples[SAMPLE_COUNT];
static uint8_t g_curr_samples[SAMPLE_COUNT];
static bool    g_has_prev_frame = false;

// 检测状态
static bool    g_person_present   = false;
static bool    g_motion_detected  = false;
static float   g_motion_ratio     = 0.0f;
static VisionEvent g_last_event   = VISION_NONE;
static unsigned long g_last_detect_ms = 0;
static unsigned long g_last_vision_ms = 0;
static unsigned long g_person_last_seen = 0;

static FaceIndex g_suggested_face = FACE_IDLE;

// ── 从 JPEG 数据中均匀采样 ──
void sample_jpeg(const uint8_t* buf, size_t len, uint8_t* samples) {
    if (len == 0) {
        memset(samples, 0, SAMPLE_COUNT);
        return;
    }
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        size_t idx = (len - 1) * i / SAMPLE_COUNT;
        if (idx >= len) idx = len - 1;
        samples[i] = buf[idx];
    }
}

// ── 对比两帧采样 ──
float compare_samples(const uint8_t* a, const uint8_t* b) {
    int changed = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        if (abs((int)a[i] - (int)b[i]) > SAMPLE_DIFF_MIN) {
            changed++;
        }
    }
    return (float)changed / SAMPLE_COUNT;
}

}  // namespace

// ═══════════════════════════════════════
// 公共 API
// ═══════════════════════════════════════

bool vision_init() {
    // 摄像头由 camera_driver 管理，此处无需重复初始化
    // 分配和清零采样缓冲
    memset(g_prev_samples, 0, SAMPLE_COUNT);
    memset(g_curr_samples, 0, SAMPLE_COUNT);
    g_has_prev_frame = false;
    g_person_present = false;
    g_motion_detected = false;
    g_last_event = VISION_NONE;
    g_suggested_face = FACE_IDLE;
    g_person_last_seen = 0;

    Serial.println("[Vision] Ready (JPEG sampling + frame diff)");
    return true;
}

void vision_loop() {
    // 限速检测
    if (millis() - g_last_vision_ms < VISION_INTERVAL_MS) return;
    g_last_vision_ms = millis();

    // 捕获一帧
    camera_fb_t* fb = camera_capture();
    if (!fb) return;

    // 采样当前帧
    sample_jpeg(fb->buf, fb->len, g_curr_samples);

    float ratio = 0.0f;

#ifdef HAS_EI_PERSON_DETECTION
    // ── Edge Impulse FOMO 人体检测 ──
    // TODO: 集成 Edge Impulse 导出的 Arduino 库
    // 步骤:
    //   1. 在 edgeimpulse.com 训练 FOMO 人体检测模型
    //   2. 导出为 Arduino 库并安装
    //   3. #include <your-project_inferencing.h>
    //   4. 将 fb->buf 转为 EI 需要的格式 (RGB888)
    //   5. 调用 run_classifier() 获取检测结果
    //   6. 根据结果更新 person_present
    //
    // 示例代码:
    //   signal_t signal;
    //   signal.total_length = fb->len;
    //   signal.get_data = &raw_data_callback;
    //   ei_impulse_result_t result;
    //   run_classifier(&signal, &result, false);
    //   person_detected = (result.classification[0].value > 0.7f);
    //
    // 在此期间用帧差法代理:
    ratio = compare_samples(g_prev_samples, g_curr_samples);
    g_motion_ratio = ratio;
    g_motion_detected = (ratio > MOTION_SAMPLE_RATIO);
    bool person_fallback = (ratio > PERSON_FALLBACK_THRESHOLD);
#else
    // 帧差法
    if (g_has_prev_frame) {
        ratio = compare_samples(g_prev_samples, g_curr_samples);
    }
    g_motion_ratio = ratio;
    g_motion_detected = (ratio > MOTION_SAMPLE_RATIO);
    bool person_fallback = (ratio > PERSON_FALLBACK_THRESHOLD);
#endif

    // 更新人体状态
    unsigned long now = millis();

    if (person_fallback) {
        g_person_last_seen = now;
        if (!g_person_present) {
            g_person_present = true;
            g_last_event = VISION_PERSON_ENTER;
            g_suggested_face = FACE_HAPPY;
            Serial.printf("[Vision] Person ENTER (ratio=%.3f)\n", ratio);
        } else {
            g_last_event = VISION_NONE;
        }
    } else if (g_person_present && (now - g_person_last_seen > PERSON_LEAVE_DELAY)) {
        g_person_present = false;
        g_last_event = VISION_PERSON_LEAVE;
        g_suggested_face = FACE_SLEEP;
        Serial.printf("[Vision] Person LEAVE (ratio=%.3f)\n", ratio);
    } else if (g_motion_detected && g_person_present) {
        g_last_event = VISION_MOTION;
        g_suggested_face = FACE_SURPRISED;
    } else {
        g_last_event = VISION_NONE;
        if (g_person_present) {
            g_suggested_face = FACE_HAPPY;
        } else {
            g_suggested_face = FACE_IDLE;
        }
    }

    // 保存当前采样为下一帧的参考
    memcpy(g_prev_samples, g_curr_samples, SAMPLE_COUNT);
    g_has_prev_frame = true;

    // 释放帧
    camera_return_fb(fb);
    g_last_detect_ms = now;
}

bool vision_person_present()  { return g_person_present; }
bool vision_motion_detected() { return g_motion_detected; }
VisionEvent vision_last_event() { return g_last_event; }
FaceIndex vision_suggested_face() { return g_suggested_face; }
float vision_motion_ratio()   { return g_motion_ratio; }
int   vision_frame_age_ms()   { return millis() - g_last_detect_ms; }
