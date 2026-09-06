#!/usr/bin/env python3
"""Иконка приложения: причальный кнехт с обвёрнутым канатом.

Рисуется кодом, а не лежит бинарником: правку видно в диффе, а размеры
пересобираются одной командой.
"""
import math
import sys
from PIL import Image, ImageDraw

SIZE = 1024
SS = 2  # рисуем вдвое крупнее и уменьшаем — дешёвое сглаживание

BG_TOP    = (34, 46, 62)
BG_BOTTOM = (20, 28, 40)
METAL     = (206, 216, 230)
METAL_DARK= (150, 164, 184)
ROPE      = (216, 164, 88)
ROPE_DARK = (176, 128, 62)

def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return m

def vertical_gradient(size, top, bottom):
    g = Image.new("RGB", (1, size))
    for y in range(size):
        t = y / (size - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return g.resize((size, size))

def draw_icon(px):
    s = px * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # Фон: скруглённый квадрат в духе macOS.
    bg = vertical_gradient(s, BG_TOP, BG_BOTTOM).convert("RGBA")
    img.paste(bg, (0, 0), rounded_mask(s, int(s * 0.225)))

    d = ImageDraw.Draw(img)

    cx = s / 2
    u = s / 1024.0

    # Кнехт занимает середину: в доке иконку видно размером с ноготь, поэтому
    # силуэт должен читаться целиком, без пустых полей и мелких деталей.
    body_w   = 210 * u
    body_top = 330 * u
    body_bot = 740 * u

    # Тень на подложке — иначе фигура висит в воздухе.
    d.ellipse([cx - 300 * u, body_bot - 20 * u, cx + 300 * u, body_bot + 90 * u],
              fill=(14, 20, 30))

    # Тумба, книзу чуть шире.
    d.polygon([
        (cx - body_w / 2, body_top),
        (cx + body_w / 2, body_top),
        (cx + body_w / 2 * 1.22, body_bot),
        (cx - body_w / 2 * 1.22, body_bot),
    ], fill=METAL)
    d.polygon([
        (cx + body_w * 0.14, body_top),
        (cx + body_w / 2, body_top),
        (cx + body_w / 2 * 1.22, body_bot),
        (cx + body_w * 0.17, body_bot),
    ], fill=METAL_DARK)

    # Шляпка.
    cap_w, cap_h = 330 * u, 112 * u
    d.ellipse([cx - cap_w / 2, body_top - cap_h / 2,
               cx + cap_w / 2, body_top + cap_h / 2], fill=METAL)
    d.ellipse([cx - cap_w / 2, body_top - cap_h / 2,
               cx + cap_w / 2, body_top + cap_h * 0.05], fill=(232, 240, 250))

    # Основание.
    base_w, base_h = 430 * u, 86 * u
    d.ellipse([cx - base_w / 2, body_bot - base_h / 2,
               cx + base_w / 2, body_bot + base_h / 2], fill=METAL_DARK)
    d.ellipse([cx - base_w / 2, body_bot - base_h / 2,
               cx + base_w / 2, body_bot + base_h * 0.1], fill=METAL)

    # Канат: три витка со смещением по высоте — так это читается обмоткой,
    # а не стопкой отдельных колец.
    rope_w = int(46 * u)
    for i in range(3):
        y = (470 + i * 86) * u
        w = (300 + i * 16) * u
        h = 104 * u
        d.ellipse([cx - w / 2, y - h / 2, cx + w / 2, y + h / 2],
                  outline=ROPE_DARK, width=rope_w)
        d.arc([cx - w / 2, y - h / 2 - 7 * u, cx + w / 2, y + h / 2 - 7 * u],
              start=0, end=180, fill=ROPE, width=int(rope_w * 0.72))

    # Свободный конец уходит вправо: причал занят, судно пришвартовано.
    tail = []
    for t in range(0, 101):
        pnt = t / 100
        x = cx + 150 * u + 300 * u * pnt
        y = 640 * u + 150 * u * math.sin(pnt * math.pi * 0.75)
        tail.append((x, y))
    d.line(tail, fill=ROPE_DARK, width=int(38 * u), joint="curve")
    d.line(tail, fill=ROPE, width=int(26 * u), joint="curve")

    return img.resize((px, px), Image.LANCZOS)

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "icon.png"
    draw_icon(SIZE).save(out)
    print(out)
