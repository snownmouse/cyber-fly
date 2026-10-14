#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Add a Delta-rms ONSET detector as an escape driver (env MAPV=v4 / v5).

Context: the MAPV sweep (patch_mapvar.py) showed that routing loudness
(rms_dbfs) straight into the escape/AUDIO port makes "listening -> escape"
work, BUT absolute loudness also fires during silent stretches (room floor +
brain self-activity lift the read-out to ~13-16 Hz, straddling TH.escape=15;
no threshold splits quiet from music because the two distributions overlap).

Fix (biologically right): escape is a STARTLE REFLEX. It should be driven by
the *onset* of loudness (Delta-rms), not by how loud things are. We take a
trailing-window minimum of rms_dbfs as the local floor and drive escape with
onset = max(0, rms - floor):
  - steady quiet     -> onset ~ 0        (no escape in silence)
  - quiet -> loud step -> onset = big    (startle fires)
  - sustained loud   -> floor catches up -> onset -> 0 (habituation), correct

New variants (env MAPV):
  v4  escape = onset(Delta-rms);                 groom = high
  v5  escape = max(onset, low+mid);              groom = high
Tunables (env): ONSET_W (trailing window frames, default 8 = 400 ms),
                ONSET_REF (dB rise = full intensity, default 20).

Usage: MAPV=v4 node stage1_loop.js --analyze /root/song.raw
"""
import re
import shutil
import sys

P = sys.argv[1] if len(sys.argv) > 1 else '/root/b_loop/stage1_loop.js'
src = open(P, encoding='utf-8').read()

if 'ONSET' in src:
    print('  already patched, skipping')
    sys.exit(0)

# ---- 0. 依赖前置：MAPV 开关必须已在（由 patch_mapvar.py 引入）
if 'MAPV' not in src:
    print('  FAILED - MAPV switch missing; run patch_mapvar.py first')
    sys.exit(1)

# ---- 1. 扩展 _MAP：加入 v4/v5 两项（escape 由 onset 特殊处理，_MAP[0] 供 v5 的频段用）
OLD_MAP_TAIL = "  }[MAPV] || [[f.band_low, f.band_mid], [f.band_high, f.rms_dbfs]];\n"
NEW_MAP_TAIL = (
    "    v4: [[f.band_low, f.band_mid], [f.band_high]],   // escape=onset(Δrms); groom=high\n"
    "    v5: [[f.band_low, f.band_mid], [f.band_high]],   // escape=max(onset,low+mid); groom=high\n"
    "  }[MAPV] || [[f.band_low, f.band_mid], [f.band_high, f.rms_dbfs]];\n"
)

# ---- 2. onset 计算 + audio01（替换 audioDb 定义行；该行可能带尾注释，用正则兜住）
OLD_A_RE = re.compile(r"  const audioDb = psumDb\(_MAP\[0\]\);[^\n]*\n")
NEW_A = (
    "  // Δrms 起始检测：以近 ONSET_W 帧的最小 rms 为局部地板，onset = max(0, rms - floor)\n"
    "  const _of = ONSET.rms.length ? Math.min.apply(null, ONSET.rms) : f.rms_dbfs;\n"
    "  const onsetDb = Math.max(0, f.rms_dbfs - _of);\n"
    "  ONSET.rms.push(f.rms_dbfs); if (ONSET.rms.length > ONSET.w) ONSET.rms.shift();\n"
    "  const onset01 = clamp01(onsetDb / ONSET.ref);\n"
    "  const audioBand01 = dbTo01(psumDb(_MAP[0]));\n"
    "  const audio01 = (MAPV === 'v4') ? onset01\n"
    "                : (MAPV === 'v5') ? Math.max(audioBand01, onset01)\n"
    "                : audioBand01;   // 低频+中频 = 听觉（v0..v3）\n"
)

# ---- 3. 返回体：audio 改用 audio01
OLD_RET_RE = re.compile(r"(\n    audio: )dbTo01\(audioDb\),")

# ---- 4. ONSET 状态 + reset（插在 featuresToIntensity 之前）
ANCHOR_FN = "function featuresToIntensity(f) {\n"
NEW_FN_PRE = (
    "// Δrms 起始检测状态（惊跳反射驱动量）。模块级：一次 node 进程只跑一条序列，\n"
    "// 状态从空开始即可；runSequence 显式 reset 以兼容长驻/多次调用。\n"
    "const ONSET = { w: +(process.env.ONSET_W || 8), ref: +(process.env.ONSET_REF || 20), rms: [] };\n"
    "function resetOnset() { ONSET.rms.length = 0; }\n"
    "\n"
)

# ---- 5. runSequence 开头 reset
ANCHOR_RUNSEQ = "function runSequence(data, frames, seed, strategy, watchdog) {\n"

# ------------------------------------------------------------------ 断言 + 应用
checks = []
if src.count(OLD_MAP_TAIL) != 1:
    checks.append('_MAP tail: %d' % src.count(OLD_MAP_TAIL))
if len(OLD_A_RE.findall(src)) != 1:
    checks.append('audioDb def: %d' % len(OLD_A_RE.findall(src)))
if len(OLD_RET_RE.findall(src)) != 1:
    checks.append('return audio: %d' % len(OLD_RET_RE.findall(src)))
if src.count(ANCHOR_FN) != 1:
    checks.append('featuresToIntensity: %d' % src.count(ANCHOR_FN))
if src.count(ANCHOR_RUNSEQ) != 1:
    checks.append('runSequence: %d' % src.count(ANCHOR_RUNSEQ))
if checks:
    print('  FAILED - anchors drifted:', '; '.join(checks))
    sys.exit(1)

shutil.copyfile(P, P + '.preonset')
src = src.replace(OLD_MAP_TAIL, NEW_MAP_TAIL)
src = OLD_A_RE.sub(NEW_A, src, count=1)
src = OLD_RET_RE.sub(r"\1audio01,", src, count=1)
src = src.replace(ANCHOR_FN, NEW_FN_PRE + ANCHOR_FN)
src = src.replace(ANCHOR_RUNSEQ, ANCHOR_RUNSEQ + "  resetOnset();\n")
open(P, 'w', encoding='utf-8', newline='').write(src)

print('  patched ->', P)
print('  ONSET sites:', src.count('ONSET'), '| MAPV sites:', src.count('MAPV'))
print('  v4/v5 in map:', ('v4:' in src and 'v5:' in src))
