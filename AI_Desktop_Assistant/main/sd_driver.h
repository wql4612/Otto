#ifndef SD_DRIVER_H
#define SD_DRIVER_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>

/**
 * @brief 初始化 SD 卡
 * @param cs_pin 片选引脚（XIAO ESP32S3 Sense 通常为 21）
 * @return true 成功，false 失败
 */
bool sd_init(uint8_t cs_pin = 21);

/**
 * @brief 检查文件是否存在
 * @param path 文件路径（如 "/rec001.wav"）
 * @return true 存在，false 不存在
 */
bool sd_file_exists(const char* path);

/**
 * @brief 生成唯一文件名（基于前缀+递增序号）
 * @param prefix 文件名前缀（如 "rec"）
 * @param ext    文件扩展名（如 "wav"）
 * @param buffer 用于存储文件名的缓冲区（需至少 32 字节）
 * @param buf_len 缓冲区长度
 * @return 生成的完整文件名（如 "/rec001.wav"）
 */
char* sd_generate_filename(const char* prefix, const char* ext, char* buffer, size_t buf_len);

/**
 * @brief 打开文件（支持多种模式）
 * @param path 文件路径
 * @param mode 打开模式（FILE_READ / FILE_WRITE / FILE_APPEND）
 * @return File 对象，若打开失败则返回无效 File（可用 !file 判断）
 */
File sd_open_file(const char* path, const char* mode);

/**
 * @brief 向已打开的文件写入数据
 * @param file  文件对象（引用）
 * @param data  数据指针
 * @param len   数据长度
 * @return 实际写入的字节数
 */
size_t sd_write_file(File &file, const uint8_t* data, size_t len);

/**
 * @brief 从已打开的文件读取数据
 * @param file   文件对象（引用）
 * @param buffer 接收数据的缓冲区
 * @param len    要读取的字节数
 * @return 实际读取的字节数
 */
size_t sd_read_file(File &file, uint8_t* buffer, size_t len);

/**
 * @brief 关闭文件
 * @param file 文件对象（引用）
 */
void sd_close_file(File &file);

#endif