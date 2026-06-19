#ifndef UART_DRIVER_H
#define UART_DRIVER_H

/**
 * uart_driver — ESP32 ↔ 51 MCU 通信协议
 *
 * 帧格式: STX(0xAA) | CMD(1B) | DATA_LEN(1B) | DATA(NB) | XOR_CHK(1B)
 * 响应帧: STX(0xBB) | CMD(1B) | STATUS(1B) | XOR_CHK(1B)
 *
 * CMD:
 *   0x01 RELAY_CTRL   DATA[0]=通道(1-2), DATA[1]=0关/1开
 *   0x02 RF_SEND      DATA[0]=编码槽位(0-7)
 *   0x03 RF_LEARN     DATA[0]=槽位(0-7)
 *   0x04 STATUS_REQ   无 DATA
 */

#include <Arduino.h>

// UART 配置
#define UART_BAUD      9600
#define UART_TX_PIN    9
#define UART_RX_PIN    8
#define UART_RX_TIMEOUT 50  // ms

#define STX_CMD  0xAA
#define STX_RSP  0xBB
#define CMD_RELAY_CTRL  0x01
#define CMD_RF_SEND     0x02
#define CMD_RF_LEARN    0x03
#define CMD_STATUS_REQ  0x04

inline void uart_init() {
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.println("[UART] Ready (9600bps, TX=9, RX=8)");
}

// 计算 XOR 校验
inline uint8_t uart_xor_checksum(uint8_t cmd, uint8_t len, const uint8_t* data) {
    uint8_t chk = cmd ^ len;
    for (uint8_t i = 0; i < len; i++) chk ^= data[i];
    return chk;
}

// 发送命令帧
inline void uart_send_frame(uint8_t cmd, uint8_t len, const uint8_t* data) {
    uint8_t chk = uart_xor_checksum(cmd, len, data);
    Serial1.write(STX_CMD);
    Serial1.write(cmd);
    Serial1.write(len);
    if (len > 0 && data) Serial1.write(data, len);
    Serial1.write(chk);
}

// ── 快捷命令 ──

// 继电器控制
inline void uart_relay_ctrl(uint8_t channel, bool on) {
    uint8_t data[] = { channel, (uint8_t)on };
    uart_send_frame(CMD_RELAY_CTRL, 2, data);
}

// RF 编码发射（槽位 0-7）
inline void uart_rf_send(uint8_t slot) {
    uart_send_frame(CMD_RF_SEND, 1, &slot);
}

// RF 编码学习
inline void uart_rf_learn(uint8_t slot) {
    uart_send_frame(CMD_RF_LEARN, 1, &slot);
}

// 查询 51 MCU 状态
inline void uart_status_req() {
    uart_send_frame(CMD_STATUS_REQ, 0, nullptr);
}

// 读取响应帧（非阻塞，返回 true 表示收到有效响应）
inline bool uart_read_response(uint8_t& cmd, uint8_t& status) {
    if (!Serial1.available()) return false;

    unsigned long deadline = millis() + UART_RX_TIMEOUT;
    while (Serial1.available() < 4 && millis() < deadline) { delay(1); }
    if (Serial1.available() < 4) return false;

    uint8_t stx = Serial1.read();
    if (stx != STX_RSP) {
        // 清空缓冲
        while (Serial1.available()) Serial1.read();
        return false;
    }

    cmd    = Serial1.read();
    status = Serial1.read();
    uint8_t chk = Serial1.read();

    uint8_t expected = STX_RSP ^ cmd ^ status;
    if (chk != expected) return false;

    return true;
}

#endif
