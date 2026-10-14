#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bring an older server-side stage1_loop.js up to the current --analyze mode.

Two surgical replacements, each asserted to match exactly once so a drifted
file fails loudly instead of being silently half-patched:

  A) segment the timeline on a +-1 s smoothed stimulus, so threshold noise
     stops shredding the [C] timeline into sub-2 s crumbs;
  B) only suggest a new TH when the quiet and loud read-out distributions are
     actually separable, otherwise keep the current TH (a purely quiet take
     must not "re-calibrate" the thresholds downward).

Usage: python3 patch_stage1_analyze.py [FILE]
"""

import sys

OLD_A = """  const segs = []; let cur = null;
  for (let i = 0; i < n; i++) {
    const a = stim[i] > activeTh;
    if (!cur || cur.active !== a) { if (cur) segs.push(cur); cur = { active: a, rows: [] }; }
    cur.rows.push(rows[i]);
  }"""

NEW_A = """  // \u5e73\u6ed1 \u00b11 s \u540e\u518d\u5206\u6bb5\uff1a\u9608\u503c\u9644\u8fd1\u7684\u9010\u5e27\u566a\u58f0\u4f1a\u628a\u65f6\u95f4\u7ebf\u5207\u6210\u65e0\u6570\u788e\u7247
  const smoothWin = Math.max(1, Math.round(1000 / STEP_MS));
  const stimSm = stim.map((_, i) => {
    let s = 0, c = 0;
    for (let j = Math.max(0, i - smoothWin); j <= Math.min(n - 1, i + smoothWin); j++) { s += stim[j]; c++; }
    return s / c;
  });
  const segs = []; let cur = null;
  for (let i = 0; i < n; i++) {
    const a = stimSm[i] > activeTh;
    if (!cur || cur.active !== a) { if (cur) segs.push(cur); cur = { active: a, rows: [] }; }
    cur.rows.push(rows[i]);
  }"""

OLD_B = """  const suggest = {};
  for (const k of ['escape', 'groom', 'feed']) {
    const q = dist(quietRows.length ? quietRows : rows, k);
    const a = dist(actRows.length ? actRows : rows, k);
    let th = Math.max(q.p99 * 1.5, q.max * 1.15);   // \u9759\u606f\u51e0\u4e4e\u4e0d\u53ef\u80fd\u89e6\u53d1
    if (a.max > 0 && th > a.p90) th = a.p90;        // \u4f46\u522b\u628a\u95e8\u9650\u62ac\u8fc7\u6709\u58f0\u6bb5\u672c\u8eab\u7684\u6c34\u5e73
    suggest[k] = th;
    console.log('    %s %s %s %s |  %s %s %s |  %s',
      k.padEnd(8), fmt(q.p50, 1).padStart(8), fmt(q.p99, 1).padStart(8), fmt(q.max, 1).padStart(8),
      fmt(a.p50, 1).padStart(8), fmt(a.p90, 1).padStart(8), fmt(a.max, 1).padStart(8), fmt(th, 1).padStart(8));
  }"""

NEW_B = """  const suggest = {};
  for (const k of ['escape', 'groom', 'feed']) {
    const q = dist(quietRows.length ? quietRows : rows, k);
    const a = haveActive ? dist(actRows, k) : null;
    const sep = a ? a.p50 - q.max : 0;
    const separable = !!a && sep > Math.max(2, 0.3 * Math.max(1, q.max));
    suggest[k] = separable ? q.max + 0.5 * sep : TH[k];
    console.log('    %s %s %s %s |  %s %s %s | %s | %s',
      k.padEnd(8), fmt(q.p50, 1).padStart(8), fmt(q.p99, 1).padStart(8), fmt(q.max, 1).padStart(8),
      a ? fmt(a.p50, 1).padStart(8) : '       -', a ? fmt(a.p90, 1).padStart(8) : '       -',
      a ? fmt(a.max, 1).padStart(8) : '       -',
      (haveActive ? (separable ? '\u6709' : '\u65e0') : '\u65e0\u6bb5').padStart(4),
      (separable ? fmt(suggest[k], 1) : (TH[k] + '(\u4fdd\u6301)')).padStart(8));
  }"""

PATCHES = [("A/smoothing", OLD_A, NEW_A), ("B/TH-recalibration", OLD_B, NEW_B)]


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "/root/b_loop/stage1_loop.js"
    src = open(path, encoding="utf-8").read()
    before = len(src)

    for name, old, new in PATCHES:
        hits = src.count(old)
        if hits == 0 and src.count(new) == 1:
            print(f"  {name}: already applied, skipping")
            continue
        if hits != 1:
            print(f"  {name}: FAILED - matched {hits} times, file has drifted")
            return 1
        src = src.replace(old, new)
        print(f"  {name}: applied")

    if src == open(path, encoding="utf-8").read():
        print("nothing to do")
        return 0

    open(path, "w", encoding="utf-8", newline="").write(src)
    print(f"  bytes {before} -> {len(src)}")
    print(f"  smoothWin={src.count('smoothWin')} separable={src.count('separable')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
