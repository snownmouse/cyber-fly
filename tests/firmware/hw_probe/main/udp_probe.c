/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "udp_probe.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "probe_common.h"

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_probe.h"

static const char *TAG = "hw_probe";

/* Receive timeout so a missing server cannot block the probe forever. */
#define UDP_RECV_TIMEOUT_US 100000

/* Collect ACKs every this many sends so the local receive buffer cannot fill up
 * while a burst is saturating the link. */
#define ACK_DRAIN_STRIDE 32

typedef struct {
    uint8_t      *seen;        /* one bit per sequence number, for dedup */
    uint32_t      seen_bits;
    uint32_t      acked;
    uint32_t      unique;
    uint32_t      crc_errors;
    uint32_t      foreign;
    probe_stats_t rtt;         /* milliseconds */
} ack_state_t;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void wait_until_us(int64_t target_us)
{
    /*
     * Spin instead of vTaskDelay(): the tick is 10 ms by default and the burst
     * needs millisecond-accurate spacing. Each burst is a second or so long, so
     * the idle-spin cost does not matter.
     */
    while (esp_timer_get_time() < target_us) {
    }
}

/*
 * esp_timer_get_time() is 64-bit but the wire field is 32-bit. Subtracting in
 * 32-bit space makes the wrap-around correct, and the result stays well inside
 * a plausible round trip.
 */
static double elapsed_us_since(uint32_t stamp_us, int64_t now_us)
{
    const uint32_t delta = (uint32_t)now_us - stamp_us;
    return (double)delta;
}

static esp_err_t ack_state_init(ack_state_t *state, uint32_t expected)
{
    memset(state, 0, sizeof(*state));
    state->seen_bits = expected > 0 ? expected : 1;

    const size_t bytes = (state->seen_bits + 7u) / 8u;
    state->seen = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return state->seen ? ESP_OK : ESP_ERR_NO_MEM;
}

static void ack_state_free(ack_state_t *state)
{
    if (state->seen) {
        heap_caps_free(state->seen);
        state->seen = NULL;
    }
}

static void ack_state_add(ack_state_t *state, const probe_pkt_t *pkt, int64_t now_us)
{
    state->acked++;

    if (pkt->seq < state->seen_bits) {
        const uint32_t byte = pkt->seq / 8u;
        const uint8_t mask = (uint8_t)(1u << (pkt->seq % 8u));
        if ((state->seen[byte] & mask) == 0) {
            state->seen[byte] |= mask;
            state->unique++;
        }
    }

    const double rtt_ms = elapsed_us_since(pkt->echo_ts_us, now_us) / 1000.0;
    if (rtt_ms >= 0.0 && rtt_ms < 60000.0) {
        probe_stats_add(&state->rtt, rtt_ms);
    }
}

/** Non-blocking: pull every ACK that is already queued. */
static void drain_acks(int sock, ack_state_t *state)
{
    probe_pkt_t pkt;

    for (;;) {
        const ssize_t received = recv(sock, &pkt, sizeof(pkt), MSG_DONTWAIT);
        if (received <= 0) {
            break;
        }
        if ((size_t)received != sizeof(pkt) || !probe_pkt_validate(&pkt)) {
            state->crc_errors++;
            continue;
        }
        if (pkt.type != PROBE_PKT_ACK) {
            state->foreign++;
            continue;
        }
        ack_state_add(state, &pkt, esp_timer_get_time());
    }
}

/** Keep listening for stragglers until the wait window expires. */
static void collect_acks(int sock, ack_state_t *state, int wait_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        drain_acks(sock, state);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    drain_acks(sock, state);
}

static void ack_state_finish(const ack_state_t *state, uint32_t sent,
                             double gap_avg_ms, double gap_max_ms,
                             udp_probe_result_t *out)
{
    memset(out, 0, sizeof(*out));

    out->sent = sent;
    out->acked = state->acked;
    out->unique_acked = state->unique;
    out->lost = sent > state->unique ? sent - state->unique : 0;
    out->loss_pct = sent > 0 ? (float)((double)out->lost * 100.0 / (double)sent) : 0.0f;
    out->crc_errors = state->crc_errors;
    out->foreign_packets = state->foreign;
    out->any_ack = state->acked > 0;
    out->send_gap_avg_ms = gap_avg_ms;
    out->send_gap_max_ms = gap_max_ms;

    if (state->rtt.count > 0) {
        out->rtt_min_ms = state->rtt.min;
        out->rtt_avg_ms = probe_stats_mean(&state->rtt);
        out->rtt_max_ms = state->rtt.max;
        out->rtt_jitter_ms = probe_stats_stddev(&state->rtt);
    }
}

/** Shared send-cadence bookkeeping. */
typedef struct {
    int64_t next_us;
    int64_t prev_us;
    int64_t gap_sum;
    int64_t gap_max;
    uint32_t gaps;
} cadence_t;

static void cadence_reset(cadence_t *cadence)
{
    memset(cadence, 0, sizeof(*cadence));
    cadence->next_us = esp_timer_get_time();
}

static void cadence_record(cadence_t *cadence, uint32_t interval_us)
{
    const int64_t now = esp_timer_get_time();

    if (cadence->prev_us > 0) {
        const int64_t gap = now - cadence->prev_us;
        cadence->gap_sum += gap;
        if (gap > cadence->gap_max) {
            cadence->gap_max = gap;
        }
        cadence->gaps++;
    }
    cadence->prev_us = now;

    if (interval_us > 0) {
        cadence->next_us += interval_us;
        wait_until_us(cadence->next_us);
    }
}

static double cadence_avg_ms(const cadence_t *cadence)
{
    return cadence->gaps > 0 ? (double)cadence->gap_sum / (double)cadence->gaps / 1000.0 : 0.0;
}

static double cadence_max_ms(const cadence_t *cadence)
{
    return (double)cadence->gap_max / 1000.0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

esp_err_t udp_probe_open(const char *server_ip, uint16_t port, int *out_sock,
                         struct sockaddr_in *out_dst)
{
    ESP_RETURN_ON_FALSE(server_ip && out_sock && out_dst, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument");

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(sock >= 0, ESP_FAIL, TAG, "socket() failed: errno %d", errno);

    const struct timeval timeout = { .tv_sec = 0, .tv_usec = UDP_RECV_TIMEOUT_US };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1) {
        close(sock);
        ESP_LOGE(TAG, "invalid server IPv4 address: %s", server_ip);
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * connect() on a UDP socket only records the peer: it sends nothing and
     * therefore cannot confirm the server is listening. The loss figures are
     * what reveal a missing or unreachable receiver.
     */
    if (connect(sock, (const struct sockaddr *)&dst, sizeof(dst)) != 0) {
        const int err = errno;
        close(sock);
        ESP_LOGE(TAG, "connect() failed: errno %d", err);
        return ESP_FAIL;
    }

    *out_sock = sock;
    *out_dst = dst;
    ESP_LOGI(TAG, "UDP socket ready for %s:%u", server_ip, (unsigned)port);
    return ESP_OK;
}

esp_err_t udp_probe_burst(int sock, const struct sockaddr_in *dst, uint32_t count,
                          uint32_t interval_us, int ack_wait_ms, udp_probe_result_t *out)
{
    (void)dst; /* the socket is already connected to the peer */

    ESP_RETURN_ON_FALSE(sock >= 0 && out, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(count > 0, ESP_ERR_INVALID_ARG, TAG, "count must be positive");

    ack_state_t state;
    ESP_RETURN_ON_ERROR(ack_state_init(&state, count), TAG, "ACK state alloc failed");

    cadence_t cadence;
    cadence_reset(&cadence);

    probe_pkt_t pkt;
    uint32_t sent = 0;
    uint32_t send_errors = 0;

    for (uint32_t i = 0; i < count; i++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.type = PROBE_PKT_BURST;
        pkt.seq = i;
        pkt.send_ts_us = (uint32_t)esp_timer_get_time();
        probe_pkt_finalize(&pkt);

        if (send(sock, &pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt)) {
            sent++;
        } else {
            send_errors++;
        }

        if ((i % ACK_DRAIN_STRIDE) == (ACK_DRAIN_STRIDE - 1)) {
            drain_acks(sock, &state);
        }

        cadence_record(&cadence, interval_us);
    }

    collect_acks(sock, &state, ack_wait_ms);
    ack_state_finish(&state, sent, cadence_avg_ms(&cadence), cadence_max_ms(&cadence), out);
    ack_state_free(&state);

    if (send_errors > 0) {
        ESP_LOGW(TAG, "%lu send() calls failed locally", (unsigned long)send_errors);
    }
    return ESP_OK;
}

esp_err_t udp_probe_stream(int sock, const struct sockaddr_in *dst, esp_codec_dev_handle_t mic,
                           uint32_t seconds, uint32_t hz, int ack_wait_ms,
                           udp_probe_result_t *out, uint32_t *out_frames_with_audio)
{
    (void)dst; /* the socket is already connected to the peer */

    ESP_RETURN_ON_FALSE(sock >= 0 && out, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(seconds > 0 && hz > 0, ESP_ERR_INVALID_ARG, TAG,
                        "seconds and hz must be positive");

    const uint32_t total = seconds * hz;
    const uint32_t interval_us = 1000000u / hz;

    ack_state_t state;
    ESP_RETURN_ON_ERROR(ack_state_init(&state, total), TAG, "ACK state alloc failed");

    cadence_t cadence;
    cadence_reset(&cadence);

    uint32_t sent = 0;
    uint32_t frame_errors = 0;
    uint32_t with_audio = 0;
    uint32_t with_imu = 0;

    for (uint32_t i = 0; i < total; i++) {
        probe_pkt_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type = PROBE_PKT_FEATURE;
        pkt.seq = i;
        pkt.sample_rate = 16000;
        pkt.channels = 1;

        /* Audio features: 50 ms window, computed on the board. Raw PCM would be
         * ~32 KB/s and is deliberately not sent. */
        if (mic) {
            mic_analysis_t audio;
            if (mic_probe_sample_features(mic, &audio) == ESP_OK && audio.valid) {
                pkt.rms_dbfs = audio.ac_rms_dbfs;
                pkt.band_low = audio.band_low_db;
                pkt.band_mid = audio.band_mid_db;
                pkt.band_high = audio.band_high_db;
                pkt.flags |= PROBE_FLAG_AUDIO_VALID;
                with_audio++;
            }
        }

        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        if (bsp_imu_get_accel(&ax, &ay, &az) == ESP_OK &&
            bsp_imu_get_gyro(&gx, &gy, &gz) == ESP_OK) {
            pkt.accel[0] = ax;
            pkt.accel[1] = ay;
            pkt.accel[2] = az;
            pkt.gyro[0] = gx;
            pkt.gyro[1] = gy;
            pkt.gyro[2] = gz;
            pkt.flags |= PROBE_FLAG_IMU_VALID;
            with_imu++;
        }

        /* Timestamp last so it is covered by the CRC. */
        pkt.send_ts_us = (uint32_t)esp_timer_get_time();
        probe_pkt_finalize(&pkt);

        if (send(sock, &pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt)) {
            sent++;
        } else {
            frame_errors++;
        }

        /* Pick up the ACK for this frame before spending the next 50 ms on
         * audio, otherwise the socket buffer fills and inflates apparent loss. */
        drain_acks(sock, &state);

        cadence_record(&cadence, interval_us);
    }

    collect_acks(sock, &state, ack_wait_ms);
    ack_state_finish(&state, sent, cadence_avg_ms(&cadence), cadence_max_ms(&cadence), out);
    ack_state_free(&state);

    if (out_frames_with_audio) {
        *out_frames_with_audio = with_audio;
    }

    ESP_LOGI(TAG, "stream: %lu frames sent, %lu carried audio, %lu carried IMU, %lu send errors",
             (unsigned long)sent, (unsigned long)with_audio, (unsigned long)with_imu,
             (unsigned long)frame_errors);
    return ESP_OK;
}

void udp_probe_close(int sock)
{
    if (sock >= 0) {
        close(sock);
    }
}
