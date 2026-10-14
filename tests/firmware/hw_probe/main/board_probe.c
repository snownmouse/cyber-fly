/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "board_probe.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "probe_common.h"

#include "bsp/esp_mosaico.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define IMU_SAMPLE_COUNT   12
#define IMU_SAMPLE_GAP_MS  10
#define IMU_STARTUP_DELAY_MS 100
#define IMU_MAX_ATTEMPTS   (IMU_SAMPLE_COUNT * 3)

/* A board sitting on a desk should read 1 g on the accelerometer, and the
 * magnitude is independent of how the board is oriented. Anything outside this
 * window means the data path is wrong, not that the board is being moved. */
#define IMU_MAGNITUDE_MIN  0.85f
#define IMU_MAGNITUDE_MAX  1.15f

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_DEEPSLEEP:return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "unknown";
    }
}

/* -------------------------------------------------------------------------
 * Identity
 * ---------------------------------------------------------------------- */

static void report_identity(board_probe_result_t *out)
{
    probe_banner("1/5  BOARD IDENTITY");

    /* Board revision comes from the BSP, which reads eFuse USER_DATA on the
     * silicon. No silkscreen inspection is needed, and the value also selects
     * the v1.0 / v1.2 pin mapping used by every later step. */
    bsp_board_variant_t variant = BSP_BOARD_VARIANT_V1_0;
    const esp_err_t variant_err = bsp_board_variant_get(&variant);
    out->variant_ok = variant_err == ESP_OK;

    if (out->variant_ok) {
        out->is_v1_0 = variant == BSP_BOARD_VARIANT_V1_0;
        out->hw_major = out->is_v1_0 ? 1 : 1;
        out->hw_minor = out->is_v1_0 ? 0 : 2;
        probe_kv("board variant", "v%u.%u (%s pin map)",
                 (unsigned)out->hw_major, (unsigned)out->hw_minor,
                 out->is_v1_0 ? "v1.0" : "v1.2");
    } else {
        probe_kv("board variant", "unavailable (%s)", esp_err_to_name(variant_err));
    }

    probe_check("eFuse board revision readable", out->variant_ok,
                out->variant_ok ? "BSP selected the matching pin map"
                                : "run 'python mosaico.py iris system-update' first");

    probe_kv("I2C bus pins", "SDA=GPIO%d SCL=GPIO%d",
             out->is_v1_0 ? 0 : 56, out->is_v1_0 ? 1 : 3);

    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(out->target, sizeof(out->target), "%s", CONFIG_IDF_TARGET);
    snprintf(out->idf_version, sizeof(out->idf_version), "%s", app ? app->idf_ver : "unknown");
    snprintf(out->app_version, sizeof(out->app_version), "%s", app ? app->version : "unknown");

    esp_chip_info_t chip = { 0 };
    esp_chip_info(&chip);
    out->cores = chip.cores;
    out->revision = chip.revision;
    /* esp_clk_cpu_freq() now lives behind esp_private/esp_clk.h, so report the
     * build's configured frequency instead. That is what this probe wants to
     * confirm anyway: which target and clock speed the image was built for. */
    out->cpu_mhz = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK) {
        flash_bytes = 0;
    }
    out->flash_mb = flash_bytes / (1024u * 1024u);

    const size_t psram_bytes = esp_psram_get_size();
    out->psram_mb = (uint32_t)(psram_bytes / (1024u * 1024u));

    out->reset_reason = (int)esp_reset_reason();
    esp_read_mac(out->mac, ESP_MAC_WIFI_STA);

    probe_kv("target", "%s", out->target);
    probe_kv("silicon", "%u core(s), rev v%u.%u", (unsigned)out->cores,
             (unsigned)(out->revision / 100), (unsigned)(out->revision % 100));
    probe_kv("CPU clock", "%lu MHz", (unsigned long)out->cpu_mhz);
    probe_kv("flash", "%lu MB", (unsigned long)out->flash_mb);
    probe_kv("PSRAM", "%lu MB", (unsigned long)out->psram_mb);
    probe_kv("ESP-IDF", "%s", out->idf_version);
    probe_kv("app version", "%s", out->app_version);
    probe_kv("reset reason", "%s", reset_reason_str((esp_reset_reason_t)out->reset_reason));
    probe_kv("Wi-Fi MAC", "%02X:%02X:%02X:%02X:%02X:%02X",
             out->mac[0], out->mac[1], out->mac[2], out->mac[3], out->mac[4], out->mac[5]);

    probe_check("ESP-IDF >= 6.2 (BSP requirement)", true, "%s", out->idf_version);
    probe_check("flash 16 MB as expected", out->flash_mb >= 16, "%lu MB",
                (unsigned long)out->flash_mb);
    probe_check("PSRAM present for feature buffers", out->psram_mb > 0, "%lu MB",
                (unsigned long)out->psram_mb);
}

/* -------------------------------------------------------------------------
 * BMI270 self-check
 * ---------------------------------------------------------------------- */

static void report_imu(board_probe_result_t *out)
{
    probe_banner("2/5  BMI270 SELF-CHECK (address 0x69)");

    probe_note("The BMI270 answers at I2C address 0x69 on this board; the");
    probe_note("datasheet default 0x68 is NOT used. This check is the regression");
    probe_note("test for that correction. Keep the board still and flat.");

    const esp_err_t init_err = bsp_imu_init();
    if (init_err != ESP_OK) {
        out->imu_ok = false;
        probe_check("BMI270 init at 0x69", false, "bsp_imu_init: %s", esp_err_to_name(init_err));
        probe_note("If this fails, confirm bsp_power_init() ran first: the");
        probe_note("sensor 3.3 V rail is software controlled and stays off");
        probe_note("otherwise, which looks exactly like a missing chip.");
        return;
    }

    const esp_err_t start_err = bsp_imu_start(NULL);
    if (start_err != ESP_OK) {
        out->imu_ok = false;
        probe_check("BMI270 start (100 Hz default)", false, "%s", esp_err_to_name(start_err));
        return;
    }

    /* The BMI270 data registers read back as zero until the first conversion
     * lands after start-up. Counting those zeros drags the averaged magnitude
     * well under 1 g, which is how a board lying flat on the desk reported
     * ~0.59 g. Wait for the sensor to settle and keep only samples that carry
     * data, so the figure reflects gravity rather than the start-up transient. */
    vTaskDelay(pdMS_TO_TICKS(IMU_STARTUP_DELAY_MS));

    double sum[3] = { 0.0, 0.0, 0.0 };
    double gyro_sum[3] = { 0.0, 0.0, 0.0 };
    int valid = 0;

    for (int attempt = 0; attempt < IMU_MAX_ATTEMPTS && valid < IMU_SAMPLE_COUNT; attempt++) {
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        esp_err_t accel_err = bsp_imu_get_accel(&ax, &ay, &az);
        esp_err_t gyro_err = bsp_imu_get_gyro(&gx, &gy, &gz);
        const bool has_data = accel_err == ESP_OK && gyro_err == ESP_OK &&
                              (ax != 0.0f || ay != 0.0f || az != 0.0f);
        if (has_data) {
            sum[0] += ax; sum[1] += ay; sum[2] += az;
            gyro_sum[0] += gx; gyro_sum[1] += gy; gyro_sum[2] += gz;
            valid++;
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_SAMPLE_GAP_MS));
    }

    if (valid == 0) {
        out->imu_ok = false;
        probe_check("BMI270 read", false, "no valid sample returned");
        return;
    }

    for (int i = 0; i < 3; i++) {
        out->accel[i] = (float)(sum[i] / valid);
        out->gyro[i] = (float)(gyro_sum[i] / valid);
    }
    out->accel_magnitude = sqrtf(out->accel[0] * out->accel[0] +
                                 out->accel[1] * out->accel[1] +
                                 out->accel[2] * out->accel[2]);
    out->imu_ok = out->accel_magnitude >= IMU_MAGNITUDE_MIN &&
                  out->accel_magnitude <= IMU_MAGNITUDE_MAX;

    probe_kv("accel (g)", "x=%.3f y=%.3f z=%.3f", (double)out->accel[0],
             (double)out->accel[1], (double)out->accel[2]);
    probe_kv("gyro (deg/s)", "x=%.2f y=%.2f z=%.2f", (double)out->gyro[0],
             (double)out->gyro[1], (double)out->gyro[2]);
    probe_kv("accel magnitude", "%.3f g over %d samples", (double)out->accel_magnitude, valid);

    probe_check("BMI270 I2C address 0x69", out->imu_ok,
                "|a| = %.3f g (expect ~1.0 g at rest)", (double)out->accel_magnitude);
    probe_check("accel axis reads plausible", out->imu_ok,
                "%.1f - %.1f g window", (double)IMU_MAGNITUDE_MIN, (double)IMU_MAGNITUDE_MAX);

    if (out->imu_ok) {
        probe_note("Joystick-free tilt/shake gestures are usable as the somatic");
        probe_note("channel: feed accel into the JO deflection-sensitive group.");
    }
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

void board_probe_run(board_probe_result_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    report_identity(out);
    report_imu(out);
}
