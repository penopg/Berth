#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "ctx.h"

// Таблица из бинаря Claude Code (`context:{window:…}` в описании моделей,
// версия 2.1.260). Суффикс «[1m]» тоже значит миллион.
long ctx_window_of(const char *model)
{
    if (strstr(model, "[1m]")) return 1000000;
    static const struct { const char *prefix; long window; } table[] = {
        { "claude-haiku",      200000 },
        { "claude-sonnet-4",   200000 },
        { "claude-opus-4-0",   200000 },
        { "claude-opus-4-1",   200000 },
        { "claude-opus-4-5",   200000 },
        { "claude-opus-4-6",   200000 },
        { "claude-opus-4-7",  1000000 },
        { "claude-opus-4-8",  1000000 },
        { "claude-opus-5",    1000000 },
        { "claude-sonnet-5",  1000000 },
        { "claude-fable",     1000000 },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (!strncmp(model, table[i].prefix, strlen(table[i].prefix)))
            return table[i].window;
    return 1000000;
}

// Число после `"key":` в пределах строки. Ключи ищем по одному, а не
// разбираем объект: внутри `usage` бывают вложенные объекты
// (`server_tool_use`, `output_tokens_details`), и «до закрывающей скобки»
// обрезало бы половину полей.
static long field_long(const char *line, const char *key)
{
    const char *p = strstr(line, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == ':') p++;
    return strtol(p, NULL, 10);
}

static void field_str(const char *line, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = strstr(line, key);
    if (!p) return;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
}

// `"timestamp":"2026-09-04T10:28:50.766Z"` — время записи, всегда UTC.
static time_t field_time(const char *line)
{
    char ts[40];
    field_str(line, "\"timestamp\"", ts, sizeof(ts));
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (sscanf(ts, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    return timegm(&tm);
}

// Разбирается только хвост: файл только дописывается, а последний ответ
// агента лежит в конце. Но за ответом идут записи человека и служебные,
// в том числе картинки base64 на сотни килобайт, поэтому сначала 256 КБ,
// а если ответа в них нет — 2 МБ. По реальным файлам: 64 КБ промахивались
// на трёх из тридцати, 256 КБ — ни разу.
static bool read_tail(const char *path, long tail, char **buf_out, size_t *n_out, long *from_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > tail ? size - tail : 0;
    fseek(f, from, SEEK_SET);
    char *buf = malloc((size_t)tail + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)tail, f);
    fclose(f);
    buf[n] = '\0';
    *buf_out = buf; *n_out = n; *from_out = from;
    return true;
}

static bool scan(char *buf, long from, CtxInfo *out);

bool ctx_read(const char *jsonl_path, CtxInfo *out)
{
    static const long tails[] = { 256 * 1024, 2 * 1024 * 1024 };
    for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
        char *buf; size_t n; long from;
        if (!read_tail(jsonl_path, tails[i], &buf, &n, &from)) return false;
        bool found = scan(buf, from, out);
        bool whole = from == 0;
        free(buf);
        if (found || whole) return found;
    }
    return false;
}

static bool scan(char *buf, long from, CtxInfo *out)
{
    // Первая строка хвоста скорее всего оборвана — пропускаем до перевода.
    char *p = buf;
    if (from > 0) {
        p = strchr(buf, '\n');
        if (!p) return false;
        p++;
    }

    bool found = false;
    CtxInfo best = {0};
    for (char *line = p; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strstr(line, "\"type\":\"assistant\"") && strstr(line, "\"usage\":{")) {
            char model[64];
            field_str(line, "\"model\"", model, sizeof(model));
            long used = field_long(line, "\"input_tokens\"")
                      + field_long(line, "\"cache_creation_input_tokens\"")
                      + field_long(line, "\"cache_read_input_tokens\"");
            // Служебные записи (`<synthetic>`) несут нули — это не ответ.
            if (used > 0 && model[0] != '<') {
                best.used = used;
                snprintf(best.model, sizeof(best.model), "%s", model);
                best.window = ctx_window_of(model);
                best.at = field_time(line);
                found = true;
            }
        } else if (strstr(line, "\"subtype\":\"compact_boundary\"")) {
            // Сжатие: Claude Code сам записывает, сколько осталось после
            // него (`postTokens`). До первого ответа в сжатом разговоре это
            // единственная верная цифра — usage последнего ответа рассказывает
            // про контекст, которого уже нет. Окно остаётся от последнего
            // ответа; если его в хвосте не было, берётся окно по умолчанию.
            long post = field_long(line, "\"postTokens\"");
            if (post > 0) {
                best.used = post;
                if (best.window <= 0) best.window = ctx_window_of(best.model);
                best.at = field_time(line);
                found = true;
            }
        }
        line = nl ? nl + 1 : NULL;
    }

    if (found) *out = best;
    return found;
}

long ctx_spent_since(const char *jsonl_path, long *offset, char last_id[64])
{
    FILE *f = fopen(jsonl_path, "r");
    if (!f) return 0;
    if (*offset < 0) {
        fseek(f, 0, SEEK_END);
        *offset = ftell(f);
        fclose(f);
        return 0;
    }
    fseek(f, *offset, SEEK_SET);

    long spent = 0;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        // Незаконченная строка (без перевода) дописывается прямо сейчас —
        // её дочитаем в следующий раз, с того же места.
        if (line[n - 1] != '\n') break;
        *offset += n;
        if (!strstr(line, "\"type\":\"assistant\"") || !strstr(line, "\"usage\":{")) continue;

        char id[64];
        field_str(line, "\"message\":{\"id\"", id, sizeof(id));
        if (!id[0]) field_str(line, "\"id\":\"msg_", id, sizeof(id));
        if (id[0] && !strcmp(id, last_id)) continue;
        snprintf(last_id, 64, "%s", id);
        spent += field_long(line, "\"output_tokens\"");
    }
    free(line);
    fclose(f);
    return spent;
}
