/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file probe_common.h
 * @brief Shared probe packet format, CRC, statistics and report helpers.
 *
 * The probe packet is the contract between this firmware and
 * tools/udp_probe_server.py. Keep both sides in sync when the layout changes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Probe packet
 * ---------------------------------------------------------------------- */

#define PROBE_PKT_MAGIC    0x4D505242u  /* "MPRB" little-endian */
#define PROBE_PKT_VERSION  1u
#define PROBE_CRC_BYTES    64u          /* CRC covers the first 64 bytes */
#define PROBE_PKT_BYTES    68u          /* fixed wire size, see _Static_assert */

typedef enum {
    PROBE_PKT_BURST   = 1,  /* link probe only; feature fields are zero */
    PROBE_PKT_FEATURE = 2,  /* carries locally extracted audio + IMU features */
    PROBE_PKT_ACK     = 3,  /* server -> board reply */
} probe_pkt_type_t;

/* Flags in probe_pkt_t::flags */
#define PROBE_FLAG_AUDIO_VALID  (1u << 0)
#define PROBE_FLAG_IMU_VALID    (1u << 1)

/*
 * Wire layout, little-endian, naturally packed:
 *   payload_len  must equal PROBE_PKT_BYTES for version 1.
 *   send_ts_us   board-local esp_timer timestamp taken just before sendto().
 *   echo_ts_us   only meaningful in an ACK: the server copies send_ts_us here
 *                so the board can compute the round-trip time without a table.
 *   crc32        CRC-32/ISO-HDLC over bytes [0, PROBE_CRC_BYTES).
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t payload_len;
    uint32_t seq;
    uint32_t send_ts_us;
    uint32_t echo_ts_us;
    uint16_t sample_rate;
    uint8_t  channels;
    uint8_t  flags;
    float    rms_dbfs;
    float    band_low;      /* < 200 Hz        -> JO-B class vibratory neurons */
    float    band_mid;      /* 200-500 Hz      -> JO-A class vibratory neurons */
    float    band_high;     /* 500-2000 Hz     -> broadband / onset cue       */
    float    accel[3];      /* g     -> JO deflection-sensitive neurons */
    float    gyro[3];       /* deg/s -> body-rotation proprioception     */
    uint32_t crc32;
} probe_pkt_t;

_Static_assert(sizeof(probe_pkt_t) == PROBE_PKT_BYTES,
               "probe packet layout changed; update udp_probe_server.py too");

/** CRC-32/ISO-HDLC, identical to Python's zlib.crc32(). */
uint32_t probe_crc32(const void *data, size_t len);

/** Fill payload_len and crc32. Call this last, right before sendto(). */
void probe_pkt_finalize(probe_pkt_t *pkt);

/** Verify magic, version, length and CRC. */
bool probe_pkt_validate(const probe_pkt_t *pkt);

/** Log a one-line decode of a packet, used by the server-side debugging path. */
void probe_pkt_log(const char *tag, const probe_pkt_t *pkt);

/* -------------------------------------------------------------------------
 * Small statistics helper
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t count;
    double   min;
    double   max;
    double   sum;
    double   sum_sq;
} probe_stats_t;

void  probe_stats_reset(probe_stats_t *stats);
void  probe_stats_add(probe_stats_t *stats, double value);
double probe_stats_mean(const probe_stats_t *stats);
double probe_stats_stddev(const probe_stats_t *stats);

/* -------------------------------------------------------------------------
 * Report helpers (UART console, visible through the repository's log path)
 * ---------------------------------------------------------------------- */

void probe_banner(const char *title);
void probe_kv(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void probe_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void probe_check(const char *item, bool pass, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/** dBFS for a linear amplitude given the full-scale value (32768 for int16). */
float probe_dbfs_from_amplitude(double amplitude, double full_scale);

#ifdef __cplusplus
}
#endif
