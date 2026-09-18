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

bool jira_scaffold(const char *dir, const char *url, const char *login)
{
    if (!dir || !*dir) return false;
    char p[1024];
    snprintf(p, sizeof(p), "%s/.berth", dir);      mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/.berth/data", dir); mkdir(p, 0755);

    // Десять колонок `jira.py список` из скилла berth-jira-connect плюс две
    // колонки разбора. У Jira свой «статус» — состояние работы, поэтому
    // решение человека зовётся «разбор», а не «статус», как у писем.
    snprintf(p, sizeof(p), "%s/%s", dir, JIRA_TABLE);
    if (!write_once(p, "\xEF\xBB\xBF"
                       "ключ\tтема\tтип\tстатус\tприоритет\tисполнитель\tавтор\t"
                       "срок\tобновлено\tссылка\tразбор\tчто делать\n"))
        return false;

    snprintf(p, sizeof(p), "%s/.berth/shown.tsv", dir);
    if (!write_once(p, "show\t" JIRA_TABLE
                       "\tключ|тема|статус|срок|разбор|что делать"
                       "|-тип|-приоритет|-исполнитель|-автор|-обновлено|-ссылка\n"
                       "filter\t" JIRA_TABLE "\tразбор\tпусто\tне разобрано\n"))
        return false;

    // «Обновить» — команда без агента, раз в полчаса сама. «Разобрать» —
    // разговор по скиллу разбора. «Новый список» — разговор: список по
    // JQL это подпроект, а JQL требует слов, значит заводит его агент.
    snprintf(p, sizeof(p), "%s/.berth/actions.tsv", dir);
    if (!write_once(p, "# область\tимя\tкуда\tчто сказать\tраз в N мин\n"
                       "таблица:" JIRA_TABLE "\tОбновить\tкоманда\t"
                       "python3 .berth/jira/jira.py обновить\t30\n"
                       "таблица:" JIRA_TABLE "\tРазобрать\tразговор\t"
                       "/berth-jira-triage\n"
                       "таблица:" JIRA_TABLE "\tНовый список\tразговор\t"
                       "Заведи список задач Jira по фильтру: предложи мои избранные "
                       "фильтры (jira.py фильтры) или спроси JQL, дальше по разделу "
                       "«Списки по фильтрам» скилла berth-jira-connect\n"))
        return false;

    // Адрес и логин берт знает из карточки; вид (cloud или server), JQL и
    // учётку связки ключей допишет агент при подключении. Токена здесь нет
    // и не будет. Логина может не быть: токен приложения (PAT) ходит без
    // него, тогда учётка связки ключей — хост сайта.
    const char *host = url ? strstr(url, "://") : NULL;
    host = host ? host + 3 : (url ? url : "");
    char hostbuf[256];
    snprintf(hostbuf, sizeof(hostbuf), "%.*s", (int)strcspn(host, "/:"), host);
    snprintf(p, sizeof(p), "%s/.berth/jira.conf", dir);
    char conf[1200];
    snprintf(conf, sizeof(conf),
             "# Настройки сайта Jira для .berth/jira/jira.py. Токена здесь нет:\n"
             "# он в связке ключей (служба berth-jira, учётка keychain).\n"
             "url = %s\n"
             "login = %s\n"
             "keychain = %s\n"
             "# kind = cloud | server — агент допишет после проверки\n"
             "jql = (assignee = currentUser() OR reporter = currentUser() "
             "OR watcher = currentUser()) AND resolution = Unresolved\n"
             "days = 30\n",
             url ? url : "", login ? login : "",
             login && *login ? login : hostbuf);
    if (!write_once(p, conf)) return false;

    snprintf(p, sizeof(p), "%s/.berth/files.tsv", dir);
    return write_once(p, "# путь\tчто это\n"
                         JIRA_TABLE "\tЗадачи Jira: ключ, тема, статус, срок и "
                         "разбор. Обновляет `jira.py обновить`, строки по ключу.\n");
}
