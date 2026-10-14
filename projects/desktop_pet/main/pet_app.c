// SPDX-License-Identifier: Apache-2.0
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "mosaico_game_2d.h"
#include "pet_app.h"
#include "pet_game.h"
#include "pet_screen_mirror.h"
#include "pet_view.h"

static pet_game_t s_pet;
static bool s_input;

static esp_err_t on_start(void)
{
    pet_game_reset(&s_pet, 0x4d4f4348u);
    return ESP_OK;
}

static void on_event(const mosaico_device_event_t *event)
{
    // The device reports contacts as TOUCH events whenever touch_points >= 2;
    // the host simulator (and single-point configs) arrive as POINTER.
    if (event->type != MOSAICO_DEVICE_EVENT_POINTER &&
        event->type != MOSAICO_DEVICE_EVENT_TOUCH) return;
    s_input = true;
    pet_game_pointer(&s_pet, event->x, event->y, event->pressed, 0);
}

static bool idle(void)
{
    bool skip = s_pet.behavior == PET_BEHAVIOR_IDLE && !s_input;
    s_input = false;
    return skip;
}

static void on_update(void)
{
    pet_game_update(&s_pet);
}

static void on_render(void)
{
    pet_view_render(&s_pet);
}

static const mosaico_game_app_config_t s_config = {
    .tag = "desktop_pet",
    .window_title = "Mosaico Desktop Pet",
    .canvas_bind = GSP_DESKTOP_PET_BIND_GAME_CANVAS,
    .touch_points = 2,
    .enable_imu = false,
    .imu_sample_ms = 0,
    .target_fps = 30,
    .gsp_bundle = gsp_bundle_config,
    .register_mirror = pet_screen_mirror_register,
    .before_display = NULL,
    .on_start = on_start,
    .after_healthy = NULL,
    .on_event = on_event,
    .idle = idle,
    .on_update = on_update,
    .on_render = on_render,
    .on_stats = NULL,
};

const mosaico_game_app_config_t *pet_app_config(void)
{
    return &s_config;
}
