#ifndef MIC_DRIVER_H
#define MIC_DRIVER_H

#include <Arduino.h>

/**
 * @brief 初始化麦克风（PDM 模式，16kHz，16bit，单声道）
 * @return true 成功，false 失败
 */
bool mic_init();

struct MicDiagResult {
    esp_err_t last_err;
    size_t total_bytes;
    size_t nonzero_samples;
    int16_t min_sample;
    int16_t max_sample;
    uint32_t reads_attempted;
    uint32_t reads_with_data;
};

/**
 * @brief 录音函数，将音频数据连续写入缓冲区
 * @param buffer          用户提供的缓冲区指针（至少能容纳 max_bytes 字节）
 * @param max_bytes       缓冲区最大容量（字节数）
 * @param stop_condition  停止条件回调函数，当该函数返回 true 时录音停止（可以为 NULL）
 * @return 实际写入的字节数
 * @note 录音格式：16-bit PCM，单声道，采样率 16 kHz
 * @note 该函数会阻塞，直到满足停止条件或缓冲区写满
 */
size_t mic_record(uint8_t *buffer, size_t max_bytes, bool (*stop_condition)(void));

/**
 * @brief 读取若干个小块并统计底层采样情况，便于诊断麦克风是否真的出数
 * @param result 诊断结果输出
 * @param attempts 尝试读取次数
 * @param chunk_bytes 每次读取字节数
 * @return true 调用完成，false 参数非法或未初始化
 */
bool mic_run_diag(MicDiagResult* result, uint32_t attempts = 12, size_t chunk_bytes = 256);

/**
 * @brief 读取一个 16 位 PCM 采样值（阻塞）
 * @return 当前采样的有符号 16 位整数值
 * @note 内联函数，直接调用底层 I2S 读取，适合需要实时数据的场景
 */
int16_t mic_read_sample();

#endif
