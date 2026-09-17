#include "alltasks.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// Один список на процесс: файл читается целиком тем же разбором, что у
// страницы проекта, и тут же ужимается до заголовков. 1.1 МБ, но один.
static TaskList g_scratch;

static void stamp_of(const char *cwd, time_t *mtime, long *ns)
{
    char path[PROJECT_PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/%s", cwd, TASKS_FILE);
    struct stat st;
    if (stat(path, &st) != 0) { *mtime = 0; *ns = 0; return; }
    *mtime = st.st_mtime;
    *ns = (long)st.st_mtimespec.tv_nsec;
}

static void load_entry(AllTasksEntry *e, time_t mtime, long ns)
{
    e->mtime = mtime;
    e->mtime_ns = ns;
    e->count = e->open = e->review = 0;
    if (!mtime) return;

    tasks_load(&g_scratch, e->path);
    for (int i = 0; i < g_scratch.count; i++) {
        const Task *t = &g_scratch.items[i];
        if (t->state == TASK_DONE) continue;
        if (t->state == TASK_OPEN) e->open++; else e->review++;
        if (e->count >= ALLTASKS_PER_ENTRY) continue;
        AllTask *a = &e->items[e->count++];
        snprintf(a->title, sizeof(a->title), "%s", t->title);
        a->state = t->state;
        a->index = i;
        a->due_day = t->due_day;
    }
}

static void sync_entry(AllTasksEntry *e, const char *path, const char *name,
                       const char *group, bool is_group, Color color)
{
    e->color = color;
    time_t mtime; long ns;
    stamp_of(path, &mtime, &ns);
    bool moved = strcmp(e->path, path) != 0;
    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->group, sizeof(e->group), "%s", group ? group : "");
    e->is_group = is_group;
    if (moved || e->mtime != mtime || e->mtime_ns != ns) load_entry(e, mtime, ns);
}

void alltasks_refresh(AllTasks *a, const ProjectList *projects,
                      const AllTasksRoot *roots, int nroots)
{
    int n = 0;
    for (int i = 0; i < projects->count && n < ALLTASKS_MAX; i++, n++) {
        const Project *p = &projects->items[i];
        sync_entry(&a->items[n], p->path, p->name, p->group, false, p->color);
    }
    a->projects = n;
    for (int i = 0; i < nroots && n < ALLTASKS_MAX; i++, n++)
        sync_entry(&a->items[n], roots[i].path, roots[i].name, roots[i].name, true,
                   (Color){ 0, 0, 0, 0 });
    a->count = n;

    a->open = a->review = a->due_soon = 0;
    long today = tasks_day_today();
    for (int i = 0; i < n; i++) {
        const AllTasksEntry *e = &a->items[i];
        a->open += e->open;
        a->review += e->review;
        for (int k = 0; k < e->count; k++)
            if (e->items[k].due_day && e->items[k].state == TASK_OPEN
                && e->items[k].due_day - today <= 1)
                a->due_soon++;
    }
}
