#ifndef VISION_DRIVER_H
#define VISION_DRIVER_H

#include <Arduino.h>
#include "screen_driver.h"  // FaceIndex

// ── Edge Impulse 模型开关 ──
// 在 Edge Impulse 训练好人体检测 FOMO 模型并导出 Arduino 库后，
// 取消下面这行注释，并安装对应的库:
// #define HAS_EI_PERSON_DETECTION

// ── 检测参数 ──
#define MOTION_SAMPLE_STEP   10     // 每 N 像素采样 1 个（下采样步长）
#define MOTION_THRESHOLD     0.15f  // 运动像素比例 > 此值 → 运动事件
#define PERSON_FALLBACK_THRESHOLD 0.30f  // 无 EI 模型时，> 此值视为"有人"
#define VISION_INTERVAL_MS   500    // 检测间隔（毫秒）
#define PERSON_LEAVE_DELAY   10000  // 人离开后延时关灯（毫秒）

// ── 视觉事件 ──
enum VisionEvent : uint8_t {
    VISION_NONE          = 0,
    VISION_PERSON_ENTER  = 1,  // 人进入
    VISION_PERSON_LEAVE  = 2,  // 人离开
    VISION_MOTION        = 3,  // 检测到运动
};

// ── 初始化 ──
bool vision_init();

// ── 主循环调用（非阻塞，按 VISION_INTERVAL_MS 间隔自动限制）──
void vision_loop();

// ── 状态查询 ──
bool vision_person_present();
bool vision_motion_detected();
VisionEvent vision_last_event();

// ── 表情建议 ──
FaceIndex vision_suggested_face();

// ── 调试 ──
float vision_motion_ratio();   // 最近一次的运动比例 (0.0~1.0)
int   vision_frame_age_ms();   // 距上次检测的毫秒数

#endif
