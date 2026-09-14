// Действия над данными: кнопка, которая одним кликом отправляет агенту
// готовую реплику.
//
// Живут в проекте, `.berth/actions.tsv`, по строке на действие:
//
//     запись:.berth/data/films.tsv<TAB>Посмотрел<TAB>задача<TAB>Отметь «{название}»…
//
// Четыре поля: к чему прикреплено, подпись кнопки, куда идёт результат,
// что сказать. Пишет их агент по скиллу `berth-actions`, правит и человек.
//
// Почему это не часть скилла: скилл — процедура (как подбирать фильмы),
// действие — привязка (к какой таблице, под каким именем, с какими
// параметрами). У них разный срок жизни, и одна процедура обслуживает
// несколько привязок. К тому же `SKILL.md` принадлежит Claude Code, и берт
// в чужой формат не пишет — как не пишет в `projects.json`.
//
// Текст — одна строка намеренно. Нужен длинный промпт — это скилл, а
// действие его зовёт: «/watchlist-подбор похожее на {название}».
#ifndef BERTH_ACTIONS_H
#define BERTH_ACTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define ACTIONS_MAX       24
#define ACTION_NAME_MAX   48
#define ACTION_TEXT_MAX   512
#define ACTION_TARGET_MAX 256
#define ACTION_ASK_MAX    8     // сколько полей формы бывает у действия
#define ACTION_LABEL_MAX  48
#define ACTIONS_FILE      ".berth/actions.tsv"

typedef enum {
    ACTION_ROW,      // кнопка у раскрытой записи таблицы
    ACTION_TABLE,    // кнопка у таблицы целиком
} ActionScope;

// Куда девать результат. Задача — вкладка `claude -p` на простой модели:
// правка идёт в файл, разговор не тронут. Разговор — реплика в разговор
// проекта: ответ нужно читать. Команда — без агента вовсе: берт запускает
// текст через `sh -c` из корня проекта, отвязанно, вывод в `.berth/log`;
// ради строки `mail.py обновить` поднимать `claude` дорого и медленно.
// Подстановок у команды нет: значения ячеек в оболочку не идут.
typedef enum {
    ROAD_TALK,
    ROAD_TASK,
    ROAD_CMD,
} ActionRoad;

typedef struct {
    ActionScope scope;
    char target[ACTION_TARGET_MAX];   // путь таблицы относительно корня
    char name[ACTION_NAME_MAX];       // подпись кнопки
    ActionRoad road;
    bool to_task;                     // road == ROAD_TASK, для старого кода
    char text[ACTION_TEXT_MAX];
    // Пятое поле, необязательное: раз в сколько минут запускать команду
    // самой, пока берт работает. 0 — только по клику. Кнопка и расписание
    // одна строка: второго файла с источниками таблицы не нужно.
    int  every;
} Action;

// Слово дороги, как оно пишется в файле и на странице.
const char *action_road_name(const Action *a);

typedef struct {
    Action items[ACTIONS_MAX];
    int    count;
    bool   exists;
    time_t mtime;
} ActionList;

void actions_load(ActionList *al, const char *cwd);
bool actions_changed(const ActionList *al, const char *cwd);

// Убрать действие с таким номером. Файл переписывается построчно: всё
// остальное — комментарии, пустые строки, чужой порядок — остаётся как
// было. Это файл берта, а не данные человека, поэтому писать в него он
// вправе; но и трогает он ровно одну строку.
bool actions_remove(ActionList *al, const char *cwd, int index);

// Подставить в текст значения по именам: `{название}` берётся из names/values.
// Имя, которого в списке нет, остаётся в тексте как есть и попадает в ask —
// это и есть поля формы: описывать их отдельно не нужно, они выводятся из
// самого текста. Возвращает, сколько разных таких имён нашлось.
int action_expand(const char *text, const char *const *names, const char *const *values,
                  int n, char *out, size_t cap,
                  char ask[][ACTION_LABEL_MAX], int ask_cap);

#endif // BERTH_ACTIONS_H
