// Сведения о проекте, собираемые с диска для страницы проекта.
//
// Всё читается файлами, без запуска процессов: страница рисуется в том же
// кадре, что и остальное окно, и `git status` посреди отрисовки уронил бы
// частоту кадров на ровном месте. Ветка берётся из `.git/HEAD`, история —
// из каталога сессий Claude Code.
#ifndef BERTH_PROJINFO_H
#define BERTH_PROJINFO_H

#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

#define PROJINFO_SESSION_MAX 12
#define PROJINFO_STATUS_MAX   8
#define PROJINFO_LINE_MAX   200
#define PROJINFO_ID_MAX      72
#define PROJINFO_SUMMARY_MAX 2400
#define PROJINFO_SUMMARY_FILE ".berth/summary.md"

// Кто держит сессию прямо сейчас. Продолжить можно только свободную: Claude
// Code не пускает второго в тот же диалог, а если пустить силой — две копии
// пишут в один jsonl и история рвётся.
typedef enum {
    PROJ_SESSION_FREE = 0,
    PROJ_SESSION_OPEN,     // открыта в живой вкладке — чьей-то или нашей
    PROJ_SESSION_AGENT,    // в ней работает фоновый агент
} ProjSessionUse;

typedef struct {
    char   id[PROJINFO_ID_MAX];    // имя jsonl без расширения — оно же id для --resume
    char   title[PROJINFO_LINE_MAX];
    time_t mtime;

    ProjSessionUse use;

} ProjSession;

typedef struct {
    char cwd[512];

    char branch[64];               // пусто, если каталог не под git
    bool detached;                 // HEAD не на ветке

    char status[PROJINFO_STATUS_MAX][PROJINFO_LINE_MAX];  // выжимка из CLAUDE.md
    int  status_lines;
    bool has_claude_md;

    // Сводка проекта: что это, где сейчас, что дальше. Её пишет агент
    // (скилл /project-summary) в .berth/summary.md — по паспорту, коду и
    // истории работы, поэтому она есть и у проектов без CLAUDE.md.
    char   summary[PROJINFO_SUMMARY_MAX];
    bool   has_summary;
    time_t summary_mtime;

    ProjSession sessions[PROJINFO_SESSION_MAX];
    int  session_count;            // сколько показываем
    int  session_total;            // сколько всего нашлось
    int  session_busy;             // из показанных — заняты живым процессом
    int  session_empty;            // живые вкладки, чей диалог ещё пуст
} ProjInfo;

// Перечитывает одну сводку, если файл изменился. Дёшево: stat и, изредка,
// чтение пары килобайт. Возвращает true, если сводка обновилась.
bool projinfo_refresh_summary(ProjInfo *info);

// Собирает всё заново. Дорогая операция по меркам кадра (чтение нескольких
// файлов), поэтому вызывается при открытии страницы и по обновлению, а не
// каждый кадр.
void projinfo_load(ProjInfo *info, const char *cwd);

// Каталог истории Claude Code для этого проекта: путь, где всё похожее на
// разделитель заменено дефисом. Пустая строка, если HOME не задан.
void projinfo_session_dir(const char *cwd, char *out, size_t cap);

// «сегодня», «вчера», «5 дн. назад» — для списка сессий.
void projinfo_age(time_t when, char *out, size_t cap);

// Чем продолжить работу в этом каталоге: `claude --resume <id>` самой свежей
// свободной сессии, а если свободных нет — `claude` с чистого листа.
//
// Именно `--resume <id>`, а не `--continue`: «продолжи самую свежую» у каждой
// вкладки разрешается одинаково, и десять вкладок садятся в один диалог. Id
// выбираем сами, и выбранное тут же откладываем (см. ниже), чтобы соседняя
// вкладка в этом же кадре получила другую сессию.
void projinfo_resume_command(const char *cwd, char *out, size_t cap);

// Выбрать свободную сессию, ничего не собирая в команду: id кладётся в out,
// пустая строка — свободных нет. Вкладка запоминает выданное и при следующем
// запуске возвращается именно в этот диалог, а не тянет жребий заново.
void projinfo_pick_session(const char *cwd, char *out, size_t cap);

// Есть ли у сессии файл диалога. Пустую (заведённую, но ни разу не
// записанную) продолжать нечем: `--resume` на неё завершится ошибкой.
bool projinfo_session_exists(const char *cwd, const char *id);

// Занята ли сессия живым процессом Claude Code прямо сейчас.
bool projinfo_session_busy(const char *cwd, const char *id);

// Какой диалог ведёт процесс, запущенный внутри `root` (обычно это оболочка
// вкладки, а `claude` — её потомок). Пустая строка, если такого нет.
//
// Нужно там, где id не выдавали мы: вкладка начала новую сессию сама, и узнать
// её имя можно только у самого Claude Code — по реестру живых процессов.
void projinfo_session_of_pid(pid_t root, char *out, size_t cap);

// Чем занят этот процесс — по полю `status` реестра: idle — ждёт человека,
// busy — работает, waiting — ждёт ответа (подтверждение, вопрос). NONE —
// процесса Claude Code под root нет.
typedef enum {
    PROJ_LIVE_NONE = 0,
    PROJ_LIVE_IDLE,
    PROJ_LIVE_BUSY,
    PROJ_LIVE_WAITING,
} ProjLiveStatus;

// То же, что projinfo_session_of_pid, но сразу и id, и состояние: реестр
// читается один раз.
// `since` — когда состояние в последний раз менялось (statusUpdatedAt из
// реестра, в секундах); 0, если не записано. Можно передать NULL.
ProjLiveStatus projinfo_live_of_pid(pid_t root, char *id_out, size_t cap, time_t *since);

#endif // BERTH_PROJINFO_H
