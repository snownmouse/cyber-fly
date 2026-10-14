// SPDX-License-Identifier: Apache-2.0
//
// Task C - board-side brain downlink receiver.
//
// Transport rationale
// -------------------
// The cloud orchestrator (tests/brain_b/b_loop/stage1_loop.js) already answers
// every MPRB feature frame with an MPRC command frame sent straight back to the
// sender's observed address:port:
//
//     sock.on('message', (msg, rinfo) => { ... sock.send(dl, rinfo.port, rinfo.address); })
//
// So the board never has to accept an inbound connection: it keeps one UDP
// socket open, emits a feature frame every control tick, and accepts whatever
// comes back. This matters because the board sits behind a phone-hotspot NAT -
// a server-initiated push would never arrive, but a reply to a packet we just
// sent does (the NAT mapping is refreshed by the upstream traffic anyway).
//
// This file is split in two halves so the interesting half is testable off-board:
//   * the core (always compiled) is pure C - feed it datagrams, it validates
//     them with the SAME fly_state_decode() the renderer uses, and keeps the
//     newest good frame plus link statistics;
//   * the ESP-IDF UDP task (only when FLY_LINK_ESP_UDP=1) owns the socket.
//
// The host harness tests/brain_b/b_loop/verify_fly_link.c compiles the core on the
// build server and replays the orchestrator's real downlink_10s.bin through it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * One 50 ms analysis window, in the units the MPRB frame carries.
 *
 * Declared HERE rather than in fly_mic.h on purpose: this header is included by
 * the host-side interop harness, which has no ESP-IDF. Keeping the struct free
 * of esp_codec_dev lets fly_link.c read its fields while still compiling with
 * the transport half switched off - which is exactly the configuration the
 * cloud interop test uses.
 */
typedef struct fly_mic_features {
    bool  valid;
    float rms_dbfs;      /* broadband loudness, DC removed  -> AUDIO (escape) */
    float band_low_db;   /* 50-150 Hz                        -> JO-B         */
    float band_mid_db;   /* 250-450 Hz                       -> JO-A         */
    float band_high_db;  /* 600-1500 Hz                      -> broadband    */
    float peak_dbfs;     /* for the CLIP/overload indicator on screen */
} fly_mic_features_t;

#ifdef __cplusplus
extern "C" {
#endif

// Wire constants shared with tests/brain_b (design doc section 3).
#define FLY_LINK_MPRB_LEN 68u
#define FLY_LINK_MPRC_LEN 32u
#define FLY_LINK_MPRB_MAGIC 0x4D505242u   // little-endian bytes 'M','P','R','B'
#define FLY_LINK_DEFAULT_PORT 33333u

// A frame is considered "live" if one arrived within this window.
#define FLY_LINK_TIMEOUT_MS 1500u

typedef struct {
    // --- link statistics (all monotonic counters) ---
    uint32_t tx_frames;      // MPRB feature frames we sent upstream
    uint32_t rx_frames;      // valid MPRC frames accepted
    uint32_t rx_rejects;     // rejected by magic / length / CRC
    uint32_t rx_short;       // datagrams shorter than 32 B
    uint32_t seq_gaps;       // accepted frames whose seq skipped ahead
    uint32_t last_seq;       // seq of the newest accepted frame
    uint32_t rtt_ms;         // echo_ts_us round trip, 0 until the first reply
    uint32_t last_rx_ms;     // monotonic timestamp of the newest accepted frame
    uint32_t last_tx_ms;     // monotonic timestamp of the newest sent frame

    // --- newest accepted frame ---
    bool have_frame;
    uint8_t slot[FLY_LINK_MPRC_LEN];
} fly_link_t;

// Reset to a clean, disconnected state. `now_ms` seeds the timestamps so the
// first timeout computation is not relative to zero.
void fly_link_reset(fly_link_t *link, uint32_t now_ms);

// Feed one received datagram.
//   len < 32              -> counted as rx_short, returns false
//   fly_state_decode fail -> counted as rx_rejects, returns false
//   otherwise             -> stored in slot, stats updated, returns true
// `now_ms` is a monotonic millisecond clock (any epoch).
bool fly_link_feed(fly_link_t *link, const uint8_t *buf, size_t len, uint32_t now_ms);

// Copy the newest accepted frame out and clear the "fresh" flag.
// Returns false when nothing has been received yet.
bool fly_link_take(fly_link_t *link, uint8_t out[FLY_LINK_MPRC_LEN]);

// True when a frame arrived within FLY_LINK_TIMEOUT_MS.
bool fly_link_online(const fly_link_t *link, uint32_t now_ms);

// link_q in [0,1] for the on-screen bar: 1.0 when frames arrive at ~20 Hz and
// no rejects/gaps have been seen recently, decaying with the frame interval.
uint8_t fly_link_quality(const fly_link_t *link, uint32_t now_ms);

// Build a valid 68 B MPRB feature frame (CRC included).
//
// `mic` may be NULL or invalid, in which case the frame still goes out with a
// silence floor of -60 dBFS. That keeps the frame *valid* - so the cloud answers
// it and the link statistics stay meaningful even when the codec failed to open -
// but the brain will (correctly) read it as "nothing is happening". A dead
// microphone must look like silence, never like a fabricated stimulus.
//
// `accel_x` is the signed sideways acceleration in g. It travels to the cloud so
// the orchestrator can map it to LEFT/RIGHT: in v4 the steering neurons have no
// named sensory input, so the brain cannot tell left from right on its own
// (design doc 0.5).
void fly_link_build_mprb(uint8_t out[FLY_LINK_MPRB_LEN], uint32_t seq,
                         uint32_t send_ts_us, const fly_mic_features_t *mic,
                         float accel_x);

#if FLY_LINK_ESP_UDP
#include "esp_err.h"

// Start the receive task. `host` is a dotted-quad IPv4 literal (no DNS on the
// critical path); `port` is the cloud orchestrator's UDP port. Returns ESP_OK
// once the socket is bound and the task is running.
esp_err_t fly_link_udp_start(fly_link_t *link, const char *host, uint16_t port);

// True once fly_link_udp_start() has succeeded.
bool fly_link_udp_running(void);

// Stop the task and close the socket (safe to call when not running).
void fly_link_udp_stop(void);
#endif  // FLY_LINK_ESP_UDP

#ifdef __cplusplus
}
#endif
