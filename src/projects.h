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

    // Сколько записей таблицы ждут человека (фильтр в shown.tsv) и как это
    // назвать: панель пишет «· 3 не разобрано» у имени. Заполняет опрос.
    int    pending;
    char   pending_label[48];
} Project;

#define PROJECT_PINNED_GROUPS_MAX 8

// Группа с видом (см. `Groups.kind`): интеграции и всё, что появится
// такого же рода. В панели такие стоят **первыми** и рисуются на своей
// подложке — это оснастка, а не рабочие проекты, и путать их не надо. И
// показываются даже без проектов: заголовок группы рождается из первого её
// проекта, а группе интеграций надо быть видной до того, как в ней что-то
// появится. Заполняет groups_apply, читает layout.
typedef struct {
    char  name[PROJECT_NAME_MAX];
    Color color;
    bool  empty;    // проектов нет — заголовок всё равно нужен
} PinnedGroup;

typedef struct {
    Project items[PROJECT_MAX];
    int     count;

    PinnedGroup pinned[PROJECT_PINNED_GROUPS_MAX];
    int         pinned_count;

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
