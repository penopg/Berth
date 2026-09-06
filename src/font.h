// Шрифт и метрики ячейки.
//
// Основной атлас — весь cmap вшитого JetBrains Mono: список кодпоинтов
// генерирует build.sh из самого шрифта. Чего в нём нет (⎿, ✻, ⤷ и прочая
// символика TUI), ищется в системных шрифтах-запасках; см. font_for().
#ifndef BERTH_FONT_H
#define BERTH_FONT_H

#include <stdbool.h>
#include <stdint.h>
#include "raylib.h"

#define FONT_FALLBACK_MAX 3

// Куда смотреть за глифом: в какой шрифт и какой в нём индекс. Держим карту
// на весь BMP, потому что поиск глифа у raylib линейный по всему атласу, а
// экран терминала — это десять тысяч ячеек за кадр: на кириллице и рамках
// один такой поиск съедал больше половины кадрового бюджета.
typedef struct {
    int16_t font;    // -1 — глифа нет нигде; 0 — основной; 1.. — запаска
    int16_t glyph;   // индекс в font.glyphs и font.recs
} GlyphSlot;

#define FONT_MAP_SIZE 0x10000

typedef struct {
    Font font;
    int  size;        // логический размер шрифта, в точках экрана
    int  size_px;     // растровый размер: size * dpi
    int  cell_width;  // ширина ячейки в логических пикселях
    int  cell_height; // высота ячейки

    Font fallback[FONT_FALLBACK_MAX];
    int  fallback_count;

    GlyphSlot *map;   // FONT_MAP_SIZE записей; кодпоинты за BMP ищутся перебором
} FontAtlas;

// Загружает вшитый в бинарь JetBrains Mono и измеряет ячейку.
// dpi_scale берётся из GetWindowScaleDPI(): на Retina это {2,2}, и шрифт
// растеризуется в натуральном разрешении, иначе глифы мылят.
bool font_load(FontAtlas *a, int logical_size, Vector2 dpi_scale);

void font_unload(FontAtlas *a);

// Шрифт, которым рисовать эту строку: основной, если глиф её первого символа
// в нём есть, иначе первая запаска, где он нашёлся. Без этого raylib молча
// подставляет «?» — так и выглядели дыры в интерфейсе агента.
//
// Для панели и страниц, где строк десятки. В отрисовке терминала используется
// font_draw_codepoint(): там счёт идёт на тысячи знакомест за кадр.
Font font_for(const FontAtlas *a, const char *utf8);

// Рисует один символ в заданной точке и возвращает, было ли что рисовать.
// Минует DrawTextEx: тот на каждый вызов заново ищет глиф и разбирает UTF-8,
// а здесь и шрифт, и индекс глифа берутся из карты за одно обращение.
bool font_draw_codepoint(const FontAtlas *a, uint32_t codepoint,
                         float x, float y, float size, Color tint);

// Каким шрифтом рисуется символ: 0 — основной, 1.. — запаска, -1 — нечем.
//
// Нужно, чтобы рисовать экран по шрифтам, а не подряд: у каждого шрифта своя
// текстура, а смена текстуры обрывает батч. На экране, где запаска встречается
// в каждой четвёртой ячейке, чередование стоило десятикратного замедления.
int font_glyph_source(const FontAtlas *a, uint32_t codepoint);

// Замена для символов, которых нет ни в одном доступном шрифте. Возвращает
// сам кодпоинт, если замена не нужна.
uint32_t font_substitute(uint32_t codepoint);

#endif // BERTH_FONT_H
