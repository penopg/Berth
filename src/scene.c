#include <string.h>

#include "scene.h"

// Темп NES: кадр анимации — восемь в секунду; бег чаще.
static const float FRAME   = 0.12f;
static const float RUN_FRAME = 0.07f;
static const float RUN_SPEED = 70.0f;   // пикселей спрайта в секунду
static const float ENEMY_FAR = 96.0f;   // откуда вбегает и куда убегает
static const float ENEMY_NEAR = 22.0f;  // дистанция боя: вытянутая нога достаёт
static const float HERO_FAR  = -44.0f;  // откуда выходит и куда уходит герой
static const float WALK_SPEED = 32.0f;
static const float WALK_FRAME = 0.1f;
static const float IDLE_LEAVE = 10.0f;  // секунд без дела — и герой уходит
static const float LYING_LEAVE = 15.0f; // сколько лежит упавший герой
static const float HIT_BYTES  = 900.0f;  // байт вывода на один удар
static const float HIT_GAP    = 0.16f;   // не чаще, чем раз в столько секунд
static const float FLOW_CAP   = 4000.0f; // задел ударов не копится бесконечно

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

// Герой свободен для удара: не посреди другого удара. Стойка зациклена и
// «done» у неё не бывает — проверять надо анимацию, а не флаг.
static bool hero_free(const Scene *sc)
{
    return !playing(&sc->hero, SPR_PUNCH) && !playing(&sc->hero, SPR_KICK);
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
    sc->enemy.flip = false;
    sc->enemy.x = ENEMY_FAR;
    sc->rng = 0x9e3779b9u;
    sc->mood = SCENE_IDLE;
    sc->hero_phase = HERO_NONE;
    sc->hero.x = HERO_FAR;
}

static void hero_enter(Scene *sc)
{
    sc->hero_phase = HERO_ENTER;
    sc->hero.flip = false;
    sc->hero.x = HERO_FAR;
    play_range(&sc->hero, SPR_WALK, 0, 9, WALK_FRAME, true);
}

static void hero_leave(Scene *sc)
{
    sc->hero_phase = HERO_LEAVE;
    sc->hero.flip = true;
    play_range(&sc->hero, SPR_WALK, 0, 9, WALK_FRAME, true);
}

// Герой на месте и готов играть настроение; если его нет — выходит, и
// настроение он сыграет, когда дойдёт (scene_update).
static bool hero_ready(Scene *sc)
{
    if (sc->hero_phase == HERO_STAY) return true;
    if (sc->hero_phase == HERO_NONE || sc->hero_phase == HERO_LEAVE) hero_enter(sc);
    return false;
}

// Что герой играет в текущем настроении, стоя на месте.
static void hero_act(Scene *sc)
{
    switch (sc->mood) {
    case SCENE_WIN: {
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
        break;
    }
    case SCENE_FAIL:
        play_range(&sc->hero, SPR_DEATH, 0, 4, 0.14f, false);
        break;
    case SCENE_COMPACT:
        play_range(&sc->hero, SPR_WALK, 0, 9, WALK_FRAME, true);
        sc->pace_dir = 1;
        sc->hero.flip = false;
        break;
    default:
        play_stance(&sc->hero);
        break;
    }
}

static void enemy_enter(Scene *sc)
{
    // Противник в раскладке нарисован влево, к герою: без отражения.
    sc->enemy_phase = ENEMY_ENTER;
    sc->enemy.flip = false;
    sc->enemy.x = ENEMY_FAR;
    play_range(&sc->enemy, SPR_RUN, 2, 9, RUN_FRAME, true);
}

static void enemy_leave(Scene *sc)
{
    sc->enemy_phase = ENEMY_LEAVE;
    sc->enemy.flip = true;   // убегает вправо — отражаем
    play_range(&sc->enemy, SPR_RUN, 2, 9, RUN_FRAME, true);
}

void scene_set(Scene *sc, SceneMood mood)
{
    if (mood == sc->mood) return;
    SceneMood prev = sc->mood;
    sc->mood = mood;
    sc->clock = 0;
    sc->idle = 0;
    // Из уборки — на место: герой мог уйти в сторону.
    if (prev == SCENE_COMPACT && sc->hero_phase == HERO_STAY) {
        sc->hero.x = 0;
        sc->hero.flip = false;
    }

    switch (mood) {
    case SCENE_IDLE:
        if (sc->hero_phase == HERO_STAY) hero_act(sc);
        if (sc->enemy_phase == ENEMY_ENTER || sc->enemy_phase == ENEMY_FIGHT)
            enemy_leave(sc);
        break;

    case SCENE_FIGHT:
        if (hero_ready(sc)) hero_act(sc);
        if (sc->enemy_phase != ENEMY_FIGHT) enemy_enter(sc);
        // После сжатия бой продолжается с тем же счётом: это тот же кусок
        // работы, просто с уборкой посередине.
        if (prev != SCENE_COMPACT) {
            sc->fight_from = sc->score;
            sc->screen_from = sc->screen;
            sc->flow = 0;
            sc->shown = 0;
        }
        sc->hero_next_attack = 0.6f;
        sc->enemy_next_attack = 1.1f;
        break;

    case SCENE_COMPACT:
        // Уборка: герой ходит взад-вперёд, противник, если он здесь, ждёт в
        // стойке. Не бой — поток вывода в это время не про работу.
        // Вбегающего не трогаем: он добежит сам (ENEMY_ENTER в update) —
        // иначе он замирал в стойке за краем ячейки, и бой шёл «без него».
        if (hero_ready(sc)) hero_act(sc);
        if (sc->enemy_phase == ENEMY_FIGHT) play_stance(&sc->enemy);
        break;

    case SCENE_WIN:
        // Противник падает, если он здесь; герой ликует и возвращается в
        // стойку сам, по часам сцены.
        if (sc->enemy_phase == ENEMY_ENTER || sc->enemy_phase == ENEMY_FIGHT) {
            sc->enemy_phase = ENEMY_DEAD;
            sc->lying = 0;
            play_range(&sc->enemy, SPR_DEATH, 0, 4, 0.14f, false);
        }
        if (sc->hero_phase == HERO_STAY) hero_act(sc);
        sc->result = sc->shown;
        if (sc->score - sc->fight_from > sc->result) sc->result = sc->score - sc->fight_from;
        if (sc->screen - sc->screen_from > sc->result) sc->result = sc->screen - sc->screen_from;
        sc->flow = 0;
        break;

    case SCENE_CALL:
        if (hero_ready(sc)) hero_act(sc);
        if (sc->enemy_phase == ENEMY_FIGHT) play_stance(&sc->enemy);
        break;

    case SCENE_FAIL:
        // Падать есть кому только если герой на сцене: выходить ради
        // падения незачем.
        if (sc->hero_phase == HERO_STAY) { hero_act(sc); sc->lying = 0; }
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
    // не назначат следующий удар. У героя удар отмечается попаданием:
    // звёздочка загорается, когда конечность доходит до крайнего кадра.
    bool kick = rnd(sc) % 3 == 0;
    if (a == &sc->hero) {
        sc->hit_delay = kick ? 0.11f : 0.19f;
        sc->hit_x = kick ? 25.0f : 22.0f;
        sc->hit_y = kick ? -34.0f : -29.0f;
        sc->since_hit = 0;
    }
    if (kick) {
        // Нога: замах быстрый, а в верхней точке нога задерживается на пару
        // кадров — иначе удара не видно, мелькает.
        static const int   kf[] = { 0, 1, 2, 3, 3, 1 };
        static const float kd[] = { 0.035f, 0.035f, 0.035f, 0.07f, 0.07f, 0.035f };
        play_seq(a, SPR_KICK, kf, kd, 6, false);
    } else {
        play_range(a, SPR_PUNCH, 0, 4, 0.09f, false);
    }
}

void scene_update(Scene *sc, float dt)
{
    if (dt > 0.1f) dt = 0.1f;   // после паузы не проматываем сцену рывком
    sc->clock += dt;

    // Герой: выходит слева, уходит влево.
    switch (sc->hero_phase) {
    case HERO_ENTER:
        sc->hero.x += WALK_SPEED * dt;
        if (sc->hero.x >= 0) {
            sc->hero.x = 0;
            sc->hero_phase = HERO_STAY;
            hero_act(sc);
        }
        break;
    case HERO_LEAVE:
        sc->hero.x -= WALK_SPEED * dt;
        if (sc->hero.x <= HERO_FAR) sc->hero_phase = HERO_NONE;
        break;
    default: break;
    }

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
    case ENEMY_DEAD:
        // Поверженный полежит немного и пропадёт: сцена — индикатор, и
        // лежащее тело через минуту уже ни о чём не говорит.
        sc->lying += dt;
        if (sc->lying > 6.0f) sc->enemy_phase = ENEMY_NONE;
        break;
    default: break;
    }

    switch (sc->mood) {
    case SCENE_FIGHT:
        if (sc->enemy_phase == ENEMY_FIGHT) {
            // Герой бьёт, когда приходят токены (scene_score); сам — лишь
            // если давно ничего не приходило, чтобы бой не выглядел замершим.
            sc->since_hit += dt;
            if (sc->since_hit > 4.0f && hero_free(sc)) attack(sc, &sc->hero);
            // Удар кончился — стойка, чтобы следующий мог начаться.
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
        // а поверженный противник лежит, пока не пропадёт.
        if (sc->clock > 3.5f && playing(&sc->hero, SPR_VICTORY)) play_stance(&sc->hero);
        break;

    case SCENE_COMPACT:
        // Шаг вправо до края ячейки, разворот, шаг влево — и так, пока
        // сжатие не кончится.
        if (sc->hero_phase == HERO_STAY && playing(&sc->hero, SPR_WALK)) {
            sc->hero.x += (float)sc->pace_dir * WALK_SPEED * 0.6f * dt;
            if (sc->hero.x > 10.0f) { sc->pace_dir = -1; sc->hero.flip = true; }
            if (sc->hero.x < -6.0f) { sc->pace_dir = 1;  sc->hero.flip = false; }
        }
        break;

    case SCENE_FAIL:
        // Противник постоял над героем и ушёл; упавший герой полежал и исчез.
        if (sc->clock > 2.0f && sc->enemy_phase == ENEMY_FIGHT) enemy_leave(sc);
        if (sc->hero_phase == HERO_STAY && sc->clock > LYING_LEAVE) sc->hero_phase = HERO_NONE;
        break;

    default: break;
    }

    // Без дела герой не стоит: работа кончилась, противник пропал — через
    // несколько секунд уходит и он. Пока зовёт (поклон) — остаётся: это
    // просьба к человеку, и она должна быть на виду.
    bool busy = sc->mood == SCENE_FIGHT || sc->mood == SCENE_CALL || sc->mood == SCENE_COMPACT
             || sc->enemy_phase != ENEMY_NONE;
    if (sc->hero_phase == HERO_STAY && !busy && sc->mood != SCENE_FAIL) {
        sc->idle += dt;
        if (sc->idle > IDLE_LEAVE) hero_leave(sc);
    } else {
        sc->idle = 0;
    }

    actor_update(&sc->hero, dt);
    actor_update(&sc->enemy, dt);
    if (sc->bump > 0) sc->bump -= dt;

    // Правда из записей jsonl подтягивает показанное вперёд — плавно, за
    // доли секунды; если оценка по потоку убежала вперёд, число просто
    // ждёт, пока правда его догонит.
    long target = sc->score - sc->fight_from;
    if (sc->screen - sc->screen_from > target) target = sc->screen - sc->screen_from;
    if (sc->shown < target) {
        long step = (long)((float)(target - sc->shown) * (dt * 5.0f)) + 1;
        sc->shown += step;
        if (sc->shown > target) sc->shown = target;
    }
    if (sc->hit_delay > 0) {
        sc->hit_delay -= dt;
        // Попадание: вспышка и подскок счётчика в один момент.
        if (sc->hit_delay <= 0) { sc->hit_t = 0.22f; sc->bump = 0.28f; }
    } else if (sc->hit_t > 0) {
        sc->hit_t -= dt;
    }
}

bool scene_active(const Scene *sc)
{
    return sc->hero_phase != HERO_NONE || sc->enemy_phase != ENEMY_NONE;
}

void scene_draw(const Scene *sc, const Sprites *sp, float x, float floor, float scale, Color hit)
{
    if (sc->enemy_phase != ENEMY_NONE)
        sprite_draw(sp, SPRITE_ENEMY, sc->enemy.anim, actor_frame(&sc->enemy),
                    x + sc->enemy.x * scale, floor, scale, sc->enemy.flip, WHITE);
    if (sc->hero_phase != HERO_NONE)
        sprite_draw(sp, SPRITE_HERO, sc->hero.anim, actor_frame(&sc->hero),
                    x + sc->hero.x * scale, floor, scale, sc->hero.flip, WHITE);

    // Попадание: звёздочка из четырёх лучей — вспыхивает сразу в полный
    // размер и сжимается, чуть тая. Примитивами, не
    // спрайтом: цвет — из темы.
    if (sc->hit_t > 0) {
        float k = sc->hit_t / 0.22f;              // 1 в момент удара, 0 в конце
        float r = (4.0f + 9.0f * k) * scale;      // сразу большая, дальше сжимается
        hit.a = (unsigned char)(hit.a * (0.4f + 0.6f * k));
        Vector2 c = { x + (sc->hero.x + sc->hit_x) * scale, floor + sc->hit_y * scale };
        DrawLineEx((Vector2){ c.x - r, c.y }, (Vector2){ c.x + r, c.y }, 1.5f * scale, hit);
        DrawLineEx((Vector2){ c.x, c.y - r }, (Vector2){ c.x, c.y + r }, 1.5f * scale, hit);
        float d = r * 0.6f;
        DrawLineEx((Vector2){ c.x - d, c.y - d }, (Vector2){ c.x + d, c.y + d }, 1.0f * scale, hit);
        DrawLineEx((Vector2){ c.x - d, c.y + d }, (Vector2){ c.x + d, c.y - d }, 1.0f * scale, hit);
    }
}

const char *scene_mood_name(SceneMood m)
{
    static const char *names[] = { "свободен", "работает", "победа", "зовёт", "упал", "сжатие" };
    return m >= 0 && m < SCENE_MOOD_COUNT ? names[m] : "?";
}

void scene_activity(Scene *sc, unsigned long bytes, float dt)
{
    // Мелкий трафик — крутилка и часы Claude Code, не работа: он не считается.
    // Крупные порции — текст, который агент пишет прямо сейчас.
    if (sc->mood == SCENE_FIGHT && bytes >= 48) {
        sc->flow += (float)bytes;
        if (sc->flow > FLOW_CAP) sc->flow = FLOW_CAP;
    }
    if (sc->flow >= HIT_BYTES && sc->enemy_phase == ENEMY_FIGHT && sc->hero_phase == HERO_STAY
        && hero_free(sc) && sc->since_hit >= HIT_GAP) {
        sc->flow -= HIT_BYTES;
        attack(sc, &sc->hero);
    }
    (void)dt;
}

void scene_screen(Scene *sc, long tokens)
{
    if (tokens > sc->screen) sc->screen = tokens;   // назад не ходит
}

void scene_score(Scene *sc, long score)
{
    // Приращение во время боя подбрасывает счётчик: видно, что цифра
    // живая, а не нарисованная.
    // Запись jsonl — правда: оценка из потока сбрасывается, счёт встаёт на
    // место. Если правда пришла без потока (удары не шли) — подскок сразу.
    if (score > sc->score && sc->mood == SCENE_FIGHT && sc->flow <= 0 && sc->since_hit > 1.0f)
        sc->bump = 0.28f;
    sc->score = score;
}
