// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "fly_game.h"

// Shared C view: compiled unchanged by both the Host simulator and the device.
void fly_view_render(const fly_game_t *fly);
