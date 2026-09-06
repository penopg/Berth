#include <stddef.h>

#include "sprite.h"
#include "sprites_data.h"

bool sprites_load(Sprites *s)
{
    Image img = LoadImageFromMemory(".png", sprites_png, (int)sizeof(sprites_png));
    if (!img.data) return false;
    s->atlas = LoadTextureFromImage(img);
    UnloadImage(img);
    // Пиксельная графика: соседние пиксели не смешиваем, иначе NES-фигурка
    // расплывается в кашу при любом увеличении.
    SetTextureFilter(s->atlas, TEXTURE_FILTER_POINT);
    s->ready = s->atlas.id != 0;
    return s->ready;
}

void sprites_unload(Sprites *s)
{
    if (s->ready) UnloadTexture(s->atlas);
    s->ready = false;
}

int sprite_frames_of(SpriteChar ch, int anim)
{
    if (ch < 0 || ch >= SPRITE_CHAR_COUNT || anim < 0 || anim >= SPRITE_ANIM_COUNT) return 0;
    return sprite_anims[ch][anim].count;
}

const SpriteFrame *sprite_frame(SpriteChar ch, int anim, int index)
{
    int n = sprite_frames_of(ch, anim);
    if (n <= 0) return NULL;
    if (index < 0) index = 0;
    if (index >= n) index = n - 1;
    return &sprite_frames[sprite_anims[ch][anim].first + index];
}

void sprite_draw(const Sprites *s, SpriteChar ch, int anim, int index,
                 float x, float floor, float scale, bool flip, Color tint)
{
    if (!s->ready) return;
    const SpriteFrame *f = sprite_frame(ch, anim, index);
    if (!f) return;

    Rectangle src = { f->x, f->y, flip ? -(float)f->w : (float)f->w, (float)f->h };
    // Якорь при отражении уходит в зеркальную точку кадра.
    float ax = flip ? (float)(f->w - f->ax) : (float)f->ax;
    Rectangle dst = { x - ax * scale, floor - (float)f->h * scale,
                      (float)f->w * scale, (float)f->h * scale };
    DrawTexturePro(s->atlas, src, dst, (Vector2){ 0, 0 }, 0.0f, tint);
}
