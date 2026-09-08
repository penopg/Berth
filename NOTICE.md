# Третьи стороны

Версии зафиксированы намеренно: libghostty-vt меняется часто, и «просто взять
свежий main» ломало сборку. Обновление пина — отдельный осознанный шаг.

| Компонент | Версия | Лицензия | Зачем |
|---|---|---|---|
| [libghostty-vt](https://github.com/ghostty-org/ghostty) | `f64f4aca2c29b554d111b36c3d946a9bddd159ff` | MIT | ядро терминала: разбор VT, состояние экрана, рефлоу, скроллбэк, Kitty-протоколы |
| [raylib](https://github.com/raysan5/raylib) | `5.5` | Zlib | окно, ввод, отрисовка, атлас шрифта |
| [Ghostling](https://github.com/mitchellh/ghostling) | `63842bf` | MIT | референсная обвязка libghostty; из неё выросли `pty.c`, `input.c`, `render.c` |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | `2.304` | OFL 1.1 | вшитый в бинарь моноширинный шрифт |
| [Symbols Nerd Font](https://github.com/ryanoasis/nerd-fonts) | `3.4.0` | MIT (сборка), OFL/MIT у исходных наборов | иконки проектов из приватной области Unicode |

Файлы `src/pty.c`, `src/input.c`, `src/render.c` и часть `src/term.c` —
производные от `main.c` Ghostling. Остальное написано с нуля.

## Ghostling — уведомление об авторских правах

Раньше этот абзац стоял в `LICENSE`, из-за чего GitHub не опознавал лицензию
проекта и показывал «Other». Сам файл лицензии теперь содержит только текст
MIT, а требуемое MIT уведомление о производном коде живёт здесь.

    MIT License

    Copyright (c) 2026 Mitchell Hashimoto

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
