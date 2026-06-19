#include "mic_driver.h"

#include <string.h>
#include <ESP_I2S.h>

#define PDM_CLK 42
#define PDM_DIN 41
#define SAMPLE_RATE 16000

static I2SClass i2sIn;
static bool is_initialized = false;

bool mic_init() {
    if (is_initialized) return true;

    i2sIn.setPinsPdmRx(PDM_CLK, PDM_DIN);
    if (!i2sIn.begin(I2S_MODE_PDM_RX, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        Serial.println("Mic PDM RX init failed");
        return false;
    }

    is_initialized = true;
    return true;
}

size_t mic_record(uint8_t *buffer, size_t max_bytes, bool (*stop_condition)(void)) {
    if (!is_initialized || !buffer || max_bytes == 0) return 0;

    i2sIn.setTimeout(50);

    size_t total = 0;
    const size_t chunk = 512;

    while (total < max_bytes) {
        if (stop_condition && stop_condition()) break;

        size_t to_read = (max_bytes - total > chunk) ? chunk : (max_bytes - total);
        size_t bytes_read = i2sIn.readBytes((char *)(buffer + total), to_read);
        if (bytes_read == 0) { delay(1); continue; }
        total += bytes_read;
    }

    return total;
}

bool mic_run_diag(MicDiagResult* result, uint32_t attempts, size_t chunk_bytes) {
    if (!is_initialized || !result || attempts == 0 || chunk_bytes < sizeof(int16_t)) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->last_err = ESP_OK;
    result->min_sample = 32767;
    result->max_sample = -32768;

    uint8_t* buf = (uint8_t*)malloc(chunk_bytes);
    if (!buf) {
        result->last_err = ESP_ERR_NO_MEM;
        return true;
    }

    i2sIn.setTimeout(50);

    for (uint32_t i = 0; i < attempts; i++) {
        i2sIn.flush();
        size_t bytes_read = i2sIn.readBytes((char *)buf, chunk_bytes);
        result->reads_attempted++;

        if (bytes_read == 0) continue;

        result->reads_with_data++;
        result->total_bytes += bytes_read;

        int16_t* samples = (int16_t*)buf;
        size_t sample_count = bytes_read / sizeof(int16_t);
        for (size_t s = 0; s < sample_count; s++) {
            int16_t v = samples[s];
            if (v < result->min_sample) result->min_sample = v;
            if (v > result->max_sample) result->max_sample = v;
            if (v != 0) result->nonzero_samples++;
        }
    }

    free(buf);

    if (result->reads_with_data == 0) {
        result->min_sample = 0;
        result->max_sample = 0;
    }

    return true;
}

int16_t mic_read_sample() {
    int16_t sample = 0;
    i2sIn.readBytes((char *)&sample, sizeof(sample));
    return sample;
}
