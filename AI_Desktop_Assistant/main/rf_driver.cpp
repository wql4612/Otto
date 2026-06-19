#include "rf_driver.h"
#include <Arduino.h>

static int tx_pin = -1;

static void rf_send_pulse_ms(int duration_ms) {
    if (tx_pin < 0) return;

    // Idle high so the 51 can see a falling edge and measure the LOW pulse width.
    digitalWrite(tx_pin, HIGH);
    delay(20);
    digitalWrite(tx_pin, LOW);
    delay(duration_ms);
    digitalWrite(tx_pin, HIGH);
    delay(20);
}

void rf_init(int pin) {
    tx_pin = pin;
    pinMode(tx_pin, OUTPUT);
    digitalWrite(tx_pin, HIGH);
}

/*
 * 单脉冲协议 — Timer0 溢出计数判决
 *   51 端按 LOW 脉冲宽度判决:
 *   ON  = 800ms LOW 脉冲 → 51 端 >= 8 次溢出
 *   OFF = 250ms LOW 脉冲 → 51 端 >= 2 次溢出
 *   Timer0 硬件时钟, 跟编译器优化无关
 */
void rf_send_on() {
    rf_send_pulse_ms(800);
}

void rf_send_off() {
    rf_send_pulse_ms(250);
}
