# Третьи стороны

Версии зафиксированы намеренно: libghostty-vt меняется часто, и «просто взять
свежий main» ломало сборку. Обновление пина — отдельный осознанный шаг.

| Компонент | Версия | Лицензия | Зачем |
|---|---|---|---|
| [libghostty-vt](https://github.com/ghostty-org/ghostty) | `f64f4aca2c29b554d111b36c3d946a9bddd159ff` | MIT | ядро терминала: разбор VT, состояние экрана, рефлоу, скроллбэк, Kitty-протоколы |
| [raylib](https://github.com/raysan5/raylib) | `5.5` | Zlib | окно, ввод, отрисовка, атлас шрифта |
| [Ghostling](https://github.com/mitchellh/ghostling) | `63842bf` | MIT | референсная обвязка libghostty; из неё выросли `pty.c`, `input.c`, `render.c` |
| JetBrains Mono | — | OFL 1.1 | вшитый в бинарь моноширинный шрифт |
| [Symbols Nerd Font](https://github.com/ryanoasis/nerd-fonts) | `3.4.0` | MIT (сборка), OFL/MIT у исходных наборов | иконки проектов из приватной области Unicode |

Файлы `src/pty.c`, `src/input.c`, `src/render.c` и часть `src/term.c` —
производные от `main.c` Ghostling. Остальное написано с нуля.
