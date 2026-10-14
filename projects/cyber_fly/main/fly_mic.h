// SPDX-License-Identifier: Apache-2.0
//
// Board-side sensing for the cyber fly.
//
// This is a deliberate, trimmed port of tests/firmware/hw_probe/main/mic_probe.c
// (Task A) rather than a rewrite. Kept from the original:
//   * the ES8311 open sequence via bsp_audio_codec_microphone_init() - we do not
//     touch a single codec register by hand;
//   * the Goertzel band analysis and the dBFS normalisation, INCLUDING the fix
//     Task A found the hard way: band_rms_dbfs() takes a full scale of 1.0
//     because band_peak_dbfs() already returns dBFS. Dividing by 32768 a second
//     time sinks every band by 90.3 dB, which is the artifact that first showed
//     up as band levels far below the broadband RMS.
//
// Dropped from the original, on purpose:
//   * mic_probe_measure() / mic_probe_gain_scan() / mic_probe_run() - the gain
//     sweep and the pretty report are diagnostics, not control path. This is why
//     this file needs no probe_common.h dependency (that only provided
//     probe_banner/probe_note/probe_dbfs_from_amplitude).
//
// What survives is exactly the 20 Hz feature path: one 50 ms window per control
// frame, turned into the four numbers the cloud orchestrator maps to neurons.
#pragma once

#include "fly_link.h"   // fly_mic_features_t

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the ES8311 microphone at 16 kHz / 16 bit / mono.
 *
 * Mono matters: the board carries one microphone, so a stereo open would return
 * an interleaved pair whose right half is meaningless.
 */
esp_err_t fly_mic_open(esp_codec_dev_handle_t *out_mic, int gain_db);

/**
 * @brief Capture exactly one 50 ms window and fill @p out.
 *
 * Uses the gain the codec is already set to. Blocking for ~50 ms, so call it
 * from a task that is allowed to block - not from the UI render callback.
 */
esp_err_t fly_mic_sample(esp_codec_dev_handle_t mic, fly_mic_features_t *out);

#ifdef __cplusplus
}
#endif
