#include "player_driver.h"
#include "sd_driver.h"
#include "speaker_driver.h"

static int16_t apply_gain_clip(int32_t sample, float gain) {
    int32_t scaled = (int32_t)(sample * gain);
    if (scaled > 32767) return 32767;
    if (scaled < -32768) return -32768;
    return (int16_t)scaled;
}

static bool parse_wav_header(File &file, uint32_t &data_offset, uint32_t &data_size,
                             uint32_t &sample_rate, uint16_t &channels, uint16_t &bits_per_sample) {
    uint8_t hdr[44];
    if (file.read(hdr, 44) != 44) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    channels = hdr[22] | (hdr[23] << 8);
    bits_per_sample = hdr[34] | (hdr[35] << 8);
    sample_rate = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) |
                  ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    uint32_t fmt_size = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) |
                        ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);

    // Always scan forward from end of "fmt " chunk to find "data" chunk.
    // This correctly handles WAV files with fact, LIST, or other extra chunks.
    data_offset = 20 + fmt_size;
    file.seek(data_offset);

    uint8_t chunk_id[4], chunk_sz[4];
    while (file.read(chunk_id, 4) == 4 && file.read(chunk_sz, 4) == 4) {
        uint32_t csize = (uint32_t)chunk_sz[0] | ((uint32_t)chunk_sz[1] << 8) |
                         ((uint32_t)chunk_sz[2] << 16) | ((uint32_t)chunk_sz[3] << 24);
        if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = csize;
            data_offset = file.position();   // current position = start of audio data
            return true;
        }
        // WAV chunks are word-aligned, so odd-sized chunks include one pad byte.
        file.seek(file.position() + csize + (csize & 1U));
    }
    return false;
}

bool inspect_wav(const char* path) {
    File file = sd_open_file(path, FILE_READ);
    if (!file) {
        Serial.print("Cannot open "); Serial.println(path);
        return false;
    }

    uint32_t data_offset, data_size, sample_rate;
    uint16_t channels, bits;
    if (!parse_wav_header(file, data_offset, data_size, sample_rate, channels, bits)) {
        Serial.println("Invalid WAV header");
        sd_close_file(file);
        return false;
    }

    Serial.print("WAV inspect: "); Serial.print(path); Serial.print(", ");
    Serial.print("file size "); Serial.print(file.size()); Serial.print(" bytes, ");
    Serial.print(sample_rate); Serial.print(" Hz, ");
    Serial.print(channels); Serial.print(" ch, "); Serial.print(bits); Serial.print("-bit, ");
    Serial.print(data_size); Serial.println(" bytes data");

    file.seek(data_offset);

    const size_t INSPECT_BYTES = 4096;
    uint8_t buf[INSPECT_BYTES];
    size_t got = file.read(buf, min((size_t)INSPECT_BYTES, (size_t)data_size));
    if (got == 0) {
        Serial.println("Inspect read returned 0 bytes");
        sd_close_file(file);
        return false;
    }

    int32_t peak = 0;
    int32_t first_l = 0;
    int32_t first_r = 0;

    if (bits == 16 && channels == 2 && got >= 4) {
        int16_t* s = (int16_t*)buf;
        first_l = s[0];
        first_r = s[1];
        for (size_t i = 0; i + 1 < got / 2; i++) {
            int32_t v = s[i];
            if (abs(v) > peak) peak = abs(v);
        }
    } else if (bits == 16 && channels == 1 && got >= 2) {
        int16_t* s = (int16_t*)buf;
        first_l = s[0];
        first_r = s[0];
        for (size_t i = 0; i < got / 2; i++) {
            int32_t v = s[i];
            if (abs(v) > peak) peak = abs(v);
        }
    } else if (bits == 8 && channels >= 1) {
        first_l = (int32_t)buf[0] - 128;
        first_r = first_l;
        for (size_t i = 0; i < got; i++) {
            int32_t v = abs((int32_t)buf[i] - 128);
            if (v > peak) peak = v;
        }
    } else {
        Serial.println("Inspect does not support this WAV format");
        sd_close_file(file);
        return false;
    }

    Serial.print("Inspect: first samples L="); Serial.print(first_l);
    Serial.print(", R="); Serial.print(first_r);
    Serial.print(", peak abs=");
    Serial.println(peak);

    sd_close_file(file);
    return true;
}

bool play_wav_gain(const char* path, float gain, bool verbose) {
    File file = sd_open_file(path, FILE_READ);
    if (!file) {
        Serial.print("Cannot open "); Serial.println(path);
        return false;
    }

    uint32_t data_offset, data_size, sample_rate;
    uint16_t channels, bits;
    if (!parse_wav_header(file, data_offset, data_size, sample_rate, channels, bits)) {
        Serial.println("Invalid WAV header");
        sd_close_file(file);
        return false;
    }

    if (verbose) {
        Serial.print("WAV play: "); Serial.print(path); Serial.print(", gain=");
        Serial.print(gain, 2); Serial.print(", ");
        Serial.print("file size "); Serial.print(file.size()); Serial.print(" bytes, ");
        Serial.print(sample_rate); Serial.print(" Hz, ");
        Serial.print(channels); Serial.print(" ch, "); Serial.print(bits); Serial.print("-bit, ");
        Serial.print(data_size); Serial.println(" bytes data");
    }

    file.seek(data_offset);

    const size_t CHUNK_FRAMES = 2048;
    size_t frame_size = channels * (bits / 8);
    size_t chunk_bytes = CHUNK_FRAMES * frame_size;

    uint8_t* read_buf = (uint8_t*)malloc(chunk_bytes);
    int16_t* play_buf = (int16_t*)malloc(CHUNK_FRAMES * 2 * sizeof(int16_t));
    if (!read_buf || !play_buf) {
        Serial.println("malloc failed");
        free(read_buf);
        free(play_buf);
        sd_close_file(file);
        return false;
    }

    if (!speaker_set_sample_rate(sample_rate)) {
        Serial.println("Speaker reconfigure failed");
        free(read_buf);
        free(play_buf);
        sd_close_file(file);
        return false;
    }

    size_t remaining = data_size;
    while (remaining > 0) {
        size_t to_read = chunk_bytes;
        if (to_read > remaining) {
            to_read = (remaining / frame_size) * frame_size;
        }
        if (to_read == 0) break;

        size_t got = file.read(read_buf, to_read);
        if (got == 0) break;

        size_t frames = got / frame_size;
        int32_t peak = 0;

        if (bits == 16 && channels == 2) {
            int16_t* src = (int16_t*)read_buf;
            for (size_t i = 0; i < frames * 2; i++) {
                int32_t v = src[i];
                if (abs(v) > peak) peak = abs(v);
                play_buf[i] = apply_gain_clip(v, gain);
            }
        } else if (bits == 16 && channels == 1) {
            int16_t* src = (int16_t*)read_buf;
            for (size_t i = 0; i < frames; i++) {
                int32_t v = src[i];
                if (abs(v) > peak) peak = abs(v);
                int16_t out = apply_gain_clip(v, gain);
                play_buf[i * 2] = out;
                play_buf[i * 2 + 1] = out;
            }
        } else if (bits == 8 && channels == 1) {
            for (size_t i = 0; i < frames; i++) {
                int32_t v = ((int32_t)read_buf[i] - 128) << 8;
                if (abs(v) > peak) peak = abs(v);
                int16_t out = apply_gain_clip(v, gain);
                play_buf[i * 2] = out;
                play_buf[i * 2 + 1] = out;
            }
        } else {
            Serial.println("Unsupported WAV format");
            break;
        }

        size_t written = speaker_play(play_buf, frames * 2, true);
        if (verbose) {
            Serial.print("chunk: got="); Serial.print(got);
            Serial.print(" bytes, frames="); Serial.print(frames);
            Serial.print(", peak="); Serial.print(peak);
            Serial.print(", written samples="); Serial.println(written);
        }
        remaining -= got;
    }

    speaker_idle();

    if (!speaker_restore_default()) {
        Serial.println("Speaker restore default failed");
    }

    free(read_buf);
    free(play_buf);

    sd_close_file(file);
    Serial.println("Playback finished");
    return true;
}

bool play_wav(const char* path) {
    return play_wav_gain(path, 1.0f, false);
}
