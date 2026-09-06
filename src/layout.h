// Раскладка окна: где панель, где терминал, где какая строка.
//
// Отдельно от отрисовки намеренно. Координаты считаются в одном месте, и
// попадание мыши проверяется по тем же прямоугольникам, по которым рисуем —
// иначе клик и картинка разъезжаются, а каждый новый элемент панели
// приходится согласовывать вручную в двух местах.
#ifndef BERTH_LAYOUT_H
#define BERTH_LAYOUT_H

#include <stdbool.h>

#include "raylib.h"
#include "common.h"
#include "session.h"
#include "projects.h"

#define SIDEBAR_WIDTH_MIN 160
#define SIDEBAR_WIDTH_MAX 460
#define SPLITTER_WIDTH    4
#define SPLITTER_GRAB     6   // зона захвата шире самой полоски

#define PANEL_ROW_MAX (PROJECT_MAX + SESSION_MAX + 32)

typedef enum {
    PANEL_ROW_GROUP = 0,   // заголовок группы проектов
    PANEL_ROW_ITEM,        // проект и/или открытая вкладка
    PANEL_ROW_TASK,        // фоновая задача проекта: сбор журнала и подобное
    PANEL_ROW_ADD_GROUP,   // «Добавить группу» в самом низу
} PanelRowKind;

// Строка панели. Проект и сессия независимы: проект без сессии — закрытый
// проект, который можно открыть; сессия без проекта — вкладка, которой нет
// в списке (открыта из командной строки или по ⌘T).
typedef struct {
    PanelRowKind kind;
    Rect rect;
    int  project;          // индекс в ProjectList, либо -1
    int  session;          // индекс в SessionList, либо -1
    const char *label;     // подпись заголовка группы
    bool collapsed;        // группа свёрнута
    int  hidden;           // сколько проектов спрятано под заголовком (или
                           // подпроектов под свёрнутым родителем)

    // Подпроекты: строка родителя знает, есть ли они и раскрыты ли;
    // строка подпроекта стоит с отступом.
    int  depth;            // 0 — проект, 1 — подпроект или задача, 2 — задача подпроекта
    bool has_subs;
    bool expanded;

    // Дерево: направляющая идёт от маркера родителя вниз через детей и
    // заканчивается уголком у последнего. Это состояние строки, а не
    // отрисовки: кто последний, известно только при сборке списка.
    bool has_children;     // под строкой видны дети — направляющая начинается здесь
    bool last;             // последний ребёнок своего родителя
    bool cont;             // сквозь строку проходит направляющая внешнего уровня

    // Заголовок группы: сколько в ней проектов и цвет первого из них —
    // тонкой чертой под заголовком, единственное место, где цвет проекта
    // из projects.json ещё виден.
    int   count;
    Color color;
} PanelRow;

#define LAYOUT_COLLAPSED_MAX 32

typedef struct {
    int row_height;        // высота строки открытой вкладки (две строки текста)
    int row_height_idle;   // высота строки закрытого проекта (одна строка)
    int task_height;       // высота строки фоновой задачи
    int row_height_scene;  // строка активного разговора со сценкой каратеки
    int group_height;
    int topbar_height;     // высота верхней полосы с кнопками окна
    int settings_width;    // ширина кнопки настроек на ней
    int pad;
} LayoutMetrics;

typedef struct {
    Rect window;

    // Верхняя полоса на всю ширину: место для того, что относится к окну, а
    // не к проекту, — настройки. До неё их можно было открыть только по ⌘,
    // и человек, не знающий сочетания, до них не добирался.
    Rect topbar;
    Rect topbar_settings;   // кнопка настроек в правом краю полосы

    Rect sidebar;
    Rect splitter;
    Rect term;

    int  sidebar_width;
    bool sidebar_visible;

    // Проектов заметно больше, чем влезает в окно, поэтому панель прокручивается.
    int  scroll;
    int  content_height;

    PanelRow rows[PANEL_ROW_MAX];
    int      row_count;

    // Свёрнутые группы — по имени: список проектов перечитывается, а
    // свёрнутость должна пережить это. Это состояние панели, не выбор
    // человека, поэтому живёт в раскладке, а не в настройках.
    char collapsed[LAYOUT_COLLAPSED_MAX][PROJECT_NAME_MAX];
    int  collapsed_count;

    // Показывать ли в свёрнутой группе проекты с живой сессией. Без этого
    // разговор пропадает с глаз; но кому-то нужна именно тишина.
    bool collapsed_show_live;
    int  ctx_warn, ctx_crit;   // пороги цвета остатка контекста, из настроек
    int  sleep_after;          // секунд простоя, после которых разговор спит; 0 — никогда
    bool scene;                // в строке активного разговора — сценка с каратекой

    // Проекты, у которых подпроекты раскрыты, — по пути. По умолчанию
    // свёрнуты: подпроектов у проекта бывает десяток, и панель не резиновая.
    char expanded[LAYOUT_COLLAPSED_MAX][PROJECT_PATH_MAX];
    int  expanded_count;

    LayoutMetrics metrics;
} Layout;

void layout_init(Layout *l, LayoutMetrics metrics);

// Свёрнутость группы по имени.
bool layout_group_collapsed(const Layout *l, const char *group);
void layout_set_group_collapsed(Layout *l, const char *group, bool collapsed);

// Раскрыты ли подпроекты проекта с таким путём.
bool layout_parent_expanded(const Layout *l, const char *path);
void layout_set_parent_expanded(Layout *l, const char *path, bool expanded);

// Пересчитывает всё под текущий размер окна, список проектов и вкладок.
void layout_compute(Layout *l, const ProjectList *projects,
                    const SessionList *sessions, int win_w, int win_h);

void layout_set_sidebar_width(Layout *l, int width);
void layout_toggle_sidebar(Layout *l);
void layout_scroll_by(Layout *l, int delta);

// Кнопка «показать страницу проекта» в правом крае строки. Считается здесь, а
// не в отрисовке, потому что по этому же прямоугольнику проверяется клик.
static inline Rect layout_row_info_rect(Rect row, int cell_width)
{
    int w = cell_width * 2 + 8;
    return (Rect){ row.x + row.w - w - 4, row.y + 2, w, row.h - 4 };
}

// «+ проект» в заголовке группы — слева от крестика, тем же размером.
static inline Rect layout_row_group_add_rect(Rect row, int cell_width)
{
    Rect x = layout_row_info_rect(row, cell_width);
    x.x -= x.w + 4;
    return x;
}

// Ячейка маркера состояния в левом столбце строки: 10×10, по центру первой
// строки текста. Здесь же поселится сценка с человечком, поэтому размер
// ячейки и её место закреплены в раскладке, а не в отрисовке.
#define ROW_MARKER      10
#define ROW_MARKER_PAD  8    // от левого края строки до ячейки
#define ROW_TEXT_X      (ROW_MARKER_PAD + ROW_MARKER + 6)  // где начинается текст
#define ROW_INDENT      (ROW_MARKER + 6)   // отступ уровня дерева: ширина ячейки

static inline Rect layout_row_marker_rect(Rect row, int line_y, int cell_height, int depth)
{
    return (Rect){ row.x + ROW_MARKER_PAD + depth * ROW_INDENT,
                   line_y + (cell_height - ROW_MARKER) / 2,
                   ROW_MARKER, ROW_MARKER };
}

// Направляющая дерева уровня `depth` идёт по центру ячейки маркера
// родителя, то есть уровня на один меньше.
static inline int layout_row_guide_x(Rect row, int depth)
{
    return row.x + ROW_MARKER_PAD + ROW_MARKER / 2 + (depth - 1) * ROW_INDENT;
}

// Стрелка подпроектов в левом краю строки родителя: клик по ней раскрывает
// или сворачивает их, клик по остальной строке открывает проект. Ячейка
// маркера входит в зону: она не кнопка, а цель для клика так шире.
static inline Rect layout_row_expand_rect(Rect row, int cell_width)
{
    return (Rect){ row.x, row.y, ROW_TEXT_X + cell_width * 2, row.h };
}

// Строка под курсором, либо NULL.
const PanelRow *layout_hit_row(const Layout *l, Vector2 mouse);

// Курсор в зоне захвата разделителя.
bool layout_hit_splitter(const Layout *l, Vector2 mouse);

// Курсор над верхней полосой и над её кнопкой настроек.
bool layout_hit_topbar(const Layout *l, Vector2 mouse);
bool layout_hit_settings(const Layout *l, Vector2 mouse);

// Переход по порядку строк панели, а не по порядку в массиве сессий.
// Панель группирует вкладки, поэтому визуальный порядок не совпадает с
// индексами: ⌘3 должно выбирать третью открытую вкладку сверху.
int  layout_session_at(const Layout *l, int nth);
int  layout_position_of(const Layout *l, int session);
int  layout_session_count(const Layout *l);

#endif // BERTH_LAYOUT_H
