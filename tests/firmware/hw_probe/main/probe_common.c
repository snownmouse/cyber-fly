/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "probe_common.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "hw_probe";

/* -------------------------------------------------------------------------
 * CRC-32/ISO-HDLC (reflected, poly 0xEDB88320), matching zlib.crc32()
 * ---------------------------------------------------------------------- */

uint32_t probe_crc32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            /* Branchless: mask is all-ones when the low bit is set. */
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void probe_pkt_finalize(probe_pkt_t *pkt)
{
    if (!pkt) {
        return;
    }
    pkt->magic = PROBE_PKT_MAGIC;
    pkt->version = PROBE_PKT_VERSION;
    pkt->payload_len = PROBE_PKT_BYTES;
    pkt->crc32 = probe_crc32(pkt, PROBE_CRC_BYTES);
}

bool probe_pkt_validate(const probe_pkt_t *pkt)
{
    if (!pkt) {
        return false;
    }
    if (pkt->magic != PROBE_PKT_MAGIC || pkt->version != PROBE_PKT_VERSION) {
        return false;
    }
    if (pkt->payload_len != PROBE_PKT_BYTES) {
        return false;
    }
    return pkt->crc32 == probe_crc32(pkt, PROBE_CRC_BYTES);
}

void probe_pkt_log(const char *tag, const probe_pkt_t *pkt)
{
    if (!pkt) {
        return;
    }
    ESP_LOGI(tag, "seq=%lu type=%u ts=%lu echo=%lu rms=%.1f dBFS low=%.1f mid=%.1f high=%.1f",
             (unsigned long)pkt->seq, (unsigned)pkt->type,
             (unsigned long)pkt->send_ts_us, (unsigned long)pkt->echo_ts_us,
             (double)pkt->rms_dbfs, (double)pkt->band_low,
             (double)pkt->band_mid, (double)pkt->band_high);
}

/* -------------------------------------------------------------------------
 * Statistics
 * ---------------------------------------------------------------------- */

void probe_stats_reset(probe_stats_t *stats)
{
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    stats->min = 0.0;
    stats->max = 0.0;
}

void probe_stats_add(probe_stats_t *stats, double value)
{
    if (!stats) {
        return;
    }
    if (stats->count == 0) {
        stats->min = value;
        stats->max = value;
    } else {
        if (value < stats->min) {
            stats->min = value;
        }
        if (value > stats->max) {
            stats->max = value;
        }
    }
    stats->count++;
    stats->sum += value;
    stats->sum_sq += value * value;
}

double probe_stats_mean(const probe_stats_t *stats)
{
    if (!stats || stats->count == 0) {
        return 0.0;
    }
    return stats->sum / (double)stats->count;
}

double probe_stats_stddev(const probe_stats_t *stats)
{
    if (!stats || stats->count < 2) {
        return 0.0;
    }
    const double mean = probe_stats_mean(stats);
    const double variance = (stats->sum_sq / (double)stats->count) - (mean * mean);
    return variance > 0.0 ? sqrt(variance) : 0.0;
}

/* -------------------------------------------------------------------------
 * Report helpers
 * ---------------------------------------------------------------------- */

void probe_banner(const char *title)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "==================== %s ====================", title);
}

void probe_kv(const char *key, const char *fmt, ...)
{
    char value[192];
    va_list args;

    va_start(args, fmt);
    vsnprintf(value, sizeof(value), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "  %-16s : %s", key, value);
}

void probe_note(const char *fmt, ...)
{
    char text[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "  . %s", text);
}

void probe_check(const char *item, bool pass, const char *fmt, ...)
{
    char detail[192];
    va_list args;

    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "  [%s] %-30s %s", pass ? "PASS" : "FAIL", item, detail);
}

float probe_dbfs_from_amplitude(double amplitude, double full_scale)
{
    if (amplitude <= 0.0 || full_scale <= 0.0) {
        /* Clamp instead of returning -inf so the report stays readable. */
        return -120.0f;
    }
    const double ratio = amplitude / full_scale;
    if (ratio >= 1.0) {
        return 0.0f;
    }
    return (float)(20.0 * log10(ratio));
}
