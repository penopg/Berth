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

static bool digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

void table_cell_text(const char *raw, char *out, size_t cap)
{
    if (!raw) { if (cap) out[0] = '\0'; return; }

    // Дата контракта данных: ГГГГ-ММ-ДД, за ней может идти ЧЧ:ММ.
    if (digits(raw, 4) && raw[4] == '-' && digits(raw + 5, 2) && raw[7] == '-'
        && digits(raw + 8, 2)
        && (raw[10] == '\0' || raw[10] == ' ' || raw[10] == 'T')) {
        static const char *months[12] = { "янв", "фев", "мар", "апр", "мая", "июн",
                                          "июл", "авг", "сен", "окт", "ноя", "дек" };
        int y = atoi(raw), m = atoi(raw + 5), d = atoi(raw + 8);
        bool has_time = raw[10] && digits(raw + 11, 2) && raw[13] == ':' && digits(raw + 14, 2);
        if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            char hm[8] = "";
            if (has_time) snprintf(hm, sizeof(hm), " %.5s", raw + 11);
            time_t now = time(NULL), yd = now - 86400;
            struct tm tn, ty;
            localtime_r(&now, &tn);
            localtime_r(&yd, &ty);
            bool today = y == tn.tm_year + 1900 && m == tn.tm_mon + 1 && d == tn.tm_mday;
            bool yest  = y == ty.tm_year + 1900 && m == ty.tm_mon + 1 && d == ty.tm_mday;
            if (today)                      snprintf(out, cap, "сегодня%s", hm);
            else if (yest)                  snprintf(out, cap, "вчера%s", hm);
            else if (y == tn.tm_year + 1900) snprintf(out, cap, "%d %s%s", d, months[m - 1], hm);
            else                            snprintf(out, cap, "%d %s %d", d, months[m - 1], y);
            return;
        }
    }

    // Адрес почты: «Имя <адрес>» — имя; имени нет — сам адрес.
    size_t len = strlen(raw);
    const char *lt = strchr(raw, '<');
    if (lt && len > 0 && raw[len - 1] == '>' && strchr(lt, '@')) {
        size_t n = (size_t)(lt - raw);
        while (n > 0 && raw[n - 1] == ' ') n--;
        if (n >= 2 && raw[0] == '"' && raw[n - 1] == '"') { raw++; n -= 2; }
        if (n > 0) {
            if (n >= cap) n = cap - 1;
            memcpy(out, raw, n);
            out[n] = '\0';
        } else {
            snprintf(out, cap, "%.*s", (int)(len - (size_t)(lt - raw) - 2), lt + 1);
        }
        return;
    }

    snprintf(out, cap, "%s", raw);
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

// Главная — среди показанных колонок: у списка писем самая длинная в
// среднем — спрятанный «фрагмент», и выбор по всем колонкам отдавал ему
// место, которого на экране нет.
static void pick_main(Table *t)
{
    t->main_col = -1;
    for (int i = 0; i < t->col_count; i++) {
        if (!t->col_show[i] || t->col_avg[i] <= 0) continue;
        if (t->main_col < 0 || t->col_avg[i] > t->col_avg[t->main_col]) t->main_col = i;
    }
    t->two_line = t->main_col >= 0 && t->col_avg[t->main_col] >= 60;
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
    t->filter_col = -1;

    // Записей больше предела — берём **последние**: таблицы, которые
    // наполняются сами (письма), дописываются в конец, и новое лежит там.
    // Первый проход только считает, второй читает.
    char line[4096];
    long total = 0;
    {
        bool head = true;
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] != '\n' && len == sizeof(line) - 1) {
                int ch;
                while ((ch = fgetc(f)) != EOF && ch != '\n') { }
            }
            char *p = line;
            rtrim(p);
            if (!*p) continue;
            if (head) { head = false; continue; }
            total++;
        }
        rewind(f);
    }
    long skip = total > TABLE_ROWS_MAX ? total - TABLE_ROWS_MAX : 0;

    // Строка длиннее буфера — редкость (ячейка на 64 байта, колонок дюжина),
    // но хвост такой строки не должен стать следующей записью: дочитываем
    // его до перевода строки и выбрасываем.
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
            t->file_cols = total;
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
        if (t->file_rows <= skip) continue;
        if (t->row_count >= TABLE_ROWS_MAX) continue;
        int taken = 0;
        char (*row)[TABLE_CELL_MAX] = t->cells[t->row_count];
        int total = split(p, row, &taken);
        if (total > t->col_count) t->wide = true;
        for (int i = 0; i < t->col_count; i++)
            if (t->col_num[i] && !numeric(row[i])) t->col_num[i] = false;
        t->row_count++;
    }
    fclose(f);

    // Ширины — по показанной форме ячеек, не по сырой: «2026-09-03 17:30»
    // занимает в списке 11 знаков, а не 16. Здесь же средняя длина и выбор
    // главной колонки.
    t->main_col = -1;
    for (int i = 0; i < t->col_count; i++) {
        long sum = 0;
        int n = 0;
        for (int r = 0; r < t->row_count; r++) {
            if (!t->cells[r][i][0]) continue;
            char shown[TABLE_CELL_MAX];
            table_cell_text(t->cells[r][i], shown, sizeof(shown));
            int w = chars_of(shown);
            if (w > t->col_chars[i]) t->col_chars[i] = w;
            sum += w;
            n++;
        }
        t->col_avg[i] = n ? (int)(sum / n) : 0;
    }

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

    // Главная — после того, как колонки помечены показанными: выбор идёт
    // только среди них.
    pick_main(t);
    table_set_filter(t, NULL, false, NULL);
}

void table_set_filter(Table *t, const char *col, bool empty, const char *label)
{
    t->filter_col = -1;
    t->filter_empty = empty;
    t->filter_label[0] = '\0';
    if (col && *col)
        for (int i = 0; i < t->col_count; i++)
            if (!strcmp(t->cols[i], col)) { t->filter_col = i; break; }
    if (t->filter_col >= 0 && label)
        snprintf(t->filter_label, sizeof(t->filter_label), "%s", label);
    t->pass_count = 0;
    for (int r = 0; r < t->row_count; r++) {
        bool pass = true;
        if (t->filter_col >= 0) {
            bool has = t->cells[r][t->filter_col][0] != '\0';
            pass = empty ? !has : has;
        }
        t->row_pass[r] = pass;
        if (pass) t->pass_count++;
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
    pick_main(t);
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
