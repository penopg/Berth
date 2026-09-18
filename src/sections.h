// Какие разделы страницы проекта свёрнуты — по проекту, у берта.
//
// Это выбор взгляда, а не свойство проекта: в репозиторий ему не место,
// а на другой машине человек сложит страницу по-своему. Файл
// `~/.config/berth/sections.tsv`, строки `<каталог>\t<раздел>\t1|0`.
// Записывается только перевёрнутое умолчание: умолчания страница считает
// по содержимому (у проекта с таблицей журнал свёрнут), и хранить их
// значило бы заморозить.
#ifndef BERTH_SECTIONS_H
#define BERTH_SECTIONS_H

#include <stdbool.h>

void sections_init(const char *path);
// -1 — человек не трогал, 0 — свёрнут, 1 — раскрыт.
int  sections_get(const char *cwd, const char *key);
void sections_set(const char *cwd, const char *key, bool open);

#endif // BERTH_SECTIONS_H
