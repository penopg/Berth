// Отрисовка интерфейса вокруг терминала: боковая панель со вкладками.
//
// Готовых виджетов у raylib нет, каждый элемент рисуется здесь руками. Плата
// за это — время; выигрыш в том, что панель стоит доли миллисекунды и любой
// новый индикатор добавляется одной функцией.
#ifndef BERTH_UI_H
#define BERTH_UI_H

#include "raylib.h"
#include "layout.h"
#include "session.h"
#include "usage.h"
#include "projects.h"
#include "theme.h"
#include "font.h"
#include "scene.h"

// Текст с обрезкой по ширине и многоточием. Живёт здесь, потому что так же
// рисуются и панель, и страницы: обрыв UTF-8 на середине символа даёт мусор
// одинаково везде.
// Возвращает ширину нарисованного в точках.
int  ui_text_clipped(const FontAtlas *f, const char *text,
                     int x, int y, Color color, int max_width);

// Маркер состояния в ячейке строки панели. Закрытый проект — пусто,
// свободный — тусклая точка, работает — живой квадратик, зовёт — янтарная
// точка с пульсом, умер — красный крестик. `open` — есть ли сессия вовсе,
// `time` — GetTime(): анимация идёт от него, а не от счётчика кадров.
void ui_draw_marker(Rect cell, bool open, SessionState state, double time,
                    const Theme *theme);

// Верхняя полоса: имя активной вкладки слева, кнопка настроек справа.
void ui_draw_topbar(const Layout *l, const SessionList *sessions,
                    const Usage *usage,
                    const FontAtlas *font, const Theme *theme, Vector2 mouse);

void ui_draw_sidebar(const Layout *l, const ProjectList *projects,
                     const SessionList *sessions,
                     const FontAtlas *font, const Theme *theme,
                     Vector2 mouse, bool splitter_active,
                     const Sprites *sprites);   // NULL — сценки выключены

#endif // BERTH_UI_H
