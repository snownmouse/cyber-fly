// Host-side compile harness for the Task C board sources.
//
// The board transport (fly_link.c's ESP half) needs ESP-IDF, so it CANNOT be
// checked here. What this does check is the half that a board-only mistake
// would silently break: the pure-C core that both the renderer and the cloud
// interop test share.
//
// Built and run on the Lighthouse box (which has gcc 12.2.0):
//   gcc -O2 -Wall -Wextra -o verify_hostcheck verify_hostcheck.c fly_link.c fly_game.c -lm
//   ./verify_hostcheck
//
// It deliberately does NOT link fly_mic.c or fly_wifi.c: those pull in
// esp_codec_dev / esp_wifi. The point of this file is to prove fly_link.c stays
// compilable with the transport half switched off, which is exactly the
// configuration the interop harness uses.
#include <stdio.h>
#include <string.h>

#include "fly_game.h"   // fly_crc32_iso_hdlc(), FLY_MPRC_* wire constants
#include "fly_link.h"

static int failures;

#define CHECK(cond, label) do { \
    if (cond) { \
        printf("  ok   %s\n", label); \
    } else { \
        printf("  FAIL %s\n", label); \
        failures++; \
    } \
} while (0)

int main(void)
{
    printf("== fly_link core host check ==\n");

    // A silent frame (mic == NULL) must still be a VALID frame: the cloud has to
    // answer it, otherwise a dead microphone would look like a dead link.
    uint8_t buf[FLY_LINK_MPRB_LEN];
    fly_link_build_mprb(buf, 7, 123456, NULL, 0.0f);

    uint32_t magic = 0, crc_stored = 0, crc_calc = 0;
    memcpy(&magic, buf + 0, 4);
    memcpy(&crc_stored, buf + 64, 4);
    crc_calc = fly_crc32_iso_hdlc(buf, 64);
    float rms = 0.0f;
    memcpy(&rms, buf + 24, 4);

    CHECK(magic == FLY_LINK_MPRB_MAGIC, "silent MPRB carries the MPRB magic");
    CHECK(buf[4] == 1 && buf[5] == 2, "version 1, type 2 (feature)");
    CHECK(buf[6] == (uint8_t)FLY_LINK_MPRB_LEN, "payload_len low byte");
    CHECK(crc_stored == crc_calc, "silent MPRB CRC verifies");
    CHECK(rms <= -59.0f, "silent MPRB reports a silence floor (<= -59 dBFS)");

    // A NULL mic and an invalid mic must be indistinguishable on the wire -
    // that is the whole point of "a dead microphone looks like silence".
    uint8_t buf2[FLY_LINK_MPRB_LEN];
    fly_link_build_mprb(buf2, 7, 123456, NULL, 0.0f);
    CHECK(memcmp(buf, buf2, FLY_LINK_MPRB_LEN) == 0, "build is deterministic");

    // Downlink path: a real MPRC must be accepted, a corrupt one rejected.
    fly_link_t link;
    fly_link_reset(&link, 1000);

    uint8_t dl[FLY_LINK_MPRC_LEN];
    memset(dl, 0, sizeof(dl));
    const uint32_t dmagic = 0x4352504Du;   // 'MPRC'
    memcpy(dl + 0, &dmagic, 4);
    dl[4] = 1; dl[5] = 4; dl[6] = (uint8_t)FLY_LINK_MPRC_LEN;
    memcpy(dl + 8, &(uint32_t){ 42 }, 4);
    dl[16] = (uint8_t)(int8_t)-100;        // turn
    dl[20] = 0x01;                         // flags.escape
    const uint32_t dcrc = fly_crc32_iso_hdlc(dl, 28);
    memcpy(dl + 28, &dcrc, 4);

    CHECK(fly_link_feed(&link, dl, FLY_LINK_MPRC_LEN, 1100), "valid MPRC accepted");
    CHECK(link.rx_frames == 1 && link.rx_rejects == 0, "counters: 1 accepted, 0 rejected");
    CHECK(link.last_seq == 42, "seq recorded");

    uint8_t taken[FLY_LINK_MPRC_LEN];
    CHECK(fly_link_take(&link, taken), "take() hands the frame over");
    CHECK(memcmp(taken, dl, FLY_LINK_MPRC_LEN) == 0, "taken frame is byte-identical");

    // Sequence gap accounting: 42 -> 45 is loss, 45 -> 46 is continuity.
    memcpy(dl + 8, &(uint32_t){ 45 }, 4);
    { const uint32_t c = fly_crc32_iso_hdlc(dl, 28); memcpy(dl + 28, &c, 4); }
    CHECK(fly_link_feed(&link, dl, FLY_LINK_MPRC_LEN, 1200), "frame after a gap accepted");
    CHECK(link.seq_gaps == 1, "gap counted once");
    memcpy(dl + 8, &(uint32_t){ 46 }, 4);
    { const uint32_t c = fly_crc32_iso_hdlc(dl, 28); memcpy(dl + 28, &c, 4); }
    CHECK(fly_link_feed(&link, dl, FLY_LINK_MPRC_LEN, 1300), "next contiguous frame accepted");
    CHECK(link.seq_gaps == 1, "contiguous frame adds no gap");

    // Negative tests.
    uint8_t bad[FLY_LINK_MPRC_LEN];
    memcpy(bad, dl, FLY_LINK_MPRC_LEN);
    bad[16] ^= 0x01;                        // flip a bit of turn -> CRC must fail
    CHECK(!fly_link_feed(&link, bad, FLY_LINK_MPRC_LEN, 1400), "bit flip rejected (CRC)");
    CHECK(link.rx_rejects == 1, "reject counted");

    memcpy(bad, dl, FLY_LINK_MPRC_LEN);
    bad[0] ^= 0x01;                         // flip a bit of magic
    CHECK(!fly_link_feed(&link, bad, FLY_LINK_MPRC_LEN, 1500), "magic flip rejected");

    CHECK(!fly_link_feed(&link, dl, 20, 1600), "20-byte datagram rejected as short");
    CHECK(link.rx_short == 1, "short counted separately from rejects");

    // A rejected frame must not damage the last good state.
    uint8_t after[FLY_LINK_MPRC_LEN];
    CHECK(fly_link_take(&link, after), "still have a frame after the bad ones");
    CHECK(memcmp(after + 8, &(uint32_t){ 46 }, 4) == 0, "good state preserved (seq 46)");

    // link_q decay: fresh frame -> high, past the timeout -> zero.
    const uint8_t q_fresh = fly_link_quality(&link, 1300);
    const uint8_t q_stale = fly_link_quality(&link, 1300 + FLY_LINK_TIMEOUT_MS + 10);
    CHECK(q_fresh > 200, "link_q high right after a frame");
    CHECK(q_stale == 0, "link_q zero once the timeout passes");
    CHECK(!fly_link_online(&link, 1300 + FLY_LINK_TIMEOUT_MS + 10), "online() false when stale");

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
