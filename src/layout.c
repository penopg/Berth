#include <stdio.h>
#include <string.h>
#include <time.h>

#include "layout.h"

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void layout_init(Layout *l, LayoutMetrics metrics)
{
    memset(l, 0, sizeof(*l));
    l->metrics = metrics;
    l->sidebar_width = 240;
    l->sidebar_visible = true;
    l->collapsed_show_live = true;
    l->ctx_warn = 30;
    l->ctx_crit = 60;
    l->sleep_after = 30 * 60;
}

bool layout_group_collapsed(const Layout *l, const char *group)
{
    for (int i = 0; i < l->collapsed_count; i++)
        if (!strcmp(l->collapsed[i], group)) return true;
    return false;
}

void layout_set_group_collapsed(Layout *l, const char *group, bool collapsed)
{
    for (int i = 0; i < l->collapsed_count; i++) {
        if (strcmp(l->collapsed[i], group)) continue;
        if (collapsed) return;
        l->collapsed_count--;
        memmove(&l->collapsed[i], &l->collapsed[i + 1],
                sizeof(l->collapsed[0]) * (size_t)(l->collapsed_count - i));
        return;
    }
    if (!collapsed || l->collapsed_count >= LAYOUT_COLLAPSED_MAX) return;
    snprintf(l->collapsed[l->collapsed_count++], PROJECT_NAME_MAX, "%s", group);
}

bool layout_parent_expanded(const Layout *l, const char *path)
{
    for (int i = 0; i < l->expanded_count; i++)
        if (!strcmp(l->expanded[i], path)) return true;
    return false;
}

void layout_set_parent_expanded(Layout *l, const char *path, bool expanded)
{
    for (int i = 0; i < l->expanded_count; i++) {
        if (strcmp(l->expanded[i], path)) continue;
        if (expanded) return;
        l->expanded_count--;
        memmove(&l->expanded[i], &l->expanded[i + 1],
                sizeof(l->expanded[0]) * (size_t)(l->expanded_count - i));
        return;
    }
    if (!expanded || l->expanded_count >= LAYOUT_COLLAPSED_MAX) return;
    snprintf(l->expanded[l->expanded_count++], PROJECT_PATH_MAX, "%s", path);
}

void layout_set_sidebar_width(Layout *l, int width)
{
    l->sidebar_width = clampi(width, SIDEBAR_WIDTH_MIN, SIDEBAR_WIDTH_MAX);
}

void layout_toggle_sidebar(Layout *l)
{
    l->sidebar_visible = !l->sidebar_visible;
}

void layout_scroll_by(Layout *l, int delta)
{
    l->scroll += delta;
}

// Вкладка держит строку на виду под свёрнутым заголовком, только если в
// ней живёт процесс: страница проекта без агента — не разговор, и прятать
// её вместе с остальными можно. Иначе стрелка у родителя, чьи подпроекты
// открыты страницами, ничего не сворачивала бы.
static bool live_row(const SessionList *sessions, int session)
{
    return session >= 0 && session_has_term(&sessions->items[session]);
}

// Высота строки открытой вкладки. Спящий разговор — свободный дольше
// порога — занимает одну строку, как закрытый проект: второй строкой ему
// сказать нечего. Активная вкладка всегда полной высоты: в ней сидят.
static int session_row_height(const Layout *l, const SessionList *sessions,
                              int session, time_t now)
{
    if (session < 0) return l->metrics.row_height_idle;
    // Вкладка-страница без процесса — одной строкой, как закрытый проект:
    // подпись «страница проекта» ничего не говорила, а место занимала.
    if (!session_has_term(&sessions->items[session])) return l->metrics.row_height_idle;
    if (session != sessions->active && l->sleep_after > 0
        && session_sleeping(&sessions->items[session], now, l->sleep_after))
        return l->metrics.row_height_idle;
    return l->metrics.row_height;
}

// Разговор проекта, либо -1. Он один: прошлые разговоры живут в журнале как
// история, а не отдельными процессами в панели.
static int session_row_of(const SessionList *sessions, int project)
{
    for (int i = 0; i < sessions->count; i++)
        if (sessions->items[i].project == project
            && sessions->items[i].role == SESSION_ROLE_MAIN)
            return i;
    return -1;
}

static PanelRow *push_row(Layout *l, PanelRowKind kind, int y, int height,
                          int project, int session, const char *label)
{
    if (l->row_count >= PANEL_ROW_MAX) return NULL;
    l->rows[l->row_count] = (PanelRow){
        .kind = kind,
        .rect = { l->sidebar.x, y, l->sidebar.w, height },
        .project = project,
        .session = session,
        .label = label,
    };
    return &l->rows[l->row_count++];
}

// Задачи проекта — строками под ним. Разговор остаётся один, а работа
// рядом с ним должна быть видна: иначе она идёт вслепую.
// Возвращает последнюю добавленную строку, либо NULL.
static PanelRow *push_task_rows(Layout *l, const SessionList *sessions,
                                int project, int *y, int depth)
{
    PanelRow *last = NULL;
    for (int k = 0; k < sessions->count; k++) {
        if (sessions->items[k].role != SESSION_ROLE_TASK) continue;
        if (sessions->items[k].project != project) continue;
        PanelRow *row = push_row(l, PANEL_ROW_TASK, *y, l->metrics.task_height, project, k, NULL);
        if (row) { row->depth = depth; last = row; }
        *y += l->metrics.task_height;
    }
    return last;
}

// Ребёнок добавлен под родителя: у родителя начинается направляющая, а
// последним ребёнком пока считается этот — следующий его сменит.
static void link_child(PanelRow *parent, PanelRow **last_child, PanelRow *child)
{
    if (!child) return;
    if (parent) parent->has_children = true;
    if (*last_child) (*last_child)->last = false;
    child->last = true;
    *last_child = child;
}

// Строки панели: проекты, сгруппированные так же, как в списке, а следом —
// вкладки, которых в списке нет. Открытый проект занимает две строки текста
// (имя и чем занят), закрытый — одну: закрытых обычно большинство, и им
// незачем занимать место.
static void build_rows(Layout *l, const ProjectList *projects,
                       const SessionList *sessions)
{
    l->row_count = 0;
    if (!l->sidebar_visible) return;

    const int top = l->sidebar.y + 6;
    int y = top - l->scroll;

    bool project_done[PROJECT_MAX] = {0};
    time_t now = time(NULL);

    for (int i = 0; i < projects->count; i++) {
        if (project_done[i]) continue;

        const char *group = projects->items[i].group;
        bool collapsed = group[0] && layout_group_collapsed(l, group);
        PanelRow *head = NULL;
        if (group[0]) {
            head = push_row(l, PANEL_ROW_GROUP, y, l->metrics.group_height, -1, -1, group);
            if (head) {
                head->collapsed = collapsed;
                head->color = projects->items[i].color;
                for (int j = i; j < projects->count; j++)
                    if (!strcmp(projects->items[j].group, group) && projects->items[j].parent < 0)
                        head->count++;
            }
            y += l->metrics.group_height;
        }

        for (int j = i; j < projects->count; j++) {
            if (project_done[j]) continue;
            if (strcmp(projects->items[j].group, group) != 0) continue;
            // Подпроекты идут под своим родителем, а не в общем порядке.
            if (projects->items[j].parent >= 0) continue;

            project_done[j] = true;
            int session = session_row_of(sessions, j);

            // В свёрнутой группе проект прячется — кроме открытого, если
            // человек так настроил: разговор не должен пропадать с глаз.
            bool hidden = collapsed && !(l->collapsed_show_live && live_row(sessions, session));
            if (hidden && head) head->hidden++;

            PanelRow *prow = NULL, *last_child = NULL;
            if (!hidden) {
                int h = session_row_height(l, sessions, session, now);
                prow = push_row(l, PANEL_ROW_ITEM, y, h, j, session, NULL);
                y += h;
                link_child(prow, &last_child, push_task_rows(l, sessions, j, &y, 1));
            }

            // Подпроекты — строками с отступом под родителем. Свёрнуты, пока
            // человек не раскрыл; открытый подпроект виден и так, по той же
            // логике, что и открытый проект в свёрнутой группе.
            bool expanded = layout_parent_expanded(l, projects->items[j].path);
            int sub_tasks_from = -1;   // задачи предыдущего подпроекта: сквозь них
                                       // пройдёт направляющая, если он не последний
            for (int k = j + 1; k < projects->count; k++) {
                if (projects->items[k].parent != j) continue;
                project_done[k] = true;
                if (prow) prow->has_subs = true;
                if (hidden) continue;
                int sub_session = session_row_of(sessions, k);
                if (!expanded && !(l->collapsed_show_live && live_row(sessions, sub_session))) {
                    if (prow) prow->hidden++;
                    continue;
                }
                if (sub_tasks_from >= 0)
                    for (int t = sub_tasks_from; t < l->row_count; t++) l->rows[t].cont = true;
                int h = session_row_height(l, sessions, sub_session, now);
                PanelRow *srow = push_row(l, PANEL_ROW_ITEM, y, h, k, sub_session, NULL);
                if (srow) srow->depth = 1;
                y += h;
                link_child(prow, &last_child, srow);
                sub_tasks_from = l->row_count;
                PanelRow *sub_last = NULL;
                link_child(srow, &sub_last, push_task_rows(l, sessions, k, &y, 2));
            }
            if (prow) prow->expanded = expanded;
        }

        y += 6;
    }

    // Вкладки вне списка проектов: открытые из командной строки или по ⌘T.
    // Все они живые, поэтому свёрнутая группа прячет их только при
    // выключенном показе открытых.
    bool has_loose = false;
    bool loose_collapsed = layout_group_collapsed(l, "прочее");
    PanelRow *loose_head = NULL;
    for (int i = 0; i < sessions->count; i++) {
        if (sessions->items[i].project >= 0) continue;
        if (!has_loose) {
            has_loose = true;
            loose_head = push_row(l, PANEL_ROW_GROUP, y, l->metrics.group_height, -1, -1, "прочее");
            if (loose_head) loose_head->collapsed = loose_collapsed;
            y += l->metrics.group_height;
        }
        if (loose_collapsed && !l->collapsed_show_live) {
            if (loose_head) loose_head->hidden++;
            continue;
        }
        int h = session_row_height(l, sessions, i, now);
        push_row(l, PANEL_ROW_ITEM, y, h, -1, i, NULL);
        y += h;
    }

    // «Добавить группу» — последней строкой: это действие над списком, а не
    // его часть, и стоять ему после всех групп.
    y += 6;
    push_row(l, PANEL_ROW_ADD_GROUP, y, l->metrics.group_height, -1, -1, "Добавить группу");
    y += l->metrics.group_height;

    l->content_height = y + l->scroll - top;

    // Прокрутку ограничиваем после подсчёта содержимого: пока строки не
    // построены, неизвестно, есть ли куда прокручивать.
    int max_scroll = l->content_height - l->sidebar.h + 12;
    if (max_scroll < 0) max_scroll = 0;
    int clamped = clampi(l->scroll, 0, max_scroll);
    if (clamped != l->scroll) {
        int shift = l->scroll - clamped;
        l->scroll = clamped;
        for (int i = 0; i < l->row_count; i++)
            l->rows[i].rect.y += shift;
    }
}

void layout_compute(Layout *l, const ProjectList *projects,
                    const SessionList *sessions, int win_w, int win_h)
{
    l->window = (Rect){ 0, 0, win_w, win_h };

    // Полоса сверху идёт над панелью и терминалом вместе: она про окно.
    int th = l->metrics.topbar_height;
    l->topbar = (Rect){ 0, 0, win_w, th };
    int bw = l->metrics.settings_width;
    l->topbar_settings = (Rect){ win_w - bw - 8, 4, bw, th - 8 };
    int top = th;
    int h = win_h - th;

    if (l->sidebar_visible) {
        layout_set_sidebar_width(l, l->sidebar_width);
        // Панель не должна съедать окно целиком на узких экранах.
        int max_w = win_w / 2;
        int sw = l->sidebar_width < max_w ? l->sidebar_width : max_w;

        l->sidebar  = (Rect){ 0, top, sw, h };
        l->splitter = (Rect){ sw, top, SPLITTER_WIDTH, h };
        l->term     = (Rect){ sw + SPLITTER_WIDTH, top,
                              win_w - sw - SPLITTER_WIDTH, h };
    } else {
        l->sidebar  = (Rect){ 0, top, 0, h };
        l->splitter = (Rect){ 0, top, 0, h };
        l->term     = (Rect){ 0, top, win_w, h };
    }

    if (l->term.w < 1) l->term.w = 1;
    if (l->term.h < 1) l->term.h = 1;

    build_rows(l, projects, sessions);
}

static bool inside(Rect r, Vector2 p)
{
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

const PanelRow *layout_hit_row(const Layout *l, Vector2 mouse)
{
    if (!l->sidebar_visible) return NULL;
    for (int i = 0; i < l->row_count; i++) {
        // Кликается всё, и заголовок группы тоже: он сворачивает её.
        if (inside(l->rows[i].rect, mouse)) return &l->rows[i];
    }
    return NULL;
}

bool layout_hit_topbar(const Layout *l, Vector2 mouse)
{
    return inside(l->topbar, mouse);
}

bool layout_hit_settings(const Layout *l, Vector2 mouse)
{
    return inside(l->topbar_settings, mouse);
}

bool layout_hit_splitter(const Layout *l, Vector2 mouse)
{
    if (!l->sidebar_visible) return false;
    Rect grab = {
        l->splitter.x - SPLITTER_GRAB,
        l->splitter.y,
        l->splitter.w + SPLITTER_GRAB * 2,
        l->splitter.h,
    };
    return inside(grab, mouse);
}

int layout_session_at(const Layout *l, int nth)
{
    if (nth < 0) return -1;
    int seen = 0;
    for (int i = 0; i < l->row_count; i++) {
        if (l->rows[i].kind != PANEL_ROW_ITEM || l->rows[i].session < 0) continue;
        if (seen == nth) return l->rows[i].session;
        seen++;
    }
    return -1;
}

int layout_position_of(const Layout *l, int session)
{
    int seen = 0;
    for (int i = 0; i < l->row_count; i++) {
        if (l->rows[i].kind != PANEL_ROW_ITEM || l->rows[i].session < 0) continue;
        if (l->rows[i].session == session) return seen;
        seen++;
    }
    return -1;
}

int layout_session_count(const Layout *l)
{
    int n = 0;
    for (int i = 0; i < l->row_count; i++)
        if (l->rows[i].kind == PANEL_ROW_ITEM && l->rows[i].session >= 0) n++;
    return n;
}
