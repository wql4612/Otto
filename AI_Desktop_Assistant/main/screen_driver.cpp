#include "screen_driver.h"
#include <SPI.h>
#include <JPEGDEC.h>

// ── 表情文件路径映射 ──
const char* const FACE_FILES[] PROGMEM = {
    "/faces/face_happy.jpg",
    "/faces/face_idle.jpg",
    "/faces/face_listening.jpg",
    "/faces/face_surprised.jpg",
    "/faces/face_sleep.jpg",
    "/faces/face_confused.jpg",
    "/faces/face_cute.jpg",
};

namespace {

constexpr uint16_t PHYS_W = 240;
constexpr uint16_t PHYS_H = 284;
constexpr uint16_t ROW_OFFSET = 36;
constexpr uint16_t GRAM_W = 240;
constexpr uint16_t GRAM_H = 320;

Adafruit_ST7789* tft = nullptr;
bool is_ready = false;

int16_t phys_y(int16_t y) {
    return y + ROW_OFFSET;
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
static int g_jpeg_y_offset = 0;

int jpeg_draw_cb(JPEGDRAW* pDraw) {
    tft->drawRGBBitmap(pDraw->x, phys_y(g_jpeg_y_offset + pDraw->y),
                       pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
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
        Serial.println("[Screen] Alloc failed");
        return false;
    }

    tft->init(GRAM_W, GRAM_H);
    tft->setSPISpeed(spi_freq);
    tft->setRotation(0);
    tft->fillScreen(ST77XX_BLACK);

    is_ready = true;
    Serial.println("[Screen] Ready");
    return true;
}

bool screen_is_ready() { return is_ready; }

void screen_clear(uint16_t color) {
    if (!is_ready) return;
    tft->fillScreen(color);
}

// ══════════════════════════════════════════════
// 系统界面
// ══════════════════════════════════════════════
void screen_show_boot() {
    if (!is_ready) return;
    screen_clear(ST77XX_BLACK);
    draw_line("AI Desktop", 18, 36, ST77XX_WHITE, 3);
    draw_line("Assistant", 28, 74, ST77XX_CYAN, 3);
    draw_line("Mic / SD / Cam", 28, 132, ST77XX_YELLOW, 2);
    draw_line("Speaker / Screen", 18, 166, ST77XX_GREEN, 2);
    draw_line("Booting...", 18, 220, ST77XX_WHITE, 1);
}

void screen_show_test_pattern() {
    if (!is_ready) return;
    tft->fillRect(0, phys_y(0),   240, 47, ST77XX_RED);
    tft->fillRect(0, phys_y(47),  240, 47, ST77XX_GREEN);
    tft->fillRect(0, phys_y(94),  240, 47, ST77XX_BLUE);
    tft->fillRect(0, phys_y(141), 240, 47, ST77XX_YELLOW);
    tft->fillRect(0, phys_y(188), 240, 47, ST77XX_MAGENTA);
    tft->fillRect(0, phys_y(235), 240, 49, ST77XX_CYAN);
}

void screen_show_message(const char* line1, const char* line2, const char* line3) {
    if (!is_ready) return;
    screen_clear(ST77XX_BLACK);
    if (line1) draw_line(line1, 18, 44, ST77XX_WHITE, 3);
    if (line2) draw_line(line2, 18, 104, ST77XX_YELLOW, 2);
    if (line3) draw_line(line3, 18, 146, ST77XX_CYAN, 2);
    draw_line("240x284 ST7789", 18, 220, ST77XX_GREEN, 1);
}

void screen_show_rgb565(const uint16_t* data, int w, int h) {
    if (!is_ready || !data || w <= 0 || h <= 0) return;
    screen_clear(ST77XX_BLACK);
    int x = (PHYS_W - w) / 2;
    int y = (PHYS_H - h) / 2;
    tft->drawRGBBitmap(x, phys_y(y), data, w, h);
}

// ══════════════════════════════════════════════
// 表情显示（从 LittleFS JPEG 解码）
// ══════════════════════════════════════════════
bool screen_show_face_jpeg(const char* path) {
    if (!is_ready) return false;

    if (!LittleFS.exists(path)) {
        Serial.printf("[Face] File not found: %s\n", path);
        return false;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("[Face] Open failed: %s\n", path);
        return false;
    }

    size_t fsize = f.size();
    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { f.close(); return false; }

    f.read(buf, fsize);
    f.close();

    screen_clear(ST77XX_BLACK);
    g_jpeg_y_offset = 0;

    if (g_jpeg.openRAM(buf, fsize, jpeg_draw_cb)) {
        g_jpeg.setPixelType(RGB565_BIG_ENDIAN);
        g_jpeg.decode(0, 0, 0);
        g_jpeg.close();
    }

    free(buf);
    return true;
}
