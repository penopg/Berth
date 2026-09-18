#include "sections.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTIONS_MAX 512

typedef struct {
    char cwd[512];
    char key[80];
    bool open;
} Entry;

static Entry g_items[SECTIONS_MAX];
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
    while (fgets(line, sizeof(line), f) && g_count < SECTIONS_MAX) {
        char *a = line, *b = strchr(a, '\t');
        if (!b) continue;
        *b++ = '\0';
        char *c = strchr(b, '\t');
        if (!c) continue;
        *c++ = '\0';
        Entry *e = &g_items[g_count];
        snprintf(e->cwd, sizeof(e->cwd), "%s", a);
        snprintf(e->key, sizeof(e->key), "%s", b);
        e->open = c[0] == '1';
        if (e->cwd[0] && e->key[0]) g_count++;
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
        fprintf(f, "%s\t%s\t%d\n", g_items[i].cwd, g_items[i].key, g_items[i].open ? 1 : 0);
    fclose(f);
    rename(tmp, g_path);
}

static Entry *find(const char *cwd, const char *key)
{
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_items[i].cwd, cwd) && !strcmp(g_items[i].key, key))
            return &g_items[i];
    return NULL;
}

void sections_init(const char *path)
{
    snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
    g_loaded = 0;
}

int sections_get(const char *cwd, const char *key)
{
    if (!cwd || !key) return -1;
    if (!g_loaded) load();
    Entry *e = find(cwd, key);
    return e ? (e->open ? 1 : 0) : -1;
}

void sections_set(const char *cwd, const char *key, bool open)
{
    if (!cwd || !key || !*cwd || !*key) return;
    if (!g_loaded) load();
    Entry *e = find(cwd, key);
    if (!e) {
        // Полный список теряет самую давнюю запись: она про проект, куда
        // давно не ходили.
        if (g_count == SECTIONS_MAX) {
            memmove(&g_items[0], &g_items[1], sizeof(Entry) * (SECTIONS_MAX - 1));
            g_count--;
        }
        e = &g_items[g_count++];
        snprintf(e->cwd, sizeof(e->cwd), "%s", cwd);
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    e->open = open;
    save();
}
