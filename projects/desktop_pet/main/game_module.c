// SPDX-License-Identifier: Apache-2.0
#include <stdbool.h>
#include <stdio.h>
#include "mosaico_game_module.h"
#include "mosaico_raylib_fast.h"
#include "pet_game.h"
#include "pet_view.h"

typedef struct {
    pet_game_t pet;
    bool paused;
} pet_module_state_t;

static const char *behavior_name(pet_behavior_t behavior)
{
    static const char *names[] = {
        "idle", "happy", "eating", "playing", "sleeping", "sad", "dragged"
    };
    return (unsigned)behavior < sizeof(names) / sizeof(names[0])
               ? names[behavior] : "unknown";
}

static int initialize(void *value, const char *asset_root)
{
    (void)asset_root;
    pet_module_state_t *state = value;
    pet_game_reset(&state->pet, 0x4d4f4348u);
    InitWindow(PET_SCREEN_SIZE, PET_SCREEN_SIZE, "Mosaico Desktop Pet");
    SetTargetFPS(30);
    return 0;
}

static void shutdown(void *value)
{
    (void)value;
}

static void input(void *value, const mosaico_host_input_v1_t *event)
{
    pet_module_state_t *state = value;
    if (!state || !event) return;
    if (event->type == MOSAICO_HOST_INPUT_POINTER) {
        pet_game_pointer(&state->pet, event->x, event->y, event->pressed,
                         event->track_id);
    } else if (event->type == MOSAICO_HOST_INPUT_CONTROL) {
        if (event->code == MOSAICO_HOST_CONTROL_PAUSE) state->paused = true;
        else if (event->code == MOSAICO_HOST_CONTROL_RESUME)
            state->paused = false;
        else if (event->code == MOSAICO_HOST_CONTROL_RESET)
            pet_game_reset(&state->pet, 0x4d4f4348u);
    }
}

static void update(void *value)
{
    pet_module_state_t *state = value;
    if (!state || state->paused) return;
    pet_game_update(&state->pet);
}

static int render(void *value)
{
    pet_module_state_t *state = value;
    pet_view_render(&state->pet);
    return 0;
}

static uint32_t state_hash(const void *value)
{
    return pet_game_state_hash(&((const pet_module_state_t *)value)->pet);
}

static int state_json(const void *value, char *output, size_t capacity)
{
    const pet_game_t *pet = &((const pet_module_state_t *)value)->pet;
    return snprintf(output, capacity,
        "{\"behavior\":\"%s\",\"fullness\":%d,\"joy\":%d,\"energy\":%d,"
        "\"tick\":%lu,\"state_hash\":\"%08lx\"}",
        behavior_name(pet->behavior), (int)pet->fullness, (int)pet->joy,
        (int)pet->energy, (unsigned long)pet->tick,
        (unsigned long)pet_game_state_hash(pet));
}

static const mosaico_game_module_v1_t s_module = {
    .descriptor = {MOSAICO_HOST_GAME_ABI_V1, "desktop_pet", "Mosaico Desktop Pet",
                   480, 480, 30, 2},
    .state_size = sizeof(pet_module_state_t),
    .initialize = initialize,
    .shutdown = shutdown,
    .input = input,
    .update = update,
    .render = render,
    .state_hash = state_hash,
    .state_json = state_json,
};

const mosaico_game_module_v1_t *mosaico_game_module_v1(void)
{
    return &s_module;
}
