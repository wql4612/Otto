#ifndef PLAYER_DRIVER_H
#define PLAYER_DRIVER_H

#include <Arduino.h>

bool play_wav(const char* path);
bool play_wav_gain(const char* path, float gain, bool verbose = false);
bool inspect_wav(const char* path);
bool play_wav_littlefs(const char* path);
bool play_wav_littlefs_gain(const char* path, float gain, bool verbose = false);
bool inspect_wav_littlefs(const char* path);

#endif
