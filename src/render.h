// Отрисовка терминала в заданную область окна.
#ifndef BERTH_RENDER_H
#define BERTH_RENDER_H

#include "raylib.h"
#include "common.h"
#include "term.h"
#include "font.h"

// Рисует текущий экран сессии в область view.
//
// font_size — логический размер шрифта (до масштаба HiDPI),
// pad — отступ от краёв области до сетки символов.
// Вызывать между BeginDrawing() и EndDrawing(); перед вызовом должен быть
// сделан term_update_render_state().
void render_term(Term *t, const FontAtlas *font, Rect view, int font_size, int pad);

// Освобождает текстуры, загруженные во время отрисовки картинок Kitty.
// Вызывать после EndDrawing(): раньше нельзя, команды рисования ещё не
// доехали до GPU.
void render_flush_deferred(void);

#endif // BERTH_RENDER_H
