#include "pending.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define PENDING_MAX 160

typedef struct {
    char   cwd[512];
    time_t shown_mtime;
    time_t table_mtime;
    time_t checked;
    char   rel[256];
    char   col[64];
    int    empty;
    char   label[48];
    int    count;
} Entry;

static Entry g_items[PENDING_MAX];
static int   g_count;

static time_t mtime_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

// Первая строка `filter` в shown.tsv: путь, колонка, пусто/не пусто, подпись.
static void read_filter(Entry *e, const char *path)
{
    e->rel[0] = e->col[0] = e->label[0] = '\0';
    e->empty = 1;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "filter\t", 7)) continue;
        rtrim(line);
        char *fld[5] = { 0 };
        int n = 0;
        char *p = line;
        while (n < 5 && p) {
            fld[n++] = p;
            p = strchr(p, '\t');
            if (p) *p++ = '\0';
        }
        if (n < 4) continue;
        const char *rel = fld[1];
        if (!strncmp(rel, "./", 2)) rel += 2;
        snprintf(e->rel, sizeof(e->rel), "%s", rel);
        snprintf(e->col, sizeof(e->col), "%s", fld[2]);
        e->empty = strcmp(fld[3], "не пусто") != 0 && strcmp(fld[3], "nonempty") != 0;
        snprintf(e->label, sizeof(e->label), "%s", n >= 5 && fld[4] ? fld[4] : "");
        break;
    }
    fclose(f);
}

// Счёт потоком: заголовок — номер колонки, дальше по строке на запись.
static int count_rows(const char *path, const char *col, int empty)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[8192];
    int idx = -1, n = 0;

    int first = 1;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] != '\n' && len == sizeof(line) - 1) {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') { }
        }
        char *p = line;
        if (first && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
            && (unsigned char)p[2] == 0xBF) p += 3;
        rtrim(p);
        if (!*p) continue;
        int k = 0;
        char *cell = p;
        int found = 0;
        while (cell) {
            char *tab = strchr(cell, '\t');
            if (tab) *tab = '\0';
            if (first) {
                if (!strcmp(cell, col)) { idx = k; break; }
            } else if (k == idx) {
                found = 1;
                int has = cell[0] != '\0';
                if (empty ? !has : has) n++;
                break;
            }
            k++;
            cell = tab ? tab + 1 : NULL;
        }
        if (first) {
            first = 0;
            if (idx < 0) break;   // колонки нет — считать нечего
            continue;
        }
        // Строка короче заголовка: ячейки нет — она пуста.
        if (!found && empty) n++;
    }
    fclose(f);
    return n;
}

int pending_get(const char *cwd, char *label, size_t cap)
{
    if (label && cap) label[0] = '\0';
    if (!cwd || !*cwd) return 0;

    Entry *e = NULL;
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_items[i].cwd, cwd)) { e = &g_items[i]; break; }
    if (!e) {
        if (g_count < PENDING_MAX) e = &g_items[g_count++];
        else {
            e = &g_items[0];
            for (int i = 1; i < g_count; i++)
                if (g_items[i].checked < e->checked) e = &g_items[i];
        }
        memset(e, 0, sizeof(*e));
        snprintf(e->cwd, sizeof(e->cwd), "%s", cwd);
    }

    time_t now = time(NULL);
    if (now - e->checked >= 2) {
        e->checked = now;
        char path[800];
        snprintf(path, sizeof(path), "%s/.berth/shown.tsv", cwd);
        time_t sm = mtime_of(path);
        if (sm != e->shown_mtime) {
            e->shown_mtime = sm;
            e->table_mtime = 0;
            e->count = 0;
            if (sm) read_filter(e, path); else e->rel[0] = '\0';
        }
        if (e->rel[0]) {
            snprintf(path, sizeof(path), "%s/%s", cwd, e->rel);
            time_t tm = mtime_of(path);
            if (tm != e->table_mtime) {
                e->table_mtime = tm;
                e->count = tm ? count_rows(path, e->col, e->empty) : 0;
            }
        }
    }
    if (label && cap) snprintf(label, cap, "%s", e->rel[0] ? e->label : "");
    return e->rel[0] ? e->count : 0;
}
