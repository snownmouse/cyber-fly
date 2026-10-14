// SPDX-License-Identifier: Apache-2.0
//
// Task C - board-side brain downlink receiver. See fly_link.h for the transport
// rationale. The first half (core) is pure C and host-testable; the ESP-IDF UDP
// task is compiled only when FLY_LINK_ESP_UDP=1.
#include "fly_link.h"

#include <string.h>

#include "fly_game.h"   // fly_state_decode(), fly_crc32_iso_hdlc()

// ---------------------------------------------------------------------------
// Core (pure C)
// ---------------------------------------------------------------------------

void fly_link_reset(fly_link_t *link, uint32_t now_ms)
{
    if (!link) return;
    memset(link, 0, sizeof(*link));
    link->last_tx_ms = now_ms;
    link->last_rx_ms = 0;
}

bool fly_link_feed(fly_link_t *link, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    if (!link || !buf) return false;
    if (len < FLY_LINK_MPRC_LEN) { link->rx_short++; return false; }

    // Reuse the renderer's decoder so there is exactly ONE definition of what a
    // valid MPRC frame is (magic + version + type + length + CRC-32).
    fly_state_t probe;
    if (fly_state_decode(buf, FLY_LINK_MPRC_LEN, &probe) != 0) {
        link->rx_rejects++;
        return false;
    }

    if (link->have_frame) {
        const uint32_t prev = link->last_seq;
        const uint32_t next = ((buf[8]) | ((uint32_t)buf[9] << 8) |
                               ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24));
        // Sequence is monotonic modulo 2^32; a forward jump means loss upstream.
        if (next != prev + 1u && (int32_t)(next - prev) > 0) link->seq_gaps++;
    }
    link->last_seq = ((uint32_t)buf[8]) | ((uint32_t)buf[9] << 8) |
                     ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    memcpy(link->slot, buf, FLY_LINK_MPRC_LEN);
    link->have_frame = true;
    link->rx_frames++;
    link->last_rx_ms = now_ms;
    // RTT is filled in by the UDP half below: only it knows when we sent the
    // matching feature frame (the cloud echoes our send_ts_us back to us).
    return true;
}

bool fly_link_take(fly_link_t *link, uint8_t out[FLY_LINK_MPRC_LEN])
{
    if (!link || !out || !link->have_frame) return false;
    memcpy(out, link->slot, FLY_LINK_MPRC_LEN);
    return true;
}

bool fly_link_online(const fly_link_t *link, uint32_t now_ms)
{
    if (!link || !link->have_frame) return false;
    return (uint32_t)(now_ms - link->last_rx_ms) < FLY_LINK_TIMEOUT_MS;
}

uint8_t fly_link_quality(const fly_link_t *link, uint32_t now_ms)
{
    if (!link || !link->have_frame) return 0;
    const uint32_t since = now_ms - link->last_rx_ms;
    if (since >= FLY_LINK_TIMEOUT_MS) return 0;

    // Start from the age of the newest frame: at the 50 ms control period the
    // margin is 0.99, and it decays linearly to 0 at the timeout.
    uint32_t q = 255u - (since * 255u) / FLY_LINK_TIMEOUT_MS;

    // Penalise what we know about the stream, gently and cumulatively.
    const uint32_t loss = link->seq_gaps * 6u;
    const uint32_t bad = link->rx_rejects * 3u;
    if (loss > 200u) q = 0; else q = (q > loss) ? (q - loss) : 0;
    if (bad > 200u) q = 0; else q = (q > bad) ? (q - bad) : 0;
    return (uint8_t)q;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_f32(uint8_t *p, float f)
{
    uint32_t bits; memcpy(&bits, &f, 4); put_u32(p, bits);
}

void fly_link_build_mprb(uint8_t out[FLY_LINK_MPRB_LEN], uint32_t seq,
                         uint32_t send_ts_us, const fly_mic_features_t *mic,
                         float accel_x)
{
    float rms_dbfs = -60.0f;
    float low_dbfs = rms_dbfs;
    float mid_dbfs = rms_dbfs;
    float high_dbfs = rms_dbfs;
    if (mic && mic->valid) {
        rms_dbfs = mic->rms_dbfs;
        low_dbfs = mic->band_low_db;
        mid_dbfs = mic->band_mid_db;
        high_dbfs = mic->band_high_db;
    }
    memset(out, 0, FLY_LINK_MPRB_LEN);
    put_u32(out + 0, FLY_LINK_MPRB_MAGIC);
    out[4] = 1;                       // version
    out[5] = 2;                       // type = feature
    out[6] = (uint8_t)FLY_LINK_MPRB_LEN;        // payload_len (low byte)
    out[7] = (uint8_t)(FLY_LINK_MPRB_LEN >> 8);
    put_u32(out + 8, seq);
    put_u32(out + 12, send_ts_us);
    put_u32(out + 16, 0);             // echo_ts_us
    out[20] = 0x80; out[21] = 0x3e;   // sample_rate = 16000 (LE u16)
    out[22] = 1;                      // channels
    out[23] = 0x03;                   // flags: AUDIO_VALID | IMU_VALID
    put_f32(out + 24, rms_dbfs);
    put_f32(out + 28, low_dbfs);
    put_f32(out + 32, mid_dbfs);
    put_f32(out + 36, high_dbfs);
    put_f32(out + 40, accel_x);
    put_u32(out + 64, fly_crc32_iso_hdlc(out, 64));
}

// ---------------------------------------------------------------------------
// ESP-IDF UDP task
// ---------------------------------------------------------------------------
#if FLY_LINK_ESP_UDP

#include <errno.h>
#include <lwip/sockets.h>
#include <lwip/ip_addr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "fly_mic.h"
#include "fly_wifi.h"

/*
 * Fallbacks for every Kconfig symbol. These must come BEFORE any
 * `#if CONFIG_...` test that depends on them, otherwise the test sees an
 * undefined macro (which evaluates to 0) and silently compiles out the code
 * that the fallback was supposed to enable. Getting this order wrong is what
 * made fly_imu_read() an implicit declaration.
 */
#ifndef CONFIG_FLY_WIFI_SSID
#define CONFIG_FLY_WIFI_SSID "your-ssid"
#endif
#ifndef CONFIG_FLY_WIFI_PASSWORD
#define CONFIG_FLY_WIFI_PASSWORD "your-wifi-password"
#endif
#ifndef CONFIG_FLY_WIFI_TIMEOUT_SEC
#define CONFIG_FLY_WIFI_TIMEOUT_SEC 20
#endif
#ifndef CONFIG_FLY_WIFI_RETRY_SEC
#define CONFIG_FLY_WIFI_RETRY_SEC 10
#endif
#ifndef CONFIG_FLY_MIC_GAIN_DB
#define CONFIG_FLY_MIC_GAIN_DB 30
#endif
#ifndef CONFIG_FLY_IMU_ENABLE
#define CONFIG_FLY_IMU_ENABLE 1
#endif

#if CONFIG_FLY_IMU_ENABLE
#include "fly_imu.h"
#endif

#define FLY_LINK_TASK_STACK 4096
#define FLY_LINK_TASK_PRIO  5
#define FLY_LINK_POLL_MS    50u      // one MPRB per control frame (20 Hz)

static const char *TAG = "fly_link";
static fly_link_t *s_link;
static int s_sock = -1;
static volatile bool s_running;
static struct sockaddr_in s_dst;
static uint32_t s_seq;
static esp_codec_dev_handle_t s_mic;
static bool s_imu;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void link_task(void *arg)
{
    (void)arg;
    uint8_t mprb[FLY_LINK_MPRB_LEN];
    while (s_running) {
        // --- upstream: ask the brain for a decision -------------------------
        // One 50 ms audio window per control frame, so the feature frame and the
        // brain step advance in lockstep. A failed read leaves mic.valid false
        // and the frame carries a silence floor instead.
        fly_mic_features_t feats;
        memset(&feats, 0, sizeof(feats));
        if (s_mic) {
            if (fly_mic_sample(s_mic, &feats) != ESP_OK) {
                feats.valid = false;
            }
        }
        s_seq++;
        /*
         * Steering comes from the board, not the cloud: the v4 DNa neurons have
         * no named sensory input, so accel[0] goes straight to the LEFT/RIGHT
         * flag with no neural step (design doc 0.5). A read failure sends 0,
         * which reads as "no lateral acceleration" - never as a fabricated turn.
         */
        float accel_x = 0.0f;
        if (s_imu) {
            (void)fly_imu_read(&accel_x);
        }
        fly_link_build_mprb(mprb, s_seq, (uint32_t)esp_timer_get_time(), &feats, accel_x);
        s_link->tx_frames++;
        s_link->last_tx_ms = now_ms();
        if (sendto(s_sock, mprb, sizeof(mprb), 0, (struct sockaddr *)&s_dst,
                   sizeof(s_dst)) < 0) {
            ESP_LOGW(TAG, "sendto 失败: errno %d", errno);
        }

        /*
         * Report the levels the cloud actually receives, slowly.
         *
         * This exists because the obvious symptom - "the fly never stops
         * escaping" - has no single cause: it is either the microphone gain, or
         * the orchestrator's GATE_DB/GATE_SPAN window, and neither can be tuned
         * from the downlink frames alone. Printing rms_dbfs here is what makes
         * the choice measurable instead of a matter of opinion.
         *
         * 2 s cadence, not every frame: at 20 Hz this would flood the log ring
         * and evict the boot messages that explain a failure.
         */
        if (s_seq % 40u == 0u) {
            ESP_LOGI(TAG,
                     "上行电平 rms=%.1f dBFS  低=%.1f  中=%.1f  高=%.1f  峰值=%.1f（增益 %d dB）"
                     "  侧向=%.3f g",
                     (double)feats.rms_dbfs, (double)feats.band_low_db,
                     (double)feats.band_mid_db, (double)feats.band_high_db,
                     (double)feats.peak_dbfs, CONFIG_FLY_MIC_GAIN_DB,
                     (double)accel_x);
        }

        // --- drain whatever came back, then wait for the next tick ----------
        // Non-blocking so a downlink burst never delays the next MPRB.
        for (int i = 0; i < 8; ++i) {
            uint8_t buf[64];
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            const int n = recvfrom(s_sock, buf, sizeof(buf), MSG_DONTWAIT,
                                   (struct sockaddr *)&from, &flen);
            if (n <= 0) break;
            if (fly_link_feed(s_link, buf, (size_t)n, now_ms())) {
                const uint32_t sent_us = (uint32_t)buf[12] |
                                         ((uint32_t)buf[13] << 8) |
                                         ((uint32_t)buf[14] << 16) |
                                         ((uint32_t)buf[15] << 24);
                const uint32_t rtt_us = (uint32_t)esp_timer_get_time() - sent_us;
                s_link->rtt_ms = rtt_us / 1000u;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(FLY_LINK_POLL_MS));
    }
    vTaskDelete(NULL);
}

esp_err_t fly_link_udp_start(fly_link_t *link, const char *host, uint16_t port)
{
    if (!link || !host) return -1;
    if (s_running) return 0;

    // The board sits behind a phone-hotspot NAT, so there is no network until
    // the station is associated. Connect BEFORE opening the socket: otherwise
    // every sendto() fails with ENETUNREACH and the link looks "up" in the log
    // while nothing is on the wire. flywifi_connect() retries forever, so a
    // hotspot that is switched on after boot needs no intervention.
    fly_wifi_info_t winfo;
    const esp_err_t wifi_err = flywifi_connect(CONFIG_FLY_WIFI_SSID,
                                               CONFIG_FLY_WIFI_PASSWORD,
                                               CONFIG_FLY_WIFI_TIMEOUT_SEC,
                                               CONFIG_FLY_WIFI_RETRY_SEC, &winfo);
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 失败: %s", esp_err_to_name(wifi_err));
        return -1;
    }
    ESP_LOGI(TAG, "Wi-Fi %s RSSI %d dBm ch%u", winfo.ip, winfo.rssi, winfo.channel);

    // Open the microphone. If this fails we still run the link: the frames go
    // out with a silence floor, which keeps the cloud answering and keeps the
    // link statistics honest instead of pretending there is no cloud.
    if (fly_mic_open(&s_mic, CONFIG_FLY_MIC_GAIN_DB) != ESP_OK) {
        ESP_LOGW(TAG, "麦克风打开失败，上行将按静音发送");
        s_mic = NULL;
    }

#if CONFIG_FLY_IMU_ENABLE
    /*
     * IMU last: it is the only stage that can fail harmlessly. A board with a
     * dead BMI270 still animates, still gets a brain readout, and simply never
     * steers - which is a much better failure mode than refusing to start.
     */
    s_imu = (fly_imu_open(120.0f) == ESP_OK);
    if (!s_imu) {
        ESP_LOGW(TAG, "IMU 不可用，方向将保持 0（不影响逃逸）");
    }
#endif

    memset(&s_dst, 0, sizeof(s_dst));
    s_dst.sin_family = AF_INET;
    s_dst.sin_port = htons(port ? port : FLY_LINK_DEFAULT_PORT);
    // inet_pton(), not lwIP's ipaddr_aton(): the latter is a macro over
    // ip4addr_aton() and takes an ip_addr_t*, which is a UNION wrapper
    // (u_addr.ip4.addr) - not the plain struct in_addr that sin_addr wants.
    // inet_pton() writes network byte order straight into sin_addr, which is
    // exactly the layout sendto() expects. Returns 1 on success, 0 on a
    // malformed address, -1 on a bad family.
    if (inet_pton(AF_INET, host, &s_dst.sin_addr) != 1) {
        ESP_LOGE(TAG, "云端地址不是点分十进制: %s", host);
        return -1;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) { ESP_LOGE(TAG, "socket 失败: errno %d", errno); return -1; }

    s_link = link;
    s_seq = 0;
    s_running = true;
    if (xTaskCreate(link_task, "fly_link", FLY_LINK_TASK_STACK, NULL,
                    FLY_LINK_TASK_PRIO, NULL) != pdPASS) {
        s_running = false;
        close(s_sock); s_sock = -1;
        ESP_LOGE(TAG, "link 任务创建失败");
        return -1;
    }
    ESP_LOGI(TAG, "下行接收已启动 -> %s:%u", host, (unsigned)port);
    return 0;
}

bool fly_link_udp_running(void) { return s_running; }

void fly_link_udp_stop(void)
{
    if (!s_running) return;
    s_running = false;
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
}

#endif  // FLY_LINK_ESP_UDP
