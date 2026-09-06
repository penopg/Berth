#include <dirent.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <time.h>

#include "projinfo.h"

// Хвост jsonl, в котором ищем заголовок сессии. Файлы диалогов доходят до
// десятков мегабайт, а заголовок дописывается по ходу работы, поэтому читаем
// конец, а не весь файл: последнее вхождение и есть актуальное имя.
#define TAIL_BYTES (512 * 1024)

static void copy_line(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// --- git ---------------------------------------------------------------------

static void read_branch(ProjInfo *info)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.git/HEAD", info->cwd);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    if (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';

        const char *ref = "ref: refs/heads/";
        if (!strncmp(line, ref, strlen(ref))) {
            snprintf(info->branch, sizeof(info->branch), "%s", line + strlen(ref));
        } else if (line[0]) {
            // Отсоединённая голова: в файле лежит сам хеш.
            info->detached = true;
            snprintf(info->branch, sizeof(info->branch), "%.7s", line);
        }
    }
    fclose(f);
}

// --- CLAUDE.md ---------------------------------------------------------------

// Заголовок раздела, с которого начинается полезное. Те же слова, что у
// brief.sh: паспорта проектов уже написаны под них.
static bool is_status_heading(const char *line)
{
    if (line[0] != '#') return false;
    while (*line == '#') line++;
    while (*line == ' ') line++;

    static const char *words[] = {
        "Статус", "Сейчас", "Текущ", "TODO", "Status", "Next",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (!strncmp(line, words[i], strlen(words[i]))) return true;
    return false;
}

static void read_claude_md(ProjInfo *info)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/CLAUDE.md", info->cwd);

    FILE *f = fopen(path, "r");
    if (!f) return;
    info->has_claude_md = true;

    char line[512];
    bool inside = false;
    while (fgets(line, sizeof(line), f) && info->status_lines < PROJINFO_STATUS_MAX) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';

        if (!inside) {
            inside = is_status_heading(line);
            continue;
        }
        if (line[0] == '#') break;               // следующий раздел — хватит
        if (line[0] == '\0' && info->status_lines == 0) continue;

        copy_line(info->status[info->status_lines], PROJINFO_LINE_MAX,
                  line, strlen(line));
        info->status_lines++;
    }

    fclose(f);
}

// --- сводка ------------------------------------------------------------------

static void read_summary(ProjInfo *info)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", info->cwd, PROJINFO_SUMMARY_FILE);

    struct stat st;
    info->has_summary = false;
    info->summary[0] = '\0';
    info->summary_mtime = stat(path, &st) == 0 ? st.st_mtime : 0;

    FILE *f = fopen(path, "r");
    if (!f) return;

    size_t got = fread(info->summary, 1, sizeof(info->summary) - 1, f);
    fclose(f);
    info->summary[got] = '\0';

    // Хвост, обрезанный посреди UTF-8, портит отрисовку — отступаем к границе.
    while (got > 0 && (info->summary[got - 1] & 0xC0) == 0x80) info->summary[--got] = '\0';
    while (got > 0 && (info->summary[got - 1] == '\n' || info->summary[got - 1] == ' '))
        info->summary[--got] = '\0';

    // Заголовок «# …» в начале — для чтения файла глазами; на странице
    // сводку и так подписывают.
    char *p = info->summary;
    while (*p == '#') {
        char *nl = strchr(p, '\n');
        if (!nl) { *p = '\0'; break; }
        memmove(p, nl + 1, strlen(nl + 1) + 1);
        while (*p == '\n') memmove(p, p + 1, strlen(p + 1) + 1);
    }
    info->has_summary = info->summary[0] != '\0';
}

bool projinfo_refresh_summary(ProjInfo *info)
{
    if (!info->cwd[0]) return false;
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", info->cwd, PROJINFO_SUMMARY_FILE);
    struct stat st;
    time_t now = stat(path, &st) == 0 ? st.st_mtime : 0;
    if (now == info->summary_mtime) return false;
    read_summary(info);
    return true;
}

// --- сессии Claude Code ------------------------------------------------------

// Каталог истории у Claude Code называется по пути проекта, где всё, что
// похоже на разделитель, заменено дефисом.
void projinfo_session_dir(const char *cwd, char *out, size_t cap)
{
    const char *home = getenv("HOME");
    if (!home) { out[0] = '\0'; return; }

    // Правило Claude Code (из бинаря): `path.replace(/[^a-zA-Z0-9]/g, "-")`,
    // по дефису на каждый символ — «polly/ИПР» это «polly----», а не
    // «polly-ИПР». Регулярное выражение работает по кодовым единицам
    // UTF-16, поэтому символ вне BMP (четыре байта UTF-8) даёт два дефиса.
    char slug[512];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)cwd; *p && n < sizeof(slug) - 2; ) {
        bool alnum = (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z')
                  || (*p >= 'a' && *p <= 'z');
        if (alnum) { slug[n++] = (char)*p++; continue; }
        int len = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
        slug[n++] = '-';
        if (len == 4) slug[n++] = '-';
        for (int i = 0; i < len && *p; i++) p++;
    }
    slug[n] = '\0';

    snprintf(out, cap, "%s/.claude/projects/%s", home, slug);
}

// Значение строкового поля JSON без разбора всего документа: находим последнее
// вхождение «"ключ":"» и раскодируем строку до закрывающей кавычки.
static bool last_json_string(const char *hay, size_t len, const char *key,
                             char *out, size_t cap)
{
    // Ключ ищем отдельно от значения: jsonl Claude Code пишет плотно
    // («"aiTitle":"…»), а state.json фоновых заданий — с отступами
    // («"state": "working"»). Один шаблон на оба случая не годится.
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t plen = strlen(pattern);
    if (len < plen) return false;

    const char *value = NULL;
    for (const char *p = hay; (p = memchr(p, '"', (size_t)(hay + len - p))); p++) {
        if ((size_t)(hay + len - p) < plen) break;
        if (memcmp(p, pattern, plen)) continue;

        const char *q = p + plen;
        while (q < hay + len && (*q == ' ' || *q == '\t')) q++;
        if (q >= hay + len || *q != ':') continue;
        q++;
        while (q < hay + len && (*q == ' ' || *q == '\t')) q++;
        if (q >= hay + len || *q != '"') continue;   // не строка — пропускаем
        value = q + 1;
    }
    if (!value) return false;

    const char *p = value;
    size_t w = 0;
    while (*p && *p != '"' && w + 4 < cap) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': case 't': case 'r': out[w++] = ' '; p++; break;
            case 'u': {
                unsigned code = 0;
                if (sscanf(p + 1, "%4x", &code) != 1) return false;
                p += 5;
                // Суррогатные пары складываем обратно: без этого эмодзи в
                // заголовке превратились бы в два битых символа.
                if (code >= 0xD800 && code <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    unsigned low = 0;
                    if (sscanf(p + 2, "%4x", &low) == 1 && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        p += 6;
                    }
                }
                if (code < 0x80) {
                    out[w++] = (char)code;
                } else if (code < 0x800) {
                    out[w++] = (char)(0xC0 | (code >> 6));
                    out[w++] = (char)(0x80 | (code & 0x3F));
                } else if (code < 0x10000) {
                    out[w++] = (char)(0xE0 | (code >> 12));
                    out[w++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    out[w++] = (char)(0x80 | (code & 0x3F));
                } else {
                    out[w++] = (char)(0xF0 | (code >> 18));
                    out[w++] = (char)(0x80 | ((code >> 12) & 0x3F));
                    out[w++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    out[w++] = (char)(0x80 | (code & 0x3F));
                }
                break;
            }
            default: out[w++] = *p ? *p++ : '\0'; break;
            }
        } else {
            out[w++] = *p++;
        }
    }
    out[w] = '\0';
    return w > 0;
}

// Последняя строка буфера, в которой встречается образец. jsonl — построчный
// формат, и служебные записи (заголовок, последний вопрос) обновляются
// дописыванием: актуальна последняя.
//
// Искать образец по всему буферу нельзя: тела сообщений — тоже текст, и в
// диалоге про сам Claude Code слово «aiTitle» встретится в чужой строке.
static const char *last_line_with(const char *buf, size_t len, const char *needle,
                                  size_t *out_len)
{
    size_t nlen = strlen(needle);
    const char *best = NULL;
    size_t best_len = 0;

    const char *line = buf;
    while (line < buf + len) {
        const char *nl = memchr(line, '\n', (size_t)(buf + len - line));
        size_t line_len = nl ? (size_t)(nl - line) : (size_t)(buf + len - line);

        if (line_len >= nlen) {
            for (size_t i = 0; i + nlen <= line_len; i++) {
                if (memcmp(line + i, needle, nlen)) continue;
                best = line;
                best_len = line_len;
                break;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    if (best) *out_len = best_len;
    return best;
}

// Читает кусок файла: from_end == true — хвост, иначе начало. Возвращает
// выделенный буфер, освобождать вызывающему.
static char *read_chunk(const char *path, long want, bool from_end, size_t *got)
{
    *got = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long take = size < want ? size : want;
    fseek(f, from_end ? size - take : 0, SEEK_SET);

    char *buf = malloc((size_t)take + 1);
    if (buf) {
        *got = fread(buf, 1, (size_t)take, f);
        buf[*got] = '\0';
    }
    fclose(f);
    return buf;
}

static void read_session_title(const char *path, char *out, size_t cap)
{
    out[0] = '\0';

    // Имя, которое Claude Code дал сессии сам. Дописывается по ходу работы,
    // поэтому ищем в хвосте.
    size_t got = 0;
    char *tail = read_chunk(path, TAIL_BYTES, true, &got);
    if (tail) {
        size_t line_len = 0;
        const char *line = last_line_with(tail, got, "\"type\":\"ai-title\"", &line_len);
        if (line) last_json_string(line, line_len, "aiTitle", out, cap);

        if (!out[0]) {
            line = last_line_with(tail, got, "\"type\":\"last-prompt\"", &line_len);
            if (line) last_json_string(line, line_len, "lastPrompt", out, cap);
        }
        free(tail);
    }

    // Имени нет вовсе — сессия оборвалась раньше, чем Claude Code её назвал.
    // Тогда берём первое, что человек сказал: без этого сессия выглядит в
    // списке пустой строкой, и до неё просто не доберутся.
    if (!out[0]) {
        char *head = read_chunk(path, 64 * 1024, false, &got);
        if (head) {
            const char *line = head;
            while (line < head + got) {
                const char *nl = memchr(line, '\n', (size_t)(head + got - line));
                size_t line_len = nl ? (size_t)(nl - line)
                                     : (size_t)(head + got - line);

                if (line_len >= 13 && memmem(line, line_len, "\"type\":\"user\"", 13)) {
                    last_json_string(line, line_len, "content", out, cap);
                    // Служебные врезки (`<local-command-caveat>`, напоминания
                    // системы) — не то, о чём человек просил. Ищем дальше.
                    if (out[0] && out[0] != '<') break;
                    out[0] = '\0';
                }
                if (!nl) break;
                line = nl + 1;
            }
            free(head);
        }
    }

    if (!out[0]) snprintf(out, cap, "(без названия)");
}

// Кто из сессий занят прямо сейчас.
//
// Claude Code ведёт реестр живых процессов: ~/.claude/sessions/<pid>.json с
// полями sessionId, cwd и kind («interactive» — вкладка человека, «bg» —
// фоновый агент). Файл остаётся и после смерти процесса, поэтому живость
// проверяем сигналом 0, а не наличием файла.
//
// Раньше мы смотрели в ~/.claude/jobs — но там только фоновые задания.
// Обычные вкладки в реестр заданий не попадают, и берт их не видел: десять
// вкладок делали `claude --continue`, все получали одну и ту же свежую сессию
// и писали в один jsonl.
// Обход живых процессов Claude Code. Всё, что нужно знать про занятость,
// лежит здесь; кто как этим распорядится — дело вызывающего.
typedef void (*LiveSessionFn)(void *ctx, pid_t pid, const char *sid,
                              const char *cwd, const char *kind,
                              const char *status, time_t status_at);

// Последнее число за ключом, либо 0. Для `statusUpdatedAt`: миллисекунды
// эпохи, здесь же переводятся в секунды.
static long long last_json_number(const char *hay, size_t len, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t plen = strlen(pattern);
    long long value = 0;
    for (const char *p = hay; (p = memchr(p, '"', (size_t)(hay + len - p))); p++) {
        if ((size_t)(hay + len - p) < plen) break;
        if (memcmp(p, pattern, plen)) continue;
        const char *q = p + plen;
        while (q < hay + len && (*q == ' ' || *q == ':')) q++;
        if (q < hay + len && *q >= '0' && *q <= '9') value = strtoll(q, NULL, 10);
    }
    return value;
}

static void for_each_live_session(LiveSessionFn fn, void *ctx)
{
    const char *home = getenv("HOME");
    if (!home) return;

    char dir[600];
    snprintf(dir, sizeof(dir), "%s/.claude/sessions", home);
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".json")) continue;

        // Имя файла — pid процесса. Мёртвый процесс своего файла не убирает.
        long pid = strtol(e->d_name, NULL, 10);
        if (pid <= 0 || kill((pid_t)pid, 0) != 0) continue;

        char path[900];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        char buf[4096];
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = '\0';
        fclose(f);

        char sid[PROJINFO_ID_MAX] = "", cwd[512] = "", kind[32] = "", status[32] = "";
        last_json_string(buf, got, "sessionId", sid, sizeof(sid));
        last_json_string(buf, got, "cwd", cwd, sizeof(cwd));
        last_json_string(buf, got, "kind", kind, sizeof(kind));
        last_json_string(buf, got, "status", status, sizeof(status));
        if (!sid[0]) continue;
        time_t status_at = (time_t)(last_json_number(buf, got, "statusUpdatedAt") / 1000);

        fn(ctx, (pid_t)pid, sid, cwd, kind, status, status_at);
    }
    closedir(d);
}

// Помечает занятые сессии и дописывает те, которых нет на диске.
//
// Занятость читается из реестра живых процессов Claude Code
// (`~/.claude/sessions/<pid>.json`: sessionId, cwd, kind). Раньше мы смотрели
// в ~/.claude/jobs — но там только фоновые задания. Обычные вкладки в реестр
// заданий не попадают, и берт их не видел: десять вкладок делали
// `claude --continue`, все получали одну и ту же свежую сессию и писали в
// один jsonl.
static void mark_live(void *ctx, pid_t pid, const char *sid,
                      const char *cwd, const char *kind, const char *status,
                      time_t status_at)
{
    (void)pid; (void)status; (void)status_at;
    ProjInfo *info = ctx;
    if (strcmp(cwd, info->cwd)) return;

    ProjSessionUse use = strcmp(kind, "bg") ? PROJ_SESSION_OPEN
                                            : PROJ_SESSION_AGENT;

    for (int i = 0; i < info->session_count; i++) {
        if (strcmp(info->sessions[i].id, sid)) continue;
        if (info->sessions[i].use == PROJ_SESSION_FREE) info->session_busy++;
        info->sessions[i].use = use;
        return;
    }

    // Сессии нет на диске: её только что начали и ещё ни о чём не спросили.
    // В список такая не идёт — продолжать в ней нечего, а девять пустых
    // вкладок вытеснили бы собой все настоящие диалоги. Но и молчать о них
    // нельзя: человек видит вкладки в панели и ищет их в списке.
    info->session_empty++;
}

static void mark_live_sessions(ProjInfo *info)
{
    for_each_live_session(mark_live, info);
}

// --- поиск сессии живого потомка ---------------------------------------------

static pid_t parent_of(pid_t pid)
{
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len == 0) return 0;
    return kp.kp_eproc.e_ppid;
}

typedef struct {
    pid_t root;
    char *out;
    size_t cap;
    ProjLiveStatus status;
    time_t since;
} DescendantCtx;

static void match_descendant(void *ctx, pid_t pid, const char *sid,
                             const char *cwd, const char *kind,
                             const char *status, time_t status_at)
{
    (void)cwd;
    DescendantCtx *d = ctx;
    if (d->status != PROJ_LIVE_NONE) return; // уже нашли
    if (!strcmp(kind, "bg")) return;         // фоновый агент — не вкладка

    // Вкладка запускает оболочку, а `claude` живёт уже под ней, поэтому
    // сравнивать pid напрямую нельзя: поднимаемся по родителям.
    for (int step = 0; step < 8 && pid > 1; step++) {
        if (pid == d->root) {
            snprintf(d->out, d->cap, "%s", sid);
            d->status = !strcmp(status, "busy")    ? PROJ_LIVE_BUSY
                      : !strcmp(status, "waiting") ? PROJ_LIVE_WAITING
                                                   : PROJ_LIVE_IDLE;
            d->since = status_at;
            return;
        }
        pid = parent_of(pid);
    }
}

ProjLiveStatus projinfo_live_of_pid(pid_t root, char *out, size_t cap, time_t *since)
{
    if (cap) out[0] = '\0';
    if (since) *since = 0;
    if (root <= 1 || !cap) return PROJ_LIVE_NONE;
    DescendantCtx ctx = { root, out, cap, PROJ_LIVE_NONE, 0 };
    for_each_live_session(match_descendant, &ctx);
    if (since) *since = ctx.since;
    return ctx.status;
}

void projinfo_session_of_pid(pid_t root, char *out, size_t cap)
{
    projinfo_live_of_pid(root, out, cap, NULL);
}

// Сессии, которые берт уже раздал вкладкам в этом запуске.
//
// Между командой `claude --resume <id>` и появлением процесса в реестре
// проходят секунды, а раскладка восстанавливается за один кадр — без своей
// отметки все вкладки проекта выбрали бы одну и ту же сессию. Отметка живёт
// полторы минуты: за это время процесс либо появился в реестре, либо не
// запустился вовсе, и сессия честно снова свободна.
#define CLAIM_SECONDS 90
#define CLAIM_MAX     64

static struct { char id[PROJINFO_ID_MAX]; time_t when; } g_claims[CLAIM_MAX];

static bool claim_held(const char *id, time_t now)
{
    for (int i = 0; i < CLAIM_MAX; i++)
        if (g_claims[i].id[0] && now - g_claims[i].when < CLAIM_SECONDS
            && !strcmp(g_claims[i].id, id))
            return true;
    return false;
}

static void claim_take(const char *id, time_t now)
{
    int slot = 0;
    time_t oldest = now;
    for (int i = 0; i < CLAIM_MAX; i++) {
        if (!g_claims[i].id[0] || now - g_claims[i].when >= CLAIM_SECONDS) {
            slot = i;
            break;
        }
        if (g_claims[i].when <= oldest) { oldest = g_claims[i].when; slot = i; }
    }
    snprintf(g_claims[slot].id, sizeof(g_claims[slot].id), "%s", id);
    g_claims[slot].when = now;
}

static int by_mtime_desc(const void *a, const void *b)
{
    const ProjSession *x = a, *y = b;
    if (x->mtime == y->mtime) return 0;
    return x->mtime < y->mtime ? 1 : -1;
}

// Только имена и время — без чтения хвостов файлов. Нужно там, где важна лишь
// занятость сессии: полмегабайта на файл ради заголовка тут ни к чему.
static void read_sessions_ids_only(ProjInfo *info);

static void read_sessions(ProjInfo *info)
{
    char dir[600];
    projinfo_session_dir(info->cwd, dir, sizeof(dir));
    if (!dir[0]) return;

    DIR *d = opendir(dir);
    if (!d) return;

    // Отбираем самые свежие: файлов в каталоге бывают сотни, а показать
    // осмысленно можно единицы.
    ProjSession found[64];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".jsonl")) continue;
        info->session_total++;
        if (n >= (int)(sizeof(found) / sizeof(found[0]))) continue;

        char path[900];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        memset(&found[n], 0, sizeof(found[n]));
        found[n].mtime = st.st_mtime;
        copy_line(found[n].id, PROJINFO_ID_MAX, e->d_name, (size_t)(dot - e->d_name));
        n++;
    }
    closedir(d);

    qsort(found, (size_t)n, sizeof(found[0]), by_mtime_desc);

    // Заголовки читаем только у тех, что покажем: каждое чтение — это полмега
    // с диска, и делать это для сотни файлов незачем.
    int show = n < PROJINFO_SESSION_MAX ? n : PROJINFO_SESSION_MAX;
    for (int i = 0; i < show; i++) {
        char path[900];
        snprintf(path, sizeof(path), "%s/%s.jsonl", dir, found[i].id);
        read_session_title(path, found[i].title, PROJINFO_LINE_MAX);
        info->sessions[i] = found[i];
    }
    info->session_count = show;
}

// --- сборка ------------------------------------------------------------------

void projinfo_load(ProjInfo *info, const char *cwd)
{
    memset(info, 0, sizeof(*info));
    snprintf(info->cwd, sizeof(info->cwd), "%s", cwd ? cwd : "");
    if (!info->cwd[0]) return;

    read_branch(info);
    read_claude_md(info);
    read_summary(info);
    read_sessions(info);
    mark_live_sessions(info);
}

void projinfo_pick_session(const char *cwd, char *out, size_t cap)
{
    if (cap) out[0] = '\0';

    ProjInfo info;
    memset(&info, 0, sizeof(info));
    snprintf(info.cwd, sizeof(info.cwd), "%s", cwd ? cwd : "");

    // Заголовки сессий здесь не нужны — только их свежесть и занятость.
    read_sessions_ids_only(&info);
    mark_live_sessions(&info);

    // Самая свежая из тех, что никем не занята и не отдана соседней вкладке.
    time_t now = time(NULL);
    for (int i = 0; i < info.session_count; i++) {
        if (info.sessions[i].use != PROJ_SESSION_FREE) continue;
        if (claim_held(info.sessions[i].id, now)) continue;

        claim_take(info.sessions[i].id, now);
        snprintf(out, cap, "%s", info.sessions[i].id);
        return;
    }
}

void projinfo_resume_command(const char *cwd, char *out, size_t cap)
{
    char id[PROJINFO_ID_MAX];
    projinfo_pick_session(cwd, id, sizeof(id));

    // Свободных нет: все диалоги проекта уже открыты. Начинаем новый — это
    // лучше, чем отказ Claude Code и пустая оболочка вместо работы.
    if (id[0]) snprintf(out, cap, "claude --resume %s", id);
    else       snprintf(out, cap, "claude");
}

bool projinfo_session_exists(const char *cwd, const char *id)
{
    if (!id || !*id) return false;

    char dir[600];
    projinfo_session_dir(cwd, dir, sizeof(dir));
    if (!dir[0]) return false;

    char path[900];
    snprintf(path, sizeof(path), "%s/%s.jsonl", dir, id);
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

typedef struct {
    const char *cwd;
    const char *id;
    bool busy;
} BusyCtx;

static void match_busy(void *ctx, pid_t pid, const char *sid,
                       const char *cwd, const char *kind, const char *status,
                       time_t status_at)
{
    (void)status; (void)status_at;
    (void)pid; (void)kind;
    BusyCtx *b = ctx;
    if (!strcmp(sid, b->id) && !strcmp(cwd, b->cwd)) b->busy = true;
}

bool projinfo_session_busy(const char *cwd, const char *id)
{
    if (!id || !*id) return false;
    BusyCtx ctx = { cwd ? cwd : "", id, false };
    for_each_live_session(match_busy, &ctx);
    return ctx.busy;
}

static void read_sessions_ids_only(ProjInfo *info)
{
    char dir[600];
    projinfo_session_dir(info->cwd, dir, sizeof(dir));
    if (!dir[0]) return;

    DIR *d = opendir(dir);
    if (!d) return;

    ProjSession found[64];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".jsonl")) continue;
        info->session_total++;
        if (n >= (int)(sizeof(found) / sizeof(found[0]))) continue;

        char path[900];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        memset(&found[n], 0, sizeof(found[n]));
        found[n].mtime = st.st_mtime;
        copy_line(found[n].id, PROJINFO_ID_MAX, e->d_name, (size_t)(dot - e->d_name));
        n++;
    }
    closedir(d);

    qsort(found, (size_t)n, sizeof(found[0]), by_mtime_desc);
    int show = n < PROJINFO_SESSION_MAX ? n : PROJINFO_SESSION_MAX;
    for (int i = 0; i < show; i++)
        info->sessions[i] = found[i];
    info->session_count = show;
}

void projinfo_age(time_t when, char *out, size_t cap)
{
    if (when <= 0) { snprintf(out, cap, "?"); return; }

    time_t now = time(NULL);
    long days = (long)((now - when) / 86400);
    if (days <= 0)      snprintf(out, cap, "сегодня");
    else if (days == 1) snprintf(out, cap, "вчера");
    else                snprintf(out, cap, "%ld дн. назад", days);
}
