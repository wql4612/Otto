#ifndef QWEN_CLIENT_H
#define QWEN_CLIENT_H

#include <Arduino.h>

struct QwenConfig {
    const char* image_api_url;
    const char* voice_api_url;
    const char* api_key;
    const char* image_model;
    const char* image_prompt;
    const char* voice_model;
};

void qwen_set_config(const QwenConfig& config);
bool qwen_is_configured();
String qwen_config_status();
bool qwen_recognize_image(const uint8_t* jpeg_data, size_t jpeg_len, String& result_text);
bool qwen_recognize_voice_command(const int16_t* pcm_data, size_t sample_count, String& command_text);
bool qwen_test_voice_sample(String& command_text);

#endif
