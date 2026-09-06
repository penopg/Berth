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

typedef struct {
    char added[GROUPS_MAX][PROJECT_PATH_MAX];   // папки-группы
    int  added_count;
    char hidden[GROUPS_MAX][PROJECT_NAME_MAX];  // имена скрытых групп
    int  hidden_count;

    char   path[PROJECT_PATH_MAX];
    time_t mtime;   // чтобы заметить правку файла руками
} Groups;

void groups_load(Groups *g, const char *path);
bool groups_save(const Groups *g);
bool groups_changed(const Groups *g);

// Папка становится группой с именем по последнему звену пути. Повтор не
// добавляется. Возвращает имя группы в name (может быть NULL).
bool groups_add(Groups *g, const char *dir, char *name, size_t cap);
void groups_hide(Groups *g, const char *name);
void groups_unhide(Groups *g, const char *name);
bool groups_is_hidden(const Groups *g, const char *name);

// Накладывает группы на список: сначала дописывает проекты из папок-групп,
// затем убирает всё, что относится к скрытым. Порядок важен — скрыть можно и
// свою папку.
void groups_apply(const Groups *g, ProjectList *list);

#endif // BERTH_GROUPS_H
