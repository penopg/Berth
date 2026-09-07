#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "page.h"
#include "ui.h"
#include "input.h"
#include "utf8.h"
#include "skills_builtin.h"

// Состояние вывода на время одного кадра: где рисуем, чем и что уже нажали.
typedef struct {
    const FontAtlas *font;
    const Theme     *theme;
    Rect    view;
    Vector2 mouse;
    bool    click;      // левая кнопка нажата именно в этом кадре

    int x, y;           // курсор вывода
    int col_w;          // ширина колонки: страница бывает в две колонки
    int row_x;          // курсор внутри ряда кнопок
    int row_home;       // куда переносится ряд, когда не влезает
    bool row_used;      // в ряду уже есть кнопка
    int line;           // высота строки

    // Откуда начинается прокручиваемая часть. Всё выше — шапка проекта и
    // кнопки: они стоят на месте, иначе лента, открытая на свежем конце,
    // уводит их за верх окна, и нажать становится нечего.
    int scroll_top;
    int scroll;

    // Чей список задач рисуем сейчас: события из него получают этот адрес.
    const char *list_cwd;
    int         list_sub;

    PageEvent event;
} Ctx;

static const int PAD_X = 28;
static const int PAD_Y = 22;

static Ctx ctx_begin(const FontAtlas *font, const Theme *theme,
                     Rect view, Vector2 mouse, int scroll)
{
    Ctx c = {
        .font = font,
        .theme = theme,
        .view = view,
        .mouse = mouse,
        .click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT),
        .x = view.x + PAD_X,
        .y = view.y + PAD_Y,
        .col_w = view.w - PAD_X * 2,
        .line = font->cell_height + 4,
        .scroll = scroll,
        .list_sub = -1,
        .event = { .sub = -1 },
    };
    DrawRectangle(view.x, view.y, view.w, view.h, theme->term_bg);
    BeginScissorMode(view.x, view.y, view.w, view.h);
    return c;
}

// Отсюда и ниже содержимое едет под колесом. Ножницы переставляем, чтобы
// прокрученное не наезжало на шапку.
static void scroll_begin(Ctx *c)
{
    c->scroll_top = c->y;
    c->y -= c->scroll;
    EndScissorMode();
    BeginScissorMode(c->view.x, c->scroll_top,
                     c->view.w, c->view.y + c->view.h - c->scroll_top);
}

// Сколько содержимого не поместилось. Считается по последней строке: сколько
// нарисовали, столько и есть — заранее эту высоту неоткуда взять.
static int ctx_end(const Ctx *c)
{
    EndScissorMode();
    if (!c->scroll_top) return 0;
    int content = c->y + c->scroll - c->scroll_top;
    int room    = c->view.y + c->view.h - PAD_Y - c->scroll_top;
    return content > room ? content - room : 0;
}

static int content_width(const Ctx *c)
{
    return c->col_w;
}

// Сколько знакомест займёт подпись: ширина кнопки и ссылки считается по ней.
static int chars_of(const char *label)
{
    int chars = 0;
    for (const char *p = label; *p; ) {
        int size = 0;
        GetCodepointNext(p, &size);
        if (size <= 0) break;
        p += size;
        chars++;
    }
    return chars;
}

static void text(Ctx *c, const char *s, Color color)
{
    ui_text_clipped(c->font, s, c->x, c->y, color, content_width(c));
    c->y += c->line;
}

static void gap(Ctx *c, int lines)
{
    c->y += c->line * lines / 2;
}

// Заголовок раздела: приглушённый, с тонкой чертой под ним. Разделы на
// странице различаются глазом, а не отступом.
static void section(Ctx *c, const char *title)
{
    gap(c, 2);
    ui_text_clipped(c->font, title, c->x, c->y, c->theme->group_label,
                    content_width(c));
    c->y += c->line;
    DrawRectangle(c->x, c->y - 3, content_width(c), 1, c->theme->sidebar_border);
    gap(c, 1);
}

static bool inside(Rect r, Vector2 m);
static bool visible_hit(const Ctx *c, Vector2 p);

// Тумблер: дорожка с ползунком, влево — выключено, вправо — включено.
// Галка не читается — стоит она или нет, видно только вблизи; у тумблера
// положение и цвет говорят сразу. Подпись справа дублирует словом.
// Возвращает true, если по нему нажали.
static bool toggle(Ctx *c, bool on)
{
    int h = c->font->cell_height - 2;
    int w = h * 2;
    Rect r = { c->row_x, c->y + 6, w, h };
    bool hover = inside(r, c->mouse);
    Color track = on ? c->theme->splitter_hover : c->theme->row_active_bg;
    if (hover && !on) track = c->theme->row_hover_bg;
    DrawRectangleRounded((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h },
                         1.0f, 8, track);
    int kr = h / 2 - 2;
    int kx = on ? r.x + w - kr - 2 : r.x + kr + 2;
    DrawCircle(kx, r.y + h / 2, (float)kr, on ? c->theme->term_bg : c->theme->row_text_dim);

    const char *word = on ? "вкл" : "выкл";
    ui_text_clipped(c->font, word, r.x + w + c->font->cell_width, c->y + 6,
                    on ? c->theme->row_text : c->theme->row_text_dim, c->font->cell_width * 5);
    c->row_x = r.x + w + c->font->cell_width * 6;

    Rect hit = { r.x, c->y, w + c->font->cell_width * 6, c->font->cell_height + 12 };
    return c->click && inside(hit, c->mouse) && visible_hit(c, c->mouse);
}

static bool inside(Rect r, Vector2 m)
{
    return m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h;
}

// Кнопка в текущем ряду. Ряд начинается с row_begin(), кнопки идут слева
// направо, ширина считается по длине подписи.
static void row_begin(Ctx *c)
{
    c->row_x = c->x;
    c->row_home = c->x;
    c->row_used = false;
}

static void row_end(Ctx *c)
{
    c->y += c->line + 10;
}

static bool button(Ctx *c, const char *label, bool accent)
{
    int chars = chars_of(label);

    Rect r = {
        .x = c->row_x,
        .y = c->y,
        .w = chars * c->font->cell_width + 24,
        .h = c->font->cell_height + 12,
    };
    // Ряд не влезает — переносим кнопку на следующую строку, в ту же
    // колонку, где ряд начался: у настройки это колонка после подписи.
    int home = c->row_home ? c->row_home : c->x;
    if (r.x + r.w > c->x + content_width(c) && c->row_x > home) {
        c->y += r.h + 8;
        c->row_x = home;
        r.x = home;
        r.y = c->y;
    }
    c->row_used = true;

    bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
    Color bg = hover ? c->theme->row_hover_bg : c->theme->row_active_bg;
    DrawRectangle(r.x, r.y, r.w, r.h, bg);
    if (accent)
        DrawRectangle(r.x, r.y, 3, r.h, c->theme->progress_fill);

    ui_text_clipped(c->font, label, r.x + 12, r.y + 6,
                    accent ? c->theme->row_text : c->theme->row_text_dim,
                    r.w - 20);

    c->row_x = r.x + r.w + 8;
    return hover && c->click;
}

// Кликабельная строка во всю ширину — для списка сессий.
// Видна ли точка в прокручиваемой части. Ножницы обрезают картинку, но не
// попадание мыши: строка, уехавшая под закреплённую шапку, оставалась
// кликабельной и перехватывала клик по кнопке над ней.
static bool visible_hit(const Ctx *c, Vector2 p)
{
    if (!c->scroll_top) return true;   // прокрутка ещё не началась
    return p.y >= c->scroll_top && p.y < c->view.y + c->view.h;
}

// Заголовок раздела с действием у правого края: «Сводка … Обновить».
// Действие живёт рядом с тем, на что действует, а не в общем ряду кнопок
// сверху, где «Обновить сводку» и «Собрать журнал» стояли без контекста.
// Возвращает true, если по действию нажали.
// Заголовок с двумя действиями: возвращает 1 или 2 по нажатому. Второе
// действие стоит левее первого — порядок чтения тот же, что порядок слов.
static int section_action2(Ctx *c, const char *title, const char *a1, const char *a2)
{
    gap(c, 2);
    int w = content_width(c);
    int hit = 0;
    int right = c->x + w;
    const char *acts[2] = { a1, a2 };
    for (int i = 0; i < 2; i++) {
        if (!acts[i]) continue;
        int aw = chars_of(acts[i]) * c->font->cell_width;
        Rect r = { right - aw - 8, c->y - 3, aw + 16, c->line + 2 };
        bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
        if (hover) DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);
        ui_text_clipped(c->font, acts[i], r.x + 8, c->y,
                        hover ? c->theme->row_text : c->theme->row_text_dim, aw);
        if (hover && c->click) hit = i + 1;
        right = r.x - 8;
    }
    ui_text_clipped(c->font, title, c->x, c->y, c->theme->group_label,
                    right - c->x - 8);
    c->y += c->line;
    DrawRectangle(c->x, c->y - 3, content_width(c), 1, c->theme->sidebar_border);
    gap(c, 1);
    return hit;
}

static bool section_action(Ctx *c, const char *title, const char *action)
{
    gap(c, 2);
    int w = content_width(c);
    bool hit = false;
    int title_w = w;
    if (action) {
        int aw = chars_of(action) * c->font->cell_width;
        Rect r = { c->x + w - aw - 8, c->y - 3, aw + 16, c->line + 2 };
        bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
        if (hover) DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);
        ui_text_clipped(c->font, action, r.x + 8, c->y,
                        hover ? c->theme->row_text : c->theme->row_text_dim, aw);
        hit = hover && c->click;
        title_w = w - aw - 24;
    }
    ui_text_clipped(c->font, title, c->x, c->y, c->theme->group_label, title_w);
    c->y += c->line;
    DrawRectangle(c->x, c->y - 3, w, 1, c->theme->sidebar_border);
    gap(c, 1);
    return hit;
}

// Складная строка внутри раздела: стрелка ▸/▾, заголовок и приглушённая
// приписка. Ею сворачиваются дни ленты и сделанные задачи — то, чего со
// временем становится больше, чем нужно видеть разом.
static bool fold_row(Ctx *c, const char *title, const char *note, bool open,
                     Color title_color)
{
    Rect r = { c->x - 8, c->y - 3, content_width(c) + 16, c->line + 4 };
    bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
    if (hover) DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);
    font_draw_codepoint(c->font, open ? 0x25BE : 0x25B8, (float)c->x, (float)c->y,
                        (float)c->font->size, c->theme->group_label);
    int x = c->x + c->font->cell_width * 2;
    int w = ui_text_clipped(c->font, title, x, c->y, title_color,
                            content_width(c) - c->font->cell_width * 2);
    if (note && *note)
        ui_text_clipped(c->font, note, x + w + c->font->cell_width, c->y,
                        c->theme->row_text_dim,
                        c->x + content_width(c) - (x + w + c->font->cell_width));
    c->y += c->line + 2;
    return hover && c->click;
}

// Русский счёт по трём формам: один итог, два итога, пять итогов.
static const char *plural3(int n, const char *one, const char *few, const char *many)
{
    int a = n % 10, b = n % 100;
    if (b >= 11 && b <= 14) return many;
    if (a == 1) return one;
    if (a >= 2 && a <= 4) return few;
    return many;
}

// Русский счёт: один разговор, два разговора, пять разговоров.
static const char *plural_talks(int n)
{
    int tens = n % 100, ones = n % 10;
    if (tens >= 11 && tens <= 14) return "разговоров идёт";
    if (ones == 1) return "разговор идёт";
    if (ones >= 2 && ones <= 4) return "разговора идут";
    return "разговоров идёт";
}

// Текст в несколько строк, с переносом по словам.
//
// Ширину меряем шрифтом, а не считаем знакоместами: cell_width — целое число
// пикселей, а настоящий advance дробный, и на сотне символов накопленная
// разница выносит строку за край. Плюс символы из запасок (⌘, →, «») шириной
// с ячейкой не совпадают вовсе.
static int draw_wrapped(Ctx *c, const char *text, int x, int width, Color color,
                        bool draw)
{
    if (!text || !*text || width < c->font->cell_width * 4) return 0;
    int lines = 0;

    Font face = font_for(c->font, text);
    char line[JOURNAL_DETAIL_MAX];
    size_t used = 0;

    const char *p = text;
    while (*p) {
        // Следующее слово вместе с ведущим пробелом.
        const char *word = p;
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        size_t n = (size_t)(p - word);
        if (!n) break;

        char probe[JOURNAL_DETAIL_MAX];
        if (used + n >= sizeof(probe)) break;
        memcpy(probe, line, used);
        memcpy(probe + used, word, n);
        probe[used + n] = '\0';

        // Слово не влезло — печатаем накопленное и начинаем строку с него.
        if (used > 0 && MeasureTextEx(face, probe, (float)c->font->size, 0).x > (float)width) {
            line[used] = '\0';
            if (draw) {
                DrawTextEx(face, line, (Vector2){ (float)x, (float)c->y },
                           (float)c->font->size, 0, color);
                c->y += c->line;
            }
            lines++;

            while (*word == ' ') word++;
            n = (size_t)(p - word);
            used = 0;
        }

        if (used + n >= sizeof(line)) break;
        memcpy(line + used, word, n);
        used += n;
    }

    if (used > 0) {
        line[used] = '\0';
        if (draw) {
            DrawTextEx(face, line, (Vector2){ (float)x, (float)c->y },
                       (float)c->font->size, 0, color);
            c->y += c->line;
        }
        lines++;
    }
    return lines;
}

// Строка ленты: время слева, содержание справа. Цвет задаётся вызывающим —
// итог работы и реплика человека весят по-разному.
static bool list_row_colored(Ctx *c, const char *left, const char *right,
                             int left_w, Color color)
{
    // Сколько строк займёт содержание, знаем только отмерив: подсветка и
    // попадание мыши должны накрывать запись целиком, а не первую строку.
    int width = content_width(c) - left_w;
    int lines = draw_wrapped(c, right, 0, width, color, false);
    if (lines < 1) lines = 1;

    Rect r = { c->x - 8, c->y - 3, content_width(c) + 16, c->line * lines + 4 };
    bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
    if (hover)
        DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);

    ui_text_clipped(c->font, left, c->x, c->y, c->theme->row_text_dim, left_w);
    draw_wrapped(c, right, c->x + left_w, width, color, true);
    return hover && c->click;
}


// Попросить показать кусок страницы целиком — не сбрасывая прокрутку к концу.
static void reveal(Ctx *c, int top, int bottom)
{
    c->event.reveal = true;
    c->event.reveal_top = top;
    c->event.reveal_bottom = bottom;
}

// Что из раскрытого уже показано: просим показать один раз, при
// раскрытии, а не каждый кадр — иначе колесо не могло бы увести от него.
// Раскрытых на странице бывает несколько разом (задача, запись журнала,
// документ), поэтому память — список, а не одна ячейка: с одной ячейкой
// два раскрытых перебивали друг друга каждый кадр, и страница дёргалась,
// не давая докрутить до верха. Что за кадр не встретилось — свернули,
// и следующее раскрытие снова попросит показать себя.
#define REVEALED_MAX 8
static struct { char cwd[SESSION_PATH_MAX]; int sub, index; bool seen; }
    g_revealed[REVEALED_MAX];
static int g_revealed_n;

static void reveal_task_once(Ctx *c, const char *cwd, int sub, int index, int top)
{
    for (int i = 0; i < g_revealed_n; i++) {
        if (g_revealed[i].index == index && g_revealed[i].sub == sub
            && !strcmp(g_revealed[i].cwd, cwd)) {
            g_revealed[i].seen = true;
            return;
        }
    }
    int slot = g_revealed_n < REVEALED_MAX ? g_revealed_n++ : 0;
    snprintf(g_revealed[slot].cwd, sizeof(g_revealed[slot].cwd), "%s", cwd);
    g_revealed[slot].sub = sub;
    g_revealed[slot].index = index;
    g_revealed[slot].seen = true;
    reveal(c, top, c->y);
}

static void revealed_begin(void)
{
    for (int i = 0; i < g_revealed_n; i++) g_revealed[i].seen = false;
}

static void revealed_end(void)
{
    int n = 0;
    for (int i = 0; i < g_revealed_n; i++)
        if (g_revealed[i].seen) g_revealed[n++] = g_revealed[i];
    g_revealed_n = n;
}

static void set_event(Ctx *c, PageEventKind kind, int arg, const char *text_arg)
{
    c->event.kind = kind;
    c->event.arg = arg;
    c->event.arg2 = 0;
    c->event.cwd = c->list_cwd;
    c->event.sub = c->list_sub;
    snprintf(c->event.text, sizeof(c->event.text), "%s", text_arg ? text_arg : "");
}

// --- задачи ------------------------------------------------------------------

// Перетаскивание живёт между кадрами: началось на одном, кончится на другом.
// Состояние одно на окно, а не на вкладку: страница в кадре рисуется одна,
// и отпустить кнопку мыши можно только один раз.
static struct {
    int   index;     // какую задачу тянем (номер в файле), -1 — никакую
    float press_y;   // где нажали: до порога это ещё клик, не перетаскивание
    float grab_dy;   // насколько ниже верха строки взялись: плашка едет за
                     // курсором, не прыгая под него
    bool  moving;    // порог пройден — тянем
    int   slot;      // куда встанет среди несделанных, если отпустить сейчас
    int   sub;       // в каком списке: -1 — свои задачи, иначе подпроект
} g_drag = { .index = -1 };

// Дальше этого пальцы не дрожат: клик остаётся кликом, а не перестановкой
// на одну строку.
static const float DRAG_THRESHOLD = 6.0f;

// Середины строк несделанных задач в показанном порядке — как они лежали в
// прошлом кадре. По ним решается, куда тянутая задача перескочит: строки
// разной высоты (раскрытая тянет за собой описание), делением их не взять.
static int g_row_center[TASK_MAX];
static int g_row_n;

// Редактор задачи: название и описание, в один момент времени один на окно.
// Живёт между кадрами, как и перетаскивание. Привязан к каталогу: страницу
// другого проекта он не должен ни показывать, ни тем более сохранять в неё.
static struct {
    bool active;
    bool subproject;       // не задача, а новый подпроект: имя и о чём
    bool skill;            // не задача, а новый скилл: имя и когда применять
    char cwd[SESSION_PATH_MAX];
    int  index;            // какую задачу правим, -1 — новую
    int  field;            // 0 — название, 1 — описание
    int  cursor;           // байтовое смещение в текущем поле
    int  last_height;      // высота редактора в прошлом кадре: выросла —
                           // просим страницу показать его целиком
    char title[TASK_TITLE_MAX];
    char body[TASK_BODY_MAX];
    // Новый проект в группе: cwd — папка группы, group — её имя, notice —
    // отказ прошлой попытки (папка есть, имя плохое).
    bool project;
    char group[PROJECT_NAME_MAX];
    char notice[200];
} g_edit;

// Отложенный ход курсора по строкам: -1 вверх, 1 вниз, 0 — нет.
static int g_edit_move;

// Строка после переноса: байты [start, start + len) текста.
typedef struct { int start, len; } Seg;
// Строк переноса хватает на всё описание целиком: сегменты считаются от
// длины текста, и на 8 КБ при узком поле их набирается несколько сотен.
#define SEG_MAX 700

// Ширина символа тем шрифтом, каким его нарисует font_draw_codepoint(). Считать
// через MeasureTextEx нельзя: тот ищет глиф перебором атласа, а на описании
// в тысячу знаков это делается каждый кадр.
static float cp_advance(const FontAtlas *a, uint32_t cp)
{
    if (cp >= FONT_MAP_SIZE || a->map[cp].font < 0) return (float)a->cell_width;
    GlyphSlot s = a->map[cp];
    const Font *f = s.font == 0 ? &a->font : &a->fallback[s.font - 1];
    float scale = (float)a->size / (float)f->baseSize;
    int adv = f->glyphs[s.glyph].advanceX;
    return (adv ? (float)adv : f->recs[s.glyph].width) * scale;
}

// Режет текст на строки по ширине: перенос по словам, слово длиннее строки
// ломается по символам, '\n' начинает строку всегда. Пустой текст — одна
// пустая строка: курсору надо где-то стоять.
static int wrap_segments(const FontAtlas *a, const char *text, int width,
                         Seg *segs, int max)
{
    int n = 0;
    int len = (int)strlen(text);
    int pos = 0;
    while (n < max) {
        int start = pos;
        float w = 0;
        int last_space = -1;    // где можно порвать строку
        int cur = pos;
        int end = -1;           // конец строки, если нашли
        int next_pos = -1;      // с чего начинать следующую

        while (cur < len) {
            if (text[cur] == '\n') { end = cur; next_pos = cur + 1; break; }
            int sz = 0;
            int cp = GetCodepointNext(text + cur, &sz);
            if (sz <= 0) sz = 1;
            float cw = cp_advance(a, (uint32_t)cp);
            if (w + cw > (float)width && cur > start) {
                if (last_space >= 0) { end = last_space; next_pos = last_space + 1; }
                else                 { end = cur;        next_pos = cur; }
                break;
            }
            if (text[cur] == ' ') last_space = cur;
            w += cw;
            cur += sz;
        }
        if (end < 0) { end = len; next_pos = -1; }

        segs[n].start = start;
        segs[n].len = end - start;
        n++;
        if (next_pos < 0) break;
        pos = next_pos;
    }
    return n;
}

static float seg_width_to(const FontAtlas *a, const char *text, const Seg *s, int upto)
{
    float w = 0;
    for (int i = s->start; i < upto && i < s->start + s->len; ) {
        int sz = 0;
        int cp = GetCodepointNext(text + i, &sz);
        if (sz <= 0) sz = 1;
        w += cp_advance(a, (uint32_t)cp);
        i += sz;
    }
    return w;
}

// Ближайшая к x позиция в строке.
static int seg_offset_at(const FontAtlas *a, const char *text, const Seg *s, float x)
{
    float w = 0;
    int i = s->start;
    while (i < s->start + s->len) {
        int sz = 0;
        int cp = GetCodepointNext(text + i, &sz);
        if (sz <= 0) sz = 1;
        float cw = cp_advance(a, (uint32_t)cp);
        if (w + cw / 2 > x) return i;
        w += cw;
        i += sz;
    }
    return i;
}

static void seg_draw(Ctx *c, const char *text, const Seg *s, float x, float y, Color color)
{
    for (int i = s->start; i < s->start + s->len; ) {
        int sz = 0;
        int cp = GetCodepointNext(text + i, &sz);
        if (sz <= 0) sz = 1;
        font_draw_codepoint(c->font, (uint32_t)cp, x, y, (float)c->font->size, color);
        x += cp_advance(c->font, (uint32_t)cp);
        i += sz;
    }
}

static char *edit_buf(void)
{
    return g_edit.field == 0 ? g_edit.title : g_edit.body;
}

static size_t edit_cap(void)
{
    return g_edit.field == 0 ? sizeof(g_edit.title) : sizeof(g_edit.body);
}

static void edit_insert(const char *s, size_t n)
{
    char *buf = edit_buf();
    size_t len = strlen(buf), cap = edit_cap();
    if (len + n >= cap) n = cap - 1 - len;
    if (!n) return;
    if (g_edit.cursor > (int)len) g_edit.cursor = (int)len;
    memmove(buf + g_edit.cursor + n, buf + g_edit.cursor, len - (size_t)g_edit.cursor + 1);
    memcpy(buf + g_edit.cursor, s, n);
    g_edit.cursor += (int)n;
}

// Границы символов: назад — к началу предыдущего, вперёд — к началу
// следующего. Байты продолжения UTF-8 начинаются с 10xxxxxx.
static int prev_cp(const char *buf, int pos)
{
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && (buf[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static int next_cp(const char *buf, int pos)
{
    int len = (int)strlen(buf);
    if (pos >= len) return len;
    pos++;
    while (pos < len && (buf[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

static void edit_erase(int from, int to)
{
    char *buf = edit_buf();
    if (from >= to) return;
    memmove(buf + from, buf + to, strlen(buf + to) + 1);
    g_edit.cursor = from;
}

static void edit_begin(const char *cwd, int index, const Task *task)
{
    memset(&g_edit, 0, sizeof(g_edit));
    g_edit.active = true;
    snprintf(g_edit.cwd, sizeof(g_edit.cwd), "%s", cwd);
    g_edit.index = index;
    if (task) {
        snprintf(g_edit.title, sizeof(g_edit.title), "%s", task->title);
        snprintf(g_edit.body, sizeof(g_edit.body), "%s", task->body);
    }
    g_edit.field = 0;
    g_edit.cursor = (int)strlen(g_edit.title);
}

// Тот же редактор — для нового подпроекта: поля те же, смысл другой.
static void edit_begin_subproject(const char *cwd)
{
    edit_begin(cwd, -1, NULL);
    g_edit.subproject = true;
}

static void edit_begin_skill(const char *cwd)
{
    edit_begin(cwd, -1, NULL);
    g_edit.skill = true;
}

void page_new_project_begin(const char *root, const char *group)
{
    edit_begin(root, -1, NULL);
    g_edit.project = true;
    snprintf(g_edit.group, sizeof(g_edit.group), "%s", group ? group : "");
}

// Создать не вышло — редактор возвращается с тем же текстом и причиной:
// человек поправит имя, а не будет набирать всё заново.
void page_new_project_failed(const char *why)
{
    char title[TASK_TITLE_MAX], body[TASK_BODY_MAX], group[PROJECT_NAME_MAX], cwd[SESSION_PATH_MAX];
    snprintf(title, sizeof(title), "%s", g_edit.title);
    snprintf(body,  sizeof(body),  "%s", g_edit.body);
    snprintf(group, sizeof(group), "%s", g_edit.group);
    snprintf(cwd,   sizeof(cwd),   "%s", g_edit.cwd);
    page_new_project_begin(cwd, group);
    snprintf(g_edit.title, sizeof(g_edit.title), "%s", title);
    snprintf(g_edit.body,  sizeof(g_edit.body),  "%s", body);
    snprintf(g_edit.notice, sizeof(g_edit.notice), "%s", why ? why : "");
    g_edit.cursor = (int)strlen(g_edit.title);
}

bool page_overlay_active(void)
{
    return g_edit.active && g_edit.project;
}

static bool key_hit(int key)
{
    return IsKeyPressed(key) || IsKeyPressedRepeat(key);
}

static void rtrim_inplace(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

// Клавиатура редактора. Возвращает: 0 — ничего, 1 — сохранить, -1 — отменить.
static int edit_keys(void)
{
    bool cmd = input_mod_down(INPUT_MOD_SUPER);
    char *buf = edit_buf();

    if (IsKeyPressed(KEY_ESCAPE)) return -1;
    if (cmd && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) return 1;

    if (cmd && IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        if (clip && *clip) {
            // Возвраты каретки из чужих буферов не нужны; в названии —
            // и переводы строк: оно одной строкой.
            char tmp[TASK_BODY_MAX];
            size_t n = 0;
            for (const char *p = clip; *p && n + 1 < sizeof(tmp); p++) {
                if (*p == '\r') continue;
                tmp[n++] = (*p == '\n' && g_edit.field == 0) ? ' ' : *p;
            }
            edit_insert(tmp, n);
        }
        input_drain_chars();
        return 0;
    }
    if (cmd) return 0;   // остальные сочетания — приложению

    int cp;
    while ((cp = GetCharPressed()) != 0) {
        if (cp < 32) continue;
        char u[4];
        int n = utf8_encode((uint32_t)cp, u);
        edit_insert(u, (size_t)n);
    }

    if (key_hit(KEY_BACKSPACE)) edit_erase(prev_cp(buf, g_edit.cursor), g_edit.cursor);
    if (key_hit(KEY_DELETE))    edit_erase(g_edit.cursor, next_cp(buf, g_edit.cursor));
    if (key_hit(KEY_LEFT))      g_edit.cursor = prev_cp(buf, g_edit.cursor);
    if (key_hit(KEY_RIGHT))     g_edit.cursor = next_cp(buf, g_edit.cursor);
    if (key_hit(KEY_HOME))      g_edit.cursor = 0;
    if (key_hit(KEY_END))       g_edit.cursor = (int)strlen(buf);

    if (IsKeyPressed(KEY_TAB)) {
        g_edit.field = !g_edit.field;
        g_edit.cursor = (int)strlen(edit_buf());
    }
    if (key_hit(KEY_ENTER) || key_hit(KEY_KP_ENTER)) {
        if (g_edit.field == 0) {
            g_edit.field = 1;
            g_edit.cursor = (int)strlen(g_edit.body);
        } else {
            edit_insert("\n", 1);
        }
    }

    // Вверх и вниз — по видимым строкам описания, как они лежали в прошлом
    // кадре, с сохранением горизонтальной позиции.
    if (g_edit.field == 1 && (key_hit(KEY_UP) || key_hit(KEY_DOWN))) {
        // Ближайшую позицию в соседней строке ищет edit_field(): там есть и
        // строки после переноса, и шрифт, чтобы мерить ширину.
        g_edit_move = key_hit(KEY_UP) ? -1 : 1;
    } else if (g_edit.field == 0 && key_hit(KEY_DOWN)) {
        g_edit.field = 1;
        g_edit.cursor = 0;
    }
    return 0;
}

// Поле ввода. Многострочное растёт с текстом, но не ниже трёх строк —
// чтобы пустое описание не выглядело щелью.
static void edit_field(Ctx *c, int field, char *buf, bool multiline,
                       const char *placeholder, int x, int width)
{
    int inner_w = width - 16;
    Seg segs[SEG_MAX];
    int n = wrap_segments(c->font, buf, inner_w, segs, SEG_MAX);
    int lines = multiline ? (n < 3 ? 3 : n) : 1;
    Rect r = { x, c->y, width, c->line * lines + 12 };
    bool focused = g_edit.field == field;

    DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_active_bg);
    DrawRectangleLines(r.x, r.y, r.w, r.h,
                       focused ? c->theme->group_label : c->theme->sidebar_border);

    // Клик ставит курсор: строка по y, позиция по x.
    if (c->click && inside(r, c->mouse) && visible_hit(c, c->mouse)) {
        g_edit.field = field;
        int line = (int)((c->mouse.y - (float)(r.y + 6)) / (float)c->line);
        if (line < 0) line = 0;
        if (line > n - 1) line = n - 1;
        g_edit.cursor = seg_offset_at(c->font, buf, &segs[line],
                                      c->mouse.x - (float)(x + 8));
        focused = true;
    }

    // Стрелки вверх-вниз, отложенные из edit_keys(): здесь есть и строки,
    // и шрифт, чтобы найти ближайшую позицию в соседней строке.
    if (focused && multiline && g_edit_move) {
        for (int i = 0; i < n; i++) {
            const Seg *s = &segs[i];
            if (g_edit.cursor < s->start || g_edit.cursor > s->start + s->len) continue;
            int target = i + g_edit_move;
            if (target >= 0 && target < n) {
                float px = seg_width_to(c->font, buf, s, g_edit.cursor);
                g_edit.cursor = seg_offset_at(c->font, buf, &segs[target], px);
            } else if (target < 0) {
                g_edit.field = 0;
                g_edit.cursor = (int)strlen(g_edit.title);
            }
            break;
        }
        g_edit_move = 0;
    }

    float ty = (float)(r.y + 6);
    if (!buf[0]) {
        ui_text_clipped(c->font, placeholder, x + 8, r.y + 6, c->theme->row_text_dim, inner_w);
    } else {
        for (int i = 0; i < n; i++) {
            seg_draw(c, buf, &segs[i], (float)(x + 8), ty, c->theme->row_text);
            ty += (float)c->line;
        }
    }

    // Курсор: в своей строке, мигает раз в полсекунды.
    if (focused && ((int)(GetTime() * 2) & 1) == 0) {
        int line = 0;
        float px = 0;
        for (int i = 0; i < n; i++) {
            const Seg *s = &segs[i];
            if (g_edit.cursor >= s->start && g_edit.cursor <= s->start + s->len) {
                line = i;
                px = seg_width_to(c->font, buf, s, g_edit.cursor);
                // Курсор на границе переноса принадлежит следующей строке,
                // если стоит за пробелом, по которому перенесли.
                if (g_edit.cursor == s->start + s->len && i + 1 < n
                    && segs[i + 1].start == g_edit.cursor) { line = i + 1; px = 0; }
                break;
            }
        }
        DrawRectangle(x + 8 + (int)px, r.y + 6 + line * c->line, 2, c->line - 2,
                      c->theme->row_text);
    }

    c->y += r.h + 8;
}

// Редактор целиком: два поля и кнопки. Возвращает true, если задачу
// сохранили — событие уже выставлено.
static void draw_editor(Ctx *c, int indent)
{
    int x = c->x + indent;
    int width = content_width(c) - indent;

    int action = edit_keys();
    int top = c->y;

    gap(c, 1);
    if (g_edit.project) {
        edit_field(c, 0, g_edit.title, false, "Имя папки проекта", x, width);
        edit_field(c, 1, g_edit.body, true, "О чём — одной строкой, попадёт в паспорт", x, width);
    } else if (g_edit.subproject) {
        edit_field(c, 0, g_edit.title, false, "Имя папки подпроекта", x, width);
        edit_field(c, 1, g_edit.body, true, "О чём — одной строкой, попадёт в паспорт", x, width);
    } else if (g_edit.skill) {
        edit_field(c, 0, g_edit.title, false, "Имя скилла — имя папки, без пробелов", x, width);
        edit_field(c, 1, g_edit.body, true, "Когда применять — по этой фразе агент решит, что скилл нужен", x, width);
    } else {
        edit_field(c, 0, g_edit.title, false, "Название задачи", x, width);
        edit_field(c, 1, g_edit.body, true, "Описание — Enter для новой строки, Tab между полями", x, width);
    }

    row_begin(c);
    c->row_x = x;
    if (button(c, "Сохранить  ⌘↩", true)) action = 1;
    if (button(c, "Отмена  esc", false)) action = -1;
    row_end(c);

    // Редактор открылся или подрос — пусть страница покажет его целиком:
    // он стоит в самом низу, и растёт он за край окна.
    if (c->y - top != g_edit.last_height) {
        g_edit.last_height = c->y - top;
        reveal(c, top, c->y);
    }

    if (action == 1) {
        rtrim_inplace(g_edit.title);
        rtrim_inplace(g_edit.body);
        if (!g_edit.title[0]) {
            // Без названия задачи нет: нечего показывать в списке.
            g_edit.field = 0;
            return;
        }
        if (g_edit.project) {
            for (char *q = g_edit.body; *q; q++) if (*q == '\n') *q = ' ';
            set_event(c, PAGE_EVENT_NEW_PROJECT, 0, g_edit.group);
            c->event.cwd = g_edit.cwd;
        } else if (g_edit.subproject || g_edit.skill) {
            // Описание — одна строка: переводы строк — в пробелы.
            for (char *q = g_edit.body; *q; q++) if (*q == '\n') *q = ' ';
            set_event(c, g_edit.skill ? PAGE_EVENT_NEW_SKILL : PAGE_EVENT_NEW_SUBPROJECT, 0, NULL);
        } else {
            set_event(c, PAGE_EVENT_TODO_SAVE, g_edit.index, NULL);
        }
        c->event.title = g_edit.title;
        c->event.body  = g_edit.body;
        g_edit.active = false;
    } else if (action == -1) {
        g_edit.active = false;
    }
}

// Подпись строки задачи: рукоятка, номер, название.
static void task_label(char *out, size_t cap, const Task *task, int number)
{
    if (task->state == TASK_DONE)
        snprintf(out, cap, "≡  ✓  %s", task->title);
    else if (task->state == TASK_REVIEW)
        snprintf(out, cap, "≡  ?  %s", task->title);
    else
        snprintf(out, cap, "≡ %2d. %s", number, task->title);
}

// Строка задачи. Возвращает её прямоугольник — по нему решается, за что
// взялась мышь. placeholder — пустое место под тянутую задачу: она сейчас
// едет за курсором отдельной плашкой.
static Rect task_row(Ctx *c, const Task *task, int number, bool open, bool placeholder)
{
    Rect r = { c->x - 8, c->y - 3, content_width(c) + 16, c->line + 6 };
    bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse)
              && g_drag.index < 0 && !g_edit.active;

    if (placeholder) {
        DrawRectangleLines(r.x, r.y, r.w, r.h, c->theme->sidebar_border);
        c->y += c->line + 6;
        return r;
    }

    char label[TASK_TITLE_MAX + 16];
    task_label(label, sizeof(label), task, number);
    int width = content_width(c);
    int hint_w = 0;
    if (task->state == TASK_REVIEW) {
        hint_w = c->font->cell_width * 9;
        width -= hint_w + c->font->cell_width;
    }
    Color tone = task->state == TASK_DONE ? c->theme->row_text_dim : c->theme->row_text;

    // Свёрнутая строка — одной строкой, длинное обрезается: список должен
    // оставаться списком. Раскрытая показывает название целиком, с
    // переносом: в узкой колонке половина названий не влезала, а клик —
    // это как раз просьба прочесть.
    int lines = open ? draw_wrapped(c, label, 0, width, tone, false) : 1;
    if (lines < 1) lines = 1;
    r.h = c->line * lines + 6;

    if (hover || open)
        DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);
    if (hint_w) {
        // Пометка справа, тем же цветом, что «агент зовёт»: задача ждёт
        // человека, а не агента.
        ui_text_clipped(c->font, "проверить", c->x + content_width(c) - hint_w, c->y,
                        c->theme->badge_attention, hint_w);
    }
    if (open) draw_wrapped(c, label, c->x, width, tone, true);
    else {
        ui_text_clipped(c->font, label, c->x, c->y, tone, width);
        c->y += c->line;
    }
    c->y += 6;
    return r;
}

// Плашка, которую тянут: чуть повёрнута и с тенью, будто приподнята над
// списком. Рисуется последней, поверх всего, — она и есть «верхний слой».
static void draw_ghost(Ctx *c, const Task *task, int number, Rect r)
{
    const float angle = 1.0f;
    Rectangle rec = { (float)r.x + r.w / 2.0f, (float)r.y + r.h / 2.0f, (float)r.w, (float)r.h };
    Vector2 origin = { r.w / 2.0f, r.h / 2.0f };

    // Тень: несколько слоёв с убывающей плотностью — мягкий край без
    // размытия, которого у raylib нет.
    for (int i = 3; i >= 1; i--) {
        Rectangle sh = { rec.x, rec.y + 3.0f + (float)i, rec.width + (float)i * 4, rec.height + (float)i * 4 };
        Vector2 so = { sh.width / 2.0f, sh.height / 2.0f };
        DrawRectanglePro(sh, so, angle, (Color){ 0, 0, 0, (unsigned char)(10 * (4 - i)) });
    }
    DrawRectanglePro(rec, origin, angle, c->theme->row_active_bg);
    Rectangle bar = { rec.x, rec.y, 3, rec.height };
    Vector2 bo = { r.w / 2.0f, r.h / 2.0f };
    DrawRectanglePro(bar, bo, angle, c->theme->group_label);

    char label[TASK_TITLE_MAX + 16];
    task_label(label, sizeof(label), task, number);
    Font face = font_for(c->font, label);
    // Точка вращения — центр плашки; текст стоит от неё на том же месте,
    // что и в строке списка.
    Vector2 to = { r.w / 2.0f - 8.0f, r.h / 2.0f - 3.0f };
    DrawTextPro(face, label, (Vector2){ rec.x, rec.y }, to, angle,
                (float)c->font->size, 0, c->theme->row_text);
}

// Описание задачи с отступом под название. Абзацы — как в Markdown: строки
// подряд склеиваются в один, пустая строка начинает следующий. В файле
// текст набран с переносами по ширине редактора, а на странице ширина своя.
static void body_text(Ctx *c, const char *body, int indent, Color color);

static void task_body(Ctx *c, const char *body, int indent)
{
    body_text(c, body, indent, c->theme->row_text_dim);
}

// Текст абзацами: одиночный перевод строки склеивается, как в Markdown,
// пустая строка отбивает абзац.
static void body_text(Ctx *c, const char *body, int indent, Color color)
{
    if (!body[0]) return;
    gap(c, 1);
    char para[TASK_BODY_MAX];
    size_t used = 0;
    const char *p = body;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        bool blank = n == 0;

        if (!blank && used + n + 1 < sizeof(para)) {
            if (used) para[used++] = ' ';
            memcpy(para + used, p, n);
            used += n;
        }
        if ((blank || !nl) && used) {
            para[used] = '\0';
            draw_wrapped(c, para, c->x + indent, content_width(c) - indent,
                         color, true);
            used = 0;
            if (blank) gap(c, 1);
        }
        if (!nl) break;
        p = nl + 1;
    }
    gap(c, 1);
}

// Заголовок раздела задач: сколько открытых и сколько ждут проверки.
static void tasks_title(const TaskList *tl, const char *prefix, char *out, size_t cap)
{
    int open_n = tasks_count_in(tl, TASK_OPEN);
    int review_n = tasks_count_in(tl, TASK_REVIEW);
    if (review_n > 0)
        snprintf(out, cap, "%s · %d · проверить %d", prefix, open_n, review_n);
    else if (open_n > 0)
        snprintf(out, cap, "%s · %d", prefix, open_n);
    else
        snprintf(out, cap, "%s", prefix);
}

// Список задач одного каталога: свои задачи проекта или задачи подпроекта.
// cwd — чей список, sub — номер подпроекта (-1 у своих): по ним события
// находят нужный файл, а раскрытая задача — свой список.
static void draw_tasks(Ctx *c, const Session *s, const TaskList *tl,
                       const char *cwd, int sub)
{
    c->list_cwd = sub >= 0 ? cwd : NULL;
    c->list_sub = sub;

    // Редактор задачи этого каталога. Чужой (другого списка на той же
    // странице или редактор подпроекта) здесь не показываем, но и не
    // закрываем: он нарисуется в своём разделе.
    bool editing = g_edit.active && !g_edit.subproject && !g_edit.skill && !g_edit.project
                && !strcmp(g_edit.cwd, cwd);

    bool down     = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool released = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

    // Кнопку отпустили мимо нас — окно, панель, другая вкладка. Тянуть
    // больше нечего.
    if (g_drag.index >= 0 && !down && !released) g_drag.index = -1;
    // Перетаскивание из другого списка на этой же странице — не наше.
    bool ours = g_drag.index < 0 || g_drag.sub == sub;
    if (ours && g_drag.index >= 0 && down && !g_drag.moving
        && (c->mouse.y - g_drag.press_y > DRAG_THRESHOLD
            || g_drag.press_y - c->mouse.y > DRAG_THRESHOLD))
        g_drag.moving = true;

    // Несделанные идут первыми, в порядке файла; сделанные — приглушённо
    // в конце. Тянуть можно только несделанные: у сделанных порядка нет.
    int order[TASK_MAX];
    int n = 0;
    for (int i = 0; i < tl->count; i++)
        if (tl->items[i].state == TASK_OPEN) order[n++] = i;

    // Порядок до перестановки: место назначения считается по нему. В
    // показанном порядке тянутая задача уже стоит на своём слоте, и спросить
    // «кто там был» по нему нельзя.
    int base[TASK_MAX];
    memcpy(base, order, sizeof(int) * (size_t)n);

    // Пока тянем, страница не меняет высоту: раскрытые описания и кнопка
    // внизу остаются на месте, иначе предел прокрутки сдвигается и список
    // уезжает из-под курсора в момент захвата.
    //
    // Место назначения — перескок через середину соседней строки, по
    // положению строк в прошлом кадре. Именно через середину: после
    // перескока сосед сдвигается на высоту строки, его середина уходит от
    // курсора, и обратно задача не прыгает.
    bool dragging = ours && g_drag.index >= 0 && g_drag.moving;
    if (dragging) {
        int from = -1;
        for (int j = 0; j < n; j++) if (order[j] == g_drag.index) from = j;
        if (from >= 0) {
            int slot = g_drag.slot;
            if (slot < 0 || slot >= n) slot = from;
            int m = g_row_n < n ? g_row_n : n;
            while (slot + 1 < m && c->mouse.y > (float)g_row_center[slot + 1]) slot++;
            while (slot - 1 >= 0 && c->mouse.y < (float)g_row_center[slot - 1]) slot--;
            g_drag.slot = slot;

            // Показываем список таким, каким он станет: тянутая задача стоит
            // на месте назначения, остальные раздвинулись.
            int moved = order[from];
            memmove(&order[from], &order[from + 1], sizeof(int) * (size_t)(n - from - 1));
            memmove(&order[slot + 1], &order[slot], sizeof(int) * (size_t)(n - slot - 1));
            order[slot] = moved;
        }
    }
    // Середины строк помнит тот список, в котором тянут; пока не тянут —
    // последний нарисованный, и нажатие в нём успевает получить свои.
    if (ours) g_row_n = n;

    int open_idx = s->page_task_sub == sub ? s->page_task_open - 1 : -1;
    int indent = c->font->cell_width * 6;
    Rect ghost_rect = { 0 };
    int  ghost_number = 0;

    // Сделанное агентом, но ещё не проверенное — первым: это ждёт человека,
    // а очередь ниже ждёт агента. Порядка у таких задач нет, тянуть нечего.
    bool first_review = true;
    for (int i = 0; i < tl->count; i++) {
        const Task *task = &tl->items[i];
        if (task->state != TASK_REVIEW) continue;
        bool open = open_idx == i;
        Rect r = task_row(c, task, 0, open, false);
        if (c->click && !editing && inside(r, c->mouse) && visible_hit(c, c->mouse))
            set_event(c, PAGE_EVENT_TODO_TOGGLE, i, NULL);
        first_review = false;

        if (editing && g_edit.index == i) {
            draw_editor(c, indent);
            continue;
        }
        if (!open) continue;
        task_body(c, task->body, indent);
        row_begin(c);
        c->row_x = c->x + indent;
        if (button(c, "Сделано", true)) {
            set_event(c, PAGE_EVENT_TODO_STATE, i, NULL);
            c->event.arg2 = TASK_DONE;
        }
        if (button(c, "Вернуть в работу", false)) {
            set_event(c, PAGE_EVENT_TODO_STATE, i, NULL);
            c->event.arg2 = TASK_OPEN;
        }
        if (button(c, "Изменить", false))
            edit_begin(cwd, i, task);
        row_end(c);
        reveal_task_once(c, cwd, sub, i, r.y);
    }
    if (!first_review && n > 0) gap(c, 1);

    for (int j = 0; j < n; j++) {
        int i = order[j];
        const Task *task = &tl->items[i];
        bool open = open_idx == i;
        bool lifted = dragging && i == g_drag.index;
        Rect r = task_row(c, task, j + 1, open, lifted);
        if (ours) g_row_center[j] = r.y + r.h / 2;
        if (lifted) {
            ghost_rect = r;
            ghost_rect.y = (int)(c->mouse.y - g_drag.grab_dy);
            ghost_number = j + 1;
        }

        // Нажали на строку — пока это кандидат: клик или перетаскивание,
        // решит движение мыши.
        if (c->click && !editing && inside(r, c->mouse) && visible_hit(c, c->mouse)) {
            g_drag.index = i;
            g_drag.sub = sub;
            g_drag.press_y = c->mouse.y;
            g_drag.grab_dy = c->mouse.y - (float)r.y;
            g_drag.moving = false;
            g_drag.slot = j;
        }

        if (editing && g_edit.index == i) {
            draw_editor(c, indent);
            continue;
        }
        if (!open) continue;
        task_body(c, task->body, indent);
        row_begin(c);
        c->row_x = c->x + indent;
        if (button(c, "Отправить в работу", true))
            set_event(c, PAGE_EVENT_TODO_SEND, i, NULL);
        if (button(c, "Изменить", false))
            edit_begin(cwd, i, task);
        if (button(c, "Сделано", false)) {
            set_event(c, PAGE_EVENT_TODO_STATE, i, NULL);
            c->event.arg2 = TASK_DONE;
        }
        row_end(c);
        reveal_task_once(c, cwd, sub, i, r.y);
    }

    // Отпустили. Тянули — переставляем, иначе это был клик: раскрыть.
    if (ours && released && g_drag.index >= 0) {
        if (g_drag.moving) {
            int to = base[g_drag.slot];
            if (to != g_drag.index) {
                set_event(c, PAGE_EVENT_TODO_MOVE, g_drag.index, NULL);
                c->event.arg2 = to;
            }
        } else {
            set_event(c, PAGE_EVENT_TODO_TOGGLE, g_drag.index, NULL);
        }
        g_drag.index = -1;
    }

    // Новая задача заводится тут же, под списком: название и описание.
    if (editing && g_edit.index < 0) {
        draw_editor(c, 0);
    } else {
        gap(c, 1);
        row_begin(c);
        if (button(c, "Новая задача", false))
            edit_begin(cwd, -1, NULL);
        row_end(c);
    }

    // Сделанные — под складной строкой, по умолчанию свёрнуты: их со
    // временем больше, чем открытых, и они не про работу. Совсем убирать
    // нельзя — иначе не вернуть, если отметили сгоряча.
    int done_n = tasks_count_in(tl, TASK_DONE);
    bool show_done = done_n > 0 && ((s->page_done_open >> (sub + 1)) & 1u);
    if (done_n > 0) {
        gap(c, 1);
        char head[48];
        snprintf(head, sizeof(head), "Сделано · %d", done_n);
        if (fold_row(c, head, NULL, show_done, c->theme->row_text_dim))
            set_event(c, PAGE_EVENT_TODO_DONE_TOGGLE, 0, NULL);
    }
    for (int i = 0; show_done && i < tl->count; i++) {
        const Task *task = &tl->items[i];
        if (task->state != TASK_DONE) continue;

        bool open = open_idx == i;
        Rect r = task_row(c, task, 0, open, false);
        if (c->click && !editing && inside(r, c->mouse) && visible_hit(c, c->mouse))
            set_event(c, PAGE_EVENT_TODO_TOGGLE, i, NULL);

        if (!open) continue;
        task_body(c, task->body, indent);
        row_begin(c);
        c->row_x = c->x + indent;
        if (button(c, "Вернуть в работу", false)) {
            set_event(c, PAGE_EVENT_TODO_STATE, i, NULL);
            c->event.arg2 = TASK_OPEN;
        }
        row_end(c);
        reveal_task_once(c, cwd, sub, i, r.y);
    }

    if (tl->partial)
        text(c, "…в файле есть ещё задачи, они не поместились", c->theme->row_text_dim);

    if (ghost_rect.w > 0)
        draw_ghost(c, &tl->items[g_drag.index], ghost_number, ghost_rect);

    c->list_cwd = NULL;
    c->list_sub = -1;
}

// --- страница проекта --------------------------------------------------------

// Заголовок раздела, по которому можно кликнуть: со стрелкой ▸/▾.
// Возвращает true, если по нему нажали.
static bool section_toggle(Ctx *c, const char *title, bool open)
{
    gap(c, 2);
    Rect r = { c->x - 8, c->y - 3, content_width(c) + 16, c->line + 6 };
    bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
    if (hover) DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);
    font_draw_codepoint(c->font, open ? 0x25BE : 0x25B8, (float)c->x, (float)c->y,
                        (float)c->font->size, c->theme->group_label);
    ui_text_clipped(c->font, title, c->x + c->font->cell_width * 2, c->y,
                    c->theme->group_label, content_width(c) - c->font->cell_width * 2);
    c->y += c->line;
    DrawRectangle(c->x, c->y - 3, content_width(c), 1, c->theme->sidebar_border);
    gap(c, 1);
    return hover && c->click;
}

// Задачи подпроектов — разделами под своими, свёрнутыми: у родителя они
// нужны реже своих, а подпроектов бывает десяток. Состояние подпроекта
// читается только у раскрытого: кэш набора невелик, и грузить все разом
// значило бы вытеснить оттуда то, с чем работают.
static void draw_subprojects(Ctx *c, const Session *s, const ProjectList *projects)
{
    int q = 0;
    for (int k = 0; projects && k < projects->count; k++) {
        const Project *sub = &projects->items[k];
        if (sub->parent < 0 || strcmp(projects->items[sub->parent].path, s->cwd)) continue;
        int idx = q++;
        if (idx >= 32) break;   // битов в page_subs_open

        bool open = (s->page_subs_open >> idx) & 1u;
        char prefix[PROJECT_NAME_MAX + 24];
        snprintf(prefix, sizeof(prefix), "Подпроект %s", sub->name);

        char title[PROJECT_NAME_MAX + 64];
        const ProjectState *sst = open ? projstate_get(sub->path) : projstate_peek(sub->path);
        if (sst) tasks_title(&sst->tasks, prefix, title, sizeof(title));
        else     snprintf(title, sizeof(title), "%s", prefix);

        if (section_toggle(c, title, open))
            set_event(c, PAGE_EVENT_TOGGLE_SUB, idx, sub->name);
        if (!open || !sst) continue;

        draw_tasks(c, s, &sst->tasks, sub->path, idx);
        row_begin(c);
        if (button(c, "Убрать из проекта", false))
            set_event(c, PAGE_EVENT_HIDE_SUBPROJECT, idx, sub->name);
        row_end(c);
    }

    gap(c, 1);
    if (g_edit.active && g_edit.subproject && !strcmp(g_edit.cwd, s->cwd)) {
        draw_editor(c, 0);
        return;
    }
    row_begin(c);
    // «Новый» заводит папку с паспортом — как newproj.sh, но внутри проекта.
    // «Добавить» — для папки, которая уже есть, но признаков не имеет.
    if (button(c, "Новый подпроект", false))
        edit_begin_subproject(s->cwd);
    if (button(c, "Добавить подпроект", false))
        set_event(c, PAGE_EVENT_ADD_SUBPROJECT, 0, NULL);
    row_end(c);
}

// Путь родителя, если это подпроект, иначе NULL.
static const char *parent_of(const ProjectList *projects, const char *cwd)
{
    if (!projects) return NULL;
    int i = projects_find_by_path(projects, cwd);
    if (i < 0 || projects->items[i].parent < 0) return NULL;
    return projects->items[projects->items[i].parent].path;
}

// Скиллы проекта — строкой на скилл: имя, описание из шапки. Клик
// раскрывает первый абзац SKILL.md — чтобы вспомнить, что он делает; читать
// и править целиком — дело редактора, для этого «Изменить». Своих скиллов
// у проекта единицы, поэтому без групп: у унаследованного пометка «от …».
// Скиллы берта и личные здесь не показываются: первые едут с каждым
// запуском claude сами, вторые действуют везде, — им строки ни к чему.
static void draw_skills(Ctx *c, const Session *s, const ProjectList *projects)
{
    const SkillList *sl = projstate_skills(s->cwd, parent_of(projects, s->cwd), false);
    if (!sl) return;

    char title[64];
    snprintf(title, sizeof(title), sl->count ? "Скиллы проекта · %d" : "Скиллы проекта", sl->count);
    section(c, title);

    const int cw = c->font->cell_width;
    int open_idx = s->page_skill_open - 1;
    for (int i = 0; i < sl->count; i++) {
        const Skill *sk = &sl->items[i];
        bool open = open_idx == i;
        Rect r = { c->x - 8, c->y - 3, content_width(c) + 16, c->line + 6 };
        bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
        if (hover || open) DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);

        font_draw_codepoint(c->font, open ? 0x25BE : 0x25B8, (float)c->x, (float)c->y,
                            (float)c->font->size, c->theme->row_text_dim);
        int x = c->x + cw * 2;
        int nw = ui_text_clipped(c->font, sk->name, x, c->y, c->theme->row_text, cw * 28);
        x += nw + cw;

        char line_text[SKILL_DESC_MAX + PROJECT_NAME_MAX + 16];
        if (sk->kind == SKILL_INHERITED)
            snprintf(line_text, sizeof(line_text), "от %s · %s", sk->from, sk->desc);
        else
            snprintf(line_text, sizeof(line_text), "%s", sk->desc);
        ui_text_clipped(c->font, line_text, x, c->y, c->theme->row_text_dim,
                        c->x + content_width(c) - x);
        if (hover && c->click) set_event(c, PAGE_EVENT_SKILL_TOGGLE, i, sk->name);
        c->y += c->line;

        if (!open) continue;
        int indent = cw * 2;
        if (sk->intro[0]) body_text(c, sk->intro, indent, c->theme->row_text);
        else              text(c, "В SKILL.md нет вводного абзаца", c->theme->row_text_dim);
        row_begin(c);
        c->row_x = c->x + indent;
        if (sk->kind == SKILL_OWN && button(c, "Изменить", true))
            set_event(c, PAGE_EVENT_SKILL_EDIT, i, sk->name);
        if (button(c, "Папка", false))
            set_event(c, PAGE_EVENT_SKILL_OPEN, i, sk->name);
        row_end(c);
    }
    if (sl->count == 0)
        text(c, "Своих скиллов у проекта нет", c->theme->row_text_dim);

    gap(c, 1);
    if (g_edit.active && g_edit.skill && !strcmp(g_edit.cwd, s->cwd)) {
        draw_editor(c, 0);
    } else {
        row_begin(c);
        if (button(c, "Новый скилл", false))
            edit_begin_skill(s->cwd);
        row_end(c);
    }

    char foot[600];
    snprintf(foot, sizeof(foot),
             "Скилл — процедура с условием запуска: живёт в .claude/skills и уезжает с кодом, "
             "подпроекты его наследуют. Скиллы берта — в настройках (⌘,)%s",
             sl->personal_count > 0 ? "; личные из ~/.claude/skills действуют везде." : ".");
    body_text(c, foot, 0, c->theme->row_text_dim);
}

// Документы проекта — реестр .berth/files.tsv: имя, пояснение, давность.
// Раздел отвечает на «что это за файл» без открытия файла; открыть —
// клик, показать в Finder — «Папка» под курсором. Пропавший файл виден
// приглушённым: реестр устарел, и это должно быть видно, а не спрятано.
static void draw_files(Ctx *c, const Session *s, const FileList *fl)
{
    // Раздел есть у любого проекта, где нашлись документы: реестр — лишь
    // пояснения к ним, а сами файлы берт находит обходом папки.
    if (!fl || (!fl->exists && fl->undescribed <= 0)) return;

    char title[64];
    snprintf(title, sizeof(title), fl->count ? "Документы · %d" : "Документы", fl->count);
    section(c, title);

    const int cw = c->font->cell_width;
    for (int i = 0; i < fl->count; i++) {
        const FileEntry *e = &fl->items[i];
        bool open = s->page_file_open == i + 1;
        Rect r = { c->x - 8, c->y - 3, content_width(c) + 16, c->line * (open ? 1 : 2) + 6 };
        bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
        if (hover) DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);
        int row_top = r.y;

        Color tone = e->exists ? c->theme->row_text : c->theme->row_text_dim;
        const char *base = strrchr(e->path, '/');
        base = base ? base + 1 : e->path;

        // Справа давность; у пропавшего файла — пометка вместо неё.
        char age[32];
        if (e->exists) projinfo_age(e->mtime, age, sizeof(age));
        else           snprintf(age, sizeof(age), "нет файла");
        int age_w = chars_of(age) * cw;
        int right = c->x + content_width(c);
        ui_text_clipped(c->font, age, right - age_w, c->y,
                        e->exists ? c->theme->row_text_dim : c->theme->badge_attention, age_w);
        right -= age_w;
        ui_text_clipped(c->font, base, c->x, c->y, tone, right - c->x - cw);
        c->y += c->line;

        // Свёрнутый — одна строка пояснения, сколько влезет; клик
        // раскрывает: пояснение целиком, под ним путь и кнопки.
        if (hover && c->click) set_event(c, PAGE_EVENT_FILE_TOGGLE, i, NULL);
        if (!open) {
            ui_text_clipped(c->font, e->note, c->x + cw * 2, c->y, c->theme->row_text_dim,
                            content_width(c) - cw * 2);
            c->y += c->line;
        } else {
            body_text(c, e->note, cw * 2, c->theme->row_text);
            if (base != e->path) {
                gap(c, 1);
                ui_text_clipped(c->font, e->path, c->x + cw * 2, c->y, c->theme->row_text_dim,
                                content_width(c) - cw * 2);
                c->y += c->line;
            }
            gap(c, 1);
            if (e->exists) {
                row_begin(c);
                c->row_x = c->x + cw * 2;
                if (button(c, "Открыть", true)) set_event(c, PAGE_EVENT_FILE_OPEN, i, NULL);
                if (button(c, "Папка", false))  set_event(c, PAGE_EVENT_FILE_REVEAL, i, NULL);
                row_end(c);

                // Таблицу можно показывать прямо на странице — тогда её не
                // надо открывать, чтобы вспомнить, что в ней. Мест немного:
                // страница не витрина файлов, а рабочее место.
                if (files_is_table(e->path)) {
                    bool on = files_shown(fl, e->path);
                    bool room = on || fl->shown_count < FILES_SHOWN_MAX;
                    row_begin(c);
                    c->row_x = c->x + cw * 2;
                    const char *label = "Показывать на странице";
                    int lw = chars_of(label) * cw;
                    ui_text_clipped(c->font, label, c->row_x, c->y + 6,
                                    c->theme->row_text_dim, lw);
                    c->row_x += lw + cw;
                    if (room) {
                        if (toggle(c, on)) set_event(c, PAGE_EVENT_FILE_SHOW, i, NULL);
                    } else {
                        ui_text_clipped(c->font, "уже показаны две", c->row_x, c->y + 6,
                                        c->theme->row_text_dim, cw * 20);
                    }
                    row_end(c);
                }
            } else {
                text(c, "файла на месте нет — строку реестра пора убрать", c->theme->row_text_dim);
            }
            reveal_task_once(c, s->cwd, -3, i, row_top);
        }
        gap(c, 0);
        c->y += 4;
    }
    if (fl->count == 0 && fl->exists)
        text(c, "Реестр пуст", c->theme->row_text_dim);
    if (fl->partial)
        text(c, "В реестре строк больше, чем показано", c->theme->row_text_dim);

    // Документы без пояснения — то, что положили руками или агент не
    // записал. Свёрнуто: важно число; имена — по клику, у каждого
    // «Описать», под списком «Описать все». Обход ограничен, поэтому число
    // может быть «не меньше».
    if (fl->undescribed > 0) {
        gap(c, 1);
        char head[96];
        snprintf(head, sizeof(head), "Без пояснения · %s%d",
                 fl->scan_cut ? "не меньше " : "", fl->undescribed);
        if (fold_row(c, head, s->page_undesc_open ? NULL : "положены руками или ещё не описаны",
                     s->page_undesc_open, c->theme->row_text_dim))
            set_event(c, PAGE_EVENT_UNDESC_TOGGLE, 0, NULL);
        if (!s->page_undesc_open) return;

        int indent = cw * 2;
        for (int i = 0; i < fl->undesc_count; i++) {
            const char *path = fl->undesc[i];
            Rect r = { c->x - 8, c->y - 3, content_width(c) + 16, c->line + 6 };
            bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
            if (hover) DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);

            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            int right = c->x + content_width(c);
            bool describe_hit = false;
            if (hover) {
                const char *act = "Описать";
                int aw = chars_of(act) * cw;
                Rect ar = { right - aw - 8, c->y - 3, aw + 16, c->line + 2 };
                bool ah = inside(ar, c->mouse);
                if (ah) DrawRectangle(ar.x, ar.y, ar.w, ar.h, c->theme->sidebar_border);
                ui_text_clipped(c->font, act, ar.x + 8, c->y,
                                ah ? c->theme->row_text : c->theme->row_text_dim, aw);
                describe_hit = ah;
                right = ar.x - cw;
            }
            int nw = ui_text_clipped(c->font, base, c->x + indent, c->y, c->theme->row_text,
                                     (right - c->x - indent) / 2);
            // Папка — приглушённо после имени, если файл не в корне.
            if (base != path) {
                char dir[FILE_PATH_MAX];
                snprintf(dir, sizeof(dir), "%.*s", (int)(base - path - 1), path);
                ui_text_clipped(c->font, dir, c->x + indent + nw + cw, c->y,
                                c->theme->row_text_dim, right - (c->x + indent + nw + cw));
            }
            if (hover && c->click)
                set_event(c, describe_hit ? PAGE_EVENT_DESCRIBE_FILE : PAGE_EVENT_UFILE_OPEN,
                          i, NULL);
            c->y += c->line + 4;
        }
        if (fl->undescribed > fl->undesc_count) {
            char more[64];
            snprintf(more, sizeof(more), "и ещё %d", fl->undescribed - fl->undesc_count);
            text(c, more, c->theme->row_text_dim);
        }
        row_begin(c);
        c->row_x = c->x + indent;
        if (button(c, fl->undescribed > 1 ? "Описать все" : "Описать", false))
            set_event(c, PAGE_EVENT_DESCRIBE_FILES, 0, NULL);
        row_end(c);
    }
}

// --- таблица на странице -----------------------------------------------------

// Показанная таблица — ответ на «что у нас есть», который иначе приходится
// открывать в Excel. Работы над ней ровно три: посмотреть, отсортировать по
// колонке и раскрыть запись целиком. Правится она по-прежнему в файле —
// руками или агентом, — а страница только показывает: редактор таблицы это
// другой продукт, и заводить его ради взгляда на двенадцать строк незачем.

static const Table *g_sort_table;
static int  g_sort_col;
static bool g_sort_desc;

static int cmp_rows(const void *pa, const void *pb)
{
    int a = *(const int *)pa, b = *(const int *)pb;
    const char *x = g_sort_table->cells[a][g_sort_col];
    const char *y = g_sort_table->cells[b][g_sort_col];
    // Пустое — всегда внизу, в обе стороны: неизвестное значение по формату
    // оставляют пустым, и всплывать наверх ему незачем.
    if (!*x || !*y) {
        if (!*x && !*y) return a - b;
        return !*x ? 1 : -1;
    }
    int r;
    if (g_sort_table->col_num[g_sort_col]) {
        double dx = atof(x), dy = atof(y);
        r = dx < dy ? -1 : dx > dy ? 1 : 0;
    } else {
        // Побайтово: UTF-8 хранит порядок кодпоинтов, и кириллица так
        // выстраивается по алфавиту. Регистр и латиница идут отдельными
        // кучами — для взгляда на список это терпимо.
        r = strcmp(x, y);
    }
    if (r == 0) return a - b;   // равные держат порядок файла
    return g_sort_desc ? -r : r;
}

static void draw_table(Ctx *c, const Session *s, const Table *t, int idx)
{
    const int cw = c->font->cell_width;
    const Theme *th = c->theme;

    char head[160];
    if (t->exists)
        snprintf(head, sizeof(head), "%s · %d %s", t->name, t->file_rows,
                 plural3(t->file_rows, "запись", "записи", "записей"));
    else
        snprintf(head, sizeof(head), "%s", t->name);
    int act = section_action2(c, head, "Открыть", "Настроить");
    if (act == 1) set_event(c, PAGE_EVENT_TABLE_OPEN, idx, NULL);
    if (act == 2) set_event(c, PAGE_EVENT_TABLE_CFG, idx, NULL);

    if (!t->exists) {
        text(c, "таблица не читается — файла нет или в нём нет заголовков",
             th->row_text_dim);
        return;
    }

    // Ширина колонки — по содержимому, но не больше 24 знаков: одна длинная
    // заметка иначе съедает строку целиком. Колонки, не влезшие в ширину
    // раздела, отбрасываются справа — запись целиком открывается по клику.
    int avail = content_width(c) / cw;
    int col[TABLE_COLS_MAX], w[TABLE_COLS_MAX];
    int cols = 0, used = 0, chosen = 0;
    for (int i = 0; i < t->col_count; i++) {
        if (!t->col_show[i]) continue;
        chosen++;
        int cwid = t->col_chars[i];
        if (cwid > 24) cwid = 24;
        if (cwid < 3) cwid = 3;
        if (cols > 0 && used + cwid > avail) continue;
        col[cols] = i;
        w[cols] = cwid;
        used += cwid + 2;
        cols++;
    }

    // Выбор колонок: чипы по всем колонкам файла, выбранные акцентом.
    // Скрытая колонка не пропадает совсем — раскрытая запись показывает
    // все: настройка про то, что видно списком, а не про то, что есть.
    if ((s->page_table_cfg >> idx) & 1u) {
        text(c, "Какие колонки показывать списком", th->row_text_dim);
        row_begin(c);
        for (int i = 0; i < t->col_count; i++)
            if (button(c, t->cols[i], t->col_show[i])) {
                set_event(c, PAGE_EVENT_TABLE_COL, idx, NULL);
                c->event.arg2 = i;
            }
        row_end(c);
        row_begin(c);
        if (button(c, "Показать все", false))
            set_event(c, PAGE_EVENT_TABLE_COLS_ALL, idx, NULL);
        if (button(c, "Готово", false))
            set_event(c, PAGE_EVENT_TABLE_CFG, idx, NULL);
        row_end(c);
        gap(c, 1);
    }

    int sort = s->page_table_sort[idx];
    int sort_col = sort > 0 ? sort - 1 : sort < 0 ? -sort - 1 : -1;

    // Заголовки — они же кнопки сортировки: клик по колонке ведёт по кругу
    // «по возрастанию — по убыванию — как в файле». Стрелка показывает, где
    // мы сейчас, иначе третье состояние не отличить от первого.
    {
        int x = c->x;
        for (int k = 0; k < cols; k++) {
            int i = col[k];
            int cell = w[k] * cw;
            Rect r = { x - 4, c->y - 3, cell + 8, c->line + 4 };
            bool hover = inside(r, c->mouse) && visible_hit(c, c->mouse);
            if (hover) DrawRectangle(r.x, r.y, r.w, r.h, th->row_hover_bg);
            bool active = i == sort_col;
            int room = cell;
            if (active) {
                font_draw_codepoint(c->font, sort > 0 ? 0x25B4 : 0x25BE,
                                    (float)(x + cell - cw), (float)c->y,
                                    (float)c->font->size, th->progress_fill);
                room -= cw;
            }
            ui_text_clipped(c->font, t->cols[i], x, c->y,
                            active ? th->progress_fill : th->group_label, room);
            if (hover && c->click) {
                set_event(c, PAGE_EVENT_TABLE_SORT, idx, NULL);
                c->event.arg2 = i;
            }
            x += cell + cw * 2;
        }
        c->y += c->line;
        DrawRectangle(c->x, c->y - 3, content_width(c), 1, th->sidebar_border);
        c->y += 3;
    }

    // Порядок строк: показанный, а не файловый. Сортировка идёт по индексам,
    // сами ячейки не двигаются — файл принадлежит человеку.
    static int order[TABLE_ROWS_MAX];
    for (int i = 0; i < t->row_count; i++) order[i] = i;
    if (sort_col >= 0 && sort_col < t->col_count) {
        g_sort_table = t;
        g_sort_col = sort_col;
        g_sort_desc = sort < 0;
        qsort(order, (size_t)t->row_count, sizeof(order[0]), cmp_rows);
    }

    bool all = (s->page_table_all >> idx) & 1u;
    int limit = all ? t->row_count : (t->row_count < 8 ? t->row_count : 8);
    int open_row = s->page_table_row[idx] - 1;

    for (int k = 0; k < limit; k++) {
        int r = order[k];
        bool open = r == open_row;
        Rect rr = { c->x - 8, c->y - 3, content_width(c) + 16, c->line + 4 };
        bool hover = inside(rr, c->mouse) && visible_hit(c, c->mouse);
        if (hover) DrawRectangle(rr.x, rr.y, rr.w, rr.h, th->row_hover_bg);
        int row_top = rr.y;

        int x = c->x;
        for (int k = 0; k < cols; k++) {
            int i = col[k];
            const char *v = t->cells[r][i];
            int cell = w[k] * cw;
            int tw = chars_of(v) * cw;
            // Числа — по правому краю: так видно порядок величины.
            int tx = (t->col_num[i] && tw < cell) ? x + cell - tw : x;
            ui_text_clipped(c->font, v, tx, c->y,
                            k == 0 ? th->row_text : th->row_text_dim, cell);
            x += cell + cw * 2;
        }
        c->y += c->line;
        if (hover && c->click) {
            set_event(c, PAGE_EVENT_TABLE_ROW, idx, NULL);
            c->event.arg2 = r;
        }

        // Раскрытая запись — все колонки, включая отброшенные справа: ради
        // них клик и нужен.
        if (open) {
            gap(c, 0);
            for (int i = 0; i < t->col_count; i++) {
                if (!t->cells[r][i][0]) continue;
                char line_text[TABLE_CELL_MAX * 2];
                snprintf(line_text, sizeof(line_text), "%s: ", t->cols[i]);
                int lw = ui_text_clipped(c->font, line_text, c->x + cw * 2, c->y,
                                         th->row_text_dim, content_width(c) - cw * 2);
                ui_text_clipped(c->font, t->cells[r][i], c->x + cw * 2 + lw, c->y,
                                th->row_text, content_width(c) - cw * 2 - lw);
                c->y += c->line;
            }
            reveal_task_once(c, s->cwd, -4 - idx, r, row_top);
            gap(c, 1);
        }
    }

    if (t->row_count > limit || t->file_rows > t->row_count) {
        gap(c, 1);
        char more[96];
        if (t->row_count > limit)
            snprintf(more, sizeof(more), "Показать все · %d", t->row_count);
        else
            snprintf(more, sizeof(more), "В файле ещё %d — на странице первые %d",
                     t->file_rows - t->row_count, t->row_count);
        if (t->row_count > limit) {
            row_begin(c);
            if (button(c, more, false)) set_event(c, PAGE_EVENT_TABLE_ALL, idx, NULL);
            row_end(c);
        } else {
            text(c, more, th->row_text_dim);
        }
    } else if (all && t->row_count > 8) {
        row_begin(c);
        if (button(c, "Свернуть", false)) set_event(c, PAGE_EVENT_TABLE_ALL, idx, NULL);
        row_end(c);
    }

    // Что не влезло в ширину, из списка выпало молча — об этом надо сказать,
    // иначе колонка выглядит потерянной. Скрытая настройкой — выбор
    // человека, о ней не напоминаем.
    if (cols < chosen || t->wide)
        text(c, "не все колонки влезли — клик по записи показывает её целиком",
             th->row_text_dim);
    else if (chosen < t->col_count)
        text(c, "часть колонок скрыта настройкой — клик по записи показывает всё",
             th->row_text_dim);
}

// Сводка — первое, что видно: что это за проект, где он сейчас и что
// дальше. Её пишет агент по паспорту, коду и истории, поэтому она есть и
// там, где CLAUDE.md не заводили. Пока сводки нет, показываем статус из
// паспорта — это хоть какой-то ориентир, но он про последние действия, а
// за ними лучше идти в журнал. Действие «Собрать»/«Обновить» — в заголовке.
static void draw_summary(Ctx *c, const ProjInfo *info)
{
    const Theme *theme = c->theme;
    if (info->has_summary) {
        char age[48], head[96];
        projinfo_age(info->summary_mtime, age, sizeof(age));
        snprintf(head, sizeof(head), "Сводка · %s", age);
        if (section_action(c, head, "Обновить сводку"))
            set_event(c, PAGE_EVENT_BUILD_SUMMARY, 0, NULL);
        c->y -= c->line / 2;   // абзацы отбиваются сами

        // Три абзаца с подписями «Что это», «Где сейчас», «Дальше». Подпись
        // — отдельной строкой цветом заголовка: три вопроса должны
        // находиться глазом, а не вычитываться из начала абзаца.
        static const char *labels[] = { "Что это:", "Где сейчас:", "Дальше:" };
        char para[PROJINFO_SUMMARY_MAX];
        size_t used = 0;
        const char *p = info->summary;
        for (;;) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            bool blank = n == 0;
            if (!blank && used + n + 1 < sizeof(para)) {
                if (used) para[used++] = ' ';
                memcpy(para + used, p, n);
                used += n;
            }
            if ((blank || !nl) && used) {
                para[used] = '\0';
                const char *body = para;
                for (size_t k = 0; k < sizeof(labels) / sizeof(labels[0]); k++) {
                    size_t ln = strlen(labels[k]);
                    if (strncmp(para, labels[k], ln)) continue;
                    char label[32];
                    snprintf(label, sizeof(label), "%.*s", (int)ln - 1, labels[k]);
                    gap(c, 1);
                    text(c, label, theme->group_label);
                    body = para + ln;
                    while (*body == ' ') body++;
                    break;
                }
                if (body == para) gap(c, 1);
                draw_wrapped(c, body, c->x, content_width(c), theme->row_text, true);
                used = 0;
            }
            if (!nl) break;
            p = nl + 1;
        }
    } else if (info->status_lines > 0) {
        if (section_action(c, info->has_claude_md ? "Статус · CLAUDE.md" : "Статус",
                           "Собрать сводку"))
            set_event(c, PAGE_EVENT_BUILD_SUMMARY, 0, NULL);
        int status_max = info->status_lines < 4 ? info->status_lines : 4;
        for (int i = 0; i < status_max; i++)
            text(c, info->status[i], theme->row_text_dim);
        text(c, "Сводки ещё нет — её соберёт действие в заголовке", theme->row_text_dim);
    } else {
        if (section_action(c, "Сводка", "Собрать сводку"))
            set_event(c, PAGE_EVENT_BUILD_SUMMARY, 0, NULL);
        text(c, info->has_claude_md
             ? "Сводки ещё нет — её соберёт действие в заголовке"
             : "CLAUDE.md нет, сводки нет — «Собрать сводку» восстановит её из истории",
             theme->row_text_dim);
    }
}

// Ключ дня для сворачивания: год и номер дня, по местному времени. Не номер
// дня в ленте — новый день сдвинул бы номера, и свёрнутое поехало бы.
static int day_key(time_t when)
{
    struct tm t;
    localtime_r(&when, &t);
    return t.tm_year * 400 + t.tm_yday;
}

// Раскрыт ли день: два свежих — да, остальные — нет, если человек не
// перевернул умолчание кликом. rank — номер дня от свежего, 0 — свежий.
static bool day_open(const Session *s, int key, int rank)
{
    bool open = rank < 2;
    for (int i = 0; i < s->page_day_toggles; i++)
        if (s->page_day_keys[i] == key) open = !open;
    return open;
}

// Журнал: что здесь происходило, по времени. Сессии в него не выносятся —
// компакт и форк режут один разговор на файлы, и для человека эти границы
// ничего не значат. Значат — дни, часы и то, о чём он тогда просил. Дни
// складные: у живого проекта лента на сотни строк, а нужен обычно вчерашний
// и сегодняшний; остальные стоят заголовками со счётом.
static void draw_journal(Ctx *c, const Session *s, const Journal *jr, const ProjInfo *info)
{
    const Theme *theme = c->theme;
    const FontAtlas *font = c->font;

    int recaps = 0, days = 0, last_key = -1;
    for (int i = 0; i < jr->count; i++) {
        const JournalEvent *ev = &jr->events[i];
        if (ev->kind == JOURNAL_MOVED) continue;
        if (ev->kind == JOURNAL_RECAP) recaps++;
        int key = day_key(ev->when);
        if (key != last_key) { days++; last_key = key; }
    }

    // Журнал пишет не берт, а агент: он читает историю проекта и складывает
    // итоги работы в .berth/journal.md. Действие открывает для этого свою
    // вкладку, чтобы не мешать тому, что идёт в текущей.
    char head[64];
    if (days > 0) snprintf(head, sizeof(head), "Журнал работы · %d %s", days,
                           plural3(days, "день", "дня", "дней"));
    else          snprintf(head, sizeof(head), "Журнал работы");
    if (section_action(c, head, recaps > 0 ? "Обновить журнал" : "Собрать журнал"))
        set_event(c, PAGE_EVENT_BUILD_JOURNAL, 0, NULL);

    if (jr->count == 0) {
        text(c, "Здесь ещё не работали", theme->row_text_dim);
        return;
    }

    // Рисуем ленту целиком: страница прокручивается, и обрезать её по
    // высоте окна больше незачем — колесом человек уходит в любой день.
    if (jr->partial)
        text(c, "…более раннее в ленту не поместилось", theme->row_text_dim);

    int time_w = font->cell_width * 8;
    last_key = -1;
    int day_idx = 0;
    bool cur_open = true;
    for (int i = 0; i < jr->count; i++) {
        const JournalEvent *ev = &jr->events[i];
        if (ev->kind == JOURNAL_MOVED) continue;

        // День — складной строкой, а не датой у каждой записи: подряд идущие
        // события одного дня читаются как один кусок работы. В приписке —
        // сколько за день итогов и реплик: по свёрнутому дню видно, был ли
        // он пустым.
        int key = day_key(ev->when);
        if (key != last_key) {
            last_key = key;
            int d_recaps = 0, d_said = 0;
            for (int j = i; j < jr->count; j++) {
                const JournalEvent *e = &jr->events[j];
                if (e->kind == JOURNAL_MOVED) continue;
                if (day_key(e->when) != key) break;
                if (e->kind == JOURNAL_RECAP) d_recaps++;
                else if (e->kind != JOURNAL_COMPACT) d_said++;
            }
            int rank = days - 1 - day_idx++;
            cur_open = day_open(s, key, rank);

            char day[48], note[96] = "";
            journal_day(ev->when, day, sizeof(day));
            if (d_recaps && d_said)
                snprintf(note, sizeof(note), "%d %s · %d %s",
                         d_recaps, plural3(d_recaps, "итог", "итога", "итогов"),
                         d_said, plural3(d_said, "реплика", "реплики", "реплик"));
            else if (d_recaps)
                snprintf(note, sizeof(note), "%d %s", d_recaps,
                         plural3(d_recaps, "итог", "итога", "итогов"));
            else if (d_said)
                snprintf(note, sizeof(note), "%d %s", d_said,
                         plural3(d_said, "реплика", "реплики", "реплик"));
            gap(c, 1);
            if (fold_row(c, day, note, cur_open, theme->row_text))
                set_event(c, PAGE_EVENT_JOURNAL_DAY, key, NULL);
        }
        if (!cur_open) continue;

        char stamp[16];
        journal_clock(ev->when, stamp, sizeof(stamp));

        if (ev->kind == JOURNAL_COMPACT) {
            char note[64];
            // Ширина под время та же, что у строк ленты: шов должен стоять
            // в общей колонке, а не гулять по строке.
            snprintf(note, sizeof(note), "%-11s сжатие контекста", stamp);
            text(c, note, theme->row_text_dim);
            continue;
        }

        // Итог работы — то, ради чего сюда и приходят: он идёт в полную
        // силу, а реплики человека остаются приглушённым фоном. Реплика
        // это начало задачи, итог — чем она кончилась.
        Color tone = ev->kind == JOURNAL_RECAP ? theme->row_text : theme->row_text_dim;
        int row_top = c->y;
        bool hit = list_row_colored(c, stamp, ev->text, time_w, tone);

        // Расшифровка идёт под сутью, с отступом в ту же колонку: строка
        // читается как «время — что сделали», а под ней подробность.
        if (ev->detail[0])
            draw_wrapped(c, ev->detail, c->x + time_w,
                         content_width(c) - time_w, theme->row_text_dim, true);

        // Клик раскрывает запись, а не поднимает старую сессию: разговор
        // у проекта один, и вернуться к обсуждению значит напомнить о нём
        // агенту в этом разговоре, а не открыть второй.
        if (hit)
            set_event(c, PAGE_EVENT_JOURNAL_TOGGLE, i, NULL);

        if (s->page_journal_open == i + 1) {
            row_begin(c);
            c->row_x += time_w;
            if (button(c, "Упомянуть в текущем разговоре", true))
                set_event(c, PAGE_EVENT_JOURNAL_MENTION, i, NULL);
            row_end(c);
            reveal_task_once(c, s->cwd, -2, i, row_top);
        }
    }

    gap(c, 1);
    char note[200];
    int live = info->session_busy + info->session_empty;
    if (recaps > 0)
        snprintf(note, sizeof(note), "%d %s работы · %d %s · клик по дню сворачивает его",
                 recaps, plural3(recaps, "итог", "итога", "итогов"),
                 live, plural_talks(live));
    else
        snprintf(note, sizeof(note), "итогов работы нет — их пишет «Собрать журнал» в заголовке");
    text(c, note, theme->row_text_dim);
}

PageEvent page_draw_project(const Session *s, const ProjectState *st,
                            const SessionList *sessions,
                            const ProjectList *projects,
                            const FontAtlas *font, const Theme *theme,
                            Rect view, Vector2 mouse, int scroll)
{
    Ctx c = ctx_begin(font, theme, view, mouse, scroll);
    const ProjInfo *info = &st->info;

    revealed_begin();

    text(&c, s->name, theme->row_text);

    // Путь, значок папки и ветка — одной строкой. Папка открывается по
    // значку рядом с путём, а не кнопкой в общем ряду: действие стоит у
    // своего предмета. Ветка — акцентным цветом: это единственное здесь, что
    // меняется от работы, и глаз должен находить её без чтения строки.
    {
        int x = c.x;
        int branch_room = info->branch[0] ? font->cell_width * 24 : font->cell_width * 4;
        int w = ui_text_clipped(font, info->cwd, x, c.y, theme->row_text_dim,
                                content_width(&c) - branch_room);
        x += w + font->cell_width;
        Rect ir = { x - 4, c.y - 2, font->cell_width * 2 + 8, c.line + 4 };
        bool ih = inside(ir, c.mouse);
        if (ih) DrawRectangle(ir.x, ir.y, ir.w, ir.h, theme->row_hover_bg);
        font_draw_codepoint(font, 0xEA83, (float)x, (float)c.y, (float)font->size,
                            ih ? theme->row_text : theme->row_text_dim);
        if (ih && c.click) set_event(&c, PAGE_EVENT_OPEN_FOLDER, 0, NULL);
        x += font->cell_width * 3;
        if (info->branch[0]) {
            char br[300];
            snprintf(br, sizeof(br), "%s%s", info->detached ? "HEAD " : "", info->branch);
            font_draw_codepoint(font, 0xE0A0, (float)x, (float)c.y, (float)font->size,
                                theme->progress_fill);
            x += font->cell_width * 2;
            ui_text_clipped(font, br, x, c.y, theme->progress_fill,
                            c.x + content_width(&c) - x);
        }
        c.y += c.line;
    }

    gap(&c, 2);

    // В ряду только то, ради чего страницу открывают: вернуться к работе
    // или начать её. Сбор сводки и журнала ушли в заголовки своих разделов,
    // «Обновить» не нужно — страница перечитывает файлы сама.
    row_begin(&c);
    bool running = session_has_term(s);
    bool has_history = info->session_count > 0;

    if (running) {
        if (button(&c, "Вернуться к работе", true))
            set_event(&c, PAGE_EVENT_HIDE_PAGE, 0, NULL);
    } else {
        if (button(&c, has_history ? "Продолжить работу" : "Запустить Claude", true))
            set_event(&c, PAGE_EVENT_START_AGENT, 0, "claude");
        if (button(&c, "Оболочка", false))
            set_event(&c, PAGE_EVENT_START_AGENT, 0, "shell");
    }
    row_end(&c);

    // Ответ на последнее действие. Кнопка, которая молчит, читается как
    // сломанная, даже когда причина уважительная.
    if (s->page_notice[0]) {
        gap(&c, 1);
        text(&c, s->page_notice, theme->row_text);
    }

    // Фоновые задачи проекта: сбор журнала и что там ещё будет. Разговор
    // остаётся один, а работа рядом с ним видна и управляема — иначе она
    // происходит вслепую, и человеку остаётся верить на слово.
    int tasks = 0;
    for (int i = 0; sessions && i < sessions->count; i++) {
        const Session *t = &sessions->items[i];
        if (t->role != SESSION_ROLE_TASK || strcmp(t->cwd, s->cwd)) continue;

        if (!tasks++) {
            gap(&c, 1);
            text(&c, "Задачи рядом с разговором", theme->row_text_dim);
        }

        row_begin(&c);
        const char *state = session_task_state_text(t);
        // Состояние — цветом: работа синим, падение красным, остальное
        // приглушённо. Слово одно и то же читается по-разному.
        bool dead = t->state == SESSION_STATE_DEAD;
        Color tone = !session_has_term(t) ? theme->row_text_dim
                   : !dead                ? theme->progress_fill
                   : (t->term.child_reaped && t->term.child_status != 0)
                                          ? theme->badge_dead : theme->row_text_dim;
        int nw = ui_text_clipped(font, t->name, c.row_x, c.y + 6, theme->row_text,
                                 font->cell_width * 18);
        ui_text_clipped(font, state, c.row_x + nw + font->cell_width, c.y + 6, tone,
                        font->cell_width * 28 - nw - font->cell_width);
        c.row_x += font->cell_width * 30;

        if (button(&c, "Показать", false))
            set_event(&c, PAGE_EVENT_SHOW_TASK, i, NULL);
        if (button(&c, "Остановить", false))
            set_event(&c, PAGE_EVENT_STOP_TASK, i, NULL);
        row_end(&c);
    }

    gap(&c, 1);
    scroll_begin(&c);

    // Две колонки, если ширина позволяет: слева то, что было, — сводка и
    // журнал, — справа то, что будет, — задачи, подпроекты, скиллы. Окно
    // проекта занимает три четверти экрана, и линейная лента на такой
    // ширине оставляла половину пустой, а задачи уезжали за журнал вниз.
    // Прокрутка одна на обе колонки: страница остаётся одним листом.
    int full = content_width(&c);
    bool wide = full >= font->cell_width * 110;
    int gutter = font->cell_width * 4;
    int top = c.y;
    int left_x = c.x;
    int left_w = wide ? (full - gutter) * 3 / 5 : full;
    int right_x = left_x + left_w + gutter;
    int right_w = full - left_w - gutter;
    c.col_w = left_w;

    draw_summary(&c, info);

    // Показанные таблицы — между сводкой и журналом, в широкой колонке:
    // таблице нужна ширина, а справа она встала бы в 2/5 экрана и
    // растеряла бы колонки.
    for (int i = 0; i < st->table_count; i++)
        draw_table(&c, s, &st->tables[i], i);

    draw_journal(&c, s, &st->journal, info);

    int left_end = c.y;
    if (wide) {
        c.x = right_x;
        c.col_w = right_w;
        c.y = top;
    }

    // Редактор чужой страницы закрываем: он про свой каталог. Свой — это
    // каталог проекта или одного из его подпроектов.
    if (g_edit.active && !g_edit.project && strcmp(g_edit.cwd, s->cwd)) {
        bool ours = false;
        for (int k = 0; projects && k < projects->count; k++) {
            const Project *sub = &projects->items[k];
            if (sub->parent >= 0 && !strcmp(projects->items[sub->parent].path, s->cwd)
                && !strcmp(sub->path, g_edit.cwd)) ours = true;
        }
        if (!ours) g_edit.active = false;
    }

    char tasks_head[64];
    tasks_title(&st->tasks, "Задачи", tasks_head, sizeof(tasks_head));
    section(&c, tasks_head);
    draw_tasks(&c, s, &st->tasks, s->cwd, -1);

    // Подпроекты — после своих задач, свёрнутыми разделами.
    draw_subprojects(&c, s, projects);

    // Документы — что агент и человек сделали для чтения: после задач,
    // перед оснасткой.
    draw_files(&c, s, &st->files);

    // Скиллы — последними: это оснастка проекта, к ней ходят реже, чем к
    // задачам.
    draw_skills(&c, s, projects);

    if (wide) {
        int end = c.y > left_end ? c.y : left_end;
        // Разделитель между колонками — от верха прокручиваемой части до
        // нижней из двух. Рисуется после содержимого: высота известна только
        // теперь.
        DrawRectangle(right_x - gutter / 2, top + c.line, 1, end - top - c.line,
                      theme->sidebar_border);
        c.x = left_x;
        c.col_w = full;
        c.y = end;
    }

    revealed_end();
    c.event.scroll_top = c.scroll_top;
    c.event.overflow = ctx_end(&c);
    return c.event;
}

// --- настройки ---------------------------------------------------------------

// Строка настройки: подпись слева, органы управления справа. Подписи выровнены
// по одной колонке, иначе кнопки прыгают от длины слова.
// Экран настроек: у каждой настройки подпись в левой колонке, кнопки в
// правой, под кнопками — пояснение с переносом. Помощники держат раскладку
// сами, чтобы новая настройка добавлялась тремя строками и не могла лечь
// поверх соседней: раньше подпись без ряда кнопок рисовалась в ту же
// строку, что и следующий текст.
static const int SETTING_LABEL_CELLS = 22;   // ширина колонки подписей

static int setting_col(const Ctx *c)
{
    return c->x + c->font->cell_width * (SETTING_LABEL_CELLS + 2);
}

static void setting_begin(Ctx *c, const char *label)
{
    ui_text_clipped(c->font, label, c->x, c->y + 6, c->theme->row_text,
                    c->font->cell_width * SETTING_LABEL_CELLS);
    c->row_home = setting_col(c);
    c->row_x = c->row_home;
    c->row_used = false;
}

// Пояснение под кнопками, в колонке кнопок. Если кнопок не было, встаёт
// вровень с подписью — это настройка, которая правится только в файле.
static void setting_note(Ctx *c, const char *note)
{
    if (c->row_used) {
        c->y += c->font->cell_height + 12 + 6;
        c->row_used = false;
    } else {
        c->y += 6;
    }
    int x = setting_col(c);
    int width = c->view.x + c->view.w - PAD_X - x;
    draw_wrapped(c, note, x, width, c->theme->row_text_dim, true);
}

static void setting_end(Ctx *c)
{
    if (c->row_used) c->y += c->font->cell_height + 12;
    c->y += 10;
    c->row_used = false;
    c->row_home = 0;
}

static void setting_section(Ctx *c, const char *title)
{
    gap(c, 1);
    section(c, title);
    gap(c, 1);
}

// Карточка «Новый проект» над областью терминала. Высота берётся с
// прошлого кадра: содержимое immediate mode, и сколько оно займёт,
// известно только после отрисовки — на первом кадре подложка чуть короче.
static int g_overlay_h;

PageEvent page_draw_overlay(const FontAtlas *font, const Theme *theme,
                            Rect view, Vector2 mouse)
{
    Rect card;
    card.w = view.w - 80 < 640 ? view.w - 80 : 640;
    if (card.w < 240) card.w = view.w - 16;
    card.x = view.x + (view.w - card.w) / 2;
    card.y = view.y + 48;
    card.h = g_overlay_h > 0 ? g_overlay_h : (font->cell_height + 4) * 9;

    Ctx c = {
        .font = font, .theme = theme, .view = card, .mouse = mouse,
        .click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT),
        .x = card.x + PAD_X, .y = card.y + PAD_Y,
        .line = font->cell_height + 4,
        .list_sub = -1,
        .event = { .sub = -1 },
    };

    // Тень и подложка: карточка должна читаться поверх терминала, а не
    // сливаться с ним.
    for (int i = 3; i >= 1; i--)
        DrawRectangle(card.x - i, card.y - i + 2, card.w + 2 * i, card.h + 2 * i,
                      (Color){ 0, 0, 0, (unsigned char)(12 * (4 - i)) });
    DrawRectangle(card.x, card.y, card.w, card.h, theme->term_bg);
    DrawRectangleLines(card.x, card.y, card.w, card.h, theme->group_label);

    char head[PROJECT_NAME_MAX + 32];
    snprintf(head, sizeof(head), "Новый проект · %s", g_edit.group);
    text(&c, head, theme->row_text);
    char where[SESSION_PATH_MAX + 16];
    snprintf(where, sizeof(where), "Папка появится в %s", g_edit.cwd);
    text(&c, where, theme->row_text_dim);
    if (g_edit.notice[0]) text(&c, g_edit.notice, theme->badge_dead);

    draw_editor(&c, 0);

    g_overlay_h = c.y - card.y + PAD_Y;
    if (!g_edit.active) g_overlay_h = 0;

    // Прокрутки у карточки нет — просьбу «показать» не передаём.
    c.event.reveal = false;
    return c.event;
}

PageEvent page_draw_settings(const Settings *st, const Groups *groups,
                             const Usage *usage, const FontAtlas *font,
                             const Theme *theme, Rect view, Vector2 mouse,
                             int scroll)
{
    Ctx c = ctx_begin(font, theme, view, mouse, scroll);

    text(&c, "Настройки", theme->row_text);
    text(&c, "~/.config/berth/settings.conf — правится и руками, подхватывается на лету",
         theme->row_text_dim);
    gap(&c, 1);
    scroll_begin(&c);

    char buf[64], line[240];

    // --- Вид ---------------------------------------------------------------
    setting_section(&c, "Вид");

    setting_begin(&c, "Размер шрифта");
    if (button(&c, "−", false)) set_event(&c, PAGE_EVENT_FONT_STEP, -1, NULL);
    snprintf(buf, sizeof(buf), "%d", st->font_size);
    if (button(&c, buf, true)) set_event(&c, PAGE_EVENT_FONT_STEP, 0, NULL);
    if (button(&c, "+", false)) set_event(&c, PAGE_EVENT_FONT_STEP, +1, NULL);
    setting_note(&c, "Также ⌘+ / ⌘− / ⌘0.");
    setting_end(&c);

    // Темы: панель с полосой и страницами — одна на окно; терминал — своя,
    // либо как у проекта. Кнопок по числу встроенных тем — их семь, и
    // список на экране честнее выпадающего.
    setting_begin(&c, "Панель слева");
    for (int i = 0; i < theme_count(); i++) {
        const Theme *t = theme_at(i);
        if (button(&c, t->name, strcmp(st->theme_panel, t->name) == 0))
            set_event(&c, PAGE_EVENT_SET_THEME_PANEL, 0, t->name);
    }
    setting_note(&c, "Панель проектов и верхняя полоса. При переключении вкладок не меняется.");
    setting_end(&c);

    setting_begin(&c, "Окно справа");
    if (button(&c, "как у проекта", strcmp(st->theme_window, "project") == 0))
        set_event(&c, PAGE_EVENT_SET_THEME_WINDOW, 0, "project");
    for (int i = 0; i < theme_count(); i++) {
        const Theme *t = theme_at(i);
        if (button(&c, t->name, strcmp(st->theme_window, t->name) == 0))
            set_event(&c, PAGE_EVENT_SET_THEME_WINDOW, 0, t->name);
    }
    setting_note(&c, "Терминал и страницы вкладки. «Как у проекта» — тема из projects.json; "
                     "у проекта без темы — тема панели.");
    setting_end(&c);

    // --- Панель ------------------------------------------------------------
    setting_section(&c, "Панель");

    setting_begin(&c, "Боковая панель");
    if (button(&c, st->sidebar_visible ? "показана" : "скрыта", true))
        set_event(&c, PAGE_EVENT_TOGGLE_SIDEBAR, 0, NULL);
    setting_note(&c, "Также ⌘B.");
    setting_end(&c);

    setting_begin(&c, "Свёрнутая группа");
    if (button(&c, st->collapsed_show_live ? "показывает открытые проекты"
                                           : "прячет всё", true))
        set_event(&c, PAGE_EVENT_TOGGLE_COLLAPSED_LIVE, 0, NULL);
    setting_end(&c);

    setting_begin(&c, "Активный разговор");
    {
        bool karateka = !strcmp(st->marker, "karateka");
        if (button(&c, "точка", !karateka)) set_event(&c, PAGE_EVENT_SET_MARKER, 0, "dot");
        if (button(&c, "каратека", karateka)) set_event(&c, PAGE_EVENT_SET_MARKER, 0, "karateka");
    }
    setting_note(&c, "Сценка в строке активного разговора: стойка, бой, пока агент "
                     "работает, победа, когда закончил, поклон, когда зовёт. "
                     "Переменная BERTH_MARKER=karateka включает её для одного запуска.");
    setting_end(&c);

    // Скрытые группы возвращаются только отсюда: в панели их нет, и клик
    // по крестику иначе был бы дорогой в один конец.
    setting_begin(&c, "Скрытые группы");
    if (!groups || groups->hidden_count == 0) {
        setting_note(&c, "Нет. Крестик у заголовка группы в панели прячет её, вернуть можно здесь.");
    } else {
        for (int i = 0; i < groups->hidden_count; i++)
            if (button(&c, groups->hidden[i], false))
                set_event(&c, PAGE_EVENT_UNHIDE_GROUP, 0, groups->hidden[i]);
        setting_note(&c, "Клик по имени возвращает группу в панель.");
    }
    setting_end(&c);

    setting_begin(&c, "Спящие разговоры");
    if (st->sleep_after > 0)
        snprintf(line, sizeof(line),
                 "Свободный дольше %d мин сжимается в панели до одной строки (sleep_after в settings.conf).",
                 st->sleep_after);
    else
        snprintf(line, sizeof(line), "Не сжимаются (sleep_after = 0 в settings.conf).");
    setting_note(&c, line);
    setting_end(&c);

    setting_begin(&c, "Контекст разговора");
    snprintf(line, sizeof(line),
             "Шкала у открытого разговора, занято от окна: янтарный от %d %%, красный от %d %% "
             "(ctx_warn, ctx_crit в settings.conf).",
             st->ctx_warn, st->ctx_crit);
    setting_note(&c, line);
    setting_end(&c);

    // --- Проекты и агент ---------------------------------------------------
    setting_section(&c, "Проекты и агент");

    setting_begin(&c, "Клик по проекту");
    if (button(&c, st->open_project_page ? "открывает страницу" : "запускает агента", true))
        set_event(&c, PAGE_EVENT_TOGGLE_OPEN_MODE, 0, NULL);
    setting_note(&c, "Страница проекта открывается и по ⌘I, поверх любой вкладки.");
    setting_end(&c);

    setting_begin(&c, "Агент по умолчанию");
    bool is_claude = strcmp(st->default_agent, "shell") != 0;
    if (button(&c, "claude", is_claude))
        set_event(&c, PAGE_EVENT_SET_DEFAULT_AGENT, 0, "claude");
    if (button(&c, "оболочка", !is_claude))
        set_event(&c, PAGE_EVENT_SET_DEFAULT_AGENT, 0, "shell");
    setting_note(&c, "⌥-клик по проекту всегда открывает оболочку.");
    setting_end(&c);

    setting_begin(&c, "При запуске");
    if (st->resume_within > 0)
        snprintf(line, sizeof(line),
                 "Процессом поднимаются разговоры с работой за последние %d ч; остальные — страницей, "
                 "«Продолжить работу» поднимет тот же диалог (resume_within в settings.conf).",
                 st->resume_within);
    else
        snprintf(line, sizeof(line), "Поднимаются все разговоры (resume_within = 0 в settings.conf).");
    setting_note(&c, line);
    setting_end(&c);

    // --- Задачи ------------------------------------------------------------
    setting_section(&c, "Задачи");

    setting_begin(&c, "Готовые задачи");
    if (button(&c, "проверить", st->task_finish == TASK_FINISH_REVIEW))
        set_event(&c, PAGE_EVENT_SET_TASK_FINISH, TASK_FINISH_REVIEW, NULL);
    if (button(&c, "сразу в сделанные", st->task_finish == TASK_FINISH_DONE))
        set_event(&c, PAGE_EVENT_SET_TASK_FINISH, TASK_FINISH_DONE, NULL);
    if (button(&c, "не отмечать", st->task_finish == TASK_FINISH_NONE))
        set_event(&c, PAGE_EVENT_SET_TASK_FINISH, TASK_FINISH_NONE, NULL);
    setting_note(&c, "Задача из списка уходит агенту с наказом отметить её в файле, когда закончит: "
                     "«проверить» — в сделанные переносите вы сами.");
    setting_end(&c);

    setting_begin(&c, "Модель служебных задач");
    if (st->task_model[0])
        snprintf(line, sizeof(line),
                 "Журнал и сводку собирает %s (task_model в settings.conf).", st->task_model);
    else
        snprintf(line, sizeof(line),
                 "Журнал и сводку собирает модель Claude Code по умолчанию (task_model пуст).");
    setting_note(&c, line);
    setting_end(&c);

    // --- Лимиты ------------------------------------------------------------
    // Чья учётка и откуда цифры. Отдельного входа здесь нет и не будет —
    // берт пользуется входом Claude Code на этой машине.
    setting_section(&c, "Лимиты Claude");

    setting_begin(&c, "Откуда цифры");
    if (button(&c, st->usage_fetch ? "берт спрашивает сам" : "только кэш Claude Code", true))
        set_event(&c, PAGE_EVENT_TOGGLE_USAGE_FETCH, 0, NULL);
    if (usage && usage->count > 0) {
        char when[16];
        struct tm tm;
        localtime_r(&usage->fetched, &tm);
        snprintf(when, sizeof(when), "%02d:%02d", tm.tm_hour, tm.tm_min);
        snprintf(line, sizeof(line), "Учётка %s · получены в %s %s.%s",
                 usage->account[0] ? usage->account : "Claude Code", when,
                 usage->source == USAGE_FROM_OWN ? "своим запросом" : "из кэша Claude Code",
                 st->usage_fetch
                     ? " Токен берётся из связки ключей macOS; отказ смотрите в ~/.config/berth/usage.json.log."
                     : "");
    } else {
        snprintf(line, sizeof(line), "Данных нет: войдите в Claude Code (/login) в любой вкладке.");
    }
    setting_note(&c, line);
    setting_end(&c);

    // --- Скиллы берта ------------------------------------------------------
    // Что есть и что включено. Едут с пакетом и подключаются к каждому
    // разговору из берта; выключенный убирается из папки пакета.
    setting_section(&c, "Скиллы берта");
    for (int i = 0; i < SKILLS_BUILTIN_COUNT; i++) {
        const char *name = SKILLS_BUILTIN[i].name;
        bool on = settings_skill_enabled(st, name);
        row_begin(&c);
        if (toggle(&c, on))
            set_event(&c, PAGE_EVENT_TOGGLE_BERTH_SKILL, i, name);
        ui_text_clipped(font, name, c.row_x, c.y + 6,
                        on ? theme->row_text : theme->row_text_dim, font->cell_width * 20);
        row_end(&c);
        char desc[SKILL_DESC_MAX];
        skills_builtin_desc(SKILLS_BUILTIN[i].skill_md, desc, sizeof(desc));
        c.y -= 6;
        body_text(&c, desc, font->cell_width * 2, theme->row_text_dim);
    }
    gap(&c, 1);
    draw_wrapped(&c, "Подключаются к каждому запуску claude из берта (--add-dir на папку пакета). "
                     "Выключенный убирается из папки, и Claude Code его не видит — и в открытых "
                     "разговорах тоже.",
                 c.x, content_width(&c), theme->row_text_dim, true);
    gap(&c, 2);

    c.event.overflow = ctx_end(&c);
    return c.event;
}
