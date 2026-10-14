// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "pet_game.h"

// Shared C view: compiled unchanged by both the Host simulator and the device.
void pet_view_render(const pet_game_t *pet);
