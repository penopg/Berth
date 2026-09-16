// Задачи всех проектов разом — для страницы «Задачи».
//
// Кэш состояний проектов (`projstate.h`) держит восемь наборов по мегабайту:
// для обзора по шестидесяти проектам он не годится, и вытеснять им то, с
// чем человек работает, нельзя. Здесь у каждого проекта только заголовки,
// состояния и сроки задач — то, что нужно списку; описание раскрытой задачи
// страница берёт из кэша состояний уже по одному проекту.
//
// Записи — проекты из списка панели по их номерам, а за ними папки групп:
// у папки группы свой `tasks.md` с задачами, у которых проекта ещё нет.
// Опрос — stat на файл раз в пару секунд, чтение только когда файл менялся.
#ifndef BERTH_ALLTASKS_H
#define BERTH_ALLTASKS_H

#include <stdbool.h>
#include <time.h>

#include "projects.h"
#include "tasks.h"

#define ALLTASKS_PER_ENTRY 40
#define ALLTASKS_GROUPS    32
#define ALLTASKS_MAX       (PROJECT_MAX + ALLTASKS_GROUPS)

typedef struct {
    char      title[TASK_TITLE_MAX];
    TaskState state;
    int       index;     // номер в файле проекта — по нему события находят задачу
    long      due_day;   // срок днём календаря, 0 — нет
} AllTask;

typedef struct {
    char    path[PROJECT_PATH_MAX];
    char    name[PROJECT_NAME_MAX];    // имя проекта или группы в панели
    char    group[PROJECT_NAME_MAX];
    bool    is_group;                  // папка группы, не проект
    time_t  mtime;                     // .berth/tasks.md при чтении; 0 — файла нет
    long    mtime_ns;
    AllTask items[ALLTASKS_PER_ENTRY];
    int     count;                     // сколько взято в items
    int     open, review;              // счёт по файлу целиком
} AllTasksEntry;

// Папка группы: имя группы и её каталог. Собирает приложение — только оно
// знает, у какой группы какая папка.
typedef struct {
    char name[PROJECT_NAME_MAX];
    char path[PROJECT_PATH_MAX];
} AllTasksRoot;

typedef struct {
    AllTasksEntry items[ALLTASKS_MAX];
    int count;           // проекты по номерам, дальше папки групп
    int projects;        // сколько из них проектов
    int open, review;    // сумма по всем
    int due_soon;        // со сроком сегодня, завтра или просроченным
} AllTasks;

// Сверить с диском. Перечитывает только записи, у которых файл менялся
// или путь сменился (список проектов перечитали и номера поехали).
void alltasks_refresh(AllTasks *a, const ProjectList *projects,
                      const AllTasksRoot *roots, int nroots);

#endif // BERTH_ALLTASKS_H
