// Подпроекты: папки внутри проекта, в которых идёт своя работа.
//
// Одна папка — один проект, но внутри неё бывают папки со своей историей
// Claude Code, своим паспортом и своими задачами. Такая папка — подпроект:
// в панели строкой под родителем, со своим разговором (история Claude Code
// привязана к каталогу), а паспорт и скиллы родителя она получает от самого
// Claude Code — он читает CLAUDE.md и .claude/skills вверх по дереву.
//
// Не всякая подпапка — подпроект. Признаки: история Claude Code в
// ~/.claude/projects, CLAUDE.md или .berth/ внутри. Поверх признаков —
// файл исключений в самом проекте, `<проект>/.berth/subprojects.tsv`:
//     add   <папка>   папка без признаков — тоже подпроект
//     hide  <папка>   папка с признаками — не подпроект
// Имена относительные: файл уезжает с кодом, а машины у людей разные.
// Автоматически найденные в файл не пишутся — он только про исключения.
#ifndef BERTH_SUBPROJECTS_H
#define BERTH_SUBPROJECTS_H

#include <stdbool.h>
#include <stddef.h>

#include "projects.h"

#define SUBPROJECTS_FILE ".berth/subprojects.tsv"

// Дописывает подпроекты в список: каждый — сразу за своим родителем, с его
// группой, цветом и темой, и с parent, указывающим на него. Проекты, уже
// стоящие в списке сами по себе, не дублируются.
void subprojects_apply(ProjectList *list);

// Файл исключений какого-то проекта переписан с момента наложения.
bool subprojects_changed(const ProjectList *list);

// Папка dir (внутри project) становится подпроектом; повторное добавление
// снимает скрытие. Папка вне проекта — отказ.
bool subprojects_add(const char *project, const char *dir);

// Завести подпроект: папка name внутри project и паспорт CLAUDE.md по
// шаблону системы вкладок (~/.claude/warp-tabs/template.md; без него —
// короткий свой). Паспорт — это и признак для поиска, и то, что Claude Code
// прочтёт вместе с паспортом родителя. При отказе err объясняет, почему.
bool subprojects_create(const char *project, const char *name, const char *oneline,
                        char *err, size_t err_cap);

// Убрать подпроект с глаз: на диске ничего не меняется, только файл
// исключений. Вернуть — правкой файла или повторным добавлением.
bool subprojects_hide(const char *project, const char *name);

#endif // BERTH_SUBPROJECTS_H
