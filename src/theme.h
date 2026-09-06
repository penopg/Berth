// Цвета интерфейса и терминала — данными, а не константами по коду.
//
// Тем несколько, все встроенные (theme.c). Какая красит панель слева с
// полосой, а какая — окно справа (терминал и страницы вкладки), решают две
// настройки (`theme_panel`, `theme_window`); тема проекта из projects.json
// красит только его окно. Панель при переключении вкладок не
// перекрашивается.
#ifndef BERTH_THEME_H
#define BERTH_THEME_H

#include <string.h>

#include "raylib.h"

typedef struct {
    const char *name;       // как тема называется в настройках и projects.json

    // Терминал: цвета по умолчанию, пока приложение внутри не задало свои.
    Color term_bg;
    Color term_fg;

    Color sidebar_bg;
    Color sidebar_border;
    Color group_label;      // заголовок группы
    Color row_text;
    Color row_text_dim;     // второстепенное: заголовок сессии от процесса
    Color row_active_bg;
    Color row_hover_bg;
    Color splitter;
    Color splitter_hover;
    Color badge_attention;  // «агент зовёт»
    Color badge_dead;       // процесс завершился
    Color progress_bg;
    Color progress_fill;

    // Терминал: курсор и шестнадцать цветов ANSI. Без палитры тема
    // терминала половинчата: фон свой, а `ls` и `git diff` — чужими цветами.
    Color cursor;
    Color palette[16];
} Theme;

int          theme_count(void);
const Theme *theme_at(int i);
const Theme *theme_default(void);             // первая встроенная, тёмная
const Theme *theme_find(const char *name);    // NULL, если такой нет
const Theme *theme_by_name(const char *name); // незнакомое имя — по умолчанию

#define THEME_DARK (*theme_default())

#endif // BERTH_THEME_H
