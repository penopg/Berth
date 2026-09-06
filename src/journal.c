#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "journal.h"

// Кэш разбора. История проекта доходит до десятков мегабайт, а событий в ней
// сотни: перечитывать всё при каждом открытии страницы незачем. jsonl только
// дописывается, поэтому храним, до какого места файл уже разобран.
#define CACHE_VERSION "# berth journal 2"

static const char *cache_dir(void)
{
    static char dir[600];
    if (dir[0]) return dir;

    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(dir, sizeof(dir), "%s/.config/berth/journal", home);
    mkdir(dir, 0755);   // если уже есть — просто EEXIST
    return dir;
}

// Имя кэша — тот же слаг, что у каталога истории: путь проекта с дефисами.
static void cache_path(const char *cwd, char *out, size_t cap)
{
    const char *dir = cache_dir();
    if (!dir) { out[0] = '\0'; return; }

    char slug[512];
    size_t n = 0;
    for (const char *p = cwd; *p && n < sizeof(slug) - 1; p++)
        slug[n++] = (*p == '/' || *p == '.' || *p == ' ') ? '-' : *p;
    slug[n] = '\0';

    snprintf(out, cap, "%s/%s.tsv", dir, slug);
}

// --- разбор jsonl ------------------------------------------------------------

// Строковое значение поля JSON внутри одной строки. Разэкранирует то, что
// встречается в тексте реплик: переводы строк, кавычки и \uXXXX.
//
// Искать надо именно в нужной строке и именно первое вхождение: в записи
// человека `"content":"` встречается один раз — в message, — а всё, что
// человек написал сам, лежит уже за ним и экранировано.
static bool json_field(const char *line, size_t len, const char *key,
                       char *out, size_t cap)
{
    char pat[64];
    int pat_len = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *at = memmem(line, len, pat, (size_t)pat_len);
    if (!at) return false;

    const char *p   = at + pat_len;
    const char *end = line + len;
    size_t n = 0;

    while (p < end && *p != '"' && n < cap - 1) {
        if (*p != '\\') { out[n++] = *p++; continue; }
        if (++p >= end) break;
        switch (*p) {
        case 'n': out[n++] = '\n'; p++; break;
        case 't': out[n++] = ' ';  p++; break;
        case 'r': p++; break;
        case 'u': {
            // \uXXXX → UTF-8. Суррогатные пары нам не встречаются: эмодзи в
            // jsonl приходят готовыми байтами.
            if (end - p < 5) { p = end; break; }
            unsigned code = 0;
            for (int i = 1; i <= 4; i++) {
                char c = p[i];
                code <<= 4;
                if      (c >= '0' && c <= '9') code |= (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') code |= (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') code |= (unsigned)(c - 'A' + 10);
            }
            p += 5;
            if (code < 0x80 && n < cap - 1) {
                out[n++] = (char)code;
            } else if (code < 0x800 && n < cap - 2) {
                out[n++] = (char)(0xC0 | (code >> 6));
                out[n++] = (char)(0x80 | (code & 0x3F));
            } else if (n < cap - 3) {
                out[n++] = (char)(0xE0 | (code >> 12));
                out[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (code & 0x3F));
            }
            break;
        }
        default: out[n++] = *p++; break;
        }
    }
    out[n] = '\0';
    return true;
}

// «2026-09-01T17:03:11.518Z» → время. Метка в UTC, поэтому timegm, а не
// mktime: иначе лента уезжает на часовой пояс.
static time_t parse_time(const char *iso)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    int y, mo, d, h, mi, s;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
        return 0;
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = s;
    return timegm(&tm);
}

// Обрезка по границе UTF-8: половина буквы на экране выглядит мусором.
static void clip_utf8(char *s, size_t limit)
{
    if (strlen(s) <= limit) return;
    size_t n = limit;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s[n] = '\0';
    strncat(s, "…", 4);
}

// Схлопывает переводы строк и лишние пробелы: в ленте у события одна строка.
static void one_line(char *s)
{
    char *w = s;
    bool space = false;
    for (char *r = s; *r; r++) {
        char c = (*r == '\n' || *r == '\t') ? ' ' : *r;
        if (c == ' ') {
            if (space || w == s) continue;
            space = true;
        } else {
            space = false;
        }
        *w++ = c;
    }
    while (w > s && w[-1] == ' ') w--;
    *w = '\0';
}

static void add_event(Journal *j, time_t when, JournalKind kind,
                      const char *sid, const char *text)
{
    if (j->count >= JOURNAL_EVENT_MAX) {
        // Держим хвост: свежее важнее давнего, а места ровно столько.
        memmove(&j->events[0], &j->events[1],
                sizeof(j->events[0]) * (JOURNAL_EVENT_MAX - 1));
        j->count = JOURNAL_EVENT_MAX - 1;
        j->partial = true;
    }
    JournalEvent *e = &j->events[j->count++];
    e->when = when;
    e->kind = kind;
    snprintf(e->session, sizeof(e->session), "%s", sid);
    snprintf(e->text, sizeof(e->text), "%s", text ? text : "");
    one_line(e->text);
    clip_utf8(e->text, JOURNAL_TEXT_MAX - 8);
}

// Разбирает строку jsonl. Событием становится только то, что относится к ходу
// работы: сказанное человеком, сжатие контекста и переход нити в другой файл.
// Ответы агента и результаты инструментов пропускаем — их в ленте столько,
// что за ними не видно работы.
static void parse_line(Journal *j, const char *line, size_t len, const char *sid)
{
    if (len < 20) return;

    if (memmem(line, len, "\"type\":\"continued-in\"", 21)) {
        char ts[40], to[PROJINFO_ID_MAX];
        if (!json_field(line, len, "timestamp", ts, sizeof(ts))) return;
        json_field(line, len, "continuedInSessionId", to, sizeof(to));
        add_event(j, parse_time(ts), JOURNAL_MOVED, sid, to);
        return;
    }

    if (!memmem(line, len, "\"type\":\"user\"", 13)) return;

    // Служебные врезки Claude Code помечает сам: пути вставленных файлов,
    // напоминания, подсказки продолжения. Работой это не является.
    if (memmem(line, len, "\"isMeta\":true", 13)) return;

    char ts[40];
    if (!json_field(line, len, "timestamp", ts, sizeof(ts))) return;

    char text[JOURNAL_TEXT_MAX * 4];
    if (memmem(line, len, "\"content\":[", 11)) {
        // Содержимое массивом бывает двух родов: результат инструмента (нам
        // не нужен) и реплика с картинкой — текст в ней лежит первым куском,
        // а дальше идут мегабайты base64.
        if (memmem(line, len, "\"tool_result\"", 13)) return;
        if (!json_field(line, len, "text", text, sizeof(text))) return;
    } else if (!json_field(line, len, "content", text, sizeof(text))) {
        return;
    }
    if (!text[0]) return;

    // Врезка компакта — это не реплика, а шов: дальше работа шла с выжимкой
    // вместо полной истории.
    if (strstr(text, "being continued from a previous conversation")) {
        add_event(j, parse_time(ts), JOURNAL_COMPACT, sid, "");
        return;
    }

    // Служебное в угловых скобках и механика продолжения — не то, о чём
    // человек просил.
    if (text[0] == '<') return;
    if (!strncmp(text, "[Image:", 7)) return;
    if (!strncmp(text, "Continue from where you left off", 32)) return;
    // Текст скилла, подставленный Claude Code вместо команды: так выглядит
    // история служебного запуска (сбор журнала до появления
    // --no-session-persistence). Работой человека это не является.
    if (!strncmp(text, "Base directory for this skill", 29)) return;

    // Задача из списка уходит агенту с наказом отметить её в файле. Наказ —
    // механика берта, а не сказанное человеком; в ленте ему не место.
    char *tail = strstr(text, "\n---\nЭто задача из списка");
    if (tail) *tail = '\0';

    // Команда со словами после неё — это тоже сказанное: «/compact <о чём>».
    // Голая команда работой не является.
    char *body = text;
    if (body[0] == '/') {
        char *space = strchr(body, ' ');
        if (!space) return;
        body = space + 1;
        while (*body == ' ') body++;
        if (!*body) return;
    }

    bool typed = memmem(line, len, "\"promptSource\":\"typed\"", 22) != NULL;
    add_event(j, parse_time(ts), (typed || body != text) ? JOURNAL_SAID : JOURNAL_PASTED,
              sid, body);
}

// --- кэш ---------------------------------------------------------------------

typedef struct {
    char id[PROJINFO_ID_MAX];
    long parsed;    // сколько байт файла уже разобрано
} FileMark;

#define FILE_MARK_MAX 64

typedef struct {
    FileMark marks[FILE_MARK_MAX];
    int      count;
} Marks;

static long mark_get(const Marks *m, const char *id)
{
    for (int i = 0; i < m->count; i++)
        if (!strcmp(m->marks[i].id, id)) return m->marks[i].parsed;
    return 0;
}

static void mark_set(Marks *m, const char *id, long parsed)
{
    for (int i = 0; i < m->count; i++)
        if (!strcmp(m->marks[i].id, id)) { m->marks[i].parsed = parsed; return; }
    if (m->count >= FILE_MARK_MAX) return;
    snprintf(m->marks[m->count].id, PROJINFO_ID_MAX, "%s", id);
    m->marks[m->count].parsed = parsed;
    m->count++;
}

static void cache_read(Journal *j, Marks *m, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[JOURNAL_TEXT_MAX * 2 + 200];
    if (!fgets(line, sizeof(line), f) || strncmp(line, CACHE_VERSION, strlen(CACHE_VERSION))) {
        // Формат сменился — разберём историю заново, это лишь время.
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab++ = '\0';

        if (!strcmp(line, "@")) {
            char *sep = strchr(tab, '\t');
            if (!sep) continue;
            *sep++ = '\0';
            mark_set(m, tab, atol(sep));
            continue;
        }
        if (strcmp(line, "E")) continue;

        // E \t время \t вид \t сессия \t текст
        char *f2 = strchr(tab, '\t'); if (!f2) continue; *f2++ = '\0';
        char *f3 = strchr(f2,  '\t'); if (!f3) continue; *f3++ = '\0';
        char *f4 = strchr(f3,  '\t'); if (!f4) continue; *f4++ = '\0';

        if (j->count >= JOURNAL_EVENT_MAX) { j->partial = true; continue; }
        JournalEvent *e = &j->events[j->count++];
        e->when = (time_t)atol(tab);
        e->kind = (JournalKind)atoi(f2);
        snprintf(e->session, sizeof(e->session), "%s", f3);
        snprintf(e->text, sizeof(e->text), "%s", f4);
    }
    fclose(f);
}

static void cache_write(const Journal *j, const Marks *m, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "%s\n", CACHE_VERSION);
    for (int i = 0; i < m->count; i++)
        fprintf(f, "@\t%s\t%ld\n", m->marks[i].id, m->marks[i].parsed);
    for (int i = 0; i < j->count; i++)
        fprintf(f, "E\t%ld\t%d\t%s\t%s\n", (long)j->events[i].when,
                (int)j->events[i].kind, j->events[i].session, j->events[i].text);
    fclose(f);
}


// --- дневник проекта ---------------------------------------------------------

// Строка записи выглядит так:
//   ## 2026-09-01 21:41–22:25 · 19a9ee9e · Разобрали тормоза кадра
// Дальше идёт абзац, который в ленту не помещается: на экране остаётся
// заголовок, а подробности человек читает в самом файле.
static bool parse_recap_head(const char *line, JournalEvent *e)
{
    int y, mo, d, h1, m1, h2 = -1, m2 = -1;
    if (sscanf(line, "## %d-%d-%d %d:%d", &y, &mo, &d, &h1, &m1) != 5)
        return false;

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h1;
    tm.tm_min  = m1;
    tm.tm_isdst = -1;
    // Дневник пишет человек и читает человек, поэтому время в нём местное —
    // в отличие от меток в jsonl, которые идут в UTC.
    e->when = timelocal(&tm);

    // Конец куска отделён длинным тире; без него запись — точка, а не отрезок.
    const char *dash = strstr(line, "–");
    if (dash && sscanf(dash + 3, "%d:%d", &h2, &m2) == 2) {
        tm.tm_hour = h2;
        tm.tm_min  = m2;
        tm.tm_isdst = -1;
        e->until = timelocal(&tm);
        if (e->until < e->when) e->until = e->when;   // кусок перешёл за полночь
    }

    // Дальше через разделители идут сессия и заголовок. Сессия нужна, чтобы
    // клик по записи продолжил тот самый разговор.
    const char *p = strstr(line, " · ");
    if (!p) return false;
    p += 4;
    const char *p2 = strstr(p, " · ");
    if (p2) {
        size_t n = (size_t)(p2 - p);
        if (n >= sizeof(e->session)) n = sizeof(e->session) - 1;
        memcpy(e->session, p, n);
        e->session[n] = '\0';
        p = p2 + 4;
    }
    snprintf(e->text, sizeof(e->text), "%s", p);
    e->kind = JOURNAL_RECAP;
    return true;
}

static void read_recaps(Journal *j, const char *cwd)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", cwd, JOURNAL_FILE);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "## ", 3)) continue;
        line[strcspn(line, "\r\n")] = '\0';

        JournalEvent e;
        memset(&e, 0, sizeof(e));
        if (!parse_recap_head(line, &e)) continue;
        one_line(e.text);
        clip_utf8(e.text, JOURNAL_TEXT_MAX - 8);

        // Первый абзац после шапки — расшифровка. Всё, что идёт дальше,
        // человек читает в самом файле: на странице для этого нет места.
        long back = ftell(f);
        size_t used = 0;
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "## ", 3)) { fseek(f, back, SEEK_SET); break; }
            line[strcspn(line, "\r\n")] = '\0';
            if (!line[0]) {
                if (used) break;      // абзац кончился
                back = ftell(f);
                continue;             // пустая строка перед абзацем
            }
            int wrote = snprintf(e.detail + used, sizeof(e.detail) - used,
                                 "%s%s", used ? " " : "", line);
            if (wrote > 0) used += (size_t)wrote;
            if (used >= sizeof(e.detail) - 1) break;
            back = ftell(f);
        }
        one_line(e.detail);
        clip_utf8(e.detail, JOURNAL_DETAIL_MAX - 8);

        if (j->count >= JOURNAL_EVENT_MAX) {
            memmove(&j->events[0], &j->events[1],
                    sizeof(j->events[0]) * (JOURNAL_EVENT_MAX - 1));
            j->count = JOURNAL_EVENT_MAX - 1;
            j->partial = true;
        }
        j->events[j->count++] = e;
    }
    fclose(f);
}

// Реплика, попавшая внутрь готовой записи дневника, из ленты убирается: рекап
// рассказывает про этот кусок работы лучше и целиком. Швы компакта остаются —
// это факт, а не пересказ.
static void drop_covered(Journal *j)
{
    int w = 0;
    for (int i = 0; i < j->count; i++) {
        const JournalEvent *e = &j->events[i];
        bool covered = false;
        if (e->kind == JOURNAL_SAID || e->kind == JOURNAL_PASTED) {
            for (int k = 0; k < j->count; k++) {
                const JournalEvent *r = &j->events[k];
                if (r->kind != JOURNAL_RECAP || r->until == 0) continue;
                if (e->when >= r->when && e->when <= r->until) { covered = true; break; }
            }
        }
        if (covered) continue;
        if (w != i) j->events[w] = j->events[i];
        w++;
    }
    j->count = w;
}

// --- сборка ------------------------------------------------------------------

static int by_time(const void *a, const void *b)
{
    const JournalEvent *x = a, *y = b;
    if (x->when != y->when) return x->when < y->when ? -1 : 1;
    // При равном времени первым идёт сказанное: форк копирует историю, и
    // порядок внутри секунды иначе зависит от того, какой файл читали раньше.
    return (int)x->kind - (int)y->kind;
}

// Форк и компакт копируют прошлое в новый файл, поэтому одно и то же событие
// встречается в нескольких: одинаковое время и начало текста — один и тот же
// момент работы.
static void dedup(Journal *j)
{
    int w = 0;
    for (int i = 0; i < j->count; i++) {
        bool dup = false;
        for (int k = 0; k < w; k++) {
            if (j->events[k].when != j->events[i].when) continue;
            if (j->events[k].kind != j->events[i].kind) continue;
            if (strncmp(j->events[k].text, j->events[i].text, 48)) continue;
            dup = true;
            break;
        }
        if (dup) continue;
        if (w != i) j->events[w] = j->events[i];
        w++;
    }
    j->count = w;
}

static void parse_tail(Journal *j, const char *path, const char *sid,
                       long from, long *parsed_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (from > 0 && fseek(f, from, SEEK_SET) != 0) { fclose(f); return; }

    // Строки истории длинные: в них лежат целые ответы агента и снимки файлов.
    // Читаем в свой буфер и то, что не поместилось, дочитываем вхолостую —
    // событие всегда начинается в первых сотнях байт строки.
    static char line[1 << 20];
    long pos = from;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        bool whole = len > 0 && line[len - 1] == '\n';
        if (whole) {
            parse_line(j, line, len, sid);
            pos += (long)len;
            continue;
        }
        // Строка длиннее буфера или файл дописывается прямо сейчас. Первый
        // случай разбираем по началу, второй бросаем: дочитаем в следующий раз.
        long chunk = (long)len;
        int c;
        bool ended = false;
        while ((c = fgetc(f)) != EOF) { chunk++; if (c == '\n') { ended = true; break; } }
        if (!ended) break;
        parse_line(j, line, len, sid);
        pos += chunk;
    }
    fclose(f);
    *parsed_out = pos;
}

void journal_load(Journal *j, const char *cwd)
{
    memset(j, 0, sizeof(*j));
    if (!cwd || !*cwd) return;

    char dir[600];
    projinfo_session_dir(cwd, dir, sizeof(dir));
    if (!dir[0]) return;

    char cache[700];
    cache_path(cwd, cache, sizeof(cache));

    Marks marks;
    memset(&marks, 0, sizeof(marks));
    if (cache[0]) cache_read(j, &marks, cache);

    DIR *d = opendir(dir);
    if (!d) return;

    bool changed = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".jsonl")) continue;

        char sid[PROJINFO_ID_MAX];
        size_t n = (size_t)(dot - e->d_name);
        if (n >= sizeof(sid)) n = sizeof(sid) - 1;
        memcpy(sid, e->d_name, n);
        sid[n] = '\0';

        char path[900];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        long done = mark_get(&marks, sid);
        if (done >= st.st_size) continue;   // с прошлого раза ничего не дописали

        long parsed = done;
        parse_tail(j, path, sid, done, &parsed);
        mark_set(&marks, sid, parsed);
        changed = true;
    }
    closedir(d);

    qsort(j->events, (size_t)j->count, sizeof(j->events[0]), by_time);
    dedup(j);

    if (changed && cache[0]) cache_write(j, &marks, cache);

    // Дневник читаем после записи кэша: в кэше живёт разбор истории, а записи
    // дневника человек правит руками, и держать их копию незачем.
    read_recaps(j, cwd);
    qsort(j->events, (size_t)j->count, sizeof(j->events[0]), by_time);
    drop_covered(j);
}

// --- как это называется на экране --------------------------------------------

static const char *MONTHS[12] = {
    "января", "февраля", "марта", "апреля", "мая", "июня",
    "июля", "августа", "сентября", "октября", "ноября", "декабря"
};

static const char *WEEKDAYS[7] = {
    "воскресенье", "понедельник", "вторник", "среда",
    "четверг", "пятница", "суббота"
};

void journal_day(time_t when, char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm a, b;
    localtime_r(&when, &a);
    localtime_r(&now, &b);

    // Дни считаем по календарю, а не разностью секунд: «вчера в 23:50» и
    // «сегодня в 00:10» разделяет десять минут, но это разные дни.
    int days = 0;
    struct tm t = b;
    for (; days < 3; days++) {
        if (t.tm_year == a.tm_year && t.tm_yday == a.tm_yday) break;
        time_t back = timelocal(&t) - 24 * 3600;
        localtime_r(&back, &t);
    }

    if (days == 0)      snprintf(out, cap, "сегодня");
    else if (days == 1) snprintf(out, cap, "вчера");
    else if (days == 2) snprintf(out, cap, "позавчера");
    else snprintf(out, cap, "%s, %d %s", WEEKDAYS[a.tm_wday % 7],
                  a.tm_mday, MONTHS[a.tm_mon % 12]);
}

void journal_clock(time_t when, char *out, size_t cap)
{
    struct tm t;
    localtime_r(&when, &t);
    snprintf(out, cap, "%02d:%02d", t.tm_hour, t.tm_min);
}

// --- упоминание в разговоре -----------------------------------------------

void journal_mention(const JournalEvent *ev, const char *cwd, char *out, size_t cap)
{
    char day[64], clock[16], until[16] = "";
    journal_day(ev->when, day, sizeof(day));
    journal_clock(ev->when, clock, sizeof(clock));
    if (ev->until > ev->when) journal_clock(ev->until, until, sizeof(until));

    // Метка в файле — UTC с точностью до минуты: по ней grep находит нужные
    // строки, а секунды только мешали бы попасть.
    char utc_from[32], utc_to[32] = "";
    struct tm tm;
    gmtime_r(&ev->when, &tm);
    strftime(utc_from, sizeof(utc_from), "%Y-%m-%dT%H:%M", &tm);
    if (ev->until > ev->when) {
        gmtime_r(&ev->until, &tm);
        strftime(utc_to, sizeof(utc_to), "%Y-%m-%dT%H:%M", &tm);
    }

    char dir[512];
    projinfo_session_dir(cwd, dir, sizeof(dir));

    // В шапке дневника id сессии короткий — восемь знаков. Файл ищется по
    // этому началу; не нашёлся — остаётся как есть, агент разберётся.
    char sid[PROJINFO_ID_MAX];
    snprintf(sid, sizeof(sid), "%s", ev->session);
    size_t plen = strlen(sid);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if (len > 6 && !strcmp(e->d_name + len - 6, ".jsonl")
                && !strncmp(e->d_name, sid, plen) && len - 6 < sizeof(sid)) {
                snprintf(sid, sizeof(sid), "%.*s", (int)(len - 6), e->d_name);
                break;
            }
        }
        closedir(d);
    }

    size_t n = 0;
    n += snprintf(out + n, cap - n, "Вернёмся к обсуждению «%s» (%s, %s", ev->text, day, clock);
    if (until[0] && n < cap) n += snprintf(out + n, cap - n, "–%s", until);
    if (n < cap) n += snprintf(out + n, cap - n, ").");
    if (ev->detail[0] && n < cap)
        n += snprintf(out + n, cap - n, " %s", ev->detail);
    if (n < cap)
        n += snprintf(out + n, cap - n,
                      "\n\nОно записано в сессии Claude Code %s: файл %s/%s.jsonl, "
                      "записи с меткой времени %s",
                      sid, dir, sid, utc_from);
    if (utc_to[0] && n < cap)
        n += snprintf(out + n, cap - n, "–%s", utc_to);
    if (n < cap)
        snprintf(out + n, cap - n,
                 " (UTC; например, grep по \"timestamp\":\"%.13s). Перечитай, о чём "
                 "там шла речь и к чему пришли, напомни мне коротко — и продолжим оттуда.",
                 utc_from);
}

time_t journal_file_mtime(const char *cwd)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", cwd, JOURNAL_FILE);
    struct stat sb;
    if (stat(path, &sb) != 0) return 0;
    return sb.st_mtime;
}
