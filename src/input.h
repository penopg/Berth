// Ввод: клавиатура, мышь и перетаскивание скроллбара.
//
// Отображение клавиш raylib → libghostty и генерацию escape-последовательностей
// делает сам libghostty: свои таблицы escape-кодов держать не нужно.
#ifndef BERTH_INPUT_H
#define BERTH_INPUT_H

#include <stdbool.h>
#include "common.h"
#include "term.h"

typedef enum {
    INPUT_MOD_SHIFT = 1 << 0,
    INPUT_MOD_CTRL  = 1 << 1,
    INPUT_MOD_ALT   = 1 << 2,
    INPUT_MOD_SUPER = 1 << 3,
} InputMod;

// Нажат ли модификатор прямо сейчас. На macOS спрашиваем систему, а не
// оконную библиотеку: событие отпускания может не дойти до окна, и тогда
// модификатор залипает — см. macos_modifier_flags().
bool input_mod_down(InputMod mod);

// Клавиатура: печатные символы, спецклавиши, ⌘V.
void input_keys(Term *t);

// Опустошает очередь символов raylib.
//
// Нужно после срабатывания горячей клавиши приложения: raylib держит нажатия
// и как события клавиш, и как очередь символов, и несъеденный символ уехал бы
// в терминал следующим кадром — ⌘T напечатал бы «t».
void input_drain_chars(void);

// Мышь внутри области терминала view: кнопки, движение, колесо.
void input_mouse(Term *t, Rect view, int pad);

// Перетаскивание ползунка скроллбара. Возвращает true, пока идёт
// перетаскивание, чтобы вызывающий не отправил тот же клик приложению
// (иначе vim и tmux получат призрачные клики по своей области).
bool input_scrollbar(Term *t, Rect view);

#endif // BERTH_INPUT_H
