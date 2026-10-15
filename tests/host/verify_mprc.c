// SPDX-License-Identifier: Apache-2.0
// 互操作验证：用【板端同一份 C 解码器】(fly_game.c) 解码【服务器 Node 编排层】产出的
// 真实 MPRC 下行流（stage1_loop.js --emit-dl）。用来证明"服务器 → 板子"字节级兼容，
// 而不是"两边各自自测都过"。
//
// 用法: ./verify_mprc <downlink.bin> [--dump N]
// 退出码: 0 = 全部通过（含反例被正确拒绝）；非 0 = 有问题
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fly_game.h"

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "用法: %s <downlink.bin> [--dump N]\n", argv[0]); return 2; }
    int dump = 0;
    for (int i = 2; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--dump")) dump = atoi(argv[i + 1]);

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 2; }

    uint8_t buf[FLY_MPRC_LEN];
    int n = 0, ok = 0, bad = 0, first_bad = -1, first_bad_code = 0;
    unsigned n_escape = 0, n_left = 0, n_right = 0, n_groom = 0, n_feed = 0;
    int turn_min = 9999, turn_max = -9999;
    uint32_t seq_first = 0, seq_last = 0;
    int seq_monotonic = 1;

    printf("=== 用板端解码器 (fly_game.c) 解码服务器下行流 %s ===\n", argv[1]);
    while (fread(buf, 1, FLY_MPRC_LEN, fp) == FLY_MPRC_LEN) {
        fly_state_t st;
        int rc = fly_state_decode(buf, FLY_MPRC_LEN, &st);
        if (rc != 0) {
            bad++;
            if (first_bad < 0) { first_bad = n; first_bad_code = rc; }
        } else {
            ok++;
            uint32_t seq = le_u32(buf + 8);
            if (n == 0) seq_first = seq;
            else if (seq != seq_last + 1) seq_monotonic = 0;
            seq_last = seq;

            if (st.flags & FLY_FLAG_ESCAPE) n_escape++;
            if (st.flags & FLY_FLAG_LEFT)   n_left++;
            if (st.flags & FLY_FLAG_RIGHT)  n_right++;
            if (st.flags & FLY_FLAG_GROOM)  n_groom++;
            if (st.flags & FLY_FLAG_FEED)   n_feed++;
            if (st.turn < turn_min) turn_min = st.turn;
            if (st.turn > turn_max) turn_max = st.turn;

            if (dump && (n < dump || (st.flags & (FLY_FLAG_ESCAPE | FLY_FLAG_LEFT | FLY_FLAG_RIGHT)))) {
                printf("  #%-4d seq=%-4u turn=%4d thrust=%3u motor=%3u arousal=%3u flags=0x%02x"
                       " groom=%3u feed=%3u link_q=%3u brain_load=%3u\n",
                       n, seq, st.turn, st.thrust, st.motor, st.arousal, st.flags,
                       st.groom, st.feed, st.link_q, st.brain_load);
            }
        }
        n++;
    }
    fclose(fp);

    printf("\n总帧数 %d 帧 x %d B = %d B\n", n, FLY_MPRC_LEN, n * FLY_MPRC_LEN);
    printf("解码成功 %d / 失败 %d\n", ok, bad);
    if (bad) printf("首个失败帧 #%d，错误码 %d\n", first_bad, first_bad_code);
    printf("seq 连续: %s（%u → %u）\n", seq_monotonic ? "是" : "否", seq_first, seq_last);
    printf("行为帧: 逃逸 %u  左转 %u  右转 %u  梳理 %u  进食 %u\n",
           n_escape, n_left, n_right, n_groom, n_feed);
    printf("turn 范围 [%d, %d]（i8，/127 后 [-1,1]）\n", turn_min, turn_max);

    // --- 反例：把第 0 帧的 turn 字节改一位，CRC 必须拒绝 ---
    fp = fopen(argv[1], "rb");
    int neg_ok = 0;
    if (fp && fread(buf, 1, FLY_MPRC_LEN, fp) == FLY_MPRC_LEN) {
        uint8_t save = buf[16];
        buf[16] = (uint8_t)(save ^ 0x01);
        fly_state_t st;
        neg_ok = (fly_state_decode(buf, FLY_MPRC_LEN, &st) == -7);   // -7 = CRC 不符
        buf[16] = save;
        uint8_t bad_magic[FLY_MPRC_LEN];
        memcpy(bad_magic, buf, FLY_MPRC_LEN);
        bad_magic[0] ^= 0xFF;
        int m = fly_state_decode(bad_magic, FLY_MPRC_LEN, &st);
        printf("反例: 改 turn 一位 → %s；改 magic → %s\n",
               neg_ok ? "被拒(CRC) OK" : "未拒 !!",
               (m == -3) ? "被拒(magic) OK" : "未拒 !!");
        if (m != -3) neg_ok = 0;
    }
    if (fp) fclose(fp);

    // --- 板端自带假下行生成器：产出的帧必须自校验通过 ---
    static fly_game_t fly;
    fly_game_reset(&fly, 0x46595A49u);
    int fake_ok = 0;
    for (int i = 0; i < 300; ++i) {
        fly.tick = (uint32_t)i;
        fly_fake_drive(&fly);
        if (!fly.frame_valid) { fake_ok = -1; break; }
    }
    if (fake_ok == 0) fake_ok = 1;
    printf("板端假下行生成器 300 帧自校验: %s（末帧 seq=%u flags=0x%02x turn=%d）\n",
           fake_ok == 1 ? "OK" : "FAIL", fly.seq, fly.state.flags, fly.state.turn);

    int pass = (n > 0 && bad == 0 && neg_ok && fake_ok == 1 && seq_monotonic);
    printf("\n%s\n", pass ? "INTEROP PASS" : "INTEROP FAIL");
    return pass ? 0 : 1;
}
