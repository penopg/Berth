#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xp.h"

#define XP_MAX      512
#define XP_PATH_MAX 512

typedef struct {
    char cwd[XP_PATH_MAX];
    long tokens;
    int  fights;
} XpEntry;

static XpEntry g_items[XP_MAX];
static int     g_count;
static char    g_path[XP_PATH_MAX];
static bool    g_dirty;
static time_t  g_written;

static XpEntry *find(const char *cwd, bool create)
{
    if (!cwd || !*cwd) return NULL;
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_items[i].cwd, cwd)) return &g_items[i];
    if (!create || g_count >= XP_MAX) return NULL;
    XpEntry *e = &g_items[g_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->cwd, sizeof(e->cwd), "%s", cwd);
    return e;
}

void xp_init(const char *path)
{
    g_count = 0;
    g_dirty = false;
    g_written = time(NULL);
    if (!path) { g_path[0] = '\0'; return; }
    snprintf(g_path, sizeof(g_path), "%s", path);

    FILE *f = fopen(g_path, "r");
    if (!f) return;
    char line[XP_PATH_MAX + 64];
    while (fgets(line, sizeof(line), f)) {
        // <путь>\t<токены>\t<бои>
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        long tokens = strtol(tab + 1, &tab, 10);
        int fights = *tab == '\t' ? (int)strtol(tab + 1, NULL, 10) : 0;
        XpEntry *e = find(line, true);
        if (e) { e->tokens = tokens; e->fights = fights; }
    }
    fclose(f);
}

long xp_tokens(const char *cwd)
{
    const XpEntry *e = find(cwd, false);
    return e ? e->tokens : 0;
}

int xp_fights(const char *cwd)
{
    const XpEntry *e = find(cwd, false);
    return e ? e->fights : 0;
}

void xp_add(const char *cwd, long tokens)
{
    if (tokens <= 0) return;
    XpEntry *e = find(cwd, true);
    if (!e) return;
    e->tokens += tokens;
    g_dirty = true;
}

void xp_fight(const char *cwd)
{
    XpEntry *e = find(cwd, true);
    if (!e) return;
    e->fights++;
    g_dirty = true;
}

void xp_flush(void)
{
    if (!g_dirty || !g_path[0]) return;
    // Через временный файл и rename: полфайла при обрыве хуже, чем старый.
    char tmp[XP_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.new", g_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_count; i++)
        fprintf(f, "%s\t%ld\t%d\n", g_items[i].cwd, g_items[i].tokens, g_items[i].fights);
    fclose(f);
    if (rename(tmp, g_path) != 0) { unlink(tmp); return; }
    g_dirty = false;
    g_written = time(NULL);
}

void xp_flush_maybe(void)
{
    if (g_dirty && time(NULL) - g_written >= 30) xp_flush();
}
