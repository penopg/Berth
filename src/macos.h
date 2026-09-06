// Мелкие правки поведения macOS. На других системах не собирается и не нужен.
#ifndef BERTH_MACOS_H
#define BERTH_MACOS_H

#include <stdbool.h>
#include <stddef.h>

// Снимает ⌘W и ⌘Q со стандартного меню, чтобы приложение обработало их само.
// Вызывать после создания окна.
void macos_release_window_shortcuts(void);

// Выводит окно на передний план: запущенный из терминала процесс без бандла
// иначе открывается позади активного приложения.
void macos_activate_app(void);
// Иконка процесса в доке — из PNG в памяти. Бандлу не нужна, голому
// бинарю без неё достаётся заглушка.
void macos_set_dock_icon(const unsigned char *png, size_t len);

// Модификаторы, нажатые прямо сейчас, по данным системы. Биты ниже.
#define MACOS_MOD_SHIFT (1u << 0)
#define MACOS_MOD_CTRL  (1u << 1)
#define MACOS_MOD_ALT   (1u << 2)
#define MACOS_MOD_SUPER (1u << 3)
unsigned macos_modifier_flags(void);

// Картинка из системного буфера обмена: сохраняет её во временный PNG и
// кладёт путь в out. Скопированный в Finder файл отдаётся своим путём, без
// копирования. Возвращает false, если картинки в буфере нет.
bool macos_clipboard_image_path(char *out, size_t out_size);

// Системный выбор папки. start — где открыть диалог (NULL — где система
// решит). Возвращает false, если человек отказался.
bool macos_choose_folder(const char *start, const char *prompt,
                         char *out, size_t cap);

#endif // BERTH_MACOS_H
