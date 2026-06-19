/**
 * 51 MCU 固件 — STC89C52
 * 功能: UART 指令接收 + 继电器控制 + 433MHz RF 编码发射/学习
 *
 * UART: 9600bps, 8N1
 * 帧格式 (接收): STX(0xAA) | CMD(1B) | LEN(1B) | DATA(NB) | XOR
 * 帧格式 (响应): STX(0xBB) | CMD(1B) | STATUS(1B) | XOR
 *
 * CMD:
 *   0x01 RELAY_CTRL   DATA[0]=通道(1-2) DATA[1]=0关/1开
 *   0x02 RF_SEND      DATA[0]=编码槽位(0-7)
 *   0x03 RF_LEARN     DATA[0]=槽位(0-7)
 *   0x04 STATUS_REQ   查询继电器状态
 *
 * 引脚分配:
 *   P1.0 = 继电器1 (台灯)
 *   P1.1 = 继电器2 (音响)
 *   P3.2 = INT0 (433MHz 接收模块 DATA 引脚)
 *   P1.2 = 433MHz TX 模块 DATA 引脚
 *   P1.3 = LED 状态指示
 *
 * 编译: Keil C51, 目标芯片 STC89C52RC, 晶振 11.0592MHz
 */

#include <8052.h>
#include <stdint.h>

// ── 引脚定义 ──
__sbit __at(0x90) RELAY1;   // P1.0 台灯继电器
__sbit __at(0x91) RELAY2;   // P1.1 音响继电器
__sbit __at(0x92) RF_TX;    // P1.2 433MHz 发射
__sbit __at(0x93) LED;      // P1.3 状态指示
__sbit __at(0xA0) Cont;     // P2.0 扩展输出 (风扇/备用)

// ── 常量 ──
#define STX_CMD        0xAA
#define STX_RSP        0xBB
#define CMD_RELAY_CTRL 0x01
#define CMD_RF_SEND    0x02
#define CMD_RF_LEARN   0x03
#define CMD_STATUS_REQ 0x04
#define MAX_SLOTS      8     // RF 编码槽位数
#define RF_PULSE_MAX   200   // 单槽位最大脉冲数
#define BUF_SIZE       16    // UART 接收缓冲区

// ── 全局变量 ──
static uint8_t rx_buf[BUF_SIZE];
static uint8_t rx_idx = 0;
static uint8_t rx_len = 0;      // 期望数据长度
static uint8_t rx_cmd = 0;
static uint8_t rx_state = 0;    // 0=等STX, 1=等CMD, 2=等LEN, 3=等DATA, 4=等XOR
static uint8_t rx_data_idx = 0;

static uint8_t relay_state = 0;  // bit0=RELAY1, bit1=RELAY2

// RF 编码存储 (槽位 0-7, 每个槽位存储脉冲宽度序列)
// 用片内 EEPROM (STC89C52RC 有 2KB EEPROM @ 0x2000)
// 简化: 用 XDATA 暂存 (掉电丢失, 演示用途)
static uint16_t rf_codes[MAX_SLOTS][RF_PULSE_MAX];
static uint8_t  rf_pulse_count[MAX_SLOTS];

// RF 学习状态
static uint8_t  rf_learn_slot = 0xFF;  // 0xFF = 未在学习
static uint16_t rf_learn_buf[RF_PULSE_MAX];
static uint8_t  rf_learn_idx = 0;

// ── 软件延时 (11.0592MHz, 约 1ms) ──
void delay_ms(uint16_t ms) {
    uint16_t i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 120; j++);
}

// ── UART 发送 ──
void uart_putc(uint8_t c) {
    SBUF = c;
    while (!TI);
    TI = 0;
}

void uart_send_response(uint8_t cmd, uint8_t status) {
    uint8_t chk = STX_RSP ^ cmd ^ status;
    uart_putc(STX_RSP);
    uart_putc(cmd);
    uart_putc(status);
    uart_putc(chk);
}

// ── 继电器控制 ──
void relay_set(uint8_t ch, uint8_t on) {
    if (ch == 1) {
        RELAY1 = on ? 0 : 1;  // 低电平触发
        if (on) relay_state |= 0x01; else relay_state &= ~0x01;
    } else if (ch == 2) {
        RELAY2 = on ? 0 : 1;
        if (on) relay_state |= 0x02; else relay_state &= ~0x02;
    }
}

// ── 433MHz RF 编码发射 (EV1527 格式简化版) ──
// 用延时实现 OOK 调制: 同步码 + 地址码 + 数据码
// 默认编码: 4ms 高 + 1ms 低 = bit 1, 1ms 高 + 4ms 低 = bit 0

static void rf_send_bit(uint8_t bit) {
    if (bit) {
        RF_TX = 1; delay_ms(4);
        RF_TX = 0; delay_ms(1);
    } else {
        RF_TX = 1; delay_ms(1);
        RF_TX = 0; delay_ms(4);
    }
}

static void rf_send_sync() {
    RF_TX = 1; delay_ms(1);
    RF_TX = 0; delay_ms(31);  // 同步码: 1ms H + 31ms L
}

void rf_send_code(uint8_t slot) {
    if (slot >= MAX_SLOTS) return;
    if (rf_pulse_count[slot] == 0) return;  // 未学习

    // 发送同步码
    rf_send_sync();

    // 发送脉冲序列 (记录时的高低交替)
    uint8_t i;
    for (i = 0; i < rf_pulse_count[slot] && i < RF_PULSE_MAX; i++) {
        uint16_t pw = rf_codes[slot][i];
        RF_TX = (i & 1) ? 0 : 1;  // 偶次=高, 奇次=低
        delay_ms(pw / 10);  // 近似: 100us 单位
    }
    RF_TX = 0;  // 归零
}

// ── RF 编码学习 (INT0 中断) ──
// Timer0: mode 1, 16-bit, 测量脉冲宽度
// 11.0592MHz / 12 = 921.6kHz → 1 tick = 1.085us
// 100us ≈ 92 ticks

static uint16_t timer0_ticks = 0;
static uint8_t  timer0_ovf   = 0;

void int0_isr(void) __interrupt(0) {
    // 记录 Timer0 计数值 (脉冲宽度)
    uint16_t val = (timer0_ovf << 8) | TH0;
    timer0_ticks = val;

    // 重置 Timer0
    TH0 = 0; TL0 = 0;
    timer0_ovf = 0;

    if (rf_learn_slot < MAX_SLOTS && rf_learn_idx < RF_PULSE_MAX) {
        rf_learn_buf[rf_learn_idx++] = val;
    }
}

void timer0_isr(void) __interrupt(1) {
    timer0_ovf++;
}

// 开始学习
void rf_learn_start(uint8_t slot) {
    if (slot >= MAX_SLOTS) return;
    rf_learn_slot = slot;
    rf_learn_idx = 0;
    LED = 1;  // 学习指示灯亮
}

// 停止学习并保存
void rf_learn_stop() {
    if (rf_learn_slot >= MAX_SLOTS) return;

    // 复制脉冲数据到槽位
    uint8_t i;
    rf_pulse_count[rf_learn_slot] = rf_learn_idx;
    for (i = 0; i < rf_learn_idx && i < RF_PULSE_MAX; i++) {
        rf_codes[rf_learn_slot][i] = rf_learn_buf[i];
    }

    rf_learn_slot = 0xFF;
    rf_learn_idx = 0;
    LED = 0;
}

// ── 命令处理 ──
void process_frame(uint8_t cmd, uint8_t len, uint8_t* data) {
    uint8_t status = 0;  // 0=成功

    switch (cmd) {
        case CMD_RELAY_CTRL:
            if (len >= 2) {
                relay_set(data[0], data[1]);
                status = relay_state;
            }
            break;

        case CMD_RF_SEND:
            if (len >= 1 && data[0] < MAX_SLOTS) {
                rf_send_code(data[0]);
                status = data[0];
            }
            break;

        case CMD_RF_LEARN:
            if (len >= 1 && data[0] < MAX_SLOTS) {
                rf_learn_start(data[0]);
                status = data[0];
                // 学习模式持续 5 秒后自动停止
                // (在主循环中通过定时器处理)
            }
            break;

        case CMD_STATUS_REQ:
            status = relay_state;
            break;

        default:
            status = 0xFF;  // 未知命令
            break;
    }

    uart_send_response(cmd, status);
}

// ── UART 接收 (中断驱动) ──
void uart_isr(void) __interrupt(4) {
    if (RI) {
        RI = 0;
        uint8_t c = SBUF;

        switch (rx_state) {
            case 0:  // 等 STX
                if (c == STX_CMD) {
                    rx_state = 1;
                    rx_data_idx = 0;
                }
                break;

            case 1:  // 等 CMD
                rx_cmd = c;
                rx_state = 2;
                break;

            case 2:  // 等 LEN
                rx_len = c;
                if (rx_len == 0) {
                    rx_state = 4;  // 无数据，直接等 XOR
                } else if (rx_len <= BUF_SIZE) {
                    rx_state = 3;
                    rx_data_idx = 0;
                } else {
                    rx_state = 0;  // 长度错误，重置
                }
                break;

            case 3:  // 收 DATA
                rx_buf[rx_data_idx++] = c;
                if (rx_data_idx >= rx_len) {
                    rx_state = 4;
                }
                break;

            case 4: {  // 收 XOR，校验
                uint8_t chk = rx_cmd ^ rx_len;
                uint8_t i;
                for (i = 0; i < rx_len; i++) chk ^= rx_buf[i];
                if (c == chk) {
                    process_frame(rx_cmd, rx_len, rx_buf);
                }
                rx_state = 0;
                break;
            }
        }
    }
}

// ── 主函数 ──
void main(void) {
    // GPIO 初始化
    RELAY1 = 1;  // 继电器低电平触发，初始断开
    RELAY2 = 1;
    RF_TX  = 0;
    LED    = 0;
    Cont   = 0;

    // UART 初始化: 9600bps, 8N1
    // 11.0592MHz: TH1 = 256 - 11059200/(32*12*9600) = 253 = 0xFD
    SCON = 0x50;  // Mode 1, REN=1
    TMOD &= 0x0F;
    TMOD |= 0x20;  // Timer1: mode 2, 8-bit auto-reload
    TH1 = 0xFD;
    TL1 = 0xFD;
    TR1 = 1;
    ES  = 1;      // 使能 UART 中断
    EA  = 1;      // 全局中断使能

    // Timer0: mode 1 (16-bit) 用于 RF 学习
    // (在 INT0 ISR 中重置, 不在此启动)
    TMOD &= 0xF0;
    TMOD |= 0x01;
    ET0 = 1;  // 使能 Timer0 中断
    EX0 = 1;  // 使能 INT0 中断 (下降沿触发)
    IT0 = 1;

    // RF 编码槽位清零
    {
        uint8_t i;
        for (i = 0; i < MAX_SLOTS; i++) rf_pulse_count[i] = 0;
    }

    // 启动指示: 闪灯 3 次
    {
        uint8_t i;
        for (i = 0; i < 3; i++) {
            LED = 1; delay_ms(150);
            LED = 0; delay_ms(150);
        }
    }

    // 主循环
    while (1) {
        // RF 学习超时 (5 秒后自动停止)
        static uint16_t learn_timer = 0;
        if (rf_learn_slot < MAX_SLOTS) {
            learn_timer++;
            if (learn_timer > 50000) {  // 约 5 秒
                rf_learn_stop();
                learn_timer = 0;
            }
        } else {
            learn_timer = 0;
        }

        delay_ms(1);
    }
}
