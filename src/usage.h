// Лимиты Claude Code: сколько израсходовано в пятичасовом окне и за неделю.
//
// Это свойство человека, а не проекта: лимит один на все вкладки и все
// каталоги. Поэтому он живёт в верхней полосе окна, рядом с настройками.
//
// Откуда берётся. Два источника, берётся более свежий:
// - кэш Claude Code: он спрашивает лимиты у сервера и складывает ответ в
//   `~/.claude.json`, поле `cachedUsageUtilization`. Обновляет редко и по
//   своим поводам — за час работы цифры там не сдвинулись;
// - свой запрос: тот же адрес, что у Claude Code, с его же токеном из связки
//   ключей macOS. Отдельного входа у берта нет и быть не может — OAuth
//   привязан к клиенту Claude Code; берт лишь пользуется входом, который на
//   этой машине уже сделан. У коллег то же: вошли в claude — берт видит.
//   Запрос идёт отвязанным процессом (`security` + `curl`) и кладёт ответ в
//   `~/.config/berth/usage.json`; кадр его не ждёт.
#ifndef BERTH_USAGE_H
#define BERTH_USAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define USAGE_LIMIT_MAX   6
#define USAGE_LABEL_MAX  40

typedef enum {
    USAGE_NORMAL = 0,
    USAGE_WARNING,
    USAGE_CRITICAL,
} UsageSeverity;

typedef enum {
    USAGE_KIND_OTHER = 0,
    USAGE_KIND_SESSION,        // пятичасовое окно
    USAGE_KIND_WEEKLY_ALL,     // неделя, все модели
    USAGE_KIND_WEEKLY_SCOPED,  // неделя, одна «дорогая» модель
} UsageKind;

typedef struct {
    char label[USAGE_LABEL_MAX];   // «сессия», «неделя», «неделя · Fable»
    UsageKind kind;
    int  percent;                  // 0..100
    UsageSeverity severity;
    time_t resets_at;              // когда окно обнулится; 0 — не сообщается
} UsageLimit;

typedef enum {
    USAGE_FROM_NONE = 0,
    USAGE_FROM_CACHE,   // кэш Claude Code
    USAGE_FROM_OWN,     // свой запрос
} UsageSource;

typedef struct {
    UsageLimit items[USAGE_LIMIT_MAX];
    int    count;
    time_t fetched;        // когда данные получены с сервера
    UsageSource source;
    char   account[96];    // почта учётки Claude Code, если известна

    time_t cache_mtime;    // время правки ~/.claude.json на момент чтения
    time_t own_mtime;      // и своего файла
    time_t last_spawn;     // когда последний раз запускали свой запрос
    char   own_path[512];  // куда свой запрос кладёт ответ
} Usage;

// Куда класть ответы своих запросов. Звать до usage_load.
void usage_set_own_path(Usage *u, const char *path);

// Запускает свой запрос отвязанным процессом, если с прошлого прошло
// достаточно. Возвращает true, если запустил.
bool usage_fetch_maybe(Usage *u, int min_interval_s);

// Читает лимиты с диска. Отсутствие файла или поля — не ошибка: у берта
// просто нечего показать, и полоса остаётся пустой.
void usage_load(Usage *u);

// Какой-то из файлов переписан с момента чтения. Claude Code трогает свой
// часто, поэтому проверка идёт по времени правки, а не чтением полутора сотен
// килобайт.
bool usage_changed(const Usage *u);

// «до 16:49» — когда окно обнулится, местным временем. Пусто, если неизвестно.
void usage_reset_text(time_t resets_at, char *out, size_t cap);

// Подсказка к одному окну — для балуна под курсором: что это за окно, когда
// оно обновится и через сколько, откуда и какой давности цифры. Строки
// разделены переводом строки.
void usage_tip_text(const Usage *u, int index, char *out, size_t cap);

#endif // BERTH_USAGE_H
