#!/usr/bin/env bash
# Сборка и запуск арены — окна со сценкой каратеки без берта.
# Пользуется raylib из vendor/ (его ставит build.sh) и теми же модулями
# спрайтов и сцены, что пойдут в берт.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build"
RAYLIB_SRC="$ROOT/vendor/raylib-5.5"
RAYLIB_LIB="$RAYLIB_SRC/src/libraylib.a"
[ -f "$RAYLIB_LIB" ] || { echo "нет raylib: сначала ./build.sh" >&2; exit 1; }

mkdir -p "$OUT"
python3 "$ROOT/assets/sprites/slice.py" "$OUT/sprites_data.h"

cc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
    -o "$OUT/arena.new" \
    "$ROOT/tools/arena.c" "$ROOT/src/sprite.c" "$ROOT/src/scene.c" \
    -I"$ROOT/src" -I"$OUT" -I"$RAYLIB_SRC/src" \
    "$RAYLIB_LIB" \
    -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL
mv -f "$OUT/arena.new" "$OUT/arena"

exec "$OUT/arena" "$@"
