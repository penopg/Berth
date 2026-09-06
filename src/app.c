// Окно, главный цикл и раскладка вкладок.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "raylib.h"
#include <ghostty/vt.h>

#include "common.h"
#include "font.h"
#include "settings.h"
#include "page.h"
#include "term.h"
#include "session.h"
#include "layout.h"
#include "input.h"
#include "render.h"
#include "theme.h"
#include "projects.h"
#include "agent.h"
#include "claude.h"
#include "ui.h"
#include "usage.h"
#include "groups.h"
#include "subprojects.h"
#include "skills.h"
#include "xp.h"
#if defined(__APPLE__)
#include "macos.h"
#include "icon_png.h"
#endif

// Отступ от краёв области терминала до сетки символов, в пикселях.
#define PAD 4

#define FONT_SIZE_DEFAULT 16
#define FONT_SIZE_MIN      9
#define FONT_SIZE_MAX     40


typedef struct {
    SessionList sessions;
    ProjectList projects;
    Layout      layout;
    FontAtlas   font;
    Theme       theme;
    Settings    settings;
    Usage       usage;        // лимиты Claude Code для верхней полосы

    // Атлас спрайтов каратеки — один на окно, грузится после создания окна.
    // Сцены живут при вкладках (Session.scene).
    Sprites     sprites;
    Groups      groups;       // группы-папки и скрытые группы

    char brief_cmd[PROJECT_PATH_MAX];   // пусто, если паспорт печатать нечем

    bool splitter_dragging;

    // Когда человек или агент последний раз что-то делали (GetTime). Лимиты
    // спрашиваем только рядом с этим: у стоящего компьютера цифры не
    // меняются, а «ручки дёргаются» каждые три минуты зря.
    double last_activity;
    int  splitter_grab_dx;   // смещение курсора от края панели в момент захвата

    Rect last_term_area;     // чтобы не пересчитывать сетку каждый кадр
} App;

// Пишет в лог, как собран libghostty-vt: включён ли SIMD и в каком режиме
// оптимизации. Дешёвый способ понять, почему всё вдруг медленно.
static void log_build_info(void)
{
    bool simd = false;
    ghostty_build_info(GHOSTTY_BUILD_INFO_SIMD, &simd);

    GhosttyOptimizeMode opt = GHOSTTY_OPTIMIZE_DEBUG;
    ghostty_build_info(GHOSTTY_BUILD_INFO_OPTIMIZE, &opt);

    const char *opt_str;
    switch (opt) {
    case GHOSTTY_OPTIMIZE_DEBUG:         opt_str = "Debug";        break;
    case GHOSTTY_OPTIMIZE_RELEASE_SAFE:  opt_str = "ReleaseSafe";  break;
    case GHOSTTY_OPTIMIZE_RELEASE_SMALL: opt_str = "ReleaseSmall"; break;
    case GHOSTTY_OPTIMIZE_RELEASE_FAST:  opt_str = "ReleaseFast";  break;
    default:                             opt_str = "Unknown";      break;
    }

    TraceLog(LOG_INFO, "ghostty-vt: simd:     %s", simd ? "enabled" : "disabled");
    TraceLog(LOG_INFO, "ghostty-vt: optimize: %s", opt_str);
}

static bool cmd_down(void)
{
    return input_mod_down(INPUT_MOD_SUPER);
}

static bool shift_down(void)
{
    return input_mod_down(INPUT_MOD_SHIFT);
}

// Метрики панели привязаны к размеру шрифта, поэтому пересчитываются вместе
// с ним, а не задаются один раз при запуске.
static LayoutMetrics metrics_for(const FontAtlas *f)
{
    return (LayoutMetrics){
        .row_height      = f->cell_height * 2 + 10,
        .row_height_idle = f->cell_height + 8,
        .task_height     = f->cell_height + 6,
        // Строка со сценой — три текстовые линии: имя, состояние, счёт боя;
        // пол сцены лежит на третьей. Спрайт NES ниже трёх линий любого
        // разумного кегля, но на всякий случай — не меньше него.
        .row_height_scene = (f->cell_height * 3 + 8 > SCENE_HEIGHT + 8)
                          ? f->cell_height * 3 + 8 : SCENE_HEIGHT + 8,
        .group_height    = f->cell_height + 26,   // отбивка сверху больше межстрочной
        .topbar_height   = f->cell_height + 14,
        .settings_width  = f->cell_width * 12 + 20,
        .pad             = PAD,
    };
}

// Сколько столбцов и строк помещается в область при текущей ячейке.
static void grid_for(Rect area, const FontAtlas *f, uint16_t *cols, uint16_t *rows)
{
    int c = (area.w - 2 * PAD) / f->cell_width;
    int r = (area.h - 2 * PAD) / f->cell_height;
    *cols = (uint16_t)(c < 1 ? 1 : c);
    *rows = (uint16_t)(r < 1 ? 1 : r);
}

// Размер сетки одинаков у всех вкладок: любая может стать активной, и
// пересчитывать её в момент переключения — значит дёргать SIGWINCH у процесса
// на каждый ⌘1, чего приложения внутри терпеть не любят.
static void resize_all(App *app)
{
    uint16_t cols, rows;
    grid_for(app->layout.term, &app->font, &cols, &rows);
    for (int i = 0; i < app->sessions.count; i++)
        term_resize(&app->sessions.items[i].term, cols, rows,
                    app->font.cell_width, app->font.cell_height);
}

static void save_layout(App *app);
static void reload_projects(App *app);

// Каталог для настроек и состояния. Создаётся при первом обращении.
static const char *config_dir(void)
{
    static char dir[PROJECT_PATH_MAX];
    if (dir[0]) return dir;

    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(dir, sizeof(dir), "%s/.config/berth", home);

    // Продукт назывался иначе; если новых настроек ещё нет, а старые лежат —
    // переносим, чтобы переименование не стоило человеку раскладки вкладок.
    struct stat st;
    if (stat(dir, &st) != 0) {
        char legacy[PROJECT_PATH_MAX];
        snprintf(legacy, sizeof(legacy), "%s/.config/term-lab", home);
        if (stat(legacy, &st) == 0 && rename(legacy, dir) == 0)
            return dir;
    }

    mkdir(dir, 0755);   // если уже есть — просто EEXIST
    return dir;
}

static const char *layout_path(void)
{
    static char path[PROJECT_PATH_MAX];
    if (path[0]) return path;

    const char *dir = config_dir();
    if (!dir) return NULL;

    // У помеченного экземпляра своя раскладка: dev-окно, запущенное рядом с
    // рабочим, не должно затирать его вкладки.
    const char *label = getenv("BERTH_LABEL");
    if (label && *label)
        snprintf(path, sizeof(path), "%s/layout-%s.tsv", dir, label);
    else
        snprintf(path, sizeof(path), "%s/layout.tsv", dir);
    return path;
}

// Настройки общие для всех экземпляров, в отличие от раскладки: dev-окно
// должно выглядеть так же, как рабочее, иначе правки не проверить.
static const char *settings_path(void)
{
    static char path[PROJECT_PATH_MAX];
    if (path[0]) return path;

    const char *dir = config_dir();
    if (!dir) return NULL;
    snprintf(path, sizeof(path), "%s/settings.conf", dir);
    return path;
}

static void save_settings(const App *app)
{
    const char *path = settings_path();
    if (path) settings_save(&app->settings, path);
}

// Команда, печатающая паспорт проекта при открытии вкладки. Пока это внешний
// скрипт из системы вкладок Warp: он уже умеет собирать историю сессий Claude,
// ветку git и статус из CLAUDE.md, и переписывать это нативно рано.
static void find_brief_script(App *app)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char path[PROJECT_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.claude/warp-tabs/brief.sh", home);
    if (access(path, X_OK) == 0)
        snprintf(app->brief_cmd, sizeof(app->brief_cmd), "%s", path);
}

// Открывает вкладку для проекта из списка (project >= 0) либо для произвольного
// каталога (project < 0).
// Меняет размер шрифта: перестраивает атлас, метрики панели и сетку всех
// вкладок. Размер один на окно — вкладки переключаются часто, и разъезжающаяся
// сетка при каждом переключении дёргала бы SIGWINCH у процессов.
static void set_font_size(App *app, int size)
{
    if (size < FONT_SIZE_MIN) size = FONT_SIZE_MIN;
    if (size > FONT_SIZE_MAX) size = FONT_SIZE_MAX;
    if (size == app->font.size) return;

    FontAtlas next;
    if (!font_load(&next, size, GetWindowScaleDPI()))
        return;   // не смогли — молча остаёмся на прежнем

    font_unload(&app->font);
    app->font = next;
    app->layout.metrics = metrics_for(&app->font);
    resize_all(app);
}

// Настройки в состояние окна. Вызывается и при старте, и когда файл правят
// снаружи: настройка, которую нельзя применить на лету, — это настройка,
// ради которой пришлось бы перезапускать терминал с живыми сессиями.
// Тема окна справа — терминала и страниц вкладки — по правилу из
// настроек: своя у проекта, если настройка «как у проекта» и тема в
// projects.json есть; иначе названная тема; иначе тема панели. Незнакомое
// имя у проекта — тема панели, а не тёмная по умолчанию: окно должно быть
// согласованным.
static const Theme *window_theme_for(const App *app, int project)
{
    const Theme *panel = theme_by_name(app->settings.theme_panel);
    const char *want = app->settings.theme_window;
    if (strcmp(want, "project") != 0) {
        const Theme *t = theme_find(want);
        return t ? t : panel;
    }
    if (project >= 0 && project < app->projects.count) {
        const Theme *t = theme_find(app->projects.items[project].theme);
        if (t) return t;
    }
    return panel;
}

static void apply_settings(App *app)
{
    app->theme = *theme_by_name(app->settings.theme_panel);
    for (int i = 0; i < app->sessions.count; i++) {
        Session *s = &app->sessions.items[i];
        session_set_theme(s, window_theme_for(app, s->project));
    }
    set_font_size(app, app->settings.font_size);
    layout_set_sidebar_width(&app->layout, app->settings.sidebar_width);
    if (app->layout.sidebar_visible != app->settings.sidebar_visible)
        layout_toggle_sidebar(&app->layout);
    app->layout.collapsed_show_live = app->settings.collapsed_show_live;
    app->layout.ctx_warn = app->settings.ctx_warn;
    app->layout.ctx_crit = app->settings.ctx_crit;
    app->layout.sleep_after = app->settings.sleep_after * 60;
    app->layout.scene = settings_scene_enabled(&app->settings) && app->sprites.ready;
}

// Настроение сцены по состоянию агента. Работа кончилась — победа: агент
// снова свободен после того, как был занят. Свободный с самого начала —
// просто стойка.
static SceneMood scene_mood_for(const Session *s, SceneMood prev)
{
    switch (s->state) {
    case SESSION_STATE_BUSY:      return s->compacting ? SCENE_COMPACT : SCENE_FIGHT;
    case SESSION_STATE_ATTENTION: return SCENE_CALL;
    case SESSION_STATE_DEAD:      return SCENE_FAIL;
    default:
        if (prev == SCENE_FIGHT || prev == SCENE_WIN) return SCENE_WIN;
        return SCENE_IDLE;   // после сжатия без работы — просто разошлись
    }
}

// Сцены всех живых разговоров идут каждый кадр: бой у неактивной вкладки
// виден в её строке и потому должен продолжаться. Цена — несколько
// сравнений на вкладку.
static void scene_tick(App *app, float dt)
{
    if (!app->layout.scene) return;
    // Записи jsonl — раз в полсекунды, а не раз в две: stat() на вкладку
    // дёшев, а счёт после готового сообщения должен встать на место быстро.
    static double last_ctx = 0;
    double now = GetTime();
    bool ctx_due = now - last_ctx >= 0.5;
    if (ctx_due) last_ctx = now;
    for (int i = 0; i < app->sessions.count; i++) {
        Session *s = &app->sessions.items[i];
        if (!session_has_term(s)) { s->scene_ready = false; continue; }
        if (!s->scene_ready) {
            scene_init(&s->scene);
            s->scene_ready = true;
            s->scene_bytes_seen = s->term.bytes_in;
        }
        if (ctx_due) {
            session_track_ctx(s);
            // Сжатие контекста снаружи неотличимо от работы — реестр
            // пишет «busy». Зато крутилка Claude Code в это время пишет
            // «Compacting conversation», а перед и после — хуки сжатия.
            // Смотрим только на строку крутилки: та же фраза в тексте
            // ответа (в разговоре про берт она встречается) не считается.
            // Между сообщениями крутилки строка на миг пропадает, поэтому
            // признак держится пару секунд после последнего появления —
            // иначе на этом миге начинался бой и тут же «побеждал».
            static const char *const compact_msgs[] = {
                "Compacting conversation", "Running PreCompact hooks",
                "Running PostCompact hooks", "Running SessionStart hooks",
            };
            bool seen = false;
            if (s->state == SESSION_STATE_BUSY)
                for (size_t k = 0; k < sizeof(compact_msgs) / sizeof(*compact_msgs) && !seen; k++)
                    seen = term_screen_status(&s->term, compact_msgs[k]);
            if (seen) s->compact_seen = now;
            s->compacting = s->state == SESSION_STATE_BUSY && s->compact_seen > 0 && now - s->compact_seen < 2.0;
        }
        unsigned long seen = s->term.bytes_in;
        scene_activity(&s->scene, seen - s->scene_bytes_seen, dt);
        s->scene_bytes_seen = seen;
        scene_score(&s->scene, s->tokens_out);
        SceneMood before = s->scene.mood;
        scene_set(&s->scene, scene_mood_for(s, s->scene.mood));
        if (before == SCENE_FIGHT && s->scene.mood == SCENE_WIN) xp_fight(s->cwd);
        scene_update(&s->scene, dt);
    }
}

static int open_task(App *app, int project, const char *cwd,
                     const char *name, const char *command);

static int open_tab_cmd(App *app, int project, const char *cwd,
                        const AgentProfile *agent, SessionKind kind,
                        SessionPageKind page, const char *command)
{
    uint16_t cols, rows;
    grid_for(app->layout.term, &app->font, &cols, &rows);

    const Project *p = (project >= 0 && project < app->projects.count)
        ? &app->projects.items[project] : NULL;

    // Путь в кавычках: в именах проектов встречаются пробелы.
    char brief[PROJECT_PATH_MAX * 2] = "";
    const char *dir = p ? p->path : cwd;
    if (app->brief_cmd[0] && dir && *dir)
        snprintf(brief, sizeof(brief), "%s \"%s\"", app->brief_cmd, dir);

    SessionOpts opts = {
        .theme = window_theme_for(app, project),
        .name = p ? p->name : NULL,
        .cwd = dir,
        .group = p ? p->group : NULL,
        .color = p ? p->color : (Color){0},
        .agent = agent ? agent : agent_default(),
        .project = project,
        .brief = brief[0] ? brief : NULL,
        .kind = kind,
        .page = page,
        .command = command,
        .cols = cols, .rows = rows,
        .cell_width = app->font.cell_width,
        .cell_height = app->font.cell_height,
    };
    return session_open(&app->sessions, &opts);
}

// « --model <x>» для служебной задачи или пустая строка: модель задач —
// настройка, и подставляется она в одном месте, а не в каждой команде.
static const char *task_model_flag(App *app)
{
    static char buf[96];
    const char *m = app->settings.task_model;
    if (!m || !*m) return "";
    snprintf(buf, sizeof(buf), " --model %s", m);
    return buf;
}

// Задача, которая закончилась хорошо, закрывает свою вкладку сама: итог
// её работы лежит в файле, и страница его уже показывает, а вкладка с
// приглашением оболочки — мусор, который приходилось убирать руками.
// Упавшая остаётся: код выхода и хвост вывода — единственное, по чему
// понять, что случилось. Если человек смотрел на задачу, уводим его в
// разговор того же проекта, а не в случайную соседнюю вкладку.
static void reap_tasks(App *app)
{
    double now = GetTime();
    for (int i = 0; i < app->sessions.count; i++) {
        Session *s = &app->sessions.items[i];
        if (s->role != SESSION_ROLE_TASK || !session_has_term(s)) continue;
        if (s->state != SESSION_STATE_DEAD || !s->term.child_reaped) continue;
        if (s->term.child_status != 0) continue;
        if (s->close_due == 0) { s->close_due = now + 2.0; continue; }
        if (now < s->close_due) continue;

        if (app->sessions.active == i) {
            int home = session_of_project(&app->sessions, s->cwd);
            if (home >= 0) session_activate(&app->sessions, home);
        }
        session_close(&app->sessions, i);
        resize_all(app);
        save_layout(app);
        i--;
    }
}

// Фоновая работа рядом с разговором: собрать журнал, например. Отдельная
// вкладка, а не отвязанный процесс — за задачей должно быть видно, что она
// делает, и её должно быть можно остановить.
static int open_task(App *app, int project, const char *cwd,
                     const char *name, const char *command)
{
    uint16_t cols, rows;
    grid_for(app->layout.term, &app->font, &cols, &rows);

    SessionOpts opts = {
        .theme = window_theme_for(app, project),
        .name = name,
        .task_name = name,
        .cwd = cwd,
        .agent = agent_by_id("claude"),
        .project = project,
        .kind = SESSION_KIND_TERM,
        .page = SESSION_PAGE_PROJECT,
        .role = SESSION_ROLE_TASK,
        .command = command,
        .cols = cols, .rows = rows,
        .cell_width = app->font.cell_width,
        .cell_height = app->font.cell_height,
    };
    return session_open(&app->sessions, &opts);
}

static int open_tab(App *app, int project, const char *cwd,
                    const AgentProfile *agent, SessionKind kind,
                    SessionPageKind page)
{
    return open_tab_cmd(app, project, cwd, agent, kind, page, NULL);
}

static int open_session(App *app, int project, const char *cwd,
                        const AgentProfile *agent)
{
    return open_tab(app, project, cwd, agent, SESSION_KIND_TERM,
                    SESSION_PAGE_PROJECT);
}

// Показать или спрятать страницу проекта поверх вкладки. Процесс при этом
// живёт и читается дальше: это оборотная сторона вкладки, а не новое окно.
static void toggle_project_page(App *app, Session *s)
{
    if (!s || s->kind == SESSION_KIND_PAGE) return;   // это и так страница
    s->show_page = !s->show_page;
    if (s->show_page) {
        session_refresh_info(s);
        s->page_scroll_end = true;
    }   // сведения могли устареть, пока шла работа
}

// Страница настроек — такая же вкладка, только без процесса. Второй раз не
// открывается: настройки одни на окно, а вкладок и так хватает.
static void open_settings(App *app)
{
    for (int i = 0; i < app->sessions.count; i++) {
        Session *s = &app->sessions.items[i];
        if (!session_has_term(s) && s->page == SESSION_PAGE_SETTINGS) {
            session_activate(&app->sessions, i);
            return;
        }
    }
    int idx = open_tab(app, -1, NULL, agent_by_id("shell"), SESSION_KIND_PAGE,
                       SESSION_PAGE_SETTINGS);
    if (idx >= 0) {
        Session *s = &app->sessions.items[idx];
        snprintf(s->name, sizeof(s->name), "Настройки");
        s->group[0] = '\0';       // вкладка не принадлежит проекту
    }
    save_layout(app);
}

// Что делать с тем, по чему кликнули на странице. Событие описывает намерение
// («запусти агента», «продолжи вот эту сессию»), а как это исполнить — знает
// приложение: у него и сетка терминала, и настройки, и файл раскладки.
// Путь родителя, если cwd — подпроект, иначе NULL.
static const char *parent_path_of(const App *app, const char *cwd)
{
    int i = projects_find_by_path(&app->projects, cwd);
    if (i < 0 || app->projects.items[i].parent < 0) return NULL;
    return app->projects.items[app->projects.items[i].parent].path;
}

static void resume_command(const char *cwd, const char *sid, char *out, size_t cap);
static void reload_projects(App *app);
static void save_layout(App *app);

// Отдать путь системе: папку — в Finder, файл — в редактор по умолчанию
// (`open -t`). Двойной fork, чтобы не оставлять зомби: внука никто не ждёт.
static void open_external(const char *target, bool edit) {
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            if (edit) execl("/usr/bin/open", "open", "-t", target, (char *)NULL);
            else      execl("/usr/bin/open", "open", target, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

// Папка, в которой живут проекты группы. У группы-папки это она сама; у
// группы из projects.json — родитель её проектов (все они лежат рядом:
// так их генератор вкладок и находит).
static bool group_root(const App *app, const char *group, char *out, size_t cap)
{
    for (int i = 0; i < app->groups.added_count; i++) {
        const char *base = strrchr(app->groups.added[i], '/');
        base = base ? base + 1 : app->groups.added[i];
        if (!strcmp(base, group)) {
            snprintf(out, cap, "%s", app->groups.added[i]);
            return true;
        }
    }
    for (int i = 0; i < app->projects.count; i++) {
        const Project *p = &app->projects.items[i];
        if (p->parent >= 0 || strcmp(p->group, group)) continue;
        snprintf(out, cap, "%s", p->path);
        char *slash = strrchr(out, '/');
        if (!slash || slash == out) continue;
        *slash = '\0';
        return true;
    }
    return false;
}

static bool is_folder_group(const App *app, const char *root)
{
    for (int i = 0; i < app->groups.added_count; i++)
        if (!strcmp(app->groups.added[i], root)) return true;
    return false;
}

// Новый проект в группе: папка с паспортом по шаблону, как у подпроекта.
// Группе из projects.json нужен ещё генератор вкладок — он перепишет
// projects.json, и берт подхватит файл по mtime; до тех пор вкладка
// проекта живёт без записи в списке, по одному каталогу.
static void handle_new_project(App *app, PageEvent ev)
{
    if (!ev.cwd || !ev.title) return;
    char err[200];
    if (!subprojects_create(ev.cwd, ev.title, ev.body, err, sizeof(err))) {
        page_new_project_failed(err);
        return;
    }
    char dir[PROJECT_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s", ev.cwd, ev.title);

    bool folder = is_folder_group(app, ev.cwd);
    if (!folder) {
        const char *home = getenv("HOME");
        char gen[PROJECT_PATH_MAX];
        snprintf(gen, sizeof(gen), "%s/.claude/warp-tabs/gen.sh", home ? home : "");
        pid_t pid = fork();
        if (pid == 0) {
            if (fork() == 0) {
                execl("/bin/bash", "bash", gen, (char *)NULL);
                _exit(127);
            }
            _exit(0);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
    }

    reload_projects(app);
    int idx = projects_find_by_path(&app->projects, dir);
    int tab = open_tab(app, idx, dir, agent_by_id("claude"),
                       SESSION_KIND_PAGE, SESSION_PAGE_PROJECT);
    if (tab >= 0) {
        session_activate(&app->sessions, tab);
        Session *s = &app->sessions.items[tab];
        snprintf(s->page_notice, sizeof(s->page_notice),
                 folder ? "Проект «%s» заведён, паспорт в CLAUDE.md"
                        : "Проект «%s» заведён, паспорт в CLAUDE.md; список проектов обновится через несколько секунд",
                 ev.title);
    }
    save_layout(app);
}

// Реплика в разговор проекта. Разговор у каталога один, и он не обязательно
// в этой вкладке: страницу открывают и поверх задачи-вкладки, а задача
// подпроекта идёт в разговор подпроекта, не родителя. Живой разговор
// получает текст вставкой; закрытого нет — запускается с этой репликой первой.
static void say_to_conversation(App *app, Session *s, const char *cwd,
                                const char *prompt, uint16_t cols, uint16_t rows)
{
    bool foreign = strcmp(cwd, s->cwd) != 0;
    int main = session_of_project(&app->sessions, cwd);
    Session *target = main >= 0 ? &app->sessions.items[main]
                    : foreign ? NULL : s;

    if (target && session_has_term(target) && term_alive(&target->term)) {
        // Вставкой, а не набором: bracketed paste держит многострочный
        // текст одной репликой. Enter — отдельно, как нажал бы человек.
        term_paste(&target->term, prompt, strlen(prompt));
        target->enter_due = GetTime() + 0.3;
        target->show_page = false;
        if (main >= 0) session_activate(&app->sessions, main);
    } else if (target && session_has_term(target)) {
        snprintf(s->page_notice, sizeof(s->page_notice),
                 "Разговор завершён — закройте его (⌘W) и отправьте задачу снова");
    } else {
        // Разговора нет — запускаем и отдаём задачу первой репликой.
        // Текст идёт через файл: в командной строке ему делать нечего —
        // кавычки, переводы строк, апострофы.
        char path[PROJECT_PATH_MAX];
        snprintf(path, sizeof(path), "%s/prompt.txt", config_dir());
        FILE *f = fopen(path, "w");
        if (!f) {
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Не удалось записать %s", path);
            return;
        }
        fputs(prompt, f);
        fclose(f);

        const AgentProfile *agent = agent_by_id("claude");
        char resume[256] = "";
        if (agent->has_history && agent->has_history(cwd) && agent->resume_command)
            agent->resume_command(cwd, resume, sizeof(resume));
        char cmd[PROJECT_PATH_MAX + 300];
        snprintf(cmd, sizeof(cmd), "%s \"$(cat '%s')\"",
                 resume[0] ? resume : agent->launch, path);
        if (target) {
            session_start_term(target, agent, cols, rows,
                               app->font.cell_width, app->font.cell_height,
                               NULL, cmd);
            if (main >= 0) session_activate(&app->sessions, main);
        } else {
            // Разговор подпроекта — своя вкладка: у него своя история.
            int idx = open_tab_cmd(app, projects_find_by_path(&app->projects, cwd),
                                   cwd, agent, SESSION_KIND_TERM,
                                   SESSION_PAGE_PROJECT, cmd);
            if (idx >= 0) session_activate(&app->sessions, idx);
            resize_all(app);
        }
    }
    save_layout(app);
}

static void handle_page_event(App *app, Session *s, PageEvent ev)
{
    if (getenv("BERTH_DEBUG_PAGE") && ev.kind)
        fprintf(stderr, "[page] событие %d arg=%d arg2=%d text=%s t=%.3f\n",
                ev.kind, ev.arg, ev.arg2, ev.text, GetTime());

    uint16_t cols, rows;
    grid_for(app->layout.term, &app->font, &cols, &rows);

    switch (ev.kind) {
    case PAGE_EVENT_NONE:
        return;

    case PAGE_EVENT_NEW_PROJECT:
        // Приходит не со страницы, а из карточки над окном — handle_new_project.
        return;

    case PAGE_EVENT_START_AGENT: {
        const AgentProfile *agent = agent_by_id(ev.text);
        if (!agent) agent = agent_default();
        // Паспорт печатать незачем: всё то же самое человек только что видел
        // на странице, ради того она и открывалась. Если вкладка помнит свой
        // диалог (поднята страницей при запуске), продолжается именно он.
        char resume[PROJINFO_ID_MAX + 32] = "";
        if (agent->has_history) resume_command(s->cwd, s->session_id, resume, sizeof(resume));
        session_start_term(s, agent, cols, rows,
                           app->font.cell_width, app->font.cell_height,
                           NULL, resume[0] ? resume : NULL);
        save_layout(app);
        break;
    }

    case PAGE_EVENT_RESUME_SESSION: {
        const AgentProfile *agent = agent_by_id("claude");
        char cmd[160];
        switch ((ProjSessionUse)ev.arg) {
        case PROJ_SESSION_AGENT:
            // В сессии работает фоновый агент: продолжить её нельзя, можно
            // только подключиться — этим и занимается `claude agents`.
            snprintf(cmd, sizeof(cmd), "claude agents");
            break;
        case PROJ_SESSION_OPEN:
            // Диалог уже открыт в живой вкладке. Форк даёт свою копию со всем
            // контекстом и не трогает оригинал — иначе два процесса пишут в один
            // jsonl и история рвётся у обоих.
            snprintf(cmd, sizeof(cmd), "claude --resume %s --fork-session", ev.text);
            break;
        default:
            snprintf(cmd, sizeof(cmd), "claude --resume %s", ev.text);
            break;
        }

        if (session_has_term(s)) {
            // Разговор у проекта один, и молча заменить его нельзя: там идёт
            // работа. Говорим прямо, что мешает, — решение за человеком.
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Здесь уже идёт разговор — закройте его (⌘W), чтобы поднять этот");
        } else {
            session_start_term(s, agent, cols, rows,
                               app->font.cell_width, app->font.cell_height,
                               NULL, cmd);
        }
        save_layout(app);
        break;
    }

    case PAGE_EVENT_HIDE_PAGE:
        s->show_page = false;
        break;

    case PAGE_EVENT_REFRESH:
        session_refresh_info(s);
        s->page_scroll_end = true;
        break;

    case PAGE_EVENT_BUILD_JOURNAL: {
        // Задачей, а не в фоне вслепую: сбор идёт минуту-другую, и человеку
        // нужно видеть, что там происходит, а при желании — остановить.
        // Страницу не отдаём: человек нажал кнопку на ней, а не просил
        // увести себя в терминал. Задача видна в списке, открыть — по клику.
        int was = app->sessions.active;
        // Права даём точечно: скиллу нужно запустить extract.py и дописать
        // journal.md. Без этого агент упирается в подтверждение, а спросить
        // его некому — задача идёт сама по себе.
        // Именно `-p`: задача должна кончиться сама. Интерактивный запуск
        // отрабатывал промпт и оставался в приглашении — работа сделана, а
        // вкладка вечно «работает», и понять это можно было только войдя.
        // `--verbose` оставляет на экране ход работы, а не один итог.
        // `--no-session-persistence`: служебный вызов не должен оставлять
        // в истории проекта свой jsonl — иначе журнал пишет сам о себе, а
        // его записи приписываются сессии сбора, а не разговора.
        char cmd[640];
        snprintf(cmd, sizeof(cmd),
                 "claude -p \"/project-journal\" --verbose --no-session-persistence%s "
                 "--permission-mode acceptEdits "
                 "--allowed-tools \"Bash(python3 *)\" Read Edit Write",
                 task_model_flag(app));
        int tab = open_task(app, s->project, s->cwd, "журнал", cmd);
        if (tab >= 0) {
            session_activate(&app->sessions, was);
            resize_all(app);
            save_layout(app);
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Журнал собирается — задача видна в панели и на этой странице");
        } else {
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Не удалось открыть задачу: вкладок уже %d", app->sessions.count);
        }
        break;
    }

    case PAGE_EVENT_SHOW_TASK:
        if (ev.arg >= 0 && ev.arg < app->sessions.count)
            session_activate(&app->sessions, ev.arg);
        break;

    case PAGE_EVENT_STOP_TASK:
        // Закрываем так же, как закрыл бы человек: гасим процесс и убираем
        // вкладку. Задача на то и задача, что её не жалко прервать.
        if (ev.arg >= 0 && ev.arg < app->sessions.count) {
            session_close(&app->sessions, ev.arg);
            resize_all(app);
            save_layout(app);
        }
        break;

    case PAGE_EVENT_TODO_TOGGLE:
        // Раскрыта одна задача на страницу, в каком бы списке она ни была.
        if (s->page_task_sub != ev.sub) s->page_task_open = 0;
        s->page_task_sub = ev.sub;
        s->page_task_open = (s->page_task_open == ev.arg + 1) ? 0 : ev.arg + 1;
        // Прокрутку не трогаем: страница сама попросит показать раскрытое
        // описание, если оно вылезло за край (PageEvent.reveal).
        break;

    case PAGE_EVENT_TODO_MOVE: {
        const char *cwd = ev.cwd ? ev.cwd : s->cwd;
        ProjectState *st = projstate_edit(cwd);
        if (!st) break;
        tasks_move(&st->tasks, ev.arg, ev.arg2);

        // Раскрытая задача едет вместе с перестановкой: номера меняются,
        // а раскрыта была задача, а не номер.
        int open = s->page_task_open - 1;
        if (open == ev.arg) open = ev.arg2;
        else if (ev.arg < open && open <= ev.arg2) open--;
        else if (ev.arg2 <= open && open < ev.arg) open++;
        s->page_task_open = open + 1;

        if (!tasks_save(&st->tasks, cwd))
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Не удалось записать %s", TASKS_FILE);
        break;
    }

    case PAGE_EVENT_TODO_STATE: {
        const char *cwd = ev.cwd ? ev.cwd : s->cwd;
        ProjectState *st = projstate_edit(cwd);
        if (!st || ev.arg < 0 || ev.arg >= st->tasks.count) break;
        st->tasks.items[ev.arg].state = (TaskState)ev.arg2;
        s->page_task_open = 0;
        if (!tasks_save(&st->tasks, cwd))
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Не удалось записать %s", TASKS_FILE);
        break;
    }

    case PAGE_EVENT_TODO_SAVE: {
        const char *cwd = ev.cwd ? ev.cwd : s->cwd;
        ProjectState *st = projstate_edit(cwd);
        if (!st || !ev.title) break;
        TaskList *tl = &st->tasks;
        int idx = ev.arg;
        if (idx < 0) {
            if (tl->count >= TASK_MAX) {
                snprintf(s->page_notice, sizeof(s->page_notice),
                         "Задач уже %d — больше в список не помещается", TASK_MAX);
                break;
            }
            // Новая встаёт последней среди несделанных: сделанные лежат
            // в конце файла, и новая работа не должна оказаться за ними.
            int at = 0;
            for (int i = 0; i < tl->count; i++)
                if (tl->items[i].state != TASK_DONE) at = i + 1;
            memmove(&tl->items[at + 1], &tl->items[at],
                    sizeof(Task) * (size_t)(tl->count - at));
            memset(&tl->items[at], 0, sizeof(Task));
            tl->count++;
            idx = at;
        }
        if (idx >= tl->count) break;
        snprintf(tl->items[idx].title, sizeof(tl->items[idx].title), "%s", ev.title);
        snprintf(tl->items[idx].body, sizeof(tl->items[idx].body), "%s", ev.body ? ev.body : "");
        s->page_task_open = idx + 1;
        if (tasks_save(tl, cwd))
            tl->exists = true;
        else
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Не удалось записать %s", TASKS_FILE);
        break;
    }

    case PAGE_EVENT_TODO_SEND: {
        const char *cwd = ev.cwd ? ev.cwd : s->cwd;
        const ProjectState *st = projstate_get(cwd);
        if (!st || ev.arg < 0 || ev.arg >= st->tasks.count) break;
        char prompt[TASK_TITLE_MAX * 3 + TASK_BODY_MAX + 400];
        tasks_prompt(&st->tasks.items[ev.arg], app->settings.task_finish,
                     prompt, sizeof(prompt));
        say_to_conversation(app, s, cwd, prompt, cols, rows);
        break;
    }

    case PAGE_EVENT_JOURNAL_TOGGLE:
        s->page_journal_open = (s->page_journal_open == ev.arg + 1) ? 0 : ev.arg + 1;
        break;

    case PAGE_EVENT_JOURNAL_DAY: {
        // Перевернуть умолчание дня: уже перевёрнут — убрать из списка,
        // иначе дописать; полный список теряет самое давнее.
        int n = s->page_day_toggles;
        int at = -1;
        for (int i = 0; i < n; i++) if (s->page_day_keys[i] == ev.arg) at = i;
        if (at >= 0) {
            memmove(&s->page_day_keys[at], &s->page_day_keys[at + 1],
                    sizeof(int) * (size_t)(n - at - 1));
            s->page_day_toggles--;
        } else {
            if (n == PAGE_DAY_TOGGLES) {
                memmove(&s->page_day_keys[0], &s->page_day_keys[1], sizeof(int) * (size_t)(n - 1));
                n--;
            }
            s->page_day_keys[n] = ev.arg;
            s->page_day_toggles = n + 1;
        }
        break;
    }

    case PAGE_EVENT_TODO_DONE_TOGGLE:
        if (ev.sub + 1 >= 0 && ev.sub + 1 < 32) s->page_done_open ^= 1u << (ev.sub + 1);
        break;

    case PAGE_EVENT_JOURNAL_MENTION: {
        // Вернуться к обсуждению — значит показать агенту, где оно лежит:
        // сессия, файл и метка времени в UTC, как в самом jsonl. Место он
        // найдёт сам, а нам не нужно ни поднимать старую сессию, ни
        // пересказывать её.
        const ProjectState *st = projstate_get(s->cwd);
        if (!st || ev.arg < 0 || ev.arg >= st->journal.count) break;
        const JournalEvent *je = &st->journal.events[ev.arg];
        char prompt[JOURNAL_TEXT_MAX + JOURNAL_DETAIL_MAX + PROJECT_PATH_MAX + 700];
        journal_mention(je, s->cwd, prompt, sizeof(prompt));
        say_to_conversation(app, s, s->cwd, prompt, cols, rows);
        break;
    }

    case PAGE_EVENT_TOGGLE_SUB:
        if (ev.arg >= 0 && ev.arg < 32) s->page_subs_open ^= 1u << ev.arg;
        break;

    case PAGE_EVENT_ADD_SUBPROJECT: {
        // Выбор начинается в папке проекта: подпроект лежит прямо в ней.
        char dir[PROJECT_PATH_MAX];
        if (!macos_choose_folder(s->cwd, "Добавить подпроект", dir, sizeof(dir)))
            break;
        if (subprojects_add(s->cwd, dir)) {
            reload_projects(app);
            layout_set_parent_expanded(&app->layout, s->cwd, true);
            save_layout(app);
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Подпроект добавлен — записано в %s", SUBPROJECTS_FILE);
        } else {
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Подпроект должен быть папкой прямо внутри проекта");
        }
        break;
    }

    case PAGE_EVENT_NEW_SUBPROJECT: {
        if (!ev.title) break;
        char err[200];
        if (!subprojects_create(s->cwd, ev.title, ev.body, err, sizeof(err))) {
            snprintf(s->page_notice, sizeof(s->page_notice), "%s", err);
            break;
        }
        char dir[PROJECT_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", s->cwd, ev.title);
        reload_projects(app);
        layout_set_parent_expanded(&app->layout, s->cwd, true);
        snprintf(s->page_notice, sizeof(s->page_notice),
                 "Подпроект «%s» заведён, паспорт в CLAUDE.md", ev.title);
        // Сразу на его страницу: работать пришли над ним, а не над родителем.
        open_tab(app, projects_find_by_path(&app->projects, dir), dir,
                 agent_by_id(app->settings.default_agent),
                 SESSION_KIND_PAGE, SESSION_PAGE_PROJECT);
        resize_all(app);
        save_layout(app);
        break;
    }

    case PAGE_EVENT_OPEN_FOLDER:
        open_external(s->cwd, false);
        snprintf(s->page_notice, sizeof(s->page_notice), "Открыта в Finder: %s", s->cwd);
        break;

    case PAGE_EVENT_SKILL_TOGGLE:
        s->page_skill_open = (s->page_skill_open == ev.arg + 1) ? 0 : ev.arg + 1;
        break;

    case PAGE_EVENT_SKILL_OPEN:
    case PAGE_EVENT_SKILL_EDIT: {
        const SkillList *sl = projstate_skills(s->cwd, parent_path_of(app, s->cwd), false);
        for (int i = 0; sl && i < sl->count; i++) {
            if (strcmp(sl->items[i].name, ev.text)) continue;
            // Папку — в Finder, файл — в редактор по умолчанию (`open -t`).
            // Двойной fork, чтобы не оставлять зомби: внука никто не ждёт.
            char target[PROJECT_PATH_MAX + 16];
            bool edit = ev.kind == PAGE_EVENT_SKILL_EDIT;
            if (edit) snprintf(target, sizeof(target), "%s/SKILL.md", sl->items[i].path);
            else      snprintf(target, sizeof(target), "%s", sl->items[i].path);
            open_external(target, edit);
            snprintf(s->page_notice, sizeof(s->page_notice), "%s", target);
            break;
        }
        break;
    }

    case PAGE_EVENT_NEW_SKILL: {
        if (!ev.title) break;
        char err[200];
        if (!skills_create(s->cwd, ev.title, ev.body, err, sizeof(err))) {
            snprintf(s->page_notice, sizeof(s->page_notice), "%s", err);
            break;
        }
        const SkillList *sl = projstate_skills(s->cwd, parent_path_of(app, s->cwd), true);
        for (int i = 0; sl && i < sl->count; i++)
            if (!strcmp(sl->items[i].name, ev.title)) s->page_skill_open = i + 1;
        snprintf(s->page_notice, sizeof(s->page_notice),
                 "Скилл «%s» заведён в .claude/skills — допишите, что делать", ev.title);
        break;
    }

    case PAGE_EVENT_HIDE_SUBPROJECT:
        if (subprojects_hide(s->cwd, ev.text)) {
            reload_projects(app);
            s->page_subs_open = 0;
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "«%s» убран из подпроектов; вернуть — правкой %s или повторным добавлением",
                     ev.text, SUBPROJECTS_FILE);
        }
        break;

    case PAGE_EVENT_BUILD_SUMMARY: {
        // Так же, как журнал: задачей-вкладкой, `-p`, права точечно. Сводке
        // нужны паспорт, код и история: чтение, поиск, git log и конспект
        // истории через python3 — и запись одного файла.
        int was = app->sessions.active;
        char cmd[640];
        snprintf(cmd, sizeof(cmd),
                 "claude -p \"/project-summary\" --verbose --no-session-persistence%s "
                 "--permission-mode acceptEdits "
                 "--allowed-tools \"Bash(python3 *)\" \"Bash(git log*)\" "
                 "\"Bash(git status*)\" \"Bash(ls *)\" \"Bash(wc *)\" "
                 "Read Glob Grep Edit Write",
                 task_model_flag(app));
        int tab = open_task(app, s->project, s->cwd, "сводка", cmd);
        if (tab >= 0) {
            session_activate(&app->sessions, was);
            resize_all(app);
            save_layout(app);
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Сводка пишется — задача видна в панели и на этой странице");
        } else {
            snprintf(s->page_notice, sizeof(s->page_notice),
                     "Не удалось открыть задачу: вкладок уже %d", app->sessions.count);
        }
        break;
    }

    case PAGE_EVENT_FONT_STEP:
        set_font_size(app, ev.arg == 0 ? FONT_SIZE_DEFAULT
                                       : app->font.size + ev.arg);
        app->settings.font_size = app->font.size;
        save_settings(app);
        break;

    case PAGE_EVENT_TOGGLE_SIDEBAR:
        layout_toggle_sidebar(&app->layout);
        app->settings.sidebar_visible = app->layout.sidebar_visible;
        save_settings(app);
        break;

    case PAGE_EVENT_TOGGLE_OPEN_MODE:
        app->settings.open_project_page = !app->settings.open_project_page;
        save_settings(app);
        break;

    case PAGE_EVENT_SET_DEFAULT_AGENT:
        snprintf(app->settings.default_agent, sizeof(app->settings.default_agent),
                 "%s", ev.text);
        save_settings(app);
        break;

    case PAGE_EVENT_SET_MARKER:
        snprintf(app->settings.marker, sizeof(app->settings.marker), "%s", ev.text);
        app->layout.scene = settings_scene_enabled(&app->settings) && app->sprites.ready;
        save_settings(app);
        break;

    case PAGE_EVENT_TOGGLE_COLLAPSED_LIVE:
        app->settings.collapsed_show_live = !app->settings.collapsed_show_live;
        app->layout.collapsed_show_live = app->settings.collapsed_show_live;
        save_settings(app);
        break;

    case PAGE_EVENT_TOGGLE_BERTH_SKILL:
        settings_skill_set(&app->settings, ev.text,
                           !settings_skill_enabled(&app->settings, ev.text));
        save_settings(app);
        claude_install_bundle(app->settings.skills_off);
        break;

    case PAGE_EVENT_TOGGLE_USAGE_FETCH:
        app->settings.usage_fetch = !app->settings.usage_fetch;
        save_settings(app);
        // Включили — пусть спросит сразу, а не через три минуты.
        if (app->settings.usage_fetch) app->usage.last_spawn = 0;
        break;

    case PAGE_EVENT_UNHIDE_GROUP:
        groups_unhide(&app->groups, ev.text);
        groups_save(&app->groups);
        reload_projects(app);
        break;

    case PAGE_EVENT_SET_TASK_FINISH:
        app->settings.task_finish = (TaskFinish)ev.arg;
        save_settings(app);
        break;

    case PAGE_EVENT_SET_THEME_PANEL:
        snprintf(app->settings.theme_panel, sizeof(app->settings.theme_panel), "%s", ev.text);
        apply_settings(app);
        save_settings(app);
        break;

    case PAGE_EVENT_SET_THEME_WINDOW:
        snprintf(app->settings.theme_window, sizeof(app->settings.theme_window), "%s", ev.text);
        apply_settings(app);
        save_settings(app);
        break;
    }
}

// Открыть проект или переключиться на него, если он уже открыт.
static void activate_project(App *app, int project)
{
    for (int i = 0; i < app->sessions.count; i++) {
        if (app->sessions.items[i].project != project) continue;

        // Первый клик переключает на вкладку проекта, повторный по той же
        // вкладке показывает его страницу: тыкать в уже открытый проект
        // незачем, если только не хочешь посмотреть на него самого.
        if (app->sessions.active == i)
            toggle_project_page(app, &app->sessions.items[i]);
        else
            session_activate(&app->sessions, i);
        return;
    }
    // ⌥-клик — всегда оболочка, мимо всех настроек: это аварийный выход к
    // приглашению, и он не должен зависеть от того, как настроен клик.
    bool shell = input_mod_down(INPUT_MOD_ALT);
    const AgentProfile *agent = shell
        ? agent_by_id("shell") : agent_by_id(app->settings.default_agent);
    if (!agent) agent = agent_by_id("claude");   // в файле мог быть мусор

    if (!shell && app->settings.open_project_page)
        open_tab(app, project, NULL, agent, SESSION_KIND_PAGE,
                 SESSION_PAGE_PROJECT);
    else
        open_session(app, project, NULL, agent);
    resize_all(app);
    save_layout(app);
}

// Перечитать список проектов. Открытые вкладки при этом не трогаем, но
// заново связываем их с проектами: после правки файла индексы уезжают, и
// вкладка, привязанная к старому индексу, показалась бы чужим проектом.
static const char *groups_path(void)
{
    static char path[PROJECT_PATH_MAX];
    if (path[0]) return path;
    const char *dir = config_dir();
    if (!dir) return NULL;
    snprintf(path, sizeof(path), "%s/groups.tsv", dir);
    return path;
}

static void reload_projects(App *app)
{
    projects_load(&app->projects, projects_default_path());
    groups_apply(&app->groups, &app->projects);
    subprojects_apply(&app->projects);

    for (int i = 0; i < app->sessions.count; i++) {
        Session *s = &app->sessions.items[i];
        s->project = projects_find_by_path(&app->projects, s->cwd);
    }
}

// Соседняя вкладка в том порядке, в каком они стоят в панели: раз панель
// группирует проекты, «следующая» — это следующая строка на экране, а не
// следующий элемент массива.
static void step_session(App *app, int delta)
{
    int total = layout_session_count(&app->layout);
    if (total <= 0) return;

    int pos = layout_position_of(&app->layout, app->sessions.active);
    if (pos < 0) pos = 0;

    int next = ((pos + delta) % total + total) % total;
    int target = layout_session_at(&app->layout, next);
    if (target >= 0)
        session_activate(&app->sessions, target);
}

// Восстанавливает вкладки прошлого запуска. Возвращает их число.
//
// Сами процессы перезапуск не пережили, но у агента состояние диалога лежит
// на диске: вкладка открывается тем же профилем, а он уже сам решает
// продолжить прошлую работу.
// Команда, продолжающая именно этот диалог, либо пустая строка: пустую
// (заведённую, но ни разу не записанную) и занятую чужим процессом сессию
// продолжать нечем — тогда сессию выбирает агент обычным порядком.
static void resume_command(const char *cwd, const char *sid, char *out, size_t cap)
{
    out[0] = '\0';
    if (sid && *sid && projinfo_session_exists(cwd, sid) && !projinfo_session_busy(cwd, sid))
        snprintf(out, cap, "claude --resume %s", sid);
}

// Когда в диалоге в последний раз что-то делалось, по хвосту его jsonl;
// 0, если неизвестно.
static time_t session_file_last_work(const char *cwd, const char *sid)
{
    char dir[PROJECT_PATH_MAX];
    projinfo_session_dir(cwd, dir, sizeof(dir));
    char path[PROJECT_PATH_MAX + PROJINFO_ID_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s.jsonl", dir, sid);
    CtxInfo info;
    return ctx_read(path, &info) ? info.at : 0;
}

static int restore_layout(App *app)
{
    const char *path = layout_path();
    if (!path) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int restored = 0;
    int active = -1;
    char line[PROJECT_PATH_MAX + 128];

    while (fgets(line, sizeof(line), f) && app->sessions.count < SESSION_MAX) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\n")] = '\0';

        if (!strncmp(line, "collapsed\t", 10)) {
            layout_set_group_collapsed(&app->layout, line + 10, true);
            continue;
        }
        if (!strncmp(line, "expanded\t", 9)) {
            layout_set_parent_expanded(&app->layout, line + 9, true);
            continue;
        }

        char *cwd = line;
        char *agent_id = strchr(line, '\t');
        if (!agent_id) continue;
        *agent_id++ = '\0';

        char *is_active = strchr(agent_id, '\t');
        if (is_active) *is_active++ = '\0';

        // Вид вкладки и диалог дописаны последними полями; файл от прошлой
        // версии их не содержит, и тогда вкладка была терминалом без имени.
        char *kind = is_active ? strchr(is_active, '\t') : NULL;
        if (kind) *kind++ = '\0';

        char *sid = kind ? strchr(kind, '\t') : NULL;
        if (sid) *sid++ = '\0';

        SessionKind sk = SESSION_KIND_TERM;
        SessionPageKind pk = SESSION_PAGE_PROJECT;
        if (kind && !strcmp(kind, "page")) {
            sk = SESSION_KIND_PAGE;
        } else if (kind && !strcmp(kind, "settings")) {
            sk = SESSION_KIND_PAGE;
            pk = SESSION_PAGE_SETTINGS;
        }

        // Вкладка возвращается в свой диалог, а не тянет жребий заново: без
        // этого десять вкладок одного проекта разбирали сессии в случайном
        // порядке, и «своей» не доставалось никому. Пустую (заведённую, но ни
        // разу не записанную) и занятую чужим процессом продолжать нечем —
        // тогда сессию выбирает агент обычным порядком.
        char resume[PROJINFO_ID_MAX + 32] = "";
        if (sk == SESSION_KIND_TERM) resume_command(cwd, sid, resume, sizeof(resume));

        // Поднимать процессом каждый разговор незачем: десяток `claude`,
        // в которых давно ничего не происходит, — это память и «только что»
        // в панели про всех разом. Давний разговор встаёт страницей, а id
        // остаётся при вкладке — «Продолжить работу» поднимет именно его.
        // Пустой диалог (файл есть, ответов нет) тоже не поднимается: там
        // нечего продолжать, а процесс всё равно стоил бы памяти. Вкладка
        // агента вовсе без диалога — тем более: раньше она заводила новый
        // `claude` при каждом запуске, и у проекта копились пустые сессии.
        const AgentProfile *ag = agent_by_id(agent_id);
        bool agent_tab = ag && ag->has_history;
        if (sk == SESSION_KIND_TERM && agent_tab && app->settings.resume_within > 0) {
            time_t last = resume[0] ? session_file_last_work(cwd, sid) : 0;
            if (last == 0 || time(NULL) - last > (time_t)app->settings.resume_within * 3600) {
                sk = SESSION_KIND_PAGE;
                pk = SESSION_PAGE_PROJECT;
                resume[0] = '\0';
            }
        }

        int index = open_tab_cmd(app, projects_find_by_path(&app->projects, cwd),
                                 cwd, agent_by_id(agent_id), sk, pk,
                                 resume[0] ? resume : NULL);
        if (index < 0) continue;
        if (sid && *sid && sk == SESSION_KIND_PAGE)
            snprintf(app->sessions.items[index].session_id,
                     sizeof(app->sessions.items[index].session_id), "%s", sid);
        restored++;
        if (is_active && *is_active == '1') active = index;
    }
    fclose(f);

    if (active >= 0) session_activate(&app->sessions, active);
    return restored;
}

// Раскладку пишем не только при выходе: если терминал упадёт или его убьют,
// файл всё равно окажется свежим.
static void save_layout(App *app)
{
    const char *path = layout_path();
    if (!path) return;

    // Перед записью уточняем, кто в какой сессии сидит: у вкладок, начавших
    // диалог самостоятельно, имя знает только Claude Code. Момент удачный —
    // раскладка пишется по событиям, а не каждый кадр.
    for (int i = 0; i < app->sessions.count; i++)
        session_track_id(&app->sessions.items[i]);

    // Свёрнутые группы — строками перед вкладками: это тоже состояние панели.
    char extra[LAYOUT_COLLAPSED_MAX * (PROJECT_NAME_MAX + PROJECT_PATH_MAX + 24)] = "";
    for (int i = 0; i < app->layout.collapsed_count; i++) {
        size_t used = strlen(extra);
        snprintf(extra + used, sizeof(extra) - used, "collapsed\t%s\n",
                 app->layout.collapsed[i]);
    }
    // Раскрытые подпроекты — тоже состояние панели.
    for (int i = 0; i < app->layout.expanded_count; i++) {
        size_t used = strlen(extra);
        snprintf(extra + used, sizeof(extra) - used, "expanded\t%s\n",
                 app->layout.expanded[i]);
    }
    session_save_layout(&app->sessions, path, extra);
}

// Горячие клавиши приложения. Возвращает true, если событие потрачено — тогда
// клавиатурный ввод в терминал в этом кадре не идёт, иначе ⌘W заодно отправит
// ^W в оболочку.
static bool handle_hotkeys(App *app)
{
    if (!cmd_down()) return false;

    bool handled = false;

    if (IsKeyPressed(KEY_Q)) {
        session_close_all(&app->sessions);
        handled = true;
    } else if (IsKeyPressed(KEY_T)) {
        // ⌘T — вспомогательная оболочка рядом с работой, в том же каталоге.
        // Проектом она не считается: разговор у проекта один, а это просто
        // руки, чтобы посмотреть файлы, пока агент занят.
        const Session *cur = session_active(&app->sessions);
        char here[SESSION_PATH_MAX];
        snprintf(here, sizeof(here), "%s", cur ? cur->cwd : "");
        open_tab_cmd(app, -1, here[0] ? here : NULL, agent_by_id("shell"),
                     SESSION_KIND_TERM, SESSION_PAGE_PROJECT, NULL);
        resize_all(app);
        save_layout(app);
        handled = true;
    } else if (IsKeyPressed(KEY_W)) {
        session_close(&app->sessions, app->sessions.active);
        save_layout(app);
        handled = true;
    } else if (IsKeyPressed(KEY_B)) {
        layout_toggle_sidebar(&app->layout);
        app->settings.sidebar_visible = app->layout.sidebar_visible;
        save_settings(app);
        handled = true;
    } else if (IsKeyPressed(KEY_R)) {
        reload_projects(app);
        // Открытая страница проекта показывает данные с диска — обновляем и её.
        Session *cur = session_active(&app->sessions);
        if (cur) session_refresh_info(cur);
        handled = true;
    } else if (IsKeyPressed(KEY_COMMA)) {
        open_settings(app);
        handled = true;
    } else if (IsKeyPressed(KEY_I)) {
        toggle_project_page(app, session_active(&app->sessions));
        handled = true;
    } else if (IsKeyPressed(KEY_K)) {
        // Почистить контекст, не выходя из агента. Чем именно — знает профиль:
        // у Claude это /clear, у оболочки — очистка экрана.
        Session *cur = session_active(&app->sessions);
        if (cur && session_has_term(cur) && term_alive(&cur->term)) {
            const char *cmd = cur->agent->clear_context
                ? cur->agent->clear_context : "clear";
            term_send(&cur->term, cmd, strlen(cmd));
            term_send(&cur->term, "\n", 1);
        }
        handled = true;
    } else if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) {
        set_font_size(app, app->font.size + 1);
        app->settings.font_size = app->font.size;
        save_settings(app);
        handled = true;
    } else if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) {
        set_font_size(app, app->font.size - 1);
        app->settings.font_size = app->font.size;
        save_settings(app);
        handled = true;
    } else if (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0)) {
        set_font_size(app, FONT_SIZE_DEFAULT);
        app->settings.font_size = app->font.size;
        save_settings(app);
        handled = true;
    } else if (shift_down() && IsKeyPressed(KEY_LEFT_BRACKET)) {
        step_session(app, -1);
        handled = true;
    } else if (shift_down() && IsKeyPressed(KEY_RIGHT_BRACKET)) {
        step_session(app, +1);
        handled = true;
    } else {
        for (int i = 0; i < 9; i++) {
            if (IsKeyPressed(KEY_ONE + i)) {
                int target = layout_session_at(&app->layout, i);
                if (target >= 0)
                    session_activate(&app->sessions, target);
                handled = true;
                break;
            }
        }
    }

    if (handled)
        input_drain_chars();
    return handled;
}

// Мышь вне области терминала: выбор вкладки и перетаскивание разделителя.
// Возвращает true, если клик забрала панель.
static bool handle_panel_mouse(App *app)
{
    Vector2 m = GetMousePosition();
    bool on_splitter = layout_hit_splitter(&app->layout, m);
    bool in_sidebar = app->layout.sidebar_visible && m.x < app->layout.sidebar.w;

    if (on_splitter || app->splitter_dragging)
        SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
    else if (layout_hit_row(&app->layout, m))
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    else
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);

    // Колесо над панелью прокручивает список: проектов заметно больше,
    // чем помещается в окно.
    if (in_sidebar) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
            layout_scroll_by(&app->layout, (int)(-wheel * app->font.cell_height * 3));
    }

    // Верхняя полоса: кнопка настроек, остальное — просто не терминал.
    if (layout_hit_topbar(&app->layout, m)) {
        if (layout_hit_settings(&app->layout, m)) {
            SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) open_settings(app);
        }
        return true;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (on_splitter) {
            app->splitter_dragging = true;
            app->splitter_grab_dx = (int)m.x - app->layout.sidebar.w;
            return true;
        }
        const PanelRow *row = layout_hit_row(&app->layout, m);
        if (row && row->kind == PANEL_ROW_ADD_GROUP) {
            // Диалог модальный: пока он открыт, кадр стоит. Это нормально —
            // выбор папки и есть то, чем человек сейчас занят.
            const char *home = getenv("HOME");
            char dir[PROJECT_PATH_MAX];
            if (macos_choose_folder(home, "Добавить группу", dir, sizeof(dir))) {
                char name[PROJECT_NAME_MAX];
                if (groups_add(&app->groups, dir, name, sizeof(name))) {
                    groups_save(&app->groups);
                    reload_projects(app);
                }
            }
            return true;
        }
        if (row && row->kind == PANEL_ROW_GROUP) {
            Rect x = layout_row_info_rect(row->rect, app->font.cell_width);
            Rect a = layout_row_group_add_rect(row->rect, app->font.cell_width);
            bool real = strcmp(row->label, "прочее") != 0;
            bool on_x = real && m.x >= x.x && m.x < x.x + x.w;
            bool on_a = real && m.x >= a.x && m.x < a.x + a.w;
            if (on_a) {
                char root[PROJECT_PATH_MAX];
                if (group_root(app, row->label, root, sizeof(root)))
                    page_new_project_begin(root, row->label);
            } else if (on_x) {
                // Скрыть, а не удалить: на диске ничего не меняется, а
                // вернуть можно на экране настроек.
                groups_hide(&app->groups, row->label);
                groups_save(&app->groups);
                reload_projects(app);
            } else {
                layout_set_group_collapsed(&app->layout, row->label, !row->collapsed);
                save_layout(app);
            }
            return true;
        }
        if (row && row->kind == PANEL_ROW_ITEM && row->has_subs && row->project >= 0) {
            Rect a = layout_row_expand_rect(row->rect, app->font.cell_width);
            if (m.x >= a.x && m.x < a.x + a.w) {
                layout_set_parent_expanded(&app->layout,
                                           app->projects.items[row->project].path,
                                           !row->expanded);
                save_layout(app);
                return true;
            }
        }
        if (row) {
            Rect info = layout_row_info_rect(row->rect, app->font.cell_width);
            bool on_info = m.x >= info.x && m.x < info.x + info.w
                        && m.y >= info.y && m.y < info.y + info.h;

            if (on_info && row->session >= 0) {
                // Значок в строке открытой вкладки показывает её страницу, не
                // трогая работу: процесс остаётся жив за страницей.
                session_activate(&app->sessions, row->session);
                Session *s = &app->sessions.items[row->session];
                if (!s->show_page) toggle_project_page(app, s);
            } else if (on_info && row->project >= 0) {
                open_tab(app, row->project, NULL,
                         agent_by_id(app->settings.default_agent),
                         SESSION_KIND_PAGE, SESSION_PAGE_PROJECT);
                resize_all(app);
                save_layout(app);
            } else if (row->session >= 0) {
                session_activate(&app->sessions, row->session);
            } else if (row->project >= 0) {
                activate_project(app, row->project);
            }
            return true;
        }
    }

    if (app->splitter_dragging) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            layout_set_sidebar_width(&app->layout,
                                     (int)m.x - app->splitter_grab_dx);
        } else {
            app->splitter_dragging = false;
            // Пишем по отпусканию, а не на каждый кадр перетаскивания: иначе
            // файл переписывался бы шестьдесят раз в секунду.
            app->settings.sidebar_width = app->layout.sidebar_width;
            save_settings(app);
        }
        return true;
    }

    // Клик или колесо в панели — не для терминала и не для страницы: колесо
    // одно на кадр, и без этого оно крутило список проектов и содержимое
    // вкладки разом.
    return in_sidebar && (IsMouseButtonDown(MOUSE_BUTTON_LEFT)
                          || GetMouseWheelMove() != 0.0f);
}

// Плашка поверх области терминала, когда процесс во вкладке завершился.
static void draw_exit_banner(const App *app, const Session *s, Rect area)
{
    char msg[160];
    if (s->term.child_status >= 0)
        snprintf(msg, sizeof(msg), "[процесс завершился, код %d — ⌘W закрыть вкладку]",
                 s->term.child_status);
    else
        snprintf(msg, sizeof(msg), "[процесс завершился — ⌘W закрыть вкладку]");

    Vector2 size = MeasureTextEx(app->font.font, msg, (float)app->font.size, 0);
    int h = (int)size.y + 8;
    DrawRectangle(area.x, area.y + area.h - h, area.w, h, (Color){0, 0, 0, 180});
    DrawTextEx(app->font.font, msg,
               (Vector2){ area.x + (area.w - size.x) / 2, area.y + area.h - h + 4 },
               (float)app->font.size, 0, WHITE);
}

int main(int argc, char **argv)
{
    log_build_info();

    // Чистим окружение до создания первой вкладки: иначе она унаследует метки
    // сессии того процесса, из которого запустили терминал.
    claude_clear_session_env();

    // HiDPI включаем до создания окна, иначе raylib не заведёт фреймбуфер
    // в натуральном разрешении дисплея.
    SetConfigFlags(FLAG_WINDOW_HIGHDPI);
    // Метка в заголовке, чтобы не перепутать рабочее окно с только что
    // собранным: терминал дорабатывают, сидя в нём же, и два одинаковых окна
    // рядом — верный способ закрыть не то.
    const char *label = getenv("BERTH_LABEL");
    char window_title[128];
    if (label && *label)
        snprintf(window_title, sizeof(window_title), "berth · %s", label);
    else
        snprintf(window_title, sizeof(window_title), "berth");

    InitWindow(1100, 700, window_title);
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetTargetFPS(60);
    // У raylib клавиша выхода по умолчанию — Escape. В терминале её ждут vim
    // и агент, а на странице она закрывает редактор задачи; закрывать окно
    // ей нечего.
    SetExitKey(KEY_NULL);

#if defined(__APPLE__)
    // ⌘W и ⌘Q обрабатываем сами, иначе меню Cocoa закроет окно и убьёт
    // процесс мимо уборки дочерних оболочек.
    macos_release_window_shortcuts();
    macos_activate_app();
    macos_set_dock_icon(icon_png, sizeof(icon_png));
#endif

    App app = {0};
    app.theme = THEME_DARK;

    settings_defaults(&app.settings);
    const char *cfg = settings_path();
    // Файла может ещё не быть. Создаём его сразу с умолчаниями: настройку
    // правят руками, а для этого её надо сперва увидеть.
    if (cfg && !settings_load(&app.settings, cfg))
        settings_save(&app.settings, cfg);
    // Тема окна — из настроек, до восстановления вкладок: их терминалы
    // красятся по тому же правилу.
    app.theme = *theme_by_name(app.settings.theme_panel);

    // Размер шрифта из настроек, но переменная окружения его перебивает:
    // dev-окно рядом с рабочим удобно запускать другим кеглем, не трогая
    // общий файл.
    int font_size = app.settings.font_size;
    const char *env_size = getenv("BERTH_FONT_SIZE");
    if (env_size && *env_size) {
        int v = atoi(env_size);
        if (v >= FONT_SIZE_MIN && v <= FONT_SIZE_MAX) font_size = v;
    }

    if (!sprites_load(&app.sprites))
        fprintf(stderr, "berth: атлас спрайтов не загрузился, сценки не будет\n");

    if (!font_load(&app.font, font_size, GetWindowScaleDPI())) {
        fprintf(stderr, "не удалось загрузить шрифт\n");
        CloseWindow();
        return 1;
    }

    term_global_init();
    session_list_init(&app.sessions);
    {
        char own[PROJECT_PATH_MAX];
        const char *dir = config_dir();
        snprintf(own, sizeof(own), "%s/usage.json", dir ? dir : ".");
        usage_set_own_path(&app.usage, own);
        // Скиллы берта — в папку пакета, уже зная, какие выключены.
        claude_install_bundle(app.settings.skills_off);
    }
    usage_load(&app.usage);
    find_brief_script(&app);
    groups_load(&app.groups, groups_path());
    projects_load(&app.projects, projects_default_path());
    groups_apply(&app.groups, &app.projects);
    layout_init(&app.layout, metrics_for(&app.font));
    layout_set_sidebar_width(&app.layout, app.settings.sidebar_width);
    if (!app.settings.sidebar_visible)
        layout_toggle_sidebar(&app.layout);
    app.layout.scene = settings_scene_enabled(&app.settings) && app.sprites.ready;
    {
        char xp_path[PROJECT_PATH_MAX];
        snprintf(xp_path, sizeof(xp_path), "%s/xp.tsv", config_dir());
        xp_init(xp_path);
    }
    layout_compute(&app.layout, &app.projects, &app.sessions,
                   GetScreenWidth(), GetScreenHeight());

    // Каждый аргумент — каталог проекта, открываемый вкладкой. Без аргументов
    // возвращаем раскладку прошлого запуска.
    if (argc > 1) {
        for (int i = 1; i < argc && app.sessions.count < SESSION_MAX; i++) {
            // Путь из списка проектов открываем как проект — со всем, что
            // о нём известно: именем, группой, цветом.
            int project = projects_find_by_path(&app.projects, argv[i]);
            open_session(&app, project, argv[i],
                         project >= 0 ? agent_by_id("claude") : agent_default());
        }
        session_activate(&app.sessions, 0);
    } else if (restore_layout(&app) == 0) {
        // Восстанавливать нечего — открываем оболочку там, откуда запустили:
        // терминал открыли, ещё не выбрав, чем заняться.
        char cwd[SESSION_PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
        open_session(&app, projects_find_by_path(&app.projects, cwd),
                     cwd[0] ? cwd : NULL, agent_default());
    }

    if (app.sessions.count == 0) {
        fprintf(stderr, "не удалось открыть ни одной сессии\n");
        font_unload(&app.font);
        CloseWindow();
        return 1;
    }
    resize_all(&app);
    save_layout(&app);

    int prev_w = GetScreenWidth(), prev_h = GetScreenHeight();
    app.last_term_area = app.layout.term;
    int frames_since_check = 0;
    int titled_session = -1;   // чьим именем сейчас названо окно

    // Кадр: раскладка → горячие клавиши → мышь → ввод → вывод процессов → отрисовка.
    while (!WindowShouldClose() && app.sessions.count > 0) {
        int win_w = GetScreenWidth();
        int win_h = GetScreenHeight();
        layout_compute(&app.layout, &app.projects, &app.sessions, win_w, win_h);

        // Список проектов правится снаружи (скриптом-генератором), поэтому
        // раз в пару секунд смотрим на время правки файла. Читать его каждый
        // кадр незачем, а требовать перезапуска — глупо.
        if (++frames_since_check > 120) {
            frames_since_check = 0;
            if (projects_changed(&app.projects))
                reload_projects(&app);
            if (groups_changed(&app.groups)) {
                groups_load(&app.groups, groups_path());
                reload_projects(&app);
            }
            if (subprojects_changed(&app.projects))
                reload_projects(&app);

            // Настройки правят и руками, файлом. Тем же способом и по той же
            // причине: требовать перезапуска ради размера шрифта — глупо.
            if (cfg && settings_changed(cfg)) {
                settings_load(&app.settings, cfg);
                apply_settings(&app);
                claude_install_bundle(app.settings.skills_off);
            }

            // Задачи агент отмечает прямо в файле проекта — тем же опросом.
            projstate_poll();
            xp_flush_maybe();

            // Состояние вкладок — из реестра Claude Code, тем же опросом:
            // по нему панель пишет «работает», «ждёт», «зовёт».
            for (int i = 0; i < app.sessions.count; i++)
                session_track_id(&app.sessions.items[i]);

            // Лимиты Claude Code пишет в ~/.claude.json сам, когда спрашивает
            // их у сервера. Файл трогают часто, поэтому смотрим на время
            // правки, а не перечитываем полторы сотни килобайт каждый раз.
            if (usage_changed(&app.usage))
                usage_load(&app.usage);
            // Свой запрос раз в три минуты, и только пока что-то происходит:
            // в последние десять минут человек трогал окно или агент
            // работал. Стоящий компьютер лимит не тратит, и спрашивать
            // сервер за него незачем. Вернулся — первый же кадр спросит,
            // если с прошлого запроса прошло больше трёх минут.
            if (app.settings.usage_fetch && GetTime() - app.last_activity < 600)
                usage_fetch_maybe(&app.usage, 180);
        }

        // Сетку пересчитываем и на изменение окна, и на движение разделителя:
        // ширина панели меняет ширину терминала ровно так же, как ресайз окна.
        Rect ta = app.layout.term;
        if (win_w != prev_w || win_h != prev_h ||
            ta.w != app.last_term_area.w || ta.h != app.last_term_area.h) {
            resize_all(&app);
            prev_w = win_w;
            prev_h = win_h;
            app.last_term_area = ta;
        }

        // Пока открыт редактор нового проекта, клавиатура его: ни горячим
        // клавишам, ни терминалу под ним она не достаётся.
        bool consumed = page_overlay_active() ? true : handle_hotkeys(&app);
        if (app.sessions.count == 0) break;

        bool panel_took_mouse = handle_panel_mouse(&app);

        Session *active = session_active(&app.sessions);

        if (active) {
            bool focused = IsWindowFocused();

            // Пока окно было неактивно, raylib копил нажатия: набранное в
            // другом приложении или в оверлее снимка экрана иначе улетело бы
            // во вкладку. Возвращая фокус, очередь выбрасываем.
            static bool was_focused = true;
            if (focused && !was_focused)
                input_drain_chars();
            was_focused = focused;

            // Ввод получает только вкладка с процессом: у страницы нет pty,
            // и опрашивать её терминал нельзя — там нули, а не открытый файл.
            if (session_has_term(active) && !session_shows_page(active)) {
                term_set_focus(&active->term, focused);

                // Скроллбар обрабатываем до передачи мыши приложению, иначе клик
                // по полоске утечёт внутрь vim или tmux как посторонний клик.
                bool scrollbar_busy = !panel_took_mouse
                    && input_scrollbar(&active->term, app.layout.term);

                if (!consumed && term_alive(&active->term)) {
                    input_keys(&active->term);
                    if (!scrollbar_busy && !panel_took_mouse)
                        input_mouse(&active->term, app.layout.term, PAD);
                }
            }

            // Заголовок окна принадлежит активной вкладке, поэтому обновляем
            // его и когда процесс прислал новый через OSC, и когда переключили
            // вкладку: иначе окно продолжает называться прошлой сессией.
            if (active->term.title_changed || app.sessions.active != titled_session) {
                char base[256];
                session_window_title(active, base, sizeof(base));

                char title[320];
                if (label && *label)
                    snprintf(title, sizeof(title), "%s · %s", base, label);
                else
                    snprintf(title, sizeof(title), "%s", base);
                SetWindowTitle(title);
                active->term.title_changed = false;
                titled_session = app.sessions.active;
            }
        }

        session_poll_all(&app.sessions);
        reap_tasks(&app);

        // Признаки работы: мышь, клавиши, колесо — человек за окном; агент
        // в состоянии «работает» — тратит лимит и без человека. Клавиши
        // смотрим по состоянию, а не по очереди нажатий: очередь читает
        // ввод, и забирать её здесь значило бы красть символы у вкладки.
        {
            Vector2 d = GetMouseDelta();
            bool active = d.x != 0 || d.y != 0 || GetMouseWheelMove() != 0.0f
                       || IsMouseButtonDown(MOUSE_BUTTON_LEFT)
                       || IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
            for (int k = KEY_SPACE; !active && k <= KEY_KB_MENU; k++)
                if (IsKeyDown(k)) active = true;
            for (int i = 0; !active && i < app.sessions.count; i++)
                if (app.sessions.items[i].state == SESSION_STATE_BUSY) active = true;
            if (active) app.last_activity = GetTime();
        }

        // Отложенный Enter после вставки задачи — см. Session.enter_due.
        for (int i = 0; i < app.sessions.count; i++) {
            Session *s = &app.sessions.items[i];
            if (s->enter_due <= 0 || GetTime() < s->enter_due) continue;
            s->enter_due = 0;
            if (session_has_term(s) && term_alive(&s->term))
                term_send(&s->term, "\r", 1);
        }

        // Снимок для отрисовки нужен только активной: остальные всё равно не
        // рисуются, а обновление снимка — не бесплатная операция.
        if (active && session_has_term(active) && !session_shows_page(active))
            term_update_render_state(&active->term);

        // Что нажали на странице, исполняем уже после кадра: смена размера
        // шрифта пересоздаёт атлас, а трогать текстуры между BeginDrawing и
        // EndDrawing — напрашиваться на неприятности.
        PageEvent page_event = { 0 };

        scene_tick(&app, GetFrameTime());

        BeginDrawing();
        ClearBackground(app.theme.sidebar_bg);

        if (active && session_shows_page(active)) {
            // Клик по странице обрабатывается там же, где она рисуется. Если
            // мышь забрала панель, страница о ней не должна знать: иначе клик
            // по вкладке проваливался бы в кнопку под ней.
            Vector2 pm = panel_took_mouse || page_overlay_active()
                       ? (Vector2){ -1, -1 } : GetMousePosition();
            page_event = active->page == SESSION_PAGE_SETTINGS
                ? page_draw_settings(&app.settings, &app.groups, &app.usage, &app.font, active->theme,
                                     app.layout.term, pm, active->page_scroll)
                : page_draw_project(active, projstate_get(active->cwd),
                                    &app.sessions, &app.projects, &app.font, active->theme,
                                    app.layout.term, pm, active->page_scroll);

            // Прокрутка колесом. Предел берём из того, что нарисовалось в этом
            // кадре: сколько содержимого не поместилось, столько и можно
            // увести вверх — иначе страница уезжала бы в пустоту.
            // overflow — это сколько всего содержимого не помещается, то есть
            // и есть предел прокрутки. Раньше к нему прибавлялась текущая
            // прокрутка, и со второго раза страница уезжала в пустоту.
            int limit = page_event.overflow;
            if (!panel_took_mouse) {
                float wheel = GetMouseWheelMove();
                if (wheel != 0.0f) {
                    active->page_scroll -= (int)(wheel * app.font.cell_height * 3);
                    if (active->page_scroll < 0) active->page_scroll = 0;
                }
            }
            if (active->page_scroll > limit) active->page_scroll = limit;

            // Страница просит показать кусок: сдвигаем ровно настолько, чтобы
            // он вошёл, — вниз, если вылез за низ окна, вверх, если ушёл под
            // шапку. Не к концу: задачи давно не последнее на странице.
            if (page_event.reveal) {
                int view_bottom = app.layout.term.y + app.layout.term.h - 12;
                if (page_event.reveal_bottom > view_bottom)
                    active->page_scroll += page_event.reveal_bottom - view_bottom;
                if (page_event.reveal_top < page_event.scroll_top)
                    active->page_scroll -= page_event.scroll_top - page_event.reveal_top;
                if (active->page_scroll > limit) active->page_scroll = limit;
                if (active->page_scroll < 0) active->page_scroll = 0;
            }

            // Открылись — уводим сразу к свежему концу ленты. Страница просит
            // того же сама, когда внизу выросло что-то, что надо видеть.
            if (page_event.scroll_end) active->page_scroll_end = true;
            if (active->page_scroll_end) {
                active->page_scroll = limit;
                active->page_scroll_end = false;
            }
        } else if (active) {
            GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
            ghostty_render_state_colors_get(active->term.render_state, &colors);
            Color bg = { colors.background.r, colors.background.g, colors.background.b, 255 };
            DrawRectangle(app.layout.term.x, app.layout.term.y,
                          app.layout.term.w, app.layout.term.h, bg);

            render_term(&active->term, &app.font, app.layout.term,
                        app.font.size, PAD);
            if (!term_alive(&active->term))
                draw_exit_banner(&app, active, app.layout.term);
        }

        // Карточка нового проекта — поверх страницы или терминала, под
        // панелью и полосой: они про окно, а она про содержимое.
        PageEvent overlay_event = { 0 };
        if (page_overlay_active())
            overlay_event = page_draw_overlay(&app.font,
                                              active && active->theme ? active->theme : &app.theme,
                                              app.layout.term, GetMousePosition());

        ui_draw_sidebar(&app.layout, &app.projects, &app.sessions,
                        &app.font, &app.theme,
                        GetMousePosition(),
                        app.splitter_dragging
                            || layout_hit_splitter(&app.layout, GetMousePosition()),
                        app.layout.scene ? &app.sprites : NULL);
        // Полоса — последней: её балуны (подсказка к лимиту) висят поверх
        // всего, что ниже.
        ui_draw_topbar(&app.layout, &app.sessions, &app.usage, &app.font, &app.theme,
                       GetMousePosition());

        EndDrawing();

        // Текстуры картинок Kitty освобождаем только после EndDrawing().
        render_flush_deferred();

        if (page_event.kind != PAGE_EVENT_NONE && active)
            handle_page_event(&app, active, page_event);
        if (overlay_event.kind == PAGE_EVENT_NEW_PROJECT)
            handle_new_project(&app, overlay_event);
    }

    save_layout(&app);
    xp_flush();
    session_close_all(&app.sessions);
    font_unload(&app.font);
    CloseWindow();
    return 0;
}
