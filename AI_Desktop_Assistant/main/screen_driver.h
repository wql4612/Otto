#ifndef SCREEN_DRIVER_H
#define SCREEN_DRIVER_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <LittleFS.h>

// ── 屏幕初始化 ──
bool screen_init(uint8_t cs_pin = 2, uint8_t dc_pin = 3, int8_t rst_pin = -1,
                 uint8_t mosi_pin = 9, uint8_t sck_pin = 7, uint32_t spi_freq = 40000000);
bool screen_is_ready();
void screen_clear(uint16_t color = ST77XX_BLACK);

struct ScreenStatusItem {
    const char* label;
    bool known;
    bool ok;
};

// ── 显示界面 ──
void screen_show_boot();
void screen_show_test_pattern();
void screen_show_message(const char* line1, const char* line2 = nullptr,
                         const char* line3 = nullptr);
void screen_show_init_status(const char* title, const ScreenStatusItem* items, size_t count,
                             const char* ip = nullptr);
void screen_show_rgb565(const uint16_t* data, int w, int h);

// ── 表情显示（从 LittleFS JPEG 文件） ──
bool screen_show_face_jpeg(const char* path);
void screen_enter_sleep();

// ── 表情索引 ──
enum FaceIndex : uint8_t {
    FACE_HAPPY     = 0,
    FACE_IDLE      = 1,
    FACE_LISTENING = 2,
    FACE_SURPRISED = 3,
    FACE_CONFUSED  = 4,
    FACE_SLEEP     = FACE_CONFUSED,
    FACE_CUTE      = FACE_HAPPY,
    FACE_COUNT     = 5
};

// 表情文件名映射（存在 PROGMEM）
extern const char* const FACE_FILES[];

#endif
