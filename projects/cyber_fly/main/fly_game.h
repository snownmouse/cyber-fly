// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Shared layout constants used by the model, the view and the device app.
#define FLY_SCREEN_SIZE 480
#define FLY_HOME_X 240.0f
#define FLY_HOME_Y 262.0f

// MPRC command-frame flag bits (positions in fly_state_t.flags).
#define FLY_FLAG_ESCAPE (1u << 0)  // startle / jump (DNp01)
#define FLY_FLAG_LEFT   (1u << 1)  // steering left (DNa)
#define FLY_FLAG_RIGHT  (1u << 2)  // steering right (DNa)
#define FLY_FLAG_GROOM  (1u << 3)  // grooming (aDN1)
#define FLY_FLAG_FEED   (1u << 4)  // feeding (MN9)
#define FLY_FLAG_ESTOP  (1u << 5)  // emergency stop

// Fixed MPRC wire layout (must match tests/brain_b STAGE1 design, section 3).
#define FLY_MPRC_MAGIC   0x4352504Du  // little-endian bytes 'M','P','R','C'
#define FLY_MPRC_VERSION 1
#define FLY_MPRC_TYPE    4
#define FLY_MPRC_LEN     32

// Decoded brain downlink. Every field mirrors the wire layout 1:1 so the view
// can be driven directly from a real UDP frame with no extra glue.
typedef struct {
    int8_t  turn;        // -128..127  -> steer [-1,1]        (DNa01-DNa02)
    uint8_t thrust;      // 0..255     -> forward pace [0,1]
    uint8_t motor;       // 0..255     -> vibration motor [0,1]
    uint8_t arousal;     // 0..255     -> overall arousal [0,1] (alertness bar)
    uint8_t flags;       // bit0 escape,1 left,2 right,3 groom,4 feed,5 estop
    uint8_t groom;       // 0..255     -> grooming strength [0,1] (aDN1)
    uint8_t feed;        // 0..255     -> feeding strength [0,1]   (MN9)
    uint8_t link_q;      // 0..255     -> link quality [0,1]       (RTT/loss)
    uint8_t brain_load;  // 0..255     -> brain step cost [0,1]
} fly_state_t;

typedef enum {
    FLY_FX_SPARK = 0,  // touch ripple
    FLY_FX_PING,       // link pulse
    FLY_FX_DUST        // groom / feed debris
} fly_fx_kind_t;

typedef struct {
    bool active;
    float x, y, vx, vy;
    uint8_t kind;
    uint16_t life, max_life;
} fly_fx_t;

#define FLY_MAX_FX 18

typedef struct {
    fly_state_t state;        // last decoded MPRC downlink
    uint32_t tick;            // animation clock (frames)
    uint32_t rng;             // deterministic PRNG state
    uint32_t seq;             // last decoded seq
    uint32_t echo_ts_us;      // last decoded echo timestamp
    uint8_t raw[FLY_MPRC_LEN];// last raw MPRC frame
    bool frame_valid;         // last decode passed CRC

    // Touch-injected escape so the demo is interactive without a server.
    bool touch_escape;
    uint16_t escape_ticks;    // lingering escape visual after a touch

    // Smoothed presentation state derived ONLY from fly_state_t fields.
    float heading;            // body heading (rad)        <- state.turn
    float wing_phase;         // wing flutter phase       <- state.motor/escape
    float groom_phase;        // grooming leg phase       <- state.groom
    uint16_t blink_next;
    uint8_t blink_phase;

    // Pointer bookkeeping (reused from the pet skeleton).
    int press_x, press_y;
    bool pointer_down;

    fly_fx_t fx[FLY_MAX_FX];
} fly_game_t;

void fly_game_reset(fly_game_t *fly, uint32_t seed);
void fly_game_pointer(fly_game_t *fly, int x, int y, bool pressed, int track);
void fly_game_update(fly_game_t *fly);
uint32_t fly_game_state_hash(const fly_game_t *fly);

// Feed one real 32 B MPRC downlink frame from the cloud (task C). CRC-verified
// with the same decoder the fake generator uses; on success the decoded fields
// become the live state and seq/echo_ts_us are refreshed. Returns false (and
// leaves the previous state untouched) for any malformed frame.
bool fly_game_push_downlink(fly_game_t *fly, const uint8_t *buf, size_t len);

// MPRC wire decoding (CRC-32/ISO-HDLC verified). Returns 0 on success,
// negative ESP-style code on structural/CRC error.
int fly_state_decode(const uint8_t *buf, size_t len, fly_state_t *out);
uint32_t fly_crc32_iso_hdlc(const uint8_t *data, size_t len);

// Deterministic demo downlink generator (stand-in for the real UDP server).
void fly_fake_drive(fly_game_t *fly);
