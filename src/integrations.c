#include "integrations.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// Пишет файл, если его нет. Заготовка бывает один раз, а дальше файлы
// принадлежат человеку и агенту — перезаписать их значило бы стереть
// письма или настроенный вид.
static bool write_once(const char *path, const char *text)
{
    struct stat st;
    if (stat(path, &st) == 0) return true;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    fclose(f);
    return ok;
}

bool mailbox_scaffold(const char *dir)
{
    if (!dir || !*dir) return false;
    char p[1024];
    snprintf(p, sizeof(p), "%s/.berth", dir);      mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/.berth/data", dir); mkdir(p, 0755);

    // Заголовки — те же семь колонок и в том же порядке, что у
    // `mail.py список` в скилле berth-mail-connect, плюс две колонки
    // разбора (`статус`, `что делать`), которые заполняет агент по
    // berth-mail-triage: состав колонок диктует файл, и `обновить`
    // разложит письма по ним, оставив разбор пустым. BOM — для Excel и
    // своего разбора.
    snprintf(p, sizeof(p), "%s/%s", dir, MAILBOX_TABLE);
    if (!write_once(p, "\xEF\xBB\xBF"
                       "дата\tот\tтема\tфрагмент\tвложения\tпрочитано\tid\t"
                       "статус\tчто делать\n"))
        return false;

    // На странице — дата, от кого, тема и разбор; остальное в раскрытой
    // записи. Колонки, которые агент допишет позже, встанут в конец
    // видимыми сами.
    // Фильтр: на странице только неразобранные, в заголовке и в панели их
    // число. Разобранные складываются под «Остальные».
    snprintf(p, sizeof(p), "%s/.berth/shown.tsv", dir);
    if (!write_once(p, "show\t" MAILBOX_TABLE
                       "\tдата|от|тема|статус|что делать|-фрагмент|-вложения|-прочитано|-id\n"
                       "filter\t" MAILBOX_TABLE "\tстатус\tпусто\tне разобрано\n"))
        return false;

    // Две кнопки над таблицей. «Обновить» — дорога «команда», без агента,
    // раз в два часа сама; текст без `--дней` — скрипт продолжает с
    // последней даты в таблице. «Разобрать» — разговор по скиллу разбора:
    // решения принимает человек, задаче их не спросить.
    snprintf(p, sizeof(p), "%s/.berth/actions.tsv", dir);
    if (!write_once(p, "# область\tимя\tкуда\tчто сказать\tраз в N мин\n"
                       "таблица:" MAILBOX_TABLE "\tОбновить\tкоманда\t"
                       "python3 .berth/mail/mail.py обновить\t120\n"
                       "таблица:" MAILBOX_TABLE "\tРазобрать\tразговор\t"
                       "/berth-mail-triage\n"))
        return false;

    // Реестр — иначе таблица висела бы в «Документах» как «без пояснения».
    snprintf(p, sizeof(p), "%s/.berth/files.tsv", dir);
    return write_once(p, "# путь\tчто это\n"
                         MAILBOX_TABLE "\tВходящие письма: дата, от кого, тема, "
                         "фрагмент. Дописывает `mail.py обновить`, повторы по id "
                         "отбрасываются.\n");
}
