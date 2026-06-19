#!/usr/bin/env python3
"""生成 LCD 表情素材 — 240×280 RGB565 位图数组, 用于 ESP32 TFT_eSPI"""

import struct, math
from pathlib import Path

OUT = Path(__file__).parent.parent / "lcd_faces"
OUT.mkdir(parents=True, exist_ok=True)

W, H = 240, 280

# ─────────────────── 绘图工具 ───────────────────
class Canvas:
    def __init__(self, w, h, bg=(20, 20, 30)):
        self.w, self.h = w, h
        self.px = [bg] * (w * h)

    def _i(self, x, y):
        x, y = int(x), int(y)
        return y * self.w + x if 0 <= x < self.w and 0 <= y < self.h else -1

    def fill(self, x1, y1, x2, y2, color):
        for y in range(max(0,int(y1)), min(self.h,int(y2)+1)):
            for x in range(max(0,int(x1)), min(self.w,int(x2)+1)):
                self.px[y*self.w+x] = color

    def circle(self, cx, cy, r, fill):
        r=int(abs(round(r))); cx,cy=int(cx),int(cy)
        if r<=0: return
        for dy in range(-r, r+1):
            for dx in range(-r, r+1):
                if dx*dx+dy*dy <= r*r:
                    i=self._i(cx+dx,cy+dy)
                    if i>=0: self.px[i]=fill

    def ellipse(self, cx, cy, rx, ry, fill):
        rx,ry=int(abs(round(rx))),int(abs(round(ry)))
        if rx<=0 or ry<=0: return
        for y in range(-ry,ry+1):
            for x in range(-rx,rx+1):
                if (x*x)/(rx*rx)+(y*y)/(ry*ry) <= 1:
                    i=self._i(cx+x,cy+y)
                    if i>=0: self.px[i]=fill

    def rect(self, x, y, w, h, fill):
        self.fill(x, y, x+w-1, y+h-1, fill)

    def line_h(self, y, x1, x2, color):
        for x in range(int(x1), int(x2)+1):
            i=self._i(x,int(y))
            if i>=0: self.px[i]=color

    def line(self, x1, y1, x2, y2, color, thick=1):
        x1,y1,x2,y2=int(x1),int(y1),int(x2),int(y2)
        dx=abs(x2-x1); dy=abs(y2-y1)
        sx=1 if x1<x2 else -1; sy=1 if y1<y2 else -1
        err=dx-dy
        while True:
            for t in range(int(thick)):
                self.circle(x1,y1,thick/2,color)
            if x1==x2 and y1==y2: break
            e2=2*err
            if e2>-dy: err-=dy; x1+=sx
            if e2<dx: err+=dx; y1+=sy

    def arc(self, cx, cy, rx, ry, a1, a2, color, thickness=2):
        thickness=int(thickness)
        steps=80
        for s in range(steps+1):
            a=a1+(a2-a1)*s/steps
            self.circle(int(cx+rx*math.cos(a)), int(cy+ry*math.sin(a)), thickness, color)

    def to_rgb565(self):
        d=bytearray()
        for r,g,b in self.px:
            v=((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)
            d.extend(struct.pack('>H',v))
        return bytes(d)

    def save_png(self,path):
        try:
            from PIL import Image
            img=Image.new('RGB',(self.w,self.h))
            img.putdata(self.px); img.save(path)
        except: pass

# ─────────────────── 颜色 ───────────────────
BG     = (18, 18, 28)
FACE   = (255, 218, 175)
FACE_D = (218, 175, 135)
EYE    = (35, 35, 50)
MOUTH  = (155, 75, 75)
CHEEK  = (255, 155, 145)
EAR    = (245, 198, 158)
EAR_IN = (255, 178, 178)
WHITE  = (255, 255, 255)
AMBER  = (255, 188, 58)
RED    = (238, 58, 58)
GREEN  = (78, 198, 98)
BLUE   = (58, 148, 218)
PINK   = (255, 135, 128)

# ─────────────────── 通用头部 (240×280 居中) ───────────────────
def head(c):
    cx, cy = W//2, H//2
    # 脸 (缩放: ~93×110)
    c.ellipse(cx, cy+2, 93, 108, FACE)
    # 耳朵
    c.ellipse(52, 55, 34, 40, EAR)
    c.ellipse(188, 55, 34, 40, EAR)
    # 内耳
    c.ellipse(52, 60, 18, 25, EAR_IN)
    c.ellipse(188, 60, 18, 25, EAR_IN)
    # 腮红
    c.ellipse(64, 156, 26, 16, CHEEK)
    c.ellipse(176, 156, 26, 16, CHEEK)

# ────────── 表情1: 开心 😊 ──────────
def draw_happy():
    c=Canvas(W,H,BG); head(c)
    # 弯弯笑眼
    c.arc(84, 116, 24, 20, -0.5, -2.64, EYE, 5)
    c.arc(156, 116, 24, 20, -0.5, -2.64, EYE, 5)
    c.circle(80, 110, 4, WHITE); c.circle(152, 110, 4, WHITE)
    # 大笑嘴 + 舌
    c.arc(W//2, 165, 32, 22, 0.3, 2.84, MOUTH, 4)
    c.ellipse(W//2, 178, 10, 8, (255, 128, 128))
    return c, "FACE_HAPPY"

# ────────── 表情2: 待机 😐 ──────────
def draw_idle():
    c=Canvas(W,H,BG); head(c)
    # 圆眼
    c.ellipse(80, 118, 20, 24, EYE); c.ellipse(160, 118, 20, 24, EYE)
    c.circle(76, 112, 6, WHITE); c.circle(156, 112, 6, WHITE)
    c.circle(83, 123, 3, WHITE); c.circle(163, 123, 3, WHITE)
    # 小嘴
    c.ellipse(W//2, 180, 8, 6, EYE); c.ellipse(W//2, 178, 3, 3, BG)
    return c, "FACE_IDLE"

# ────────── 表情3: 聆听 🤔 ──────────
def draw_listening():
    c=Canvas(W,H,BG); head(c)
    # 左小右大眼
    c.ellipse(78, 116, 18, 22, EYE); c.ellipse(164, 112, 22, 26, EYE)
    c.circle(74, 110, 6, WHITE); c.circle(160, 106, 7, WHITE)
    c.circle(80, 119, 3, WHITE); c.circle(166, 116, 4, WHITE)
    # 歪嘴
    c.ellipse(132, 178, 8, 6, EYE); c.ellipse(132, 176, 3, 3, BG)
    # 头顶问号
    c.circle(90, 28, 10, AMBER); c.circle(90, 20, 5, AMBER); c.rect(88, 10, 5, 6, AMBER)
    c.circle(90, 31, 3, BG)
    return c, "FACE_LISTENING"

# ────────── 表情4: 惊讶 😲 ──────────
def draw_surprised():
    c=Canvas(W,H,BG); head(c)
    # 超大眼
    c.ellipse(80, 114, 22, 26, EYE); c.ellipse(160, 114, 22, 26, EYE)
    c.circle(75, 106, 7, WHITE); c.circle(155, 106, 7, WHITE)
    c.circle(83, 118, 4, WHITE); c.circle(163, 118, 4, WHITE)
    # 大圆嘴
    c.ellipse(W//2, 185, 18, 20, EYE); c.ellipse(W//2, 180, 12, 14, MOUTH)
    # 惊叹号
    c.rect(108, 18, 8, 18, AMBER); c.circle(112, 48, 5, AMBER)
    return c, "FACE_SURPRISED"

# ────────── 表情5: 睡觉 💤 ──────────
def draw_sleep():
    c=Canvas(W,H,BG); head(c)
    # 闭眼弧线
    c.arc(80, 124, 22, 14, 0.1, 3.04, EYE, 5)
    c.arc(160, 124, 22, 14, 0.1, 3.04, EYE, 5)
    # 微开嘴
    c.line_h(188, 104, 136, EYE)
    c.ellipse(W//2, 190, 7, 5, EYE)
    # Zzz (缩放)
    for ox,oy,s in [(168,72,1.0),(188,56,1.1),(208,40,1.2)]:
        w2,h2=int(20*s),int(5*s)
        c.rect(ox,oy,w2,h2,AMBER)
        c.rect(ox+w2-4,oy,5,int(16*s),AMBER)
        c.rect(ox,oy+int(16*s),w2,h2,AMBER)
    return c, "FACE_SLEEP"

# ────────── 表情6: 困惑 😕 ──────────
def draw_confused():
    c=Canvas(W,H,BG); head(c)
    # 不对称眼
    c.ellipse(76, 118, 17, 22, EYE); c.ellipse(166, 118, 17, 22, EYE)
    c.circle(72, 112, 4, WHITE); c.circle(162, 112, 4, WHITE)
    # 歪嘴
    c.arc(W//2, 178, 28, 14, -0.2, 2.6, EYE, 3)
    # 困惑线
    c.rect(100, 30, 7, 22, EYE)
    c.rect(117, 28, 7, 22, EYE)
    c.rect(134, 30, 7, 22, EYE)
    return c, "FACE_CONFUSED"

# ────────── 表情7: 卖萌 🥺 ──────────
def draw_cute():
    c=Canvas(W,H,BG); head(c)
    # 超大眼
    c.ellipse(76, 114, 24, 28, EYE); c.ellipse(164, 114, 24, 28, EYE)
    # 大高光
    c.circle(70, 104, 10, WHITE); c.circle(158, 104, 10, WHITE)
    c.circle(80, 118, 5, WHITE); c.circle(168, 118, 5, WHITE)
    # W嘴
    c.arc(108, 176, 14, 10, 0.2, 2.94, EYE, 3)
    c.arc(132, 176, 14, 10, 0.2, 2.94, EYE, 3)
    # 深腮红
    c.ellipse(58, 158, 22, 13, PINK); c.ellipse(182, 158, 22, 13, PINK)
    # 头顶爱心
    hx=W//2
    c.circle(hx-9, 14, 8, RED); c.circle(hx+9, 14, 8, RED)
    c.fill(hx-12, 17, hx+11, 25, RED)
    return c, "FACE_CUTE"

# ─────────────────── 生成 ───────────────────
FACES = [draw_happy, draw_idle, draw_listening, draw_surprised,
         draw_sleep, draw_confused, draw_cute]

for fn in FACES:
    c, name = fn()
    # PNG预览
    png = OUT / f"{name.lower()}.png"
    c.save_png(str(png))
    # JPG (LittleFS用)
    try:
        from PIL import Image
        img = Image.open(png)
        jpg = OUT / f"{name.lower()}.jpg"
        img.save(jpg, "JPEG", quality=75)
    except: pass
    # C头文件 (RGB565 PROGMEM数组)
    data = c.to_rgb565()
    lines=[]
    for i in range(0,len(data),16):
        chunk=data[i:i+16]
        lines.append('    '+', '.join(f'0x{b:02X}' for b in chunk))
    hdr = (f"// {name} — {W}×{H} RGB565 ({len(data)} bytes)\n"
           f"#ifndef {name}_H\n#define {name}_H\n"
           f"#include <cstdint>\n"
           f"const PROGMEM uint16_t {name}[{W*H}] = {{\n"
           + ',\n'.join(lines) + f"\n}};\n#endif\n")
    (OUT / f"{name.lower()}.h").write_text(hdr, encoding='utf-8')
    jsz = (OUT/f"{name.lower()}.jpg").stat().st_size if (OUT/f"{name.lower()}.jpg").exists() else 0
    print(f"[OK] {name} → .h({len(data)}B)  .jpg({jsz}B)")

print(f"\n生成完成: {len(FACES)} 套表情 @ {W}×{H}")
print(f"目录: {OUT}/")
