#include <stdio.h>
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
    return c->view.w - PAD_X * 2;
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
    int chars = 0;
    for (const char *p = label; *p; ) {
        int size = 0;
        GetCodepointNext(p, &size);
        if (size <= 0) break;
        p += size;
        chars++;
    }

    Rect r = {
        .x = c->row_x,
        .y = c->y,
        .w = chars * c->font->cell_width + 24,
        .h = c->font->cell_height + 12,
    };
    // Ряд не влезает — переносим кнопку на следующую строку, в ту же
    // колонку, где ряд начался: у настройки это колонка после подписи.
    int home = c->row_home ? c->row_home : c->x;
    if (r.x + r.w > c->view.x + c->view.w - PAD_X && c->row_x > home) {
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
        DrawRectangle(r.x, r.y, 3, r.h, c->theme->group_label);

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

// Какая раскрытая задача уже показана: просим показать один раз, при
// раскрытии, а не каждый кадр — иначе колесо не могло бы увести от неё.
static struct { char cwd[SESSION_PATH_MAX]; int sub, index; } g_revealed = { "", -1, -1 };

static void reveal_task_once(Ctx *c, const char *cwd, int sub, int index, int top)
{
    if (g_revealed.index == index && g_revealed.sub == sub && !strcmp(g_revealed.cwd, cwd))
        return;
    snprintf(g_revealed.cwd, sizeof(g_revealed.cwd), "%s", cwd);
    g_revealed.sub = sub;
    g_revealed.index = index;
    reveal(c, top, c->y);
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
    } else {
        if (hover || open)
            DrawRectangle(r.x, r.y, r.w, r.h, c->theme->row_hover_bg);
        char label[TASK_TITLE_MAX + 16];
        task_label(label, sizeof(label), task, number);
        int width = content_width(c);
        if (task->state == TASK_REVIEW) {
            // Пометка справа, тем же цветом, что «агент зовёт»: задача ждёт
            // человека, а не агента.
            const char *hint = "проверить";
            int hint_w = c->font->cell_width * 9;
            ui_text_clipped(c->font, hint, c->x + width - hint_w, c->y,
                            c->theme->badge_attention, hint_w);
            width -= hint_w + c->font->cell_width;
        }
        ui_text_clipped(c->font, label, c->x, c->y,
                        task->state == TASK_DONE ? c->theme->row_text_dim
                                                 : c->theme->row_text,
                        width);
    }
    c->y += c->line + 6;
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

    // Сделанные: их немного и они не мешают, но убирать с глаз нельзя —
    // иначе не вернуть, если отметили сгоряча.
    bool first_done = true;
    for (int i = 0; i < tl->count; i++) {
        const Task *task = &tl->items[i];
        if (task->state != TASK_DONE) continue;
        if (first_done) { gap(c, 1); first_done = false; }

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

PageEvent page_draw_project(const Session *s, const ProjectState *st,
                            const SessionList *sessions,
                            const ProjectList *projects,
                            const FontAtlas *font, const Theme *theme,
                            Rect view, Vector2 mouse, int scroll)
{
    Ctx c = ctx_begin(font, theme, view, mouse, scroll);
    const ProjInfo *info = &st->info;

    // Ничего не раскрыто — следующее раскрытие снова попросит показать себя.
    if (!s->page_task_open) g_revealed.index = -1;

    text(&c, s->name, theme->row_text);

    char line[700];
    if (info->branch[0])
        snprintf(line, sizeof(line), "%s  ·  %s%s", info->cwd,
                 info->detached ? "HEAD " : "", info->branch);
    else
        snprintf(line, sizeof(line), "%s", info->cwd);
    text(&c, line, theme->row_text_dim);

    gap(&c, 2);

    // Первой идёт кнопка, ради которой страницу и открывают. Что на ней
    // написано, зависит от того, работает ли уже агент в этой вкладке:
    // страницу можно открыть и поверх живой сессии, чтобы посмотреть на
    // проект, — тогда возвращаться некуда, кроме как обратно к работе.
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
    if (button(&c, "Обновить", false))
        set_event(&c, PAGE_EVENT_REFRESH, 0, NULL);
    // Журнал пишет не берт, а агент: он читает историю проекта и складывает
    // итоги работы в .berth/journal.md. Кнопка открывает для этого свою
    // вкладку, чтобы не мешать тому, что идёт в текущей.
    if (button(&c, "Собрать журнал", false))
        set_event(&c, PAGE_EVENT_BUILD_JOURNAL, 0, NULL);
    if (button(&c, info->has_summary ? "Обновить сводку" : "Собрать сводку", false))
        set_event(&c, PAGE_EVENT_BUILD_SUMMARY, 0, NULL);
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
        char label[96];
        const char *state = session_task_state_text(t);
        snprintf(label, sizeof(label), "%s · %s", t->name, state);
        ui_text_clipped(font, label, c.row_x, c.y + 6, theme->row_text,
                        font->cell_width * 26);
        c.row_x += font->cell_width * 28;

        if (button(&c, "Показать", false))
            set_event(&c, PAGE_EVENT_SHOW_TASK, i, NULL);
        if (button(&c, "Остановить", false))
            set_event(&c, PAGE_EVENT_STOP_TASK, i, NULL);
        row_end(&c);
    }

    gap(&c, 1);
    scroll_begin(&c);

    // Сводка — первое, что видно: что это за проект, где он сейчас и что
    // дальше. Её пишет агент по паспорту, коду и истории, поэтому она есть и
    // там, где CLAUDE.md не заводили. Пока сводки нет, показываем статус из
    // паспорта — это хоть какой-то ориентир, но он про последние действия, а
    // за ними лучше идти в журнал.
    if (info->has_summary) {
        char age[48], head[96];
        projinfo_age(info->summary_mtime, age, sizeof(age));
        snprintf(head, sizeof(head), "Сводка · %s", age);
        section(&c, head);
        c.y -= c.line / 2;   // body_text сам отбивает абзацы
        body_text(&c, info->summary, 0, theme->row_text);
    } else if (info->status_lines > 0) {
        section(&c, info->has_claude_md ? "Статус · CLAUDE.md" : "Статус");
        int status_max = info->status_lines < 4 ? info->status_lines : 4;
        for (int i = 0; i < status_max; i++)
            text(&c, info->status[i], theme->row_text_dim);
        text(&c, "Сводки ещё нет — её пишет кнопка «Собрать сводку»", theme->row_text_dim);
    } else {
        section(&c, "Сводка");
        text(&c, info->has_claude_md
             ? "Сводки ещё нет — её пишет кнопка «Собрать сводку»"
             : "CLAUDE.md нет, сводки нет — «Собрать сводку» восстановит её из истории",
             theme->row_text_dim);
    }

    // Журнал: что здесь происходило, по времени. Сессии в него не выносятся —
    // компакт и форк режут один разговор на файлы, и для человека эти границы
    // ничего не значат. Значат — дни, часы и то, о чём он тогда просил.
    const Journal *jr = &st->journal;
    if (jr->count > 0) {
        section(&c, "Журнал работы");

        // Рисуем ленту целиком: страница прокручивается, и обрезать её по
        // высоте окна больше незачем — колесом человек уходит в любой день.
        if (jr->partial)
            text(&c, "…более раннее в ленту не поместилось", theme->row_text_dim);

        int time_w = font->cell_width * 8;
        char last_day[48] = "";
        for (int i = 0; i < jr->count; i++) {
            const JournalEvent *ev = &jr->events[i];
            if (ev->kind == JOURNAL_MOVED) continue;

            // День отбивается заголовком, а не датой у каждой строки: подряд
            // идущие события одного дня читаются как один кусок работы.
            char day[48], stamp[16];
            journal_day(ev->when, day, sizeof(day));
            journal_clock(ev->when, stamp, sizeof(stamp));
            if (strcmp(day, last_day)) {
                snprintf(last_day, sizeof(last_day), "%s", day);
                gap(&c, 1);
                text(&c, day, theme->row_text_dim);
            }

            if (ev->kind == JOURNAL_COMPACT) {
                char note[64];
                // Ширина под время та же, что у строк ленты: шов должен
                // стоять в общей колонке, а не гулять по строке.
                snprintf(note, sizeof(note), "%-11s сжатие контекста", stamp);
                text(&c, note, theme->row_text_dim);
                continue;
            }

            // Итог работы — то, ради чего сюда и приходят: он идёт в полную
            // силу, а реплики человека остаются приглушённым фоном. Реплика
            // это начало задачи, итог — чем она кончилась.
            Color tone = ev->kind == JOURNAL_RECAP ? theme->row_text
                                                   : theme->row_text_dim;
            int row_top = c.y;
            bool hit = list_row_colored(&c, stamp, ev->text, time_w, tone);

            // Расшифровка идёт под сутью, с отступом в ту же колонку: строка
            // читается как «время — что сделали», а под ней подробность.
            if (ev->detail[0])
                draw_wrapped(&c, ev->detail, c.x + time_w,
                             content_width(&c) - time_w, theme->row_text_dim, true);

            // Клик раскрывает запись, а не поднимает старую сессию: разговор
            // у проекта один, и вернуться к обсуждению значит напомнить о
            // нём агенту в этом разговоре, а не открыть второй.
            if (hit)
                set_event(&c, PAGE_EVENT_JOURNAL_TOGGLE, i, NULL);

            if (s->page_journal_open == i + 1) {
                row_begin(&c);
                c.row_x += time_w;
                if (button(&c, "Упомянуть в текущем разговоре", true))
                    set_event(&c, PAGE_EVENT_JOURNAL_MENTION, i, NULL);
                row_end(&c);
                reveal_task_once(&c, s->cwd, -2, i, row_top);
            }
        }

        gap(&c, 1);
        char note[200];
        int live = info->session_busy + info->session_empty;
        int recaps = 0;
        for (int i = 0; i < jr->count; i++)
            if (jr->events[i].kind == JOURNAL_RECAP) recaps++;
        if (recaps > 0)
            snprintf(note, sizeof(note),
                     "%d %s работы · %d %s · клик продолжит разговор",
                     recaps, recaps == 1 ? "итог" : "итогов",
                     live, plural_talks(live));
        else
            snprintf(note, sizeof(note),
                     "итогов работы нет — их пишет кнопка «Собрать журнал»");
        text(&c, note, theme->row_text_dim);
    } else {
        section(&c, "Журнал работы");
        text(&c, "Здесь ещё не работали", theme->row_text_dim);
    }

    // Задачи — после журнала: журнал про то, что было, задачи про то, что
    // будет, и страница открывается на свежем конце — там они и видны.
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

    // Скиллы — последними: это оснастка проекта, к ней ходят реже, чем к
    // задачам, а страница открывается на конце, и они всё равно на виду.
    draw_skills(&c, s, projects);

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
