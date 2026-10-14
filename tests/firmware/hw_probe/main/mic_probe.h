/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mic_probe.h
 * @brief ES8311 microphone noise-floor measurement, band energies and gain scan.
 *
 * Capture format is 16 kHz / 16 bit / mono. The mono channel count matters:
 * the board carries a single microphone, so opening the codec with two
 * channels would return an interleaved pair whose right half is meaningless.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Analysis result for one capture window. */
typedef struct {
    bool     valid;
    uint32_t samples;
    int      gain_db;

    /* Level metrics, all in dBFS relative to int16 full scale. */
    float    ac_rms_dbfs;      /* RMS after removing the DC bias: the real floor */
    float    raw_rms_dbfs;     /* RMS including the DC bias, for reference      */
    float    peak_dbfs;
    float    crest_db;         /* peak - rms, hints at impulsive noise          */

    /* DC bias in int16 LSB. A large value points at a codec config problem. */
    float    dc_offset;

    /* Clipping. Any sustained value above zero costs sensitivity headroom. */
    float    clip_ratio;
    uint32_t clipped_samples;

    /* Band energies as the strongest component in each band, dBFS. */
    float    band_low_db;      /* 50-150 Hz    -> JO-B class neurons  */
    float    band_mid_db;      /* 250-450 Hz   -> JO-A class neurons  */
    float    band_high_db;     /* 600-1500 Hz  -> broadband / onset   */
} mic_analysis_t;

/**
 * @brief Initialize the codec and open the microphone at 16 kHz / 16 bit / mono.
 *
 * @param out_mic   Receives the codec handle. Reused later for the steady UDP
 *                  stream so the codec is opened exactly once.
 * @param gain_db   Input gain in dB, applied with esp_codec_dev_set_in_gain().
 */
esp_err_t mic_probe_open(esp_codec_dev_handle_t *out_mic, int gain_db);

/** Change the input gain on an already open microphone. */
esp_err_t mic_probe_set_gain(esp_codec_dev_handle_t mic, int gain_db);

/**
 * @brief Capture for @p seconds and fill @p out.
 *
 * Discards the first 200 ms so the codec's AGC and DC settling do not pollute
 * the floor figure.
 */
esp_err_t mic_probe_measure(esp_codec_dev_handle_t mic, int seconds, int gain_db,
                            mic_analysis_t *out);

/**
 * @brief Capture one short window and return features for a UDP feature frame.
 *
 * Reuses the same Goertzel band analysis as mic_probe_measure().
 */
esp_err_t mic_probe_sample_features(esp_codec_dev_handle_t mic, mic_analysis_t *out);

/**
 * @brief Sweep 0..50 dB, print a table and recommend a gain.
 *
 * @param mic             Open microphone handle.
 * @param recommend_out   Receives the recommended gain in dB.
 */
esp_err_t mic_probe_gain_scan(esp_codec_dev_handle_t mic, int *recommend_out);

/**
 * @brief Full step 3 of the probe run: open, measure, sweep and report.
 *
 * On success @p out_mic keeps a usable handle for the UDP feature stream.
 */
void mic_probe_run(int default_gain_db, esp_codec_dev_handle_t *out_mic,
                   mic_analysis_t *out, int *recommended_gain_db);

#ifdef __cplusplus
}
#endif
