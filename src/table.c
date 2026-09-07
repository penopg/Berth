#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "table.h"

static void abs_path(const char *cwd, const char *rel, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", cwd, rel);
}

static time_t file_mtime(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

// Ширина в знаках, а не в байтах: в ячейке кириллица, и по strlen колонка
// вышла бы вдвое шире нужного.
static int chars_of(const char *s)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

// Число ли это целиком: «7.3» да, «7.3/10» и «к просмотру» нет. Пустая
// ячейка не мешает: неизвестное значение по формату оставляют пустым.
static bool numeric(const char *s)
{
    if (!*s) return true;
    char *end = NULL;
    strtod(s, &end);
    if (end == s) return false;
    while (*end == ' ') end++;
    return *end == '\0';
}

// Ячейка длиннее буфера обрезается по байтам, и последний символ может
// оказаться разрублен: в шрифте это «?», а то и мусор. Обрываем на границе.
static void trim_utf8(char *s)
{
    size_t n = strlen(s);
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2
                : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
    if (i - 1 + need > n) s[i - 1] = '\0';
}

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

// Разобрать строку на ячейки по табуляции. Возвращает, сколько колонок было
// в строке — включая те, что не поместились.
static int split(char *line, char cells[TABLE_COLS_MAX][TABLE_CELL_MAX], int *taken)
{
    int total = 0;
    *taken = 0;
    char *p = line;
    for (;;) {
        char *tab = strchr(p, '\t');
        if (tab) *tab = '\0';
        if (total < TABLE_COLS_MAX) {
            snprintf(cells[total], TABLE_CELL_MAX, "%s", p);
            trim_utf8(cells[total]);
            rtrim(cells[total]);
            (*taken)++;
        }
        total++;
        if (!tab) break;
        p = tab + 1;
    }
    return total;
}

void table_load(Table *t, const char *cwd, const char *rel)
{
    memset(t, 0, sizeof(*t));
    if (!cwd || !*cwd || !rel || !*rel) return;
    snprintf(t->path, sizeof(t->path), "%s", rel);

    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    snprintf(t->name, sizeof(t->name), "%s", base);
    char *dot = strrchr(t->name, '.');
    if (dot && dot != t->name) *dot = '\0';

    char path[800];
    abs_path(cwd, rel, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    t->mtime = file_mtime(path);

    // Строка длиннее буфера — редкость (ячейка на 64 байта, колонок дюжина),
    // но хвост такой строки не должен стать следующей записью: дочитываем
    // его до перевода строки и выбрасываем.
    char line[4096];
    bool first = true;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        bool full_line = len > 0 && line[len - 1] == '\n';
        if (!full_line && len == sizeof(line) - 1) {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') { }
        }
        char *p = line;
        if (first) {
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
                && (unsigned char)p[2] == 0xBF) p += 3;   // BOM для Excel
        }
        rtrim(p);
        if (!*p) continue;

        if (first) {
            int taken = 0;
            int total = split(p, t->cols, &taken);
            t->col_count = taken;
            t->wide = total > taken;
            for (int i = 0; i < t->col_count; i++) {
                t->col_chars[i] = chars_of(t->cols[i]);
                t->col_num[i] = true;
            }
            t->exists = t->col_count > 0;
            first = false;
            continue;
        }

        t->file_rows++;
        if (t->row_count >= TABLE_ROWS_MAX) continue;
        int taken = 0;
        char (*row)[TABLE_CELL_MAX] = t->cells[t->row_count];
        int total = split(p, row, &taken);
        if (total > t->col_count) t->wide = true;
        for (int i = 0; i < t->col_count; i++) {
            int w = chars_of(row[i]);
            if (w > t->col_chars[i]) t->col_chars[i] = w;
            if (t->col_num[i] && !numeric(row[i])) t->col_num[i] = false;
        }
        t->row_count++;
    }
    fclose(f);

    for (int i = 0; i < t->col_count; i++) {
        t->col_show[i] = true;
        t->col_order[i] = i;
    }

    // Колонка из одних пустых ячеек числовой не считается: равнять там
    // нечего, а правый край выглядел бы ошибкой.
    for (int i = 0; i < t->col_count; i++) {
        bool any = false;
        for (int r = 0; r < t->row_count && !any; r++) any = t->cells[r][i][0] != '\0';
        if (!any) t->col_num[i] = false;
    }
}

void table_set_cols(Table *t, const char *spec)
{
    for (int i = 0; i < t->col_count; i++) {
        t->col_order[i] = i;
        t->col_show[i] = true;
    }
    if (!spec || !*spec) return;

    bool placed[TABLE_COLS_MAX] = { false };
    int n = 0;
    const char *p = spec;
    while (*p) {
        const char *sep = strchr(p, '|');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        bool hide = len > 0 && *p == '-';
        const char *name = hide ? p + 1 : p;
        size_t nlen = hide ? len - 1 : len;
        for (int i = 0; i < t->col_count; i++) {
            if (placed[i] || strlen(t->cols[i]) != nlen || strncmp(t->cols[i], name, nlen))
                continue;
            placed[i] = true;
            t->col_order[n++] = i;
            t->col_show[i] = !hide;
            break;
        }
        if (!sep) break;
        p = sep + 1;
    }
    if (n == 0) return;   // ни одного знакомого имени — оставляем как в файле

    // Колонки, которых в списке не было: агент дописал их после того, как
    // человек настроил вид. Встают в конец и видимыми — пропасть молча они
    // не должны.
    for (int i = 0; i < t->col_count; i++)
        if (!placed[i]) { t->col_order[n++] = i; t->col_show[i] = true; }

    bool any = false;
    for (int i = 0; i < t->col_count; i++) any = any || t->col_show[i];
    if (!any) for (int i = 0; i < t->col_count; i++) t->col_show[i] = true;
}

void table_cols_spec(const Table *t, char *out, size_t cap)
{
    out[0] = '\0';
    bool plain = true;
    for (int k = 0; k < t->col_count; k++)
        if (t->col_order[k] != k || !t->col_show[t->col_order[k]]) plain = false;
    if (plain) return;

    size_t used = 0;
    for (int k = 0; k < t->col_count; k++) {
        int i = t->col_order[k];
        int n = snprintf(out + used, cap - used, "%s%s%s", used ? "|" : "",
                         t->col_show[i] ? "" : "-", t->cols[i]);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
}

bool table_changed(const Table *t, const char *cwd)
{
    if (!cwd || !*cwd || !t->path[0]) return false;
    char path[800];
    abs_path(cwd, t->path, path, sizeof(path));
    return file_mtime(path) != t->mtime;
}
