#include "timers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMERS_MAX 64

typedef struct {
    char   cwd[512];
    char   name[48];
    time_t when;
} Timer;

static Timer g_items[TIMERS_MAX];
static int   g_count;
static char  g_path[600];
static int   g_loaded;

static void load(void)
{
    g_loaded = 1;
    g_count = 0;
    if (!g_path[0]) return;
    FILE *f = fopen(g_path, "r");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof(line), f) && g_count < TIMERS_MAX) {
        char *a = line, *b = strchr(a, '\t');
        if (!b) continue;
        *b++ = '\0';
        char *c = strchr(b, '\t');
        if (!c) continue;
        *c++ = '\0';
        Timer *t = &g_items[g_count];
        snprintf(t->cwd, sizeof(t->cwd), "%s", a);
        snprintf(t->name, sizeof(t->name), "%s", b);
        t->when = (time_t)strtoll(c, NULL, 10);
        if (t->cwd[0] && t->name[0] && t->when > 0) g_count++;
    }
    fclose(f);
}

static void save(void)
{
    if (!g_path[0]) return;
    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.new", g_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_count; i++)
        fprintf(f, "%s\t%s\t%lld\n", g_items[i].cwd, g_items[i].name,
                (long long)g_items[i].when);
    fclose(f);
    rename(tmp, g_path);
}

static Timer *find(const char *cwd, const char *name)
{
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_items[i].cwd, cwd) && !strcmp(g_items[i].name, name))
            return &g_items[i];
    return NULL;
}

void timers_init(const char *path)
{
    snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
    g_loaded = 0;
}

time_t timers_last(const char *cwd, const char *name)
{
    if (!cwd || !name) return 0;
    if (!g_loaded) load();
    Timer *t = find(cwd, name);
    return t ? t->when : 0;
}

void timers_mark(const char *cwd, const char *name, time_t when)
{
    if (!cwd || !name) return;
    if (!g_loaded) load();
    Timer *t = find(cwd, name);
    if (!t) {
        // Места нет — вытесняем самый давний: он всё равно давно не нужен.
        if (g_count < TIMERS_MAX) t = &g_items[g_count++];
        else {
            t = &g_items[0];
            for (int i = 1; i < g_count; i++)
                if (g_items[i].when < t->when) t = &g_items[i];
        }
        snprintf(t->cwd, sizeof(t->cwd), "%s", cwd);
        snprintf(t->name, sizeof(t->name), "%s", name);
    }
    t->when = when;
    save();
}
