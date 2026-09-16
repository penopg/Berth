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
// Прописными: ASCII и кириллица таблицей, без локали. Заголовки групп в
// панели и на странице всех задач.
void upper_utf8(char *dst, size_t cap, const char *src);

int  ui_text_clipped(const FontAtlas *f, const char *text,
                     int x, int y, Color color, int max_width);

// Маркер состояния в ячейке строки панели. Закрытый проект — пусто,
// свободный — тусклая точка, работает — живой квадратик, зовёт — янтарная
// точка с пульсом, умер — красный крестик. `open` — есть ли сессия вовсе,
// `time` — GetTime(): анимация идёт от него, а не от счётчика кадров.
// Полоса, за пределами которой строки не рисуются: ножницы обрезают
// картинку уже на видеокарте, а процессор до этого честно раскладывает
// каждый глиф. У страницы с длинным журналом это и есть весь кадр.
// bottom <= top — рисовать всё (так работает панель).
void ui_clip_rows(int top, int bottom);

void ui_draw_marker(Rect cell, bool open, SessionState state, double time,
                    const Theme *theme);

// Верхняя полоса: имя активной вкладки слева, кнопка настроек справа.
// tasks_review — сколько задач по всем проектам ждут проверки: кнопка
// «Задачи» пишет число цветом внимания, это то, что ждёт человека.
void ui_draw_topbar(const Layout *l, const SessionList *sessions,
                    const Usage *usage, int tasks_review,
                    const FontAtlas *font, const Theme *theme, Vector2 mouse);

// Контекстное меню по правому щелчку в панели. Место для того, что делают
// редко: отдельный раздел на странице ради удаления проекта — перекос, а
// меню не стоит ни пикселя, пока его не позвали. Геометрия считается при
// открытии и не меняется, поэтому рисование и попадание смотрят на одно и
// то же — как у строк панели, живущих между кадрами.
enum {
    UI_MENU_NONE = 0,
    UI_MENU_CLOSE_SESSION,   // погасить разговор, папку не трогать
    UI_MENU_REVEAL,          // показать папку в Finder
    UI_MENU_HIDE,            // убрать проект из списка; на диске — ничего
    UI_MENU_RENAME,          // своё имя в панели; папка на диске та же
};

void ui_menu_open(const char *cwd, bool has_session, Vector2 at,
                  const FontAtlas *f);
// Меню заголовка группы: «Переименовать» (только у группы-папки, can_rename)
// и «Показать папку» (dir; пусто — пункта нет).
void ui_menu_open_group(const char *label, const char *dir, bool can_rename,
                        Vector2 at, const FontAtlas *f);
bool ui_menu_is_open(void);
bool ui_menu_is_group(void);
const char *ui_menu_label(void);   // подпись строки, по которой открыли
const char *ui_menu_cwd(void);
void ui_menu_close(void);
// Пункт под мышью и закрытие меню. Щелчок мимо меню закрывает его и
// возвращает UI_MENU_NONE: меню съедает клик, чтобы он не ушёл в панель.
int  ui_menu_click(Vector2 mouse, const FontAtlas *f);
void ui_menu_draw(const FontAtlas *f, const Theme *th, Vector2 mouse);

void ui_draw_sidebar(const Layout *l, const ProjectList *projects,
                     const SessionList *sessions,
                     const FontAtlas *font, const Theme *theme,
                     Vector2 mouse, bool splitter_active,
                     const Sprites *sprites);   // NULL — сценки выключены

#endif // BERTH_UI_H
