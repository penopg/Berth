// Скиллы проекта: что лежит в его .claude/skills и что приходит от родителя.
//
// Скилл для Claude Code — папка с SKILL.md. Он берёт их из ~/.claude/skills
// (личные, действуют везде), из <проект>/.claude/skills и тех же папок
// родительских каталогов, а ещё из каждой папки, переданной флагом
// --add-dir. Слои и хозяева у них разные:
// - скиллы берта — часть его пакета: лежат в бинаре, раскладываются в
//   папку берта и подключаются к каждому запуску claude через --add-dir
//   (claude.c). В проекты и в личную папку не пишутся;
// - личные — дело человека, берт их не трогает и показывает только счётом;
// - скиллы проекта — часть кода, уезжают с ним; это единственное, что
//   здесь показывается строками и заводится кнопкой;
// - от родителя — у подпроекта: действуют по правилам Claude Code, только
//   чтение.
// Одноимённый личный скилл перекрывает проектный — так устроен Claude Code,
// поэтому встроенные носят префикс berth-.
#ifndef BERTH_SKILLS_H
#define BERTH_SKILLS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "projects.h"

#define SKILLS_MAX       48
#define SKILL_NAME_MAX   64
#define SKILL_DESC_MAX   640
#define SKILL_INTRO_MAX  600

typedef enum {
    SKILL_OWN = 0,     // папка проекта
    SKILL_INHERITED,   // из .claude/skills родителя
} SkillKind;

typedef struct {
    char name[SKILL_NAME_MAX];
    char desc[SKILL_DESC_MAX];     // description из шапки, одной строкой
    char intro[SKILL_INTRO_MAX];   // первый абзац после шапки — чтобы вспомнить
    char path[PROJECT_PATH_MAX];   // папка скилла
    SkillKind kind;
    char from[PROJECT_NAME_MAX];   // у унаследованного — имя родителя
} Skill;

typedef struct {
    Skill items[SKILLS_MAX];
    int   count;
    int   personal_count;   // личных в ~/.claude/skills — действуют везде

    char   cwd[PROJECT_PATH_MAX];
    char   parent[PROJECT_PATH_MAX];
    time_t own_mtime, parent_mtime, personal_mtime;
    time_t checked;
} SkillList;

// Список для проекта cwd; parent — путь родителя у подпроекта, иначе NULL.
void skills_load(SkillList *sl, const char *cwd, const char *parent);

// Какая-то из папок переписана. По stat, не чаще раза в пару секунд.
bool skills_changed(SkillList *sl);

// Завести скилл в проекте: папка .claude/skills/<name> с SKILL.md по
// шаблону — шапка с описанием-триггером, «когда применять», «что делать».
bool skills_create(const char *cwd, const char *name, const char *when,
                   char *err, size_t err_cap);

// Описание встроенного скилла берта из его шапки — для экрана настроек.
void skills_builtin_desc(const char *skill_md, char *out, size_t cap);

#endif // BERTH_SKILLS_H
