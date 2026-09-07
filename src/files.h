// Реестр документов проекта: что за файл, без открытия файла.
//
// Живёт в проекте, `.berth/files.tsv`, по строке на документ: путь
// относительно корня, табуляция, пояснение одной фразой. Пишет его агент по
// скиллу berth-files, правит и человек. Даты в реестре нет намеренно: она
// врёт после правки руками, время берётся с самого файла.
//
// Берт сюда не пишет: реестр только читается и показывается. Единственное,
// что берт считает сам, — сколько документов в папке ещё не описано: обход
// по тем же правилам, что у скилла, не чаще раза в полминуты.
#ifndef BERTH_FILES_H
#define BERTH_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define FILES_MAX      96
#define FILE_PATH_MAX  256
#define FILE_NOTE_MAX  480    // 200 знаков кириллицы с запасом
#define FILES_FILE     ".berth/files.tsv"
#define FILES_UNDESC_MAX 64   // сколько неописанных показываем по именам
#define FILES_SHOWN_MAX  2    // сколько таблиц разом показываем на странице
#define FILES_SHOWN_FILE ".berth/shown.tsv"

typedef struct {
    char   path[FILE_PATH_MAX];   // как в реестре, относительно корня
    char   note[FILE_NOTE_MAX];
    bool   exists;                // файл на месте; нет — реестр устарел
    time_t mtime;                 // правка самого файла, 0 если файла нет
} FileEntry;

typedef struct {
    FileEntry items[FILES_MAX];
    int    count;
    bool   exists;        // реестр есть (без него раздел живёт одним обходом)
    bool   partial;       // строк в реестре больше, чем помещается
    time_t mtime;         // реестра при последнем чтении

    // Документы в папке, которых в реестре нет: их берт находит сам, чтобы
    // положенное руками было видно рядом с описанным, а не пропадало.
    // -1 — ещё не считали. Имена — первые FILES_UNDESC_MAX по алфавиту,
    // число — всё найденное. Обход ограничен по числу записей: у проекта
    // с чужим деревом внутри (референсные исходники) он не должен стоить
    // кадра.
    int    undescribed;
    char   undesc[FILES_UNDESC_MAX][FILE_PATH_MAX];
    int    undesc_count;
    bool   scan_cut;      // обход упёрся в предел — число неполное
    time_t scanned_at;

    // Какие таблицы человек попросил показывать прямо на странице проекта.
    // Свой файл, а не третья колонка реестра: реестр пишет агент, и при
    // очередном «опиши документы» лишняя колонка не пережила бы перезапись.
    // Строки `show\t<путь>`, как у subprojects.tsv.
    char   shown[FILES_SHOWN_MAX][FILE_PATH_MAX];
    int    shown_count;
    time_t shown_mtime;
} FileList;

void files_load(FileList *fl, const char *cwd);
bool files_changed(const FileList *fl, const char *cwd);   // реестр на диске другой

// Уточнить, что на диске: есть ли файлы и когда правлены, сколько документов
// без пояснения. Дёшево, если звать раз в пару секунд: stat на строку, а
// обход папки — не чаще раза в полминуты.
void files_refresh(FileList *fl, const char *cwd);

// Документ ли это по правилам скилла: расширение из списка, не паспорт, не
// README, не лицензия. Путь — относительно корня проекта.
bool files_is_document(const char *rel);

// Таблица ли это: только такие берт умеет показывать на странице.
bool files_is_table(const char *rel);

// Показывается ли этот файл на странице проекта.
bool files_shown(const FileList *fl, const char *rel);

// Включить или выключить показ. Пишет `.berth/shown.tsv` (через временный
// файл и rename, как задачи) и правит список в памяти. false — не вышло:
// мест уже нет или файл не записался.
bool files_show_toggle(FileList *fl, const char *cwd, const char *rel);

#endif // BERTH_FILES_H
