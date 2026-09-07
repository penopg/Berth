#include <string.h>
#include <math.h>

#include <stdio.h>

#include "ui.h"
#include "projstate.h"
#include "xp.h"

// Текст с обрезкой по ширине. Шрифт моноширинный, поэтому ширина считается
// по ячейкам, без измерения строки на каждый символ. Резать надо по границам
// кодпоинтов: обрыв UTF-8 на середине символа даёт мусор в панели.
// Знакомест в строке: ширина текста считается по ним, а не по байтам —
// в русской строке байт вдвое больше букв.
static size_t strlen_utf8_cells(const char *s)
{
    size_t n = 0;
    for (const char *p = s; *p; ) {
        int size = 0;
        GetCodepointNext(p, &size);
        if (size <= 0) break;
        p += size;
        n++;
    }
    return n;
}

static int g_clip_top, g_clip_bottom;

void ui_clip_rows(int top, int bottom)
{
    g_clip_top = top;
    g_clip_bottom = bottom;
}

static bool clipped_out(int y, int h)
{
    return g_clip_bottom > g_clip_top && (y + h < g_clip_top || y > g_clip_bottom);
}

int ui_text_clipped(const FontAtlas *f, const char *text,
                    int x, int y, Color color, int max_width)
{
    int max_chars = f->cell_width > 0 ? max_width / f->cell_width : 0;
    if (max_chars <= 0 || !text || !*text) return 0;

    char buf[512];
    int  written = 0;   // байт в buf
    int  chars = 0;     // отрисованных знакомест
    const char *p = text;

    while (*p && chars < max_chars) {
        int size = 0;
        GetCodepointNext(p, &size);
        if (size <= 0) break;
        if (written + size >= (int)sizeof(buf) - 4) break;
        memcpy(buf + written, p, (size_t)size);
        written += size;
        p += size;
        chars++;
    }

    // Строка не поместилась — заменяем последнее знакоместо многоточием.
    if (*p && chars > 0) {
        while (written > 0 && (buf[written - 1] & 0xC0) == 0x80) written--;
        if (written > 0) written--;
        const char ellipsis[] = "…";
        if (written + (int)sizeof(ellipsis) < (int)sizeof(buf)) {
            memcpy(buf + written, ellipsis, sizeof(ellipsis) - 1);
            written += (int)sizeof(ellipsis) - 1;
        }
    }

    buf[written] = '\0';
    // Ширину возвращаем ту же — от неё зависит раскладка; не рисуем только
    // то, чего всё равно не видно.
    if (!clipped_out(y, f->cell_height))
        DrawTextEx(font_for(f, buf), buf, (Vector2){ (float)x, (float)y },
                   (float)f->size, 0, color);
    return chars * f->cell_width;
}

// Полоска прогресса. Данных пока нет ни от одного агента — рисуется только
// когда сессия сообщила прогресс через OSC 9;4 (Этап 3).
static void draw_progress(const Theme *th, Rect row, int progress)
{
    const int h = 3;
    int y = row.y + row.h - h - 2;
    int x = row.x + 10;
    int w = row.w - 20;
    if (w <= 0) return;

    DrawRectangle(x, y, w, h, th->progress_bg);
    int fill = w * progress / 100;
    if (fill > 0)
        DrawRectangle(x, y, fill, h, th->progress_fill);
}

// Маркер состояния. Три покоя и два движения: движется только то, что
// действительно происходит, — работа и зов. Крестик и точки стоят на
// месте, иначе панель из десяти проектов мельтешила бы вся.
void ui_draw_marker(Rect cell, bool open, SessionState state, double time,
                    const Theme *th)
{
    if (!open) return;
    float cx = cell.x + cell.w / 2.0f;
    float cy = cell.y + cell.h / 2.0f;

    switch (state) {
    case SESSION_STATE_IDLE:
        DrawCircle((int)cx, (int)cy, 2.5f, th->row_text_dim);
        break;

    case SESSION_STATE_BUSY: {
        // Квадратик медленно вращается и дышит: полный оборот за четыре
        // секунды, размер гуляет на пиксель — заметно, но не назойливо.
        float side  = 6.0f + 1.0f * sinf((float)(time * 2.5));
        float angle = (float)fmod(time * 90.0, 360.0);
        Rectangle rec = { cx, cy, side, side };
        DrawRectanglePro(rec, (Vector2){ side / 2, side / 2 }, angle,
                         th->progress_fill);
        break;
    }

    case SESSION_STATE_ATTENTION: {
        // Пульс: точка, а вокруг неё расходится и гаснет кольцо. Кольцо и
        // есть «зов» — точка сама по себе читалась бы как «свободна».
        float phase = (float)fmod(time * 1.2, 1.0);
        Color halo = th->badge_attention;
        halo.a = (unsigned char)(140 * (1.0f - phase));
        DrawCircle((int)cx, (int)cy, 2.0f + 3.5f * phase, halo);
        DrawCircle((int)cx, (int)cy, 3.0f, th->badge_attention);
        break;
    }

    case SESSION_STATE_DEAD: {
        float d = 3.0f;
        DrawLineEx((Vector2){ cx - d, cy - d }, (Vector2){ cx + d, cy + d }, 2.0f, th->badge_dead);
        DrawLineEx((Vector2){ cx - d, cy + d }, (Vector2){ cx + d, cy - d }, 2.0f, th->badge_dead);
        break;
    }
    }
}

// Прописные для заголовка группы: ASCII и кириллица, остального в именах
// групп не встречается. Простая таблица вместо towupper: локаль нам ни к чему.
static void upper_utf8(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 3 < cap; ) {
        if (p[0] < 0x80) {
            dst[o++] = (char)((p[0] >= 'a' && p[0] <= 'z') ? p[0] - 32 : p[0]);
            p++;
        } else if (p[0] == 0xD0 && p[1] >= 0xB0 && p[1] <= 0xBF) {      // а–п
            dst[o++] = (char)0xD0; dst[o++] = (char)(p[1] - 0x20); p += 2;
        } else if (p[0] == 0xD1 && p[1] >= 0x80 && p[1] <= 0x8F) {      // р–я
            dst[o++] = (char)0xD0; dst[o++] = (char)(p[1] + 0x20); p += 2;
        } else if (p[0] == 0xD1 && p[1] == 0x91) {                      // ё
            dst[o++] = (char)0xD0; dst[o++] = (char)0x81; p += 2;
        } else {
            dst[o++] = (char)*p++;
        }
    }
    dst[o] = 0;
}

// Заголовок группы: прописными, приглушённо, с числом проектов и отбивкой
// сверху больше межстрочной — чтобы группа читалась как раздел, а не как
// ещё один проект тем же кеглем. Под заголовком тонкая черта цвета
// первого проекта группы: единственное, что осталось от цветов Warp.
static void draw_group_row(const PanelRow *row, const FontAtlas *f,
                           const Theme *th, bool hovered)
{
    Rect r = row->rect;
    int text_y = r.y + r.h - f->cell_height - 6;
    if (hovered)
        DrawRectangle(r.x, text_y - 4, r.w, f->cell_height + 8, th->row_hover_bg);

    // ▸ / ▾ — из запасного шрифта, если в основном нет.
    font_draw_codepoint(f, row->collapsed ? 0x25B8 : 0x25BE,
                        (float)(r.x + ROW_MARKER_PAD), (float)text_y, (float)f->size,
                        th->group_label);

    char label[PROJECT_NAME_MAX * 2];
    upper_utf8(label, sizeof(label), row->label);

    int x = r.x + ROW_TEXT_X;
    int right = r.x + r.w - 14;

    // Под курсором в правом краю — «убрать группу». Только под курсором:
    // держать крестик на каждом заголовке значило бы звать нажать его.
    // «прочее» не убирается: это открытые вкладки, а не список.
    if (hovered && strcmp(row->label, "прочее") != 0) {
        Rect b = layout_row_info_rect(r, f->cell_width);
        b.y = text_y - 2;
        b.h = f->cell_height + 4;
        DrawRectangle(b.x, b.y, b.w, b.h, th->row_hover_bg);
        ui_text_clipped(f, "×", b.x + (b.w - f->cell_width) / 2, text_y,
                        th->row_text_dim, f->cell_width * 2);
        // Рядом — «+»: завести проект в этой группе. Тоже только под
        // курсором, по той же причине.
        Rect a = layout_row_group_add_rect(r, f->cell_width);
        a.y = b.y; a.h = b.h;
        DrawRectangle(a.x, a.y, a.w, a.h, th->row_hover_bg);
        ui_text_clipped(f, "+", a.x + (a.w - f->cell_width) / 2, text_y,
                        th->row_text_dim, f->cell_width * 2);
        right = a.x - 6;
    }

    int text_w = ui_text_clipped(f, label, x, text_y, th->group_label, right - x);

    // Число проектов — всегда, а не только у свёрнутой: оно говорит,
    // сколько в разделе, а не сколько спрятано. Узкой панели не навязываем.
    int total = row->count > 0 ? row->count : row->hidden;   // у «прочее» — спрятанные
    char n[16];
    snprintf(n, sizeof(n), "%d", total);
    int nx = x + text_w + f->cell_width;
    if (total > 0 && nx + (int)strlen(n) * f->cell_width <= right)
        ui_text_clipped(f, n, nx, text_y, th->row_text_dim, right - nx);

    Color line = row->color;
    line.a = 150;
    int lw = right - x;
    if (lw > 8)
        DrawRectangle(x, text_y + f->cell_height + 3, lw, 1, line);
}

// Направляющие дерева у строки-ребёнка: вертикаль от верха строки до
// низа (или до середины маркера у последнего), уголок к ячейке маркера.
// Внешний уровень, если он проходит сквозь строку, — сплошной вертикалью.
static void draw_guides(const PanelRow *row, Rect marker, const Theme *th)
{
    if (row->depth <= 0) return;
    Rect r = row->rect;
    Color c = th->sidebar_border;
    int cy = marker.y + marker.h / 2;

    if (row->cont)
        DrawRectangle(layout_row_guide_x(r, row->depth - 1), r.y, 1, r.h, c);

    int gx = layout_row_guide_x(r, row->depth);
    int bottom = row->last ? cy : r.y + r.h;
    DrawRectangle(gx, r.y, 1, bottom - r.y, c);
    DrawRectangle(gx, cy, marker.x - 2 - gx, 1, c);
}

// У родителя направляющая начинается под маркером и идёт до низа строки.
static void draw_guide_stub(const PanelRow *row, Rect marker, const Theme *th)
{
    if (!row->has_children) return;
    Rect r = row->rect;
    int gx = layout_row_guide_x(r, row->depth + 1);
    int top = marker.y + marker.h + 2;
    DrawRectangle(gx, top, 1, r.y + r.h - top, th->sidebar_border);
}

// Активная вкладка: подложка и двухпиксельная черта акцентного цвета у
// левого края. Это единственная полоска в панели — значит, «вы здесь».
static void draw_active_mark(Rect r, const Theme *th)
{
    DrawRectangle(r.x, r.y, r.w, r.h, th->row_active_bg);
    DrawRectangle(r.x, r.y, 2, r.h, th->progress_fill);
}

// Последняя строка панели: добавить группу-папку. Приглушённая, с плюсом —
// это действие, а не проект, и внимания ему нужно ровно столько, чтобы
// найти, когда понадобится.
static void draw_add_group_row(const PanelRow *row, const FontAtlas *f,
                               const Theme *th, bool hovered)
{
    Rect r = row->rect;
    int text_y = r.y + r.h - f->cell_height - 4;
    if (hovered)
        DrawRectangle(r.x, text_y - 4, r.w, f->cell_height + 8, th->row_hover_bg);
    char label[PROJECT_NAME_MAX + 4];
    snprintf(label, sizeof(label), "+ %s", row->label);
    ui_text_clipped(f, label, r.x + 12, text_y,
                    hovered ? th->row_text : th->row_text_dim, r.w - 24);
}

// Контекст разговора: четыре сегмента и процент занятого, правым краем у
// правого края строки. Шкала заполняется по мере расхода — так привычнее,
// чем остаток. Ширина столбца фиксированная — числа разной длины не должны
// дёргать шкалу. Возвращает занятую ширину, 0 — если не рисовался.
static int draw_ctx_column(const Session *session, Rect r, int line_y,
                           const FontAtlas *f, const Theme *th,
                           int warn, int crit)
{
    int used = ctx_percent_used(&session->ctx);
    if (used < 0) return 0;

    const int seg_w = 5, seg_h = 9, gap = 2, segs = 4;
    int scale_w = segs * seg_w + (segs - 1) * gap;
    int width = scale_w + 6 + f->cell_width * 4;
    if (r.w < 200) return 0;   // узкой панели столбец не навязываем

    Color c = used >= crit ? th->badge_dead
            : used >= warn ? th->badge_attention
            : th->row_text_dim;

    int x = r.x + r.w - 14 - width;
    int y = line_y + (f->cell_height - seg_h) / 2;
    int filled = (used + 12) / 25;   // 0..4, с округлением к ближайшему
    if (filled == 0 && used > 0) filled = 1;
    for (int i = 0; i < segs; i++) {
        Rect seg = { x + i * (seg_w + gap), y, seg_w, seg_h };
        if (i < filled) DrawRectangle(seg.x, seg.y, seg.w, seg.h, c);
        else            DrawRectangleLines(seg.x, seg.y, seg.w, seg.h, c);
    }

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", used);
    int tw = (int)strlen(pct) * f->cell_width;
    ui_text_clipped(f, pct, r.x + r.w - 14 - tw, line_y, c, tw);
    return width + f->cell_width;
}

// Длина строки в знаках, не в байтах: «Σ» — два байта, одно знакоместо.
static size_t utf8_len(const char *s)
{
    size_t n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

// Токены коротко: 850, 12.3k, 240k, 1.3M.
static void fmt_tokens(long n, char *out, size_t cap)
{
    if (n < 1000)         snprintf(out, cap, "%ld", n);
    else if (n < 100000)  snprintf(out, cap, "%.1fk", n / 1000.0);
    else if (n < 1000000) snprintf(out, cap, "%ldk", n / 1000);
    else                  snprintf(out, cap, "%.1fM", n / 1000000.0);
}

// Строка панели. Проект может быть открыт (есть сессия) или закрыт — тогда
// показываем только имя, а клик его откроет.
static void draw_item_row(const PanelRow *row, const Project *project,
                          const Session *session,
                          const FontAtlas *f, const Theme *th,
                          bool active, bool hovered, double time,
                          int ctx_warn, int ctx_crit,
                          const Sprites *sprites, Rect clip)
{
    Rect r = row->rect;

    const char *name = project ? project->name : (session ? session->name : "");

    if (active)       draw_active_mark(r, th);
    else if (hovered) DrawRectangle(r.x, r.y, r.w, r.h, th->row_hover_bg);

    // Маркер состояния в левом столбце. Цвет проекта здесь не рисуется:
    // он назначен для вкладок Warp, у каждого проекта свой, и на этом месте
    // читался бы как индикатор, которым не является. Маркер — про агента:
    // у вкладки-страницы без процесса ячейка пуста, как у закрытого.
    Rect marker = layout_row_marker_rect(r, r.y + 4, f->cell_height, row->depth);
    draw_guides(row, marker, th);
    draw_guide_stub(row, marker, th);
    bool live = session && session_has_term(session);
    int text_x = r.x + ROW_TEXT_X + row->depth * ROW_INDENT;

    // Сценка с каратекой вместо маркера — у каждого живого разговора в
    // строке на три линии (спящий сжат до одной, ему точка). Ячейка
    // обрезается ножницами: противник вбегает из-за её края, а не из-под
    // имени проекта. Пол сцены — на третьей текстовой линии: ноги бойцов и
    // счёт боя стоят на одной базовой линии. Текстовый блок сдвигается за
    // ячейку целиком, с тем же зазором, что у маркера до имени.
    const Scene *scene = NULL;
    int line_y3 = r.y + 4 + f->cell_height * 2;
    if (sprites && live && session->scene_ready && r.h >= f->cell_height * 3 + 6) {
        scene = &session->scene;
        Rect cell = { marker.x, r.y + 2, SCENE_WIDTH, r.h - 4 };
        int cx0 = cell.x > clip.x ? cell.x : clip.x;
        int cy0 = cell.y > clip.y ? cell.y : clip.y;
        int cx1 = cell.x + cell.w < clip.x + clip.w ? cell.x + cell.w : clip.x + clip.w;
        int cy1 = cell.y + cell.h < clip.y + clip.h ? cell.y + cell.h : clip.y + clip.h;
        if (cx1 > cx0 && cy1 > cy0) {
            EndScissorMode();
            BeginScissorMode(cx0, cy0, cx1 - cx0, cy1 - cy0);
            float floor_y = (float)(line_y3 + f->cell_height - 2);
            scene_draw(scene, sprites, (float)(cell.x + 14), floor_y, 1.0f, th->badge_dead);
            EndScissorMode();
            BeginScissorMode(clip.x, clip.y, clip.w, clip.h);
        }
        text_x = cell.x + cell.w + 6;
    } else {
        ui_draw_marker(marker, live, session ? session->state : SESSION_STATE_IDLE,
                       time, th);
    }

    int avail = r.x + r.w - 14 - text_x;

    // Родитель с подпроектами получает шеврон у самого левого края, перед
    // ячейкой маркера. Имя при этом не сдвигается: первая версия ставила
    // глиф-стрелку перед именем, и родитель уезжал вправо ровно на отступ
    // подпроекта — они читались одним уровнем.
    if (row->has_subs) {
        float cx = (float)(r.x + ROW_MARKER_PAD / 2);
        float cy = (float)(marker.y + marker.h / 2);
        Color c = th->row_text_dim;
        if (row->expanded) {
            DrawLineEx((Vector2){ cx - 2.5f, cy - 1.5f }, (Vector2){ cx, cy + 1.5f }, 1.5f, c);
            DrawLineEx((Vector2){ cx, cy + 1.5f }, (Vector2){ cx + 2.5f, cy - 1.5f }, 1.5f, c);
        } else {
            DrawLineEx((Vector2){ cx - 1.5f, cy - 2.5f }, (Vector2){ cx + 1.5f, cy }, 1.5f, c);
            DrawLineEx((Vector2){ cx + 1.5f, cy }, (Vector2){ cx - 1.5f, cy + 2.5f }, 1.5f, c);
        }
    }

    // Остаток контекста — справа на первой строке. Под курсором там кнопка
    // страницы, и столбец уступает ей место.
    if (session && !hovered && session_has_term(session))
        avail -= draw_ctx_column(session, r, r.y + 4, f, th, ctx_warn, ctx_crit);

    Color name_color = th->row_text;
    if (!session) name_color = th->row_text_dim;
    else if (session->state == SESSION_STATE_DEAD) name_color = th->badge_dead;
    int nw = ui_text_clipped(f, name, text_x, r.y + 4, name_color, avail);

    // Сколько подпроектов свёрнуто — как у группы, числом после имени.
    if (row->has_subs && !row->expanded && row->hidden > 0) {
        char n[16];
        snprintf(n, sizeof(n), "· %d", row->hidden);
        ui_text_clipped(f, n, text_x + nw + f->cell_width, r.y + 4,
                        th->group_label, avail - nw - f->cell_width);
    }

    // Вторая строка есть только у открытых: чем занята сессия — состояние
    // и, если агент назвал работу, её название. «Зовёт» — цветом внимания:
    // это единственное состояние, которое просит человека подойти.
    if (session && r.h >= f->cell_height * 2 + 6) {
        Color sub = session->state == SESSION_STATE_ATTENTION
                  ? th->badge_attention : th->row_text_dim;
        int line_y2 = r.y + 4 + f->cell_height;
        int avail2 = avail;

        // Сумма проекта — справа на второй линии, приглушённо, тем же правым
        // краем, что столбец контекста: сколько токенов агент написал в
        // этом проекте за всё время. Под курсором уступает кнопке страницы.
        // После победы итог боя «+X» перелетает сюда, и сумма докручивается.
        long total = live ? xp_tokens(session->cwd) : 0;
        if (total > 0 && !hovered) {
            long show = total;
            float fly_t = -1;
            if (scene && scene->mood == SCENE_WIN && scene->result > 0) {
                if (scene->clock < 0.9f) { show = total - scene->result; fly_t = scene->clock / 0.9f; }
                else if (scene->clock < 1.7f) {
                    float k = (scene->clock - 0.9f) / 0.8f;
                    show = total - scene->result + (long)((float)scene->result * k);
                }
                if (show < 0) show = 0;
            }
            char sum[24], label[32];
            fmt_tokens(show, sum, sizeof(sum));
            snprintf(label, sizeof(label), "Σ %s", sum);
            int lw = (int)(utf8_len(label)) * f->cell_width;
            int lx = r.x + r.w - 14 - lw;
            ui_text_clipped(f, label, lx, line_y2, th->row_text_dim, lw + 2);
            avail2 -= lw + f->cell_width;

            // Перелёт «+X»: от счёта боя на третьей линии к сумме, по дуге,
            // тая к концу пути.
            if (fly_t >= 0) {
                char n[16], plus[20];
                fmt_tokens(scene->result, n, sizeof(n));
                snprintf(plus, sizeof(plus), "+%s", n);
                float x0 = (float)text_x + 12.0f, y0 = (float)line_y3;
                float x1 = (float)lx, y1 = (float)line_y2;
                float ease = fly_t * fly_t * (3.0f - 2.0f * fly_t);
                float px = x0 + (x1 - x0) * ease;
                float py = y0 + (y1 - y0) * ease - 10.0f * sinf(fly_t * 3.14159f);
                Color c = th->progress_fill;
                if (fly_t > 0.7f) c.a = (unsigned char)(255 * (1.0f - (fly_t - 0.7f) / 0.3f));
                ui_text_clipped(f, plus, (int)px, (int)py, c, f->cell_width * 8);
            }
        }
        ui_text_clipped(f, session_subtitle(session), text_x, line_y2, sub, avail2);
    }

    // Третья линия при сжатии — подпись вместо счёта: бой на паузе.
    if (scene && scene->mood == SCENE_COMPACT)
        ui_text_clipped(f, "сжатие контекста…", text_x, line_y3, th->row_text_dim, avail);

    // Третья линия — счёт боя: точка, что дышит в такт и вспыхивает на
    // попадании, число токенов с начала боя и приглушённая единица «tok».
    if (scene && scene->mood == SCENE_FIGHT && scene->enemy_phase == ENEMY_FIGHT) {
        char score[24];
        fmt_tokens(scene_fight_score(scene), score, sizeof(score));
        float grow = scene->bump > 0 ? 1.0f + 0.4f * (scene->bump / 0.28f) : 1.0f;
        float size = (float)f->size * grow;
        float breath = 0.5f + 0.5f * sinf((float)time * 4.0f);
        float rad = 2.0f + 0.8f * breath + (scene->bump > 0 ? 1.5f * (scene->bump / 0.28f) : 0);
        Color dot = th->row_text;
        dot.a = (unsigned char)(255 * (0.55f + 0.45f * breath));
        DrawCircle(text_x + 4, line_y3 + f->cell_height / 2, rad, dot);
        Font face = font_for(f, score);
        float tx = (float)text_x + 12.0f;
        // Растёт от базовой линии: цифра подскакивает, а не сползает.
        float ty = (float)line_y3 + (float)f->cell_height - size * ((float)f->cell_height / (float)f->size);
        DrawTextEx(face, score, (Vector2){ tx, ty }, size, 0, th->row_text);
        int sw = (int)strlen(score) * f->cell_width;
        ui_text_clipped(f, "tok", text_x + 12 + sw + f->cell_width / 2, line_y3,
                        th->row_text_dim, f->cell_width * 3);
    }

    // Кнопка страницы проекта — только под курсором: держать её на каждой
    // строке значило бы засорить панель ради жеста, который делают редко.
    if (hovered && (session || project)) {
        Rect b = layout_row_info_rect(r, f->cell_width);
        DrawRectangle(b.x, b.y, b.w, b.h, th->row_hover_bg);
        // ⓘ из Nerd Font: значок «сведения», он же намекает, что откроется
        // карточка проекта, а не ещё одна вкладка.
        font_draw_codepoint(f, 0xF05A, (float)(b.x + 4), (float)(b.y + 4),
                            (float)f->size, th->row_text_dim);
    }

    if (session && session->progress >= 0)
        draw_progress(th, r, session->progress);
}

// Строка фоновой задачи: с отступом под проектом, одной строкой. Маркер
// тот же, что у проектов: задача либо работает, либо закончилась.
static void draw_task_row(const PanelRow *row, const Session *session,
                          const FontAtlas *f, const Theme *th,
                          bool active, bool hovered, double time)
{
    Rect r = row->rect;

    if (active)       draw_active_mark(r, th);
    else if (hovered) DrawRectangle(r.x, r.y, r.w, r.h, th->row_hover_bg);

    int line_y = r.y + 3;
    Rect cell = layout_row_marker_rect(r, line_y, f->cell_height, row->depth);
    draw_guides(row, cell, th);
    ui_draw_marker(cell, true, session->state, time, th);

    char label[96];
    snprintf(label, sizeof(label), "%s · %s", session->name,
             session_task_state_text(session));
    int text_x = r.x + ROW_TEXT_X + row->depth * ROW_INDENT;
    ui_text_clipped(f, label, text_x, line_y,
                    active ? th->row_text : th->row_text_dim, r.x + r.w - 14 - text_x);
}

// Балун под окном лимита: что это, когда обновится, откуда цифры. Висит
// под полосой, правым краем у своего окна, и рисуется поверх всего —
// поэтому полоса идёт в кадре последней.
static void draw_usage_tip(const Usage *u, int index, const FontAtlas *f,
                           const Theme *th, Rect bar, Rect item)
{
    char text[320];
    usage_tip_text(u, index, text, sizeof(text));
    if (!text[0]) return;

    // Строки — по переводам строки; ширина балуна по самой длинной.
    const char *lines[4];
    int lens[4], n = 0;
    for (const char *p = text; *p && n < 4; ) {
        const char *e = strchr(p, '\n');
        lines[n] = p;
        lens[n] = (int)(e ? e - p : (long)strlen(p));
        n++;
        if (!e) break;
        p = e + 1;
    }
    int cells = 0;
    for (int i = 0; i < n; i++) {
        char line[320];
        snprintf(line, sizeof(line), "%.*s", lens[i], lines[i]);
        int c = (int)strlen_utf8_cells(line);
        if (c > cells) cells = c;
    }

    const int padx = 12, pady = 8, gap = 3, nose = 6;
    int w = cells * f->cell_width + padx * 2;
    int h = n * f->cell_height + (n - 1) * gap + pady * 2;
    int x = item.x + item.w - w;
    if (x < bar.x + 8) x = bar.x + 8;
    int y = bar.y + bar.h + nose;

    // Носик к своему окну — чтобы было видно, к какому из трёх подсказка.
    int nx = item.x + item.w / 2;
    if (nx < x + 10) nx = x + 10;
    if (nx > x + w - 10) nx = x + w - 10;
    DrawTriangle((Vector2){ (float)nx, (float)(y - nose) },
                 (Vector2){ (float)(nx - nose), (float)y + 1 },
                 (Vector2){ (float)(nx + nose), (float)y + 1 }, th->sidebar_border);
    DrawTriangle((Vector2){ (float)nx, (float)(y - nose + 2) },
                 (Vector2){ (float)(nx - nose + 2), (float)y + 1 },
                 (Vector2){ (float)(nx + nose - 2), (float)y + 1 }, th->row_active_bg);

    DrawRectangle(x - 1, y - 1, w + 2, h + 2, th->sidebar_border);
    DrawRectangle(x, y, w, h, th->row_active_bg);

    int ty = y + pady;
    for (int i = 0; i < n; i++) {
        char line[320];
        snprintf(line, sizeof(line), "%.*s", lens[i], lines[i]);
        // Первая строка — про что окно; последняя — откуда цифры, приглушённо.
        Color c = (i == n - 1 && n > 2) ? th->row_text_dim : th->row_text;
        ui_text_clipped(f, line, x + padx, ty, c, w - padx * 2 + f->cell_width);
        ty += f->cell_height + gap;
    }
}

// Лимиты справа, перед кнопкой настроек: сначала самый жгучий. Возвращает
// левый край нарисованного — по нему обрезается имя вкладки слева, чтобы они
// не наехали друг на друга в узком окне. Окно под курсором получает балун
// с подробностями: когда обновится и откуда цифры.
static int draw_usage(const Usage *u, const FontAtlas *f, const Theme *th,
                      Rect bar, int right, Vector2 mouse)
{
    if (!u || u->count == 0) return right;

    // Строки готовим заранее: рисуем справа налево, и надо знать ширину.
    char parts[USAGE_LIMIT_MAX][USAGE_LABEL_MAX + 24];
    Color colors[USAGE_LIMIT_MAX];
    int n = 0;
    for (int i = 0; i < u->count && n < USAGE_LIMIT_MAX; i++) {
        const UsageLimit *l = &u->items[i];

        // Когда до предела далеко, время сброса — лишний шум; когда близко,
        // это единственное, что человек хочет знать. Остальное — в балуне.
        char when[24] = "";
        if (l->severity != USAGE_NORMAL || l->percent >= 80)
            usage_reset_text(l->resets_at, when, sizeof(when));

        snprintf(parts[n], sizeof(parts[n]), "%s %d%%%s%s", l->label, l->percent,
                 when[0] ? " · " : "", when);
        colors[n] = l->severity == USAGE_CRITICAL ? th->badge_dead
                  : l->severity == USAGE_WARNING  ? th->badge_attention
                                                  : th->row_text_dim;
        n++;
    }

    // Места мало — отбрасываем хвост: первым в списке идёт то окно, которое
    // упрётся раньше.
    const int gap = f->cell_width * 2;
    int x = right;
    int drawn = 0;
    int widths[USAGE_LIMIT_MAX];
    for (int i = 0; i < n; i++) {
        widths[i] = (int)strlen_utf8_cells(parts[i]) * f->cell_width;
        int need = widths[i] + (drawn ? gap : 0);
        if (x - need < bar.x + f->cell_width * 14) break;   // место под имя вкладки
        x -= need;
        drawn++;
    }

    int ty = bar.y + (bar.h - f->cell_height) / 2;
    int cx = x;
    int hovered = -1;
    Rect hovered_rect = { 0, 0, 0, 0 };
    for (int i = 0; i < drawn; i++) {
        // Зона наведения — вся высота полосы: в строку высотой в глиф
        // попадать курсором неудобно.
        Rect item = { cx - f->cell_width / 2, bar.y, widths[i] + f->cell_width, bar.h };
        bool hover = mouse.x >= item.x && mouse.x < item.x + item.w
                  && mouse.y >= item.y && mouse.y < item.y + item.h;
        if (hover) { hovered = i; hovered_rect = item; }
        ui_text_clipped(f, parts[i], cx, ty,
                        hover ? th->row_text : colors[i], widths[i] + f->cell_width);
        cx += widths[i] + gap;
    }
    if (hovered >= 0) draw_usage_tip(u, hovered, f, th, bar, hovered_rect);
    return drawn ? x : right;
}

void ui_draw_topbar(const Layout *l, const SessionList *sessions,
                    const Usage *usage,
                    const FontAtlas *font, const Theme *theme, Vector2 mouse)
{
    Rect t = l->topbar;
    DrawRectangle(t.x, t.y, t.w, t.h, theme->sidebar_bg);
    DrawRectangle(t.x, t.y + t.h - 1, t.w, 1, theme->sidebar_border);

    // Лимиты — свойство человека, а не вкладки: они одни на все окна и все
    // проекты, поэтому им место здесь, а не на странице проекта.
    int usage_x = draw_usage(usage, font, theme, t,
                             l->topbar_settings.x - font->cell_width * 2, mouse);

    // Слева — над чем работаем: то же, что в заголовке окна. Заголовок
    // окна в полноэкранном режиме не виден, а здесь — всегда.
    const Session *s = sessions->active >= 0 ? &sessions->items[sessions->active] : NULL;
    if (s) {
        char title[320];
        session_window_title(s, title, sizeof(title));
        ui_text_clipped(font, title, t.x + 12, t.y + (t.h - font->cell_height) / 2,
                        theme->row_text_dim, usage_x - t.x - 24);
    }

    Rect b = l->topbar_settings;
    bool hover = mouse.x >= b.x && mouse.x < b.x + b.w
              && mouse.y >= b.y && mouse.y < b.y + b.h;
    DrawRectangle(b.x, b.y, b.w, b.h, hover ? theme->row_hover_bg : theme->row_active_bg);
    // Шестерёнки в JetBrains Mono нет — рисуем кодпоинтом, чтобы глиф взялся
    // из запаски, а не превратился в вопросительный знак.
    int ty = b.y + (b.h - font->cell_height) / 2;
    font_draw_codepoint(font, 0x2699, (float)(b.x + 10), (float)ty,
                        (float)font->size, theme->row_text_dim);
    ui_text_clipped(font, "Настройки", b.x + 10 + font->cell_width * 2, ty,
                    theme->row_text_dim, b.w - font->cell_width * 2 - 16);
}

void ui_draw_sidebar(const Layout *l, const ProjectList *projects,
                     const SessionList *sessions,
                     const FontAtlas *font, const Theme *theme,
                     Vector2 mouse, bool splitter_active,
                     const Sprites *sprites)
{
    if (!l->sidebar_visible) return;

    DrawRectangle(l->sidebar.x, l->sidebar.y, l->sidebar.w, l->sidebar.h,
                  theme->sidebar_bg);

    const PanelRow *hovered = layout_hit_row(l, mouse);

    // Панель прокручивается, поэтому строки обрезаем по её границам —
    // иначе верхняя уезжала бы поверх заголовка окна.
    BeginScissorMode(l->sidebar.x, l->sidebar.y, l->sidebar.w, l->sidebar.h);

    // Одно время на весь кадр: маркеры соседних строк идут в такт.
    double now = GetTime();

    for (int i = 0; i < l->row_count; i++) {
        const PanelRow *row = &l->rows[i];

        // За пределами видимой части не рисуем: при сотне проектов это
        // заметная доля кадра.
        if (row->rect.y + row->rect.h < l->sidebar.y) continue;
        if (row->rect.y > l->sidebar.y + l->sidebar.h) break;

        if (row->kind == PANEL_ROW_GROUP) {
            draw_group_row(row, font, theme, hovered == row);
            continue;
        }
        if (row->kind == PANEL_ROW_ADD_GROUP) {
            draw_add_group_row(row, font, theme, hovered == row);
            continue;
        }

        const Project *project = row->project >= 0
            ? &projects->items[row->project] : NULL;
        const Session *session = row->session >= 0
            ? &sessions->items[row->session] : NULL;

        bool active = row->session >= 0 && row->session == sessions->active;
        if (row->kind == PANEL_ROW_TASK && session)
            draw_task_row(row, session, font, theme, active, hovered == row, now);
        else
            draw_item_row(row, project, session, font, theme, active, hovered == row, now,
                          l->ctx_warn, l->ctx_crit, sprites, l->sidebar);
    }

    EndScissorMode();

    DrawRectangle(l->splitter.x, l->splitter.y, l->splitter.w, l->splitter.h,
                  splitter_active ? theme->splitter_hover : theme->splitter);
}
