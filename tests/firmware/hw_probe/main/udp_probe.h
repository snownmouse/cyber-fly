/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file udp_probe.h
 * @brief UDP link quality measurement: loss, ordering, jitter and RTT.
 *
 * Every packet carries a monotonically increasing sequence number and a
 * board-local timestamp. The server echoes the timestamp in its ACK, so the
 * board can compute a real round-trip time instead of guessing from the send
 * rate. Bandwidth was never the constraint here; burst loss and jitter on a
 * shared 2.4 GHz channel are what actually need measuring.
 */

#pragma once

/* lwip/sockets.h (not netinet/in.h) is what actually defines struct sockaddr_in,
 * AF_INET and the socket calls in ESP-IDF. Pulling it in here keeps the type
 * complete for every caller, so main.c can hold a struct sockaddr_in by value. */
#include "lwip/sockets.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sent;
    uint32_t acked;              /* total ACKs received */
    uint32_t unique_acked;       /* duplicate ACKs collapsed */
    uint32_t lost;               /* sent - unique_acked */
    float    loss_pct;
    uint32_t crc_errors;
    uint32_t foreign_packets;    /* replies that were not valid probe ACKs */

    /* Round-trip time in milliseconds, measured from the echoed timestamp. */
    double   rtt_min_ms;
    double   rtt_avg_ms;
    double   rtt_max_ms;
    double   rtt_jitter_ms;      /* standard deviation of RTT */

    /* Actual send cadence, to expose drift caused by local processing. */
    double   send_gap_avg_ms;
    double   send_gap_max_ms;

    bool     any_ack;
} udp_probe_result_t;

/**
 * @brief Create and connect the UDP socket used for every probe stage.
 *
 * @param server_ip  IPv4 address running tools/udp_probe_server.py.
 * @param port       Server UDP port.
 * @param out_sock   Receives the socket descriptor.
 * @param out_dst    Receives the resolved destination address.
 */
esp_err_t udp_probe_open(const char *server_ip, uint16_t port, int *out_sock,
                         struct sockaddr_in *out_dst);

/**
 * @brief Fire @p count packets back to back and collect the replies.
 *
 * @param interval_us  Gap between packets. 0 sends as fast as the stack drains,
 *                     which measures local buffering more than the radio.
 * @param ack_wait_ms  How long to keep listening for ACKs after the last send.
 */
esp_err_t udp_probe_burst(int sock, const struct sockaddr_in *dst, uint32_t count,
                          uint32_t interval_us, int ack_wait_ms, udp_probe_result_t *out);

/**
 * @brief Stream locally extracted feature frames at a fixed cadence.
 *
 * This is the shape the real closed loop will use: capture a 50 ms audio
 * window, read the IMU, encode both into one packet, send it, and pick up the
 * ACKs in between. Audio and IMU validity are reported through the packet flags.
 *
 * @param mic          Open microphone handle, or NULL to send zeroed audio.
 * @param hz           Target frame rate.
 * @param out_frames_with_audio  Receives the count of frames carrying audio.
 */
esp_err_t udp_probe_stream(int sock, const struct sockaddr_in *dst, esp_codec_dev_handle_t mic,
                           uint32_t seconds, uint32_t hz, int ack_wait_ms,
                           udp_probe_result_t *out, uint32_t *out_frames_with_audio);

/** Close a socket created by udp_probe_open(). */
void udp_probe_close(int sock);

#ifdef __cplusplus
}
#endif
