#ifndef CAMERA_DRIVER_H
#define CAMERA_DRIVER_H

#include <Arduino.h>
#include "esp_camera.h"

/**
 * @brief 初始化摄像头（XIAO ESP32S3 Sense 专用引脚配置）
 * @return true 成功，false 失败
 */
bool camera_init();

/**
 * @brief 捕获一帧 JPEG 图像
 * @return camera_fb_t 指针，包含图像数据和长度；若失败返回 NULL
 * @note 调用者必须使用 camera_return_fb() 释放缓冲区
 */
camera_fb_t* camera_capture();

/**
 * @brief 释放帧缓冲区
 * @param fb 由 camera_capture() 返回的帧指针
 */
void camera_return_fb(camera_fb_t *fb);

/**
 * @brief 反初始化摄像头（释放资源）
 */
void camera_deinit();

#endif