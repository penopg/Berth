// Когда команда действия запускалась по таймеру в последний раз.
//
// Состояние машины, а не проекта: файл `~/.config/berth/timers.tsv` со
// строками `<каталог проекта>\t<имя кнопки>\t<время, epoch>`. В проект оно
// не пишется — там это было бы чужим мусором в репозитории, а таймер идёт
// на этой машине и только пока берт работает.
#ifndef BERTH_TIMERS_H
#define BERTH_TIMERS_H

#include <time.h>

// Путь к файлу состояния; до вызова таймеры считаются не запускавшимися.
void   timers_init(const char *path);
time_t timers_last(const char *cwd, const char *name);
void   timers_mark(const char *cwd, const char *name, time_t when);

#endif // BERTH_TIMERS_H
