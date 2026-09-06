#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

#include "session.h"
#include "xp.h"
#include "claude.h"
#include "projstate.h"

void session_list_init(SessionList *list)
{
    memset(list, 0, sizeof(*list));
    list->active = -1;
}

// Последний сегмент пути — обычно это и есть имя проекта.
static const char *basename_of(const char *path)
{
    if (!path || !*path) return "";
    const char *slash = strrchr(path, '/');
    return (slash && slash[1]) ? slash + 1 : path;
}

// Группа по умолчанию — имя родительского каталога. Проекты, лежащие рядом
// в ~/personal или ~/projects, сами собираются в группы, ещё до того как
// появится их описание в настройках.
static void parent_dir_name(char *dst, size_t cap, const char *path)
{
    dst[0] = '\0';
    if (!path || !*path) return;

    const char *last = strrchr(path, '/');
    if (!last || last == path) return;

    const char *prev = last - 1;
    while (prev > path && *prev != '/') prev--;
    if (*prev == '/') prev++;

    size_t n = (size_t)(last - prev);
    if (n == 0 || n >= cap) return;
    memcpy(dst, prev, n);
    dst[n] = '\0';
}

// Цвет группы — детерминированный по её имени, а не по позиции в списке.
// Иначе цвета перетасовывались бы при каждом открытии и закрытии вкладки,
// а глаз привыкает именно к устойчивому цвету.
static Color color_for_group(const char *name)
{
    static const Color palette[] = {
        {120, 170, 245, 255},  // синий
        {130, 200, 150, 255},  // зелёный
        {225, 170,  95, 255},  // янтарный
        {200, 140, 220, 255},  // сиреневый
        {235, 130, 130, 255},  // красный
        {120, 200, 210, 255},  // бирюзовый
    };
    const int n = (int)(sizeof(palette) / sizeof(palette[0]));

    // FNV-1a — короткий и хорошо перемешивает короткие строки.
    uint32_t h = 2166136261u;
    for (const char *p = name; p && *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return palette[h % (uint32_t)n];
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Выполняет команду паспорта и выводит результат прямо на экран вкладки.
//
// Блокирует кадр на время работы скрипта — он быстрый (единицы миллисекунд),
// а открытие вкладки происходит редко. Если паспорт когда-нибудь начнёт лезть
// в сеть, это придётся переделать на чтение по кадрам.
static void print_brief(Session *s, const char *cmd)
{
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return;

    // Перевод строки надо превращать в возврат каретки с переводом строки.
    // Обычно это делает драйвер pty (режим ONLCR), но мы пишем на экран
    // напрямую, минуя его, и без замены текст пошёл бы лесенкой.
    char in[4096];
    char out[sizeof(in) * 2];
    size_t total = 0;
    size_t n;
    while ((n = fread(in, 1, sizeof(in), pipe)) > 0) {
        size_t m = 0;
        for (size_t i = 0; i < n; i++) {
            if (in[i] == '\n') out[m++] = '\r';
            out[m++] = in[i];
        }
        term_feed(&s->term, out, m);

        total += n;
        if (total > 256 * 1024) break;   // защита от бесконечного вывода
    }
    pclose(pipe);

    term_feed(&s->term, "\r\n", 2);
}

// Открывает pty, красит его темой вкладки и запускает агента. Отдельно от
// session_open, потому что тем же путём идёт страница, которую превращают в
// терминал кнопкой: вкладка уже существует, а процесса у неё ещё нет.
// Какой диалог продолжает команда. Разбираем ту самую строку, которую
// отправляем в оболочку: другого места, где это знание есть, нет — id выбирает
// projinfo, а до вкладки доезжает уже готовая команда.
static void id_from_command(Session *s, const char *cmd)
{
    if (!cmd) return;
    const char *flag = strstr(cmd, "--resume ");
    if (!flag) return;

    const char *id = flag + strlen("--resume ");
    size_t n = strcspn(id, " \t");
    if (!n || n >= sizeof(s->session_id)) return;
    memcpy(s->session_id, id, n);
    s->session_id[n] = '\0';

    // Форк заводит собственный диалог: id родителя ему не принадлежит, и
    // держать его за вкладкой нельзя — иначе при перезапуске она уведёт
    // чужую сессию. Настоящее имя узнаем из реестра.
    if (strstr(cmd, "--fork-session")) s->session_id[0] = '\0';
}

static void term_apply_theme(Term *t, const Theme *theme);

static bool start_term(Session *s, const AgentProfile *agent,
                       uint16_t cols, uint16_t rows,
                       int cell_width, int cell_height,
                       const char *brief, const char *command)
{
    TermOpts topts = {
        .cwd = s->cwd,
        .cols = cols,
        .rows = rows,
        .cell_width = cell_width,
        .cell_height = cell_height,
    };
    if (!term_open(&s->term, &topts)) return false;

    // Цвета терминала задаёт тема вкладки. Приложение внутри вольно
    // переопределить их своими escape-последовательностями — это лишь
    // значения по умолчанию. Прямо в терминал: kind здесь может быть ещё
    // «страница», и session_set_theme ничего бы не сделала.
    term_apply_theme(&s->term, s->theme ? s->theme : theme_default());

    // Паспорт проекта печатается первым, до запуска агента: человек видит,
    // куда попал, ещё до того как агент начнёт занимать экран.
    if (brief && *brief)
        print_brief(s, brief);

    // Агент запускается так же, как если бы команду набрали руками. Если у
    // проекта есть прошлая работа — продолжаем её, а не начинаем заново:
    // флаги запуска не должны быть заботой человека.
    if (!agent) agent = s->agent;
    const char *launch = agent->launch;

    char resume[256];
    if (agent->launch_resume && agent->has_history && agent->has_history(s->cwd)) {
        launch = agent->launch_resume;
        // Агент может уточнить, чем именно продолжать: свежая сессия бывает
        // занята работающим в ней фоновым процессом.
        if (agent->resume_command) {
            agent->resume_command(s->cwd, resume, sizeof(resume));
            if (resume[0]) launch = resume;
        }
    }
    if (command && *command) launch = command;

    // Запомним, какой диалог продолжаем: из этой же строки, которую сейчас
    // отправим. Предварительно — у форка реальный id будет другим, и его
    // потом уточнит session_track_id по реестру живых процессов.
    id_from_command(s, launch);

    if (launch && *launch) {
        // Скиллы берта едут с каждым запуском claude: --add-dir на папку
        // пакета. Одно место на все пути запуска — страница, раскладка,
        // задачи, отправка задачи в новый разговор.
        char full[1600];
        claude_with_bundle(launch, full, sizeof(full));
        // Задача — это один процесс, а не оболочка с командой внутри: без
        // exec после `claude -p` оставалось приглашение, оболочка жила
        // дальше, и вкладка вечно «работала». С exec код выхода задачи —
        // код выхода claude, и по нему видно, удалась ли она.
        if (s->role == SESSION_ROLE_TASK)
            term_send(&s->term, "exec ", 5);
        term_send(&s->term, full, strlen(full));
        term_send(&s->term, "\n", 1);
    }
    return true;
}

// Вкладка проекта, если она уже открыта. Проект и разговор — одно и то же:
// у проекта одна живая сессия, а всё прошлое лежит в журнале и живёт там как
// история, а не как десять процессов, ждущих неизвестно чего.
int session_of_project(const SessionList *list, const char *cwd)
{
    if (!cwd || !*cwd) return -1;
    for (int i = 0; i < list->count; i++)
        if (list->items[i].role == SESSION_ROLE_MAIN
            && !strcmp(list->items[i].cwd, cwd))
            return i;
    return -1;
}

int session_open(SessionList *list, const SessionOpts *opts)
{
    if (list->count >= SESSION_MAX) return -1;

    // Второго разговора у проекта не бывает: если он уже открыт — это он и
    // есть. Инвариант держим здесь, а не в каждом месте, откуда открывают
    // вкладку: забыть проверку в одном из них — вопрос времени. Задачи под
    // это правило не подпадают: они рядом с разговором, а не вместо него.
    if (opts->role != SESSION_ROLE_TASK) {
        const char *want = (opts->cwd && *opts->cwd) ? opts->cwd : getenv("HOME");
        int existing = session_of_project(list, want);
        if (existing >= 0) {
            list->active = existing;
            return existing;
        }
    }

    Session *s = &list->items[list->count];
    memset(s, 0, sizeof(*s));
    s->page_task_sub = -1;   // ноль — это первый подпроект, а не «свои»

    const char *cwd = opts->cwd;
    if (!cwd || !*cwd) cwd = getenv("HOME");
    copy_str(s->cwd, sizeof(s->cwd), cwd);

    copy_str(s->name, sizeof(s->name),
             (opts->name && *opts->name) ? opts->name : basename_of(s->cwd));
    if (opts->group && *opts->group)
        copy_str(s->group, sizeof(s->group), opts->group);
    else
        parent_dir_name(s->group, sizeof(s->group), s->cwd);

    s->color = opts->color.a ? opts->color : color_for_group(s->group);
    s->agent = opts->agent ? opts->agent : agent_default();
    s->project = opts->project;
    s->theme = opts->theme ? opts->theme : &THEME_DARK;
    s->state = SESSION_STATE_IDLE;
    s->progress = -1;
    s->kind = opts->kind;
    s->page = opts->page;
    s->role = opts->role;
    if (opts->role == SESSION_ROLE_TASK && opts->task_name)
        copy_str(s->name, sizeof(s->name), opts->task_name);

    if (s->kind == SESSION_KIND_PAGE) {
        // У страницы нет ни процесса, ни pty: она рисуется нами, а её
        // содержимое читается с диска. Именно get, а не перечитывание: при
        // восстановлении раскладки вкладок одного проекта бывает десяток, и
        // журнал незачем разбирать десять раз подряд.
        projstate_get(s->cwd);
        // К свежему концу ленты — только страница проекта. Настройки
        // читаются сверху.
        s->page_scroll_end = (s->page == SESSION_PAGE_PROJECT);
        list->active = list->count;
        list->count++;
        return list->active;
    }

    if (!start_term(s, s->agent, opts->cols, opts->rows,
                    opts->cell_width, opts->cell_height, opts->brief, opts->command))
        return -1;

    list->active = list->count;
    list->count++;
    return list->active;
}

bool session_start_term(Session *s, const AgentProfile *agent,
                        uint16_t cols, uint16_t rows,
                        int cell_width, int cell_height,
                        const char *brief, const char *command)
{
    if (s->kind == SESSION_KIND_TERM) return true;   // уже терминал

    if (!start_term(s, agent, cols, rows, cell_width, cell_height, brief, command))
        return false;

    s->kind = SESSION_KIND_TERM;
    s->agent = agent ? agent : s->agent;
    s->state = SESSION_STATE_IDLE;
    s->show_page = false;
    return true;
}

static GhosttyColorRgb rgb_of(Color c)
{
    return (GhosttyColorRgb){ c.r, c.g, c.b };
}

// Тема вкладки красит только её терминал: панель и страницы берут тему
// окна. Живому терминалу цвета ставятся сразу — смена темы в настройках
// видна без перезапуска.
// Цвета темы — в терминал. Отдельно от session_set_theme: при превращении
// страницы в терминал kind ещё «страница», и проверка session_has_term
// молча пропускала бы установку — так и вышло: первый запуск агента шёл
// в цветах по умолчанию, пока тему не передёргивали в настройках.
static void term_apply_theme(Term *t, const Theme *theme)
{
    term_set_default_colors(t, rgb_of(theme->term_bg), rgb_of(theme->term_fg));
    GhosttyColorRgb ansi[16];
    for (int i = 0; i < 16; i++) ansi[i] = rgb_of(theme->palette[i]);
    term_set_palette(t, rgb_of(theme->cursor), ansi);
}

void session_set_theme(Session *s, const Theme *theme)
{
    if (!theme) theme = theme_default();
    s->theme = theme;
    if (!session_has_term(s)) return;
    term_apply_theme(&s->term, theme);
}


void session_refresh_info(Session *s)
{
    if (s->page != SESSION_PAGE_PROJECT) return;
    projstate_reload(s->cwd);
}

void session_track_id(Session *s)
{
    if (!session_has_term(s) || !term_alive(&s->term)) return;

    char id[PROJINFO_ID_MAX];
    time_t since = 0;
    ProjLiveStatus live = projinfo_live_of_pid(s->term.child, id, sizeof(id), &since);
    if (since > 0) s->state_since = since;

    // Реестр авторитетнее нашей догадки из команды: там записано то, чем
    // Claude Code себя считает на самом деле. Пустой ответ ничего не значит —
    // процесс мог ещё не успеть зарегистрироваться, — и старое имя мы в этом
    // случае не стираем.
    if (id[0]) snprintf(s->session_id, sizeof(s->session_id), "%s", id);

    // Состояние — оттуда же: Claude Code сам пишет, работает он, ждёт или
    // просит ответа. Снаружи это не угадать: экран одинаково молчит и когда
    // агент думает, и когда ждёт подтверждения.
    switch (live) {
    case PROJ_LIVE_BUSY:    s->state = SESSION_STATE_BUSY;      break;
    case PROJ_LIVE_WAITING: s->state = SESSION_STATE_ATTENTION; break;
    case PROJ_LIVE_IDLE:    s->state = SESSION_STATE_IDLE;      break;
    case PROJ_LIVE_NONE:    break;   // нет процесса — нечего и говорить
    }

    session_track_ctx(s);
}

bool session_empty(const Session *s)
{
    return s->ctx_mtime > 0 && s->ctx.at == 0;
}

time_t session_last_work(const Session *s)
{
    if (s->ctx.at > 0) return s->ctx.at;
    // Файл прочитан, ответов нет — время процесса ничего не скажет.
    if (session_empty(s)) return 0;
    return s->state_since;
}

bool session_sleeping(const Session *s, time_t now, int after)
{
    if (!session_has_term(s) || !s->agent->has_history) return false;
    if (s->state != SESSION_STATE_IDLE) return false;
    if (session_empty(s)) return true;   // работы не было — спать нечему мешать
    time_t last = session_last_work(s);
    return last > 0 && now - last >= after;
}

// Давность по-человечески: «только что», «12 мин назад», «3 ч назад»,
// «2 дн назад».
static const char *ago_text(time_t since, char *buf, size_t cap)
{
    long d = (long)(time(NULL) - since);
    if (d < 60)          snprintf(buf, cap, "только что");
    else if (d < 3600)   snprintf(buf, cap, "%ld мин назад", d / 60);
    else if (d < 86400)  snprintf(buf, cap, "%ld ч назад", d / 3600);
    else                 snprintf(buf, cap, "%ld дн назад", d / 86400);
    return buf;
}

// Остаток контекста — из jsonl разговора. Файл только дописывается, и
// читается лишь его хвост, но и это делается только когда файл изменился:
// опрос идёт раз в пару секунд по всем вкладкам.
void session_track_ctx(Session *s)
{
    if (!s->session_id[0]) return;
    char dir[SESSION_PATH_MAX];
    projinfo_session_dir(s->cwd, dir, sizeof(dir));
    char path[SESSION_PATH_MAX + PROJINFO_ID_MAX + 8];
    snprintf(path, sizeof(path), "%s/%s.jsonl", dir, s->session_id);

    struct stat st;
    if (stat(path, &st) != 0) return;
    if (st.st_mtime == s->ctx_mtime) return;
    s->ctx_mtime = st.st_mtime;

    CtxInfo info;
    if (ctx_read(path, &info)) s->ctx = info;

    if (!s->spent_started) { s->spent_offset = -1; s->spent_started = true; }
    long spent = ctx_spent_since(path, &s->spent_offset, s->spent_last_id);
    s->tokens_out += spent;
    xp_add(s->cwd, spent);
}

// Заголовок без служебных значков впереди. Claude Code ставит перед именем
// сессии свою крутилку (✳, ✶, ◐ и подобные): в шрифте её половины нет, и
// в панели она выглядела как «?». Название работы — то, что после неё.
static const char *title_words(const char *title)
{
    const unsigned char *p = (const unsigned char *)title;
    for (;;) {
        uint32_t cp;
        int n;
        if (p[0] < 0x80)              { cp = p[0]; n = 1; }
        else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
        else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
        else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
        else break;
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) return (const char *)p;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        bool skip = cp == ' ' || cp == 0xFE0F
                 || (cp >= 0x2000 && cp <= 0x2BFF)     // знаки, стрелки, фигуры
                 || (cp >= 0x1F000 && cp <= 0x1FAFF);  // эмодзи
        if (!skip || cp == 0) break;
        p += n;
    }
    return (const char *)p;
}

void session_close(SessionList *list, int index)
{
    if (index < 0 || index >= list->count) return;

    if (session_has_term(&list->items[index]))
        term_close(&list->items[index].term);

    // Сдвигаем хвост. Term содержит только собственные указатели и дескриптор,
    // внешних ссылок на него нет, поэтому перемещение структуры безопасно.
    for (int i = index; i < list->count - 1; i++)
        list->items[i] = list->items[i + 1];
    list->count--;
    memset(&list->items[list->count], 0, sizeof(Session));

    if (list->count == 0) {
        list->active = -1;
    } else if (list->active > index) {
        list->active--;
    } else if (list->active >= list->count) {
        list->active = list->count - 1;
    }
}

void session_close_all(SessionList *list)
{
    // Сначала просим уйти всех, потом собираем: иначе окно закрывается
    // столько времени, сколько сумма ожиданий по вкладкам.
    for (int i = 0; i < list->count; i++)
        if (session_has_term(&list->items[i]))
            term_request_close(&list->items[i].term);
    for (int i = 0; i < list->count; i++)
        if (session_has_term(&list->items[i]))
            term_close(&list->items[i].term);
    list->count = 0;
    list->active = -1;
}

void session_activate(SessionList *list, int index)
{
    if (index < 0 || index >= list->count) return;
    list->active = index;
}

// Раскладка пишется простыми строками с табуляцией: каталог, профиль агента,
// признак активной, вид вкладки, диалог Claude Code. Формат должен читаться глазами и чиниться руками — это
// файл состояния, а не данные, ради которых стоит заводить разбор JSON.
bool session_save_layout(const SessionList *list, const char *path, const char *extra)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "# раскладка вкладок berth: каталог, агент, активная, вид, сессия\n");
    if (extra && *extra) fputs(extra, f);
    for (int i = 0; i < list->count; i++) {
        const Session *s = &list->items[i];
        // Вкладки с погибшим процессом не восстанавливаем: человек их уже
        // закрыл бы, если бы они были нужны.
        if (s->state == SESSION_STATE_DEAD) continue;
        // Вид вкладки идёт последним полем, чтобы файл, написанный прошлой
        // версией, читался без него: отсутствие поля означает терминал.
        const char *kind = !session_has_term(s)
            ? (s->page == SESSION_PAGE_SETTINGS ? "settings" : "page")
            : "term";
        // Диалог вкладки пишем последним полем: без него вкладка после
        // перезапуска выбирает сессию заново и попадает в чужую.
        fprintf(f, "%s\t%s\t%d\t%s\t%s\n", s->cwd, s->agent->id,
                i == list->active ? 1 : 0, kind, s->session_id);
    }

    fclose(f);
    return true;
}

void session_poll_all(SessionList *list)
{
    // Бюджет на чтение — общий на кадр, а не на вкладку: иначе десять вкладок
    // множили бы его на десять. Активной достаётся львиная доля, фоновым — по
    // капле: им важно лишь не дать переполниться буферу pty, а на экран они
    // всё равно не попадают.
    const double active_budget = 0.006;
    const double idle_budget   = 0.0005;

    for (int i = 0; i < list->count; i++) {
        Session *s = &list->items[i];
        if (!session_has_term(s)) continue;   // у страницы нет процесса
        term_poll(&s->term, i == list->active ? active_budget : idle_budget);
        if (!term_alive(&s->term))
            s->state = SESSION_STATE_DEAD;
    }
}

// Подпись задачи-вкладки. Упавшая называет код выхода: «закончила» у
// задачи, которая ничего не сделала, читалось бы как успех.
const char *session_task_state_text(const Session *s)
{
    static char buf[48];
    if (!session_has_term(s)) return "готовится";
    if (s->state != SESSION_STATE_DEAD) return "работает";
    if (!s->term.child_reaped || s->term.child_status == 0) return "закончила";
    snprintf(buf, sizeof(buf), "упала, код %d", s->term.child_status);
    return buf;
}

void session_window_title(const Session *s, char *out, size_t cap)
{
    if (!session_has_term(s)) {
        snprintf(out, cap, "%s", s->page == SESSION_PAGE_SETTINGS
                                 ? "Настройки" : s->name);
        return;
    }

    const char *title = title_words(s->term.title);

    // Заголовок вида «✳ Claude Code» ничего не добавляет к имени проекта:
    // это агент представился, а не назвал работу. Такие пропускаем, а вот
    // осмысленный заголовок сессии показываем — ради него всё и затевалось.
    bool informative = title[0] != '\0'
        && (!s->agent->label || strstr(title, s->agent->label) == NULL);

    if (informative)
        snprintf(out, cap, "%s · %s", s->name, title);
    else
        snprintf(out, cap, "%s", s->name);
}

const char *session_subtitle(const Session *s)
{
    if (session_shows_page(s))
        return s->page == SESSION_PAGE_SETTINGS ? "настройки" : "страница проекта";
    if (s->state == SESSION_STATE_DEAD) return "завершено";

    const char *title = title_words(s->term.title);
    bool informative = title[0] != '\0'
        && (!s->agent->label || strstr(title, s->agent->label) == NULL);

    // У оболочки состояний нет — она всегда ждёт. Показываем, что она
    // сообщает о себе сама: каталог или запущенную программу.
    if (!s->agent->has_history)
        return informative ? title : s->agent->label;

    // Состояние — первым словом: оно и есть ответ на вопрос «что там».
    // Название работы следом, если агент его назвал. У свободного агента
    // состояния нет — «ждёт» читалось как «ждёт меня», а это не так:
    // ждёт ответа только «зовёт». Вместо слова — давность: когда там в
    // последний раз что-то происходило.
    char ago[32];
    time_t last = session_last_work(s);
    const char *state = s->state == SESSION_STATE_BUSY      ? "работает"
                      : s->state == SESSION_STATE_ATTENTION ? "зовёт"
                      : last > 0 ? ago_text(last, ago, sizeof(ago))
                      : session_empty(s) ? "пусто"
                      : "свободен";
    static char buf[160];
    if (informative)
        snprintf(buf, sizeof(buf), "%s · %s", state, title);
    else
        snprintf(buf, sizeof(buf), "%s", state);
    return buf;
}
