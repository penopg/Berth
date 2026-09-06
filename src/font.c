#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"

// Шрифт вшит в бинарь на этапе сборки: заголовок генерирует build.sh из
// fonts/JetBrainsMono-Regular.ttf, чтобы не искать файл в рантайме.
#include "font_jetbrains_mono.h"
// Кодпоинты, которые этот шрифт умеет рисовать — тоже от build.sh, из cmap.
#include "font_codepoints.h"

// Шрифт иконок вкладываем в бинарь ассемблером: .incbin копирует файл в
// секцию данных как есть. Заголовок с массивом байт (как у JetBrains Mono)
// на 2.5 МБ добавлял бы к каждой сборке секунды. Путь подставляет build.sh.
#if defined(__APPLE__) && defined(BERTH_NERD_FONT)
__asm__(".section __TEXT,__const\n"
        ".p2align 4\n"
        ".globl _berth_nerd_font\n"
        "_berth_nerd_font:\n"
        ".incbin \"" BERTH_NERD_FONT "\"\n"
        ".globl _berth_nerd_font_end\n"
        "_berth_nerd_font_end:\n");
extern const unsigned char berth_nerd_font[];
extern const unsigned char berth_nerd_font_end[];
#define HAVE_NERD_FONT 1
#endif

typedef struct { int lo, hi; } CpRange;

// Символика терминальных интерфейсов: ⎿ у ветки результата, ✻ у спиннера,
// ⤷ у переноса. В JetBrains Mono этого нет, в системных шрифтах есть.
// Целиком их cmap брать нельзя (в Arial Unicode десятки тысяч глифов) —
// берём блоки, из которых такая символика и состоит.
static const CpRange symbol_ranges[] = {
    { 0x00A0, 0x00FF },  // латиница с диакритикой
    { 0x2190, 0x21FF },  // стрелки
    { 0x2300, 0x23FF },  // технические: ⎿ ⌘ ⏎
    { 0x25A0, 0x25FF },  // геометрия
    { 0x2600, 0x27BF },  // разное и дингбаты: ✻ ✽ ✔
    { 0x2900, 0x297F },  // дополнительные стрелки: ⤷
    { 0x2B00, 0x2B5F },  // дополнительные символы: ⬡
};

// Иконки Nerd Font: приватная область Unicode, блоки идут с большими дырами,
// поэтому перечисляем именно их, а не весь диапазон E000–F8FF.
static const CpRange nerd_ranges[] = {
    { 0xE000, 0xE00A },  // seti
    { 0xE0A0, 0xE0D7 },  // powerline
    { 0xE200, 0xE2A9 },  // font awesome extension
    { 0xE300, 0xE3E3 },  // weather
    { 0xE5FA, 0xE6B8 },  // custom: папки, языки
    { 0xE700, 0xE8EF },  // devicons
    { 0xEA60, 0xEC1E },  // codicons
    { 0xED00, 0xF381 },  // font awesome
    { 0xF400, 0xF533 },  // octicons
};

// Запаски по порядку поиска. Системные — файлом, шрифт иконок — из памяти.
static const struct {
    const char    *path;    // NULL, если шрифт вшит в бинарь
    const CpRange *ranges;
    size_t         count;
} fallback_fonts[] = {
    { "/System/Library/Fonts/Apple Symbols.ttf",
      symbol_ranges, sizeof(symbol_ranges) / sizeof(symbol_ranges[0]) },
    { "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
      symbol_ranges, sizeof(symbol_ranges) / sizeof(symbol_ranges[0]) },
    { NULL, nerd_ranges, sizeof(nerd_ranges) / sizeof(nerd_ranges[0]) },
};

// Есть ли в шрифте настоящий глиф для кодпоинта. GetGlyphIndex на промахе
// возвращает индекс «?», а LoadFontEx для кодпоинта без глифа заводит запись
// с пустым прямоугольником — проверяем оба случая.
static bool has_glyph(Font f, int cp)
{
    if (f.glyphCount <= 0) return false;
    int i = GetGlyphIndex(f, cp);
    if (i < 0 || i >= f.glyphCount) return false;
    if (f.glyphs[i].value != cp) return false;
    if (f.recs[i].width <= 0 && f.glyphs[i].advanceX <= 0) return false;
    return true;
}

static void load_fallbacks(FontAtlas *a)
{
    static int cps[4096];

    a->fallback_count = 0;
    for (size_t i = 0; i < sizeof(fallback_fonts) / sizeof(fallback_fonts[0]); i++) {
        if (a->fallback_count >= FONT_FALLBACK_MAX) break;

        int n = 0;
        for (size_t r = 0; r < fallback_fonts[i].count; r++)
            for (int c = fallback_fonts[i].ranges[r].lo; c <= fallback_fonts[i].ranges[r].hi; c++)
                if (n < (int)(sizeof(cps) / sizeof(cps[0])))
                    cps[n++] = c;

        Font f = { 0 };
        if (fallback_fonts[i].path) {
            if (!FileExists(fallback_fonts[i].path)) continue;
            f = LoadFontEx(fallback_fonts[i].path, a->size_px, cps, n);
        } else {
#ifdef HAVE_NERD_FONT
            f = LoadFontFromMemory(".ttf", berth_nerd_font,
                                   (int)(berth_nerd_font_end - berth_nerd_font),
                                   a->size_px, cps, n);
#else
            continue;
#endif
        }
        if (f.texture.id == 0) continue;
        SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
        a->fallback[a->fallback_count++] = f;
    }
}

// Карта «кодпоинт → где его глиф». Заполняется один раз на загрузку атласа:
// сначала основной шрифт, потом запаски по порядку — первый, кто умеет
// рисовать символ, тот его и рисует.
static void build_map(FontAtlas *a)
{
    a->map = calloc(FONT_MAP_SIZE, sizeof(GlyphSlot));
    if (!a->map) return;
    for (int i = 0; i < FONT_MAP_SIZE; i++)
        a->map[i].font = -1;

    for (int src = 0; src <= a->fallback_count; src++) {
        const Font *f = src == 0 ? &a->font : &a->fallback[src - 1];
        for (int i = 0; i < f->glyphCount; i++) {
            int cp = f->glyphs[i].value;
            if (cp < 0 || cp >= FONT_MAP_SIZE) continue;
            if (a->map[cp].font >= 0) continue;          // уже занято тем, кто раньше

            // Пустой прямоугольник — заглушка, которую raylib завёл для
            // кодпоинта без глифа. Пробел так выглядит законно, остальное нет.
            if (f->recs[i].width <= 0 && f->glyphs[i].advanceX <= 0) continue;

            a->map[cp].font = (int16_t)src;
            a->map[cp].glyph = (int16_t)i;
        }
    }
}

bool font_load(FontAtlas *a, int logical_size, Vector2 dpi_scale)
{
    a->size = logical_size;
    a->size_px = (int)(logical_size * dpi_scale.y);
    if (a->size_px < 1) a->size_px = logical_size;

    // raylib ругается на каждый глиф, который шире ожидаемой ширины ячейки —
    // для рамок и иконок это норма, а строк набегает на пол-экрана. Молчим на
    // время растеризации.
    SetTraceLogLevel(LOG_ERROR);

    int n = (int)(sizeof(font_codepoints) / sizeof(font_codepoints[0]));
    a->font = LoadFontFromMemory(".ttf", font_jetbrains_mono,
                                 (int)sizeof(font_jetbrains_mono),
                                 a->size_px, (int *)font_codepoints, n);
    if (a->font.texture.id == 0) {
        SetTraceLogLevel(LOG_INFO);
        return false;
    }

    // Билинейная фильтрация: текстура уже в натуральном разрешении, так что
    // размытия от увеличения нет, зато нет и рваных краёв при дробных позициях.
    SetTextureFilter(a->font.texture, TEXTURE_FILTER_BILINEAR);

    load_fallbacks(a);
    build_map(a);

    SetTraceLogLevel(LOG_INFO);

    // Ячейку меряем по представительному глифу. MeasureTextEx возвращает
    // логические пиксели с учётом внутреннего масштаба шрифта, поэтому делим
    // на DPI, чтобы получить размер в координатах экрана.
    Vector2 g = MeasureTextEx(a->font, "M", a->size_px, 0);
    a->cell_width  = (int)(g.x / dpi_scale.x);
    a->cell_height = (int)(g.y / dpi_scale.y);
    if (a->cell_width  < 1) a->cell_width  = 1;
    if (a->cell_height < 1) a->cell_height = 1;

    return true;
}

void font_unload(FontAtlas *a)
{
    free(a->map);
    a->map = NULL;

    for (int i = 0; i < a->fallback_count; i++)
        UnloadFont(a->fallback[i]);
    a->fallback_count = 0;

    if (a->font.texture.id != 0) {
        UnloadFont(a->font);
        a->font = (Font){0};
    }
}

// Где лежит глиф символа. Возвращает NULL, если рисовать нечего.
static const Font *lookup(const FontAtlas *a, uint32_t cp, int *glyph)
{
    if (cp < FONT_MAP_SIZE && a->map) {
        GlyphSlot slot = a->map[cp];
        if (slot.font < 0) return NULL;
        *glyph = slot.glyph;
        return slot.font == 0 ? &a->font : &a->fallback[slot.font - 1];
    }

    // За пределами BMP (эмодзи и прочая экзотика) карты нет: таких символов
    // единицы на экран, и перебор для них дешевле лишних мегабайт памяти.
    if (has_glyph(a->font, (int)cp)) {
        *glyph = GetGlyphIndex(a->font, (int)cp);
        return &a->font;
    }
    for (int i = 0; i < a->fallback_count; i++) {
        if (has_glyph(a->fallback[i], (int)cp)) {
            *glyph = GetGlyphIndex(a->fallback[i], (int)cp);
            return &a->fallback[i];
        }
    }
    return NULL;
}

Font font_for(const FontAtlas *a, const char *utf8)
{
    if (!utf8 || !*utf8) return a->font;

    int len = 0;
    int cp = GetCodepointNext(utf8, &len);
    if (cp <= 0x7F) return a->font;

    int glyph = 0;
    const Font *f = lookup(a, (uint32_t)cp, &glyph);
    return f ? *f : a->font;
}

int font_glyph_source(const FontAtlas *a, uint32_t codepoint)
{
    if (codepoint < FONT_MAP_SIZE && a->map)
        return a->map[codepoint].font;

    int glyph = 0;
    const Font *f = lookup(a, codepoint, &glyph);
    if (!f) return -1;
    if (f == &a->font) return 0;
    return (int)(f - a->fallback) + 1;
}

bool font_draw_codepoint(const FontAtlas *a, uint32_t codepoint,
                         float x, float y, float size, Color tint)
{
    int glyph = 0;
    const Font *f = lookup(a, codepoint, &glyph);
    if (!f) return false;

    Rectangle src = f->recs[glyph];
    if (src.width <= 0 || src.height <= 0) return true;   // пробел: место занял, рисовать нечего

    float scale = size / (float)f->baseSize;
    Rectangle dst = {
        x + (float)f->glyphs[glyph].offsetX * scale,
        y + (float)f->glyphs[glyph].offsetY * scale,
        src.width  * scale,
        src.height * scale,
    };
    DrawTexturePro(f->texture, src, dst, (Vector2){ 0, 0 }, 0.0f, tint);
    return true;
}

uint32_t font_substitute(uint32_t cp)
{
    // Значки управления воспроизведением (U+23E9–23FA) Claude Code ставит
    // перед строкой шага и в статусе режима: ⏺ у шага, ⏵⏵ у accept edits,
    // ⏸ у plan mode. Весь этот блок Unicode считает эмодзи, и в macOS он
    // есть ровно в одном шрифте — цветном Apple Color Emoji, чьи растровые
    // таблицы (sbix) stb_truetype не читает. Подменяем на глифы той же
    // формы: геометрию из JetBrains Mono, а паузу и часы — из Nerd Font.
    switch (cp) {
    case 0x23E9: case 0x23ED: case 0x23EF:
    case 0x23F5: return 0x25B6;  // ⏩ ⏭ ⏯ ⏵ → ▶
    case 0x23EA: case 0x23EE:
    case 0x23F4: return 0x25C0;  // ⏪ ⏮ ⏴ → ◀
    case 0x23EB: case 0x23F6: return 0x25B2;  // ⏫ ⏶ → ▲
    case 0x23EC: case 0x23F7: return 0x25BC;  // ⏬ ⏷ → ▼
    case 0x23F0: case 0x23F1: case 0x23F2:
    case 0x23F3: return 0xF017;  // ⏰ ⏱ ⏲ ⏳ → часы Nerd Font
    case 0x23F8: return 0xF04C;  // ⏸ → пауза Nerd Font
    case 0x23F9: return 0x25A0;  // ⏹ → ■
    case 0x23FA: return 0x25CF;  // ⏺ → ●
    default:     return cp;
    }
}
