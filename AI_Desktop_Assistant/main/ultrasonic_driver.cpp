#include "ultrasonic_driver.h"

namespace {

static int g_trig_pin = -1;
static int g_echo_pin = -1;
static bool g_ready = false;
static float g_last_distance_cm = -1.0f;

}

bool ultrasonic_init(int trig_pin, int echo_pin) {
    g_ready = false;
    g_last_distance_cm = -1.0f;
    g_trig_pin = trig_pin;
    g_echo_pin = echo_pin;

    if (trig_pin < 0 || echo_pin < 0) {
        return false;
    }

    pinMode(g_trig_pin, OUTPUT);
    pinMode(g_echo_pin, INPUT);
    digitalWrite(g_trig_pin, LOW);
    delay(20);

    g_ready = true;
    return true;
}

bool ultrasonic_is_ready() {
    return g_ready;
}

bool ultrasonic_read_cm(float& distance_cm, uint32_t timeout_us) {
    distance_cm = -1.0f;
    if (!g_ready) return false;

    digitalWrite(g_trig_pin, LOW);
    delayMicroseconds(3);
    digitalWrite(g_trig_pin, HIGH);
    delayMicroseconds(10);
    digitalWrite(g_trig_pin, LOW);

    unsigned long duration = pulseIn(g_echo_pin, HIGH, timeout_us);
    if (duration == 0) {
        g_last_distance_cm = -1.0f;
        return false;
    }

    distance_cm = (float)duration * 0.0343f * 0.5f;
    g_last_distance_cm = distance_cm;
    return true;
}

float ultrasonic_last_distance_cm() {
    return g_last_distance_cm;
}
