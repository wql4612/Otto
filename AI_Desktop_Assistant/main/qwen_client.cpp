#include "qwen_client.h"

#include "debug_log.h"
#include "wifi_driver.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>

namespace {

const char* QWEN_SAMPLE_AUDIO_URL = "https://dashscope.oss-cn-beijing.aliyuncs.com/audios/welcome.mp3";

QwenConfig g_qwen_config = { "", "", "", "", "", "" };

bool contains_placeholder(const char* text) {
    return text && (strstr(text, "{") != nullptr || strstr(text, "}") != nullptr);
}

String normalized_api_url(const char* raw_url) {
    String url = raw_url ? String(raw_url) : String();
    url.trim();
    if (!url.length()) return url;

    if (url.endsWith("/chat/completions")) return url;
    if (url.endsWith("/")) url.remove(url.length() - 1);
    if (url.endsWith("/compatible-mode/v1")) {
        url += "/chat/completions";
    }
    return url;
}

bool has_basic_config() {
    return g_qwen_config.image_api_url && g_qwen_config.voice_api_url && g_qwen_config.api_key &&
           strlen(g_qwen_config.image_api_url) > 0 &&
           strlen(g_qwen_config.voice_api_url) > 0 &&
           strlen(g_qwen_config.api_key) > 0 &&
           !contains_placeholder(g_qwen_config.image_api_url) &&
           !contains_placeholder(g_qwen_config.voice_api_url);
}

String json_unescape(const String& input) {
    String out;
    out.reserve(input.length());
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == '\\' && i + 1 < input.length()) {
            char n = input[++i];
            switch (n) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                default: out += n; break;
            }
        } else {
            out += c;
        }
    }
    return out;
}

bool extract_json_string_after(const String& input, int quote_pos, String& out) {
    if (quote_pos < 0 || quote_pos >= (int)input.length() || input[quote_pos] != '"') {
        return false;
    }

    String raw;
    bool escaped = false;
    for (int i = quote_pos + 1; i < (int)input.length(); ++i) {
        char c = input[i];
        if (escaped) {
            raw += '\\';
            raw += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            out = json_unescape(raw);
            return true;
        }
        raw += c;
    }
    return false;
}

bool extract_response_text(const String& response, String& out) {
    int content_idx = response.indexOf("\"content\"");
    if (content_idx >= 0) {
        int colon = response.indexOf(':', content_idx);
        if (colon >= 0) {
            int quote = response.indexOf('"', colon + 1);
            if (quote >= 0 && extract_json_string_after(response, quote, out)) {
                return true;
            }
        }
    }

    int text_idx = response.indexOf("\"text\"");
    if (text_idx >= 0) {
        int colon = response.indexOf(':', text_idx);
        if (colon >= 0) {
            int quote = response.indexOf('"', colon + 1);
            if (quote >= 0 && extract_json_string_after(response, quote, out)) {
                return true;
            }
        }
    }

    return false;
}

bool extract_string_field(const String& json, const char* key, String& value) {
    String pattern = String("\"") + key + "\"";
    int key_idx = json.indexOf(pattern);
    if (key_idx < 0) return false;
    int colon = json.indexOf(':', key_idx + pattern.length());
    if (colon < 0) return false;
    int quote = json.indexOf('"', colon + 1);
    if (quote < 0) return false;
    return extract_json_string_after(json, quote, value);
}

bool extract_bool_field(const String& json, const char* key, bool& value) {
    String pattern = String("\"") + key + "\"";
    int key_idx = json.indexOf(pattern);
    if (key_idx < 0) return false;
    int colon = json.indexOf(':', key_idx + pattern.length());
    if (colon < 0) return false;

    int value_start = colon + 1;
    while (value_start < (int)json.length() && isspace((unsigned char)json[value_start])) {
        value_start++;
    }

    if (json.startsWith("true", value_start)) {
        value = true;
        return true;
    }
    if (json.startsWith("false", value_start)) {
        value = false;
        return true;
    }
    return false;
}

bool extract_int_field(const String& json, const char* key, int& value) {
    String pattern = String("\"") + key + "\"";
    int key_idx = json.indexOf(pattern);
    if (key_idx < 0) return false;
    int colon = json.indexOf(':', key_idx + pattern.length());
    if (colon < 0) return false;

    int value_start = colon + 1;
    while (value_start < (int)json.length() && isspace((unsigned char)json[value_start])) {
        value_start++;
    }

    int value_end = value_start;
    if (value_end < (int)json.length() && (json[value_end] == '-' || isdigit((unsigned char)json[value_end]))) {
        value_end++;
        while (value_end < (int)json.length() && isdigit((unsigned char)json[value_end])) value_end++;
        value = json.substring(value_start, value_end).toInt();
        return true;
    }
    return false;
}

String simplify_face_result(const String& raw_text) {
    bool has_face = false;
    int face_count = 0;

    bool has_face_ok = extract_bool_field(raw_text, "has_face", has_face);
    bool face_count_ok = extract_int_field(raw_text, "face_count", face_count);

    if (!has_face_ok && !face_count_ok) {
        return raw_text;
    }

    if (!has_face_ok) has_face = face_count > 0;
    if (!face_count_ok) face_count = has_face ? 1 : 0;
    if (!has_face) face_count = 0;

    return String("has_face=") + (has_face ? "true" : "false") +
           ", face_count=" + String(face_count);
}

String normalize_transcript(const String& input) {
    String out = input;
    out.trim();
    out.replace(" ", "");
    out.replace("\n", "");
    out.replace("\r", "");
    out.replace("\t", "");
    out.replace("。", "");
    out.replace("，", "");
    out.replace(",", "");
    out.replace("！", "");
    out.replace("!", "");
    out.replace("？", "");
    out.replace("?", "");
    return out;
}

bool contains_any(const String& text, const char* const* patterns, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (patterns[i] && text.indexOf(patterns[i]) >= 0) return true;
    }
    return false;
}

String classify_voice_command(const String& transcript) {
    static const char* kLightOnKeywords[] = {
        "开灯", "打开灯", "把灯打开", "灯打开", "亮灯", "开一下灯", "把台灯打开", "台灯打开", "开台灯"
    };
    static const char* kLightOffKeywords[] = {
        "关灯", "关闭灯", "把灯关掉", "灯关掉", "熄灯", "灭灯", "关一下灯", "把台灯关掉", "台灯关掉", "关台灯"
    };

    const bool wants_off = contains_any(transcript, kLightOffKeywords, sizeof(kLightOffKeywords) / sizeof(kLightOffKeywords[0]));
    const bool wants_on  = contains_any(transcript, kLightOnKeywords,  sizeof(kLightOnKeywords)  / sizeof(kLightOnKeywords[0]));

    if (wants_off && !wants_on) return "light_off";
    if (wants_on && !wants_off) return "light_on";

    // 优先处理更具体的关灯/开灯动词，避免“不要关灯”这类后续误判时难扩展。
    if (transcript.indexOf("关") >= 0 && transcript.indexOf("灯") >= 0) return "light_off";
    if ((transcript.indexOf("开") >= 0 || transcript.indexOf("亮") >= 0) && transcript.indexOf("灯") >= 0) return "light_on";
    if (transcript.indexOf("台灯") >= 0 && transcript.indexOf("关闭") >= 0) return "light_off";
    if (transcript.indexOf("台灯") >= 0 && transcript.indexOf("打开") >= 0) return "light_on";

    return "none";
}

String sanitize_base64(const String& input) {
    String out;
    out.reserve(input.length());
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') continue;
        out += c;
    }
    return out;
}

String build_payload(const uint8_t* jpeg_data, size_t jpeg_len) {
    String image_b64 = sanitize_base64(base64::encode(jpeg_data, jpeg_len));
    String payload;
    payload.reserve(image_b64.length() + 1400);
    payload += "{\"model\":\"";
    payload += json_escape(String(g_qwen_config.image_model));
    payload += "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"";
    payload += json_escape(String(g_qwen_config.image_prompt));
    payload += "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
    payload += image_b64;
    payload += "\"}}]}],\"response_format\":{\"type\":\"json_object\"},\"max_tokens\":300}";
    return payload;
}

String wav_data_base64(const int16_t* pcm_data, size_t sample_count) {
    const uint32_t sample_rate = 16000;
    const uint16_t bits_per_sample = 16;
    const uint16_t channels = 1;
    const uint32_t data_size = sample_count * sizeof(int16_t);
    const uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    const uint16_t block_align = channels * bits_per_sample / 8;
    const uint32_t riff_size = 36 + data_size;

    uint8_t* wav = (uint8_t*)malloc(44 + data_size);
    if (!wav) return String();

    memcpy(wav + 0, "RIFF", 4);
    wav[4] = riff_size & 0xFF;
    wav[5] = (riff_size >> 8) & 0xFF;
    wav[6] = (riff_size >> 16) & 0xFF;
    wav[7] = (riff_size >> 24) & 0xFF;
    memcpy(wav + 8, "WAVEfmt ", 8);
    wav[16] = 16; wav[17] = 0; wav[18] = 0; wav[19] = 0;
    wav[20] = 1;  wav[21] = 0;
    wav[22] = channels & 0xFF; wav[23] = (channels >> 8) & 0xFF;
    wav[24] = sample_rate & 0xFF;
    wav[25] = (sample_rate >> 8) & 0xFF;
    wav[26] = (sample_rate >> 16) & 0xFF;
    wav[27] = (sample_rate >> 24) & 0xFF;
    wav[28] = byte_rate & 0xFF;
    wav[29] = (byte_rate >> 8) & 0xFF;
    wav[30] = (byte_rate >> 16) & 0xFF;
    wav[31] = (byte_rate >> 24) & 0xFF;
    wav[32] = block_align & 0xFF; wav[33] = (block_align >> 8) & 0xFF;
    wav[34] = bits_per_sample & 0xFF; wav[35] = (bits_per_sample >> 8) & 0xFF;
    memcpy(wav + 36, "data", 4);
    wav[40] = data_size & 0xFF;
    wav[41] = (data_size >> 8) & 0xFF;
    wav[42] = (data_size >> 16) & 0xFF;
    wav[43] = (data_size >> 24) & 0xFF;
    memcpy(wav + 44, pcm_data, data_size);

    String out = sanitize_base64(base64::encode(wav, 44 + data_size));
    free(wav);
    return out;
}

String build_audio_payload(const int16_t* pcm_data, size_t sample_count) {
    String audio_b64 = wav_data_base64(pcm_data, sample_count);
    if (!audio_b64.length()) return String();

    String payload;
    payload.reserve(audio_b64.length() + 700);
    payload += "{\"model\":\"";
    payload += json_escape(String(g_qwen_config.voice_model));
    payload += "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,";
    payload += audio_b64;
    payload += "\"}}]}],\"extra_body\":{\"asr_options\":{\"language\":\"zh\",\"enable_itn\":false}}}";
    return payload;
}

String build_text_json_payload(const String& transcript) {
    String payload;
    payload.reserve(transcript.length() + 1400);
    payload += "{\"model\":\"";
    payload += json_escape(String(g_qwen_config.image_model));
    payload += "\",\"messages\":[{\"role\":\"system\",\"content\":\"";
    payload += json_escape(
        "你是桌面助手的动作规划器。"
        "你必须只返回一个 JSON 对象，不要返回 Markdown，不要返回额外解释。"
        "JSON 至少包含字段 result, face, servo, answer, light。"
        "result 只能是 succeed 或 error。"
        "face 只能是 happy、confused、surprised、idle、listening 之一。"
        "servo 只能是 happy、confused、surprised、none 之一。"
        "light 只能是 on、off、keep 之一。"
        "answer 必须是给用户的一句简短中文回复。"
        "如果用户要求开灯、打开灯、亮灯，则 light=on。"
        "如果用户要求关灯、关闭灯、熄灯，则 light=off。"
        "如果没有提到灯，则 light=keep。"
        "如果用户表达困惑、没听懂、疑问，face=confused, servo=confused。"
        "如果用户表达震惊、惊讶，face=surprised, servo=surprised。"
        "如果用户表达开心、夸奖、欢迎、积极肯定，face=happy, servo=happy。"
        "如果只是普通控制指令且没有明显情绪，face=idle, servo=none。"
        "如果文本无法理解，result=error, face=confused, servo=confused, light=keep。");
    payload += "\"},{\"role\":\"user\",\"content\":\"";
    payload += json_escape(String("用户语音转写：") + transcript);
    payload += "\"}],\"response_format\":{\"type\":\"json_object\"},\"max_tokens\":220}";
    return payload;
}

String build_audio_payload_from_base64(const String& audio_b64, const char* format) {
    if (!audio_b64.length() || !format || !strlen(format)) return String();

    String payload;
    payload.reserve(audio_b64.length() + 760);
    payload += "{\"model\":\"";
    payload += json_escape(String(g_qwen_config.voice_model));
    payload += "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/";
    payload += json_escape(String(format));
    payload += ";base64,";
    payload += audio_b64;
    payload += "\"}}]}],\"extra_body\":{\"asr_options\":{\"language\":\"zh\",\"enable_itn\":false}}}";
    return payload;
}

bool fetch_url_base64(const char* url, String& b64_out, size_t& byte_count) {
    b64_out = "";
    byte_count = 0;
    if (!url || !strlen(url)) return false;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, url)) {
        debug_log_append("[Qwen] Sample audio HTTP begin failed", "system");
        return false;
    }

    http.setConnectTimeout(15000);
    http.setTimeout(60000);
    int http_code = http.GET();
    if (http_code != 200) {
        debug_log_printf("system", "[Qwen] Sample audio GET failed: %d", http_code);
        http.end();
        return false;
    }

    int total = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        http.end();
        return false;
    }

    uint8_t* data = nullptr;
    if (total > 0) {
        data = (uint8_t*)ps_malloc((size_t)total);
        if (!data) {
            http.end();
            debug_log_append("[Qwen] Sample audio alloc failed", "system");
            return false;
        }

        size_t offset = 0;
        while (http.connected() && offset < (size_t)total) {
            size_t available = stream->available();
            if (!available) {
                delay(1);
                continue;
            }
            int got = stream->readBytes(data + offset, available);
            if (got <= 0) break;
            offset += (size_t)got;
        }
        byte_count = offset;
    } else {
        const size_t cap = 192 * 1024;
        data = (uint8_t*)ps_malloc(cap);
        if (!data) {
            http.end();
            debug_log_append("[Qwen] Sample audio alloc failed", "system");
            return false;
        }
        while (http.connected() && byte_count < cap) {
            size_t available = stream->available();
            if (!available) {
                delay(1);
                continue;
            }
            if (byte_count + available > cap) available = cap - byte_count;
            int got = stream->readBytes(data + byte_count, available);
            if (got <= 0) break;
            byte_count += (size_t)got;
        }
    }

    http.end();
    if (!data || byte_count == 0) {
        if (data) free(data);
        debug_log_append("[Qwen] Sample audio download empty", "system");
        return false;
    }

    b64_out = sanitize_base64(base64::encode(data, byte_count));
    free(data);
    return b64_out.length() > 0;
}

bool post_qwen_payload(const char* api_url_raw, const String& payload, String& result_text) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String api_url = normalized_api_url(api_url_raw);
    if (!http.begin(client, api_url)) {
        result_text = "HTTP begin failed";
        return false;
    }

    http.setConnectTimeout(15000);
    http.setTimeout(60000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + g_qwen_config.api_key);

    int http_code = http.POST((uint8_t*)payload.c_str(), payload.length());
    String response = http.getString();
    http.end();

    if (http_code <= 0) {
        result_text = String("HTTP POST failed: ") + http_code;
        debug_log_append(String("[Qwen] ") + result_text, "system");
        return false;
    }
    if (http_code < 200 || http_code >= 300) {
        result_text = String("Qwen HTTP ") + http_code + ": " + response.substring(0, 180);
        debug_log_append(result_text, "system");
        return false;
    }

    if (!extract_response_text(response, result_text)) {
        result_text = "Qwen response parsed, but no text content found";
        debug_log_append(String("[Qwen] Raw response: ") + response.substring(0, 180), "system");
        return false;
    }

    result_text.trim();
    return result_text.length() > 0;
}

}  // namespace

void qwen_set_config(const QwenConfig& config) {
    g_qwen_config = config;
}

bool qwen_is_configured() {
    return has_basic_config() &&
           g_qwen_config.image_model && strlen(g_qwen_config.image_model) > 0 &&
           g_qwen_config.image_prompt && strlen(g_qwen_config.image_prompt) > 0 &&
           g_qwen_config.voice_model && strlen(g_qwen_config.voice_model) > 0;
}

String qwen_config_status() {
    if (!g_qwen_config.image_api_url || strlen(g_qwen_config.image_api_url) == 0) {
        return "Qwen image_api_url missing";
    }
    if (contains_placeholder(g_qwen_config.image_api_url)) {
        return "Qwen image_api_url still contains placeholder";
    }
    if (!g_qwen_config.voice_api_url || strlen(g_qwen_config.voice_api_url) == 0) {
        return "Qwen voice_api_url missing";
    }
    if (contains_placeholder(g_qwen_config.voice_api_url)) {
        return "Qwen voice_api_url still contains placeholder";
    }
    if (!g_qwen_config.api_key || strlen(g_qwen_config.api_key) == 0) {
        return "Qwen api_key missing";
    }
    if (!g_qwen_config.image_model || strlen(g_qwen_config.image_model) == 0) {
        return "Qwen image model missing";
    }
    if (!g_qwen_config.image_prompt || strlen(g_qwen_config.image_prompt) == 0) {
        return "Qwen image prompt missing";
    }
    if (!g_qwen_config.voice_model || strlen(g_qwen_config.voice_model) == 0) {
        return "Qwen voice model missing";
    }
    String image_url = normalized_api_url(g_qwen_config.image_api_url);
    String voice_url = normalized_api_url(g_qwen_config.voice_api_url);
    if (!image_url.length() || !voice_url.length()) {
        return "Qwen not configured";
    }
    return String("Qwen ready: vision=") + g_qwen_config.image_model +
           ", voice=" + g_qwen_config.voice_model +
           " @ vision:" + image_url + " voice:" + voice_url;
}

bool qwen_recognize_image(const uint8_t* jpeg_data, size_t jpeg_len, String& result_text) {
    result_text = "";

    if (!qwen_is_configured()) {
        result_text = qwen_config_status();
        return false;
    }
    if (!jpeg_data || jpeg_len == 0) {
        result_text = "Empty image";
        return false;
    }

    debug_log_append("[Qwen] Encoding image and sending request...", "system");

    String payload = build_payload(jpeg_data, jpeg_len);
    if (!payload.length()) {
        result_text = "Failed to build payload";
        return false;
    }

    if (!post_qwen_payload(g_qwen_config.image_api_url, payload, result_text)) return false;
    result_text = simplify_face_result(result_text);
    return result_text.length() > 0;
}

bool qwen_recognize_voice_command(const int16_t* pcm_data, size_t sample_count, String& command_text) {
    command_text = "";

    if (!qwen_is_configured()) {
        command_text = qwen_config_status();
        return false;
    }
    if (!pcm_data || sample_count == 0) {
        command_text = "Empty audio";
        return false;
    }

    debug_log_append("[Qwen] Encoding audio and sending request...", "system");

    String payload = build_audio_payload(pcm_data, sample_count);
    if (!payload.length()) {
        command_text = "Failed to build audio payload";
        return false;
    }
    debug_log_printf("system", "[Qwen] Voice payload audio bytes=%u", (unsigned)(sample_count * sizeof(int16_t) + 44));

    if (!post_qwen_payload(g_qwen_config.voice_api_url, payload, command_text)) return false;

    String transcript = normalize_transcript(command_text);

    debug_log_printf("system", "[Qwen] Voice transcript: %s", transcript.c_str());

    command_text = classify_voice_command(transcript);
    return true;
}

bool qwen_plan_voice_response_json(const int16_t* pcm_data, size_t sample_count, String& json_text) {
    json_text = "";

    if (!qwen_is_configured()) {
        json_text = qwen_config_status();
        return false;
    }
    if (!pcm_data || sample_count == 0) {
        json_text = "Empty audio";
        return false;
    }

    String transcript;
    String asr_payload = build_audio_payload(pcm_data, sample_count);
    if (!asr_payload.length()) {
        json_text = "Audio payload build failed";
        return false;
    }
    if (!post_qwen_payload(g_qwen_config.voice_api_url, asr_payload, transcript)) {
        json_text = transcript;
        return false;
    }

    transcript = normalize_transcript(transcript);
    debug_log_append(String("[Qwen] Transcript: ") + transcript, "system");
    if (!transcript.length()) {
        json_text = "Empty transcript";
        return false;
    }

    String planner_payload = build_text_json_payload(transcript);
    if (!planner_payload.length()) {
        json_text = "Planner payload build failed";
        return false;
    }

    if (!post_qwen_payload(g_qwen_config.image_api_url, planner_payload, json_text)) {
        return false;
    }

    json_text.trim();
    return json_text.startsWith("{");
}

bool qwen_test_voice_sample(String& command_text) {
    command_text = "";

    if (!qwen_is_configured()) {
        command_text = qwen_config_status();
        return false;
    }

    debug_log_append("[Qwen] Downloading official sample mp3...", "system");
    String audio_b64;
    size_t audio_bytes = 0;
    if (!fetch_url_base64(QWEN_SAMPLE_AUDIO_URL, audio_b64, audio_bytes)) {
        command_text = "Failed to download sample audio";
        return false;
    }

    debug_log_printf("system", "[Qwen] Sample audio downloaded: %u bytes", (unsigned)audio_bytes);
    String payload = build_audio_payload_from_base64(audio_b64, "mp3");
    if (!payload.length()) {
        command_text = "Failed to build sample audio payload";
        return false;
    }
    debug_log_printf("system", "[Qwen] Sample base64 length: %u", (unsigned)audio_b64.length());
    debug_log_printf("system", "[Qwen] Sample base64 head: %.24s", audio_b64.c_str());

    if (!post_qwen_payload(g_qwen_config.voice_api_url, payload, command_text)) return false;

    debug_log_printf("system", "[Qwen] Sample transcript: %s", command_text.c_str());
    return true;
}
