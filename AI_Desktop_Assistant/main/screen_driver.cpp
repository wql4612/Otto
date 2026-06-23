#include "screen_driver.h"
#include "debug_log.h"
#include <SPI.h>
#include <JPEGDEC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ── 表情文件路径映射 ──
const char* const FACE_FILES[] PROGMEM = {
    "/faces/face_happy.jpg",
    "/faces/face_idle.jpg",
    "/faces/face_listening.jpg",
    "/faces/face_surprised.jpg",
    "/faces/face_confused.jpg",
};

namespace {

constexpr uint16_t PHYS_W = 240;
constexpr uint16_t PHYS_H = 284;
constexpr uint16_t ROW_OFFSET = 36;
constexpr uint16_t GRAM_W = 240;
constexpr uint16_t GRAM_H = 320;
constexpr bool LCD_INVERT_COLORS = true;

Adafruit_ST7789* tft = nullptr;
bool is_ready = false;
SemaphoreHandle_t screen_mutex = nullptr;

int16_t phys_y(int16_t y) {
    return y + ROW_OFFSET;
}

bool lock_screen() {
    if (!screen_mutex) return true;
    return xSemaphoreTake(screen_mutex, portMAX_DELAY) == pdTRUE;
}

void unlock_screen() {
    if (screen_mutex) xSemaphoreGive(screen_mutex);
}

void fill_screen_unlocked(uint16_t color) {
    if (is_ready) tft->fillScreen(color);
}

void draw_line(const char* text, int16_t x, int16_t y, uint16_t color, uint8_t size) {
    if (!is_ready || !text) return;
    tft->setCursor(x, phys_y(y));
    tft->setTextColor(color);
    tft->setTextSize(size);
    tft->print(text);
}

// ── JPEG 解码回调（逐行写入 TFT） ──
static JPEGDEC g_jpeg;
static int g_jpeg_x_offset = 0;
static int g_jpeg_y_offset = 0;
static int g_jpeg_draw_w = 0;
static int g_jpeg_draw_h = 0;

int jpeg_draw_cb(JPEGDRAW* pDraw) {
    // JPEGDEC uses iWidth for the MCU buffer width and iWidthUsed for the
    // actual visible width on the rightmost edge block.
    int draw_w = pDraw->iWidthUsed > 0 ? pDraw->iWidthUsed : pDraw->iWidth;
    int draw_h = pDraw->iHeight;
    if (pDraw->x + draw_w > g_jpeg_draw_w) {
        draw_w = g_jpeg_draw_w - pDraw->x;
    }
    if (pDraw->y + draw_h > g_jpeg_draw_h) {
        draw_h = g_jpeg_draw_h - pDraw->y;
    }
    if (draw_w <= 0 || draw_h <= 0) {
        return 1;
    }

    for (int row = 0; row < draw_h; ++row) {
        const uint16_t* row_pixels = pDraw->pPixels + (row * pDraw->iWidth);
        tft->startWrite();
        tft->setAddrWindow(g_jpeg_x_offset + pDraw->x,
                           phys_y(g_jpeg_y_offset + pDraw->y + row),
                           draw_w, 1);
        tft->writePixels(const_cast<uint16_t*>(row_pixels), draw_w, true, false);
        tft->endWrite();
    }
    return 1;
}

}  // namespace

// ══════════════════════════════════════════════
// 屏幕初始化
// ══════════════════════════════════════════════
bool screen_init(uint8_t cs_pin, uint8_t dc_pin, int8_t rst_pin,
                 uint8_t mosi_pin, uint8_t sck_pin, uint32_t spi_freq) {
    if (is_ready) return true;

    SPI.begin(sck_pin, -1, mosi_pin, cs_pin);
    SPI.setFrequency(spi_freq);

    if (!tft) tft = new Adafruit_ST7789(cs_pin, dc_pin, rst_pin);
    if (!tft) {
        debug_log_append("[Screen] Alloc failed", "system");
        return false;
    }
    if (!screen_mutex) {
        screen_mutex = xSemaphoreCreateMutex();
        if (!screen_mutex) {
            debug_log_append("[Screen] Mutex alloc failed", "system");
            return false;
        }
    }

    tft->init(GRAM_W, GRAM_H);
    tft->setSPISpeed(spi_freq);
    tft->setRotation(0);
    tft->invertDisplay(LCD_INVERT_COLORS);
    tft->fillScreen(ST77XX_BLACK);

    is_ready = true;
    debug_log_append("[Screen] Ready", "system");
    return true;
}

bool screen_is_ready() { return is_ready; }

void screen_clear(uint16_t color) {
    if (!is_ready) return;
    if (!lock_screen()) return;
    fill_screen_unlocked(color);
    unlock_screen();
}

// ══════════════════════════════════════════════
// 系统界面
// ══════════════════════════════════════════════
void screen_show_boot() {
    if (!is_ready) return;
    if (!lock_screen()) return;
    fill_screen_unlocked(ST77XX_BLACK);
    draw_line("AI Desktop", 18, 36, ST77XX_WHITE, 3);
    draw_line("Assistant", 28, 74, ST77XX_CYAN, 3);
    draw_line("Mic / SD / Cam", 28, 132, ST77XX_YELLOW, 2);
    draw_line("Speaker / Screen", 18, 166, ST77XX_GREEN, 2);
    draw_line("Booting...", 18, 220, ST77XX_WHITE, 1);
    unlock_screen();
}

void screen_show_test_pattern() {
    if (!is_ready) return;
    if (!lock_screen()) return;
    tft->fillRect(0, phys_y(0),   240, 47, ST77XX_RED);
    tft->fillRect(0, phys_y(47),  240, 47, ST77XX_GREEN);
    tft->fillRect(0, phys_y(94),  240, 47, ST77XX_BLUE);
    tft->fillRect(0, phys_y(141), 240, 47, ST77XX_YELLOW);
    tft->fillRect(0, phys_y(188), 240, 47, ST77XX_MAGENTA);
    tft->fillRect(0, phys_y(235), 240, 49, ST77XX_CYAN);
    unlock_screen();
}

void screen_show_message(const char* line1, const char* line2, const char* line3) {
    if (!is_ready) return;
    if (!lock_screen()) return;
    fill_screen_unlocked(ST77XX_BLACK);
    if (line1) draw_line(line1, 18, 44, ST77XX_WHITE, 3);
    if (line2) draw_line(line2, 18, 104, ST77XX_YELLOW, 2);
    if (line3) draw_line(line3, 18, 146, ST77XX_CYAN, 2);
    draw_line("240x284 ST7789", 18, 220, ST77XX_GREEN, 1);
    unlock_screen();
}

void screen_show_init_status(const char* title, const ScreenStatusItem* items, size_t count,
                             const char* ip) {
    if (!is_ready) return;
    if (!lock_screen()) return;

    fill_screen_unlocked(ST77XX_BLACK);
    draw_line(title ? title : "System Boot", 14, 18, ST77XX_WHITE, 2);
    draw_line("Init Status", 14, 42, ST77XX_CYAN, 2);

    const int col_x[2] = { 14, 126 };
    const int start_y = 78;
    const int row_h = 28;

    for (size_t i = 0; i < count; ++i) {
        int col = i % 2;
        int row = i / 2;
        int y = start_y + row * row_h;
        if (y > 210) break;

        uint16_t color = ST77XX_YELLOW;
        const char* state = "...";
        if (items[i].known) {
            color = items[i].ok ? ST77XX_GREEN : ST77XX_RED;
            state = items[i].ok ? "OK" : "FAIL";
        }

        if (items[i].label) draw_line(items[i].label, col_x[col], y, ST77XX_WHITE, 1);
        draw_line(state, col_x[col] + 62, y, color, 1);
    }

    draw_line("IP:", 14, 226, ST77XX_WHITE, 1);
    if (ip && strlen(ip) > 0) {
        draw_line(ip, 42, 226, ST77XX_GREEN, 1);
    } else {
        draw_line("connecting...", 42, 226, ST77XX_YELLOW, 1);
    }
    unlock_screen();
}

void screen_show_rgb565(const uint16_t* data, int w, int h) {
    if (!is_ready || !data || w <= 0 || h <= 0) return;
    if (!lock_screen()) return;
    fill_screen_unlocked(ST77XX_BLACK);
    int x = (PHYS_W - w) / 2;
    int y = (PHYS_H - h) / 2;
    tft->drawRGBBitmap(x, phys_y(y), data, w, h);
    unlock_screen();
}

// ══════════════════════════════════════════════
// 表情显示（从 LittleFS JPEG 解码）
// ══════════════════════════════════════════════
bool screen_show_face_jpeg(const char* path) {
    if (!is_ready) return false;
    if (!lock_screen()) return false;

    if (!LittleFS.exists(path)) {
        debug_log_printf("system", "[Face] File not found: %s", path);
        unlock_screen();
        return false;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        debug_log_printf("system", "[Face] Open failed: %s", path);
        unlock_screen();
        return false;
    }

    if (f.size() == 0) {
        debug_log_printf("system", "[Face] Empty file: %s", path);
        f.close();
        unlock_screen();
        return false;
    }

    fill_screen_unlocked(ST77XX_BLACK);
    g_jpeg_x_offset = 0;
    g_jpeg_y_offset = 0;

    bool ok = false;
    if (g_jpeg.open(f, jpeg_draw_cb)) {
        g_jpeg_draw_w = g_jpeg.getWidth();
        g_jpeg_draw_h = g_jpeg.getHeight();
        g_jpeg_x_offset = (PHYS_W - g_jpeg_draw_w) / 2;
        g_jpeg_y_offset = (PHYS_H - g_jpeg_draw_h) / 2;
        if (g_jpeg_x_offset < 0) g_jpeg_x_offset = 0;
        if (g_jpeg_y_offset < 0) g_jpeg_y_offset = 0;
        // Adafruit_GFX::drawRGBBitmap()/writePixels() consume native 16-bit
        // RGB565 values, so the JPEG decoder should output little-endian
        // pixels in memory for this ESP32 + Adafruit_ST7789 path.
        g_jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        ok = g_jpeg.decode(0, 0, 0);
        g_jpeg.close();
    } else {
        debug_log_printf("system", "[Face] JPEG open failed: %s", path);
        f.close();
    }

    if (!ok) {
        debug_log_printf("system", "[Face] JPEG decode failed: %s", path);
    }

    unlock_screen();
    return ok;
}

void screen_enter_sleep() {
    if (!is_ready || !tft) return;
    if (!lock_screen()) return;
    fill_screen_unlocked(ST77XX_BLACK);
    tft->enableDisplay(false);
    tft->enableSleep(true);
    unlock_screen();
}
