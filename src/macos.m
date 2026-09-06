// Правки системного меню macOS.
//
// GLFW сам собирает стандартное меню приложения, и в нём Close (⌘W) и Quit
// (⌘Q). Cocoa разбирает эти сочетания в меню раньше, чем нажатие доходит до
// окна: ⌘W закрывало всё окно вместо вкладки, а ⌘Q завершал процесс мимо
// нашей уборки, оставляя дочерние оболочки без присмотра.
//
// Отключать меню целиком (GLFW_COCOA_MENUBAR) — слишком грубо: пропадает вся
// строка меню вместе с ⌘H и ⌘M. Поэтому снимаем сочетания только с этих двух
// пунктов, а сами действия обрабатываем в приложении.
#import <Cocoa/Cocoa.h>

#include "macos.h"

static void release_shortcut(NSMenu *menu, NSString *key)
{
    for (NSMenuItem *item in [menu itemArray]) {
        if ([[item keyEquivalent] isEqualToString:key] &&
            ([item keyEquivalentModifierMask] & NSEventModifierFlagCommand)) {
            [item setKeyEquivalent:@""];
        }
        if ([item submenu]) release_shortcut([item submenu], key);
    }
}

void macos_release_window_shortcuts(void)
{
    NSMenu *main = [NSApp mainMenu];
    if (!main) return;
    release_shortcut(main, @"w");
    release_shortcut(main, @"q");
}

void macos_activate_app(void)
{
    // Процесс запущен из терминала и не является бандлом, поэтому окно
    // открывается позади активного приложения. Выводим его вперёд явно.
    [NSApp activateIgnoringOtherApps:YES];
}

void macos_set_dock_icon(const unsigned char *png, size_t len)
{
    NSData *data = [NSData dataWithBytesNoCopy:(void *)png length:len freeWhenDone:NO];
    NSImage *image = [[NSImage alloc] initWithData:data];
    if (image) [NSApp setApplicationIconImage:image];
}

// Вставка картинки. Claude Code принимает изображение путём к файлу — так же,
// как при перетаскивании, — поэтому терминалу достаточно выложить содержимое
// буфера на диск и напечатать путь. Текстовый буфер сюда не попадает: его
// отдаёт raylib.
bool macos_clipboard_image_path(char *out, size_t out_size)
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];

    // Файл, скопированный в Finder, лежит в буфере как file URL. Копировать
    // его незачем — отдаём путь как есть.
    NSArray<NSURL *> *urls =
        [pb readObjectsForClasses:@[[NSURL class]]
                          options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    for (NSURL *url in urls) {
        NSString *ext = [[url pathExtension] lowercaseString];
        if ([@[@"png", @"jpg", @"jpeg", @"gif", @"webp", @"bmp", @"tiff", @"heic"]
                containsObject:ext]) {
            const char *path = [[url path] fileSystemRepresentation];
            if (!path || strlen(path) + 1 > out_size) return false;
            strlcpy(out, path, out_size);
            return true;
        }
    }

    // Скриншот или картинка из браузера приходит данными. TIFF приводим к PNG:
    // так файл меньше и его понимает кто угодно.
    NSData *png = [pb dataForType:NSPasteboardTypePNG];
    if (!png) {
        NSData *tiff = [pb dataForType:NSPasteboardTypeTIFF];
        if (!tiff) return false;
        NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:tiff];
        png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    }
    if (!png) return false;

    NSString *name = [NSString stringWithFormat:@"berth-paste-%.0f.png",
                                                [[NSDate date] timeIntervalSince1970] * 1000];
    NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:name];
    if (![png writeToFile:path atomically:YES]) return false;

    const char *cpath = [path fileSystemRepresentation];
    if (!cpath || strlen(cpath) + 1 > out_size) return false;
    strlcpy(out, cpath, out_size);
    return true;
}

// Состояние модификаторов у системы, а не у оконной библиотеки.
//
// GLFW знает об отпускании клавиши только из события, а системные сочетания
// (⇧⌃⌘4 у снимка экрана) забирают событие себе: окно видит нажатие и не видит
// отпускания. Модификатор «залипает», и дальше каждая набранная буква уходит
// в приложение как Ctrl+буква — у Claude Code это стирало слово за словом.
// NSEvent отдаёт физическое состояние клавиш и залипнуть не может.
unsigned macos_modifier_flags(void)
{
    NSEventModifierFlags f = [NSEvent modifierFlags];
    unsigned mods = 0;
    if (f & NSEventModifierFlagShift)   mods |= MACOS_MOD_SHIFT;
    if (f & NSEventModifierFlagControl) mods |= MACOS_MOD_CTRL;
    if (f & NSEventModifierFlagOption)  mods |= MACOS_MOD_ALT;
    if (f & NSEventModifierFlagCommand) mods |= MACOS_MOD_SUPER;
    return mods;
}

bool macos_choose_folder(const char *start, const char *prompt,
                         char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = '\0';

    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.canCreateDirectories = NO;
        panel.message = @"Выберите папку: её подпапки станут проектами группы";
        if (prompt && *prompt)
            panel.prompt = [NSString stringWithUTF8String:prompt];
        if (start && *start)
            panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:start]];

        // Модальный цикл внутри кадра raylib: GLFW на это время не опрашивает
        // события, и после закрытия окно продолжает как ни в чём не бывало.
        if ([panel runModal] != NSModalResponseOK) return false;
        NSURL *url = panel.URLs.firstObject;
        if (!url) return false;
        snprintf(out, cap, "%s", url.path.fileSystemRepresentation);
    }
    return out[0] != '\0';
}
