#include <string.h>

#include "scene.h"

// Темп NES: кадр анимации — восемь в секунду; бег чаще.
static const float FRAME   = 0.12f;
static const float RUN_FRAME = 0.07f;
static const float RUN_SPEED = 70.0f;   // пикселей спрайта в секунду
static const float ENEMY_FAR = 96.0f;   // откуда вбегает и куда убегает
static const float ENEMY_NEAR = 30.0f;  // дистанция боя

static unsigned rnd(Scene *sc)
{
    sc->rng = sc->rng * 1103515245u + 12345u;
    return (sc->rng >> 16) & 0x7fff;
}

static float rndf(Scene *sc, float lo, float hi)
{
    return lo + (hi - lo) * (float)rnd(sc) / 32767.0f;
}

// --- сценарии кадров ---------------------------------------------------------

static void play_seq(SceneActor *a, int anim, const int *frames, const float *durs,
                     int n, bool loop)
{
    a->anim = anim;
    a->steps = n > SCENE_SCRIPT_MAX ? SCENE_SCRIPT_MAX : n;
    for (int i = 0; i < a->steps; i++) {
        a->frames[i] = frames[i];
        a->durs[i] = durs ? durs[i] : FRAME;
    }
    a->step = 0;
    a->t = 0;
    a->loop = loop;
    a->done = false;
}

// Кадры анимации подряд, от from до to включительно, с одной длительностью.
static void play_range(SceneActor *a, int anim, int from, int to, float dur, bool loop)
{
    int frames[SCENE_SCRIPT_MAX], n = 0;
    int n_all = sprite_frames_of(a->ch, anim);
    if (n_all <= 0) { play_range(a, SPR_STANCE, 0, 0, dur, true); return; }
    if (to >= n_all) to = n_all - 1;
    if (from > to) from = to;
    for (int i = from; i <= to && n < SCENE_SCRIPT_MAX; i++) frames[n++] = i;
    float durs[SCENE_SCRIPT_MAX];
    for (int i = 0; i < n; i++) durs[i] = dur;
    play_seq(a, anim, frames, durs, n, loop);
}

static bool playing(const SceneActor *a, int anim)
{
    return a->anim == anim && !a->done;
}

// Стойка: последние два кадра ряда — сам ряд начинается с разворота в
// профиль, а стоять надо лицом к противнику.
static void play_stance(SceneActor *a)
{
    int n = sprite_frames_of(a->ch, SPR_STANCE);
    play_range(a, SPR_STANCE, n - 2, n - 1, 0.45f, true);
}

static void actor_update(SceneActor *a, float dt)
{
    if (a->steps <= 0 || a->done) return;
    a->t += dt;
    while (a->t >= a->durs[a->step]) {
        a->t -= a->durs[a->step];
        if (a->step + 1 < a->steps) {
            a->step++;
        } else if (a->loop) {
            a->step = 0;
        } else {
            a->done = true;   // остаёмся на последнем кадре
            a->t = 0;
            break;
        }
    }
}

static int actor_frame(const SceneActor *a)
{
    if (a->steps <= 0) return 0;
    return a->frames[a->step];
}

// --- сцена ---------------------------------------------------------------------

void scene_init(Scene *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->hero.ch = SPRITE_HERO;
    sc->enemy.ch = SPRITE_ENEMY;
    sc->enemy.flip = true;
    sc->enemy.x = ENEMY_FAR;
    sc->rng = 0x9e3779b9u;
    sc->mood = SCENE_IDLE;
    play_stance(&sc->hero);
}

static void enemy_enter(Scene *sc)
{
    sc->enemy_phase = ENEMY_ENTER;
    sc->enemy.flip = true;
    sc->enemy.x = ENEMY_FAR;
    play_range(&sc->enemy, SPR_RUN, 2, 9, RUN_FRAME, true);
}

static void enemy_leave(Scene *sc)
{
    sc->enemy_phase = ENEMY_LEAVE;
    sc->enemy.flip = false;
    play_range(&sc->enemy, SPR_RUN, 2, 9, RUN_FRAME, true);
}

void scene_set(Scene *sc, SceneMood mood)
{
    if (mood == sc->mood) return;
    sc->mood = mood;
    sc->clock = 0;

    switch (mood) {
    case SCENE_IDLE:
        play_stance(&sc->hero);
        if (sc->enemy_phase == ENEMY_ENTER || sc->enemy_phase == ENEMY_FIGHT)
            enemy_leave(sc);
        break;

    case SCENE_FIGHT:
        play_stance(&sc->hero);
        if (sc->enemy_phase != ENEMY_FIGHT) enemy_enter(sc);
        sc->hero_next_attack = 0.6f;
        sc->enemy_next_attack = 1.1f;
        break;

    case SCENE_WIN: {
        // Противник падает, если он здесь; герой ликует и возвращается в
        // стойку сам, по часам сцены.
        if (sc->enemy_phase == ENEMY_ENTER || sc->enemy_phase == ENEMY_FIGHT) {
            sc->enemy_phase = ENEMY_DEAD;
            play_range(&sc->enemy, SPR_DEATH, 0, 4, 0.14f, false);
        }
        static const int   vf[] = { 0, 1 };
        static const float vd[] = { 0.5f, 0.5f };
        play_seq(&sc->hero, SPR_VICTORY, vf, vd, 2, true);
        break;
    }

    case SCENE_CALL: {
        // Поклон: вперёд, выдержать, назад, постоять.
        static const int   bf[] = { 0, 1, 2, 1, 0 };
        static const float bd[] = { 0.12f, 0.12f, 0.9f, 0.12f, 1.6f };
        play_seq(&sc->hero, SPR_BOW, bf, bd, 5, true);
        if (sc->enemy_phase == ENEMY_ENTER || sc->enemy_phase == ENEMY_FIGHT)
            play_stance(&sc->enemy);
        if (sc->enemy_phase == ENEMY_ENTER) sc->enemy_phase = ENEMY_FIGHT;
        break;
    }

    case SCENE_FAIL:
        play_range(&sc->hero, SPR_DEATH, 0, 4, 0.14f, false);
        if (sc->enemy_phase == ENEMY_ENTER || sc->enemy_phase == ENEMY_FIGHT) {
            play_stance(&sc->enemy);
            sc->enemy_phase = ENEMY_FIGHT;
        }
        break;

    default: break;
    }
}

static void attack(Scene *sc, SceneActor *a)
{
    // Удар рукой или ногой; рука чаще. Дальше — снова стойка, пока часы
    // не назначат следующий удар.
    if (rnd(sc) % 3 == 0) play_range(a, SPR_KICK, 0, 4, 0.1f, false);
    else                  play_range(a, SPR_PUNCH, 0, 4, 0.09f, false);
}

void scene_update(Scene *sc, float dt)
{
    if (dt > 0.1f) dt = 0.1f;   // после паузы не проматываем сцену рывком
    sc->clock += dt;

    // Противник: движение по фазам.
    switch (sc->enemy_phase) {
    case ENEMY_ENTER:
        sc->enemy.x -= RUN_SPEED * dt;
        if (sc->enemy.x <= ENEMY_NEAR) {
            sc->enemy.x = ENEMY_NEAR;
            sc->enemy_phase = ENEMY_FIGHT;
            play_stance(&sc->enemy);
        }
        break;
    case ENEMY_LEAVE:
        sc->enemy.x += RUN_SPEED * dt;
        if (sc->enemy.x >= ENEMY_FAR) sc->enemy_phase = ENEMY_NONE;
        break;
    default: break;
    }

    switch (sc->mood) {
    case SCENE_FIGHT:
        if (sc->enemy_phase == ENEMY_FIGHT) {
            if (sc->clock >= sc->hero_next_attack) {
                attack(sc, &sc->hero);
                sc->hero_next_attack = sc->clock + rndf(sc, 0.7f, 1.6f);
            }
            if (sc->clock >= sc->enemy_next_attack) {
                attack(sc, &sc->enemy);
                sc->enemy_next_attack = sc->clock + rndf(sc, 0.9f, 2.0f);
            }
            if (sc->hero.done) play_stance(&sc->hero);
            if (sc->enemy.done) play_stance(&sc->enemy);
        }
        break;

    case SCENE_WIN:
        // Ликование не вечно: через несколько секунд герой снова в стойке,
        // а поверженный противник лежит, пока не вбежит следующий.
        if (sc->clock > 3.5f && playing(&sc->hero, SPR_VICTORY)) play_stance(&sc->hero);
        break;

    case SCENE_FAIL:
        // Противник постоял над героем и ушёл.
        if (sc->clock > 2.0f && sc->enemy_phase == ENEMY_FIGHT) enemy_leave(sc);
        break;

    default: break;
    }

    actor_update(&sc->hero, dt);
    actor_update(&sc->enemy, dt);
}

void scene_draw(const Scene *sc, const Sprites *sp, float x, float floor, float scale)
{
    if (sc->enemy_phase != ENEMY_NONE)
        sprite_draw(sp, SPRITE_ENEMY, sc->enemy.anim, actor_frame(&sc->enemy),
                    x + sc->enemy.x * scale, floor, scale, sc->enemy.flip, WHITE);
    sprite_draw(sp, SPRITE_HERO, sc->hero.anim, actor_frame(&sc->hero),
                x + sc->hero.x * scale, floor, scale, sc->hero.flip, WHITE);
}

const char *scene_mood_name(SceneMood m)
{
    static const char *names[] = { "свободен", "работает", "победа", "зовёт", "упал" };
    return m >= 0 && m < SCENE_MOOD_COUNT ? names[m] : "?";
}
