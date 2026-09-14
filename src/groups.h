// Группы проектов, заведённые рукой: папка, чьи подпапки — проекты.
//
// Основной список проектов приходит из projects.json генератора вкладок, и
// править его берт не вправе — файл перезаписывается скриптом. Поэтому свои
// группы берт держит отдельно, в `~/.config/berth/groups.tsv`, и накладывает
// на список при каждом чтении: добавляет проекты из выбранных папок и
// убирает группы, которые человек скрыл.
//
// Скрыть можно любую группу, и свою, и из projects.json. Скрытие — только
// про панель: на диске ничего не меняется, а вернуть группу можно на экране
// настроек.
#ifndef BERTH_GROUPS_H
#define BERTH_GROUPS_H

#include <stdbool.h>
#include <time.h>

#include "projects.h"

#define GROUPS_MAX 32
#define GROUP_KIND_MAX 24

// Вид группы, у которой «+» подключает интеграцию, а не заводит проект.
#define GROUP_KIND_INTEGRATIONS "интеграции"

typedef struct {
    char added[GROUPS_MAX][PROJECT_PATH_MAX];   // папки-группы
    // Вид группы — третье поле строки `add`. Пусто у обычной; «интеграции»
    // значит, что «+» у заголовка заводит не проект, а подключение, и что
    // группа видна в панели даже пустой. Вид — данные, а не имя папки:
    // папку переименуют, а развилка в коде по имени молча исчезнет.
    char kind[GROUPS_MAX][GROUP_KIND_MAX];
    int  added_count;
    char hidden[GROUPS_MAX][PROJECT_NAME_MAX];  // имена скрытых групп
    int  hidden_count;

    // Скрытые поодиночке проекты — путями, а не именами: имя папки в разных
    // группах повторяется, а путь единственный. Скрытие только про панель:
    // на диске ничего не меняется, вернуть можно на экране настроек.
    char hidden_proj[GROUPS_MAX][PROJECT_PATH_MAX];
    int  hidden_proj_count;

    // Имена, заданные человеком, — путями: у группы это её папка, у проекта
    // — его каталог. Имя только для панели: папка на диске не меняется, и
    // история Claude Code остаётся при ней. Нет записи — имя по последнему
    // звену пути, как раньше.
    char name_path[GROUPS_MAX][PROJECT_PATH_MAX];
    char name_text[GROUPS_MAX][PROJECT_NAME_MAX];
    int  name_count;

    char   path[PROJECT_PATH_MAX];
    time_t mtime;   // чтобы заметить правку файла руками
} Groups;

void groups_load(Groups *g, const char *path);
bool groups_save(const Groups *g);
bool groups_changed(const Groups *g);

// Папка становится группой с именем по последнему звену пути. Повтор не
// добавляется. Возвращает имя группы в name (может быть NULL).
bool groups_add(Groups *g, const char *dir, char *name, size_t cap);

// То же, но с видом группы (см. Groups.kind). Пустой вид — как groups_add.
bool groups_add_kind(Groups *g, const char *dir, const char *kind,
                     char *name, size_t cap);

// Вид группы по её имени (последнему звену пути). Никогда не NULL: у
// обычной группы — пустая строка.
const char *groups_kind_of(const Groups *g, const char *name);

// Есть ли среди добавленных папок группа такого вида.
bool groups_has_kind(const Groups *g, const char *kind);

// Переносит вид на другую папку: у неё он появляется, у прежней снимается.
// Прежняя папка без подпапок уходит из списка вовсе (это была пустая
// заготовка), с подпапками — остаётся обычной группой: на диске ничего не
// двигается, и проекты в ней не должны пропасть из панели. Имя новой группы
// — в name (может быть NULL).
bool groups_set_kind_dir(Groups *g, const char *kind, const char *dir,
                         char *name, size_t cap);
// Своё имя у пути (папки-группы или проекта). Пустое имя снимает
// переименование — имя снова по папке.
bool groups_set_name(Groups *g, const char *path, const char *name);
// Заданное человеком имя у пути или NULL.
const char *groups_name_of(const Groups *g, const char *path);
// Папка группы по её имени в панели. Только у групп-папок.
bool groups_dir_of(const Groups *g, const char *name, char *out, size_t cap);
// Переименования проектов поверх готового списка — после подпроектов, они
// добавляются позже groups_apply.
void groups_apply_names(const Groups *g, ProjectList *list);

void groups_hide(Groups *g, const char *name);
void groups_hide_project(Groups *g, const char *path);
void groups_unhide_project(Groups *g, const char *path);
void groups_unhide(Groups *g, const char *name);
bool groups_is_hidden(const Groups *g, const char *name);

// Накладывает группы на список: сначала дописывает проекты из папок-групп,
// затем убирает всё, что относится к скрытым. Порядок важен — скрыть можно и
// свою папку.
void groups_apply(const Groups *g, ProjectList *list);

#endif // BERTH_GROUPS_H
