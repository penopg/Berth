// Остаток контекста разговора Claude Code.
//
// Снаружи Claude Code процент не отдаёт: в реестре живых процессов только
// статус. Зато каждый ответ агента в jsonl сессии несёт `usage` — сколько
// токенов ушло на вход (свежих и из кэша). Сумма по последнему ответу и
// есть занятое место в окне; окно — по имени модели из той же записи.
#ifndef BERTH_CTX_H
#define BERTH_CTX_H

#include <stdbool.h>
#include <stddef.h>

#include <time.h>

typedef struct {
    long   used;      // токенов в контексте по последнему ответу
    long   window;    // окно модели
    char   model[64];
    time_t at;        // когда этот ответ (или сжатие) записан; 0 — неизвестно
} CtxInfo;

// Читает хвост jsonl и заполняет `out`. false — записи с usage не нашлось.
bool ctx_read(const char *jsonl_path, CtxInfo *out);

// Окно модели по её имени; неизвестные считаются миллионными — новые
// модели все такие, а ошибиться в большую сторону значит показать
// остаток меньше, чем есть, а не наоборот.
long ctx_window_of(const char *model);

// Занято в процентах окна, 0..100; -1, если данных нет.
static inline int ctx_percent_used(const CtxInfo *c)
{
    if (c->window <= 0) return -1;
    long used = c->used * 100 / c->window;
    return used < 0 ? 0 : (used > 100 ? 100 : (int)used);
}

#endif // BERTH_CTX_H
