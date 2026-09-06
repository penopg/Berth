// Сценка с каратекой: герой и противник, хореография по настроению.
//
// Настроение — то, что сцене говорят снаружи: свободен, работает, победил,
// зовёт, упал. Всё остальное — вход противника, обмен ударами, падение,
// уход — сцена разыгрывает сама, по своим часам. Она ничего не читает и не
// пишет: на входе состояние и время, на выходе картинка.
#ifndef BERTH_SCENE_H
#define BERTH_SCENE_H

#include "sprite.h"

typedef enum {
    SCENE_IDLE = 0,   // никого не ждём: герой в стойке
    SCENE_FIGHT,      // идёт работа: противник вбегает, бой
    SCENE_WIN,        // работа кончилась хорошо: противник падает, герой ликует
    SCENE_CALL,       // агент зовёт человека: герой кланяется
    SCENE_FAIL,       // работа упала: падает герой
    SCENE_MOOD_COUNT
} SceneMood;

#define SCENE_SCRIPT_MAX 16

// Актёр: персонаж, его текущий сценарий кадров и положение по x в
// пикселях спрайта относительно героя (у героя 0).
typedef struct {
    SpriteChar ch;
    int   anim;
    int   frames[SCENE_SCRIPT_MAX];   // сценарий: какие кадры показывать
    float durs[SCENE_SCRIPT_MAX];     // и сколько каждый держать, секунд
    int   steps, step;
    float t;            // сколько держим текущий шаг
    bool  loop, done;
    bool  flip;         // смотрит влево
    float x;
} SceneActor;

typedef enum { ENEMY_NONE = 0, ENEMY_ENTER, ENEMY_FIGHT, ENEMY_DEAD, ENEMY_LEAVE } EnemyPhase;

typedef struct {
    SceneMood  mood;
    SceneActor hero, enemy;
    EnemyPhase enemy_phase;
    float      clock;            // время с последней смены настроения
    float      hero_next_attack, enemy_next_attack;
    unsigned   rng;
} Scene;

void scene_init(Scene *sc);
void scene_set(Scene *sc, SceneMood mood);   // повторный вызов с тем же — ничего
void scene_update(Scene *sc, float dt);

// Ширина сцены в пикселях спрайта: столько места нужно справа от якоря
// героя, чтобы противник помещался.
#define SCENE_WIDTH  64
#define SCENE_HEIGHT 48

// x, floor — экранная точка якоря героя; scale — увеличение.
void scene_draw(const Scene *sc, const Sprites *sp, float x, float floor, float scale);

const char *scene_mood_name(SceneMood m);

#endif // BERTH_SCENE_H
