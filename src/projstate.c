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
    tasks_load(&st->tasks, cwd);
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

void projstate_poll(void)
{
    for (int i = 0; i < PROJSTATE_MAX; i++) {
        ProjectState *st = &g_states[i];
        if (!st->cwd[0]) continue;
        if (tasks_changed(&st->tasks, st->cwd))
            tasks_load(&st->tasks, st->cwd);
        projinfo_refresh_summary(&st->info);
    }
}
