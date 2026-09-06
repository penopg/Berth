// Арена: отдельное окно, где сценка с каратекой разыгрывается без берта.
//
// Здесь подбираются тайминги и хореография: клавиши 1–5 задают
// настроение, F — просмотр кадров по одному (стрелки: персонаж, ряд,
// кадр), пробел — пауза, +/- — масштаб. Данных берта тут нет вовсе.
// `--selftest` крутит все настроения без окна на глазах и печатает
// среднее время кадра — чтобы говорить о цене цифрами.
#include <stdio.h>
#include <string.h>

#include "raylib.h"
#include "sprite.h"
#include "scene.h"

static const char *ANIM_NAMES[SPRITE_ANIM_COUNT] = {
    "stance", "walk", "bow", "run", "victory", "punch", "kick", "death"
};

int main(int argc, char **argv)
{
    bool selftest = argc > 1 && !strcmp(argv[1], "--selftest");

    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
    InitWindow(760, 320, "Berth arena");
    SetTargetFPS(60);

    Sprites sp = { 0 };
    if (!sprites_load(&sp)) {
        fprintf(stderr, "атлас спрайтов не загрузился\n");
        return 1;
    }

    Scene sc;
    scene_init(&sc);

    float scale = 4.0f;
    bool paused = false;
    bool browse = false;
    int b_char = 0, b_anim = 0, b_frame = 0;

    double draw_total = 0;
    int frames = 0;
    int test_step = 0;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        if (selftest) {
            // Настроения по кругу, по две секунды на каждое.
            int want = (int)(GetTime() / 2.0) % SCENE_MOOD_COUNT;
            scene_set(&sc, (SceneMood)want);
            if (++test_step > 60 * 12) break;
        } else {
            if (IsKeyPressed(KEY_ONE))   scene_set(&sc, SCENE_IDLE);
            if (IsKeyPressed(KEY_TWO))   scene_set(&sc, SCENE_FIGHT);
            if (IsKeyPressed(KEY_THREE)) scene_set(&sc, SCENE_WIN);
            if (IsKeyPressed(KEY_FOUR))  scene_set(&sc, SCENE_CALL);
            if (IsKeyPressed(KEY_FIVE))  scene_set(&sc, SCENE_FAIL);
            if (IsKeyPressed(KEY_SIX))   scene_set(&sc, SCENE_COMPACT);
            if (IsKeyPressed(KEY_SPACE)) paused = !paused;
            if (IsKeyPressed(KEY_F))     browse = !browse;
            if (IsKeyPressed(KEY_EQUAL) && scale < 8) scale += 1;
            if (IsKeyPressed(KEY_MINUS) && scale > 1) scale -= 1;
            if (browse) {
                if (IsKeyPressed(KEY_TAB)) b_char = (b_char + 1) % SPRITE_CHAR_COUNT;
                if (IsKeyPressed(KEY_UP))   b_anim = (b_anim + SPRITE_ANIM_COUNT - 1) % SPRITE_ANIM_COUNT;
                if (IsKeyPressed(KEY_DOWN)) b_anim = (b_anim + 1) % SPRITE_ANIM_COUNT;
                int n = sprite_frames_of((SpriteChar)b_char, b_anim);
                if (IsKeyPressed(KEY_RIGHT) && n) b_frame = (b_frame + 1) % n;
                if (IsKeyPressed(KEY_LEFT) && n)  b_frame = (b_frame + n - 1) % n;
                if (b_frame >= n) b_frame = 0;
            }
        }

        if (!paused) scene_update(&sc, dt);

        BeginDrawing();
        ClearBackground((Color){ 24, 24, 30, 255 });

        float floor = 240;
        DrawLine(0, (int)floor, GetScreenWidth(), (int)floor, (Color){ 60, 60, 74, 255 });

        double t0 = GetTime();
        if (browse) {
            int n = sprite_frames_of((SpriteChar)b_char, b_anim);
            // Весь ряд в ряд, текущий кадр подсвечен — так видно, где в
            // ряду какой удар и с какого кадра начинать сценарий.
            float x = 40;
            for (int i = 0; i < n; i++) {
                const SpriteFrame *f = sprite_frame((SpriteChar)b_char, b_anim, i);
                Color tint = i == b_frame ? WHITE : (Color){ 255, 255, 255, 110 };
                sprite_draw(&sp, (SpriteChar)b_char, b_anim, i, x + f->ax * 2, floor, 2, false, tint);
                if (i == b_frame)
                    DrawText(TextFormat("%d", i), (int)x, (int)floor + 6, 10, WHITE);
                x += (f->w + 4) * 2;
            }
            sprite_draw(&sp, (SpriteChar)b_char, b_anim, b_frame, 560, floor - 20, scale, false, WHITE);
            DrawText(TextFormat("%s / %s  frame %d of %d   (tab, up/down, left/right)",
                                b_char == SPRITE_HERO ? "hero" : "enemy",
                                ANIM_NAMES[b_anim], b_frame, n),
                     16, 16, 10, LIGHTGRAY);
        } else {
            scene_draw(&sc, &sp, 200, floor, scale, (Color){ 235, 80, 70, 255 });
            // Клавиша T — как будто пришли токены: удар и подскок счёта.
            if (IsKeyPressed(KEY_T)) scene_score(&sc, sc.score + 700);
            // Зажатая B — как будто агент льёт текст: удары подряд.
            scene_activity(&sc, IsKeyDown(KEY_B) ? 500 : 0, dt);
        }
        double draw_ms = (GetTime() - t0) * 1000.0;
        draw_total += draw_ms;
        frames++;

        DrawText(TextFormat("1 idle  2 fight  3 win  4 call  5 fail  6 compact  t tokens  b (hold) stream  space pause  f frames  +/- scale (%d)",
                            (int)scale), 16, 290, 10, GRAY);
        DrawText(TextFormat("mood: %d   enemy phase: %d   scene draw %.3f ms   frame %.2f ms",
                            sc.mood, sc.enemy_phase, draw_ms, dt * 1000.0f),
                 16, 270, 10, GRAY);
        EndDrawing();
    }

    if (selftest && frames)
        printf("кадров %d, отрисовка сцены в среднем %.4f мс\n", frames, draw_total / frames);

    sprites_unload(&sp);
    CloseWindow();
    return 0;
}
