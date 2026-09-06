// Опыт проекта: сколько токенов агент написал в нём за всё время и сколько
// было боёв. Живёт у берта (~/.config/berth/xp.tsv), не в проекте: это
// личная метрика, ей нечего делать в репозитории и незачем сталкиваться
// у коллег. Считается честно, по output_tokens из jsonl, тем же
// приращением, что читается для счёта боя, — без привязки к победе:
// последняя запись сообщения может прийти уже после смены состояния.
#ifndef BERTH_XP_H
#define BERTH_XP_H

#include <stdbool.h>

void xp_init(const char *path);          // читает файл; путь запоминается
long xp_tokens(const char *cwd);         // 0, если проекта в файле нет
int  xp_fights(const char *cwd);
void xp_add(const char *cwd, long tokens);
void xp_fight(const char *cwd);          // ещё один бой выигран
void xp_flush_maybe(void);               // пишет, если есть что и прошло полминуты
void xp_flush(void);                     // пишет, если есть что

#endif // BERTH_XP_H
