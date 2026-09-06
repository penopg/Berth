#!/usr/bin/env python3
"""Нарезка раскладок спрайтов Karateka в атлас и таблицу кадров.

Вход — две раскладки (hero.png, enemy.png): ряды анимаций с подписями,
кадры в ряду разной ширины, между ними прозрачные колонки. Подписи —
чёрный пиксельный текст, фигуры — цветные, по этому и отличаем.

Выход — заголовок C: атлас PNG байтами, кадры (x, y, w, h и якорь — центр
тени по низу) и таблица анимаций по персонажам. Порядок рядов в файлах
известен и зашит здесь: у противника нет ряда «победа».

    python3 slice.py <выход.h>
"""
import io
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

# Порядок анимаций — общий для всех персонажей (enum в sprite.h). У кого
# ряда нет, у того count = 0.
ANIMS = ["stance", "walk", "bow", "run", "victory", "punch", "kick", "death"]
CHARS = [
    ("hero",  ["stance", "walk", "bow", "run", "victory", "punch", "kick", "death"]),
    ("enemy", ["stance", "walk", "bow", "run", "punch", "kick", "death"]),
]


def colored(px):
    r, g, b, a = px
    if a == 0:
        return False
    if r < 40 and g < 40 and b < 40:
        return False
    if r > 240 and g > 240 and b > 240:
        return False
    return True


def bands(pred, n, gap):
    """Отрезки [a, b) подряд идущих i с pred(i), слитые через зазор < gap."""
    out, start = [], None
    for i in range(n):
        if pred(i) and start is None:
            start = i
        if not pred(i) and start is not None:
            out.append((start, i))
            start = None
    if start is not None:
        out.append((start, n))
    merged = []
    for a, b in out:
        if merged and a - merged[-1][1] < gap:
            merged[-1] = (merged[-1][0], b)
        else:
            merged.append((a, b))
    return merged


def slice_sheet(path):
    im = Image.open(path).convert("RGBA")
    W, H = im.size
    P = im.load()
    rows = bands(lambda y: any(P[x, y][3] > 0 for x in range(W)), H, 4)
    frames = []
    for y0, y1 in rows:
        cols = bands(lambda x: any(P[x, y][3] > 0 for y in range(y0, y1)), W, 3)
        row = []
        for x0, x1 in cols:
            if not any(colored(P[x, y]) for x in range(x0, x1) for y in range(y0, y1)):
                continue   # подпись ряда
            row.append(im.crop((x0, y0, x1, y1)))
        frames.append(row)
    return frames


def anchor_x(frame):
    """Центр тени: чёрные пиксели в нижних трёх строках кадра. Тени нет —
    середина кадра."""
    w, h = frame.size
    P = frame.load()
    xs = [x for y in range(max(0, h - 3), h) for x in range(w)
          if P[x, y][3] > 0 and P[x, y][0] < 40 and P[x, y][1] < 40 and P[x, y][2] < 40]
    if not xs:
        return w // 2
    return (min(xs) + max(xs)) // 2


def main(out_path):
    atlas_rows = []   # (char, anim, [frames])
    for char, anims in CHARS:
        rows = slice_sheet(os.path.join(HERE, char + ".png"))
        if len(rows) != len(anims):
            sys.exit(f"{char}: рядов {len(rows)}, ожидалось {len(anims)}")
        for anim, row in zip(anims, rows):
            atlas_rows.append((char, anim, row))

    gap = 1
    width = max(sum(f.size[0] + gap for f in row) for _, _, row in atlas_rows)
    height = sum(max(f.size[1] for f in row) + gap for _, _, row in atlas_rows)
    atlas = Image.new("RGBA", (width, height), (0, 0, 0, 0))

    frames = []                       # (x, y, w, h, ax)
    table = {c: {a: (0, 0) for a in ANIMS} for c, _ in CHARS}
    y = 0
    for char, anim, row in atlas_rows:
        x = 0
        first = len(frames)
        rh = max(f.size[1] for f in row)
        for f in row:
            w, h = f.size
            # Все кадры ряда прижаты к общему низу: пол один на всех.
            atlas.paste(f, (x, y + rh - h), f)
            frames.append((x, y + rh - h, w, h, anchor_x(f)))
            x += w + gap
        table[char][anim] = (first, len(row))
        y += rh + gap

    buf = io.BytesIO()
    atlas.save(buf, "PNG", optimize=True)
    png = buf.getvalue()

    with open(out_path, "w") as o:
        o.write("// Сгенерировано assets/sprites/slice.py — не править руками.\n")
        o.write("#ifndef BERTH_SPRITES_DATA_H\n#define BERTH_SPRITES_DATA_H\n\n")
        o.write(f"#define SPRITES_ATLAS_W {width}\n#define SPRITES_ATLAS_H {height}\n\n")
        o.write("static const unsigned char sprites_png[] = {\n")
        for i in range(0, len(png), 24):
            o.write("    " + ",".join(str(b) for b in png[i:i + 24]) + ",\n")
        o.write("};\n\n")
        o.write("static const SpriteFrame sprite_frames[] = {\n")
        for x, y, w, h, ax in frames:
            o.write(f"    {{ {x}, {y}, {w}, {h}, {ax} }},\n")
        o.write("};\n\n")
        o.write("static const SpriteAnim sprite_anims[SPRITE_CHAR_COUNT][SPRITE_ANIM_COUNT] = {\n")
        for char, _ in CHARS:
            cells = ", ".join(f"{{ {table[char][a][0]}, {table[char][a][1]} }}" for a in ANIMS)
            o.write(f"    {{ {cells} }},   // {char}\n")
        o.write("};\n\n#endif\n")
    print(f"{out_path}: {len(frames)} кадров, атлас {width}x{height}, PNG {len(png)} байт")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "sprites_data.h")
