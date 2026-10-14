// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include "pet_game.h"

#define PET_BODY_HIT_RX 106.0f
#define PET_BODY_HIT_RY 98.0f

static float clampf(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

static uint16_t next_rand(pet_game_t *pet)
{
    pet->rng = pet->rng * 1664525u + 1013904223u;
    return (uint16_t)(pet->rng >> 16);
}

static uint16_t rand_range(pet_game_t *pet, uint16_t low, uint16_t span)
{
    return (uint16_t)(low + (uint16_t)(next_rand(pet) % span));
}

void pet_game_reset(pet_game_t *pet, uint32_t seed)
{
    if (!pet) return;
    for (size_t i = 0; i < PET_MAX_FX; ++i) pet->fx[i].active = false;
    pet->x = PET_HOME_X;
    pet->y = PET_HOME_Y;
    pet->target_x = PET_HOME_X;
    pet->target_y = PET_HOME_Y;
    pet->fullness = 72.0f;
    pet->joy = 80.0f;
    pet->energy = 78.0f;
    pet->behavior = PET_BEHAVIOR_IDLE;
    pet->tick = 0;
    pet->rng = seed ? seed : 0x4d4f4348u;
    pet->action_ticks = 0;
    pet->happy_ticks = 0;
    pet->blink_next = 80;
    pet->blink_phase = 0;
    pet->look_dir = 0;
    pet->look_ticks = 0;
    pet->dragging = false;
    pet->sleep_requested = false;
    pet->drag_dx = pet->drag_dy = 0;
    pet->press_x = pet->press_y = 0;
    pet->pointer_down = false;
    pet->pressed_button = 0;
    pet->activate_button = 0;
    pet->pet_ticks = 0;
}

bool pet_game_hit_body(const pet_game_t *pet, int x, int y)
{
    float dx = ((float)x - pet->x) / PET_BODY_HIT_RX;
    float dy = ((float)y - pet->y) / PET_BODY_HIT_RY;
    return dx * dx + dy * dy <= 1.0f;
}

int pet_game_hit_button(int x, int y)
{
    if (y < PET_BTN_Y || y > PET_BTN_Y + PET_BTN_H) return 0;
    static const int xs[PET_BTN_COUNT] = {PET_BTN1_X, PET_BTN2_X, PET_BTN3_X};
    for (int i = 0; i < PET_BTN_COUNT; ++i)
        if (x >= xs[i] && x <= xs[i] + PET_BTN_W) return i + 1;
    return 0;
}

void pet_game_pointer(pet_game_t *pet, int x, int y, bool pressed, int track)
{
    (void)track;
    if (!pet) return;
    if (pressed) {
        bool fresh_press = !pet->pointer_down;
        pet->press_x = x;
        pet->press_y = y;
        pet->pointer_down = true;
        int button = pet_game_hit_button(x, y);
        if (button) {
            pet->pressed_button = (uint8_t)button;
            return;
        }
        // The drag anchor is fixed at the initial press; moves must not reset it.
        if (fresh_press && !pet->dragging && pet_game_hit_body(pet, x, y)) {
            if (pet->behavior == PET_BEHAVIOR_SLEEP)
                pet->sleep_requested = false;  // a tap wakes Mochi up
            pet->drag_dx = x - (int)pet->x;
            pet->drag_dy = y - (int)pet->y;
        }
    } else {
        pet->pointer_down = false;
        if (pet->pressed_button) {
            int button = pet_game_hit_button(x, y);
            if (button == (int)pet->pressed_button)
                pet->activate_button = pet->pressed_button;
            pet->pressed_button = 0;
        }
        if (pet->dragging) {
            pet->dragging = false;
            pet->happy_ticks = 32;
        }
        pet->pet_ticks = 0;
    }
}

static void spawn_fx(pet_game_t *pet, uint8_t kind, float x, float y)
{
    int slot = -1, oldest = 0;
    uint16_t oldest_life = 0xffffU;
    for (int i = 0; i < PET_MAX_FX; ++i) {
        if (!pet->fx[i].active) { slot = i; break; }
        if (pet->fx[i].life < oldest_life) {
            oldest_life = pet->fx[i].life;
            oldest = i;
        }
    }
    if (slot < 0) slot = oldest;
    pet_fx_t *fx = &pet->fx[slot];
    fx->active = true;
    fx->x = x;
    fx->y = y;
    fx->kind = kind;
    fx->vx = fx->vy = 0.0f;
    switch (kind) {
    case PET_FX_HEART:
        fx->vx = (float)(next_rand(pet) % 60) / 100.0f - 0.3f;
        fx->vy = -0.7f - (float)(next_rand(pet) % 50) / 100.0f;
        fx->life = fx->max_life = 52;
        break;
    case PET_FX_SLEEP:
        fx->vx = 0.22f;
        fx->vy = -0.42f;
        fx->life = fx->max_life = 72;
        break;
    case PET_FX_NOTE:
        fx->vx = 0.35f + (float)(next_rand(pet) % 40) / 100.0f;
        fx->vy = -0.85f - (float)(next_rand(pet) % 30) / 100.0f;
        fx->life = fx->max_life = 46;
        break;
    case PET_FX_CRUMB:
        fx->vx = (float)(next_rand(pet) % 120) / 100.0f - 0.6f;
        fx->vy = -1.6f - (float)(next_rand(pet) % 40) / 100.0f;
        fx->life = fx->max_life = 42;
        break;
    default:  // PET_FX_SPARK
        fx->vx = (float)(next_rand(pet) % 160) / 100.0f - 0.8f;
        fx->vy = -1.1f - (float)(next_rand(pet) % 50) / 100.0f;
        fx->life = fx->max_life = 28;
        break;
    }
}

static void pick_play_target(pet_game_t *pet)
{
    pet->target_x = (float)rand_range(pet, 90, 300);
    pet->target_y = (float)rand_range(pet, 190, 160);
}

static bool body_busy(const pet_game_t *pet)
{
    return pet->sleep_requested || pet->action_ticks > 0;
}

static void update_held_pointer(pet_game_t *pet)
{
    if (!pet->pointer_down || pet->pressed_button) return;
    bool on_body = pet_game_hit_body(pet, pet->press_x, pet->press_y);
    if (!on_body && !pet->dragging) return;
    int move_x = pet->press_x - (int)pet->x - pet->drag_dx;
    int move_y = pet->press_y - (int)pet->y - pet->drag_dy;
    if (!pet->dragging && (abs(move_x) > 6 || abs(move_y) > 6) && !body_busy(pet))
        pet->dragging = true;
    if (pet->dragging) {
        pet->target_x = (float)(pet->press_x - pet->drag_dx);
        pet->target_y = (float)(pet->press_y - pet->drag_dy);
        pet->pet_ticks = 0;
    } else if (!body_busy(pet)) {
        // Standing touch on the body counts as petting.
        pet->pet_ticks++;
        if ((pet->pet_ticks & 7U) == 0) {
            pet->joy = clampf(pet->joy + 0.9f, 0.0f, 100.0f);
            pet->happy_ticks = 26;
            spawn_fx(pet, PET_FX_HEART,
                     pet->x + (float)(next_rand(pet) % 70) - 35.0f, pet->y - 70.0f);
        }
    }
}

static void handle_buttons(pet_game_t *pet)
{
    if (!pet->activate_button) return;
    uint8_t button = pet->activate_button;
    pet->activate_button = 0;
    if (button == PET_BUTTON_SLEEP) {
        pet->sleep_requested = !pet->sleep_requested;
        if (pet->sleep_requested) {
            pet->action_ticks = 0;
            pet->dragging = false;
            pet->target_x = PET_HOME_X;
            pet->target_y = PET_HOME_Y;
        }
        return;
    }
    if (pet->sleep_requested || pet->action_ticks > 0) return;
    if (button == PET_BUTTON_FEED && pet->fullness < 92.0f) {
        pet->behavior = PET_BEHAVIOR_EATING;
        pet->action_ticks = PET_EAT_TICKS;
    } else if (button == PET_BUTTON_PLAY && pet->energy > 18.0f) {
        pet->behavior = PET_BEHAVIOR_PLAY;
        pet->action_ticks = PET_PLAY_TICKS;
        pick_play_target(pet);
    }
}

static void update_blink(pet_game_t *pet)
{
    if (pet->blink_phase) {
        pet->blink_phase++;
        if (pet->blink_phase > 4) pet->blink_phase = 0;
    } else if (--pet->blink_next == 0) {
        pet->blink_phase = 1;
        pet->blink_next = rand_range(pet, 65, 115);
    }
}

static void update_look(pet_game_t *pet)
{
    if (pet->look_ticks) {
        pet->look_ticks--;
        if (pet->look_ticks == 0) pet->look_dir = 0;
    } else if ((next_rand(pet) & 63U) == 0) {
        pet->look_dir = (next_rand(pet) & 1U) ? 1 : -1;
        pet->look_ticks = rand_range(pet, 30, 25);
    }
}

static void update_fx(pet_game_t *pet)
{
    for (int i = 0; i < PET_MAX_FX; ++i) {
        pet_fx_t *fx = &pet->fx[i];
        if (!fx->active) continue;
        fx->life--;
        if (fx->life == 0) { fx->active = false; continue; }
        fx->x += fx->vx;
        fx->y += fx->vy;
        if (fx->kind == PET_FX_CRUMB) {
            fx->vy += 0.12f;
        } else {
            fx->vy *= 0.97f;
            fx->vx *= 0.99f;
        }
    }
}

void pet_game_update(pet_game_t *pet)
{
    if (!pet) return;
    pet->tick++;
    handle_buttons(pet);
    update_held_pointer(pet);

    if (pet->sleep_requested) {
        // Sleeping: recover energy and slowly drift back home.
        pet->x += (PET_HOME_X - pet->x) * 0.12f;
        pet->y += (PET_HOME_Y - pet->y) * 0.12f;
        pet->energy = clampf(pet->energy + 0.42f, 0.0f, 100.0f);
        if ((pet->tick % 26U) == 0)
            spawn_fx(pet, PET_FX_SLEEP, pet->x + 58.0f, pet->y - 86.0f);
    } else if (pet->action_ticks > 0 && pet->behavior == PET_BEHAVIOR_EATING) {
        pet->action_ticks--;
        if ((pet->tick & 3U) == 0)
            spawn_fx(pet, PET_FX_CRUMB, pet->x - 8.0f, pet->y + 24.0f);
        if (pet->action_ticks == 0) {
            pet->fullness = clampf(pet->fullness + 34.0f, 0.0f, 100.0f);
            pet->joy = clampf(pet->joy + 6.0f, 0.0f, 100.0f);
            pet->happy_ticks = 26;
            for (int i = 0; i < 4; ++i)
                spawn_fx(pet, PET_FX_SPARK, pet->x - 30.0f + (float)i * 20.0f,
                         pet->y - 60.0f);
        }
    } else if (pet->action_ticks > 0 && pet->behavior == PET_BEHAVIOR_PLAY) {
        pet->action_ticks--;
        float dx = pet->target_x - pet->x;
        float dy = pet->target_y - pet->y;
        pet->x += dx * 0.11f;
        pet->y += dy * 0.11f;
        if (dx * dx + dy * dy < 196.0f) pick_play_target(pet);
        pet->joy = clampf(pet->joy + 0.16f, 0.0f, 100.0f);
        pet->energy = clampf(pet->energy - 0.10f, 0.0f, 100.0f);
        pet->fullness = clampf(pet->fullness - 0.03f, 0.0f, 100.0f);
        if ((pet->tick % 18U) == 0)
            spawn_fx(pet, PET_FX_NOTE, pet->x + 46.0f, pet->y - 70.0f);
        if (pet->action_ticks == 0) {
            pet->target_x = PET_HOME_X;
            pet->target_y = PET_HOME_Y;
            pet->happy_ticks = 30;
        }
    } else if (!pet->dragging) {
        // No transient action: ease back to the home spot, stats decay.
        pet->x += (PET_HOME_X - pet->x) * 0.16f;
        pet->y += (PET_HOME_Y - pet->y) * 0.16f;
        pet->fullness = clampf(pet->fullness - 0.010f, 0.0f, 100.0f);
        float joy_drain = pet->fullness < 25.0f ? 0.020f : 0.008f;
        pet->joy = clampf(pet->joy - joy_drain, 0.0f, 100.0f);
        pet->energy = clampf(pet->energy - 0.004f, 0.0f, 100.0f);
        update_look(pet);
    }

    if (pet->happy_ticks) pet->happy_ticks--;

    // Resolve the behavior presented by the view.
    if (pet->sleep_requested) pet->behavior = PET_BEHAVIOR_SLEEP;
    else if (pet->action_ticks > 0) pet->behavior = pet->behavior;  // eat/play own it
    else if (pet->dragging) pet->behavior = PET_BEHAVIOR_DRAGGED;
    else if (pet->happy_ticks > 0) pet->behavior = PET_BEHAVIOR_HAPPY;
    else if (pet->fullness < 20.0f || pet->joy < 20.0f)
        pet->behavior = PET_BEHAVIOR_SAD;
    else pet->behavior = PET_BEHAVIOR_IDLE;

    update_blink(pet);
    update_fx(pet);
}

static void fnv_bytes(uint32_t *hash, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        *hash ^= bytes[i];
        *hash *= 16777619u;
    }
}

uint32_t pet_game_state_hash(const pet_game_t *pet)
{
    if (!pet) return 0;
    uint32_t hash = 2166136261u;
    fnv_bytes(&hash, &pet->x, sizeof(pet->x));
    fnv_bytes(&hash, &pet->y, sizeof(pet->y));
    fnv_bytes(&hash, &pet->fullness, sizeof(pet->fullness));
    fnv_bytes(&hash, &pet->joy, sizeof(pet->joy));
    fnv_bytes(&hash, &pet->energy, sizeof(pet->energy));
    fnv_bytes(&hash, &pet->behavior, sizeof(pet->behavior));
    fnv_bytes(&hash, &pet->tick, sizeof(pet->tick));
    fnv_bytes(&hash, &pet->action_ticks, sizeof(pet->action_ticks));
    fnv_bytes(&hash, &pet->blink_phase, sizeof(pet->blink_phase));
    fnv_bytes(&hash, &pet->look_dir, sizeof(pet->look_dir));
    fnv_bytes(&hash, &pet->sleep_requested, sizeof(pet->sleep_requested));
    fnv_bytes(&hash, &pet->dragging, sizeof(pet->dragging));
    for (int i = 0; i < PET_MAX_FX; ++i)
        if (pet->fx[i].active)
            fnv_bytes(&hash, &pet->fx[i], sizeof(pet_fx_t));
    return hash;
}
