#include "speaker_driver.h"

#include <ESP_I2S.h>

static I2SClass i2sOut;
static bool is_initialized = false;
static bool has_default_config = false;
static int g_bclk = 6, g_lrc = 5, g_dout = 1;
static uint32_t g_def_rate = 16000;
static uint32_t g_cur_rate = 16000;

bool speaker_init(uint8_t bclk_pin, uint8_t lrc_pin, uint8_t dout_pin,
                  uint32_t sample_rate) {
    if (is_initialized) return true;

    g_bclk = bclk_pin;
    g_lrc = lrc_pin;
    g_dout = dout_pin;
    g_def_rate = sample_rate;
    g_cur_rate = sample_rate;

    i2sOut.setPins(bclk_pin, lrc_pin, dout_pin);
    if (!i2sOut.begin(I2S_MODE_STD, sample_rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
        Serial.println("Speaker I2S init failed");
        return false;
    }

    has_default_config = true;
    is_initialized = true;
    return true;
}

bool speaker_set_sample_rate(uint32_t sample_rate) {
    if (!is_initialized) return false;

    i2sOut.end();
    is_initialized = false;

    i2sOut.setPins(g_bclk, g_lrc, g_dout);
    if (!i2sOut.begin(I2S_MODE_STD, sample_rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
        return false;
    }

    is_initialized = true;
    g_cur_rate = sample_rate;
    return true;
}

bool speaker_restore_default() {
    if (!has_default_config) return false;
    return speaker_set_sample_rate(g_def_rate);
}

size_t speaker_play(const int16_t* data, size_t samples, bool blocking) {
    if (!is_initialized || !data || samples == 0) return 0;

    size_t bytes = samples * sizeof(int16_t);
    size_t total_written = 0;
    const size_t chunk_samples = 2048;  // 每块 ~32ms @16kHz，定期 yield 防看门狗

    i2sOut.setTimeout(blocking ? 500 : 0);

    while (total_written < bytes) {
        size_t chunk_bytes = bytes - total_written;
        if (chunk_bytes > chunk_samples * sizeof(int16_t)) {
            chunk_bytes = chunk_samples * sizeof(int16_t);
        }
        size_t w = i2sOut.write((uint8_t*)data + total_written, chunk_bytes);
        if (w == 0) break;
        total_written += w;
        if (blocking) delay(1);  // 喂看门狗
    }

    return total_written / sizeof(int16_t);
}

bool speaker_play_tone(int freq, int duration_ms, int amplitude, uint32_t sample_rate) {
    if (freq <= 0 || duration_ms <= 0) return false;

    bool need_restore = false;
    if (!is_initialized) {
        if (!speaker_init()) return false;
    }

    uint32_t cur_rate = g_cur_rate;
    if (sample_rate != cur_rate) {
        if (!speaker_set_sample_rate(sample_rate)) return false;
        need_restore = true;
    }

    int total_frames = (sample_rate * duration_ms) / 1000;
    if (total_frames <= 0) {
        if (need_restore) speaker_restore_default();
        return false;
    }

    int16_t* buf = (int16_t*)malloc(total_frames * 2 * sizeof(int16_t));
    if (!buf) {
        if (need_restore) speaker_restore_default();
        return false;
    }

    float phase = 0.0f;
    float delta = 2.0f * PI * freq / sample_rate;
    for (int i = 0; i < total_frames; i++) {
        int16_t val = (int16_t)(amplitude * sin(phase));
        buf[2 * i] = val;
        buf[2 * i + 1] = val;
        phase += delta;
        if (phase >= 2.0f * PI) phase -= 2.0f * PI;
    }

    size_t written = speaker_play(buf, total_frames * 2, true);
    free(buf);

    if (need_restore) speaker_restore_default();
    return written > 0;
}

void speaker_idle() {
}

void speaker_deinit() {
    if (is_initialized) {
        i2sOut.end();
        is_initialized = false;
    }
}
