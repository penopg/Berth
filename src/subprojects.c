#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "subprojects.h"
#include "projinfo.h"

#define SUB_MAX 64

typedef struct {
    char added[SUB_MAX][PROJECT_NAME_MAX];
    int  added_count;
    char hidden[SUB_MAX][PROJECT_NAME_MAX];
    int  hidden_count;
} SubFile;

static void file_path(const char *project, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", project, SUBPROJECTS_FILE);
}

static time_t dir_mtime(const char *dir)
{
    struct stat st;
    return stat(dir, &st) == 0 ? st.st_mtime : 0;
}

static time_t file_mtime(const char *project)
{
    char path[PROJECT_PATH_MAX];
    file_path(project, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static bool in_list(char names[][PROJECT_NAME_MAX], int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], name)) return true;
    return false;
}

static void remove_from(char names[][PROJECT_NAME_MAX], int *n, const char *name)
{
    for (int i = 0; i < *n; i++) {
        if (strcmp(names[i], name)) continue;
        (*n)--;
        memmove(names[i], names[i + 1], PROJECT_NAME_MAX * (size_t)(*n - i));
        return;
    }
}

static void subfile_load(SubFile *sf, const char *project)
{
    memset(sf, 0, sizeof(*sf));
    char path[PROJECT_PATH_MAX];
    file_path(project, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[PROJECT_NAME_MAX + 32];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\r\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        if (!*tab || strchr(tab, '/')) continue;   // только имя папки, не путь
        if (!strcmp(line, "add") && sf->added_count < SUB_MAX && !in_list(sf->added, sf->added_count, tab))
            snprintf(sf->added[sf->added_count++], PROJECT_NAME_MAX, "%s", tab);
        else if (!strcmp(line, "hide") && sf->hidden_count < SUB_MAX && !in_list(sf->hidden, sf->hidden_count, tab))
            snprintf(sf->hidden[sf->hidden_count++], PROJECT_NAME_MAX, "%s", tab);
    }
    fclose(f);
}

static bool subfile_save(const SubFile *sf, const char *project)
{
    char dir[PROJECT_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.berth", project);
    mkdir(dir, 0755);

    char path[PROJECT_PATH_MAX], tmp[PROJECT_PATH_MAX + 8];
    file_path(project, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "# подпроекты berth: исключения к автоматическому поиску.\n"
               "# add <папка> — подпроект без признаков; hide <папка> — не подпроект.\n"
               "# Имена относительно папки проекта. Правится и руками.\n");
    for (int i = 0; i < sf->added_count; i++)  fprintf(f, "add\t%s\n", sf->added[i]);
    for (int i = 0; i < sf->hidden_count; i++) fprintf(f, "hide\t%s\n", sf->hidden[i]);
    fclose(f);
    return rename(tmp, path) == 0;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

// Признаки того, что в папке идёт своя работа. Один .git не считается:
// вложенный репозиторий чаще референс или зависимость, чем проект.
static bool looks_like_project(const char *path)
{
    char p[PROJECT_PATH_MAX + 32];
    snprintf(p, sizeof(p), "%s/CLAUDE.md", path);
    if (exists(p)) return true;
    snprintf(p, sizeof(p), "%s/.berth", path);
    if (is_dir(p)) return true;
    projinfo_session_dir(path, p, sizeof(p));
    return p[0] && is_dir(p);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Подпроекты одного проекта, отсортированные по имени. Возвращает число.
static int discover(const char *project, char names[][PROJECT_NAME_MAX], int cap)
{
    SubFile sf;
    subfile_load(&sf, project);

    DIR *d = opendir(project);
    if (!d) return 0;

    char found[SUB_MAX][PROJECT_NAME_MAX];
    char *sorted[SUB_MAX];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < SUB_MAX) {
        if (e->d_name[0] == '.') continue;
        if (in_list(sf.hidden, sf.hidden_count, e->d_name)) continue;
        char path[PROJECT_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", project, e->d_name);
        if (!is_dir(path)) continue;
        if (!in_list(sf.added, sf.added_count, e->d_name) && !looks_like_project(path))
            continue;
        snprintf(found[n], PROJECT_NAME_MAX, "%s", e->d_name);
        sorted[n] = found[n];
        n++;
    }
    closedir(d);
    qsort(sorted, (size_t)n, sizeof(sorted[0]), by_name);

    int out = 0;
    for (int i = 0; i < n && out < cap; i++)
        snprintf(names[out++], PROJECT_NAME_MAX, "%s", sorted[i]);
    return out;
}

void subprojects_apply(ProjectList *list)
{
    // Идём по индексам: вставка сдвигает хвост, но родитель остаётся на
    // месте, а вставленные подпроекты пропускаются по parent.
    for (int i = 0; i < list->count; i++) {
        Project *parent = &list->items[i];
        if (parent->parent >= 0) continue;
        parent->subs_mtime = file_mtime(parent->path);
        parent->dir_mtime  = dir_mtime(parent->path);

        char names[SUB_MAX][PROJECT_NAME_MAX];
        int n = discover(parent->path, names, SUB_MAX);
        int at = i + 1;
        for (int k = 0; k < n && list->count < PROJECT_MAX; k++) {
            char path[PROJECT_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", parent->path, names[k]);
            if (projects_find_by_path(list, path) >= 0) continue;   // уже проект

            memmove(&list->items[at + 1], &list->items[at],
                    sizeof(Project) * (size_t)(list->count - at));
            list->count++;
            Project *p = &list->items[at];
            memset(p, 0, sizeof(*p));
            snprintf(p->name, sizeof(p->name), "%s", names[k]);
            snprintf(p->path, sizeof(p->path), "%s", path);
            snprintf(p->group, sizeof(p->group), "%s", list->items[i].group);
            snprintf(p->theme, sizeof(p->theme), "%s", list->items[i].theme);
            p->color = list->items[i].color;
            p->parent = i;
            at++;
        }
    }
}

bool subprojects_changed(const ProjectList *list)
{
    for (int i = 0; i < list->count; i++) {
        const Project *p = &list->items[i];
        if (p->parent >= 0) continue;
        if (file_mtime(p->path) != p->subs_mtime) return true;
        // Новая папка с паспортом должна появиться без ⌘R: создание подпапки
        // меняет время правки родителя.
        if (dir_mtime(p->path) != p->dir_mtime) return true;
    }
    return false;
}

// Имя папки внутри проекта, если dir лежит прямо в нём.
static bool child_name(const char *project, const char *dir, char *out, size_t cap)
{
    char clean[PROJECT_PATH_MAX];
    snprintf(clean, sizeof(clean), "%s", dir);
    size_t n = strlen(clean);
    while (n > 1 && clean[n - 1] == '/') clean[--n] = '\0';

    size_t pl = strlen(project);
    if (strncmp(clean, project, pl) != 0 || clean[pl] != '/') return false;
    const char *rest = clean + pl + 1;
    if (!*rest || strchr(rest, '/')) return false;   // глубже одного уровня
    snprintf(out, cap, "%s", rest);
    return true;
}

bool subprojects_add(const char *project, const char *dir)
{
    char name[PROJECT_NAME_MAX];
    if (!child_name(project, dir, name, sizeof(name))) return false;

    SubFile sf;
    subfile_load(&sf, project);
    remove_from(sf.hidden, &sf.hidden_count, name);
    if (!in_list(sf.added, sf.added_count, name) && sf.added_count < SUB_MAX)
        snprintf(sf.added[sf.added_count++], PROJECT_NAME_MAX, "%s", name);
    return subfile_save(&sf, project);
}

bool subprojects_hide(const char *project, const char *name)
{
    if (!name || !*name) return false;
    SubFile sf;
    subfile_load(&sf, project);
    remove_from(sf.added, &sf.added_count, name);
    if (!in_list(sf.hidden, sf.hidden_count, name) && sf.hidden_count < SUB_MAX)
        snprintf(sf.hidden[sf.hidden_count++], PROJECT_NAME_MAX, "%s", name);
    return subfile_save(&sf, project);
}

// Подстановка {{KEY}} в строке шаблона.
static void subst(char *line, size_t cap, const char *key, const char *val)
{
    char *at;
    while ((at = strstr(line, key))) {
        char tail[1024];
        snprintf(tail, sizeof(tail), "%s", at + strlen(key));
        size_t head = (size_t)(at - line);
        snprintf(line + head, cap - head, "%s%s", val, tail);
    }
}

bool subprojects_create(const char *project, const char *name, const char *oneline,
                        char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    if (!name || !*name || name[0] == '.' || strchr(name, '/')) {
        if (err) snprintf(err, err_cap, "Имя — это имя папки: без «/», не с точки");
        return false;
    }

    char dir[PROJECT_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s", project, name);
    if (exists(dir)) {
        if (err) snprintf(err, err_cap, "«%s» уже есть в этой папке", name);
        return false;
    }
    if (mkdir(dir, 0755) != 0) {
        if (err) snprintf(err, err_cap, "Не удалось создать папку %s", dir);
        return false;
    }

    char date[16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);

    char passport[PROJECT_PATH_MAX + 16];
    snprintf(passport, sizeof(passport), "%s/CLAUDE.md", dir);
    FILE *out = fopen(passport, "w");
    if (!out) {
        if (err) snprintf(err, err_cap, "Папка создана, но паспорт записать не удалось");
        return false;
    }

    const char *home = getenv("HOME");
    char tpl_path[PROJECT_PATH_MAX];
    snprintf(tpl_path, sizeof(tpl_path), "%s/.claude/warp-tabs/template.md", home ? home : "");
    FILE *tpl = home ? fopen(tpl_path, "r") : NULL;
    if (tpl) {
        char line[1024];
        while (fgets(line, sizeof(line), tpl)) {
            subst(line, sizeof(line), "{{NAME}}", name);
            subst(line, sizeof(line), "{{ONELINE}}", oneline ? oneline : "");
            subst(line, sizeof(line), "{{DATE}}", date);
            fputs(line, out);
        }
        fclose(tpl);
    } else {
        fprintf(out, "# %s\n\n**Что это:** %s\n\n## Статус\n- заведён %s\n\n## Как работать\n-\n",
                name, oneline ? oneline : "", date);
    }
    fclose(out);
    return true;
}
