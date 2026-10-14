// SPDX-License-Identifier: Apache-2.0
//
// Task C verification harness: the board-side downlink receiver, off-board.
//
// fly_link.c's core and fly_game.c's new push path are plain C with no ESP-IDF
// dependency, so we can compile the exact same sources here (gcc -O2 -Wall
// -Wextra) and drive them with the REAL downlink stream the cloud orchestrator
// produced (stage1_loop.js --emit-dl downlink_10s.bin).
//
// What this proves, and what it deliberately does not:
//   1. fly_link_feed() accepts every real orchestrator frame (CRC-verified with
//      the same fly_state_decode() the renderer uses).
//   2. fly_game_push_downlink() turns those bytes into a live fly_state_t, so
//      the board path is proven without flashing hardware.
//   3. The negative tests still bite: a flipped payload bit is rejected by CRC,
//      a flipped magic byte is rejected by magic, a short datagram is counted
//      separately and never reaches the state.
//   4. fly_link_build_mprb() emits a 68 B feature frame the SERVER accepts --
//      verified by also writing those frames to a file and feeding them to
//      `node stage1_loop.js --analyze`, i.e. the orchestrator's own decoder.
//      That closes the loop in the upstream direction too, so the board really
//      can ask the brain for a decision.
//
// It does NOT prove the socket works: that still needs the board (and a
// phone-hotspot NAT). See projects/cyber_fly/README.md.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fly_game.h"
#include "fly_link.h"

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
           detail ? " -- " : "", detail ? detail : "");
    if (!ok) failures++;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
    const char *in = (argc > 1) ? argv[1] : "/root/b_loop/downlink_10s.bin";
    const char *mprbOut = (argc > 2) ? argv[2] : "/root/mprb_from_board.bin";

    FILE *f = fopen(in, "rb");
    if (!f) { printf("!! 打开 %s 失败\n", in); return 1; }
    fseek(f, 0, SEEK_END);
    const long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (bytes <= 0 || bytes % FLY_LINK_MPRC_LEN != 0) {
        printf("!! %s 大小 %ld 不是 32 的整数倍\n", in, bytes);
        fclose(f);
        return 1;
    }
    const int nframes = (int)(bytes / FLY_LINK_MPRC_LEN);
    uint8_t *raw = (uint8_t *)malloc((size_t)bytes);
    if (!raw || fread(raw, 1, (size_t)bytes, f) != (size_t)bytes) {
        printf("!! 读取失败\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    printf("MPRC 下行接收链路验证（板端同一份 C 代码，编译于构建服务器）\n");
    printf("  输入 %s：%d 帧 × %u B = %ld B\n\n", in, nframes,
           (unsigned)FLY_LINK_MPRC_LEN, bytes);

    // ---- 1. 真实下行流：每帧按 50 ms 控制周期喂入 ------------------------
    printf("[1] 逐帧喂入 fly_link_feed()\n");
    fly_link_t link;
    fly_link_reset(&link, 1000);
    fly_game_t fly;
    fly_game_reset(&fly, 0x46595A49u);

    uint32_t clock = 1000;
    int accepted = 0, pushed = 0;
    int esc = 0, groom = 0, feed = 0, left = 0, right = 0;
    int turn_min = 128, turn_max = -128;
    uint32_t first_seq = 0, last_seq = 0;
    for (int i = 0; i < nframes; ++i) {
        const uint8_t *fr = raw + (size_t)i * FLY_LINK_MPRC_LEN;
        if (fly_link_feed(&link, fr, FLY_LINK_MPRC_LEN, clock)) {
            accepted++;
            if (i == 0) first_seq = link.last_seq;
            last_seq = link.last_seq;
            uint8_t take[FLY_LINK_MPRC_LEN];
            if (fly_link_take(&link, take) &&
                fly_game_push_downlink(&fly, take, FLY_LINK_MPRC_LEN)) {
                pushed++;
                const fly_state_t *s = &fly.state;
                if (s->flags & FLY_FLAG_ESCAPE) esc++;
                if (s->flags & FLY_FLAG_GROOM) groom++;
                if (s->flags & FLY_FLAG_FEED) feed++;
                if (s->flags & FLY_FLAG_LEFT) left++;
                if (s->flags & FLY_FLAG_RIGHT) right++;
                if (s->turn < turn_min) turn_min = s->turn;
                if (s->turn > turn_max) turn_max = s->turn;
            }
        }
        clock += 50;   // 20 Hz
    }
    printf("    收 %u 帧 / 坏帧 %lu / 短包 %lu / 丢序 %lu / seq %lu→%lu\n",
           (unsigned)link.rx_frames, (unsigned long)link.rx_rejects,
           (unsigned long)link.rx_short, (unsigned long)link.seq_gaps,
           (unsigned long)first_seq, (unsigned long)last_seq);
    printf("    推送进 fly_game %d 帧；逃逸 %d 左 %d 右 %d 梳理 %d 进食 %d；turn [%d, %d]\n",
           pushed, esc, left, right, groom, feed, turn_min, turn_max);

    check(accepted == nframes, "真实下行 200/200 全部通过 CRC 校验",
          "0 丢弃才算数");
    check(pushed == nframes, "全部帧都能推进 fly_state_t", "板端路径打通");
    check(link.rx_rejects == 0 && link.rx_short == 0, "无误判、无短包",
          "统计必须干净");
    check(esc + groom + feed + left + right > 0, "确实解码出了行为位",
          "不是全零的假通过");
    check(fly.frame_valid, "末帧 frame_valid 为真", "渲染器可直接用");

    // ---- 2. 负例：CRC / magic / 短包 --------------------------------------
    printf("\n[2] 负例（必须被拒）\n");
    uint8_t bad[FLY_LINK_MPRC_LEN];
    memcpy(bad, raw, FLY_LINK_MPRC_LEN);
    bad[16] ^= 0x01;                       // flip one bit of turn
    fly_link_t neg;
    fly_link_reset(&neg, 0);
    check(!fly_link_feed(&neg, bad, FLY_LINK_MPRC_LEN, 0) &&
          neg.rx_rejects == 1, "翻转 turn 一位 → CRC 拒绝", "rx_rejects=1");

    memcpy(bad, raw, FLY_LINK_MPRC_LEN);
    bad[0] ^= 0x01;                       // flip one bit of magic
    fly_link_reset(&neg, 0);
    check(!fly_link_feed(&neg, bad, FLY_LINK_MPRC_LEN, 0) &&
          neg.rx_rejects == 1, "翻转 magic 一位 → magic 拒绝", "rx_rejects=1");

    fly_link_reset(&neg, 0);
    check(!fly_link_feed(&neg, raw, 20, 0) && neg.rx_short == 1 &&
          neg.rx_rejects == 0, "20 字节短包 → 记为短包而非坏帧", "rx_short=1");

    fly_game_t fly2;
    fly_game_reset(&fly2, 0x46595A49u);
    const uint8_t keep = (uint8_t)fly2.state.turn;
    check(!fly_game_push_downlink(&fly2, bad, FLY_LINK_MPRC_LEN) &&
          (uint8_t)fly2.state.turn == keep && !fly2.frame_valid,
          "坏帧不会污染已解码的状态", "保持上一帧");

    // ---- 3. 板端造 MPRB，交云端解码器验收 ---------------------------------
    printf("\n[3] fly_link_build_mprb() -> %s（下一步用 stage1_loop.js 解码）\n", mprbOut);
    FILE *g = fopen(mprbOut, "wb");
    if (!g) { printf("!! 无法写 %s\n", mprbOut); free(raw); return 1; }
    uint8_t m[FLY_LINK_MPRB_LEN];
    // Loud music-level features, so the cloud's own decoder sees a real stimulus
    // rather than the silence floor. The values mirror the dose-response split:
    // broadband around -31 dBFS is over the -44 dBFS gate that MAPV=v6 uses.
    fly_mic_features_t loud = {
        .valid = true,
        .rms_dbfs = -31.5f,
        .band_low_db = -51.5f,
        .band_mid_db = -55.6f,
        .band_high_db = -46.9f,
        .peak_dbfs = -12.0f,
    };
    for (uint32_t i = 0; i < 200; ++i) {
        fly_link_build_mprb(m, i + 1, 1000000u + i * 50000u, &loud, 0.0f);
        fwrite(m, 1, sizeof(m), g);
    }
    fclose(g);
    printf("    已写 200 帧 × %u B；第 1 帧 magic=%02x%02x%02x%02x type=%u len=%u crc=%08x\n",
           (unsigned)FLY_LINK_MPRB_LEN,
           (unsigned)m[3], (unsigned)m[2], (unsigned)m[1], (unsigned)m[0],
           (unsigned)m[5], (unsigned)(m[6] | (m[7] << 8)),
           (unsigned)rd_u32(m + 64));
    check(rd_u32(m + 0) == FLY_LINK_MPRB_MAGIC, "magic = 'MPRB'", "0x4D505242");
    check(m[5] == 2, "type = 2 (feature)", "编排层只收 feature");
    check((m[6] | (m[7] << 8)) == FLY_LINK_MPRB_LEN, "payload_len = 68", "固定帧长");
    check(rd_u32(m + 64) == fly_crc32_iso_hdlc(m, 64), "CRC-32 覆盖 [0,64)", "与板端同算法");

    // The four audio numbers must land where the cloud reads them, and each band
    // must be distinct: filling every band with one value (the old placeholder
    // behaviour) would make the orchestrator's band->neuron mapping untestable.
    float f_rms = 0.0f, f_low = 0.0f, f_mid = 0.0f, f_high = 0.0f;
    memcpy(&f_rms, m + 24, 4);
    memcpy(&f_low, m + 28, 4);
    memcpy(&f_mid, m + 32, 4);
    memcpy(&f_high, m + 36, 4);
    printf("    上行特征: rms=%.1f low=%.1f mid=%.1f high=%.1f dBFS\n",
           f_rms, f_low, f_mid, f_high);
    check(fabsf(f_rms - (-31.5f)) < 0.01f, "rms_dbfs 落在偏移 24", "-31.5");
    check(fabsf(f_low - (-51.5f)) < 0.01f, "band_low 落在偏移 28", "-51.5");
    check(fabsf(f_mid - (-55.6f)) < 0.01f, "band_mid 落在偏移 32", "-55.6");
    check(fabsf(f_high - (-46.9f)) < 0.01f, "band_high 落在偏移 36", "-46.9");
    check(f_low != f_mid && f_mid != f_high, "三个频段互相独立", "不是同一个值填三遍");

    free(raw);
    printf("\n%s（%d 项失败）\n", failures ? "=== 有失败项" : "=== 全部通过", failures);
    return failures ? 1 : 0;
}
