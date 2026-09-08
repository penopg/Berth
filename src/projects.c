#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "settings.h"
#include "projects.h"
#include "json.h"

const char *projects_default_path(void)
{
    static char path[PROJECT_PATH_MAX];
    if (path[0]) return path;

    const char *home = berth_home();
    if (!home) return NULL;
    snprintf(path, sizeof(path), "%s/.claude/warp-tabs/projects.json", home);
    return path;
}

// Цвет приходит именем ANSI. Берём не чистый ANSI, а приглушённый тон:
// панель должна читаться, а не светиться.
static Color color_by_name(const char *name)
{
    struct { const char *name; Color color; } table[] = {
        { "red",     {235, 130, 130, 255} },
        { "green",   {130, 200, 150, 255} },
        { "yellow",  {225, 190,  95, 255} },
        { "blue",    {120, 170, 245, 255} },
        { "magenta", {200, 140, 220, 255} },
        { "cyan",    {120, 200, 210, 255} },
        { "white",   {200, 200, 210, 255} },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].name, name) == 0)
            return table[i].color;
    return (Color){170, 170, 185, 255};
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(f); return false; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return false; }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';

    *out = buf;
    *out_len = got;
    return true;
}

bool projects_load(ProjectList *list, const char *path)
{
    memset(list, 0, sizeof(*list));
    if (!path) return false;
    copy_str(list->source, sizeof(list->source), path);

    struct stat st;
    if (stat(path, &st) == 0) list->mtime = st.st_mtime;

    char *text = NULL;
    size_t len = 0;
    if (!read_file(path, &text, &len)) return false;

    JsonScan scan;
    if (!json_scan_init(&scan, text, len)) { free(text); return false; }

    while (list->count < PROJECT_MAX && json_scan_object(&scan)) {
        Project *p = &list->items[list->count];
        memset(p, 0, sizeof(*p));
        p->parent = -1;
        char color_name[PROJECT_NAME_MAX] = "";

        char key[64], val[PROJECT_PATH_MAX];
        while (json_scan_field(&scan, key, sizeof(key), val, sizeof(val))) {
            if      (strcmp(key, "name")  == 0) copy_str(p->name,  sizeof(p->name),  val);
            else if (strcmp(key, "path")  == 0) copy_str(p->path,  sizeof(p->path),  val);
            else if (strcmp(key, "group") == 0) copy_str(p->group, sizeof(p->group), val);
            else if (strcmp(key, "icon")  == 0) copy_str(p->icon,  sizeof(p->icon),  val);
            else if (strcmp(key, "theme") == 0) copy_str(p->theme, sizeof(p->theme), val);
            else if (strcmp(key, "color") == 0) copy_str(color_name, sizeof(color_name), val);
        }

        // Запись без пути бесполезна — открывать нечего.
        if (p->path[0] == '\0') continue;
        if (p->name[0] == '\0') copy_str(p->name, sizeof(p->name), p->path);
        p->color = color_by_name(color_name);
        list->count++;
    }

    free(text);
    return list->count > 0;
}

bool projects_changed(const ProjectList *list)
{
    if (!list->source[0]) return false;
    struct stat st;
    if (stat(list->source, &st) != 0) return false;
    return st.st_mtime != list->mtime;
}

int projects_find_by_path(const ProjectList *list, const char *path)
{
    if (!path || !*path) return -1;
    for (int i = 0; i < list->count; i++)
        if (strcmp(list->items[i].path, path) == 0)
            return i;
    return -1;
}
