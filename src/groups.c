#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "groups.h"

static void trim_line(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = '\0';
}

// Последнее звено пути — имя группы. Хвостовой слэш не в счёт.
static const char *base_name(const char *path)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    const char *p = path + n;
    while (p > path && p[-1] != '/') p--;
    return p;
}

static void group_name(const char *dir, char *out, size_t cap)
{
    const char *b = base_name(dir);
    size_t n = strlen(b);
    while (n > 0 && b[n - 1] == '/') n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n);
    out[n] = '\0';
}

void groups_load(Groups *g, const char *path)
{
    memset(g, 0, sizeof(*g));
    if (!path) return;
    snprintf(g->path, sizeof(g->path), "%s", path);

    struct stat st;
    g->mtime = stat(path, &st) == 0 ? st.st_mtime : 0;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[PROJECT_PATH_MAX + 16];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        trim_line(line);
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        if (!*tab) continue;

        if (!strcmp(line, "add") && g->added_count < GROUPS_MAX)
            snprintf(g->added[g->added_count++], PROJECT_PATH_MAX, "%s", tab);
        else if (!strcmp(line, "hide") && g->hidden_count < GROUPS_MAX)
            snprintf(g->hidden[g->hidden_count++], PROJECT_NAME_MAX, "%s", tab);
    }
    fclose(f);
}

bool groups_save(const Groups *g)
{
    if (!g->path[0]) return false;

    char tmp[PROJECT_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g->path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;

    fprintf(f, "# группы проектов berth. add <папка> — её подпапки становятся\n"
               "# проектами группы; hide <имя> — группа скрыта из панели.\n"
               "# Файл правится и руками, изменения подхватываются на лету.\n");
    for (int i = 0; i < g->added_count; i++)
        fprintf(f, "add\t%s\n", g->added[i]);
    for (int i = 0; i < g->hidden_count; i++)
        fprintf(f, "hide\t%s\n", g->hidden[i]);
    fclose(f);

    if (rename(tmp, g->path) != 0) return false;
    struct stat st;
    ((Groups *)g)->mtime = stat(g->path, &st) == 0 ? st.st_mtime : 0;
    return true;
}

bool groups_changed(const Groups *g)
{
    if (!g->path[0]) return false;
    struct stat st;
    time_t now = stat(g->path, &st) == 0 ? st.st_mtime : 0;
    return now != g->mtime;
}

bool groups_add(Groups *g, const char *dir, char *name, size_t cap)
{
    if (!dir || !*dir) return false;
    char clean[PROJECT_PATH_MAX];
    snprintf(clean, sizeof(clean), "%s", dir);
    size_t n = strlen(clean);
    while (n > 1 && clean[n - 1] == '/') clean[--n] = '\0';

    char gname[PROJECT_NAME_MAX];
    group_name(clean, gname, sizeof(gname));
    if (name && cap) snprintf(name, cap, "%s", gname);

    // Добавили — значит, хотят видеть: если такая группа была скрыта,
    // скрытие снимается, иначе кнопка молча ничего не сделает. И для уже
    // известной папки тоже — повторный выбор той же папки и есть «верни».
    groups_unhide(g, gname);

    for (int i = 0; i < g->added_count; i++)
        if (!strcmp(g->added[i], clean)) return true;   // уже есть — не ошибка
    if (g->added_count >= GROUPS_MAX) return false;
    snprintf(g->added[g->added_count++], PROJECT_PATH_MAX, "%s", clean);
    return true;
}

bool groups_is_hidden(const Groups *g, const char *name)
{
    for (int i = 0; i < g->hidden_count; i++)
        if (!strcmp(g->hidden[i], name)) return true;
    return false;
}

void groups_hide(Groups *g, const char *name)
{
    if (!name || !*name || groups_is_hidden(g, name)) return;
    if (g->hidden_count >= GROUPS_MAX) return;
    snprintf(g->hidden[g->hidden_count++], PROJECT_NAME_MAX, "%s", name);
}

void groups_unhide(Groups *g, const char *name)
{
    for (int i = 0; i < g->hidden_count; i++) {
        if (strcmp(g->hidden[i], name)) continue;
        g->hidden_count--;
        memmove(&g->hidden[i], &g->hidden[i + 1],
                sizeof(g->hidden[0]) * (size_t)(g->hidden_count - i));
        return;
    }
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Подпапки одной папки-группы — проектами. Скрытые каталоги пропускаем;
// что уже есть в списке под другой группой, не дублируем: у проекта одно
// место в панели.
static void add_folder_group(const char *dir, ProjectList *list)
{
    DIR *d = opendir(dir);
    if (!d) return;

    char group[PROJECT_NAME_MAX];
    group_name(dir, group, sizeof(group));

    // Цвета по кругу: без своего цвета группа читалась бы серой полосой.
    static const Color palette[] = {
        {120, 170, 245, 255}, {120, 200, 210, 255}, {200, 140, 220, 255},
        {130, 200, 150, 255}, {225, 190,  95, 255}, {235, 130, 130, 255},
    };
    const int palette_n = (int)(sizeof(palette) / sizeof(palette[0]));

    // Сначала собираем имена и сортируем: порядок readdir случаен, а панель
    // не должна перемешиваться от запуска к запуску.
    char names[PROJECT_MAX][PROJECT_NAME_MAX];
    char *sorted[PROJECT_MAX];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < PROJECT_MAX) {
        if (e->d_name[0] == '.') continue;
        char path[PROJECT_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        snprintf(names[n], PROJECT_NAME_MAX, "%s", e->d_name);
        sorted[n] = names[n];
        n++;
    }
    closedir(d);
    qsort(sorted, (size_t)n, sizeof(sorted[0]), by_name);

    for (int i = 0; i < n && list->count < PROJECT_MAX; i++) {
        char path[PROJECT_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, sorted[i]);
        if (projects_find_by_path(list, path) >= 0) continue;

        Project *p = &list->items[list->count++];
        memset(p, 0, sizeof(*p));
        p->parent = -1;
        snprintf(p->name, sizeof(p->name), "%s", sorted[i]);
        snprintf(p->path, sizeof(p->path), "%s", path);
        snprintf(p->group, sizeof(p->group), "%s", group);
        p->color = palette[i % palette_n];
    }
}

void groups_apply(const Groups *g, ProjectList *list)
{
    for (int i = 0; i < g->added_count; i++)
        add_folder_group(g->added[i], list);

    if (g->hidden_count == 0) return;
    int kept = 0;
    for (int i = 0; i < list->count; i++) {
        if (groups_is_hidden(g, list->items[i].group)) continue;
        if (kept != i) list->items[kept] = list->items[i];
        kept++;
    }
    list->count = kept;
}
