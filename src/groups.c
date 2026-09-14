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

static void group_name(const Groups *g, const char *dir, char *out, size_t cap)
{
    const char *own = g ? groups_name_of(g, dir) : NULL;
    if (own) { snprintf(out, cap, "%s", own); return; }
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

    char line[PROJECT_PATH_MAX + PROJECT_NAME_MAX + 16];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        trim_line(line);
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        if (!*tab) continue;

        if (!strcmp(line, "add") && g->added_count < GROUPS_MAX) {
            // Третье поле — вид группы, и его может не быть: файлы, писанные
            // до появления видов, читаются как обычные группы.
            char *kind = strchr(tab, '\t');
            if (kind) *kind++ = '\0';
            snprintf(g->kind[g->added_count], GROUP_KIND_MAX, "%s", kind ? kind : "");
            snprintf(g->added[g->added_count++], PROJECT_PATH_MAX, "%s", tab);
        }
        else if (!strcmp(line, "hide") && g->hidden_count < GROUPS_MAX)
            snprintf(g->hidden[g->hidden_count++], PROJECT_NAME_MAX, "%s", tab);
        else if (!strcmp(line, "hide-project") && g->hidden_proj_count < GROUPS_MAX)
            snprintf(g->hidden_proj[g->hidden_proj_count++], PROJECT_PATH_MAX,
                     "%s", tab);
        else if (!strcmp(line, "name") && g->name_count < GROUPS_MAX) {
            char *text = strchr(tab, '\t');
            if (!text) continue;
            *text++ = '\0';
            if (!*text) continue;
            snprintf(g->name_path[g->name_count], PROJECT_PATH_MAX, "%s", tab);
            snprintf(g->name_text[g->name_count++], PROJECT_NAME_MAX, "%s", text);
        }
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

    fprintf(f, "# группы проектов berth. add <папка> [вид] — её подпапки\n"
               "# становятся проектами группы; hide <имя> — группа скрыта из панели;\n"
               "# hide-project <путь> — один проект убран из списка;\n"
               "# name <путь> <имя> — своё имя группы или проекта в панели.\n"
               "# Файл правится и руками, изменения подхватываются на лету.\n");
    for (int i = 0; i < g->added_count; i++) {
        if (g->kind[i][0]) fprintf(f, "add\t%s\t%s\n", g->added[i], g->kind[i]);
        else               fprintf(f, "add\t%s\n", g->added[i]);
    }
    for (int i = 0; i < g->hidden_count; i++)
        fprintf(f, "hide\t%s\n", g->hidden[i]);
    for (int i = 0; i < g->hidden_proj_count; i++)
        fprintf(f, "hide-project\t%s\n", g->hidden_proj[i]);
    for (int i = 0; i < g->name_count; i++)
        fprintf(f, "name\t%s\t%s\n", g->name_path[i], g->name_text[i]);
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
    return groups_add_kind(g, dir, NULL, name, cap);
}

bool groups_add_kind(Groups *g, const char *dir, const char *kind,
                     char *name, size_t cap)
{
    if (!dir || !*dir) return false;
    char clean[PROJECT_PATH_MAX];
    snprintf(clean, sizeof(clean), "%s", dir);
    size_t n = strlen(clean);
    while (n > 1 && clean[n - 1] == '/') clean[--n] = '\0';

    char gname[PROJECT_NAME_MAX];
    group_name(g, clean, gname, sizeof(gname));
    if (name && cap) snprintf(name, cap, "%s", gname);

    // Добавили — значит, хотят видеть: если такая группа была скрыта,
    // скрытие снимается, иначе кнопка молча ничего не сделает. И для уже
    // известной папки тоже — повторный выбор той же папки и есть «верни».
    groups_unhide(g, gname);

    // И поштучные скрытия внутри этой папки — по той же причине: выбрать
    // папку заново это и есть «покажи мне её содержимое целиком». Иначе
    // убранный из списка проект не вернулся бы никаким действием в окне.
    for (int i = g->hidden_proj_count - 1; i >= 0; i--)
        if (!strncmp(g->hidden_proj[i], clean, n) && g->hidden_proj[i][n] == '/')
            groups_unhide_project(g, g->hidden_proj[i]);

    for (int i = 0; i < g->added_count; i++) {
        if (strcmp(g->added[i], clean)) continue;
        // Уже есть — не ошибка. Вид дописываем, если его просят и его нет:
        // папку могли добавить руками раньше, чем у групп появились виды.
        if (kind && *kind && !g->kind[i][0])
            snprintf(g->kind[i], GROUP_KIND_MAX, "%s", kind);
        return true;
    }
    if (g->added_count >= GROUPS_MAX) return false;
    snprintf(g->kind[g->added_count], GROUP_KIND_MAX, "%s", kind ? kind : "");
    snprintf(g->added[g->added_count++], PROJECT_PATH_MAX, "%s", clean);
    return true;
}

static bool dir_has_subdirs(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[PROJECT_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        found = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }
    closedir(d);
    return found;
}

bool groups_set_kind_dir(Groups *g, const char *kind, const char *dir,
                         char *name, size_t cap)
{
    if (!kind || !*kind || !dir || !*dir) return false;
    char clean[PROJECT_PATH_MAX];
    snprintf(clean, sizeof(clean), "%s", dir);
    size_t n = strlen(clean);
    while (n > 1 && clean[n - 1] == '/') clean[--n] = '\0';

    for (int i = 0; i < g->added_count; i++) {
        if (strcmp(g->kind[i], kind)) continue;
        if (!strcmp(g->added[i], clean)) break;   // та же папка — менять нечего
        g->kind[i][0] = '\0';
        if (!dir_has_subdirs(g->added[i])) {
            for (int j = i; j + 1 < g->added_count; j++) {
                snprintf(g->added[j], PROJECT_PATH_MAX, "%s", g->added[j + 1]);
                snprintf(g->kind[j], GROUP_KIND_MAX, "%s", g->kind[j + 1]);
            }
            g->added_count--;
        }
        break;
    }
    return groups_add_kind(g, clean, kind, name, cap);
}

const char *groups_kind_of(const Groups *g, const char *name)
{
    if (!name || !*name) return "";
    for (int i = 0; i < g->added_count; i++) {
        // Через group_name, а не base_name: в файле, поправленном руками,
        // у пути бывает хвостовой слэш, и сравнение имён на нём разъедется.
        char gname[PROJECT_NAME_MAX];
        group_name(g, g->added[i], gname, sizeof(gname));
        if (!strcmp(gname, name)) return g->kind[i];
    }
    return "";
}

bool groups_dir_of(const Groups *g, const char *name, char *out, size_t cap)
{
    if (!name || !*name) return false;
    for (int i = 0; i < g->added_count; i++) {
        char gname[PROJECT_NAME_MAX];
        group_name(g, g->added[i], gname, sizeof(gname));
        if (strcmp(gname, name)) continue;
        snprintf(out, cap, "%s", g->added[i]);
        return true;
    }
    return false;
}

const char *groups_name_of(const Groups *g, const char *path)
{
    if (!g || !path || !*path) return NULL;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;   // хвостовой слэш из файла руками
    for (int i = 0; i < g->name_count; i++)
        if (strlen(g->name_path[i]) == n && !strncmp(g->name_path[i], path, n))
            return g->name_text[i];
    return NULL;
}

bool groups_set_name(Groups *g, const char *path, const char *name)
{
    if (!g || !path || !*path) return false;
    char clean[PROJECT_PATH_MAX];
    snprintf(clean, sizeof(clean), "%s", path);
    size_t n = strlen(clean);
    while (n > 1 && clean[n - 1] == '/') clean[--n] = '\0';

    char text[PROJECT_NAME_MAX];
    snprintf(text, sizeof(text), "%s", name ? name : "");
    size_t t = strlen(text);
    while (t > 0 && (text[t - 1] == ' ' || text[t - 1] == '\t')) text[--t] = '\0';
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;

    int at = -1;
    for (int i = 0; i < g->name_count; i++)
        if (!strcmp(g->name_path[i], clean)) { at = i; break; }

    if (!*p) {
        // Пустое имя — «верни имя папки»: запись убирается, а не пишется пустой.
        if (at < 0) return true;
        for (int i = at; i + 1 < g->name_count; i++) {
            memcpy(g->name_path[i], g->name_path[i + 1], PROJECT_PATH_MAX);
            memcpy(g->name_text[i], g->name_text[i + 1], PROJECT_NAME_MAX);
        }
        g->name_count--;
        return true;
    }
    if (at < 0) {
        if (g->name_count >= GROUPS_MAX) return false;
        at = g->name_count++;
        snprintf(g->name_path[at], PROJECT_PATH_MAX, "%s", clean);
    }
    snprintf(g->name_text[at], PROJECT_NAME_MAX, "%s", p);
    return true;
}

void groups_apply_names(const Groups *g, ProjectList *list)
{
    if (!g || !list) return;
    for (int i = 0; i < list->count; i++) {
        const char *own = groups_name_of(g, list->items[i].path);
        if (own) snprintf(list->items[i].name, sizeof(list->items[i].name), "%s", own);
    }
}

bool groups_has_kind(const Groups *g, const char *kind)
{
    if (!kind || !*kind) return false;
    for (int i = 0; i < g->added_count; i++)
        if (!strcmp(g->kind[i], kind)) return true;
    return false;
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

// Убрать один проект из списка. Путём, а не именем: имя папки повторяется
// в разных группах, а путь единственный. На диске ничего не меняется —
// это ровно то же скрытие, что у группы, только поштучное.
void groups_hide_project(Groups *g, const char *path)
{
    if (!path || !*path || g->hidden_proj_count >= GROUPS_MAX) return;
    for (int i = 0; i < g->hidden_proj_count; i++)
        if (!strcmp(g->hidden_proj[i], path)) return;
    snprintf(g->hidden_proj[g->hidden_proj_count++], PROJECT_PATH_MAX, "%s", path);
}

void groups_unhide_project(Groups *g, const char *path)
{
    for (int i = 0; i < g->hidden_proj_count; i++) {
        if (strcmp(g->hidden_proj[i], path)) continue;
        for (int j = i; j + 1 < g->hidden_proj_count; j++)
            snprintf(g->hidden_proj[j], PROJECT_PATH_MAX, "%s", g->hidden_proj[j + 1]);
        g->hidden_proj_count--;
        return;
    }
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
static void add_folder_group(const Groups *g, const char *dir, ProjectList *list)
{
    DIR *d = opendir(dir);
    if (!d) return;

    char group[PROJECT_NAME_MAX];
    group_name(g, dir, group, sizeof(group));

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

// Есть ли в списке хоть один проект такой группы.
static bool group_has_projects(const ProjectList *list, const char *group)
{
    for (int i = 0; i < list->count; i++)
        if (!strcmp(list->items[i].group, group)) return true;
    return false;
}

// Проекты группы — в начало списка, порядок между собой сохраняя. Панель
// рисует их в порядке списка, поэтому «первым разделом» группа становится
// здесь, а не в раскладке.
static void move_group_first(ProjectList *list, const char *group, int *at)
{
    for (int i = *at; i < list->count; i++) {
        if (strcmp(list->items[i].group, group)) continue;
        if (i > *at) {
            Project tmp = list->items[i];
            memmove(&list->items[*at + 1], &list->items[*at],
                    sizeof(Project) * (size_t)(i - *at));
            list->items[*at] = tmp;
        }
        (*at)++;
    }
}

void groups_apply(const Groups *g, ProjectList *list)
{
    list->pinned_count = 0;

    for (int i = 0; i < g->added_count; i++)
        add_folder_group(g, g->added[i], list);

    if (g->hidden_count > 0) {
        int kept = 0;
        for (int i = 0; i < list->count; i++) {
            if (groups_is_hidden(g, list->items[i].group)) continue;
            if (kept != i) list->items[kept] = list->items[i];
            kept++;
        }
        list->count = kept;
    }

    // Второй проход, тем же способом: поштучно скрытые проекты. Отдельно от
    // групп, потому что группа сравнивается по имени, а проект по пути.
    if (g->hidden_proj_count > 0) {
        int kept = 0;
        for (int i = 0; i < list->count; i++) {
            bool hide = false;
            for (int j = 0; j < g->hidden_proj_count && !hide; j++)
                hide = !strcmp(g->hidden_proj[j], list->items[i].path);
            if (hide) continue;
            if (kept != i) list->items[kept] = list->items[i];
            kept++;
        }
        list->count = kept;
    }

    // Группы с видом — последним проходом, когда список уже отфильтрован:
    // группа, из которой убрали единственный проект, тоже пуста. Здесь же
    // их проекты уезжают в начало списка — в панели они идут первыми.
    int at = 0;
    for (int i = 0; i < g->added_count; i++) {
        if (!g->kind[i][0]) continue;
        if (list->pinned_count >= PROJECT_PINNED_GROUPS_MAX) break;

        char name[PROJECT_NAME_MAX];
        group_name(g, g->added[i], name, sizeof(name));
        if (groups_is_hidden(g, name)) continue;

        PinnedGroup *pg = &list->pinned[list->pinned_count++];
        snprintf(pg->name, sizeof(pg->name), "%s", name);
        pg->color = (Color){ 120, 170, 245, 255 };
        pg->empty = !group_has_projects(list, name);
        if (!pg->empty) move_group_first(list, name, &at);
    }
}
