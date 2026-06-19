#include <8052.h>

/*
 * 433MHz 接收端 — Timer0 溢出判决 (硬件时钟, 跟编译器无关)
 *
 *   12T: 65536 tick = 71.1ms,    6T: 65536 tick = 35.6ms
 *   ON  (800ms) → >= 8 次溢出  (12T=11, 6T=22, 均 >= 8)
 *   OFF (250ms) → >= 2 次溢出  (12T=3,  6T= 7,  均 >= 2)
 *   噪声 (< 50ms) → < 2 溢出,  忽略
 */

__sbit __at(0x80) RF_DATA;
__sbit __at(0xA0) LED;
__sbit __at(0x81) Cont;

void main(void) {
    unsigned char ovf;
    unsigned int  t;

    LED = 0;
    Cont = 0;

    /* Timer0: mode 1, 16-bit */
    TMOD &= 0xF0;
    TMOD |= 0x01;

    while (1) {
        /* 等下降沿 (200ms 超时) */
        t = 0;
        while (RF_DATA == 1) { if (++t > 60000) break; }
        if (t >= 60000) continue;

        /* Timer0 测量 LOW 时长 */
        TH0 = 0;  TL0 = 0;
        TF0 = 0;  TR0 = 1;
        ovf = 0;

        while (RF_DATA == 0) {
            if (TF0) {
                TF0 = 0;
                if (++ovf > 30) break;
            }
        }
        TR0 = 0;

        /* 判决 */
        if (ovf >= 8)       { LED = 1; Cont = 1; }   /* ON  */
        else if (ovf >= 2)  { LED = 0; Cont = 0; }   /* OFF */
        /* else 噪声 < 2 溢出 → 忽略 */

        /* 等信号走高 */
        t = 0;
        while (RF_DATA == 0 && t < 60000) t++;
    }
}
