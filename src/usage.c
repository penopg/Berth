#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "usage.h"

// Разбор здесь свой и нарочно грубый: нужен один вложенный массив в файле на
// полтораста килобайт, а json.c умеет только плоский массив объектов. Полный
// парсер ради трёх полей — цена выше пользы.

static const char *usage_path(void)
{
    static char path[512];
    if (path[0]) return path;
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(path, sizeof(path), "%s/.claude.json", home);
    return path;
}

// Конец объекта или массива, начинающегося в p. Скобки внутри строк не в счёт.
static const char *span_end(const char *p, const char *end)
{
    char open = *p, close = open == '{' ? '}' : ']';
    int depth = 0;
    bool in_str = false;
    for (; p < end; p++) {
        if (in_str) {
            if (*p == '\\') p++;
            else if (*p == '"') in_str = false;
            continue;
        }
        if (*p == '"') in_str = true;
        else if (*p == open) depth++;
        else if (*p == close && --depth == 0) return p + 1;
    }
    return end;
}

// Начало значения поля "name" в пределах [p, end). NULL, если поля нет.
static const char *field(const char *p, const char *end, const char *name)
{
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", name);
    for (const char *q = p; q + n < end; q++) {
        if (memcmp(q, pat, (size_t)n)) continue;
        q += n;
        while (q < end && (*q == ' ' || *q == ':')) q++;
        return q;
    }
    return NULL;
}

static bool str_field(const char *p, const char *end, const char *name,
                      char *out, size_t cap)
{
    const char *v = field(p, end, name);
    if (!v || v >= end || *v != '"') return false;
    v++;
    size_t n = 0;
    while (v < end && *v != '"' && n + 1 < cap) {
        if (*v == '\\') v++;
        out[n++] = *v++;
    }
    out[n] = '\0';
    return n > 0;
}

static bool int_field(const char *p, const char *end, const char *name, int *out)
{
    const char *v = field(p, end, name);
    if (!v || v >= end) return false;
    if (*v < '0' || *v > '9') return false;   // null или что-то не то
    *out = atoi(v);
    return true;
}

// «2026-09-02T13:49:59.732469+00:00» — время UTC. Дробь и смещение
// пропускаем: сервер отдаёт нули, а разбирать часовые пояса ради шапки незачем.
static time_t parse_iso_utc(const char *s)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    int y, mo, d, h, mi, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = sec;
    return timegm(&tm);
}

// Подпись окна. Недельных окна два: общее на все модели и отдельное на
// «дорогую» — сервер называет её сам (`scope.model.display_name`). Писать
// обоим «неделя» нельзя: в полосе получалось «неделя 17% · неделя · Fable
// 21%», и было не понять, что это разные счётчики. Поэтому у второго подпись —
// имя модели: оно и отличает его от общего.
static void label_for(const char *kind, const char *model, char *out, size_t cap)
{
    if (!strcmp(kind, "session"))          snprintf(out, cap, "сессия");
    else if (!strcmp(kind, "weekly_all"))  snprintf(out, cap, "неделя");
    else if (!strcmp(kind, "weekly_scoped"))
        snprintf(out, cap, "%s", model[0] ? model : "неделя · модель");
    else                                   snprintf(out, cap, "%s", kind);
}

// Разбирает массив `limits` где угодно внутри [p, end). У кэша Claude Code
// он лежит в `cachedUsageUtilization.utilization`, у ответа сервера — в корне;
// форма самих записей одна.
static void parse_limits(const char *p, const char *end, Usage *u)
{
    const char *arr = field(p, end, "limits");
    if (!arr || *arr != '[') return;
    const char *arr_end = span_end(arr, end);

    time_t now = time(NULL);
    const char *q = arr + 1;
    while (q < arr_end && u->count < USAGE_LIMIT_MAX) {
        while (q < arr_end && *q != '{') q++;
        if (q >= arr_end) break;
        const char *obj_end = span_end(q, arr_end);

        char kind[32] = "", sev[16] = "", model[24] = "", resets[40] = "";
        int percent = 0;
        str_field(q, obj_end, "kind", kind, sizeof(kind));
        str_field(q, obj_end, "severity", sev, sizeof(sev));
        str_field(q, obj_end, "display_name", model, sizeof(model));
        str_field(q, obj_end, "resets_at", resets, sizeof(resets));
        int_field(q, obj_end, "percent", &percent);

        time_t resets_at = resets[0] ? parse_iso_utc(resets) : 0;

        // Окно, которое уже обнулилось, показывать нечестно: проценты
        // остались от прошлого окна, а свежих данных ещё нет.
        bool stale = resets_at > 0 && resets_at <= now;
        if (kind[0] && !stale) {
            UsageLimit *l = &u->items[u->count++];
            label_for(kind, model, l->label, sizeof(l->label));
            l->kind = !strcmp(kind, "session")       ? USAGE_KIND_SESSION
                    : !strcmp(kind, "weekly_all")    ? USAGE_KIND_WEEKLY_ALL
                    : !strcmp(kind, "weekly_scoped") ? USAGE_KIND_WEEKLY_SCOPED
                                                     : USAGE_KIND_OTHER;
            l->percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
            l->severity = !strcmp(sev, "critical") ? USAGE_CRITICAL
                        : !strcmp(sev, "warning")  ? USAGE_WARNING
                                                   : USAGE_NORMAL;
            l->resets_at = resets_at;
        }
        q = obj_end;
    }
}

static char *read_whole(const char *path, size_t *len, time_t *mtime)
{
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    if (mtime) *mtime = st.st_mtime;
    if (st.st_size <= 0 || st.st_size > 8 * 1024 * 1024) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

void usage_set_own_path(Usage *u, const char *path)
{
    snprintf(u->own_path, sizeof(u->own_path), "%s", path ? path : "");
}

void usage_load(Usage *u)
{
    // Пути и время последнего запуска переживают перечитывание.
    char own_path[sizeof(u->own_path)];
    snprintf(own_path, sizeof(own_path), "%s", u->own_path);
    time_t last_spawn = u->last_spawn;
    memset(u, 0, sizeof(*u));
    snprintf(u->own_path, sizeof(u->own_path), "%s", own_path);
    u->last_spawn = last_spawn;

    // Кэш Claude Code: лимиты и почта учётки.
    time_t cache_fetched = 0;
    Usage cache;
    memset(&cache, 0, sizeof(cache));
    const char *path = usage_path();
    size_t len = 0;
    char *buf = path ? read_whole(path, &len, &u->cache_mtime) : NULL;
    if (buf) {
        const char *end = buf + len;
        str_field(buf, end, "emailAddress", u->account, sizeof(u->account));
        const char *root = field(buf, end, "cachedUsageUtilization");
        if (root && *root == '{') {
            const char *root_end = span_end(root, end);
            const char *fetched = field(root, root_end, "fetchedAtMs");
            if (fetched && *fetched >= '0' && *fetched <= '9')
                cache_fetched = (time_t)(atoll(fetched) / 1000);
            parse_limits(root, root_end, &cache);
        }
        free(buf);
    }

    // Свой файл: время получения — время файла, он пишется сразу после ответа.
    Usage own;
    memset(&own, 0, sizeof(own));
    time_t own_fetched = 0;
    buf = u->own_path[0] ? read_whole(u->own_path, &len, &u->own_mtime) : NULL;
    if (buf) {
        own_fetched = u->own_mtime;
        parse_limits(buf, buf + len, &own);
        free(buf);
    }

    // Более свежий побеждает. Пустой ответ (ошибка, отказ) не считается.
    bool take_own = own.count > 0 && own_fetched >= cache_fetched;
    const Usage *src = take_own ? &own : &cache;
    memcpy(u->items, src->items, sizeof(u->items));
    u->count   = src->count;
    u->fetched = take_own ? own_fetched : cache_fetched;
    u->source  = u->count > 0 ? (take_own ? USAGE_FROM_OWN : USAGE_FROM_CACHE)
                              : USAGE_FROM_NONE;
}

// Запрос в отвязанном процессе: `security` достаёт токен Claude Code из
// связки ключей, `curl` спрашивает лимиты. Ответ кладётся рядом через
// временный файл, чтобы берт не прочитал половину. Ошибка HTTP (`-f`) —
// файл не трогается, остаётся прошлый ответ; стандартный поток ошибок — в
// usage.log рядом, чтобы было куда посмотреть, когда «не работает».
static const char *FETCH_SCRIPT =
    "tok=$(security find-generic-password -s \"Claude Code-credentials\" -w 2>>\"$BERTH_USAGE_LOG\""
    " | sed -n 's/.*\"accessToken\":\"\\([^\"]*\\)\".*/\\1/p');"
    " if [ -z \"$tok\" ]; then echo \"$(date +%H:%M:%S) токен Claude Code не найден в связке ключей\" >>\"$BERTH_USAGE_LOG\"; exit 1; fi;"
    " curl -sS -f -m 20 -H \"Authorization: Bearer $tok\" -H \"anthropic-beta: oauth-2025-04-20\""
    " -H \"User-Agent: berth\" https://api.anthropic.com/api/oauth/usage"
    " -o \"$BERTH_USAGE_OUT.tmp\" 2>>\"$BERTH_USAGE_LOG\""
    " && mv -f \"$BERTH_USAGE_OUT.tmp\" \"$BERTH_USAGE_OUT\" || rm -f \"$BERTH_USAGE_OUT.tmp\"";

bool usage_fetch_maybe(Usage *u, int min_interval_s)
{
    if (!u->own_path[0]) return false;
    time_t now = time(NULL);
    if (u->last_spawn && now - u->last_spawn < min_interval_s) return false;
    u->last_spawn = now;

    char log[sizeof(u->own_path) + 8];
    snprintf(log, sizeof(log), "%s.log", u->own_path);
    // Лог не растёт бесконечно: каждый запуск начинает его заново.
    FILE *lf = fopen(log, "w");
    if (lf) fclose(lf);

    // Двойной fork: внук не остаётся зомби, когда берт его не ждёт.
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (fork() != 0) _exit(0);
        setenv("BERTH_USAGE_OUT", u->own_path, 1);
        setenv("BERTH_USAGE_LOG", log, 1);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", FETCH_SCRIPT, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return true;
}

bool usage_changed(const Usage *u)
{
    struct stat st;
    const char *path = usage_path();
    if (path && stat(path, &st) == 0 && st.st_mtime != u->cache_mtime) return true;
    if (u->own_path[0]) {
        time_t own = stat(u->own_path, &st) == 0 ? st.st_mtime : 0;
        if (own != u->own_mtime) return true;
    }
    return false;
}

void usage_reset_text(time_t resets_at, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (resets_at <= 0) return;

    time_t now = time(NULL);
    struct tm when, today;
    localtime_r(&resets_at, &when);
    localtime_r(&now, &today);

    // У пятичасового окна важен час, у недельного — день: «до 23:59» про
    // субботу читается как «сегодня вечером» и обманывает.
    if (when.tm_yday == today.tm_yday && when.tm_year == today.tm_year) {
        snprintf(out, cap, "до %02d:%02d", when.tm_hour, when.tm_min);
        return;
    }
    static const char *months[] = {
        "янв", "фев", "мар", "апр", "мая", "июн",
        "июл", "авг", "сен", "окт", "ноя", "дек",
    };
    snprintf(out, cap, "до %d %s", when.tm_mday, months[when.tm_mon % 12]);
}

// «2 часа», «5 минут», «1 день»: русское число с существительным.
static void plural(long n, const char *one, const char *few, const char *many,
                   char *out, size_t cap)
{
    long r10 = n % 10, r100 = n % 100;
    const char *word = (r10 == 1 && r100 != 11) ? one
                     : (r10 >= 2 && r10 <= 4 && (r100 < 12 || r100 > 14)) ? few
                     : many;
    snprintf(out, cap, "%ld %s", n, word);
}

// «через 2 часа 15 минут», «через 3 дня 6 часов». Две единицы, не три:
// минуты при днях — шум.
static void countdown_text(long secs, char *out, size_t cap)
{
    long mins = (secs + 59) / 60;
    char a[32] = "", b[32] = "";
    if (mins < 60) {
        plural(mins < 1 ? 1 : mins, "минуту", "минуты", "минут", a, sizeof(a));
    } else if (mins < 24 * 60) {
        plural(mins / 60, "час", "часа", "часов", a, sizeof(a));
        if (mins % 60) plural(mins % 60, "минуту", "минуты", "минут", b, sizeof(b));
    } else {
        long days = mins / (24 * 60), hours = (mins / 60) % 24;
        plural(days, "день", "дня", "дней", a, sizeof(a));
        if (hours) plural(hours, "час", "часа", "часов", b, sizeof(b));
    }
    snprintf(out, cap, "через %s%s%s", a, b[0] ? " " : "", b);
}

static void clock_text(time_t t, bool with_day, char *out, size_t cap)
{
    struct tm tm;
    localtime_r(&t, &tm);
    static const char *months[] = {
        "янв", "фев", "мар", "апр", "мая", "июн",
        "июл", "авг", "сен", "окт", "ноя", "дек",
    };
    if (with_day)
        snprintf(out, cap, "%d %s в %02d:%02d", tm.tm_mday, months[tm.tm_mon % 12],
                 tm.tm_hour, tm.tm_min);
    else
        snprintf(out, cap, "в %02d:%02d", tm.tm_hour, tm.tm_min);
}

void usage_tip_text(const Usage *u, int index, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = '\0';
    if (!u || index < 0 || index >= u->count) return;
    const UsageLimit *l = &u->items[index];

    char title[USAGE_LABEL_MAX + 48];
    switch (l->kind) {
    case USAGE_KIND_SESSION:
        snprintf(title, sizeof(title), "Пятичасовое окно");
        break;
    case USAGE_KIND_WEEKLY_ALL:
        snprintf(title, sizeof(title), "Неделя, все модели");
        break;
    case USAGE_KIND_WEEKLY_SCOPED:
        snprintf(title, sizeof(title), "Неделя, только %s", l->label);
        break;
    default:
        snprintf(title, sizeof(title), "%s", l->label);
        break;
    }

    time_t now = time(NULL);
    char when[96];
    if (l->resets_at <= 0) {
        snprintf(when, sizeof(when), "Когда обновится, сервер не сообщил");
    } else if (l->resets_at <= now) {
        // Между опросами окно могло обнулиться; проценты уже прошлые.
        snprintf(when, sizeof(when), "Уже обновилось, ждём свежих цифр");
    } else {
        struct tm a, b;
        localtime_r(&l->resets_at, &a);
        localtime_r(&now, &b);
        bool same_day = a.tm_yday == b.tm_yday && a.tm_year == b.tm_year;
        char at[48], left[64];
        clock_text(l->resets_at, !same_day, at, sizeof(at));
        countdown_text((long)(l->resets_at - now), left, sizeof(left));
        snprintf(when, sizeof(when), "Обновится %s, %s", at, left);
    }

    char source[96] = "";
    if (u->fetched > 0) {
        char at[48];
        struct tm a, b;
        localtime_r(&u->fetched, &a);
        localtime_r(&now, &b);
        bool same_day = a.tm_yday == b.tm_yday && a.tm_year == b.tm_year;
        clock_text(u->fetched, !same_day, at, sizeof(at));
        snprintf(source, sizeof(source), "Цифры получены %s, %s", at,
                 u->source == USAGE_FROM_OWN ? "своим запросом"
                                             : "из кэша Claude Code");
    }

    snprintf(out, cap, "%s · %d%%\n%s%s%s", title, l->percent, when,
             source[0] ? "\n" : "", source);
}
