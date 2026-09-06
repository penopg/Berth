#!/usr/bin/env bash
# Сборка term-lab.
#
# Почему не CMake, как у Ghostling: build.zig ghostty безусловно собирает
# XCFramework для iOS, а это требует полного Xcode, хотя README обещает
# Command Line Tools. Сама libghostty-vt при этом собирается нормально —
# `zig build lib-vt` доходит до конца. Поэтому зависимости собираем адресно,
# а линкуем компилятором напрямую.
#
# Переменные:
#   DEPS   — куда класть исходники и артефакты зависимостей (по умолчанию vendor/)
#   OUT    — куда класть бинарь (по умолчанию build/)
#   CC     — компилятор
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="${DEPS:-$ROOT/vendor}"
OUT="${OUT:-$ROOT/build}"
CC="${CC:-cc}"

# Версии зафиксированы намеренно, см. NOTICE.md.
GHOSTTY_REPO="https://github.com/ghostty-org/ghostty.git"
GHOSTTY_COMMIT="f64f4aca2c29b554d111b36c3d946a9bddd159ff"
RAYLIB_VERSION="5.5"
RAYLIB_URL="https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz"

FONT_TTF="${FONT_TTF:-$ROOT/vendor/fonts/JetBrainsMono-Regular.ttf}"
# Иконки проектов (папка, ветка, логотипы языков) живут в приватной области
# Unicode: их знает только Nerd Font. Без него на их месте «?».
NERD_TTF="${NERD_TTF:-$ROOT/vendor/fonts/SymbolsNerdFontMono-Regular.ttf}"
NERD_URL="https://github.com/ryanoasis/nerd-fonts/raw/v3.4.0/patched-fonts/NerdFontsSymbolsOnly/SymbolsNerdFontMono-Regular.ttf"

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mошибка:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$DEPS" "$OUT"

# --- libghostty-vt -----------------------------------------------------------
GHOSTTY_SRC="$DEPS/ghostty"
GHOSTTY_LIB="$GHOSTTY_SRC/zig-out/lib/libghostty-vt.a"

if [ ! -f "$GHOSTTY_LIB" ]; then
    command -v zig >/dev/null || die "нужен zig (brew install zig)"
    if [ ! -d "$GHOSTTY_SRC/.git" ]; then
        say "клонирую ghostty на $GHOSTTY_COMMIT"
        git clone --filter=blob:none "$GHOSTTY_REPO" "$GHOSTTY_SRC"
    fi
    git -C "$GHOSTTY_SRC" fetch --depth 1 origin "$GHOSTTY_COMMIT" 2>/dev/null || true
    git -C "$GHOSTTY_SRC" checkout -q "$GHOSTTY_COMMIT"
    say "собираю libghostty-vt (несколько минут при первом запуске)"
    # Именно lib-vt, а не полная сборка: полная упирается в XCFramework.
    (cd "$GHOSTTY_SRC" && zig build lib-vt -Doptimize=ReleaseFast)
    [ -f "$GHOSTTY_LIB" ] || die "libghostty-vt.a не появилась"
fi

# --- raylib ------------------------------------------------------------------
RAYLIB_SRC="$DEPS/raylib-${RAYLIB_VERSION}"
RAYLIB_LIB="$RAYLIB_SRC/src/libraylib.a"

if [ ! -f "$RAYLIB_LIB" ]; then
    if [ ! -d "$RAYLIB_SRC" ]; then
        say "качаю raylib ${RAYLIB_VERSION}"
        curl -fsSL "$RAYLIB_URL" | tar -xz -C "$DEPS"
    fi
    say "собираю raylib"
    make -C "$RAYLIB_SRC/src" PLATFORM=PLATFORM_DESKTOP -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
    [ -f "$RAYLIB_LIB" ] || die "libraylib.a не появилась"
fi

# --- иконка в заголовок ------------------------------------------------------
# Голый бинарь (build/berth, berth-stable) запускается из терминала и иконку
# из Berth.app не получает — в доке была заглушка. Поэтому картинка едет в
# самом бинаре, как шрифт, и ставится в док при старте (macos_set_dock_icon).
# 512 px хватает: док крупнее не рисует, а 1024 добавляли бы 200 КБ.
ICON_HEADER="$OUT/icon_png.h"
if [ ! -f "$ICON_HEADER" ] || [ "$ROOT/assets/make_icon.py" -nt "$ICON_HEADER" ]; then
    say "вшиваю иконку в заголовок"
    python3 "$ROOT/assets/make_icon.py" "$OUT/berth-1024.png" >/dev/null
    sips -z 512 512 "$OUT/berth-1024.png" --out "$OUT/berth-512.png" >/dev/null
    python3 - "$OUT/berth-512.png" "$ICON_HEADER" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write("// Сгенерировано build.sh. Не редактировать.\n")
    f.write("static const unsigned char icon_png[] = {\n")
    for i in range(0, len(data), 16):
        f.write("    " + ",".join("0x%02x" % b for b in data[i:i+16]) + ",\n")
    f.write("};\n")
PY
fi

# --- шрифт в заголовок -------------------------------------------------------
FONT_HEADER="$OUT/font_jetbrains_mono.h"
CP_HEADER="$OUT/font_codepoints.h"
if [ ! -f "$FONT_HEADER" ] || [ ! -f "$CP_HEADER" ] || [ "$FONT_TTF" -nt "$FONT_HEADER" ]; then
    [ -f "$FONT_TTF" ] || die "нет шрифта: $FONT_TTF (положите JetBrainsMono-Regular.ttf или задайте FONT_TTF)"
    say "вшиваю шрифт в заголовок"
    python3 - "$FONT_TTF" "$FONT_HEADER" "$CP_HEADER" <<'PY'
import struct, sys
src, dst, cp_dst = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
with open(dst, 'w') as f:
    f.write("// Сгенерировано build.sh. Не редактировать.\n")
    f.write("#include <stddef.h>\n#include <stdint.h>\n\n")
    f.write("static const unsigned char font_jetbrains_mono[] = {\n")
    for i in range(0, len(data), 16):
        f.write("    " + ",".join("0x%02x" % b for b in data[i:i+16]) + ",\n")
    f.write("};\n")

# Кодпоинты атласа берём из самого шрифта (таблица cmap): грузить нужно ровно
# то, что он умеет нарисовать. Список руками отставал от жизни — не было ни é,
# ни половины стрелок, а вместо отсутствующего глифа raylib рисует «?».
def cmap_codepoints(d):
    tables = {}
    for i in range(struct.unpack(">H", d[4:6])[0]):
        o = 12 + 16 * i
        tables[d[o:o+4]] = struct.unpack(">II", d[o+8:o+16])
    off = tables[b"cmap"][0]
    cps = set()
    for i in range(struct.unpack(">H", d[off+2:off+4])[0]):
        _, _, sub = struct.unpack(">HHI", d[off+4+8*i:off+12+8*i])
        sub += off
        fmt = struct.unpack(">H", d[sub:sub+2])[0]
        if fmt == 4:
            seg2 = struct.unpack(">H", d[sub+6:sub+8])[0]
            seg = seg2 // 2
            ends = struct.unpack(">%dH" % seg, d[sub+14:sub+14+seg2])
            starts = struct.unpack(">%dH" % seg, d[sub+16+seg2:sub+16+2*seg2])
            for s, e in zip(starts, ends):
                if s != 0xFFFF:
                    cps.update(range(s, e + 1))
        elif fmt == 12:
            n = struct.unpack(">I", d[sub+12:sub+16])[0]
            for g in range(n):
                s, e, _ = struct.unpack(">III", d[sub+16+12*g:sub+28+12*g])
                cps.update(range(s, e + 1))
    return cps

cps = sorted(c for c in cmap_codepoints(data) if 32 <= c < 0xFFFE)
with open(cp_dst, "w") as f:
    f.write("// Сгенерировано build.sh из cmap шрифта. Не редактировать.\n\n")
    f.write("static const int font_codepoints[] = {\n")
    for i in range(0, len(cps), 12):
        f.write("    " + ",".join("0x%04x" % c for c in cps[i:i+12]) + ",\n")
    f.write("};\n")
PY
fi

# --- шрифт иконок ------------------------------------------------------------
if [ ! -f "$NERD_TTF" ]; then
    say "качаю Symbols Nerd Font"
    mkdir -p "$(dirname "$NERD_TTF")"
    if curl -fsSL -o "$NERD_TTF.part" "$NERD_URL"; then
        mv -f "$NERD_TTF.part" "$NERD_TTF"
    else
        rm -f "$NERD_TTF.part"
        say "не скачался — иконки проектов останутся пустыми"
    fi
fi

# --- сборка ------------------------------------------------------------------
say "компилирую berth"
DEFINES=()
# Шрифт иконок вкладываем в бинарь директивой ассемблера .incbin (см. font.c):
# заголовок с массивом на 2.5 МБ компилировался бы секунды.
[ -f "$NERD_TTF" ] && DEFINES+=(-DBERTH_NERD_FONT="\"$NERD_TTF\"")
FRAMEWORKS=()
PLATFORM_SRC=()
case "$(uname -s)" in
    Darwin)
        FRAMEWORKS=(-framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL)
        # Правки меню Cocoa — единственный Objective-C в проекте.
        PLATFORM_SRC=("$ROOT"/src/*.m)
        ;;
    Linux)  FRAMEWORKS=(-lm -lpthread -ldl -lrt -lX11) ;;
esac

# Собираем во временный файл и подменяем переименованием. Так уже запущенный
# экземпляр не пострадает: он держит старый inode и доживёт до перезапуска —
# это важно, когда терминал дорабатывают, сидя в нём же.
"$CC" -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT/berth.new" \
    "$ROOT"/src/*.c "${PLATFORM_SRC[@]}" "${DEFINES[@]}" \
    -I"$GHOSTTY_SRC/include" -I"$OUT" -I"$RAYLIB_SRC/src" \
    "$GHOSTTY_LIB" "$RAYLIB_LIB" \
    "${FRAMEWORKS[@]}"

mv -f "$OUT/berth.new" "$OUT/berth"

say "готово: $OUT/berth"
