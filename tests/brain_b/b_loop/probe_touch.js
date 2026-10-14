#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// 定点验证：三条输入通道的【左右不对称】→ DNa01/DNa02 转向读出
// 目的：查清 v4 子回路的转向通路是否可做「双极」控制（左转/右转都要有）。
//
// 背景：首轮单种子探针（probe_touch.js 初版）显示
//   TOUCH_L 300 → DNa02_L 38 且 DNa01_R 14；而 TOUCH_R 300 → 全 0。
// 若那是真的 ⇒ 右侧触觉在 v4 里没有下游，机器人只能单向转。
// 本轮用多随机种子复核，并把 AUDIO / JO 也一并扫，找可用的差信号。
'use strict';
const fs = require('fs');
const path = require('path');
const REPO = process.env.FLYLAB || '/root/flywire-fly-lab';
const { ConnectomeBrain } = require(path.join(REPO, 'dodge/brain.js'));
const data = JSON.parse(fs.readFileSync(path.join(REPO, 'results/dodge/subcircuit_v4.json'), 'utf8'));

const CH = {
  AUDIO: ['AUDIO_left', 'AUDIO_right'],
  JO: ['JO_left', 'JO_right'],
  TOUCH: ['TOUCH_left', 'TOUCH_right'],
};
const OUT = ['DNa01_left', 'DNa01_right', 'DNa02_left', 'DNa02_right', 'DNp01_left', 'DNp01_right'];
const SEEDS = [1, 3, 7];
const MEAS_STEPS = 6000;                 // 0.6 s 生物时间

function trial(chName, L, R, seed) {
  const b = new ConnectomeBrain(data, seed);
  b.reset(); b.reseed(seed);
  for (const k of Object.keys(CH)) for (const g of CH[k]) b.setRate(g, 0);
  b.run(8000);
  b.readRates(OUT, 1);
  b.setRate(CH[chName][0], L); b.setRate(CH[chName][1], R);
  const acc = {}; for (const g of OUT) acc[g] = 0;
  const N = 12;
  for (let i = 0; i < N; i++) {
    b.run(MEAS_STEPS / N);
    const r = b.readRates(OUT, MEAS_STEPS / N);
    for (const g of OUT) acc[g] += r[g] / N;
  }
  return acc;
}
const mean = (a) => a.reduce((s, x) => s + x, 0) / a.length;

console.log('每格 = 3 个种子(1/3/7)平均（单位：每神经元 Hz）');
for (const ch of Object.keys(CH)) {
  console.log(`\n=== ${ch}  (L/R 组规模 ${data.groups[CH[ch][0]].length}/${data.groups[CH[ch][1]].length}) ===`);
  console.log('   L/R 驱动   | DNa01 L  R | DNa02 L  R | DNp01 L   R | steer=(01L+02L)-(01R+02R)');
  for (const [L, R] of [[0, 0], [300, 0], [0, 300], [300, 300], [150, 300], [300, 150]]) {
    const runs = SEEDS.map((s) => trial(ch, L, R, s));
    const m = (g) => mean(runs.map((r) => r[g]));
    const steers = runs.map((r) => (r.DNa01_left + r.DNa02_left) - (r.DNa01_right + r.DNa02_right));
    console.log(`   ${String(L).padStart(4)}/${String(R).padEnd(4)} |`
      + `   ${m('DNa01_left').toFixed(1).padStart(4)} ${m('DNa01_right').toFixed(1).padStart(5)} |`
      + `  ${m('DNa02_left').toFixed(1).padStart(4)} ${m('DNa02_right').toFixed(1).padStart(4)} |`
      + `  ${m('DNp01_left').toFixed(0).padStart(4)} ${m('DNp01_right').toFixed(0).padStart(4)} |`
      + `  ${mean(steers).toFixed(1).padStart(6)}  (±${(Math.max(...steers) - Math.min(...steers)).toFixed(1)})`);
  }
}
console.log('\n判读：若某一通道在 L/R 互换时 steer 变号且量级相当 ⇒ 该通道可做双极转向；');
console.log('      若只有单侧有输出 ⇒ 该通道只能做单极（单向）转向。');
