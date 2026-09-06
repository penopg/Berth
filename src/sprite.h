// Спрайты Karateka: атлас в бинаре, кадры и анимации таблицей.
//
// Кадр рисуется от якоря: точка на полу под центром тени. Так фигуры
// разной ширины (стойка узкая, удар ногой втрое шире) стоят на одном месте
// и не ёрзают между кадрами. Герой в раскладке смотрит вправо, противник —
// влево, на героя; отражение нужно тому, кто разворачивается.
#ifndef BERTH_SPRITE_H
#define BERTH_SPRITE_H

#include <stdbool.h>

#include "raylib.h"

typedef enum { SPRITE_HERO = 0, SPRITE_ENEMY, SPRITE_CHAR_COUNT } SpriteChar;

typedef enum {
    SPR_STANCE = 0, SPR_WALK, SPR_BOW, SPR_RUN, SPR_VICTORY, SPR_PUNCH, SPR_KICK,
    SPR_DEATH, SPRITE_ANIM_COUNT
} SpriteAnim_;

typedef struct { short x, y, w, h, ax; } SpriteFrame;   // ax — якорь по x
typedef struct { short first, count; } SpriteAnim;

typedef struct {
    Texture2D atlas;
    bool      ready;
} Sprites;

bool sprites_load(Sprites *s);       // атлас из бинаря в текстуру; нужен контекст GL
void sprites_unload(Sprites *s);

int  sprite_frames_of(SpriteChar ch, int anim);   // сколько кадров у анимации
const SpriteFrame *sprite_frame(SpriteChar ch, int anim, int index);

// Нарисовать кадр: (x, floor) — экранная точка якоря, scale — целое
// увеличение, flip — смотреть влево.
void sprite_draw(const Sprites *s, SpriteChar ch, int anim, int index,
                 float x, float floor, float scale, bool flip, Color tint);

#endif // BERTH_SPRITE_H
