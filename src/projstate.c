#include <stdio.h>
#include <string.h>

#include "projstate.h"

// Сколько проектов держим прочитанными. Набор весит около 170 КБ, а держать
// их сотнями незачем: человек работает с единицами, остальные читаются заново
// за десятки миллисекунд.
#define PROJSTATE_MAX 8

static ProjectState g_states[PROJSTATE_MAX];

static ProjectState *find(const char *cwd)
{
    for (int i = 0; i < PROJSTATE_MAX; i++)
        if (g_states[i].cwd[0] && !strcmp(g_states[i].cwd, cwd))
            return &g_states[i];
    return NULL;
}

// Свободное место, а если его нет — набор, к которому дольше всех не
// обращались. Вытеснять по времени обращения, а не по времени чтения: проект,
// открытый вчера и с тех пор не тронутый, нужнее не тому, кто в нём работает.
static ProjectState *slot_for(const char *cwd)
{
    ProjectState *oldest = &g_states[0];
    for (int i = 0; i < PROJSTATE_MAX; i++) {
        if (!g_states[i].cwd[0]) return &g_states[i];
        if (g_states[i].touched < oldest->touched) oldest = &g_states[i];
    }
    (void)cwd;
    return oldest;
}

static void load(ProjectState *st, const char *cwd)
{
    memset(st, 0, sizeof(*st));
    snprintf(st->cwd, sizeof(st->cwd), "%s", cwd);
    projinfo_load(&st->info, cwd);
    journal_load(&st->journal, cwd);
    st->journal_mtime = journal_file_mtime(cwd);
    tasks_load(&st->tasks, cwd);
    files_load(&st->files, cwd);
    for (int i = 0; i < st->files.shown_count; i++)
        table_load(&st->tables[i], cwd, st->files.shown[i]);
    st->table_count = st->files.shown_count;
}

ProjectState *projstate_edit(const char *cwd)
{
    return (ProjectState *)projstate_get(cwd);
}

const ProjectState *projstate_get(const char *cwd)
{
    if (!cwd || !*cwd) return NULL;

    ProjectState *st = find(cwd);
    if (!st) {
        st = slot_for(cwd);
        load(st, cwd);
    }
    st->touched = time(NULL);
    return st;
}

const ProjectState *projstate_peek(const char *cwd)
{
    if (!cwd || !*cwd) return NULL;
    return find(cwd);
}

void projstate_reload(const char *cwd)
{
    if (!cwd || !*cwd) return;

    ProjectState *st = find(cwd);
    if (!st) st = slot_for(cwd);
    load(st, cwd);
    st->touched = time(NULL);
}

const SkillList *projstate_skills(const char *cwd, const char *parent, bool force)
{
    ProjectState *st = (ProjectState *)projstate_get(cwd);
    if (!st) return NULL;
    const char *want = parent ? parent : "";
    bool other_parent = strcmp(st->skills.parent, want) != 0;
    if (force || !st->skills.cwd[0] || other_parent || skills_changed(&st->skills))
        skills_load(&st->skills, cwd, parent);
    return &st->skills;
}

// Что показано на странице, решает реестр; таблицы под него подстраиваются.
// Путь сменился или файл правили — перечитываем, иначе не трогаем: разбор
// стоит чтения файла, а опрос идёт раз в пару секунд.
static void sync_tables(ProjectState *st)
{
    const FileList *fl = &st->files;
    for (int i = 0; i < fl->shown_count; i++) {
        if (strcmp(st->tables[i].path, fl->shown[i]))
            table_load(&st->tables[i], st->cwd, fl->shown[i]);
        else if (table_changed(&st->tables[i], st->cwd))
            table_load(&st->tables[i], st->cwd, fl->shown[i]);
    }
    for (int i = fl->shown_count; i < FILES_SHOWN_MAX; i++)
        if (st->tables[i].path[0]) memset(&st->tables[i], 0, sizeof(st->tables[i]));
    st->table_count = fl->shown_count;
}

void projstate_sync_tables(const char *cwd)
{
    ProjectState *st = projstate_edit(cwd);
    if (st) sync_tables(st);
}

void projstate_poll(void)
{
    for (int i = 0; i < PROJSTATE_MAX; i++) {
        ProjectState *st = &g_states[i];
        if (!st->cwd[0]) continue;
        if (tasks_changed(&st->tasks, st->cwd))
            tasks_load(&st->tasks, st->cwd);
        // Реестр документов дописывает агент (задача «Описать» или по ходу
        // работы): перечитывается по mtime, а файлы в нём — по stat.
        if (files_changed(&st->files, st->cwd))
            files_load(&st->files, st->cwd);
        else
            files_refresh(&st->files, st->cwd);
        sync_tables(st);
        projinfo_refresh_summary(&st->info);
        // Дневник дописал агент (задача сбора кончилась) или человек:
        // лента перечитывается сама, кнопка «Обновить» для этого не нужна.
        // История jsonl при этом дочитывается инкрементально — дёшево.
        time_t jm = journal_file_mtime(st->cwd);
        if (jm != st->journal_mtime) {
            journal_load(&st->journal, st->cwd);
            st->journal_mtime = jm;
        }
    }
}
