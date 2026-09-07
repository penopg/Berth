// Свои экраны внутри вкладки: страница проекта и настройки.
//
// Рисуются и обрабатываются в один проход (immediate mode): кнопка проверяет
// попадание мыши по тому же прямоугольнику, который только что нарисовала.
// Для панели раскладка вынесена отдельно — там строки живут между кадрами и
// по ним ходят клавишами; здесь же элементы рождаются и умирают внутри кадра,
// и разносить их по двум местам значило бы согласовывать вручную то, что
// иначе не может разъехаться.
#ifndef BERTH_PAGE_H
#define BERTH_PAGE_H

#include "raylib.h"
#include "common.h"
#include "font.h"
#include "theme.h"
#include "session.h"
#include "projstate.h"
#include "settings.h"
#include "groups.h"
#include "usage.h"

typedef enum {
    PAGE_EVENT_NONE = 0,
    PAGE_EVENT_START_AGENT,      // запустить агента во вкладке; text — его id
    PAGE_EVENT_RESUME_SESSION,   // продолжить сессию Claude; text — её id
    PAGE_EVENT_REFRESH,          // перечитать сведения о проекте
    PAGE_EVENT_OPEN_FOLDER,      // открыть папку проекта в Finder
    PAGE_EVENT_HIDE_PAGE,        // спрятать страницу, вернуться к терминалу
    PAGE_EVENT_FONT_STEP,        // arg: +1, -1, 0 — сброс к умолчанию
    PAGE_EVENT_TOGGLE_SIDEBAR,
    PAGE_EVENT_TOGGLE_OPEN_MODE, // страница проекта или сразу агент
    PAGE_EVENT_SET_DEFAULT_AGENT,// text — id профиля
    PAGE_EVENT_SET_TASK_FINISH,  // arg — TaskFinish
    PAGE_EVENT_SET_THEME_PANEL,  // text — имя темы
    PAGE_EVENT_SET_THEME_WINDOW,   // text — имя темы или project
    PAGE_EVENT_TOGGLE_COLLAPSED_LIVE, // свёрнутая группа: показывать ли открытые
    PAGE_EVENT_SET_MARKER,       // маркер активного разговора; text — dot или karateka
    PAGE_EVENT_UNHIDE_GROUP,     // вернуть скрытую группу; text — её имя
    PAGE_EVENT_TOGGLE_USAGE_FETCH, // лимиты: свой запрос или только кэш
    PAGE_EVENT_TOGGLE_BERTH_SKILL, // скилл берта включить/выключить; text — имя
    PAGE_EVENT_JOURNAL_TOGGLE,   // раскрыть или свернуть запись журнала; arg — номер
    PAGE_EVENT_JOURNAL_MENTION,  // упомянуть запись в разговоре проекта; arg — номер
    PAGE_EVENT_JOURNAL_DAY,      // свернуть или раскрыть день ленты; arg — ключ дня
    PAGE_EVENT_BUILD_JOURNAL,    // собрать журнал проекта скиллом
    PAGE_EVENT_BUILD_SUMMARY,    // написать сводку проекта скиллом
    PAGE_EVENT_SHOW_TASK,        // показать фоновую задачу; arg — её вкладка
    PAGE_EVENT_STOP_TASK,        // остановить задачу; arg — её вкладка

    // Подпроекты. arg — порядковый номер подпроекта среди подпроектов
    // этого проекта, text — имя его папки.
    PAGE_EVENT_ADD_SUBPROJECT,   // выбрать папку и сделать её подпроектом
    PAGE_EVENT_HIDE_SUBPROJECT,  // убрать подпроект с глаз
    PAGE_EVENT_TOGGLE_SUB,       // раскрыть или свернуть раздел подпроекта
    PAGE_EVENT_NEW_SUBPROJECT,   // завести папку с паспортом; title — имя,
                                 // body — о чём, одной строкой

    // Скиллы проекта. text — имя скилла; arg — его номер в списке.
    PAGE_EVENT_SKILL_TOGGLE,     // раскрыть или свернуть описание
    PAGE_EVENT_SKILL_OPEN,       // открыть папку скилла в Finder
    PAGE_EVENT_SKILL_EDIT,       // открыть SKILL.md в редакторе
    PAGE_EVENT_NEW_SKILL,        // завести скилл; title — имя, body — когда применять
    // Документы из реестра .berth/files.tsv. arg — номер строки реестра.
    PAGE_EVENT_FILE_TOGGLE,      // раскрыть или свернуть документ
    PAGE_EVENT_FILE_SHOW,        // показывать таблицу на странице или нет
    PAGE_EVENT_TABLE_SORT,       // сортировать таблицу arg по колонке arg2
    PAGE_EVENT_TABLE_ROW,        // раскрыть запись arg2 таблицы arg
    PAGE_EVENT_TABLE_ALL,        // показать все записи таблицы arg
    PAGE_EVENT_TABLE_OPEN,       // открыть файл таблицы arg
    // Действие над данными: arg — номер в ActionList, event.prompt — готовая
    // реплика (подстановки уже сделаны, форма заполнена).
    PAGE_EVENT_ACTION_RUN,
    PAGE_EVENT_ACTION_TOGGLE,    // раскрыть действие в разделе «Действия»
    PAGE_EVENT_ACTIONS_EDIT,     // открыть .berth/actions.tsv в редакторе
    PAGE_EVENT_ACTIONS_NEW,      // попросить агента завести кнопку
    PAGE_EVENT_ACTION_REMOVE,    // убрать действие arg из файла
    PAGE_EVENT_TWO_COLUMNS,      // сложить страницу в одну колонку и обратно
    PAGE_EVENT_TABLE_CFG,        // открыть или закрыть настройку колонок
    PAGE_EVENT_TABLE_COL,        // показывать колонку arg2 таблицы arg или нет
    PAGE_EVENT_TABLE_COLS_ALL,   // показать все колонки таблицы arg
    // Переставить колонку: arg2 = откуда * TABLE_COLS_MAX + куда, оба —
    // места в порядке показа. Двух чисел в событии нет, а заводить третье
    // ради одной перестановки не стоит.
    PAGE_EVENT_TABLE_COL_MOVE,
    PAGE_EVENT_FILE_OPEN,        // открыть файл штатно
    PAGE_EVENT_FILE_REVEAL,      // показать файл в Finder
    PAGE_EVENT_DESCRIBE_FILES,   // описать все неописанные документы скиллом berth-files
    PAGE_EVENT_UFILE_OPEN,       // открыть неописанный файл; arg — номер в списке обхода
    PAGE_EVENT_DESCRIBE_FILE,    // описать один неописанный файл; arg — номер
    PAGE_EVENT_UNDESC_TOGGLE,    // раскрыть или свернуть список без пояснения

    PAGE_EVENT_NEW_PROJECT,      // завести проект в группе: cwd — папка группы, text — группа,
                                 // title — имя папки, body — о чём одной строкой

    // Задачи проекта из .berth/tasks.md. arg — номер задачи в списке.
    PAGE_EVENT_TODO_TOGGLE,      // раскрыть или свернуть описание
    PAGE_EVENT_TODO_DONE_TOGGLE, // показать или спрятать сделанные задачи списка
    PAGE_EVENT_TODO_MOVE,        // переставить: с места arg на место arg2
    PAGE_EVENT_TODO_SEND,        // отправить в разговор проекта
    PAGE_EVENT_TODO_STATE,       // перевести в состояние arg2 (TaskState)
    PAGE_EVENT_TODO_SAVE,        // записать текст из редактора; arg — номер
                                 // задачи, -1 — новая; title и body — текст
} PageEventKind;

typedef struct {
    PageEventKind kind;
    int  arg;
    int  arg2;
    char text[PROJINFO_ID_MAX];

    // К какому списку задач относится событие: NULL — к задачам самого
    // проекта, иначе путь подпроекта (указывает в ProjectList и живёт дольше
    // кадра). sub — его порядковый номер среди подпроектов, -1 у своих.
    const char *cwd;
    int  sub;

    // Готовая реплика действия: указывает на буфер страницы, живёт до
    // следующего кадра — исполнителю события этого хватает.
    const char *prompt;

    // Текст задачи из редактора страницы. Указатели на буферы редактора —
    // живут до следующего кадра, чего исполнителю события хватает.
    const char *title;
    const char *body;

    // На сколько содержимое выше окна — это и предел прокрутки. Знать это
    // можно только нарисовав, а прокрутка нужна уже в следующем кадре —
    // поэтому высота едет наружу вместе с событием, а не считается заранее.
    int  overflow;

    // Страница просит прокрутить себя к концу: внизу появилось или выросло
    // то, что человек сейчас правит, и оно не должно уехать за край.
    bool scroll_end;

    // Страница просит показать кусок [reveal_top, reveal_bottom) — в
    // координатах экрана этого кадра: раскрытая задача, редактор. Прокрутка
    // сдвигается ровно настолько, чтобы кусок попал в окно, а не к концу:
    // задачи давно не последнее на странице, и «к концу» уводит мимо них.
    bool reveal;
    int  reveal_top, reveal_bottom;
    int  scroll_top;   // верх прокручиваемой части — выше него шапка
} PageEvent;

// Обе рисуют страницу в область view и возвращают то, по чему кликнули.
// scroll — на сколько пикселей содержимое сдвинуто вверх.
// sessions нужен, чтобы показать фоновые задачи этого проекта: они живут в
// списке вкладок, а страница о них рассказывает.
// projects нужен ради подпроектов: их задачи страница родителя показывает
// разделами под своими.
PageEvent page_draw_project(const Session *s, const ProjectState *st,
                            const SessionList *sessions,
                            const ProjectList *projects,
                            const FontAtlas *font, const Theme *theme,
                            Rect view, Vector2 mouse, int scroll,
                            bool two_columns);
// Редактор нового проекта поверх окна. Он не принадлежит странице: группу
// заводят из панели, а на экране в этот момент может быть что угодно —
// поэтому рисуется отдельно, карточкой над областью терминала.
void      page_new_project_begin(const char *root, const char *group);
void      page_new_project_failed(const char *why);   // вернуть редактор с ошибкой
bool      page_overlay_active(void);
PageEvent page_draw_overlay(const FontAtlas *font, const Theme *theme,
                            Rect view, Vector2 mouse);

PageEvent page_draw_settings(const Settings *st, const Groups *groups,
                             const Usage *usage, const FontAtlas *font,
                             const Theme *theme, Rect view, Vector2 mouse,
                             int scroll);

#endif // BERTH_PAGE_H
