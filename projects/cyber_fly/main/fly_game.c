// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include <math.h>
#include <stddef.h>
#include "fly_game.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int clampi(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

static uint32_t next_rand(fly_game_t *fly)
{
    fly->rng = fly->rng * 1664525u + 1013904223u;
    return fly->rng;
}

static uint16_t read_le_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t read_le_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void write_le_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

// ---------------------------------------------------------------------------
// CRC-32 / ISO-HDLC over [0,28). Poly 0xEDB88320, init/xorout 0xFFFFFFFF,
// reflected in/out — identical to zlib.crc32.
// ---------------------------------------------------------------------------
uint32_t fly_crc32_iso_hdlc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

// ---------------------------------------------------------------------------
// MPRC decode: offsets
//   0  u32 magic    0x4352504D "MPRC"
//   4  u8  version  (==1)
//   5  u8  type     (==4 command)
//   6  u16 payload_len (==32)
//   8  u32 seq
//   12 u32 echo_ts_us
//   16 i8  turn
//   17 u8  thrust
//   18 u8  motor
//   19 u8  arousal
//   20 u8  flags
//   21 u8  groom
//   22 u8  feed
//   23 u8  link_q
//   24 u8  brain_load
//   25 u8  reserved
//   26 u16 reserved2
//   28 u32 crc32    (over [0,28))
// ---------------------------------------------------------------------------
int fly_state_decode(const uint8_t *buf, size_t len, fly_state_t *out)
{
    if (!buf || !out) return -1;
    if (len < FLY_MPRC_LEN) return -2;                 // too short
    if (read_le_u32(buf + 0) != FLY_MPRC_MAGIC) return -3;  // bad magic
    if (buf[4] != FLY_MPRC_VERSION) return -4;        // bad version
    if (buf[5] != FLY_MPRC_TYPE) return -5;           // bad type
    if (read_le_u16(buf + 6) != FLY_MPRC_LEN) return -6;  // bad length
    uint32_t got = read_le_u32(buf + 28);
    uint32_t want = fly_crc32_iso_hdlc(buf, 28);
    if (got != want) return -7;                        // CRC mismatch

    out->turn = (int8_t)buf[16];
    out->thrust = buf[17];
    out->motor = buf[18];
    out->arousal = buf[19];
    out->flags = buf[20];
    out->groom = buf[21];
    out->feed = buf[22];
    out->link_q = buf[23];
    out->brain_load = buf[24];
    return 0;
}

// ---------------------------------------------------------------------------
// Fake downlink generator: a deterministic, self-driving MPRC stream so the
// demo runs with no server attached. Each branch below maps to a real field.
// ---------------------------------------------------------------------------
void fly_fake_drive(fly_game_t *fly)
{
    if (!fly) return;
    uint8_t *b = fly->raw;
    uint32_t t = fly->tick;
    memset(b, 0, FLY_MPRC_LEN);

    write_le_u32(b + 0, FLY_MPRC_MAGIC);     // magic "MPRC"
    b[4] = FLY_MPRC_VERSION;                 // version
    b[5] = FLY_MPRC_TYPE;                    // type = command
    write_le_u16(b + 6, FLY_MPRC_LEN);       // payload_len
    fly->seq += 1;
    write_le_u32(b + 8, fly->seq);           // seq (monotonic)
    fly->echo_ts_us = fly->seq * 50000u;     // echo_ts_us (50 ms/seq)
    write_le_u32(b + 12, fly->echo_ts_us);

    // Gentle steering oscillation -> turn in [-1,1] encoded as i8.
    float turn_f = sinf(t * 0.05f);
    int turn = (int)(turn_f * 115.0f);

    // Base throttle: slow cruise with a breathing wave (thrust [0,1]).
    int thrust = (int)(120.0f + 90.0f * sinf(t * 0.02f));
    int motor = 0;
    int arousal = (int)(140.0f + 90.0f * sinf(t * 0.013f));
    int flags = 0;
    int groom = 0;
    int feed = 0;

    // Escape burst: a sharp startle ~every 6 s (180 frames) for 36 frames.
    uint32_t burst = t % 180;
    bool escaping = burst < 36;
    if (escaping) {
        flags |= FLY_FLAG_ESCAPE;
        motor = 220;                                  // motor [0,1] spikes
        arousal = 250;                                // arousal bar jumps
        turn = (turn >= 0 ? 1 : -1) * 120;            // committed dodge dir
    }

    // Grooming window: ~every 5 s for 50 frames (aDN1) -> groom [0,1].
    uint32_t groom_w = t % 150;
    if (groom_w < 50) {
        flags |= FLY_FLAG_GROOM;
        float g = 1.0f - fabsf((float)groom_w - 25.0f) / 25.0f;
        groom = (int)(255.0f * g);
    }

    // Feeding window: ~every 8 s for 40 frames (MN9) -> feed [0,1].
    uint32_t feed_w = t % 240;
    if (feed_w < 40) {
        flags |= FLY_FLAG_FEED;
        float f = 1.0f - fabsf((float)feed_w - 20.0f) / 20.0f;
        feed = (int)(255.0f * f);
    }

    // Steering indicators from the sign of turn (DNa left/right).
    if (turn < -8) flags |= FLY_FLAG_LEFT;
    else if (turn > 8) flags |= FLY_FLAG_RIGHT;

    b[16] = (uint8_t)(int8_t)turn;
    b[17] = (uint8_t)clampi(thrust, 0, 255);
    b[18] = (uint8_t)clampi(motor, 0, 255);
    b[19] = (uint8_t)clampi(arousal, 0, 255);
    b[20] = (uint8_t)flags;
    b[21] = (uint8_t)clampi(groom, 0, 255);
    b[22] = (uint8_t)clampi(feed, 0, 255);
    // link_q (RTT/loss) [0,1]: slow wobble around a good link.
    b[23] = (uint8_t)clampi((int)(180.0f + 60.0f * sinf(t * 0.007f)), 0, 255);
    // brain_load (step cost) [0,1]: slow wobble.
    b[24] = (uint8_t)clampi((int)(120.0f + 80.0f * sinf(t * 0.017f)), 0, 255);
    // b[25] reserved = 0, b[26..27] reserved2 = 0

    uint32_t crc = fly_crc32_iso_hdlc(b, 28);
    write_le_u32(b + 28, crc);

    // Decode back into the live state; CRC validates by construction.
    fly->frame_valid = fly_state_decode(b, FLY_MPRC_LEN, &fly->state) == 0;
}

// ---------------------------------------------------------------------------
// Real downlink ingest (task C)
// ---------------------------------------------------------------------------
// Kept deliberately free of any transport dependency: the caller (board
// fly_app.c via fly_link, or a host harness) owns the socket and just hands us
// the bytes. That also keeps this file compilable by the host simulator, whose
// source list does not include fly_link.c.
bool fly_game_push_downlink(fly_game_t *fly, const uint8_t *buf, size_t len)
{
    if (!fly || !buf || len < FLY_MPRC_LEN) return false;
    if (len > FLY_MPRC_LEN) return false;          // 32 B exactly, no trailing slack
    fly_state_t next;
    if (fly_state_decode(buf, FLY_MPRC_LEN, &next) != 0) {
        fly->frame_valid = false;                  // keep last good state on screen
        return false;
    }
    memcpy(fly->raw, buf, FLY_MPRC_LEN);
    fly->state = next;
    fly->seq = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
               ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    fly->echo_ts_us = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8) |
                      ((uint32_t)buf[14] << 16) | ((uint32_t)buf[15] << 24);
    fly->frame_valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// FX
// ---------------------------------------------------------------------------
static void spawn_fx(fly_game_t *fly, uint8_t kind, float x, float y)
{
    int slot = -1, oldest = 0;
    uint16_t oldest_life = 0xffffU;
    for (int i = 0; i < FLY_MAX_FX; ++i) {
        if (!fly->fx[i].active) { slot = i; break; }
        if (fly->fx[i].life < oldest_life) {
            oldest_life = fly->fx[i].life;
            oldest = i;
        }
    }
    if (slot < 0) slot = oldest;
    fly_fx_t *fx = &fly->fx[slot];
    fx->active = true;
    fx->x = x;
    fx->y = y;
    fx->kind = kind;
    fx->vx = fx->vy = 0.0f;
    switch (kind) {
    case FLY_FX_SPARK:
        fx->vx = (float)(next_rand(fly) % 160) / 100.0f - 0.8f;
        fx->vy = -1.1f - (float)(next_rand(fly) % 50) / 100.0f;
        fx->life = fx->max_life = 28;
        break;
    case FLY_FX_PING:
        fx->vy = -0.5f;
        fx->life = fx->max_life = 36;
        break;
    default:  // FLY_FX_DUST
        fx->vx = (float)(next_rand(fly) % 120) / 100.0f - 0.6f;
        fx->vy = -1.2f - (float)(next_rand(fly) % 40) / 100.0f;
        fx->life = fx->max_life = 40;
        break;
    }
}

static void update_fx(fly_game_t *fly)
{
    for (int i = 0; i < FLY_MAX_FX; ++i) {
        fly_fx_t *fx = &fly->fx[i];
        if (!fx->active) continue;
        fx->life--;
        if (fx->life == 0) { fx->active = false; continue; }
        fx->x += fx->vx;
        fx->y += fx->vy;
        fx->vx *= 0.98f;
        fx->vy *= 0.98f;
    }
}

static void update_blink(fly_game_t *fly)
{
    if (fly->blink_phase) {
        fly->blink_phase++;
        if (fly->blink_phase > 3) fly->blink_phase = 0;
    } else if (--fly->blink_next == 0) {
        fly->blink_phase = 1;
        fly->blink_next = (uint16_t)(40 + (next_rand(fly) % 90));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void fly_game_reset(fly_game_t *fly, uint32_t seed)
{
    if (!fly) return;
    memset(fly, 0, sizeof(*fly));
    for (int i = 0; i < FLY_MAX_FX; ++i) fly->fx[i].active = false;
    fly->rng = seed ? seed : 0x46595A49u;
    fly->tick = 0;
    fly->seq = 0;
    fly->echo_ts_us = 0;
    fly->frame_valid = false;
    fly->heading = 0.0f;
    fly->wing_phase = 0.0f;
    fly->groom_phase = 0.0f;
    fly->blink_next = 60;
    fly->blink_phase = 0;
    fly->pointer_down = false;
    fly->escape_ticks = 0;
    fly->touch_escape = false;
    // Prime one frame so the very first render has a valid state.
    fly_fake_drive(fly);
}

void fly_game_pointer(fly_game_t *fly, int x, int y, bool pressed, int track)
{
    (void)track;
    if (!fly) return;
    fly->press_x = x;
    fly->press_y = y;
    if (pressed) {
        fly->pointer_down = true;
        // Interactive demo hook: a tap injects a fake "escape" burst.
        fly->touch_escape = true;
        fly->escape_ticks = 45;
        spawn_fx(fly, FLY_FX_SPARK, (float)x, (float)y);
    } else {
        fly->pointer_down = false;
    }
}

void fly_game_update(fly_game_t *fly)
{
    if (!fly) return;
    fly->tick++;
    // NOTE: the downlink is injected by the caller (fly_fake_drive for the
    // offline demo / host sim, fly_game_push_downlink for the real cloud link),
    // so this function is pure animation + smoothing and stays transport-free.

    // Touch-injected escape overrides/ORs the decoded flag for a few frames.
    if (fly->escape_ticks > 0) {
        fly->escape_ticks--;
        fly->state.flags |= FLY_FLAG_ESCAPE;
        if (fly->state.motor < 200) fly->state.motor = 200;
        if (fly->state.arousal < 230) fly->state.arousal = 230;
    }

    // Smooth body heading from turn (i8 in [-128,127] -> [-1,1] -> ~51 deg).
    float turn_norm = (float)fly->state.turn / 127.0f;
    float target_heading = turn_norm * 0.9f;
    fly->heading += (target_heading - fly->heading) * 0.12f;

    // Wing flutter speed scales with motor + escape startle.
    float flutter = 0.25f + (float)fly->state.motor / 255.0f * 0.9f
                    + ((fly->state.flags & FLY_FLAG_ESCAPE) ? 0.8f : 0.0f);
    fly->wing_phase += flutter;

    // Grooming leg phase advances with groom strength.
    float groom_norm = (float)fly->state.groom / 255.0f;
    fly->groom_phase += (groom_norm * 0.55f - 0.04f);
    if (fly->groom_phase < 0) fly->groom_phase = 0;
    if (fly->groom_phase > 6.2831853f) fly->groom_phase -= 6.2831853f;

    update_blink(fly);
    update_fx(fly);
}

static void fnv_bytes(uint32_t *hash, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        *hash ^= bytes[i];
        *hash *= 16777619u;
    }
}

uint32_t fly_game_state_hash(const fly_game_t *fly)
{
    if (!fly) return 0;
    uint32_t hash = 2166136261u;
    fnv_bytes(&hash, &fly->state, sizeof(fly->state));
    fnv_bytes(&hash, &fly->frame_valid, sizeof(fly->frame_valid));
    fnv_bytes(&hash, &fly->seq, sizeof(fly->seq));
    fnv_bytes(&hash, &fly->tick, sizeof(fly->tick));
    fnv_bytes(&hash, &fly->heading, sizeof(fly->heading));
    fnv_bytes(&hash, &fly->wing_phase, sizeof(fly->wing_phase));
    fnv_bytes(&hash, &fly->groom_phase, sizeof(fly->groom_phase));
    fnv_bytes(&hash, &fly->blink_phase, sizeof(fly->blink_phase));
    fnv_bytes(&hash, &fly->escape_ticks, sizeof(fly->escape_ticks));
    for (int i = 0; i < FLY_MAX_FX; ++i)
        if (fly->fx[i].active)
            fnv_bytes(&hash, &fly->fx[i], sizeof(fly_fx_t));
    return hash;
}
