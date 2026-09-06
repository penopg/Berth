#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "settings.h"

#define FONT_SIZE_LO   9
#define FONT_SIZE_HI  40
#define SIDEBAR_LO   160
#define SIDEBAR_HI   460

// Время правки файла на момент нашего последнего чтения или записи. Нужно,
// чтобы отличить правку снаружи от собственной: сохраняя настройки, мы не
// должны тут же перечитывать их обратно.
static time_t g_mtime;

static void remember_mtime(const char *path)
{
    struct stat st;
    g_mtime = (stat(path, &st) == 0) ? st.st_mtime : 0;
}

void settings_defaults(Settings *s)
{
    s->font_size       = 16;
    s->sidebar_width   = 240;
    s->sidebar_visible = true;
    s->open_project_page = true;
    s->task_finish     = TASK_FINISH_REVIEW;
    s->collapsed_show_live = true;
    s->usage_fetch     = true;
    s->ctx_warn        = 30;
    s->ctx_crit        = 60;
    s->sleep_after     = 30;
    s->resume_within   = 24;
    snprintf(s->default_agent, sizeof(s->default_agent), "claude");
    snprintf(s->task_model, sizeof(s->task_model), "sonnet");
    snprintf(s->theme_panel, sizeof(s->theme_panel), "berth");
    snprintf(s->theme_window, sizeof(s->theme_window), "project");
    snprintf(s->marker, sizeof(s->marker), "karateka");
}

bool settings_scene_enabled(const Settings *s)
{
    const char *env = getenv("BERTH_MARKER");
    if (env && *env) return !strcmp(env, "karateka");
    return !strcmp(s->marker, "karateka");
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static bool parse_bool(const char *v, bool fallback)
{
    if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1"))  return true;
    if (!strcmp(v, "no")  || !strcmp(v, "false") || !strcmp(v, "0")) return false;
    return fallback;
}

static int parse_int(const char *v, int lo, int hi, int fallback)
{
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || n < lo || n > hi) return fallback;
    return (int)n;
}

bool settings_load(Settings *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = trim(p);
        char *val = trim(eq + 1);

        if (!strcmp(key, "font_size"))
            s->font_size = parse_int(val, FONT_SIZE_LO, FONT_SIZE_HI, s->font_size);
        else if (!strcmp(key, "sidebar_width"))
            s->sidebar_width = parse_int(val, SIDEBAR_LO, SIDEBAR_HI, s->sidebar_width);
        else if (!strcmp(key, "sidebar_visible"))
            s->sidebar_visible = parse_bool(val, s->sidebar_visible);
        else if (!strcmp(key, "open_project"))
            s->open_project_page = !strcmp(val, "page") ? true
                                 : !strcmp(val, "agent") ? false
                                 : s->open_project_page;
        else if (!strcmp(key, "usage_fetch"))
            s->usage_fetch = parse_bool(val, s->usage_fetch);
        else if (!strcmp(key, "collapsed_show_live"))
            s->collapsed_show_live = parse_bool(val, s->collapsed_show_live);
        else if (!strcmp(key, "task_finish"))
            tasks_finish_parse(val, &s->task_finish);
        else if (!strcmp(key, "theme_panel") && *val)
            snprintf(s->theme_panel, sizeof(s->theme_panel), "%s", val);
        else if ((!strcmp(key, "theme_window") || !strcmp(key, "theme_terminal")) && *val)
            snprintf(s->theme_window, sizeof(s->theme_window), "%s", val);
        else if (!strcmp(key, "task_model"))
            snprintf(s->task_model, sizeof(s->task_model), "%s", val);
        else if (!strcmp(key, "ctx_warn"))
            s->ctx_warn = parse_int(val, 0, 100, s->ctx_warn);
        else if (!strcmp(key, "ctx_crit"))
            s->ctx_crit = parse_int(val, 0, 100, s->ctx_crit);
        else if (!strcmp(key, "sleep_after"))
            s->sleep_after = parse_int(val, 0, 100000, s->sleep_after);
        else if (!strcmp(key, "resume_within"))
            s->resume_within = parse_int(val, 0, 100000, s->resume_within);
        else if (!strcmp(key, "skills_off"))
            snprintf(s->skills_off, sizeof(s->skills_off), "%s", val);
        else if (!strcmp(key, "marker") && *val)
            snprintf(s->marker, sizeof(s->marker), "%s", val);
        else if (!strcmp(key, "default_agent") && *val)
            snprintf(s->default_agent, sizeof(s->default_agent), "%s", val);
    }

    fclose(f);
    remember_mtime(path);
    return true;
}

bool settings_save(const Settings *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f,
        "# Настройки Berth. Правится руками — изменения подхватываются на лету,\n"
        "# перезапускать не нужно. Файл перезаписывается, когда настройку\n"
        "# меняют из окна, так что свои комментарии сюда добавлять бесполезно.\n"
        "\n"
        "# Размер шрифта, %d..%d. Меняется на лету по ⌘+ / ⌘− / ⌘0.\n"
        "font_size = %d\n"
        "\n"
        "# Ширина боковой панели в точках, %d..%d. Тянется мышью за разделитель.\n"
        "sidebar_width = %d\n"
        "\n"
        "# Показывать ли боковую панель (⌘B).\n"
        "sidebar_visible = %s\n"
        "\n"
        "# Что открывается по клику на проект: page — страница проекта\n"
        "# (паспорт, история сессий, кнопки), agent — сразу агент.\n"
        "open_project = %s\n"
        "\n"
        "# Тема панели слева (с верхней полосой) — одна на окно. Тема окна справа\n"
        "# (терминал и страницы вкладки) — имя встроенной темы или project: как у\n"
        "# проекта в projects.json, а у проекта без темы — тема панели. Имена:\n"
        "# berth, berth light, Tokyo Night, Catppuccin Mocha, Catppuccin Latte,\n"
        "# Gruvbox Dark, Gruvbox Light.\n"
        "theme_panel = %s\n"
        "theme_window = %s\n"
        "\n"
        "# Каким агентом открывается проект: claude | shell.\n"
        "# ⌥-клик всегда открывает оболочку, независимо от этой строки.\n"
        "default_agent = %s\n"
        "\n"
        "# Что агент делает с задачей из .berth/tasks.md, когда закончил её:\n"
        "# review — отмечает «сделано, нужно проверить» ([?]), в сделанные её\n"
        "# переносит человек; done — сразу отмечает сделанной ([x]);\n"
        "# none — ничего не просить.\n"
        "task_finish = %s\n"
        "\n"
        "# Модель для служебных задач берта — сбор журнала и сводки (--model у\n"
        "# claude -p): sonnet | haiku | opus или полный id. Пусто — модель\n"
        "# Claude Code по умолчанию. Дорогая модель здесь не нужна, а лимит\n"
        "# она тратит так же, как разговор.\n"
        "task_model = %s\n"
        "\n"
        "# Показывать ли в свёрнутой группе панели проекты с открытым разговором.\n"
        "collapsed_show_live = %s\n"
        "\n"
        "# Лимиты Claude Code в верхней полосе: yes — берт спрашивает их у сервера\n"
        "# сам, токеном Claude Code из связки ключей (свежие, раз в три минуты);\n"
        "# no — только из кэша Claude Code, который обновляется редко.\n"
        "usage_fetch = %s\n"
        "\n"
        "# Выключенные скиллы берта, через запятую (например berth-tasks).\n"
        "# Скиллы берта едут с ним и подключаются к каждому разговору; выключенный\n"
        "# не подключается.\n"
        "skills_off = %s\n"
        "\n"
        "# Контекст разговора в панели, пороги в процентах занятого окна:\n"
        "# от ctx_warn — янтарный, от ctx_crit — красный.\n"
        "ctx_warn = %d\n"
        "ctx_crit = %d\n"
        "\n"
        "# Через сколько минут простоя свободный разговор сжимается в панели до\n"
        "# одной строки (0 — никогда). Активная вкладка всегда полной высоты.\n"
        "sleep_after = %d\n"
        "\n"
        "# При запуске поднимать процессом только разговоры с работой за последние\n"
        "# N часов; остальные вкладки открываются страницей, разговор продолжается\n"
        "# по кнопке тем же id. 0 — поднимать все.\n"
        "resume_within = %d\n"
        "\n"
        "# Маркер активного разговора в панели: dot — точка, karateka — сценка с\n"
        "# каратекой (бой, пока агент работает; победа; поклон, когда зовёт).\n"
        "marker = %s\n",
        FONT_SIZE_LO, FONT_SIZE_HI, s->font_size,
        SIDEBAR_LO, SIDEBAR_HI, s->sidebar_width,
        s->sidebar_visible ? "yes" : "no",
        s->open_project_page ? "page" : "agent",
        s->theme_panel, s->theme_window,
        s->default_agent,
        tasks_finish_name(s->task_finish),
        s->task_model,
        s->collapsed_show_live ? "yes" : "no",
        s->usage_fetch ? "yes" : "no",
        s->skills_off, s->ctx_warn, s->ctx_crit, s->sleep_after, s->resume_within,
        s->marker);

    fclose(f);
    remember_mtime(path);
    return true;
}

bool settings_changed(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return st.st_mtime != g_mtime;
}

bool settings_skill_enabled(const Settings *s, const char *name)
{
    const char *p = s->skills_off;
    size_t n = strlen(name);
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == n && !strncmp(p, name, n)) return false;
        p = e;
    }
    return true;
}

void settings_skill_set(Settings *s, const char *name, bool enabled)
{
    if (settings_skill_enabled(s, name) == enabled) return;
    if (!enabled) {
        size_t used = strlen(s->skills_off);
        snprintf(s->skills_off + used, sizeof(s->skills_off) - used, "%s%s",
                 used ? "," : "", name);
        return;
    }
    // Убрать имя из списка: собираем заново без него.
    char out[sizeof(s->skills_off)] = "";
    const char *p = s->skills_off;
    size_t n = strlen(name);
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if (e > p && !((size_t)(e - p) == n && !strncmp(p, name, n))) {
            size_t used = strlen(out);
            snprintf(out + used, sizeof(out) - used, "%s%.*s", used ? "," : "", (int)(e - p), p);
        }
        p = e;
    }
    snprintf(s->skills_off, sizeof(s->skills_off), "%s", out);
}
