/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file board_probe.h
 * @brief Board identity readback and the BMI270 address self-check.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Board revision reported by the BSP (read from eFuse USER_DATA). */
    bool    variant_ok;
    uint8_t hw_major;
    uint8_t hw_minor;
    bool    is_v1_0;          /* pins differ a lot between v1.0 and v1.2 */

    /* Silicon and image identity. */
    char    target[24];
    char    idf_version[32];
    char    app_version[32];
    uint8_t cores;
    uint16_t revision;
    uint32_t cpu_mhz;
    uint32_t flash_mb;
    uint32_t psram_mb;
    int     reset_reason;
    uint8_t mac[6];

    /* BMI270 self-check. Passing this proves the 0x69 address is correct. */
    bool    imu_ok;
    float   accel[3];
    float   gyro[3];
    float   accel_magnitude;
} board_probe_result_t;

/**
 * @brief Print board identity and run the BMI270 self-check.
 *
 * Requires bsp_power_init() to have run first so the 3.3 V rails are up.
 */
void board_probe_run(board_probe_result_t *out);

#ifdef __cplusplus
}
#endif
