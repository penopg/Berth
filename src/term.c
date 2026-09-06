// Сессия терминала. Колбэки-эффекты и декодер PNG — производное от main.c
// проекта Ghostling (MIT); жизненный цикл Term написан заново.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "raylib.h"

#include "term.h"
#include "pty.h"

// ---------------------------------------------------------------------------
// Разовая настройка процесса
// ---------------------------------------------------------------------------

// decode_png — decodes raw PNG data into RGBA pixels using Raylib's
// stb_image-based decoder.  The output buffer is allocated through the
// provided GhosttyAllocator so the library can free it later.
static bool decode_png(void *userdata,
                       const GhosttyAllocator *allocator,
                       const uint8_t *data,
                       size_t data_len,
                       GhosttySysImage *out)
{
    (void)userdata;

    // Raylib's LoadImageFromMemory decodes the PNG via stb_image.
    Image img = LoadImageFromMemory(".png", data, (int)data_len);
    if (img.data == NULL) return false;

    // Convert to uncompressed R8G8B8A8 so we have a known pixel layout.
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    const size_t pixel_len = (size_t)img.width * (size_t)img.height * 4;
    uint8_t *pixels = ghostty_alloc(allocator, pixel_len);
    if (!pixels) {
        UnloadImage(img);
        return false;
    }
    memcpy(pixels, img.data, pixel_len);
    UnloadImage(img);

    out->width    = (uint32_t)img.width;
    out->height   = (uint32_t)img.height;
    out->data     = pixels;
    out->data_len = pixel_len;
    return true;
}


// ---------------------------------------------------------------------------
// Эффекты: ответы терминала на запросы приложения. userdata — сам Term.
// ---------------------------------------------------------------------------

// write_pty effect — the terminal calls this whenever a VT sequence
// requires a response back to the application (device status reports,
// mode queries, device attributes, etc.).  Without this, programs like
// vim and tmux that probe terminal capabilities would hang.
static void effect_write_pty(GhosttyTerminal terminal, void *userdata,
                             const uint8_t *data, size_t len)
{
    (void)terminal;
    Term *t = (Term *)userdata;
    pty_write(t->pty_fd, (const char *)data, len);
}

// size effect — responds to XTWINOPS size queries (CSI 14/16/18 t)
// so programs can discover the terminal geometry in cells and pixels.
static bool effect_size(GhosttyTerminal terminal, void *userdata,
                        GhosttySizeReportSize *out_size)
{
    (void)terminal;
    Term *t = (Term *)userdata;
    out_size->rows = t->rows;
    out_size->columns = t->cols;
    out_size->cell_width = (uint32_t)t->cell_width;
    out_size->cell_height = (uint32_t)t->cell_height;
    return true;
}

// device_attributes effect — responds to DA1/DA2/DA3 queries so
// terminal applications can identify the terminal's capabilities.
// We report VT220-level conformance with a modest feature set.
static bool effect_device_attributes(GhosttyTerminal terminal, void *userdata,
                                     GhosttyDeviceAttributes *out_attrs)
{
    (void)terminal;
    (void)userdata;

    // DA1: VT220-level with a few common features.
    out_attrs->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
    out_attrs->primary.features[0] = GHOSTTY_DA_FEATURE_COLUMNS_132;
    out_attrs->primary.features[1] = GHOSTTY_DA_FEATURE_SELECTIVE_ERASE;
    out_attrs->primary.features[2] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
    out_attrs->primary.num_features = 3;

    // DA2: VT220-type, version 1, no ROM cartridge.
    out_attrs->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
    out_attrs->secondary.firmware_version = 1;
    out_attrs->secondary.rom_cartridge = 0;

    // DA3: arbitrary unit id.
    out_attrs->tertiary.unit_id = 0;

    return true;
}

// xtversion effect — responds to CSI > q with our application name.
static GhosttyString effect_xtversion(GhosttyTerminal terminal, void *userdata)
{
    (void)terminal;
    (void)userdata;
    return (GhosttyString){ .ptr = (const uint8_t *)"berth", .len = 5 };
}

// title_changed effect — приложение выставило заголовок через OSC 0/2.
// Демо звало здесь SetWindowTitle; при нескольких вкладках так нельзя —
// заголовок окна принадлежит активной сессии, а не той, что первой пикнула.
static void effect_title_changed(GhosttyTerminal terminal, void *userdata)
{
    Term *t = (Term *)userdata;
    GhosttyString title = {0};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title) != GHOSTTY_SUCCESS)
        return;

    size_t len = title.len < sizeof(t->title) - 1 ? title.len : sizeof(t->title) - 1;
    memcpy(t->title, title.ptr, len);
    t->title[len] = '\0';
    t->title_changed = true;
}

// color_scheme effect — responds to CSI ? 996 n.  Raylib has no API to
// query the OS color scheme, so we return false to silently ignore the
// query rather than guessing.
static bool effect_color_scheme(GhosttyTerminal terminal, void *userdata,
                                GhosttyColorScheme *out_scheme)
{
    (void)terminal;
    (void)userdata;
    (void)out_scheme;
    return false;
}

// ---------------------------------------------------------------------------
// Жизненный цикл сессии
// ---------------------------------------------------------------------------

void term_global_init(void)
{
    // Декодер PNG для Kitty-графики ставится один раз на процесс и
    // обязательно до создания первого терминала.
    ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG, (const void *)decode_png);
}

// Регистрирует колбэки-эффекты. Без них запросы возможностей (device
// attributes, размеры, режимы), которые шлют vim, tmux и htop при старте,
// молча теряются, и такие программы либо виснут, либо работают урезанно.
static void term_install_effects(Term *t)
{
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_USERDATA, t);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
        (const void *)effect_write_pty);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_SIZE,
        (const void *)effect_size);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
        (const void *)effect_device_attributes);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_XTVERSION,
        (const void *)effect_xtversion);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
        (const void *)effect_title_changed);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,
        (const void *)effect_color_scheme);
}

// Включает Kitty-графику. Без лимита хранилища терминал отвергает любые
// картинки; помимо inline-передачи разрешаем файл, временный файл и shm.
static void term_enable_kitty_graphics(Term *t)
{
    uint64_t storage_limit = 64 * 1024 * 1024;
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,
        &storage_limit);

    bool enabled = true;
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &enabled);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE, &enabled);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM, &enabled);
}

// Обёртка, чтобы не повторять один и тот же if-fail-goto десять раз подряд.
#define TERM_TRY(expr, what)                                       \
    do {                                                           \
        if ((expr) != GHOSTTY_SUCCESS) {                           \
            fprintf(stderr, "term_open: %s failed\n", (what));      \
            goto fail;                                             \
        }                                                          \
    } while (0)

bool term_open(Term *t, const TermOpts *opts)
{
    memset(t, 0, sizeof(*t));
    t->pty_fd = -1;
    t->child = -1;
    t->child_status = -1;

    t->cols = opts->cols ? opts->cols : 80;
    t->rows = opts->rows ? opts->rows : 24;
    t->cell_width  = opts->cell_width  > 0 ? opts->cell_width  : 1;
    t->cell_height = opts->cell_height > 0 ? opts->cell_height : 1;

    TERM_TRY(ghostty_terminal_new(NULL, &t->vt, t->cols, t->rows),
             "ghostty_terminal_new");

    // Скроллбэк по строкам — ориентир, а не точная величина: libghostty
    // выбрасывает историю целыми страницами, и параллельно действует лимит
    // по байтам. Что раньше упрётся, то и сработает.
    size_t scrollback = opts->scrollback_lines ? opts->scrollback_lines : 1000;
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &scrollback);

    // Размер ячейки в пикселях не передаётся конструктором, поэтому первый
    // resize обязателен: иначе размещение Kitty-картинок делит на ноль.
    ghostty_terminal_resize(t->vt, t->cols, t->rows,
                            (uint32_t)t->cell_width, (uint32_t)t->cell_height);

    t->pty_fd = pty_spawn(&t->child, opts->cwd, t->cols, t->rows,
                          t->cell_width, t->cell_height);
    if (t->pty_fd < 0) goto fail;

    term_install_effects(t);
    term_enable_kitty_graphics(t);

    TERM_TRY(ghostty_key_encoder_new(NULL, &t->key_encoder), "key_encoder_new");
    TERM_TRY(ghostty_key_event_new(NULL, &t->key_event), "key_event_new");
    TERM_TRY(ghostty_mouse_encoder_new(NULL, &t->mouse_encoder), "mouse_encoder_new");
    TERM_TRY(ghostty_mouse_event_new(NULL, &t->mouse_event), "mouse_event_new");
    TERM_TRY(ghostty_render_state_new(NULL, &t->render_state), "render_state_new");
    TERM_TRY(ghostty_render_state_row_iterator_new(NULL, &t->row_iter), "row_iterator_new");
    TERM_TRY(ghostty_render_state_row_cells_new(NULL, &t->row_cells), "row_cells_new");
    TERM_TRY(ghostty_kitty_graphics_placement_iterator_new(NULL, &t->placement_iter),
             "kitty_placement_iterator_new");

    return true;

fail:
    term_close(t);
    return false;
}

#undef TERM_TRY

void term_request_close(Term *t)
{
    // Закрытие master fd само по себе шлёт ребёнку SIGHUP от ядра.
    if (t->pty_fd >= 0) {
        close(t->pty_fd);
        t->pty_fd = -1;
    }
    if (t->child > 0 && !t->child_reaped && !t->child_exited)
        kill(t->child, SIGHUP);
}

// Дожидается смерти ребёнка, повышая настойчивость.
//
// Простой блокирующий waitpid здесь не годится: агент по SIGHUP не обязан
// уходить сразу — он может дописывать историю диалога, — а окно в это время
// висит с курсором ожидания. Даём время уйти по-хорошему, потом настаиваем.
static void reap_child(Term *t)
{
    if (t->child <= 0 || t->child_reaped) return;

    const int step_ms = 10;
    const int term_after = 30;   // 0.3 с — столько ждём добровольного выхода
    const int kill_after = 80;   // 0.8 с — дальше уже не спрашиваем

    for (int i = 0; i < 120; i++) {
        if (waitpid(t->child, NULL, WNOHANG) > 0) {
            t->child_reaped = true;
            return;
        }
        if (i == term_after) kill(t->child, SIGTERM);
        if (i == kill_after) kill(t->child, SIGKILL);
        usleep(step_ms * 1000);
    }

    // После SIGKILL ждать уже безопасно: его не игнорируют.
    waitpid(t->child, NULL, 0);
    t->child_reaped = true;
}

void term_close(Term *t)
{
    term_request_close(t);
    reap_child(t);
    t->child = -1;

    if (t->placement_iter) { ghostty_kitty_graphics_placement_iterator_free(t->placement_iter); t->placement_iter = NULL; }
    if (t->row_cells)      { ghostty_render_state_row_cells_free(t->row_cells);   t->row_cells = NULL; }
    if (t->row_iter)       { ghostty_render_state_row_iterator_free(t->row_iter); t->row_iter = NULL; }
    if (t->render_state)   { ghostty_render_state_free(t->render_state);          t->render_state = NULL; }
    if (t->mouse_event)    { ghostty_mouse_event_free(t->mouse_event);            t->mouse_event = NULL; }
    if (t->mouse_encoder)  { ghostty_mouse_encoder_free(t->mouse_encoder);        t->mouse_encoder = NULL; }
    if (t->key_event)      { ghostty_key_event_free(t->key_event);                t->key_event = NULL; }
    if (t->key_encoder)    { ghostty_key_encoder_free(t->key_encoder);            t->key_encoder = NULL; }
    if (t->vt)             { ghostty_terminal_free(t->vt);                        t->vt = NULL; }
}

void term_resize(Term *t, uint16_t cols, uint16_t rows,
                 int cell_width, int cell_height)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cell_width  < 1) cell_width  = 1;
    if (cell_height < 1) cell_height = 1;

    if (cols == t->cols && rows == t->rows &&
        cell_width == t->cell_width && cell_height == t->cell_height)
        return;

    t->cols = cols;
    t->rows = rows;
    t->cell_width = cell_width;
    t->cell_height = cell_height;

    ghostty_terminal_resize(t->vt, cols, rows,
                            (uint32_t)cell_width, (uint32_t)cell_height);

    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = (unsigned short)(cols * cell_width),
        .ws_ypixel = (unsigned short)(rows * cell_height),
    };
    ioctl(t->pty_fd, TIOCSWINSZ, &ws);
}

void term_poll(Term *t, double budget_s)
{
    if (!t->child_exited) {
        unsigned long got = 0;
        if (pty_read(t->pty_fd, t->vt, budget_s, &got) != PTY_READ_OK)
            t->child_exited = true;
        t->bytes_in += got;
    }

    // EOF на pty может прийти раньше, чем ребёнок станет ожидаемым, поэтому
    // одной попытки WNOHANG в момент EOF мало — пробуем каждый кадр.
    if (t->child_exited && !t->child_reaped) {
        int wstatus = 0;
        if (waitpid(t->child, &wstatus, WNOHANG) > 0) {
            t->child_reaped = true;
            if (WIFEXITED(wstatus))
                t->child_status = WEXITSTATUS(wstatus);
            else if (WIFSIGNALED(wstatus))
                t->child_status = 128 + WTERMSIG(wstatus);
        }
    }
}

void term_send(Term *t, const char *buf, size_t len)
{
    if (t->child_exited || len == 0) return;
    pty_write(t->pty_fd, buf, len);
}

void term_set_default_colors(Term *t, GhosttyColorRgb bg, GhosttyColorRgb fg)
{
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
}

void term_set_palette(Term *t, GhosttyColorRgb cursor, const GhosttyColorRgb ansi[16])
{
    GhosttyColorRgb palette[256];
    ghostty_color_palette_default(palette);
    for (int i = 0; i < 16; i++) palette[i] = ansi[i];
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cursor);
}

void term_feed(Term *t, const char *data, size_t len)
{
    if (len == 0) return;
    ghostty_terminal_vt_write(t->vt, (const uint8_t *)data, len);
}

void term_paste(Term *t, const char *text, size_t len)
{
    if (t->child_exited || !text || len == 0) return;

    // Bracketed paste (DEC 2004): приложение включает режим, когда хочет
    // отличать вставку от набора с клавиатуры. Оболочка тогда не исполняет
    // многострочное сразу, редактор не тянет автоотступ, а Claude Code по
    // этим же маркерам понимает, что ему вставили путь к картинке, а не
    // печатают его вручную.
    GhosttyTerminalModeConfig mode = {
        .mode = GHOSTTY_MODE_BRACKETED_PASTE,
        .value = false,
    };
    bool bracketed = ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_MODE, &mode)
                         == GHOSTTY_SUCCESS && mode.value;

    if (bracketed) pty_write(t->pty_fd, "\x1b[200~", 6);
    pty_write(t->pty_fd, text, len);
    if (bracketed) pty_write(t->pty_fd, "\x1b[201~", 6);
}

void term_set_focus(Term *t, bool focused)
{
    if (t->child_exited || focused == t->focused) {
        t->focused = focused;
        return;
    }
    t->focused = focused;

    GhosttyTerminalModeConfig focus_mode = {
        .mode = GHOSTTY_MODE_FOCUS_EVENT,
        .value = false,
    };
    if (ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_MODE, &focus_mode)
            != GHOSTTY_SUCCESS || !focus_mode.value)
        return;

    char buf[8];
    size_t written = 0;
    if (ghostty_focus_encode(focused ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST,
                             buf, sizeof(buf), &written) == GHOSTTY_SUCCESS
        && written > 0)
        pty_write(t->pty_fd, buf, written);
}

void term_update_render_state(Term *t)
{
    ghostty_render_state_update(t->render_state, t->vt);
}

// Общий проход по строкам экрана: каждая строка сводится к ASCII (глиф вне
// ASCII — «?») и отдаётся проверке; первая удачная — ответ.
static bool screen_scan(Term *t, bool (*match)(const char *line, const void *arg),
                        const void *arg)
{
    if (!t->vt || !t->render_state) return false;
    ghostty_render_state_update(t->render_state, t->vt);

    GhosttyRenderStateRowIterator row_iter = t->row_iter;
    GhosttyRenderStateRowCells    cells    = t->row_cells;
    if (ghostty_render_state_get(t->render_state,
            GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &row_iter) != GHOSTTY_SUCCESS)
        return false;

    char line[1024];
    while (ghostty_render_state_row_iterator_next(row_iter)) {
        if (ghostty_render_state_row_get(row_iter,
                GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells) != GHOSTTY_SUCCESS)
            continue;
        size_t n = 0;
        while (ghostty_render_state_row_cells_next(cells) && n + 1 < sizeof(line)) {
            uint32_t len = 0;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &len);
            if (len == 0) { line[n++] = ' '; continue; }
            uint32_t cps[16];
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
            line[n++] = cps[0] < 128 ? (char)cps[0] : '?';
        }
        line[n] = '\0';
        if (match(line, arg)) return true;
    }
    return false;
}

static bool match_has(const char *line, const void *arg)
{
    return strstr(line, (const char *)arg) != NULL;
}

bool term_screen_has(Term *t, const char *needle)
{
    if (!needle || !*needle) return false;
    return screen_scan(t, match_has, needle);
}

// Строка крутилки Claude Code: глиф, пробел, сообщение, многоточие — и
// больше ничего. Тот же текст посреди ответа агента (например, в разговоре
// про сам берт) так не выглядит: вокруг него другие слова.
static bool match_status(const char *line, const void *arg)
{
    const char *msg = (const char *)arg;
    const char *p = line;
    while (*p == ' ') p++;
    if (*p != '?' || p[1] != ' ') return false;
    p += 2;
    size_t len = strlen(msg);
    if (strncmp(p, msg, len) != 0) return false;
    p += len;
    if (*p == '?') p++;             // многоточие
    while (*p == ' ') p++;
    return *p == '\0';
}

bool term_screen_status(Term *t, const char *msg)
{
    if (!msg || !*msg) return false;
    return screen_scan(t, match_status, msg);
}
