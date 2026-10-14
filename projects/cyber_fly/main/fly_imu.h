// SPDX-License-Identifier: Apache-2.0
//
// Board-side IMU for the cyber fly's steering fast-reflex.
//
// Ported (trimmed) from tests/firmware/hw_probe/main/board_probe.c. The
// important part is not the BMI270 access - the BSP already wraps that - but
// the two start-up traps Task A hit, both kept here:
//
//   * BMI270 data registers read back as ZERO until the first conversion lands
//     after start-up. Averaging those zeros is how a board lying flat on the
//     desk reported |a| = 0.59 g instead of 1.0 g. Hence the settle delay and
//     the "skip all-zero samples" test.
//   * The IMU address on this board is 0x69, not the more common 0x68. That is
//     the BSP's business here, but it is why a driver that hardcodes 0x68 finds
//     nothing.
//
// Why the board supplies steering at all: in the v4 subcircuit the LEFT/RIGHT
// (DNa) neurons have no named sensory input - they only receive the fast
// reflex. So the brain genuinely cannot tell left from right on its own, and a
// cloud round trip cannot supply it either (that is ~100 ms, far too slow for
// a balance reflex). Design doc section 0.5: accel[0] goes straight to the
// flag, with no neural step in between.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the BMI270 and wait for it to produce real samples.
 *
 * Not fatal on failure: the caller keeps sending accel = 0, which reads as
 * "no lateral acceleration" rather than as a fabricated turn.
 */
esp_err_t fly_imu_open(float settle_ms);

/**
 * @brief Read the current acceleration in g.
 *
 * @param out_lateral  Signed sideways acceleration. Positive means the board is
 *                    accelerating to its right.
 * @return ESP_OK when a real (non-zero) sample was read.
 */
esp_err_t fly_imu_read(float *out_lateral);

/** True once fly_imu_open() succeeded. */
bool fly_imu_ready(void);

#ifdef __cplusplus
}
#endif
