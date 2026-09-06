// Псевдотерминал. Производное от main.c проекта Ghostling (MIT).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/wait.h>
#include <pwd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <time.h>

#include "pty.h"

// Монотонные секунды. Раньше здесь был GetTime() из raylib — из-за него
// работа с pty тянула за собой оконную библиотеку.
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Spawn the user's default shell in a new pseudo-terminal.
//
// Creates a pty pair via forkpty(), sets the initial window size, execs the
// shell in the child, and puts the master fd into non-blocking mode so we
// can poll it each frame without stalling the render loop.
//
// The shell is chosen by checking, in order:
//   1. $SHELL environment variable
//   2. The pw_shell field from the passwd database
//   3. /bin/sh as a last resort
//
// Returns the master fd on success (>= 0) and stores the child pid in
// *child_out.  Returns -1 on failure.
int pty_spawn(pid_t *child_out, const char *cwd,
              uint16_t cols, uint16_t rows,
              int cell_width, int cell_height)
{
    int pty_fd;
    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = (unsigned short)(cols * cell_width),
        .ws_ypixel = (unsigned short)(rows * cell_height),
    };

    // forkpty() combines openpty + fork + login_tty into one call.
    // In the child it sets up the slave side as stdin/stdout/stderr.
    pid_t child = forkpty(&pty_fd, NULL, NULL, &ws);
    if (child < 0) {
        perror("forkpty");
        return -1;
    }
    if (child == 0) {
        // Проект задаёт рабочий каталог вкладки. Если каталог исчез —
        // не роняем сессию, стартуем там, где есть.
        if (cwd && cwd[0] != '\0')
            (void)chdir(cwd);

        // Determine the user's preferred shell.  We try $SHELL first (the
        // standard convention), then fall back to the passwd entry, and
        // finally to /bin/sh if nothing else is available.
        const char *shell = getenv("SHELL");
        if (!shell || shell[0] == '\0') {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_shell && pw->pw_shell[0] != '\0')
                shell = pw->pw_shell;
            else
                shell = "/bin/sh";
        }

        // Extract just the program name for argv[0] (e.g. "/bin/zsh" → "zsh").
        const char *shell_name = strrchr(shell, '/');
        shell_name = shell_name ? shell_name + 1 : shell;

        // Запускаем оболочку как login shell: дефис в argv[0] — общепринятый
        // для этого способ, так делает и системный Терминал. Иначе профиль
        // пользователя не читается, и запущенному из Dock приложению не
        // достаётся его PATH — а значит, не находится ни claude, ни git.
        char argv0[64];
        snprintf(argv0, sizeof(argv0), "-%s", shell_name);

        // Child process — replace ourselves with the shell.
        // TERM tells programs what escape sequences we understand.
        setenv("TERM", "xterm-256color", 1);
        execl(shell, argv0, NULL);
        _exit(127); // execl only returns on error
    }

    // Parent — make the master fd non-blocking so read() returns EAGAIN
    // instead of blocking when there's no data, letting us poll each frame.
    int flags = fcntl(pty_fd, F_GETFL);
    if (flags < 0 || fcntl(pty_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl O_NONBLOCK");
        close(pty_fd);
        return -1;
    }

    *child_out = child;
    return pty_fd;
}

// Best-effort write to the pty master fd.  Because the fd is
// non-blocking, write() may return short or fail with EAGAIN.  We
// retry on EINTR, advance past partial writes, and silently drop
// data if the kernel buffer is full (EAGAIN) — this matches what
// most terminal emulators do under back-pressure.
void pty_write(int pty_fd, const char *buf, size_t len)
{
    // Входной буфер pty невелик (порядка килобайта), а дескриптор
    // неблокирующий: длинная вставка получает EAGAIN на полпути. Выбросить
    // остаток нельзя — с ним пропадает и маркер конца вставки, и приложение
    // ждёт его до своего таймаута, а всё набранное следом считает частью
    // вставки. Поэтому ждём, пока процесс вычитает буфер; предел — на случай
    // процесса, который не читает вовсе.
    int waited_ms = 0;
    while (len > 0) {
        ssize_t n = write(pty_fd, buf, len);
        if (n > 0) {
            buf += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN && waited_ms < 2000) {
            struct pollfd pfd = { .fd = pty_fd, .events = POLLOUT };
            if (poll(&pfd, 1, 50) < 0 && errno != EINTR) break;
            waited_ms += 50;
            continue;
        }
        break;   // настоящая ошибка или процесс так и не прочитал
    }
}

// Drain all available output from the pty master and feed it into the
// ghostty terminal.  The terminal's VT parser will process any escape
// sequences and update its internal screen/cursor/style state.
//
// Ключевой момент производительности: EAGAIN означает лишь, что kernel-буфер
// пуст в этот момент, а не что процесс закончил вывод. Выход по первому EAGAIN
// давал ~8 КБ за кадр, то есть потолок около 500 КБ/с. Поэтому на кадр
// отводится бюджет времени, внутри которого мы ждём данные через poll().
PtyReadResult pty_read(int pty_fd, GhosttyTerminal terminal, double budget_s)
{
    // Буфер побольше: 4 КБ означало лишний системный вызов на каждые 4 КБ вывода.
    static uint8_t buf[65536];

    // EAGAIN значит «ядерный буфер pty пуст ПРЯМО СЕЙЧАС», а он всего несколько
    // килобайт. Выходя по первому EAGAIN, мы читали ~8 КБ за кадр и засыпали до
    // следующего — потолок выходил ~500 КБ/с при простаивающем процессоре.
    // Вместо этого даём кадру бюджет времени: дренируем pty, пока данные идут,
    // но не дольше бюджета, чтобы не ронять частоту кадров.
    const double started = now_seconds();
    bool got_data = false;   // ждать имеет смысл только если поток уже пошёл

    for (;;) {
        ssize_t n = read(pty_fd, buf, sizeof(buf));
        if (n > 0) {
            ghostty_terminal_vt_write(terminal, buf, (size_t)n);
            got_data = true;
            if (now_seconds() - started >= budget_s)
                return PTY_READ_OK;
        } else if (n == 0) {
            // EOF — the child closed its side of the pty.
            return PTY_READ_EOF;
        } else {
            // n == -1: distinguish "no data right now" from real errors.
            if (errno == EAGAIN) {
                // Ничего не пришло и поток не идёт — уходим немедленно.
                // Ждать здесь значило бы усыплять кадр ради вкладки, в которой
                // ничего не происходит: с несколькими такими вкладками окно
                // переставало успевать за клавиатурой.
                if (!got_data)
                    return PTY_READ_OK;

                // Поток идёт: данные подойдут через доли миллисекунды, и
                // дождаться их дешевле, чем возвращаться сюда следующим кадром.
                // Но не дольше остатка бюджета — и не дольше двух миллисекунд
                // за раз, чтобы ввод не ждал конца вывода.
                double left_s = budget_s - (now_seconds() - started);
                int left_ms = (int)(left_s * 1000.0);
                if (left_ms <= 0)
                    return PTY_READ_OK;
                if (left_ms > 2) left_ms = 2;
                struct pollfd pfd = { .fd = pty_fd, .events = POLLIN, .revents = 0 };
                if (poll(&pfd, 1, left_ms) <= 0)
                    return PTY_READ_OK;
                continue;
            }
            if (errno == EINTR)
                continue; // retry the read
            // On Linux, the slave closing often produces EIO rather
            // than a clean EOF (read returning 0).  Treat it the same.
            if (errno == EIO)
                return PTY_READ_EOF;
            perror("pty read");
            return PTY_READ_ERROR;
        }
    }
}
