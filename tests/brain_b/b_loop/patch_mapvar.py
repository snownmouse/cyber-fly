#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Add a mapping-variant switch (env MAPV) to stage1_loop.js featuresToIntensity.

Why: the mic reports BOTH loudness (rms_dbfs) and spectral shape
(band_low / band_mid / band_high). The current mapping sends low+mid -> AUDIO
(escape) and high+rms -> JO (groom), so a loud, treble-heavy song drives
grooming while the escape port only ever sees the quiet low/mid bands. This
patch lets us re-route loudness vs bands per channel and measure the effect
offline on the archived recording (no hardware, no re-recording).

Variants (env MAPV):
  v0  default, identical to current : audio=[low,mid]        jo=[high,rms]
  v1  both elements onto escape     : audio=[low,mid,rms]    jo=[high,rms]
  v2  loudness->escape, high->groom : audio=[rms]            jo=[high]
  v3  v2 + low/mid also on escape   : audio=[low,mid,rms]    jo=[high]

Usage: MAPV=v2 node stage1_loop.js --analyze /root/song.raw
"""
import shutil
import sys

P = sys.argv[1] if len(sys.argv) > 1 else '/root/b_loop/stage1_loop.js'
src = open(P, encoding='utf-8').read()

OLD_A = '  const audioDb = psumDb([f.band_low, f.band_mid]);'
OLD_B = '  const joDb = psumDb([f.band_high, f.rms_dbfs]);'

NEW_A = (
    "  const MAPV = (process.env.MAPV || 'v0');\n"
    "  const _MAP = {\n"
    "    v0: [[f.band_low, f.band_mid], [f.band_high, f.rms_dbfs]],\n"
    "    v1: [[f.band_low, f.band_mid, f.rms_dbfs], [f.band_high, f.rms_dbfs]],\n"
    "    v2: [[f.rms_dbfs], [f.band_high]],\n"
    "    v3: [[f.band_low, f.band_mid, f.rms_dbfs], [f.band_high]],\n"
    "  }[MAPV] || [[f.band_low, f.band_mid], [f.band_high, f.rms_dbfs]];\n"
    "  const audioDb = psumDb(_MAP[0]);"
)
NEW_B = '  const joDb = psumDb(_MAP[1]);'

if 'MAPV' in src:
    print('  already patched, skipping')
    sys.exit(0)

for name, old in (('audio', OLD_A), ('jo', OLD_B)):
    hits = src.count(old)
    if hits != 1:
        print('  %s: FAILED - matched %d times, file drifted' % (name, hits))
        sys.exit(1)

shutil.copyfile(P, P + '.premap')
src = src.replace(OLD_A, NEW_A).replace(OLD_B, NEW_B)
open(P, 'w', encoding='utf-8', newline='').write(src)
print('  patched ->', P, '| MAPV sites:', src.count('MAPV'))
