// Список проектов: что показывать в панели и откуда открывать вкладки.
//
// Источник — projects.json от системы вкладок Warp: там уже есть имя, путь,
// группа, цвет, иконка и тема. Общий файл держит Warp, WezTerm и этот
// терминал в одной картине мира, поэтому свой формат заводить рано.
#ifndef BERTH_PROJECTS_H
#define BERTH_PROJECTS_H

#include <stdbool.h>
#include <time.h>

#include "raylib.h"

#define PROJECT_MAX 128
#define PROJECT_NAME_MAX 64
#define PROJECT_PATH_MAX 512

typedef struct {
    char  name[PROJECT_NAME_MAX];
    char  path[PROJECT_PATH_MAX];
    char  group[PROJECT_NAME_MAX];
    char  icon[8];                  // глиф Nerd Font; в текущем атласе его нет
    char  theme[PROJECT_NAME_MAX];
    Color color;

    // Подпроект: индекс родителя в списке, -1 у обычного проекта.
    // См. subprojects.h. subs_mtime — время правки файла исключений
    // родителя, чтобы заметить правку руками.
    int    parent;
    time_t subs_mtime;
    time_t dir_mtime;   // время правки самой папки: новая подпапка меняет его
} Project;

typedef struct {
    Project items[PROJECT_MAX];
    int     count;

    char   source[PROJECT_PATH_MAX];
    time_t mtime;                   // чтобы замечать правку файла без перезапуска
} ProjectList;

// Путь по умолчанию: ~/.claude/warp-tabs/projects.json.
const char *projects_default_path(void);

// Читает список. При ошибке оставляет список пустым и возвращает false —
// это не повод не запускаться, терминал работает и без списка проектов.
bool projects_load(ProjectList *list, const char *path);

// Файл изменился с момента загрузки.
bool projects_changed(const ProjectList *list);

// Индекс проекта с таким путём, либо -1.
int projects_find_by_path(const ProjectList *list, const char *path);

#endif // BERTH_PROJECTS_H
