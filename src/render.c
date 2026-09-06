// Отрисовка. Производное от main.c проекта Ghostling (MIT).
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "render.h"
#include "utf8.h"

// Deferred texture cleanup — textures uploaded during a frame can't be
// freed until after EndDrawing() flushes the draw commands to the GPU.
#define MAX_DEFERRED_TEXTURES 256
static Texture2D deferred_textures[MAX_DEFERRED_TEXTURES];
static int deferred_texture_count = 0;

static void defer_unload_texture(Texture2D tex)
{
    if (deferred_texture_count < MAX_DEFERRED_TEXTURES)
        deferred_textures[deferred_texture_count++] = tex;
    else
        UnloadTexture(tex); // overflow fallback — may glitch but won't leak
}

void render_flush_deferred(void)
{
    for (int i = 0; i < deferred_texture_count; i++)
        UnloadTexture(deferred_textures[i]);
    deferred_texture_count = 0;
}

// Draw all Kitty graphics placements for a given z-layer.
//
// The layer filter is applied by the iterator itself via
// ghostty_kitty_graphics_placement_iterator_set(), so we only see
// placements matching the requested layer.
//
// WARNING: This is deliberately simple but very inefficient.  Every
// visible image is re-uploaded to the GPU every frame and destroyed
// right after.  A real implementation should cache Texture2D objects
// keyed by image ID and only re-upload when the image is re-transmitted
// or evicted from the terminal's storage.
static void render_kitty_images(GhosttyTerminal terminal,
                                GhosttyKittyGraphics graphics,
                                GhosttyKittyGraphicsPlacementIterator placement_iter,
                                int cell_width, int cell_height,
                                int origin_x, int origin_y,
                                GhosttyKittyPlacementLayer layer)
{
    // Configure the layer filter on the iterator so
    // placement_next() only yields matching placements.
    ghostty_kitty_graphics_placement_iterator_set(placement_iter,
        GHOSTTY_KITTY_GRAPHICS_PLACEMENT_ITERATOR_OPTION_LAYER, &layer);

    // Re-populate the iterator for this layer scan.
    if (ghostty_kitty_graphics_get(graphics,
            GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,
            &placement_iter) != GHOSTTY_SUCCESS)
        return;

    while (ghostty_kitty_graphics_placement_next(placement_iter)) {
        // Look up the image for this placement.
        uint32_t image_id = 0;
        ghostty_kitty_graphics_placement_get(placement_iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IMAGE_ID, &image_id);

        GhosttyKittyGraphicsImage image_handle =
            ghostty_kitty_graphics_image(graphics, image_id);
        if (!image_handle)
            continue;

        // Get viewport-relative position.  Returns NO_VALUE when the
        // placement is entirely off-screen or is a virtual (unicode
        // placeholder) placement, so both cases are handled in one call.
        int32_t vp_col = 0, vp_row = 0;
        if (ghostty_kitty_graphics_placement_viewport_pos(
                placement_iter, image_handle, terminal,
                &vp_col, &vp_row) != GHOSTTY_SUCCESS)
            continue;

        // Read image dimensions and pixel data.  We only handle RGBA
        // (the PNG decoder we registered converts everything to RGBA).
        uint32_t img_w = 0, img_h = 0;
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_WIDTH, &img_w);
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_HEIGHT, &img_h);
        if (img_w == 0 || img_h == 0)
            continue;

        GhosttyKittyImageFormat fmt = GHOSTTY_KITTY_IMAGE_FORMAT_RGBA;
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_FORMAT, &fmt);
        if (fmt != GHOSTTY_KITTY_IMAGE_FORMAT_RGBA)
            continue;

        const uint8_t *data_ptr = NULL;
        size_t data_len = 0;
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR, &data_ptr);
        ghostty_kitty_graphics_image_get(image_handle,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN, &data_len);
        if (!data_ptr || data_len < (size_t)img_w * img_h * 4)
            continue;

        // Compute grid cell count for rendered size.
        uint32_t grid_cols = 0, grid_rows = 0;
        if (ghostty_kitty_graphics_placement_grid_size(
                placement_iter, image_handle, terminal,
                &grid_cols, &grid_rows) != GHOSTTY_SUCCESS)
            continue;
        if (grid_cols == 0 || grid_rows == 0)
            continue;

        uint32_t dest_w = grid_cols * (uint32_t)cell_width;
        uint32_t dest_h = grid_rows * (uint32_t)cell_height;

        // Get the resolved source rectangle (handles "0 = full image"
        // semantics and clamps to image bounds).
        uint32_t src_x = 0, src_y = 0, src_w = 0, src_h = 0;
        if (ghostty_kitty_graphics_placement_source_rect(
                placement_iter, image_handle,
                &src_x, &src_y, &src_w, &src_h) != GHOSTTY_SUCCESS)
            continue;

        // Read the sub-cell pixel offsets.
        uint32_t x_offset = 0, y_offset = 0;
        ghostty_kitty_graphics_placement_get(placement_iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_X_OFFSET, &x_offset);
        ghostty_kitty_graphics_placement_get(placement_iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Y_OFFSET, &y_offset);

        // Upload the RGBA data to a temporary texture, draw, and free.
        Image img = {
            .data    = (void *)(uintptr_t)data_ptr,
            .width   = (int)img_w,
            .height  = (int)img_h,
            .mipmaps = 1,
            .format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        };
        Texture2D tex = LoadTextureFromImage(img);
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);

        int dest_x = origin_x + (int)vp_col * cell_width  + (int)x_offset;
        int dest_y = origin_y + (int)vp_row * cell_height + (int)y_offset;

        Rectangle src_rect = {
            (float)src_x, (float)src_y,
            (float)src_w, (float)src_h
        };
        Rectangle dst_rect = {
            (float)dest_x, (float)dest_y,
            (float)dest_w, (float)dest_h
        };
        DrawTexturePro(tex, src_rect, dst_rect,
                       (Vector2){0, 0}, 0.0f, WHITE);

        defer_unload_texture(tex);
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Render the current terminal screen using the RenderState API.
//
// For each row/cell we read the grapheme codepoints and the cell's style,
// resolve foreground/background colors via the palette, and draw each
// character individually with DrawTextEx.  This supports per-cell colors
// from SGR sequences (bold, 256-color, 24-bit RGB, etc.).
//
// cell_width and cell_height are the measured dimensions of a single
// monospace glyph at the current font size, in screen (logical) pixels.
// font_size is the logical font size (before DPI scaling).
// pad is the pixel margin between the window edges and the terminal grid.
//
// If scrollbar is non-NULL, a scrollbar indicator is drawn on the right
// edge of the window.
// Глифы из запасных шрифтов, отложенные до конца кадра. Их немного — рамки,
// стрелки, значки режимов, — но разбросаны они по всему экрану.
#define DEFERRED_GLYPH_MAX 4096

typedef struct {
    uint32_t cp;
    float    x, y;
    Color    color;
    bool     bold;
} DeferredGlyph;

static DeferredGlyph deferred_glyphs[DEFERRED_GLYPH_MAX];
static int deferred_glyph_count;

static void defer_glyph(uint32_t cp, float x, float y, Color color, bool bold)
{
    if (deferred_glyph_count >= DEFERRED_GLYPH_MAX) return;   // экран столько и не вмещает
    deferred_glyphs[deferred_glyph_count++] = (DeferredGlyph){ cp, x, y, color, bold };
}

// Дорисовывает отложенное, сгруппировав по шрифтам: столько смен текстуры,
// сколько запасок, а не сколько символов.
static void flush_deferred_glyphs(const FontAtlas *font, int font_size)
{
    for (int src = 1; src <= font->fallback_count; src++) {
        for (int i = 0; i < deferred_glyph_count; i++) {
            const DeferredGlyph *g = &deferred_glyphs[i];
            if (font_glyph_source(font, g->cp) != src) continue;
            font_draw_codepoint(font, g->cp, g->x, g->y, (float)font_size, g->color);
            if (g->bold)
                font_draw_codepoint(font, g->cp, g->x + 1, g->y, (float)font_size, g->color);
        }
    }
    deferred_glyph_count = 0;
}

void render_term(Term *t, const FontAtlas *font, Rect view, int font_size, int pad)
{
    GhosttyRenderState            render_state   = t->render_state;
    GhosttyRenderStateRowIterator row_iter       = t->row_iter;
    GhosttyRenderStateRowCells    cells          = t->row_cells;
    GhosttyTerminal               terminal       = t->vt;
    GhosttyKittyGraphicsPlacementIterator placement_iter = t->placement_iter;
    int cell_width  = t->cell_width;
    int cell_height = t->cell_height;

    // Левый верхний угол сетки символов в координатах окна.
    const int origin_x = view.x + pad;
    const int origin_y = view.y + pad;

    // Скроллбар рисуем, только если есть что прокручивать.
    GhosttyTerminalScrollbar scrollbar_val = {0};
    const GhosttyTerminalScrollbar *scrollbar = NULL;
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                             &scrollbar_val) == GHOSTTY_SUCCESS)
        scrollbar = &scrollbar_val;

    // Grab colors (palette, default fg/bg) from the render state so we
    // can resolve palette-indexed cell colors.
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    if (ghostty_render_state_colors_get(render_state, &colors) != GHOSTTY_SUCCESS)
        return;

    // Obtain the Kitty graphics storage from the terminal.  This is a
    // borrowed pointer valid until the next mutating terminal call.
    GhosttyKittyGraphics kitty_gfx = NULL;
    bool has_kitty = (ghostty_terminal_get(terminal,
        GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &kitty_gfx) == GHOSTTY_SUCCESS
        && kitty_gfx != NULL);

    // Populate the row iterator from the current render state snapshot.
    if (ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &row_iter) != GHOSTTY_SUCCESS)
        return;

    // --- Layer 1: images below cell backgrounds (z < INT32_MIN/2) ---
    if (has_kitty && placement_iter) {
        render_kitty_images(terminal, kitty_gfx, placement_iter,
                            cell_width, cell_height, origin_x, origin_y,
                            GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_BG);
    }

    int y = origin_y;

    while (ghostty_render_state_row_iterator_next(row_iter)) {
        // Get the cells for this row (reuses the same cells handle).
        if (ghostty_render_state_row_get(row_iter,
                GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells) != GHOSTTY_SUCCESS)
            continue;

        int x = origin_x;

        while (ghostty_render_state_row_cells_next(cells)) {
            // How many codepoints make up the grapheme? 0 = empty cell.
            uint32_t grapheme_len = 0;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &grapheme_len);

            if (grapheme_len == 0) {
                // The cell has no text, but it might have a background
                // color (e.g. from an erase with a color set).  The
                // BG_COLOR data query resolves content-tag bg colors
                // and palette indices for us, returning INVALID_VALUE
                // when the cell has no background.
                GhosttyColorRgb bg = {0};
                if (ghostty_render_state_row_cells_get(cells,
                        GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS) {
                    DrawRectangle(x, y, cell_width, cell_height,
                                  (Color){ bg.r, bg.g, bg.b, 255 });
                }

                x += cell_width;
                continue;
            }

            // Read the grapheme codepoints.
            uint32_t codepoints[16];
            uint32_t len = grapheme_len < 16 ? grapheme_len : 16;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, codepoints);

            // Resolve foreground and background colors using the new
            // per-cell color queries.  These flatten style colors,
            // content-tag colors, and palette lookups into a single RGB
            // value, returning INVALID_VALUE when the cell has no
            // explicit color (in which case we use the terminal default).
            GhosttyColorRgb fg = colors.foreground;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);

            GhosttyColorRgb bg_rgb = colors.background;
            bool has_bg = ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg_rgb) == GHOSTTY_SUCCESS;

            // Read the style for flags (inverse, bold, italic) — color
            // resolution is handled above via the new API.
            GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);

            // Inverse (reverse video): swap foreground and background colors.
            if (style.inverse) {
                GhosttyColorRgb tmp = fg;
                fg = bg_rgb;
                bg_rgb = tmp;
                has_bg = true;
            }

            Color ray_fg = { fg.r, fg.g, fg.b, 255 };

            // Draw a background rectangle if the cell has a non-default bg
            // or if inverse mode forced a swap.
            if (has_bg) {
                DrawRectangle(x, y, cell_width, cell_height, (Color){ bg_rgb.r, bg_rgb.g, bg_rgb.b, 255 });
            }

            // Italic: apply a simple shear by shifting the top of the glyph
            // to the right.  The offset is proportional to font size so it
            // looks reasonable at any scale.
            int italic_offset = style.italic ? (font_size / 6) : 0;

            // Рисуем по кодпоинтам, а не строкой: DrawTextEx на каждый вызов
            // заново разбирает UTF-8 и ищет глиф линейным перебором атласа, а
            // знакомест на экране — десять тысяч. Здесь и шрифт, и индекс
            // глифа берутся из карты за одно обращение.
            //
            // Все кодпоинты графемы идут в одну точку: сверх базового символа
            // там только комбинирующие знаки, своего знакоместа у них нет.
            float gx = (float)(x + italic_offset);
            for (uint32_t i = 0; i < len; i++) {
                uint32_t cp = font_substitute(codepoints[i]);

                // Символы из запасных шрифтов откладываем на потом: у каждого
                // шрифта своя текстура, а смена текстуры обрывает батч. Рисуя
                // подряд, мы получали по паре тысяч вызовов отрисовки на кадр.
                int src = font_glyph_source(font, cp);
                if (src > 0) {
                    defer_glyph(cp, gx, (float)y, ray_fg, style.bold);
                    continue;
                }
                if (src < 0) continue;

                font_draw_codepoint(font, cp, gx, (float)y, (float)font_size, ray_fg);

                // Bold: рисуем второй раз со сдвигом в пиксель — утолщение
                // штрихов вместо настоящего полужирного начертания.
                if (style.bold)
                    font_draw_codepoint(font, cp, gx + 1, (float)y, (float)font_size, ray_fg);
            }

            x += cell_width;
        }

        // Clear per-row dirty flag after rendering it.
        bool clean = false;
        ghostty_render_state_row_set(row_iter,
            GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &clean);

        y += cell_height;
    }

    // --- Layer 2: images below text (INT32_MIN/2 <= z < 0) ---
    // Drawn after cell backgrounds but before the cursor and any
    // above-text images.  In our single-pass renderer the cell text
    // has already been drawn, but this still achieves the correct
    // visual for the common case where images sit behind text.
    if (has_kitty && placement_iter) {
        render_kitty_images(terminal, kitty_gfx, placement_iter,
                            cell_width, cell_height, origin_x, origin_y,
                            GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_TEXT);
    }

    // Draw the cursor.
    bool cursor_visible = false;
    ghostty_render_state_get(render_state,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cursor_visible);
    bool cursor_in_viewport = false;
    ghostty_render_state_get(render_state,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cursor_in_viewport);

    if (cursor_visible && cursor_in_viewport) {
        uint16_t cx = 0, cy = 0;
        ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
        ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);

        // Draw the cursor using the foreground color (or explicit cursor
        // color if the terminal set one).
        GhosttyColorRgb cur_rgb = colors.foreground;
        if (colors.cursor_has_value)
            cur_rgb = colors.cursor;
        int cur_x = origin_x + cx * cell_width;
        int cur_y = origin_y + cy * cell_height;
        DrawRectangle(cur_x, cur_y, cell_width, cell_height, (Color){ cur_rgb.r, cur_rgb.g, cur_rgb.b, 128 });
    }

    // --- Layer 3: images above text (z >= 0) ---
    if (has_kitty && placement_iter) {
        render_kitty_images(terminal, kitty_gfx, placement_iter,
                            cell_width, cell_height, origin_x, origin_y,
                            GHOSTTY_KITTY_PLACEMENT_LAYER_ABOVE_TEXT);
    }

    // Draw the scrollbar when there is scrollback content to scroll through.
    if (scrollbar && scrollbar->total > scrollbar->len) {
        // Дорожка скроллбара занимает высоту области терминала; ползунок
        // пропорционален видимой доле содержимого.
        const int bar_width = 6;
        const int bar_margin = 2;
        int bar_x = view.x + view.w - bar_width - bar_margin;

        double visible_frac = (double)scrollbar->len / (double)scrollbar->total;
        int thumb_height = (int)(view.h * visible_frac);
        if (thumb_height < 10) thumb_height = 10;

        // Offset: 0 = scrolled all the way up (oldest), total-len =
        // bottom (most recent).  Map to y so bottom-of-viewport aligns
        // with the bottom of the track.
        double scroll_frac = (scrollbar->total > scrollbar->len)
            ? (double)scrollbar->offset / (double)(scrollbar->total - scrollbar->len)
            : 1.0;
        int thumb_y = view.y + (int)(scroll_frac * (view.h - thumb_height));

        DrawRectangle(bar_x, thumb_y, bar_width, thumb_height,
                      (Color){ 200, 200, 200, 128 });
    }

    // Символы из запасных шрифтов дорисовываем в конце, разом по шрифтам.
    flush_deferred_glyphs(font, font_size);

    // Reset global dirty state so the next update reports changes accurately.
    GhosttyRenderStateDirty clean_state = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(render_state,
        GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean_state);
}
