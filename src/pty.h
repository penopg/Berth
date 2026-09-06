// Псевдотерминал: запуск дочерней оболочки и обмен байтами с ней.
#ifndef BERTH_PTY_H
#define BERTH_PTY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <ghostty/vt.h>

// Запускает оболочку пользователя в новом псевдотерминале.
//
// cwd — рабочий каталог дочернего процесса; NULL означает «не менять».
// Возвращает master fd (>= 0) и кладёт pid ребёнка в *child_out,
// либо -1 при ошибке.
int pty_spawn(pid_t *child_out, const char *cwd,
              uint16_t cols, uint16_t rows,
              int cell_width, int cell_height);

// Пишет в master fd по мере возможности. fd неблокирующий, поэтому
// write() может вернуть частичную запись или EAGAIN.
void pty_write(int pty_fd, const char *buf, size_t len);

// Результат вычитывания вывода дочернего процесса.
typedef enum {
    PTY_READ_OK = 0,   // прочитали сколько успели, fd жив
    PTY_READ_EOF,      // ребёнок закрыл свою сторону
    PTY_READ_ERROR,    // ошибка чтения
} PtyReadResult;

// Вычитывает вывод из master fd и скармливает его VT-парсеру ghostty.
//
// Читает не до EAGAIN, а в пределах бюджета времени на кадр: пустой
// kernel-буфер не означает, что процесс замолчал, и выход по первому
// EAGAIN упирает пропускную способность в размер буфера на кадр.
// budget_s — сколько времени кадр готов потратить на эту вкладку. Ждём данные
// только когда поток уже пошёл: у молчащей вкладки чтение стоит один системный
// вызов, иначе каждая простаивающая вкладка проедала бы кадр целиком.
// bytes_out — сколько байт прочитано за вызов (может быть NULL).
PtyReadResult pty_read(int pty_fd, GhosttyTerminal terminal, double budget_s,
                       unsigned long *bytes_out);

#endif // BERTH_PTY_H
