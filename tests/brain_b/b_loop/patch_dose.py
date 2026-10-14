#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Add an [E] loudness dose-response table to stage1_loop.js analyze().

Why: analyze() segments quiet/loud with stim = max(intensity.*). Once MAPV=v4/v5
redefine audio as an ONSET (delta) signal, stim no longer tracks loudness, so the
"[C] auto-segment" quiet/loud labels become meaningless for onset runs. [E] sidesteps
that by binning frames directly on the absolute rms_dbfs recorded in each frame,
independent of any mapping. It answers the question that matters:
  "as the room gets louder, does escape rise?"  (startle should; rest stays 0)

NOTE: all newlines are built with chr(10) and the JS payload contains no backslash,
so no escape-mangling can corrupt the generated string literals.

Usage: node stage1_loop.js --analyze /root/song.raw   ([E] prints after [C])
"""
import shutil
import sys

P = sys.argv[1] if len(sys.argv) > 1 else '/root/b_loop/stage1_loop.js'
src = open(P, encoding='utf-8').read()

MARK = '[E] \u54cd\u5ea6\u5242\u91cf'          # '[E] 响度剂量' guard
ANCHOR = '  // [D] TH \u91cd\u6807\u5b9a'       # '  // [D] TH 重标定'

if MARK in src:
    print('  already patched, skipping')
    sys.exit(0)
if src.count(ANCHOR) != 1:
    print('  FAILED - [D] anchor matched %d times' % src.count(ANCHOR))
    sys.exit(1)

NL = chr(10)
JS_LINES = [
    '',
    '  // [E] \u54cd\u5ea6\u5242\u91cf-\u53cd\u5e94\uff1a\u76f4\u63a5\u6309 rms_dbfs \u4e94\u5206\u4f4d\u5206\u7bb1\uff08\u4e0e stim \u65e0\u5173\uff09\u3002',
    '  //      \u7528\u4e8e onset \u6620\u5c04\uff1a\u6b64\u65f6 stim \u5df2\u4e0d\u518d\u4ee3\u8868\u54cd\u5ea6\uff0c[C] \u7684\u9759\u606f/\u6709\u58f0\u6807\u7b7e\u4e0d\u53ef\u7528\u3002',
    '  const rmsArr = frames.map((f) => f.rms_dbfs);',
    '  const srtR = [...rmsArr].sort((a, b) => a - b);',
    '  const cuts = [pctl(srtR, 0.2), pctl(srtR, 0.4), pctl(srtR, 0.6), pctl(srtR, 0.8)];',
    '  const dBins = [[], [], [], [], []];',
    '  for (let i = 0; i < n; i++) { let b = 0; while (b < 4 && rmsArr[i] >= cuts[b]) b++; dBins[b].push(rows[i]); }',
    "  console.log('');",
    "  console.log('[E] \u54cd\u5ea6\u5242\u91cf-\u53cd\u5e94\uff08\u6309 rms_dbfs \u4e94\u5206\u4f4d\uff1b\u9759\u606f=\u6700\u5c0f\u4e00\u6863\uff0c\u5f88\u54cd=\u6700\u5927\u4e00\u6863\uff09');",
    "  console.log('      \u54cd\u5ea6\u533a\u95f4(dBFS)        \u5e27   \u9003\u9038% \u8f6c\u5411% \u68b3\u7406% \u8fdb\u98df% \u9759\u606f%');",
    '  for (let b = 0; b < 5; b++) {',
    '    const rs = dBins[b]; if (!rs.length) continue;',
    '    const s = shares(rs);',
    "    const lo = (b === 0) ? '(\u6700\u5c0f)' : fmt(cuts[b - 1], 1);",
    "    const hi = (b === 4) ? '(\u6700\u5927)' : fmt(cuts[b], 1);",
    "    console.log('    %s ~ %s  %s  %s  %s  %s  %s  %s',",
    '      String(lo).padStart(7), String(hi).padStart(7), String(s.n).padStart(6),',
    '      pct(s.esc, s.n), pct(s.dir, s.n), pct(s.groom, s.n), pct(s.feed, s.n), pct(s.rest, s.n));',
    '  }',
]
BLOCK = NL.join(JS_LINES) + NL + NL

shutil.copyfile(P, P + '.predose')
src = src.replace(ANCHOR, BLOCK + ANCHOR, 1)
open(P, 'w', encoding='utf-8', newline='').write(src)
print('  patched ->', P, '| [E] sites:', src.count(MARK))
