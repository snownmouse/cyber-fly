// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Shared layout constants used by the model, the view and the device app.
#define PET_SCREEN_SIZE 480
#define PET_HOME_X 240.0f
#define PET_HOME_Y 278.0f

// Bottom action buttons.
#define PET_BTN_COUNT 3
#define PET_BTN_Y 410
#define PET_BTN_H 46
#define PET_BTN_W 132
#define PET_BTN1_X 12
#define PET_BTN2_X 156
#define PET_BTN3_X 300

#define PET_BUTTON_FEED 1
#define PET_BUTTON_PLAY 2
#define PET_BUTTON_SLEEP 3

#define PET_EAT_TICKS 75
#define PET_PLAY_TICKS 150

#define PET_MAX_FX 18

typedef enum {
    PET_BEHAVIOR_IDLE = 0,
    PET_BEHAVIOR_HAPPY,
    PET_BEHAVIOR_EATING,
    PET_BEHAVIOR_PLAY,
    PET_BEHAVIOR_SLEEP,
    PET_BEHAVIOR_SAD,
    PET_BEHAVIOR_DRAGGED
} pet_behavior_t;

typedef enum {
    PET_FX_HEART = 0,   // floats up while the pet is petted
    PET_FX_SLEEP,       // Zzz while sleeping
    PET_FX_NOTE,        // music notes while playing
    PET_FX_CRUMB,       // food crumbs while eating
    PET_FX_SPARK        // sparkle when a stat tops up
} pet_fx_kind_t;

typedef struct {
    bool active;
    float x, y, vx, vy;
    uint8_t kind;
    uint16_t life, max_life;
} pet_fx_t;

typedef struct {
    // Current center of the pet (world == screen, 480x480).
    float x, y;
    float target_x, target_y;
    // Stats in the 0..100 range.
    float fullness, joy, energy;
    pet_behavior_t behavior;
    uint32_t tick, rng;
    uint16_t action_ticks;    // remaining eat/play time
    uint16_t happy_ticks;     // short happy linger after petting
    uint16_t blink_next;      // ticks until the next blink
    uint8_t blink_phase;      // 0 = open, 1..4 = blinking
    int8_t look_dir;          // -1/0/1 glance offset
    uint16_t look_ticks;      // remaining glance time
    bool dragging;
    bool sleep_requested;
    int drag_dx, drag_dy;     // pointer-to-center offset while dragging
    int press_x, press_y;     // current pointer position
    bool pointer_down;
    uint8_t pressed_button;   // button currently held
    uint8_t activate_button;  // button released this frame (consumed by update)
    uint16_t pet_ticks;       // continuous petting timer
    pet_fx_t fx[PET_MAX_FX];
} pet_game_t;

void pet_game_reset(pet_game_t *pet, uint32_t seed);
void pet_game_pointer(pet_game_t *pet, int x, int y, bool pressed, int track);
void pet_game_update(pet_game_t *pet);
bool pet_game_hit_body(const pet_game_t *pet, int x, int y);
int pet_game_hit_button(int x, int y);
uint32_t pet_game_state_hash(const pet_game_t *pet);
