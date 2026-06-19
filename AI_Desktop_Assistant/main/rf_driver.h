#ifndef RF_DRIVER_H
#define RF_DRIVER_H

#include <Arduino.h>

// 433MHz OOK 单脉冲协议:
//   51 端在检测到下降沿后, 统计 DATA 引脚保持 LOW 的时长。
//   因此 ESP32 侧需要输出 "低电平脉冲":
//     ON  = LOW 800ms
//     OFF = LOW 250ms
//   51 Timer0 溢出计数判决: >=8==ON, >=2==OFF, 6T/12T 通用

void rf_init(int tx_pin);
void rf_send_on();
void rf_send_off();

#endif
