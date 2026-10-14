// SPDX-License-Identifier: Apache-2.0
#include <math.h>
#include <stddef.h>
#include <stdio.h>   // snprintf(), used by the telemetry readout
#include "mosaico_game.h"
#include "mosaico_raylib_fast.h"
#include "fly_view.h"

// ---- Palette (cyber lab) -------------------------------------------------
static const Color C_BG_TOP = {14, 20, 38, 255};
static const Color C_BG_BOT = {22, 30, 54, 255};
static const Color C_GRID = {40, 52, 84, 120};
static const Color C_BODY = {196, 214, 236, 255};   // thorax/abdomen shell
static const Color C_BODY_HI = {232, 242, 255, 255};
static const Color C_HEAD = {210, 226, 246, 255};
static const Color C_WING = {150, 210, 255, 90};     // translucent wing
static const Color C_WING_EDGE = {180, 230, 255, 150};
static const Color C_LEG = {120, 140, 170, 255};
static const Color C_EYE = {255, 120, 120, 255};      // compound eye
static const Color C_EYE_HI = {255, 220, 220, 255};
static const Color C_ANTENNA = {180, 200, 230, 255};

static const Color C_PANEL = {16, 22, 40, 220};
static const Color C_BAR_BG = {44, 54, 84, 255};
static const Color C_BAR_AROUSAL = {240, 120, 90, 255};
static const Color C_LINK_OK = {110, 220, 160, 255};
static const Color C_LINK_BAD = {240, 120, 120, 255};
static const Color C_TEXT = {200, 214, 240, 255};
static const Color C_TEXT_DIM = {130, 146, 178, 255};

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

// Rotate a local point (lx,ly) around (cx,cy) by angle a — used so the whole
// fly turns with its heading (source: fly->heading <- state.turn).
static Vector2 rot(float cx, float cy, float lx, float ly, float a)
{
    float c = cosf(a), s = sinf(a);
    return (Vector2){cx + lx * c - ly * s, cy + lx * s + ly * c};
}

// Filled ellipse at (cx,cy) with radii (rx,ry), oriented by angle a.
static void fill_ellipse(float cx, float cy, float rx, float ry, float a,
                         Color col)
{
    Vector2 pts[28];
    const int N = 28;
    for (int i = 0; i < N; ++i) {
        float th = (float)i / (float)N * 6.2831853f;
        float lx = cosf(th) * rx, ly = sinf(th) * ry;
        pts[i] = rot(cx, cy, lx, ly, a);
    }
    DrawTriangleFan(pts, N, col);
}

static void draw_text_centered(const char *text, int x, int y, int width,
                               int size, Color color)
{
    DrawText(text, x + (width - MeasureText(text, size)) / 2, y, size, color);
}

// ---- Background ----------------------------------------------------------
// Steady cyber-lab backdrop. No animation here so it never reads as decorative.
static void draw_bg(void)
{
    DrawRectangleGradientV(0, 0, MOSAICO_GAME_WIDTH, MOSAICO_GAME_HEIGHT,
                           C_BG_TOP, C_BG_BOT);
    for (int gx = 0; gx <= 480; gx += 40)
        DrawLine(gx, 0, gx, 480, C_GRID);
    for (int gy = 0; gy <= 480; gy += 40)
        DrawLine(0, gy, 480, gy, C_GRID);
}

// ---- Fly body ------------------------------------------------------------
// Every animated sub-part below traces to a real fly_state_t field.
static void draw_fly(const fly_game_t *fly)
{
    float cx = FLY_HOME_X;
    float cy = FLY_HOME_Y;
    float a = fly->heading;  // body orientation <- state.turn

    bool escaping = (fly->state.flags & FLY_FLAG_ESCAPE) != 0;
    float motor_norm = (float)fly->state.motor / 255.0f;
    float thrust_norm = (float)fly->state.thrust / 255.0f;
    float groom_norm = (float)fly->state.groom / 255.0f;

    // Forward lean: body nudges along its heading with thrust (speed lines).
    // Source: state.thrust.
    Vector2 lean = rot(cx, cy, 8.0f * thrust_norm, 0.0f, a);
    cx = lean.x; cy = lean.y;

    // Speed lines trailing behind the fly: count + length scale with thrust.
    // Source: state.thrust.
    int lines = (int)(thrust_norm * 6.0f);
    for (int i = 0; i < lines; ++i) {
        float back = -(28.0f + (float)i * 14.0f);
        Vector2 p1 = rot(cx, cy, back, (float)(i - lines / 2) * 10.0f, a);
        Vector2 p2 = rot(cx, cy, back - 18.0f - thrust_norm * 22.0f,
                         (float)(i - lines / 2) * 10.0f, a);
        DrawLineEx(p1, p2, 2.0f, ColorAlpha(C_BODY_HI,
                    0.25f + thrust_norm * 0.5f));
    }

    // Wings: two translucent ellipses hinged at the thorax, flapping with
    // wing_phase. Amplitude grows with motor + escape startle.
    // Source: motor + escape (-> wing_phase).
    float wing_amp = 0.30f + motor_norm * 0.55f + (escaping ? 0.45f : 0.0f);
    float flap = sinf(fly->wing_phase) * wing_amp;
    for (int side = -1; side <= 1; side += 2) {
        float hinge_x = -6.0f, hinge_y = (float)side * 14.0f;
        Vector2 h = rot(cx, cy, hinge_x, hinge_y, a);
        float wang = a + (float)side * (0.5f + flap);
        // Wing sweeps outward (negative local x = behind) and up.
        Vector2 wpos = rot(h.x, h.y, -34.0f, (float)side * 26.0f, wang);
        fill_ellipse(wpos.x, wpos.y, 40.0f, 18.0f, wang, C_WING);
        DrawEllipseLines((int)wpos.x, (int)wpos.y, 40.0f, 18.0f, C_WING_EDGE);
    }

    // Abdomen (rear segment) + thorax (mid segment) + head (front).
    Vector2 abdomen = rot(cx, cy, -34.0f, 0.0f, a);
    fill_ellipse(abdomen.x, abdomen.y, 48.0f, 34.0f, a, C_BODY);
    Vector2 thorax = rot(cx, cy, 6.0f, 0.0f, a);
    fill_ellipse(thorax.x, thorax.y, 34.0f, 30.0f, a, C_BODY_HI);

    // Startle flash: a bright ring pulses around the body on escape.
    // Source: state.flags escape + motor.
    if (escaping || motor_norm > 0.5f) {
        float ring = 0.5f + 0.5f * sinf(fly->wing_phase * 2.0f);
        DrawEllipseLines((int)thorax.x, (int)thorax.y, 70.0f + ring * 8.0f,
                         62.0f + ring * 8.0f,
                         ColorAlpha(C_EYE_HI, 0.35f + motor_norm * 0.4f));
    }

    // Head + compound eyes (front, +x local).
    Vector2 head = rot(cx, cy, 44.0f, 0.0f, a);
    DrawCircleV(head, 20.0f, C_HEAD);
    for (int side = -1; side <= 1; side += 2) {
        Vector2 eye = rot(cx, cy, 50.0f, (float)side * 11.0f, a);
        DrawCircleV(eye, 9.0f, C_EYE);
        DrawCircleV((Vector2){eye.x - 2, eye.y - 3}, 2.4f, C_EYE_HI);
    }

    // Antennae: two strokes forward from the head; they quiver with arousal.
    // Source: state.arousal.
    float quiver = (float)fly->state.arousal / 255.0f * 0.5f;
    for (int side = -1; side <= 1; side += 2) {
        Vector2 base = rot(cx, cy, 56.0f, (float)side * 8.0f, a);
        float wob = sinf(fly->wing_phase * 1.3f + (float)side) * quiver;
        Vector2 tip = rot(cx, cy, 78.0f,
                          (float)side * 18.0f + wob * 14.0f, a);
        DrawLineEx(base, tip, 2.2f, C_ANTENNA);
        DrawCircleV(tip, 2.6f, C_ANTENNA);
    }

    // Six legs: strokes from thorax/abdomen. When grooming, the front pair
    // rubs together (phase from groom). Source: state.groom.
    for (int i = 0; i < 3; ++i) {
        float lx = -20.0f + (float)i * 22.0f;
        for (int side = -1; side <= 1; side += 2) {
            Vector2 root = rot(cx, cy, lx, (float)side * 16.0f, a);
            float swing;
            if (i == 0 && groom_norm > 0.05f) {
                // Front legs groom: tips meet under the head, oscillating.
                swing = sinf(fly->groom_phase) * 10.0f * groom_norm;
            } else {
                swing = sinf(fly->wing_phase * 1.1f + (float)i) * 4.0f;
            }
            Vector2 foot = rot(cx, cy, lx - 6.0f,
                               (float)side * (40.0f + swing), a);
            DrawLineEx(root, foot, 1.8f, C_LEG);
        }
    }
}

// ---- Telemetry panel -----------------------------------------------------
// Arousal bar + corner link/brain readouts. All from real state fields.
static void draw_telemetry(const fly_game_t *fly)
{
    // Arousal bar (top-left). Source: state.arousal.
    DrawRectangleRounded((Rectangle){12, 12, 200, 40}, 0.18f, 6, C_PANEL);
    DrawText("AROUSAL", 24, 20, 11, C_TEXT);
    DrawRectangleRounded((Rectangle){24, 38, 176, 10}, 0.5f, 4, C_BAR_BG);
    int aw = (int)(176.0f * clampf((float)fly->state.arousal / 255.0f, 0, 1));
    if (aw >= 6)
        DrawRectangleRounded((Rectangle){24, 38, (float)aw, 10}, 0.5f, 4,
                             C_BAR_AROUSAL);

    // Link quality bar (top-right). Source: state.link_q.
    Color link_col = fly->state.link_q > 120 ? C_LINK_OK : C_LINK_BAD;
    DrawRectangleRounded((Rectangle){268, 12, 200, 40}, 0.18f, 6, C_PANEL);
    DrawText("LINK", 280, 20, 11, C_TEXT);
    DrawRectangleRounded((Rectangle){280, 38, 176, 10}, 0.5f, 4, C_BAR_BG);
    int lw = (int)(176.0f * clampf((float)fly->state.link_q / 255.0f, 0, 1));
    if (lw >= 6)
        DrawRectangleRounded((Rectangle){280, 38, (float)lw, 10}, 0.5f, 4,
                             link_col);

    // Corner readout: brain load + decoded flags + seq. Sources: brain_load,
    // flags, seq.
    char buf[64];
    DrawText("BRAIN", 280, 60, 11, C_TEXT);
    DrawRectangleRounded((Rectangle){280, 78, 176, 10}, 0.5f, 4, C_BAR_BG);
    int bw = (int)(176.0f * clampf((float)fly->state.brain_load / 255.0f, 0, 1));
    if (bw >= 6)
        DrawRectangleRounded((Rectangle){280, 78, (float)bw, 10}, 0.5f, 4,
                             C_TEXT_DIM);

    snprintf(buf, sizeof(buf), "seq %lu", (unsigned long)fly->seq);
    DrawText(buf, 24, 60, 11, C_TEXT_DIM);

    // Flag glyphs: E(escape) L R G(groom) F(feed) ! (estop).
    char fline[16];
    int n = 0;
    if (fly->state.flags & FLY_FLAG_ESCAPE) fline[n++] = 'E';
    if (fly->state.flags & FLY_FLAG_LEFT)   fline[n++] = 'L';
    if (fly->state.flags & FLY_FLAG_RIGHT)  fline[n++] = 'R';
    if (fly->state.flags & FLY_FLAG_GROOM)  fline[n++] = 'G';
    if (fly->state.flags & FLY_FLAG_FEED)   fline[n++] = 'F';
    if (fly->state.flags & FLY_FLAG_ESTOP)  fline[n++] = '!';
    if (n == 0) fline[n++] = '-';
    fline[n] = '\0';
    snprintf(buf, sizeof(buf), "FLAGS %s  motor%d groom%d feed%d",
             fline, (int)fly->state.motor, (int)fly->state.groom,
             (int)fly->state.feed);
    DrawText(buf, 24, 78, 11, C_TEXT_DIM);

    // Frame health (CRC). Source: frame_valid.
    DrawText(fly->frame_valid ? "MPRC CRC OK" : "MPRC CRC BAD",
             24, 96, 11,
             fly->frame_valid ? C_LINK_OK : C_LINK_BAD);

    // Hint line.
    draw_text_centered("tap = inject ESCAPE", 0, 452, 480, 12, C_TEXT_DIM);
}

// ---- FX ------------------------------------------------------------------
static void draw_fx(const fly_game_t *fly)
{
    for (int i = 0; i < FLY_MAX_FX; ++i) {
        const fly_fx_t *fx = &fly->fx[i];
        if (!fx->active) continue;
        Color tint = alpha_of((Color){255, 255, 255, 255}, fx->life,
                              fx->max_life);
        switch (fx->kind) {
        case FLY_FX_SPARK: {
            Color spark = ColorAlpha((Color){120, 220, 255, 255}, tint.a / 255.0f);
            DrawCircleV((Vector2){fx->x, fx->y}, 2.2f, spark);
            DrawLineEx((Vector2){fx->x, fx->y - 5}, (Vector2){fx->x, fx->y + 5},
                       1.4f, spark);
            DrawLineEx((Vector2){fx->x - 5, fx->y}, (Vector2){fx->x + 5, fx->y},
                       1.4f, spark);
            break;
        }
        case FLY_FX_PING:
            DrawCircleV((Vector2){fx->x, fx->y}, 3.0f,
                        ColorAlpha((Color){110, 220, 160, 255}, tint.a / 255.0f));
            break;
        default:  // FLY_FX_DUST
            DrawCircleV((Vector2){fx->x, fx->y}, 2.0f,
                        ColorAlpha((Color){210, 200, 170, 255}, tint.a / 255.0f));
            break;
        }
    }
}

// ---- Frame ---------------------------------------------------------------
void fly_view_render(const fly_game_t *fly)
{
    if (!fly) return;
    BeginDrawing();
    draw_bg();
    draw_fly(fly);
    draw_fx(fly);
    draw_telemetry(fly);
    EndDrawing();
}
