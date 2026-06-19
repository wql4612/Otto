#include "sd_driver.h"

bool sd_init(uint8_t cs_pin) {
    return SD.begin(cs_pin);
}

bool sd_file_exists(const char* path) {
    return SD.exists(path);
}

char* sd_generate_filename(const char* prefix, const char* ext, char* buffer, size_t buf_len) {
    int max_num = 0;
    File root = SD.open("/");
    if (!root) {
        snprintf(buffer, buf_len, "/%s001.%s", prefix, ext);
        return buffer;
    }
    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        if (name.startsWith(prefix) && name.endsWith(ext)) {
            int numStart = strlen(prefix);
            int numEnd = name.length() - strlen(ext) - 1;
            if (numEnd > numStart) {
                String numStr = name.substring(numStart, numEnd);
                int num = numStr.toInt();
                if (num > max_num) max_num = num;
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    snprintf(buffer, buf_len, "/%s%03d.%s", prefix, max_num + 1, ext);
    return buffer;
}

File sd_open_file(const char* path, const char* mode) {
    return SD.open(path, mode);
}

size_t sd_write_file(File &file, const uint8_t* data, size_t len) {
    return file.write(data, len);
}

size_t sd_read_file(File &file, uint8_t* buffer, size_t len) {
    return file.read(buffer, len);
}

void sd_close_file(File &file) {
    file.close();
}