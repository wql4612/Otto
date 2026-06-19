#ifndef SPEAKER_DRIVER_H
#define SPEAKER_DRIVER_H

#include <Arduino.h>

bool speaker_init(uint8_t bclk_pin = 6, uint8_t lrc_pin = 5, uint8_t dout_pin = 1,
                  uint32_t sample_rate = 16000);
bool speaker_set_sample_rate(uint32_t sample_rate);
bool speaker_restore_default();
size_t speaker_play(const int16_t* data, size_t samples, bool blocking = true);
bool speaker_play_tone(int freq, int duration_ms, int amplitude = 16000,
                       uint32_t sample_rate = 16000);
void speaker_idle();
void speaker_deinit();

#endif
