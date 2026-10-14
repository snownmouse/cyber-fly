// SPDX-License-Identifier: Apache-2.0
#include <math.h>
#include <stddef.h>
#include "mosaico_game.h"
#include "mosaico_raylib_fast.h"
#include "pet_view.h"

// ---- Palette -------------------------------------------------------------
static const Color C_WALL_TOP = {255, 236, 212, 255};
static const Color C_WALL_BOTTOM = {255, 222, 192, 255};
static const Color C_BASEBOARD = {244, 228, 206, 255};
static const Color C_FLOOR = {222, 178, 132, 255};
static const Color C_FRAME = {196, 150, 104, 255};
static const Color C_PICTURE = {180, 220, 208, 255};
static const Color C_CLOCK_FACE = {255, 250, 240, 255};
static const Color C_RUG = {228, 128, 120, 235};
static const Color C_RUG_EDGE = {200, 100, 96, 255};

static const Color C_BODY = {255, 240, 214, 255};
static const Color C_BELLY = {255, 226, 192, 255};
static const Color C_PAW = {248, 220, 194, 255};
static const Color C_PAW_LINE = {228, 198, 170, 255};
static const Color C_INNER_EAR = {244, 158, 176, 255};
static const Color C_NOSE = {238, 128, 150, 255};
static const Color C_BLUSH = {242, 130, 152, 255};
static const Color C_DARK = {74, 52, 46, 255};

static const Color C_PANEL = {255, 250, 240, 225};
static const Color C_BAR_BG = {228, 214, 196, 255};
static const Color C_BAR_FOOD = {255, 160, 72, 255};
static const Color C_BAR_JOY = {238, 102, 128, 255};
static const Color C_BAR_ENERGY = {90, 196, 160, 255};

static const Color C_BTN_FEED = {255, 158, 82, 255};
static const Color C_BTN_PLAY = {66, 176, 162, 255};
static const Color C_BTN_SLEEP = {146, 122, 222, 255};

static const Color C_NIGHT = {18, 22, 58, 150};
static const Color C_MOON = {246, 244, 220, 255};

// ---- Small helpers -------------------------------------------------------
static float clampf(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

static Color alpha_of(Color color, uint16_t life, uint16_t max_life)
{
    float threshold = (float)max_life * 0.32f;
    float alpha = life > (uint16_t)threshold ? 1.0f : (float)life / threshold;
    return ColorAlpha(color, clampf(alpha, 0.0f, 1.0f));
}

static void draw_text_centered(const char *text, int x, int y, int width,
                               int size, Color color)
{
    DrawText(text, x + (width - MeasureText(text, size)) / 2, y, size, color);
}

// ---- Room ----------------------------------------------------------------
static void draw_room(const pet_game_t *pet)
{
    DrawRectangleGradientV(0, 0, MOSAICO_GAME_WIDTH, 356,
                           C_WALL_TOP, C_WALL_BOTTOM);

    // Picture frame.
    DrawRectangle(34, 92, 66, 54, C_FRAME);
    DrawRectangle(40, 98, 54, 42, C_PICTURE);
    DrawTriangle((Vector2){42, 138}, (Vector2){62, 112}, (Vector2){74, 138},
                 (Color){120, 170, 150, 255});
    DrawTriangle((Vector2){58, 138}, (Vector2){78, 108}, (Vector2){92, 138},
                 (Color){96, 148, 130, 255});

    // Wall clock.
    DrawCircle(424, 118, 25.0f, (Color){206, 172, 136, 255});
    DrawCircle(424, 118, 23.0f, C_CLOCK_FACE);
    DrawLineEx((Vector2){424, 118}, (Vector2){424, 105}, 2.0f, C_DARK);
    DrawLineEx((Vector2){424, 118}, (Vector2){433, 122}, 2.0f, C_DARK);
    DrawCircle(424, 118, 2.5f, C_DARK);

    // Baseboard and floor.
    DrawRectangle(0, 356, 480, 12, C_BASEBOARD);
    DrawRectangle(0, 367, 480, 2, (Color){210, 184, 160, 255});
    DrawRectangle(0, 368, 480, 112, C_FLOOR);
    for (int x = 0; x <= 480; x += 60)
        DrawLine(x, 368, x, 480, (Color){202, 158, 114, 255});
    DrawLine(0, 424, 480, 424, (Color){202, 158, 114, 255});

    // Rug under the home spot.
    DrawEllipse(240, 392, 152.0f, 22.0f, C_RUG);
    DrawEllipseLines(240, 392, 152.0f, 22.0f, C_RUG_EDGE);
    (void)pet;
}

// ---- Pet -----------------------------------------------------------------
static void draw_tail(const pet_game_t *pet, float cx, float cy)
{
    float fast = pet->behavior == PET_BEHAVIOR_HAPPY ||
                 pet->behavior == PET_BEHAVIOR_EATING;
    float sway = fast ? sinf(pet->tick * 0.55f) * 1.25f
                      : sinf(pet->tick * 0.12f);
    Vector2 base = {cx + 72, cy + 52};
    Vector2 mid = {cx + 124 + sway * 8, cy + 26 - sway * 8};
    Vector2 tip = {cx + 130 + sway * 14, cy - 22 - sway * 16};
    DrawLineEx(base, mid, 16.0f, C_BODY);
    DrawLineEx(mid, tip, 12.0f, C_BODY);
    DrawCircleV(tip, 8.5f, C_BODY);
}

static void draw_ears(const pet_game_t *pet, float cx, float cy)
{
    float droop = 0.0f, wiggle = 0.0f;
    if (pet->behavior == PET_BEHAVIOR_SLEEP ||
        pet->behavior == PET_BEHAVIOR_DRAGGED) droop = 14.0f;
    if (pet->behavior == PET_BEHAVIOR_PLAY)
        wiggle = 5.0f * sinf(pet->tick * 0.45f);

    // Left ear: broad triangle rooted well inside the head edge.
    Vector2 l_outer = {cx - 78, cy - 48};
    Vector2 l_inner_base = {cx - 26, cy - 72};
    Vector2 l_tip = {cx - 54 + wiggle - droop * 0.5f, cy - 118 + droop};
    DrawTriangle(l_outer, l_tip, l_inner_base, C_BODY);
    DrawTriangle((Vector2){l_tip.x + 6, l_tip.y + 22},
                 (Vector2){l_outer.x + 12, l_outer.y - 8},
                 (Vector2){l_inner_base.x - 8, l_inner_base.y + 4},
                 C_INNER_EAR);

    // Right ear.
    Vector2 r_outer = {cx + 78, cy - 48};
    Vector2 r_inner_base = {cx + 26, cy - 72};
    Vector2 r_tip = {cx + 54 - wiggle + droop * 0.5f, cy - 118 + droop};
    DrawTriangle(r_outer, r_inner_base, r_tip, C_BODY);
    DrawTriangle((Vector2){r_tip.x - 6, r_tip.y + 22},
                 (Vector2){r_inner_base.x + 8, r_inner_base.y + 4},
                 (Vector2){r_outer.x - 12, r_outer.y - 8},
                 C_INNER_EAR);
}

static void draw_closed_happy_eye(float ex, float ey)
{
    DrawLineEx((Vector2){ex - 7, ey + 4}, (Vector2){ex, ey - 5}, 2.6f, C_DARK);
    DrawLineEx((Vector2){ex, ey - 5}, (Vector2){ex + 7, ey + 4}, 2.6f, C_DARK);
}

static void draw_open_eye(const pet_game_t *pet, float ex, float ey)
{
    float glance = (float)pet->look_dir * 3.0f;
    DrawEllipse((int)(ex + glance), (int)ey, 9.0f, 11.0f, C_DARK);
    DrawCircleV((Vector2){ex + glance - 3, ey - 4}, 2.4f, WHITE);
    DrawCircleV((Vector2){ex + glance + 2, ey + 3}, 1.2f, WHITE);
    if (pet->energy < 22.0f) {
        // Sleepy half-closed lid.
        DrawEllipse((int)(ex + glance), (int)(ey - 5), 10.0f, 6.0f, C_BODY);
        DrawLineEx((Vector2){ex - 8, ey - 4}, (Vector2){ex + 8, ey - 4},
                   2.0f, C_DARK);
    }
}

static void draw_sleep_eye(float ex, float ey)
{
    DrawLineEx((Vector2){ex - 7, ey - 2}, (Vector2){ex, ey + 5}, 2.2f, C_DARK);
    DrawLineEx((Vector2){ex, ey + 5}, (Vector2){ex + 7, ey - 2}, 2.2f, C_DARK);
}

static void draw_sad_eye(float ex, float ey, bool left)
{
    DrawEllipse((int)ex, (int)ey, 7.0f, 8.0f, C_DARK);
    if (left)
        DrawLineEx((Vector2){ex - 8, ey - 13}, (Vector2){ex + 3, ey - 9},
                   2.2f, C_DARK);
    else
        DrawLineEx((Vector2){ex - 3, ey - 9}, (Vector2){ex + 8, ey - 13},
                   2.2f, C_DARK);
}

static void draw_shocked_eye(float ex, float ey)
{
    DrawEllipse((int)ex, (int)ey, 12.0f, 13.0f, WHITE);
    DrawCircleV((Vector2){ex, ey}, 5.0f, C_DARK);
    DrawCircleV((Vector2){ex - 2, ey - 2}, 1.4f, WHITE);
}

static void draw_face(const pet_game_t *pet, float cx, float cy)
{
    float left_x = cx - 38, right_x = cx + 38, eye_y = cy - 16;
    bool blink = pet->blink_phase != 0 &&
                 (pet->behavior == PET_BEHAVIOR_IDLE ||
                  pet->behavior == PET_BEHAVIOR_SAD);

    // Eyes.
    switch (pet->behavior) {
    case PET_BEHAVIOR_HAPPY:
    case PET_BEHAVIOR_EATING:
        draw_closed_happy_eye(left_x, eye_y);
        draw_closed_happy_eye(right_x, eye_y);
        break;
    case PET_BEHAVIOR_PLAY:
        DrawCircleV((Vector2){left_x, eye_y}, 10.0f, C_DARK);
        DrawCircleV((Vector2){left_x - 3, eye_y - 4}, 2.6f, WHITE);
        DrawCircleV((Vector2){right_x, eye_y}, 10.0f, C_DARK);
        DrawCircleV((Vector2){right_x - 3, eye_y - 4}, 2.6f, WHITE);
        break;
    case PET_BEHAVIOR_SLEEP:
        draw_sleep_eye(left_x, eye_y);
        draw_sleep_eye(right_x, eye_y);
        break;
    case PET_BEHAVIOR_DRAGGED:
        draw_shocked_eye(left_x, eye_y);
        draw_shocked_eye(right_x, eye_y);
        break;
    case PET_BEHAVIOR_SAD:
        if (blink) {
            DrawLineEx((Vector2){left_x - 7, eye_y},
                       (Vector2){left_x + 7, eye_y}, 2.0f, C_DARK);
            DrawLineEx((Vector2){right_x - 7, eye_y},
                       (Vector2){right_x + 7, eye_y}, 2.0f, C_DARK);
        } else {
            draw_sad_eye(left_x, eye_y, true);
            draw_sad_eye(right_x, eye_y, false);
        }
        break;
    default:  // PET_BEHAVIOR_IDLE
        if (blink) {
            DrawLineEx((Vector2){left_x - 7, eye_y},
                       (Vector2){left_x + 7, eye_y}, 2.0f, C_DARK);
            DrawLineEx((Vector2){right_x - 7, eye_y},
                       (Vector2){right_x + 7, eye_y}, 2.0f, C_DARK);
        } else {
            draw_open_eye(pet, left_x, eye_y);
            draw_open_eye(pet, right_x, eye_y);
        }
        break;
    }

    // Nose.
    DrawTriangle((Vector2){cx - 5, cy + 6}, (Vector2){cx + 5, cy + 6},
                 (Vector2){cx, cy + 12}, C_NOSE);

    // Mouth.
    switch (pet->behavior) {
    case PET_BEHAVIOR_HAPPY:
    case PET_BEHAVIOR_PLAY:
        DrawEllipse((int)cx, (int)(cy + 22), 7.0f, 8.0f, C_DARK);
        DrawEllipse((int)cx, (int)(cy + 25), 4.0f, 4.0f, C_NOSE);
        break;
    case PET_BEHAVIOR_EATING: {
        float open = (pet->tick & 2U) ? 9.0f : 5.0f;
        DrawEllipse((int)cx, (int)(cy + 22), 7.0f, open, C_DARK);
        DrawEllipse((int)cx, (int)(cy + 24), 4.0f, 3.0f, C_NOSE);
        break;
    }
    case PET_BEHAVIOR_SLEEP:
        DrawEllipse((int)cx, (int)(cy + 19), 3.0f, 3.5f, C_DARK);
        break;
    case PET_BEHAVIOR_DRAGGED:
        DrawEllipse((int)cx, (int)(cy + 19), 4.0f, 5.0f, C_DARK);
        break;
    case PET_BEHAVIOR_SAD:
        DrawLineEx((Vector2){cx, cy + 13}, (Vector2){cx, cy + 18}, 1.8f, C_DARK);
        DrawLineEx((Vector2){cx, cy + 18}, (Vector2){cx - 8, cy + 23},
                   1.8f, C_DARK);
        DrawLineEx((Vector2){cx, cy + 18}, (Vector2){cx + 8, cy + 23},
                   1.8f, C_DARK);
        break;
    default:
        DrawLineEx((Vector2){cx, cy + 13}, (Vector2){cx, cy + 17}, 1.8f, C_DARK);
        DrawLineEx((Vector2){cx, cy + 17}, (Vector2){cx - 7, cy + 22},
                   1.8f, C_DARK);
        DrawLineEx((Vector2){cx - 7, cy + 22}, (Vector2){cx - 13, cy + 17},
                   1.8f, C_DARK);
        DrawLineEx((Vector2){cx, cy + 17}, (Vector2){cx + 7, cy + 22},
                   1.8f, C_DARK);
        DrawLineEx((Vector2){cx + 7, cy + 22}, (Vector2){cx + 13, cy + 17},
                   1.8f, C_DARK);
        break;
    }

    // Whiskers.
    Color whisker = ColorAlpha(C_DARK, 0.62f);
    for (int i = 0; i < 3; ++i) {
        float y = cy + 6 + (float)i * 8;
        float slope = (float)(i - 1) * 4;
        DrawLineEx((Vector2){cx - 56, y}, (Vector2){cx - 92, y + slope},
                   1.2f, whisker);
        DrawLineEx((Vector2){cx + 56, y}, (Vector2){cx + 92, y + slope},
                   1.2f, whisker);
    }

    // Blush.
    float blush_alpha = pet->behavior == PET_BEHAVIOR_HAPPY ? 0.72f : 0.40f;
    DrawEllipse((int)(cx - 60), (int)(cy + 16), 12.0f, 7.0f,
                ColorAlpha(C_BLUSH, blush_alpha));
    DrawEllipse((int)(cx + 60), (int)(cy + 16), 12.0f, 7.0f,
                ColorAlpha(C_BLUSH, blush_alpha));
}

static void draw_pet(const pet_game_t *pet)
{
    float cx = pet->x, cy = pet->y;
    float rh = 100.0f, rv = 86.0f;
    float lift = 0.0f;

    // Per-behavior motion.
    switch (pet->behavior) {
    case PET_BEHAVIOR_IDLE:
    case PET_BEHAVIOR_SAD:
        rv *= 1.0f + 0.025f * sinf(pet->tick * 0.08f);
        break;
    case PET_BEHAVIOR_HAPPY:
        lift = 7.0f * fabsf(sinf(pet->tick * 0.35f));
        cy -= lift;
        break;
    case PET_BEHAVIOR_EATING: {
        float pulse = 1.0f + 0.04f * sinf(pet->tick * 0.5f);
        rh *= pulse;
        rv *= pulse;
        break;
    }
    case PET_BEHAVIOR_PLAY: {
        float phase = (float)(PET_PLAY_TICKS - pet->action_ticks);
        lift = 11.0f * fabsf(sinf(phase * 0.42f));
        cy -= lift;
        break;
    }
    case PET_BEHAVIOR_SLEEP:
        rv *= 1.03f;
        cy += 2.0f;
        break;
    case PET_BEHAVIOR_DRAGGED:
        rh *= 0.90f;
        rv *= 1.10f;
        break;
    }

    // Shadow stays on the ground and shrinks as Mochi lifts.
    float shadow_scale = 1.0f - lift / 26.0f;
    DrawEllipse((int)cx, (int)(pet->y + 94), 92.0f * shadow_scale,
                15.0f * shadow_scale, (Color){120, 86, 70, 70});

    draw_tail(pet, cx, cy);
    draw_ears(pet, cx, cy);

    DrawEllipse((int)cx, (int)cy, rh, rv, C_BODY);
    DrawEllipse((int)cx, (int)(cy + 24), rh * 0.60f, rv * 0.58f, C_BELLY);

    // Front paws.
    DrawEllipse((int)(cx - 40), (int)(cy + 70), 24.0f, 14.0f, C_PAW);
    DrawEllipse((int)(cx + 40), (int)(cy + 70), 24.0f, 14.0f, C_PAW);
    for (int side = -1; side <= 1; side += 2) {
        float px = cx + (float)side * 40;
        for (int toe = -1; toe <= 1; ++toe)
            DrawLine((int)(px + (float)toe * 6), (int)(cy + 64),
                     (int)(px + (float)toe * 6), (int)(cy + 74), C_PAW_LINE);
    }

    draw_face(pet, cx, cy);
}

// ---- Floating effects ----------------------------------------------------
static void draw_heart(float x, float y, Color tint)
{
    DrawCircleV((Vector2){x - 4, y}, 4.5f, tint);
    DrawCircleV((Vector2){x + 4, y}, 4.5f, tint);
    DrawTriangle((Vector2){x - 8, y + 3}, (Vector2){x + 8, y + 3},
                 (Vector2){x, y + 12}, tint);
}

static void draw_fx(const pet_game_t *pet)
{
    for (int i = 0; i < PET_MAX_FX; ++i) {
        const pet_fx_t *fx = &pet->fx[i];
        if (!fx->active) continue;
        Color tint = alpha_of((Color){255, 255, 255, 255}, fx->life, fx->max_life);
        switch (fx->kind) {
        case PET_FX_HEART:
            draw_heart(fx->x, fx->y,
                       ColorAlpha((Color){240, 90, 120, 255}, tint.a / 255.0f));
            break;
        case PET_FX_SLEEP: {
            int size = 15 + (int)(fx->max_life - fx->life) / 5;
            DrawText("Z", (int)fx->x, (int)fx->y, size,
                     ColorAlpha((Color){222, 226, 255, 255}, tint.a / 255.0f));
            break;
        }
        case PET_FX_NOTE: {
            Color note = ColorAlpha((Color){80, 160, 200, 255}, tint.a / 255.0f);
            DrawEllipse((int)fx->x, (int)fx->y, 4.0f, 3.2f, note);
            DrawLineEx((Vector2){fx->x + 3, fx->y},
                       (Vector2){fx->x + 3, fx->y - 11}, 1.6f, note);
            DrawLineEx((Vector2){fx->x + 3, fx->y - 11},
                       (Vector2){fx->x + 9, fx->y - 8}, 1.6f, note);
            break;
        }
        case PET_FX_CRUMB:
            DrawCircleV((Vector2){fx->x, fx->y}, 2.6f,
                        ColorAlpha((Color){190, 140, 85, 255}, tint.a / 255.0f));
            break;
        default: {  // PET_FX_SPARK
            Color spark = ColorAlpha((Color){255, 214, 96, 255}, tint.a / 255.0f);
            DrawCircleV((Vector2){fx->x, fx->y}, 2.2f, spark);
            DrawLineEx((Vector2){fx->x, fx->y - 5},
                       (Vector2){fx->x, fx->y + 5}, 1.4f, spark);
            DrawLineEx((Vector2){fx->x - 5, fx->y},
                       (Vector2){fx->x + 5, fx->y}, 1.4f, spark);
            break;
        }
        }
    }
}

// ---- Night overlay -------------------------------------------------------
static void draw_night(const pet_game_t *pet)
{
    if (pet->behavior != PET_BEHAVIOR_SLEEP) return;
    DrawRectangle(0, 0, MOSAICO_GAME_WIDTH, MOSAICO_GAME_HEIGHT, C_NIGHT);

    // Fixed star field.
    for (int i = 0; i < 26; ++i) {
        int x = (i * 97 + (i % 5) * 31) % 470 + 5;
        int y = (i * 53 + (i % 3) * 17) % 300 + 14;
        Color star = (i & 3U) == 0 ? (Color){255, 255, 230, 220}
                                   : (Color){220, 228, 255, 170};
        DrawPixel(x, y, star);
    }

    // Crescent moon: pale disc with a night-colored disc biting into it.
    DrawCircle(152, 92, 24.0f, C_MOON);
    DrawCircle(161, 86, 20.0f, C_NIGHT);
}

// ---- Stat panel ----------------------------------------------------------
static void draw_stat(const char *label, int x, float value, Color fill)
{
    DrawText(label, x, 20, 12, C_DARK);
    DrawRectangleRounded((Rectangle){x, 38, 110, 10}, 0.5f, 4, C_BAR_BG);
    int width = (int)(110.0f * clampf(value, 0.0f, 100.0f) / 100.0f);
    if (width >= 6)
        DrawRectangleRounded((Rectangle){x, 38, (float)width, 10}, 0.5f, 4, fill);
}

static void draw_stat_panel(const pet_game_t *pet)
{
    DrawRectangleRounded((Rectangle){12, 12, 456, 52}, 0.18f, 6, C_PANEL);
    draw_stat("FOOD", 30, pet->fullness, C_BAR_FOOD);
    draw_stat("HAPPY", 182, pet->joy, C_BAR_JOY);
    draw_stat("ENERGY", 334, pet->energy, C_BAR_ENERGY);
}

// ---- Buttons -------------------------------------------------------------
static void draw_button_icon(uint8_t button, int x, int y, Color color)
{
    switch (button) {
    case PET_BUTTON_FEED:
        // Bowl: rim line plus filled lower half, with steam above.
        DrawLineEx((Vector2){x + 15, y + 21}, (Vector2){x + 37, y + 21},
                   2.0f, color);
        DrawEllipse(x + 26, y + 25, 10.0f, 5.0f, color);
        DrawLine(x + 20, y + 8, x + 20, y + 14, color);
        DrawLine(x + 31, y + 6, x + 31, y + 12, color);
        break;
    case PET_BUTTON_PLAY:
        DrawCircle(x + 26, y + 23, 8.0f, color);
        DrawCircleLines(x + 23, y + 21, 4.0f, ColorAlpha(color, 0.7f));
        break;
    default:  // PET_BUTTON_SLEEP
        DrawCircle(x + 26, y + 23, 8.0f, color);
        DrawCircle(x + 30, y + 20, 7.0f, C_BTN_SLEEP);
        break;
    }
}

static void draw_button(uint8_t button, int x, const char *label,
                        Color base, const pet_game_t *pet)
{
    bool held = pet->pressed_button == button;
    bool sleep_lit = button == PET_BUTTON_SLEEP && pet->sleep_requested;
    DrawRectangleRounded((Rectangle){x, PET_BTN_Y, PET_BTN_W, PET_BTN_H},
                         0.22f, 6, base);
    DrawRectangleRounded((Rectangle){x, PET_BTN_Y, PET_BTN_W, 14},
                         0.4f, 4, ColorAlpha(WHITE, 0.22f));
    draw_button_icon(button, x, PET_BTN_Y, ColorAlpha(WHITE, 0.95f));

    int text_x = x + 44;
    int text_y = PET_BTN_Y + 15 + (held ? 2 : 0);
    draw_text_centered(label, text_x, text_y, 88, 17, WHITE);

    if (held)
        DrawRectangleRounded((Rectangle){x + 3, PET_BTN_Y + 3,
                                         PET_BTN_W - 6, PET_BTN_H - 6},
                             0.2f, 4, ColorAlpha(WHITE, 0.20f));
    if (sleep_lit)
        DrawRectangleRoundedLines((Rectangle){x, PET_BTN_Y, PET_BTN_W, PET_BTN_H},
                                  0.22f, 3.0f, WHITE);
}

static void draw_buttons(const pet_game_t *pet)
{
    const char *sleep_label = pet->sleep_requested ? "WAKE" : "SLEEP";
    draw_button(PET_BUTTON_FEED, PET_BTN1_X, "FEED", C_BTN_FEED, pet);
    draw_button(PET_BUTTON_PLAY, PET_BTN2_X, "PLAY", C_BTN_PLAY, pet);
    draw_button(PET_BUTTON_SLEEP, PET_BTN3_X, sleep_label, C_BTN_SLEEP, pet);
}

// ---- Frame ---------------------------------------------------------------
void pet_view_render(const pet_game_t *pet)
{
    if (!pet) return;
    BeginDrawing();
    draw_room(pet);
    draw_pet(pet);
    draw_night(pet);
    draw_fx(pet);
    draw_stat_panel(pet);
    draw_buttons(pet);
    EndDrawing();
}
