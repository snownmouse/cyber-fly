/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief ESP-Mosaico hardware acceptance probe (task A).
 *
 * Answers the three questions the hardware interface checklist left open:
 *   1. which board revision this unit actually is (v1.0 vs v1.2)
 *   2. what the microphone noise floor looks like, and which gain to use
 *   3. what the real UDP loss and jitter are on 2.4 GHz
 *
 * Plus a BMI270 self-check that doubles as the regression test for the
 * address correction: this board answers at 0x69, not 0x68.
 *
 * Ordering matters. bsp_power_init() runs before anything else because the
 * codec and sensor 3.3 V rails are software controlled; without it the I2C bus
 * scans clean and looks exactly like dead hardware.
 */

#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iris_ota_support.h"
#include "nvs_flash.h"

#include "board_probe.h"
#include "mic_probe.h"
#include "probe_common.h"
#include "udp_probe.h"
#include "wifi_sta.h"

static const char *TAG = "hw_probe";

/* Verdict thresholds, gathered here so they are easy to tune after the first
 * run on real hardware. The 2% loss baseline comes from the reference
 * flybrain-robot-bridge figure at 2.4 GHz. */
#define PROBE_LOSS_BASELINE_PCT   2.0f
#define PROBE_JITTER_LIMIT_MS     30.0
#define PROBE_RSSI_GOOD_DBM      (-70)
#define PROBE_RSSI_USABLE_DBM    (-80)

typedef struct {
    board_probe_result_t board;
    mic_analysis_t       mic;
    int                  gain_db;

    bool                 wifi_ok;
    wifi_sta_result_t    wifi;

    bool                 burst_done;
    udp_probe_result_t   burst;

    bool                 stream_done;
    udp_probe_result_t   stream;
    uint32_t             stream_frames_with_audio;
} probe_summary_t;

static probe_summary_t s_summary;
static esp_codec_dev_handle_t s_mic;
static int s_sock = -1;
static struct sockaddr_in s_dst;

/* -------------------------------------------------------------------------
 * Step 0: power rails
 * ---------------------------------------------------------------------- */

static void step_power(void)
{
    probe_banner("0/5  BOARD POWER");

    /* Deliberately the first BSP call in the image. Everything downstream
     * depends on the 3.3 V rails this turns on. */
    const esp_err_t err = bsp_power_init();

    probe_check("bsp_power_init() first call", err == ESP_OK, "%s", esp_err_to_name(err));

    if (err == ESP_OK) {
        probe_note("VCC_3V3 and (on v1.0) CODEC_3V3 rails are now configured.");
        return;
    }

    probe_note("Without this the codec and IMU 3.3 V rails stay off and the I2C");
    probe_note("bus reads empty, which looks identical to missing hardware.");
    probe_note("The usual cause is an unprogrammed or unsupported eFuse revision.");
    probe_note("Install or repair the device image first:");
    probe_note("  python mosaico.py recover");
    probe_note("  python mosaico.py iris system-update --project tests/firmware/hw_probe");
}

/* -------------------------------------------------------------------------
 * UDP reporting
 * ---------------------------------------------------------------------- */

static void report_udp_result(const char *label, const udp_probe_result_t *result,
                              float loss_limit_pct)
{
    probe_kv("packets sent", "%lu", (unsigned long)result->sent);
    probe_kv("ACKs received", "%lu (unique %lu)",
             (unsigned long)result->acked, (unsigned long)result->unique_acked);
    probe_kv("loss", "%.2f%% (%lu packets)", (double)result->loss_pct, (unsigned long)result->lost);

    if (result->any_ack) {
        probe_kv("RTT min/avg/max", "%.1f / %.1f / %.1f ms",
                 result->rtt_min_ms, result->rtt_avg_ms, result->rtt_max_ms);
        probe_kv("RTT jitter", "%.1f ms stddev", result->rtt_jitter_ms);
    } else {
        probe_kv("RTT", "no ACK received");
    }
    probe_kv("send cadence", "avg %.1f ms, worst %.1f ms",
             result->send_gap_avg_ms, result->send_gap_max_ms);

    if (result->crc_errors > 0 || result->foreign_packets > 0) {
        probe_kv("rejected replies", "%lu bad CRC, %lu unexpected",
                 (unsigned long)result->crc_errors, (unsigned long)result->foreign_packets);
    }

    if (!result->any_ack) {
        probe_check("server reachable", false,
                    "no ACK at all: is udp_probe_server.py running?");
        probe_note("Zero loss with zero ACKs would mean the ACK path is missing,");
        probe_note("not that the link is perfect. Check the server address and port.");
        return;
    }

    probe_check("server reachable", true, "ACK path confirmed");
    probe_check("loss within baseline", result->loss_pct <= loss_limit_pct,
                "%.2f%% (baseline %.2f%%)", (double)result->loss_pct, (double)loss_limit_pct);
    probe_check("jitter within limit", result->rtt_jitter_ms <= PROBE_JITTER_LIMIT_MS,
                "%.1f ms (limit %.0f ms)", result->rtt_jitter_ms, (double)PROBE_JITTER_LIMIT_MS);

    (void)label;
}

/* -------------------------------------------------------------------------
 * Step 4: Wi-Fi, step 5: UDP
 * ---------------------------------------------------------------------- */

static void step_wifi(void)
{
    probe_banner("4/5  WI-FI LINK");

    const char *ssid = CONFIG_HW_PROBE_WIFI_SSID;
    if (ssid[0] == '\0') {
        probe_check("Wi-Fi SSID configured", false, "empty SSID");
        probe_note("Set the network in menuconfig: HW Probe -> Wi-Fi station.");
        return;
    }

    probe_note("ESP32-S31 has a 2.4 GHz-only radio, so a 5 GHz-only SSID is");
    probe_note("invisible here no matter how strong it is.");

    /* Diagnostic: dump what the radio can actually hear before trying to join.
     * A "reason 201" on its own cannot distinguish "hotspot off" from "SSID
     * renamed" from "5 GHz-only"; this list can. */
    ESP_LOGI(TAG, "scanning for visible 2.4 GHz APs before joining \"%s\"", ssid);
    hwprobe_wifi_scan();

    const esp_err_t err = hwprobe_wifi_connect(ssid, CONFIG_HW_PROBE_WIFI_PASSWORD,
                                           CONFIG_HW_PROBE_WIFI_TIMEOUT_SEC,
                                           CONFIG_HW_PROBE_WIFI_MAX_RETRY,
                                           &s_summary.wifi);
    if (err != ESP_OK) {
        probe_check("Wi-Fi connected", false, "%s", esp_err_to_name(err));
        return;
    }

    s_summary.wifi_ok = true;

    probe_kv("SSID", "%s", ssid);
    probe_kv("IP address", "%s", s_summary.wifi.ip);
    probe_kv("netmask", "%s", s_summary.wifi.netmask);
    probe_kv("gateway", "%s", s_summary.wifi.gateway);
    probe_kv("channel", "%u", (unsigned)s_summary.wifi.channel);
    probe_kv("RSSI", "%d dBm", (int)s_summary.wifi.rssi);
    probe_kv("DHCP time", "%lu ms", (unsigned long)s_summary.wifi.connect_ms);

    const bool rssi_good = s_summary.wifi.rssi >= PROBE_RSSI_GOOD_DBM;
    const bool rssi_usable = s_summary.wifi.rssi >= PROBE_RSSI_USABLE_DBM;
    probe_check("signal strength", rssi_usable,
                rssi_good ? "%d dBm (good)" : (rssi_usable ? "%d dBm (usable)" : "%d dBm (weak)"),
                (int)s_summary.wifi.rssi);

    if (!rssi_good) {
        probe_note("Weak signal is the single most common cause of the loss and");
        probe_note("jitter numbers being worse than the theory predicts.");
    }
}

static void step_udp(void)
{
    probe_banner("5/5  UDP LINK QUALITY");

    if (!s_summary.wifi_ok) {
        probe_check("UDP measurement", false, "skipped, no Wi-Fi link");
        return;
    }

    const esp_err_t open_err = udp_probe_open(CONFIG_HW_PROBE_SERVER_IP,
                                              CONFIG_HW_PROBE_SERVER_PORT,
                                              &s_sock, &s_dst);
    if (open_err != ESP_OK) {
        probe_check("UDP socket", false, "%s", esp_err_to_name(open_err));
        return;
    }

    probe_note("Start the receiver on the server before this step:");
    probe_note("  python3 tools/udp_probe_server.py --port %d", CONFIG_HW_PROBE_SERVER_PORT);
    probe_note("Every packet carries a sequence number and a board timestamp; the");
    probe_note("RTT comes from the timestamp the server echoes back, so write the");
    probe_note("send rate and the measured jitter as separate facts.");

    /* --- 5a: burst ------------------------------------------------------- */
    probe_banner("5a/5  BURST (limit test)");
    probe_note("%lu packets, %lu us apart",
               (unsigned long)CONFIG_HW_PROBE_UDP_BURST_COUNT,
               (unsigned long)CONFIG_HW_PROBE_UDP_BURST_INTERVAL_US);

    if (udp_probe_burst(s_sock, &s_dst,
                        CONFIG_HW_PROBE_UDP_BURST_COUNT,
                        CONFIG_HW_PROBE_UDP_BURST_INTERVAL_US,
                        CONFIG_HW_PROBE_UDP_ACK_TIMEOUT_MS,
                        &s_summary.burst) == ESP_OK) {
        s_summary.burst_done = true;
        report_udp_result("burst", &s_summary.burst, PROBE_LOSS_BASELINE_PCT);
    } else {
        probe_check("burst test", false, "measurement failed");
    }

    /* --- 5b: steady stream ----------------------------------------------- */
    probe_banner("5b/5  STEADY STREAM (the shape the closed loop will use)");
    probe_note("%lu s at %lu Hz, each frame a 50 ms audio window plus one IMU read.",
               (unsigned long)CONFIG_HW_PROBE_UDP_STREAM_SEC,
               (unsigned long)CONFIG_HW_PROBE_UDP_STREAM_HZ);
    probe_note("Frames carry features only: three band energies, RMS and IMU.");
    probe_note("Raw PCM is never sent.");

    if (udp_probe_stream(s_sock, &s_dst, s_mic,
                         CONFIG_HW_PROBE_UDP_STREAM_SEC,
                         CONFIG_HW_PROBE_UDP_STREAM_HZ,
                         CONFIG_HW_PROBE_UDP_ACK_TIMEOUT_MS,
                         &s_summary.stream,
                         &s_summary.stream_frames_with_audio) == ESP_OK) {
        s_summary.stream_done = true;
        report_udp_result("stream", &s_summary.stream, PROBE_LOSS_BASELINE_PCT);

        /* The cadence line matters here: capturing 50 ms of audio and reading
         * the IMU may not fit inside a 50 ms budget, and the worst-case gap
         * shows exactly how much the frame rate drifts. */
        const double target_ms = 1000.0 / (double)CONFIG_HW_PROBE_UDP_STREAM_HZ;
        const double overrun = s_summary.stream.send_gap_max_ms - target_ms;
        probe_check("frame budget holds", overrun <= 5.0,
                    "target %.0f ms, worst %.1f ms", target_ms, s_summary.stream.send_gap_max_ms);
        if (overrun > 5.0) {
            probe_note("The frame cost exceeds the target period, so the effective");
            probe_note("rate is lower than requested. Either accept the slower");
            probe_note("cadence or shorten the audio window below 50 ms.");
        }

        probe_kv("frames with audio", "%lu / %lu",
                 (unsigned long)s_summary.stream_frames_with_audio,
                 (unsigned long)s_summary.stream.sent);
    } else {
        probe_check("steady stream", false, "measurement failed");
    }
}

/* -------------------------------------------------------------------------
 * Final report
 * ---------------------------------------------------------------------- */

static void print_final_report(void)
{
    probe_banner("SUMMARY");

    probe_check("board revision readable", s_summary.board.variant_ok,
                s_summary.board.variant_ok
                    ? (s_summary.board.is_v1_0 ? "v1.0 pin map" : "v1.2 pin map")
                    : "eFuse not programmed");
    probe_check("BMI270 answers at 0x69", s_summary.board.imu_ok,
                "|a| = %.3f g", (double)s_summary.board.accel_magnitude);
    probe_check("microphone opens 16 kHz mono", s_summary.mic.valid,
                s_summary.mic.valid ? "ES8311 configured by the BSP" : "open failed");
    probe_check("microphone noise floor", s_summary.mic.valid && s_summary.mic.ac_rms_dbfs <= -45.0f,
                "%.1f dBFS", (double)s_summary.mic.ac_rms_dbfs);
    if (s_summary.wifi_ok) {
        probe_check("Wi-Fi link", true, "%s ch%u %d dBm", s_summary.wifi.ip,
                    (unsigned)s_summary.wifi.channel, (int)s_summary.wifi.rssi);
    } else {
        probe_check("Wi-Fi link", false, "not connected");
    }
    probe_check("UDP server reachable", s_summary.burst_done && s_summary.burst.any_ack,
                s_summary.burst_done ? "ACK path confirmed" : "not measured");

    if (s_summary.stream_done) {
        probe_check("UDP loss (steady stream)", s_summary.stream.loss_pct <= PROBE_LOSS_BASELINE_PCT,
                    "%.2f%%", (double)s_summary.stream.loss_pct);
    }

    probe_banner("PARAMETERS TO CARRY INTO TASK B/C");
    probe_kv("board revision", "v1.0=%d (pin map decided by the BSP)",
             (int)s_summary.board.is_v1_0);
    probe_kv("mic gain", "%d dB", s_summary.gain_db);
    probe_kv("mic noise floor", "%.1f dBFS", (double)s_summary.mic.ac_rms_dbfs);
    probe_kv("band <200 Hz", "%.1f dBFS", (double)s_summary.mic.band_low_db);
    probe_kv("band 200-500 Hz", "%.1f dBFS", (double)s_summary.mic.band_mid_db);
    probe_kv("band 500-2000 Hz", "%.1f dBFS", (double)s_summary.mic.band_high_db);
    probe_kv("feature frame rate", "%d Hz", CONFIG_HW_PROBE_UDP_STREAM_HZ);

    if (s_summary.stream_done && s_summary.stream.any_ack) {
        probe_kv("link RTT", "min %.1f / avg %.1f / max %.1f ms",
                 s_summary.stream.rtt_min_ms, s_summary.stream.rtt_avg_ms,
                 s_summary.stream.rtt_max_ms);
        probe_kv("link loss", "%.2f%%", (double)s_summary.stream.loss_pct);
        probe_kv("link jitter", "%.1f ms", s_summary.stream.rtt_jitter_ms);

        /* One-way delay is half the round trip; the local 50 ms capture window
         * and the neural step add on top. */
        const double one_way = s_summary.stream.rtt_avg_ms / 2.0;
        probe_kv("estimated one-way", "%.1f ms + 50 ms window + neural step", one_way);
        probe_note("Treat that as the floor for the closed loop, not a target.");
        probe_note("A slow pet that reacts in a few hundred ms reads as a cold-");
        probe_note("blooded animal, which fits the design rather than fighting it.");
    }

    probe_note("Feed these numbers into the task B encoder constants: the band");
    probe_note("energies become JO stimulation baselines, and the measured loss");
    probe_note("decides whether the downlink needs interpolation.");
}

/* -------------------------------------------------------------------------
 * Optional continuous streaming
 * ---------------------------------------------------------------------- */

/* Compiled only when the option is on, so a build without it does not warn
 * about an unused static function. */
#if CONFIG_HW_PROBE_CONTINUOUS_STREAM

static void continuous_stream_loop(void)
{
    probe_banner("CONTINUOUS STREAM (observation mode)");

    /* Never leave this function. A board that returns from app_main() looks
     * dead to ESP-Iris, so an in-flight install is declared unhealthy and the
     * device falls back to Recovery. Instead, keep re-establishing the radio
     * path until the stream can actually run.
     *
     * Retry forever, not a fixed number of times: an access point switched on
     * (or renamed back) five minutes from now must still be picked up without
     * a reflash. Every ~60 s also dump what the radio can hear -- the boot-time
     * scan scrolls out of the retained log within seconds, and a bare
     * "reason 201" cannot say what *was* heard. */
    uint32_t attempt = 0;
    int64_t  last_scan_us = 0;

    while (!s_summary.wifi_ok || s_sock < 0) {
        attempt++;

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_scan_us >= 60 * 1000000) {
            last_scan_us = now_us;
            ESP_LOGW(TAG, "no live radio path (attempt %lu); scanning 2.4 GHz to see what "
                          "is actually audible", (unsigned long)attempt);
            hwprobe_wifi_scan();
        }

        if (!s_summary.wifi_ok) {
            const esp_err_t err = hwprobe_wifi_connect(CONFIG_HW_PROBE_WIFI_SSID,
                                                       CONFIG_HW_PROBE_WIFI_PASSWORD,
                                                       CONFIG_HW_PROBE_WIFI_TIMEOUT_SEC,
                                                       CONFIG_HW_PROBE_WIFI_MAX_RETRY,
                                                       &s_summary.wifi);
            if (err == ESP_OK) {
                s_summary.wifi_ok = true;
                ESP_LOGI(TAG, "Wi-Fi recovered: %s ch%u %d dBm", s_summary.wifi.ip,
                         (unsigned)s_summary.wifi.channel, (int)s_summary.wifi.rssi);
            } else {
                ESP_LOGW(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(err));
            }
        }

        if (s_summary.wifi_ok && s_sock < 0) {
            const esp_err_t open_err = udp_probe_open(CONFIG_HW_PROBE_SERVER_IP,
                                                      CONFIG_HW_PROBE_SERVER_PORT,
                                                      &s_sock, &s_dst);
            if (open_err == ESP_OK) {
                ESP_LOGI(TAG, "UDP socket ready for %s:%u", CONFIG_HW_PROBE_SERVER_IP,
                         (unsigned)CONFIG_HW_PROBE_SERVER_PORT);
            } else {
                ESP_LOGW(TAG, "udp_probe_open failed: %s", esp_err_to_name(open_err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    probe_note("Streaming %d Hz frames in 10 s windows. Watch the server output",
               CONFIG_HW_PROBE_UDP_STREAM_HZ);
    probe_note("for loss creep over a longer period than a single test run.");

    uint32_t round = 0;
    while (true) {
        udp_probe_result_t result;
        uint32_t with_audio = 0;

        if (udp_probe_stream(s_sock, &s_dst, s_mic, 10,
                             CONFIG_HW_PROBE_UDP_STREAM_HZ,
                             CONFIG_HW_PROBE_UDP_ACK_TIMEOUT_MS,
                             &result, &with_audio) != ESP_OK) {
            ESP_LOGW(TAG, "stream round failed, retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        round++;
        ESP_LOGI(TAG, "  round %lu: sent %lu, loss %.2f%%, RTT avg %.1f ms, jitter %.1f ms, audio frames %lu",
                 (unsigned long)round, (unsigned long)result.sent, (double)result.loss_pct,
                 result.rtt_avg_ms, result.rtt_jitter_ms, (unsigned long)with_audio);
    }
}

#endif /* CONFIG_HW_PROBE_CONTINUOUS_STREAM */

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

void app_main(void)
{
    /* Hard ordering requirement: the power rails come up before any other BSP
     * peripheral is touched. */
    step_power();

    /* Unrelated to the BSP, but it must run before the device is used for
     * anything else, and it does not disturb the power sequence above. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* The Wi-Fi driver dumps a whole rate table on every connect attempt (~40
     * lines). That torrent evicts everything interesting -- notably the AP
     * scan -- from the retained log ring within seconds, so quiet it down.
     * The probe's own lines are tagged "hw_probe" and are unaffected. */
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    probe_banner("ESP-Mosaico HW PROBE  (task A)");
    probe_kv("purpose", "board revision, microphone floor, UDP loss/jitter");
    probe_kv("build", "%s", CONFIG_APP_PROJECT_VER);

    /* 1-2: identity and the BMI270 address regression check. */
    board_probe_run(&s_summary.board);

    /* 3: microphone noise floor and gain recommendation. The handle is kept for
     * the UDP feature stream so the codec is opened exactly once. */
    s_summary.gain_db = CONFIG_HW_PROBE_MIC_GAIN_DB;
#if CONFIG_HW_PROBE_MIC_ENABLE
    mic_probe_run(CONFIG_HW_PROBE_MIC_GAIN_DB, &s_mic, &s_summary.mic, &s_summary.gain_db);
#else
    probe_banner("3/5  MICROPHONE NOISE FLOOR");
    probe_note("Disabled by configuration; the UDP stream will send IMU only.");
#endif

    /* 4-5: link quality. */
    step_wifi();
    step_udp();

    print_final_report();

    /* Expose the enter-Recovery RPC before settling into the main loop, so the
     * device stays reachable for the repository's own tooling. */
    iris_ota_support_start();

    probe_banner("PROBE COMPLETE");
    probe_note("Logs above are the deliverable. Re-run after changing the");
    probe_note("antenna position, the enclosure, or the access point channel.");

#if CONFIG_HW_PROBE_CONTINUOUS_STREAM
    continuous_stream_loop();
#else
    /* The report above is a single early burst. A board whose retained log
     * window is short can evict it before anyone reads it, so re-print the
     * same summary on a slow cadence. This is a re-print, not a
     * re-measurement: the numbers stay the ones captured at boot. */
    uint32_t idle_round = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        idle_round++;
        ESP_LOGI(TAG, "idle round %lu, uptime %lu s (report re-prints every 30 s)",
                 (unsigned long)idle_round,
                 (unsigned long)(esp_timer_get_time() / 1000000));

        if ((idle_round % 3u) == 0u) {
            print_final_report();
        }
    }
#endif
}
