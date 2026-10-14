#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Add GATED-LOUDNESS escape variants (env MAPV=v6 / v7) to stage1_loop.js.

Why: the [E] dose-response of MAPV=v4 (onset-only escape) came out NON-monotonic:
  loudness quintile (rms dBFS):  -53/-48.5  -48.5/-36.7  -36.7/-33.5  -33.5/max
  v4 escape%                        1           36          10           9
The trailing-minimum floor catches up during sustained loud music, so onset -> 0
(the fly habituates). Good for a startle reflex, but it means "loud and still
playing" stops producing escape, and the curve is not a clean dose-response.

Meanwhile the [E] table for v0 shows the INPUT loudness separates cleanly:
room floor sits at -53..-48 dBFS, music at -36..-30 dBFS -- a ~20 dB gap that
dbTo01(floorDb=-60) never exploited (silence still got 0.30 drive, which is why
v1/v2/v3 leaked escape into quiet segments).

So: gate the absolute loudness at GATE_DB (default -44 dBFS, i.e. between the room
floor and the music) and span it over GATE_SPAN dB (default 16). Below the gate
the escape port sees EXACTLY zero; above it, a real ramp.

  v6  escape = max(onset, gatedLevel)   -- transient punch + sustained response
  v7  escape = gatedLevel               -- proves the gate alone suffices

Tunables: GATE_DB, GATE_SPAN.

Usage: MAPV=v6 node stage1_loop.js --analyze /root/song.raw
"""
import shutil
import sys

P = sys.argv[1] if len(sys.argv) > 1 else '/root/b_loop/stage1_loop.js'
src = open(P, encoding='utf-8').read()

if 'GATE_SPAN' in src:
    print('  already patched, skipping')
    sys.exit(0)

NL = chr(10)
A1 = '  const onset01 = clamp01(onsetDb / ONSET.ref);'
A2 = "                : (MAPV === 'v5') ? Math.max(audioBand01, onset01)"

ADD_GATE = [
    '  const gateDb = +(process.env.GATE_DB || -44);',
    '  const gateSpan = +(process.env.GATE_SPAN || 16);',
    '  const level01 = clamp01((f.rms_dbfs - gateDb) / gateSpan);',
]
ADD_VARIANTS = [
    "                : (MAPV === 'v6') ? Math.max(onset01, level01)",
    "                : (MAPV === 'v7') ? level01",
]

checks = []
if src.count(A1) != 1:
    checks.append('onset01 anchor: %d' % src.count(A1))
if src.count(A2) != 1:
    checks.append('v5 branch anchor: %d' % src.count(A2))
if checks:
    print('  FAILED - anchors drifted:', '; '.join(checks))
    sys.exit(1)

shutil.copyfile(P, P + '.pregate')
src = src.replace(A1, A1 + NL + NL.join(ADD_GATE), 1)
src = src.replace(A2, A2 + NL + NL.join(ADD_VARIANTS), 1)
open(P, 'w', encoding='utf-8', newline='').write(src)
print('  patched ->', P)
print('  GATE sites:', src.count('GATE_'), '| v6:', ('v6' in src), '| v7:', ('v7' in src))
