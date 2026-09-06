// Ввод. Производное от main.c проекта Ghostling (MIT).
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "raylib.h"

#include "input.h"
#include "utf8.h"
#include "pty.h"

#if defined(__APPLE__)
#include "macos.h"
#endif

// Map a raylib key constant to a GhosttyKey code.
// Returns GHOSTTY_KEY_UNIDENTIFIED for keys we don't handle.
static GhosttyKey raylib_key_to_ghostty(int rl_key)
{
    // Letters — raylib KEY_A..KEY_Z are contiguous, and so are
    // GHOSTTY_KEY_A..GHOSTTY_KEY_Z.
    if (rl_key >= KEY_A && rl_key <= KEY_Z)
        return GHOSTTY_KEY_A + (rl_key - KEY_A);

    // Digits — raylib KEY_ZERO..KEY_NINE are contiguous.
    if (rl_key >= KEY_ZERO && rl_key <= KEY_NINE)
        return GHOSTTY_KEY_DIGIT_0 + (rl_key - KEY_ZERO);

    // Function keys — raylib KEY_F1..KEY_F12 are contiguous.
    if (rl_key >= KEY_F1 && rl_key <= KEY_F12)
        return GHOSTTY_KEY_F1 + (rl_key - KEY_F1);

    switch (rl_key) {
    case KEY_SPACE:       return GHOSTTY_KEY_SPACE;
    case KEY_ENTER:       return GHOSTTY_KEY_ENTER;
    case KEY_TAB:         return GHOSTTY_KEY_TAB;
    case KEY_BACKSPACE:   return GHOSTTY_KEY_BACKSPACE;
    case KEY_DELETE:      return GHOSTTY_KEY_DELETE;
    case KEY_ESCAPE:      return GHOSTTY_KEY_ESCAPE;
    case KEY_UP:          return GHOSTTY_KEY_ARROW_UP;
    case KEY_DOWN:        return GHOSTTY_KEY_ARROW_DOWN;
    case KEY_LEFT:        return GHOSTTY_KEY_ARROW_LEFT;
    case KEY_RIGHT:       return GHOSTTY_KEY_ARROW_RIGHT;
    case KEY_HOME:        return GHOSTTY_KEY_HOME;
    case KEY_END:         return GHOSTTY_KEY_END;
    case KEY_PAGE_UP:     return GHOSTTY_KEY_PAGE_UP;
    case KEY_PAGE_DOWN:   return GHOSTTY_KEY_PAGE_DOWN;
    case KEY_INSERT:      return GHOSTTY_KEY_INSERT;
    case KEY_MINUS:       return GHOSTTY_KEY_MINUS;
    case KEY_EQUAL:       return GHOSTTY_KEY_EQUAL;
    case KEY_LEFT_BRACKET:  return GHOSTTY_KEY_BRACKET_LEFT;
    case KEY_RIGHT_BRACKET: return GHOSTTY_KEY_BRACKET_RIGHT;
    case KEY_BACKSLASH:   return GHOSTTY_KEY_BACKSLASH;
    case KEY_SEMICOLON:   return GHOSTTY_KEY_SEMICOLON;
    case KEY_APOSTROPHE:  return GHOSTTY_KEY_QUOTE;
    case KEY_COMMA:       return GHOSTTY_KEY_COMMA;
    case KEY_PERIOD:      return GHOSTTY_KEY_PERIOD;
    case KEY_SLASH:       return GHOSTTY_KEY_SLASH;
    case KEY_GRAVE:       return GHOSTTY_KEY_BACKQUOTE;
    default:              return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

bool input_mod_down(InputMod mod)
{
#if defined(__APPLE__)
    unsigned flags = macos_modifier_flags();
    switch (mod) {
    case INPUT_MOD_SHIFT: return (flags & MACOS_MOD_SHIFT) != 0;
    case INPUT_MOD_CTRL:  return (flags & MACOS_MOD_CTRL)  != 0;
    case INPUT_MOD_ALT:   return (flags & MACOS_MOD_ALT)   != 0;
    case INPUT_MOD_SUPER: return (flags & MACOS_MOD_SUPER) != 0;
    }
    return false;
#else
    switch (mod) {
    case INPUT_MOD_SHIFT: return IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT);
    case INPUT_MOD_CTRL:  return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    case INPUT_MOD_ALT:   return IsKeyDown(KEY_LEFT_ALT)     || IsKeyDown(KEY_RIGHT_ALT);
    case INPUT_MOD_SUPER: return IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER);
    }
    return false;
#endif
}

// Build a GhosttyMods bitmask from the current modifier key state.
static GhosttyMods get_ghostty_mods(void)
{
    GhosttyMods mods = 0;
    if (input_mod_down(INPUT_MOD_SHIFT)) mods |= GHOSTTY_MODS_SHIFT;
    if (input_mod_down(INPUT_MOD_CTRL))  mods |= GHOSTTY_MODS_CTRL;
    if (input_mod_down(INPUT_MOD_ALT))   mods |= GHOSTTY_MODS_ALT;
    if (input_mod_down(INPUT_MOD_SUPER)) mods |= GHOSTTY_MODS_SUPER;
    return mods;
}

// Return the unshifted Unicode codepoint for a raylib key, i.e. the
// character the key produces with no modifiers on a US layout.  The
// Kitty keyboard protocol requires this to identify keys.  Returns 0
// for keys that don't have a natural codepoint (arrows, F-keys, etc.).
static uint32_t raylib_key_unshifted_codepoint(int rl_key)
{
    if (rl_key >= KEY_A && rl_key <= KEY_Z)
        return 'a' + (uint32_t)(rl_key - KEY_A);
    if (rl_key >= KEY_ZERO && rl_key <= KEY_NINE)
        return '0' + (uint32_t)(rl_key - KEY_ZERO);

    switch (rl_key) {
    case KEY_SPACE:          return ' ';
    case KEY_MINUS:          return '-';
    case KEY_EQUAL:          return '=';
    case KEY_LEFT_BRACKET:   return '[';
    case KEY_RIGHT_BRACKET:  return ']';
    case KEY_BACKSLASH:      return '\\';
    case KEY_SEMICOLON:      return ';';
    case KEY_APOSTROPHE:     return '\'';
    case KEY_COMMA:          return ',';
    case KEY_PERIOD:         return '.';
    case KEY_SLASH:          return '/';
    case KEY_GRAVE:          return '`';
    default:                 return 0;
    }
}

// Map a raylib mouse button to a GhosttyMouseButton.
static GhosttyMouseButton raylib_mouse_to_ghostty(int rl_button)
{
    switch (rl_button) {
    case MOUSE_BUTTON_LEFT:    return GHOSTTY_MOUSE_BUTTON_LEFT;
    case MOUSE_BUTTON_RIGHT:   return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case MOUSE_BUTTON_MIDDLE:  return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case MOUSE_BUTTON_SIDE:    return GHOSTTY_MOUSE_BUTTON_FOUR;
    case MOUSE_BUTTON_EXTRA:   return GHOSTTY_MOUSE_BUTTON_FIVE;
    case MOUSE_BUTTON_FORWARD: return GHOSTTY_MOUSE_BUTTON_SIX;
    case MOUSE_BUTTON_BACK:    return GHOSTTY_MOUSE_BUTTON_SEVEN;
    default:                   return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

// Encode a mouse event and write the resulting escape sequence to the pty.
// If the encoder produces no output (e.g. tracking is disabled), this is
// a no-op.
static void mouse_encode_and_write(int pty_fd, GhosttyMouseEncoder encoder,
                                   GhosttyMouseEvent event)
{
    char buf[128];
    size_t written = 0;
    GhosttyResult res = ghostty_mouse_encoder_encode(
        encoder, event, buf, sizeof(buf), &written);
    if (res == GHOSTTY_SUCCESS && written > 0)
        pty_write(pty_fd, buf, written);
}

// Poll raylib for mouse events and use the libghostty mouse encoder
// to produce the correct VT escape sequences, which are then written
// to the pty.  The encoder handles tracking mode (X10, normal, button,
// any-event) and output format (X10, UTF8, SGR, URxvt, SGR-Pixels)
// based on what the terminal application has requested.
void input_mouse(Term *t, Rect view, int pad)
{
    int pty_fd                   = t->pty_fd;
    GhosttyMouseEncoder encoder  = t->mouse_encoder;
    GhosttyMouseEvent   event    = t->mouse_event;
    GhosttyTerminal     terminal = t->vt;
    int cell_width  = t->cell_width;
    int cell_height = t->cell_height;

    // Sync encoder tracking mode and format from terminal state so
    // mode changes (e.g. applications enabling SGR mouse reporting)
    // are honoured automatically.
    ghostty_mouse_encoder_setopt_from_terminal(encoder, terminal);

    // Provide the encoder with the current terminal geometry so it
    // can convert pixel positions to cell coordinates.
    GhosttyMouseEncoderSize enc_size = {
        .size          = sizeof(GhosttyMouseEncoderSize),
        .screen_width  = (uint32_t)view.w,
        .screen_height = (uint32_t)view.h,
        .cell_width    = (uint32_t)cell_width,
        .cell_height   = (uint32_t)cell_height,
        .padding_top   = (uint32_t)pad,
        .padding_bottom = (uint32_t)pad,
        .padding_left  = (uint32_t)pad,
        .padding_right = (uint32_t)pad,
    };
    ghostty_mouse_encoder_setopt(encoder,
        GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &enc_size);

    // Track whether any button is currently held — the encoder uses
    // this to distinguish drags from plain motion.
    bool any_pressed = IsMouseButtonDown(MOUSE_BUTTON_LEFT)
                    || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)
                    || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    ghostty_mouse_encoder_setopt(encoder,
        GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &any_pressed);

    // Enable motion deduplication so the encoder suppresses redundant
    // motion events within the same cell.
    bool track_cell = true;
    ghostty_mouse_encoder_setopt(encoder,
        GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &track_cell);

    GhosttyMods mods = get_ghostty_mods();
    // Координаты переводим в систему области терминала: приложение внутри
    // не знает ни про боковую панель, ни про соседние вкладки.
    Vector2 pos = GetMousePosition();
    ghostty_mouse_event_set_mods(event, mods);
    ghostty_mouse_event_set_position(event,
        (GhosttyMousePosition){ .x = pos.x - view.x, .y = pos.y - view.y });

    // Check each mouse button for press/release events.
    static const int buttons[] = {
        MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE,
        MOUSE_BUTTON_SIDE, MOUSE_BUTTON_EXTRA, MOUSE_BUTTON_FORWARD,
        MOUSE_BUTTON_BACK,
    };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        int rl_btn = buttons[i];
        GhosttyMouseButton gbtn = raylib_mouse_to_ghostty(rl_btn);
        if (gbtn == GHOSTTY_MOUSE_BUTTON_UNKNOWN)
            continue;

        if (IsMouseButtonPressed(rl_btn)) {
            ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_PRESS);
            ghostty_mouse_event_set_button(event, gbtn);
            mouse_encode_and_write(pty_fd, encoder, event);
        } else if (IsMouseButtonReleased(rl_btn)) {
            ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_RELEASE);
            ghostty_mouse_event_set_button(event, gbtn);
            mouse_encode_and_write(pty_fd, encoder, event);
        }
    }

    // Mouse motion — send a motion event with whatever button is held
    // (or no button for pure motion in any-event tracking mode).
    Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f) {
        ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_MOTION);
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            ghostty_mouse_event_set_button(event, GHOSTTY_MOUSE_BUTTON_LEFT);
        else if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            ghostty_mouse_event_set_button(event, GHOSTTY_MOUSE_BUTTON_RIGHT);
        else if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
            ghostty_mouse_event_set_button(event, GHOSTTY_MOUSE_BUTTON_MIDDLE);
        else
            ghostty_mouse_event_clear_button(event);
        mouse_encode_and_write(pty_fd, encoder, event);
    }

    // Scroll wheel handling.  When a mouse tracking mode is active the
    // wheel events are forwarded to the application as button 4/5
    // press+release pairs.  Otherwise we scroll the viewport through
    // the scrollback buffer so the user can review history.
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        // Check whether any mouse tracking mode is enabled.  If so,
        // the application wants to handle scroll events itself.
        bool mouse_tracking = false;
        ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &mouse_tracking);

        if (mouse_tracking) {
            // Forward to the application via the mouse encoder.
            GhosttyMouseButton scroll_btn = (wheel > 0.0f)
                ? GHOSTTY_MOUSE_BUTTON_FOUR
                : GHOSTTY_MOUSE_BUTTON_FIVE;
            ghostty_mouse_event_set_button(event, scroll_btn);
            ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_PRESS);
            mouse_encode_and_write(pty_fd, encoder, event);
            ghostty_mouse_event_set_action(event, GHOSTTY_MOUSE_ACTION_RELEASE);
            mouse_encode_and_write(pty_fd, encoder, event);
        } else {
            // Scroll the viewport through scrollback.  Scroll 3 rows
            // per wheel tick for a comfortable pace.  Delta is negative
            // to scroll up (into history), positive to scroll down.
            int delta = (wheel > 0.0f) ? -3 : 3;
            GhosttyTerminalScrollViewport sv = {
                .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA,
                .value = { .delta = delta },
            };
            ghostty_terminal_scroll_viewport(terminal, sv);
        }
    }
}

// Poll raylib for keyboard events and use the libghostty key encoder
// to produce the correct VT escape sequences, which are then written
// to the pty.  The encoder respects terminal modes (cursor key
// application mode, Kitty keyboard protocol, etc.) so we don't need
// to maintain our own escape-sequence tables.
void input_keys(Term *t)
{
    int pty_fd                 = t->pty_fd;
    GhosttyKeyEncoder encoder  = t->key_encoder;
    GhosttyKeyEvent   event    = t->key_event;
    GhosttyTerminal   terminal = t->vt;

    // Sync encoder options from the terminal so mode changes (e.g.
    // application cursor keys, Kitty keyboard protocol) are honoured.
    ghostty_key_encoder_setopt_from_terminal(encoder, terminal);

    // Символы, набранные за кадр. Держим их по одному, а не одной строкой:
    // раздавать надо ровно по символу на нажатие. Слепив их вместе, мы отдавали
    // весь текст первой клавише кадра, а остальные уходили в приложение без
    // текста — и кодировщик подставлял им латиницу по физической клавише.
    // При русской раскладке это и выглядело как случайные чужие буквы.
    uint32_t chars[32];
    int char_count = 0;
    int next_char = 0;
    int ch;
    while ((ch = GetCharPressed()) != 0) {
        if (char_count < (int)(sizeof(chars) / sizeof(chars[0])))
            chars[char_count++] = (uint32_t)ch;
    }

    // All raylib keys we want to check for press/repeat events.
    // Letters and digits are handled via ranges; everything else is
    // enumerated explicitly.
    static const int special_keys[] = {
        KEY_SPACE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_DELETE,
        KEY_ESCAPE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
        KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_INSERT,
        KEY_MINUS, KEY_EQUAL, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET,
        KEY_BACKSLASH, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_COMMA,
        KEY_PERIOD, KEY_SLASH, KEY_GRAVE,
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
        KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    };

    // Build the set of raylib keys to scan: letters + digits + specials.
    int keys_to_check[26 + 10 + sizeof(special_keys) / sizeof(special_keys[0])];
    int num_keys = 0;
    for (int k = KEY_A; k <= KEY_Z; k++)
        keys_to_check[num_keys++] = k;
    for (int k = KEY_ZERO; k <= KEY_NINE; k++)
        keys_to_check[num_keys++] = k;
    for (size_t i = 0; i < sizeof(special_keys) / sizeof(special_keys[0]); i++)
        keys_to_check[num_keys++] = special_keys[i];

    GhosttyMods mods = get_ghostty_mods();

    for (int i = 0; i < num_keys; i++) {
        int rl_key = keys_to_check[i];
        bool pressed  = IsKeyPressed(rl_key);
        bool repeated = IsKeyPressedRepeat(rl_key);
        bool released = IsKeyReleased(rl_key);
        if (!pressed && !repeated && !released)
            continue;

        // ⌘V — вставка из буфера обмена. libghostty-vt пока не отдаёт
        // OSC-клипборд (числится в «What Is Coming»), поэтому читаем
        // системный буфер сами и пишем прямо в pty.
        if ((pressed || repeated) && rl_key == KEY_V && input_mod_down(INPUT_MOD_SUPER)) {
#if defined(__APPLE__)
            // Картинку GetClipboardText не видит вовсе. Выкладываем её файлом
            // и печатаем путь: агент во вкладке читает изображение так же, как
            // при перетаскивании файла в окно.
            char img_path[PATH_MAX];
            if (macos_clipboard_image_path(img_path, sizeof img_path)) {
                term_paste(t, img_path, strlen(img_path));
                continue;
            }
#endif
            const char *clip = GetClipboardText();
            if (clip && *clip)
                term_paste(t, clip, strlen(clip));
            continue;
        }

        GhosttyKey gkey = raylib_key_to_ghostty(rl_key);
        if (gkey == GHOSTTY_KEY_UNIDENTIFIED)
            continue;

        GhosttyKeyAction action = released  ? GHOSTTY_KEY_ACTION_RELEASE
                                : pressed   ? GHOSTTY_KEY_ACTION_PRESS
                                            : GHOSTTY_KEY_ACTION_REPEAT;

        ghostty_key_event_set_key(event, gkey);
        ghostty_key_event_set_action(event, action);
        ghostty_key_event_set_mods(event, mods);

        // The unshifted codepoint is the character the key produces
        // with no modifiers.  The Kitty protocol needs it to identify
        // keys independent of the current shift state.
        uint32_t ucp = raylib_key_unshifted_codepoint(rl_key);
        ghostty_key_event_set_unshifted_codepoint(event, ucp);

        // Consumed mods are modifiers the platform's text input
        // already accounted for when producing the UTF-8 text.
        // For printable keys, shift is consumed (it turns 'a' → 'A').
        // For non-printable keys nothing is consumed.
        GhosttyMods consumed = 0;
        if (ucp != 0 && (mods & GHOSTTY_MODS_SHIFT))
            consumed |= GHOSTTY_MODS_SHIFT;
        ghostty_key_event_set_consumed_mods(event, consumed);

        // Печатная клавиша сама по себе ничего не значит: что она печатает,
        // знает раскладка, и это приходит отдельным символом. Если символа для
        // неё нет — отправлять нечего, иначе в приложение уйдёт буква с
        // клавиатурной крышки вместо набранной.
        bool text_key = ucp >= 32 && ucp != 127
            && !(mods & (GHOSTTY_MODS_CTRL | GHOSTTY_MODS_ALT | GHOSTTY_MODS_SUPER));

        if (text_key && !released) {
            if (next_char >= char_count)
                continue;
            char u8[4];
            int n = utf8_encode(chars[next_char++], u8);
            ghostty_key_event_set_utf8(event, u8, (size_t)n);
        } else {
            ghostty_key_event_set_utf8(event, NULL, 0);
        }

        char buf[128];
        size_t written = 0;
        GhosttyResult res = ghostty_key_encoder_encode(
            encoder, event, buf, sizeof(buf), &written);
        if (res == GHOSTTY_SUCCESS && written > 0)
            pty_write(pty_fd, buf, written);
    }

    // Символы, которым не досталось нажатия: событие клавиши могло прийти
    // кадром раньше символа (так бывает в виртуальных машинах), а терять
    // набранное нельзя. Порядок сохраняем — пишем как есть.
    for (int i = next_char; i < char_count; i++) {
        char u8[4];
        int n = utf8_encode(chars[i], u8);
        pty_write(pty_fd, u8, n);
    }
}

// Handle scrollbar drag-to-scroll.  When the user clicks in the
// scrollbar region we begin tracking; while held we map the mouse Y
// position directly to an absolute scroll offset so the thumb follows
// the cursor exactly.
//
// Returns true while a drag is in progress so the caller can skip
// normal mouse handling if desired.
bool input_scrollbar(Term *t, Rect view)
{
    GhosttyTerminal    terminal     = t->vt;
    GhosttyRenderState render_state = t->render_state;
    bool              *dragging     = &t->scrollbar_dragging;

    // Query scrollbar geometry from the terminal.
    GhosttyTerminalScrollbar scrollbar = {0};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                             &scrollbar) != GHOSTTY_SUCCESS)
        return false;

    // Nothing to drag when the viewport covers all content.
    if (scrollbar.total <= scrollbar.len) {
        *dragging = false;
        return false;
    }

    // Скроллбар живёт у правого края области терминала, а не окна.
    const int bar_width = 6;
    const int bar_margin = 2;
    int bar_right = view.x + view.w;
    int bar_left = bar_right - bar_width - bar_margin;
    // Зона захвата шире самой полоски — так в неё проще попасть.
    int hit_left = bar_left - 8;
    Vector2 mpos = GetMousePosition();

    // Start a drag when the user clicks inside the hit region.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)
        && mpos.x >= hit_left && mpos.x <= bar_right
        && mpos.y >= view.y && mpos.y <= view.y + view.h) {
        *dragging = true;
    }

    if (*dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        // Позицию курсора по Y переводим в абсолютное смещение прокрутки:
        // верх области — начало истории, низ — её конец.
        uint64_t scrollable = scrollbar.total - scrollbar.len;
        double frac = view.h > 0 ? (double)(mpos.y - view.y) / (double)view.h : 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int64_t target = (int64_t)(frac * (double)scrollable);

        intptr_t delta = (intptr_t)(target - (int64_t)scrollbar.offset);
        if (delta != 0) {
            GhosttyTerminalScrollViewport sv = {
                .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA,
                .value = { .delta = delta },
            };
            ghostty_terminal_scroll_viewport(terminal, sv);
            ghostty_render_state_update(render_state, terminal);
        }
    }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
        *dragging = false;

    return *dragging;
}

void input_drain_chars(void)
{
    while (GetCharPressed() != 0) { }
}
