// SPDX-License-Identifier: Apache-2.0
//
// See fly_imu.h for the two Task A start-up traps this preserves.
#include "fly_imu.h"

#include <math.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fly_imu";

/* The BMI270 runs at 100 Hz by default, so 20 ms is one fresh sample. */
#define IMU_SETTLE_MS_DEFAULT 120
#define IMU_READ_GAP_MS        20
#define IMU_READ_ATTEMPTS      5
/* Below this the sample is indistinguishable from the start-up zeros. */
#define IMU_NOISE_FLOOR_G      0.05f

static bool s_ready;

esp_err_t fly_imu_open(float settle_ms)
{
    /* bsp_imu_init() already probes the BMI270 at 0x69 and configures it. */
    ESP_RETURN_ON_ERROR(bsp_imu_init(), TAG, "bsp_imu_init failed (BMI270 is at 0x69)");
    ESP_RETURN_ON_ERROR(bsp_imu_start(NULL), TAG, "bsp_imu_start failed");

    /*
     * Wait for the first conversion. Without this the first reads come back as
     * all-zero, and Task A measured exactly that failure: |a| = 0.59 g for a
     * board resting flat on the desk.
     */
    const uint32_t ms = (settle_ms > 0.0f) ? (uint32_t)settle_ms : IMU_SETTLE_MS_DEFAULT;
    vTaskDelay(pdMS_TO_TICKS(ms));

    /* Prove it produces real data before declaring success. */
    for (int i = 0; i < IMU_READ_ATTEMPTS; i++) {
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        if (bsp_imu_get_accel(&ax, &ay, &az) == ESP_OK &&
            fabsf(ax) + fabsf(ay) + fabsf(az) > IMU_NOISE_FLOOR_G) {
            s_ready = true;
            ESP_LOGI(TAG, "BMI270 就绪：accel x=%.3f y=%.3f z=%.3f（|a|=%.3f g）",
                     (double)ax, (double)ay, (double)az,
                     (double)sqrtf(ax * ax + ay * ay + az * az));
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_READ_GAP_MS));
    }

    s_ready = false;
    ESP_LOGE(TAG, "BMI270 一直读不到有效样本（保持静止再试）");
    return ESP_ERR_TIMEOUT;
}

esp_err_t fly_imu_read(float *out_lateral)
{
    ESP_RETURN_ON_FALSE(out_lateral, ESP_ERR_INVALID_ARG, TAG, "out_lateral is null");
    *out_lateral = 0.0f;
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "IMU not opened");

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    ESP_RETURN_ON_ERROR(bsp_imu_get_accel(&ax, &ay, &az), TAG, "bsp_imu_get_accel failed");

    /* Reject the zero sample rather than steering on it. */
    if (fabsf(ax) + fabsf(ay) + fabsf(az) <= IMU_NOISE_FLOOR_G) {
        return ESP_ERR_TIMEOUT;
    }
    *out_lateral = ax;
    return ESP_OK;
}

bool fly_imu_ready(void)
{
    return s_ready;
}
