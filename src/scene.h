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
    SCENE_COMPACT,    // сжатие контекста: не бой, а уборка — герой ходит,
                      // противник ждёт; счёт боя не сбрасывается
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

// Герой тоже не вечен на сцене: выходит слева, когда начинается работа, и
// уходит, постояв немного после неё. Сцена — индикатор работы, стоять без
// дела ему незачем; пустая сцена — обычная строка панели.
typedef enum { HERO_NONE = 0, HERO_ENTER, HERO_STAY, HERO_LEAVE } HeroPhase;

typedef struct {
    SceneMood  mood;
    SceneActor hero, enemy;
    EnemyPhase enemy_phase;
    HeroPhase  hero_phase;
    float      clock;            // время с последней смены настроения
    float      idle;             // сколько герой стоит без дела
    float      hero_next_attack, enemy_next_attack;
    float      lying;            // сколько поверженный противник уже лежит
    unsigned   rng;

    // Счёт: токены, которые агент написал. score — всего у разговора,
    // fight_from — сколько было в начале боя, result — итог последнего
    // боя, показывается после победы.
    long       score, fight_from, result;
    float      bump;             // секунд до конца подскока счётчика после приращения

    // Попадание: звёздочка в точке удара героя. Загорается с задержкой —
    // когда рука или нога доходит до крайнего кадра, — и гаснет сама.
    float      hit_delay, hit_t, hit_x, hit_y;
    float      since_hit;        // сколько герой не бил: без токенов бьёт сам, редко

    // Поток вывода агента: байты из pty идут в реальном времени, в отличие
    // от записей jsonl, которые появляются по одной на готовое сообщение.
    // По потоку герой бьёт (бам-бам-бам, пока текст льётся). Число при
    // этом — только правда из записей: оценка по байтам завышала и потом
    // откатывалась. Чтобы правда не прыгала скачком, shown догоняет её.
    float      flow;             // байт вывода, ещё не отработанных ударами
    long       shown;            // что показывает счётчик боя сейчас
    float      accrual;          // дробный остаток оценки из потока
    int        pace_dir;         // куда герой идёт во время сжатия: +1 вправо
} Scene;

// Вывод процесса за кадр, байт. Зовётся каждый кадр.
void scene_activity(Scene *sc, unsigned long bytes, float dt);

void scene_score(Scene *sc, long score);
static inline long scene_fight_score(const Scene *sc) { return sc->shown; }

void scene_init(Scene *sc);
void scene_set(Scene *sc, SceneMood mood);   // повторный вызов с тем же — ничего
void scene_update(Scene *sc, float dt);
bool scene_active(const Scene *sc);   // на сцене кто-то есть — строке нужна высота

// Ширина сцены в пикселях спрайта: столько места нужно справа от якоря
// героя, чтобы противник помещался.
#define SCENE_WIDTH  64
#define SCENE_HEIGHT 48

// x, floor — экранная точка якоря героя; scale — увеличение.
// hit — цвет вспышки попадания.
void scene_draw(const Scene *sc, const Sprites *sp, float x, float floor, float scale, Color hit);

const char *scene_mood_name(SceneMood m);

#endif // BERTH_SCENE_H
