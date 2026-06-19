#include "camera_driver.h"

// ===== XIAO ESP32S3 Sense 摄像头引脚定义 =====
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      10
#define SIOD_GPIO_NUM      40
#define SIOC_GPIO_NUM      39
#define Y9_GPIO_NUM        48
#define Y8_GPIO_NUM        11
#define Y7_GPIO_NUM        12
#define Y6_GPIO_NUM        14
#define Y5_GPIO_NUM        16
#define Y4_GPIO_NUM        18
#define Y3_GPIO_NUM        17
#define Y2_GPIO_NUM        15
#define VSYNC_GPIO_NUM     38
#define HREF_GPIO_NUM      47
#define PCLK_GPIO_NUM      13

static bool is_initialized = false;

bool camera_init() {
    if (is_initialized) {
        return true;
    }

    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sscb_sda = SIOD_GPIO_NUM,
        .pin_sscb_scl = SIOC_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,      // 默认 VGA，若有 PSRAM 则使用，否则降级
        .jpeg_quality = 10,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    // 根据 PSRAM 调整配置
    if (psramFound()) {
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;      // 双缓冲减少丢帧
    } else {
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return false;
    }

    is_initialized = true;
    return true;
}

camera_fb_t* camera_capture() {
    if (!is_initialized) {
        Serial.println("Camera not initialized!");
        return NULL;
    }
    return esp_camera_fb_get();
}

void camera_return_fb(camera_fb_t *fb) {
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

void camera_deinit() {
    if (is_initialized) {
        esp_camera_deinit();
        is_initialized = false;
    }
}