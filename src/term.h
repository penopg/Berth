// Term — одна сессия терминала: дочерний процесс, псевдотерминал и
// состояние экрана libghostty.
//
// В демо Ghostling всё это лежало локальными переменными в main(), потому что
// сессия была ровно одна. Здесь сессия — самостоятельная сущность, и окно
// держит их массив: вкладка-проект = Term.
#ifndef BERTH_TERM_H
#define BERTH_TERM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <ghostty/vt.h>

typedef struct {
    const char *cwd;        // рабочий каталог; NULL — унаследовать
    uint16_t    cols, rows;
    int         cell_width, cell_height;
    size_t      scrollback_lines;   // 0 — значение по умолчанию (1000)
} TermOpts;

typedef struct {
    // Дочерний процесс и канал к нему
    int    pty_fd;
    pid_t  child;
    bool   child_exited;    // pty отдал EOF/ошибку
    bool   child_reaped;    // waitpid прошёл
    int    child_status;    // -1 пока неизвестен
    unsigned long bytes_in; // сколько байт процесс вывел за всё время: поток
                            // вывода — живой признак работы агента

    // Состояние терминала и кодировщики ввода
    GhosttyTerminal      vt;
    GhosttyKeyEncoder    key_encoder;
    GhosttyKeyEvent      key_event;
    GhosttyMouseEncoder  mouse_encoder;
    GhosttyMouseEvent    mouse_event;

    // Снимок для отрисовки и переиспользуемые итераторы
    GhosttyRenderState                    render_state;
    GhosttyRenderStateRowIterator         row_iter;
    GhosttyRenderStateRowCells            row_cells;
    GhosttyKittyGraphicsPlacementIterator placement_iter;

    // Геометрия. Дублируется здесь, потому что effect-колбэки отвечают
    // на запросы размера от приложений в терминале.
    uint16_t cols, rows;
    int      cell_width, cell_height;

    // Заголовок из OSC 0/2. Демо звало SetWindowTitle прямо из колбэка —
    // при нескольких вкладках так нельзя: фоновая сессия переписывала бы
    // заголовок окна. Складываем сюда, окно берёт заголовок активной.
    char title[256];
    bool title_changed;

    bool scrollbar_dragging;
    bool focused;           // последнее, что мы сообщили приложению
} Term;

// Разовая настройка на процесс: декодер PNG для Kitty-графики.
// Обязана быть вызвана до создания первого Term.
void term_global_init(void);

// Создаёт сессию: терминал libghostty, дочернюю оболочку в pty и кодировщики.
// При ошибке возвращает false, уже захваченное освобождает сам.
bool term_open(Term *t, const TermOpts *opts);

// Просит дочерний процесс завершиться, не дожидаясь его.
//
// Нужно, чтобы закрыть много вкладок сразу: сначала просим всех, потом
// собираем. Иначе окно висит, ожидая каждого по очереди.
void term_request_close(Term *t);

// Закрывает сессию: дожидается дочернего процесса и освобождает ресурсы.
// Безопасно вызывать на частично созданном или уже закрытом Term.
void term_close(Term *t);

// Меняет размер сетки: терминал (с рефлоу) и winsize pty, чтобы
// дочерний процесс получил SIGWINCH.
void term_resize(Term *t, uint16_t cols, uint16_t rows,
                 int cell_width, int cell_height);

// Кадровый шаг: вычитать вывод ребёнка и, если он умер, прибрать зомби.
// budget_s — доля кадра, которую можно потратить на чтение вывода этой вкладки.
void term_poll(Term *t, double budget_s);

// Отправить байты приложению (ввод с клавиатуры, вставка, ответы на запросы).
void term_send(Term *t, const char *buf, size_t len);

// Вставка текста. От term_send отличается тем, что уважает bracketed paste:
// приложение во вкладке должно отличать вставку от набора с клавиатуры.
void term_paste(Term *t, const char *text, size_t len);

// Написать прямо на экран терминала, минуя дочерний процесс.
//
// Нужно для того, что показывает сам терминал, а не программа внутри: паспорт
// проекта при открытии вкладки. Через pty это выглядело бы как команда,
// которую якобы набрал человек, — с эхом и следом в истории оболочки.
void term_feed(Term *t, const char *data, size_t len);

// Сообщить о смене фокуса окна — но только если приложение включило
// focus reporting (DECSET 1004), иначе в шелл прилетят лишние CSI I / CSI O.
void term_set_focus(Term *t, bool focused);

// Цвета терминала по умолчанию — пока приложение внутри не задало свои
// через escape-последовательности.
void term_set_default_colors(Term *t, GhosttyColorRgb bg, GhosttyColorRgb fg);
// Курсор и шестнадцать цветов ANSI; остальные 240 остаются палитрой
// libghostty по умолчанию. OSC-переопределения приложений сохраняются.
void term_set_palette(Term *t, GhosttyColorRgb cursor, const GhosttyColorRgb ansi[16]);

// Обновить снимок для отрисовки. Дальше рисующему коду терминал не нужен.
void term_update_render_state(Term *t);

// Есть ли на экране (в видимых строках) такая ASCII-строка. Обновляет
// снимок экрана и проходит по ячейкам: цена — как у одного кадра
// отрисовки без рисования. Нужна, чтобы отличить сжатие контекста от
// работы: снаружи Claude Code и там и там «busy», а на экране пишет
// «Compacting conversation».
bool term_screen_has(Term *t, const char *needle);

// true, если сессия ещё жива.
static inline bool term_alive(const Term *t) { return !t->child_exited; }

#endif // BERTH_TERM_H
