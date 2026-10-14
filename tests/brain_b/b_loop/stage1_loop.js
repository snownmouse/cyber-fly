#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// 任务 B 阶段 1 · 云端编排层（特征 → v4 子回路 → 下行命令帧 MPRC）
//
// 一个文件打通全部模式：
//   node stage1_loop.js --replay              回放任务 A 存档（文本特征，安静房间）
//   node stage1_loop.js --synth               合成"有声"上行二进制帧 → 全链闭环自测（不经网络）
//   node stage1_loop.js --emit-dl FILE        把 --synth 跑出的真实 MPRC 下行流写成文件（喂给 cyber_fly）
//   node stage1_loop.js --grid                死区看门狗验证（二维强度网格）
//   node stage1_loop.js --selftest            MPRC/MPRB 编解码自检
//   node stage1_loop.js --emit FILE           生成合成上行二进制帧（68 B/帧）
//   node stage1_loop.js --blast HOST:PORT     把合成帧经真实 UDP 发出去（配合 --serve 联测）
//   node stage1_loop.js --serve               常驻：UDP 收 68 B → 跑脑 → 回 32 B MPRC（真·云端大脑）
//
// 设计要点见 tests/brain_b/STAGE1-SENSOR-TO-BRAIN-DESIGN.md：
//   * 端口只按【组名】寻址，绝不按下标 —— 换 v4 → v5 → 定制 vS 时本文件不用改。
//   * 输入侧允许叠加（生物学如此），网络自身产生的互相压制交给网络。
//   * 读出前只做「驱动权重」分配（优先级加权叠加 = 默认策略）。
//   * 死区看门狗：连续 N 帧"全静且输入非零" → 临时切硬 WTA（只留最强通道）并放大，兜底防卡死。
//
// ★ 2026-10-06 结构性更正（本轮实测，shallow 探针 + CSR 连边双重确认）：
//   ① 方向【不能】由脑给。v4 里 DNa01/DNa02 没有任何命名感觉输入（入边只来自 MDN/DNa 自身，
//      见 probe_touch.js 的 --edges 输出），"TOUCH→DNa 转向" 只经未命名中间神经元、且实测
//      只有单侧有效（TOUCH_left 有效 / TOUCH_right 全 0，跨 3 种子稳定）⇒ v4 给不出左右方向。
//      ⇒ 转向改由【上行帧里的带符号侧向加速度】直接给出。板端本就有 IMU，这条路也不该
//        等 400 ms 的云端往返（与设计文档"板端快反射必须留在板上"一致）。
//   ② AUDIO 是"只吃右侧输入"的通道（仅 L 驱动 → 全 0；仅 R → DNp01_R 150；双侧 → 163）。
//      双侧驱动仍是最优，保留。
//   ③ DNp01（逃逸）的入边构成：LPLC2 94 + LC4 54 vs AUDIO 仅 12（左右类似）
//      ⇒ 逃逸回路的"正牌"输入是视觉 loom 探测器，麦克风是少数派。这是加相机子板的强理由。
'use strict';
const fs = require('fs');
const path = require('path');
const dgram = require('dgram');

const REPO = process.env.FLYLAB || '/root/flywire-fly-lab';
const DATA_PATH = process.env.FLYDATA || path.join(REPO, 'results/dodge/subcircuit_v4.json');
const PROBE_LOG = process.env.PROBELOG || '/root/probe.log';

const { ConnectomeBrain } = require(path.join(REPO, 'dodge/brain.js'));

// ---------------------------------------------------------------- 1. CRC-32/ISO-HDLC
// 与板端 probe_common.c、服务端 udp_probe_server.py 逐位一致
// （多项式 0xEDB88320，反射，初值/终值异或 0xFFFFFFFF）
const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c;
  }
  return t;
})();
function crc32(buf, len = buf.length) {
  let c = -1;
  for (let i = 0; i < len; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

// ---------------------------------------------------------------- 2. 端口表（按组名）
const IN_PORT = {
  audio: { L: 'AUDIO_left', R: 'AUDIO_right' }, // 江氏器听觉亚类 JO-A*/JO-B* → 巨纤维 1 跳（结构已确认：AUDIO→DNp01）
  jo:    { L: 'JO_left',    R: 'JO_right'    }, // 江氏器风/偏转亚类
  touch: { L: 'TOUCH_left', R: 'TOUCH_right' }, // 头部刚毛
};
const OUT_PORT = {
  escape: { L: 'DNp01_left', R: 'DNp01_right' },
  steer1: { L: 'DNa01_left', R: 'DNa01_right' },  // ⚠️ 诊断用：v4 中它们无命名感觉输入，不做转向控制
  steer2: { L: 'DNa02_left', R: 'DNa02_right' },  // ⚠️ 同上
  back:   { L: 'MDN_left',   R: 'MDN_right'   },
  feed:   { L: 'MN9_left',   R: 'MN9_right'   },
  groom:  { L: 'aDN1_left',  R: 'aDN1_right'  },
};
function assertPorts(data) {
  const missing = [];
  const check = (map) => {
    for (const k of Object.keys(map))
      for (const s of ['L', 'R'])
        if (!data.groups || !data.groups[map[k][s]]) missing.push(map[k][s]);
  };
  check(IN_PORT); check(OUT_PORT);
  if (missing.length) throw new Error('子回路缺少端口组：' + missing.join(', '));
  const sizes = {};
  for (const map of [IN_PORT, OUT_PORT])
    for (const k of Object.keys(map)) {
      const L = data.groups[map[k].L].length, R = data.groups[map[k].R].length;
      sizes[k] = `${L}/${R}`;
    }
  return sizes;
}

// ---------------------------------------------------------------- 3. 通道仲裁 + 看门狗
// 生物学依据：输入端多通道电流本就线性叠加（叠加是真的）；LIF 的发放阈值 + 网络自身的
// 抑制会让输出互相压制。⇒ 不干预网络，只在读出前把「驱动权重」按行为优先级分配。
const PRIORITY = { audio: 1.00, touch: 0.85, jo: 0.45 };
const LAMBDA = 0.15;     // 落选通道保底分量（软 WTA）
const GAMMA = 3.0;       // 落选通道衰减指数
const HYSTERESIS = 1.25; // 迟滞：新科须超过当前注意力通道 25% 才换台，防抖

const ARBITERS = {
  sum(drives) { return { ...drives }; },
  wta(drives) {
    let best = null, bv = -Infinity;
    for (const k of Object.keys(drives)) { const v = drives[k] * PRIORITY[k]; if (v > bv) { bv = v; best = k; } }
    const out = {};
    for (const k of Object.keys(drives)) out[k] = (k === best) ? drives[k] : 0;
    return out;
  },
  soft_wta(drives, st) {
    const score = {}; let top = null, tv = 0;
    for (const k of Object.keys(drives)) { score[k] = drives[k] * PRIORITY[k]; if (score[k] > tv) { tv = score[k]; top = k; } }
    if (tv <= 1e-6) { st.attending = null; return { ...drives }; }
    if (st.attending && st.attending !== top) {
      const cur = score[st.attending] || 0;
      if (!(tv > cur * HYSTERESIS)) top = st.attending;
    }
    st.attending = top;
    const out = {};
    for (const k of Object.keys(drives)) {
      if (k === top) { out[k] = drives[k]; continue; }
      const r = score[k] / tv;
      out[k] = drives[k] * (LAMBDA + (1 - LAMBDA) * Math.pow(r, GAMMA));
    }
    return out;
  },
  round_robin(drives, st) {
    const order = ['audio', 'touch', 'jo'];
    const slot = order[Math.floor(st.t / 1000) % order.length];
    const out = {};
    for (const k of Object.keys(drives)) out[k] = (k === slot) ? drives[k] : 0;
    return out;
  },
};

// 死区看门狗（默认开）：连续 window 帧"全静且输入非零" → 临时硬 WTA（只留最强通道，其余置 0）
// 并 ×boost，持续 hold 帧。只放大不压制【无效】——死区根因是交叉抑制（见 stepFrame 内注释）。
const WATCH = { window: 8, inputEps: 0.20, boost: 2.0, hold: 16 };

// ---------------------------------------------------------------- 4. 特征 → 驱动强度
const CFG = { floorDb: -60, refDb: -20, rMaxHz: 300, touchRestG: 1.0, touchSpanG: 0.8, latSpanG: 0.6, turnDeadband: 0.25 };
const clamp01 = (x) => x < 0 ? 0 : (x > 1 ? 1 : x);
const clamp = (x, a, b) => x < a ? a : (x > b ? b : x);
const psumDb = (list) => 10 * Math.log10(list.reduce((a, d) => a + Math.pow(10, d / 10), 0));
const dbTo01 = (db) => clamp01((db - CFG.floorDb) / (CFG.refDb - CFG.floorDb));

// 68 字节上行帧字段 → 归一化通道强度
//   audio/jo/touch : 强度 ∈ [0,1]（标量，双侧同幅驱动）
//   lat            : 带符号侧向 ∈ [-1,1]（ax 为正 = 从右侧被推）—— ★ 方向就来自这里
// Δrms 起始检测状态（惊跳反射驱动量）。模块级：一次 node 进程只跑一条序列，
// 状态从空开始即可；runSequence 显式 reset 以兼容长驻/多次调用。
const ONSET = { w: +(process.env.ONSET_W || 8), ref: +(process.env.ONSET_REF || 20), rms: [] };
function resetOnset() { ONSET.rms.length = 0; }

function featuresToIntensity(f) {
  const MAPV = (process.env.MAPV || 'v0');
  const _MAP = {
    v0: [[f.band_low, f.band_mid], [f.band_high, f.rms_dbfs]],
    v1: [[f.band_low, f.band_mid, f.rms_dbfs], [f.band_high, f.rms_dbfs]],
    v2: [[f.rms_dbfs], [f.band_high]],
    v3: [[f.band_low, f.band_mid, f.rms_dbfs], [f.band_high]],
    v4: [[f.band_low, f.band_mid], [f.band_high]],   // escape=onset(Δrms); groom=high
    v5: [[f.band_low, f.band_mid], [f.band_high]],   // escape=max(onset,low+mid); groom=high
  }[MAPV] || [[f.band_low, f.band_mid], [f.band_high, f.rms_dbfs]];
  // Δrms 起始检测：以近 ONSET_W 帧的最小 rms 为局部地板，onset = max(0, rms - floor)
  const _of = ONSET.rms.length ? Math.min.apply(null, ONSET.rms) : f.rms_dbfs;
  const onsetDb = Math.max(0, f.rms_dbfs - _of);
  ONSET.rms.push(f.rms_dbfs); if (ONSET.rms.length > ONSET.w) ONSET.rms.shift();
  const onset01 = clamp01(onsetDb / ONSET.ref);
  const gateDb = +(process.env.GATE_DB || -44);
  const gateSpan = +(process.env.GATE_SPAN || 16);
  const level01 = clamp01((f.rms_dbfs - gateDb) / gateSpan);
  const audioBand01 = dbTo01(psumDb(_MAP[0]));
  const audio01 = (MAPV === 'v4') ? onset01
                : (MAPV === 'v5') ? Math.max(audioBand01, onset01)
                : (MAPV === 'v6') ? Math.max(onset01, level01)
                : (MAPV === 'v7') ? level01
                : audioBand01;   // 低频+中频 = 听觉（v0..v3）
  const joDb = psumDb(_MAP[1]);            // 高频+总 RMS = 风/机械扰动
  const accelMag = Math.sqrt(f.accel[0] * f.accel[0] + f.accel[1] * f.accel[1] + f.accel[2] * f.accel[2]);
  return {
    audio: audio01,
    jo: dbTo01(joDb),
    touch: clamp01((accelMag - CFG.touchRestG) / CFG.touchSpanG), // |a|=1g 静止 → 0
    lat: clamp(f.accel[0] / CFG.latSpanG, -1, 1),                 // 右正；静止 ax=0 → 0
  };
}

// 二进制 68 B 帧解码（实机/网络走这条）
const MAGIC_UP = 0x4D505242;   // "MPRB"
function decodeFeature68(buf) {
  if (buf.length < 68) throw new Error('短帧');
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (dv.getUint32(0, true) !== MAGIC_UP) throw new Error('magic != MPRB');
  if (crc32(buf, 64) !== dv.getUint32(64, true)) throw new Error('CRC 不符');
  return {
    seq: dv.getUint32(8, true), send_ts_us: dv.getUint32(12, true),
    rms_dbfs: dv.getFloat32(24, true), band_low: dv.getFloat32(28, true),
    band_mid: dv.getFloat32(32, true), band_high: dv.getFloat32(36, true),
    accel: [dv.getFloat32(40, true), dv.getFloat32(44, true), dv.getFloat32(48, true)],
  };
}
// 二进制 68 B 帧编码（合成数据用；布局严格对齐 probe_common.h / udp_probe_server.py）
function encodeFeature68(o) {
  const b = Buffer.alloc(68);
  b.writeUInt32LE(MAGIC_UP, 0);
  b.writeUInt8(1, 4);            // version
  b.writeUInt8(2, 5);            // type = feature
  b.writeUInt16LE(68, 6);        // payload_len
  b.writeUInt32LE(o.seq >>> 0, 8);
  b.writeUInt32LE(o.send_ts_us >>> 0, 12);
  b.writeUInt32LE(0, 16);        // echo_ts_us
  b.writeUInt16LE(16000, 20);    // sample_rate
  b.writeUInt8(1, 22);           // channels
  b.writeUInt8(0x3, 23);         // flags: AUDIO_VALID | IMU_VALID
  b.writeFloatLE(o.rms_dbfs, 24);
  b.writeFloatLE(o.band_low, 28);
  b.writeFloatLE(o.band_mid, 32);
  b.writeFloatLE(o.band_high, 36);
  b.writeFloatLE(o.accel[0], 40); b.writeFloatLE(o.accel[1], 44); b.writeFloatLE(o.accel[2], 48);
  b.writeFloatLE(0, 52); b.writeFloatLE(0, 56); b.writeFloatLE(0, 60);
  b.writeUInt32LE(crc32(b, 64), 64);
  return b;
}

// ---------------------------------------------------------------- 5. 下行命令帧 MPRC（32 B）
function encodeDownlink(o) {
  const b = Buffer.alloc(32);
  b.writeUInt32LE(0x4352504D, 0);   // magic 'MPRC'
  b.writeUInt8(1, 4);               // version
  b.writeUInt8(4, 5);               // type = 4
  b.writeUInt16LE(32, 6);           // payload_len
  b.writeUInt32LE(o.seq >>> 0, 8);
  b.writeUInt32LE(o.echo_ts_us >>> 0, 12);
  b.writeInt8(Math.max(-128, Math.min(127, Math.round(o.turn * 127))), 16);
  b.writeUInt8(Math.round(clamp01(o.thrust) * 255), 17);
  b.writeUInt8(Math.round(clamp01(o.motor) * 255), 18);
  b.writeUInt8(Math.round(clamp01(o.arousal) * 255), 19);
  b.writeUInt8(o.flags & 0xff, 20);
  b.writeUInt8(Math.round(clamp01(o.groom) * 255), 21);
  b.writeUInt8(Math.round(clamp01(o.feed) * 255), 22);
  b.writeUInt8(Math.round(clamp01(o.link_q) * 255), 23);
  b.writeUInt8(Math.round(clamp01(o.brain_load) * 255), 24);
  b.writeUInt32LE(crc32(b, 28), 28);
  return b;
}
function decodeDownlink(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (buf.length !== 32) throw new Error('长度 != 32');
  if (dv.getUint32(0, true) !== 0x4352504D) throw new Error('magic != MPRC');
  if (dv.getUint8(4) !== 1 || dv.getUint8(5) !== 4 || dv.getUint16(6, true) !== 32) throw new Error('头部不符');
  if (crc32(buf, 28) !== dv.getUint32(28, true)) throw new Error('CRC 不符');
  return {
    seq: dv.getUint32(8, true), echo_ts_us: dv.getUint32(12, true),
    turn: dv.getInt8(16) / 127, thrust: dv.getUint8(17) / 255, motor: dv.getUint8(18) / 255,
    arousal: dv.getUint8(19) / 255, flags: dv.getUint8(20), groom: dv.getUint8(21) / 255,
    feed: dv.getUint8(22) / 255, link_q: dv.getUint8(23) / 255, brain_load: dv.getUint8(24) / 255,
  };
}
const FLAG = { ESCAPE: 1, LEFT: 2, RIGHT: 4, GROOM: 8, FEED: 16, ESTOP: 32 };
const DIR_MASK = FLAG.LEFT | FLAG.RIGHT;
function flagNames(f) {
  const n = [];
  if (f & FLAG.ESCAPE) n.push('逃逸');
  if (f & FLAG.LEFT) n.push('左转');
  if (f & FLAG.RIGHT) n.push('右转');
  if (f & FLAG.GROOM) n.push('梳理');
  if (f & FLAG.FEED) n.push('进食');
  if (f & FLAG.ESTOP) n.push('急停');
  return n.length ? n.join('|') : '-';
}

// ---------------------------------------------------------------- 6. 单帧推进
const STEP_MS = 50, STEPS_PER_FRAME = 500, DT_MS = 0.1;      // 20 Hz 帧 = 50 ms 生物时间
const EMA_TAU_MS = 200, EMA_A = 1 - Math.exp(-STEP_MS / EMA_TAU_MS);
const TH = { escape: 15, back: 20, feed: 30, groom: 20 };      // 沿用 game_core.js 量级

function newEma() {
  return { escape: 0, steer1L: 0, steer1R: 0, steer2L: 0, steer2R: 0, back: 0, feed: 0, groom: 0, arousal: 0 };
}
function newChState() { return { t: 0, deadRun: 0, wakeLeft: 0, wakeCh: null, wakeEvents: 0 }; }
const steerOf = (ema) => (ema.steer1L - ema.steer1R + ema.steer2L - ema.steer2R) / 2;

function stepFrame(brain, arb, chState, ema, intensity, seq, tMs, opts) {
  opts = opts || {};
  const useWatch = opts.watchdog !== false;
  const drives = {
    audio: CFG.rMaxHz * intensity.audio,
    jo:    CFG.rMaxHz * intensity.jo,
    touch: CFG.rMaxHz * intensity.touch,
  };
  chState.t = tMs;
  const w = ARBITERS[arb](drives, chState);

  if (useWatch && chState.wakeLeft > 0) {
    if (chState.wakeCh && (chState.wakeCh in w)) {
      // ★ 2026-10-06 实测修正：死区由【跨通道交叉抑制】造成，不是"驱动太弱"。
      //   证据（§0.4 二维表）：AUDIO 300 单独 → DNp01 155 Hz；一旦 TOUCH 同时驱动
      //   （即使只 100）→ 掉到 80；AUDIO 300 × TOUCH 300 → 仅 5。
      //   ⇒ 只把最强通道放大（旧实现 ×2）【救不回来】：另一通道仍在原位交叉抑制它。
      //   正解 = 唤醒期间临时切硬 WTA：其余通道压到 0，被唤醒通道再放大，
      //   让它脱离交叉抑制重新点火。语义上即"唤醒时注意力收窄"。
      for (const k of Object.keys(w)) if (k !== chState.wakeCh) w[k] = 0;
      w[chState.wakeCh] = Math.min(w[chState.wakeCh] * WATCH.boost, CFG.rMaxHz * WATCH.boost);
    }
    chState.wakeLeft--;
  }

  // 三条通道都双侧同幅驱动（实测 AUDIO 双侧 163 ≥ 仅右侧 150 > 仅左侧 0）
  brain.setRate(IN_PORT.audio.L, w.audio); brain.setRate(IN_PORT.audio.R, w.audio);
  brain.setRate(IN_PORT.jo.L, w.jo);       brain.setRate(IN_PORT.jo.R, w.jo);
  brain.setRate(IN_PORT.touch.L, w.touch); brain.setRate(IN_PORT.touch.R, w.touch);

  const t0 = process.hrtime.bigint();
  brain.run(STEPS_PER_FRAME);
  const brainMs = Number(process.hrtime.bigint() - t0) / 1e6;

  const r = brain.readRates([
    OUT_PORT.escape.L, OUT_PORT.escape.R, OUT_PORT.steer1.L, OUT_PORT.steer1.R,
    OUT_PORT.steer2.L, OUT_PORT.steer2.R, OUT_PORT.back.L, OUT_PORT.back.R,
    OUT_PORT.feed.L, OUT_PORT.feed.R, OUT_PORT.groom.L, OUT_PORT.groom.R,
  ], STEPS_PER_FRAME);

  const mix = (a, b) => (r[a] + r[b]) / 2;
  const esc = mix(OUT_PORT.escape.L, OUT_PORT.escape.R);
  const back = mix(OUT_PORT.back.L, OUT_PORT.back.R);
  const feed = mix(OUT_PORT.feed.L, OUT_PORT.feed.R);
  const groom = mix(OUT_PORT.groom.L, OUT_PORT.groom.R);

  const raw = {
    escape: esc, steer1L: r[OUT_PORT.steer1.L], steer1R: r[OUT_PORT.steer1.R],
    steer2L: r[OUT_PORT.steer2.L], steer2R: r[OUT_PORT.steer2.R],
    back, feed, groom, arousal: esc,
  };
  for (const k of Object.keys(ema)) ema[k] += EMA_A * (raw[k] - ema[k]);

  // ---- 行为判定 ----
  // 脑只给"强度/价态"：逃逸(DNp01) / 梳理(aDN1) / 进食(MN9) / 觉醒
  let flags = 0;
  if (ema.escape > TH.escape) flags |= FLAG.ESCAPE;
  if (ema.groom > TH.groom) flags |= FLAG.GROOM;
  if (ema.feed > TH.feed) flags |= FLAG.FEED;
  // 方向由【带符号侧向加速度】给（★见文件头结构性更正①）：lat>0 = 从右被推 → 左转
  const lat = intensity.lat || 0;
  if (lat > CFG.turnDeadband) flags |= FLAG.LEFT;
  else if (lat < -CFG.turnDeadband) flags |= FLAG.RIGHT;

  const motorOn = ema.escape > TH.escape;

  // 看门狗触发判定
  const dead = flags === 0 && !(ema.back > TH.back);
  let woke = false;
  if (useWatch) {
    const inMax = Math.max(intensity.audio, intensity.touch, intensity.jo);
    if (dead && inMax > WATCH.inputEps) {
      chState.deadRun++;
      if (chState.deadRun >= WATCH.window && chState.wakeLeft <= 0) {
        // 兜底唤醒通道：优先【最高行为优先级】且输入已达阈值的通道
        // （audio>touch>jo，即 逃逸>转向>梳理）。不能用"最大驱动"——touch 占优时会去唤醒
        // touch，而 v4 里 TOUCH 单独【出不了任何行为】（实测 audio0.30×touch0.45 那格旧规则
        // 唤醒 touch → 仍 100% 死区）。都不到阈值时退回"最大驱动"。
        let best = null;
        for (const k of ['audio', 'touch', 'jo'])
          if (intensity[k] > WATCH.inputEps) { best = k; break; }
        if (!best) {
          let bv = -Infinity;
          for (const k of Object.keys(drives)) { const v = drives[k] * PRIORITY[k]; if (v > bv) { bv = v; best = k; } }
        }
        chState.wakeCh = best; chState.wakeLeft = WATCH.hold; chState.deadRun = 0;
        chState.wakeEvents++; woke = true;
      }
    } else chState.deadRun = 0;
  }

  return {
    w, r, ema: { ...ema }, dead, woke, wakeLeft: chState.wakeLeft,
    steer: -lat, dnaSteer: steerOf(ema),          // dnaSteer 仅诊断（v4 里不是可靠转向信号）
    cmd: {
      seq, echo_ts_us: 0,
      turn: clamp(-lat, -1, 1),                    // lat>0(右推) → turn<0(左转)，与 flags 一致
      thrust: motorOn ? 1 : 0.15,
      motor: motorOn ? 1 : 0,
      arousal: clamp01(ema.arousal / 40),
      flags,
      groom: clamp01(ema.groom / 40),
      feed: clamp01(ema.feed / 40),
      link_q: 0.995,                               // 任务 A 实测突发丢包 0.50%
      brain_load: clamp01(brainMs / 50),
    },
    brainMs,
  };
}
function resetIo(brain) {
  for (const map of [IN_PORT, OUT_PORT])
    for (const k of Object.keys(map))
      for (const s of ['L', 'R']) brain.setRate(map[k][s], 0);
  brain.readRates(Object.values(OUT_PORT).map((p) => p.L), 1);
}
function warmBrain(brain, seed) {
  brain.reset(); brain.reseed(seed); resetIo(brain);
  brain.run(10000);                      // 1 s 生物时间预热（与 bench 同口径）
  brain.readRates(Object.values(OUT_PORT).map((p) => p.L), 1);
}

// ---------------------------------------------------------------- 7. 回放任务 A 存档
const FEATURE_RE = /^\s*#(\d+)\s+68 B\s+ts\s+(\d+)\s+rms\s+(-?[\d.]+)\s+dBFS\s+bands\s+(-?[\d.]+)\/\s*(-?[\d.]+)\/\s*(-?[\d.]+)/;
function parseProbeLog(file) {
  const frames = [];
  for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    const m = FEATURE_RE.exec(line);
    if (m) frames.push({
      seq: +m[1], send_ts_us: +m[2], rms_dbfs: +m[3],
      band_low: +m[4], band_mid: +m[5], band_high: +m[6],
      accel: [0, 0, 1.0],   // probe.log 只记了 rms+bands（无 IMU）→ 回放时 TOUCH/方向恒 0
    });
  }
  return frames;
}

// ---------------------------------------------------------------- 8. 合成场景
// 8.1 强度空间（仲裁对比用）：音频由安静→巨响，触觉两次右侧撞击，JO 平稳小扰动
function synthIntensity(tMs) {
  const t = tMs / 1000;
  let audio = 0.05;
  if (t > 0.6) audio = 0.05 + 0.9 * Math.min(1, (t - 0.6) / 0.6);
  if ((t > 1.0 && t < 1.15) || (t > 1.8 && t < 1.95)) audio = 1.0;   // 两声巨响
  const touch = ((t > 1.2 && t < 1.5) || (t > 2.6 && t < 2.9)) ? 0.9 : 0.0;
  const jo = 0.25;
  const lat = touch > 0.5 ? 0.7 : 0.0;   // 撞击来自右侧 → 左转
  return { audio: clamp01(audio), jo, touch, lat };
}

// 8.2 物理量空间（合成"有声"上行帧用）：安静的房间里 -> 说话 -> 拍手 -> 从右被撞 -> 刮风
function synthFeatureAt(tMs) {
  const t = tMs / 1000;
  const jitter = Math.sin(t * 13.1) * 1.5 + Math.sin(t * 7.3) * 1.2;   // 底噪抖动
  let low = -66 + jitter, mid = -68 + jitter, high = -70 + jitter, rms = -64 + jitter;
  let ax = 0, ay = 0, az = 1.0;
  let kind = 'quiet';
  if (t >= 2.0 && t < 3.2) {                       // 说话（中频为主）
    low = -40 + Math.sin(t * 17) * 3; mid = -42 + Math.sin(t * 23) * 3; high = -52; rms = -40; kind = 'speech';
  } else if (t >= 3.2 && t < 3.55) {               // 拍手（宽带瞬态，很响）
    low = -22; mid = -24; high = -28; rms = -21; kind = 'clap';
  } else if (t >= 4.0 && t < 4.45) {               // 从右侧被撞（IMU 冲击，ax>0）
    const k = (t - 4.0) / 0.45, s = Math.sin(Math.PI * k);
    az = 1.0 + 0.9 * s; ax = 0.55 * s; ay = 0.25 * s;
    low = -58; mid = -60; high = -62; rms = -58; kind = 'bump';
  } else if (t >= 5.5 && t < 6.4) {                // 又说话
    low = -41; mid = -43; high = -53; rms = -41; kind = 'speech';
  } else if (t >= 7.0 && t < 7.4) {                // 又一声巨响
    low = -23; mid = -25; high = -29; rms = -22; kind = 'clap';
  } else if (t >= 8.0 && t < 8.6) {                // 刮风/机械扰动（高频）
    low = -58; mid = -50; high = -38; rms = -46; kind = 'wind';
  }
  return { rms_dbfs: rms, band_low: low, band_mid: mid, band_high: high, accel: [ax, ay, az], kind };
}
function synthUplink(nFrames, hz) {
  const out = [];
  for (let i = 0; i < nFrames; i++) {
    const tMs = hz ? i * (1000 / hz) : i * STEP_MS;
    const f = synthFeatureAt(tMs);
    out.push({ tMs, kind: f.kind, buf: encodeFeature68({ seq: i, send_ts_us: Math.round(tMs) * 1000, rms_dbfs: f.rms_dbfs, band_low: f.band_low, band_mid: f.band_mid, band_high: f.band_high, accel: f.accel }) });
  }
  return out;
}

// ---------------------------------------------------------------- 9. 运行器
function runReplay(data, frames, seed = 1) {
  const brain = new ConnectomeBrain(data, seed);
  warmBrain(brain, seed);
  const ema = newEma(), st = newChState();
  const rows = [];
  for (let i = 0; i < frames.length; i++) {
    const f = frames[i];
    const intensity = featuresToIntensity(f);
    const o = stepFrame(brain, 'sum', st, ema, intensity, f.seq, i * STEP_MS, {});
    rows.push({ i, t: i * STEP_MS, intensity, ...o });
  }
  return { rows, st };
}

function runScenario(data, strategy, opts) {
  opts = opts || {};
  const brain = new ConnectomeBrain(data, 7);
  warmBrain(brain, 7);
  const ema = newEma(), st = newChState();
  const rows = [];
  for (let i = 0; i < 100; i++) {          // 5 s @20 Hz
    const inten = synthIntensity(i * STEP_MS);
    const o = stepFrame(brain, strategy, st, ema, inten, i, i * STEP_MS, opts);
    rows.push({ i, t: i * STEP_MS, intensity: inten, ...o });
  }
  let overlap = 0, oEsc = 0, oSteer = 0, oDead = 0;
  for (const x of rows) {
    if (!(x.intensity.audio > 0.5 && x.intensity.touch > 0.5)) continue;
    overlap++;
    if (x.cmd.flags & FLAG.ESCAPE) oEsc++;
    if (x.cmd.flags & DIR_MASK) oSteer++;
    if (x.dead) oDead++;
  }
  return { rows, st, stats: { overlap, oEsc, oSteer, oDead } };
}

// 死区网格：对 (audio, touch) 强度组合跑固定帧数，无/有看门狗各一遍
function runCell(data, strategy, audio, touch, nFrames, useWatch, seed) {
  const brain = new ConnectomeBrain(data, seed);
  warmBrain(brain, seed);
  const ema = newEma(), st = newChState();
  const rows = [];
  for (let i = 0; i < nFrames; i++) {
    const inten = { audio, jo: 0.15, touch, lat: 0 };
    const o = stepFrame(brain, strategy, st, ema, inten, i, i * STEP_MS, { watchdog: useWatch });
    rows.push({ i, t: i * STEP_MS, intensity: inten, ...o });
  }
  let dead = 0, esc = 0;
  for (const x of rows) { if (x.dead) dead++; if (x.cmd.flags & FLAG.ESCAPE) esc++; }
  return { rows, st, dead, esc, n: rows.length };
}

// ---------------------------------------------------------------- 10. 常驻云端大脑（--serve）
function startServer(data, argv) {
  const port = argv.includes('--port') ? +argv[argv.indexOf('--port') + 1] : 33333;
  // 只读 HTTP API 端口（0 = 关闭）。给网页看板用，见文件末 10c 段。
  const apiPort = argv.includes('--api') ? +argv[argv.indexOf('--api') + 1] : 0;
  const strategy = argv.includes('--strategy') ? argv[argv.indexOf('--strategy') + 1] : 'sum';
  const watchdog = !argv.includes('--no-watchdog');
  const quiet = argv.includes('--quiet');

  const brain = new ConnectomeBrain(data, 1);
  warmBrain(brain, 1);
  const ema = newEma(), st = newChState();

  const sock = dgram.createSocket('udp4');
  let frames = 0, rejects = 0, dead = 0, wasted = 0, tPrev = 0;
  let lastStats = Date.now();
  // 只读 API 的状态（--api 关闭时全程为 null / 0，不产生任何开销）
  let lastFrameForApi = null, lastCmdForApi = {}, lastBrainMsForApi = 0;

  sock.on('message', (msg, rinfo) => {
    let f;
    try { f = decodeFeature68(msg); }
    catch (e) {
      rejects++;
      if (!quiet) console.log(`  ! 丢弃 ${msg.length} B 来自 ${rinfo.address}:${rinfo.port}（${e.message}）`);
      return;
    }
    const dt = tPrev ? Date.now() - tPrev : 50; tPrev = Date.now();
    const o = stepFrame(brain, strategy, st, ema, featuresToIntensity(f), f.seq, Date.now(), { watchdog });
    // Echo the board's own send timestamp back so it can measure the real RTT.
    // Without this the field stays 0 and the board's RTT degenerates into
    // "uptime so far" - which is what happened on the first hardware run.
    o.cmd.echo_ts_us = f.send_ts_us >>> 0;
    const dl = encodeDownlink(o.cmd);
    sock.send(dl, rinfo.port, rinfo.address);
    frames++;
    if (o.dead) dead++;
    wasted += o.brainMs;
    // 给只读 API 的影子脑留一份最近特征（--api 开启时才需要）
    if (apiPort > 0) { lastFrameForApi = { features: f, seq: f.seq }; lastCmdForApi = o.cmd; lastBrainMsForApi = o.brainMs; }
    if (!quiet) {
      const inten = featuresToIntensity(f);
      console.log(`  #${String(f.seq).padStart(6)}  aud ${inten.audio.toFixed(2)} tou ${inten.touch.toFixed(2)} jo ${inten.jo.toFixed(2)} lat ${inten.lat.toFixed(2)}`
        + `  → esc ${o.ema.escape.toFixed(1)} groom ${o.ema.groom.toFixed(1)}  flags 0x${o.cmd.flags.toString(16).padStart(2, '0')} ${flagNames(o.cmd.flags)}`
        + `  turn ${o.cmd.turn.toFixed(2)}  ${o.brainMs.toFixed(1)}ms（到达间隔 ${dt}ms）`);
    }
    const now = Date.now();
    if (now - lastStats > 3000) {
      lastStats = now;
      console.log(`  --- 已处理 ${frames} 帧，静息 ${(100 * dead / frames).toFixed(0)}%，平均脑步 ${(wasted / frames).toFixed(1)} ms/帧，丢弃 ${rejects}`);
    }
  });
  sock.on('listening', () => {
    const a = sock.address();
    console.log(`=== 云端大脑就绪：UDP ${a.address}:${a.port}（策略 ${strategy}，看门狗 ${watchdog ? '开' : '关'}）`);
    console.log(`    子回路 ${path.basename(DATA_PATH)}，${data.fids.length} 神经元`);
    console.log('    板子指向本机即闭环；Ctrl-C 退出并打印汇总');
  });
  sock.bind(port);

  // --------------------------------------------------------------- 10c. 只读 API（--api）
  // 目的：让网页看板能看见【脑内部】在放什么，而不只是行为标量。
  // 做法：另开一个 HTTP 口，暴露 38 个神经元组的实时发放率。纯只读，不参与闭环。
  //
  // 关键实现点：`brain.readRates()` 会把计数清零，所以【不能】在 stepFrame 里顺带读
  // 全部38 组（那会让 12 个行为输出端口的读数归零，行为判定立刻崩）。
  // 正确做法是让 API 在**独立的短窗口**里跑一个影子副本：把同样的驱动打进去，
  // 跑 STEPS_PER_FRAME 步，读一次全组，再丢掉。代价是每 250 ms 多一次脑步
  // （约 18 ms，占单核 7%），换来的是看板能看到真实的组活跃度。
  if (apiPort > 0) {
    const ghost = new ConnectomeBrain(data, 1);
    const probeNames = Object.keys(data.groups);
    const ghostEma = newEma(), ghostSt = newChState();
    let ghostRates = {};

    const http = require('http');
    http.createServer((req, res) => {
      if (req.url !== '/api') { res.writeHead(404); res.end(); return; }
      const groups = probeNames.map((name) => {
        const m = name.match(/^(.*)_(left|right)$/);
        return {
          name,
          base: m ? m[1] : name,
          side: m ? m[2] : '',
          n: data.groups[name].length,
          hz: +((ghostRates[name]) || 0).toFixed(2),
        };
      });
      res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
      res.end(JSON.stringify({
        frame: lastFrameForApi ? lastFrameForApi.seq : null,
        brainMs: lastBrainMsForApi,
        deadPct: frames ? (100 * dead / frames) : 0,
        processed: frames,
        cmd: lastCmdForApi,
        groups,
        gates: { GATE_DB, GATE_SPAN, MAPV: process.env.MAPV || 'v6' },
      }));
    }).listen(apiPort, '127.0.0.1', () => {
      console.log(`=== 只读 API：http://127.0.0.1:${apiPort}/api （${probeNames.length} 个神经元组）`);
    });

    setInterval(() => {
      if (!lastFrameForApi) return;
      try {
        // 用最近一帧的真实特征驱动影子脑，量每一组的发放率。
        const it = featuresToIntensity(lastFrameForApi.features);
        stepFrame(ghost, strategy, ghostSt, ghostEma, it, lastFrameForApi.seq, Date.now(), { watchdog });
        ghostRates = ghost.readRates(probeNames, STEPS_PER_FRAME);
      } catch (e) { /* 影子脑失败不影响主闭环 */ }
    }, 250);
  }

  process.on('SIGINT', () => {
    console.log(`\n=== 汇总：收 ${frames} 帧，静息 ${frames ? (100 * dead / frames).toFixed(0) : 0}%，`
      + `平均脑步 ${frames ? (wasted / frames).toFixed(2) : 0} ms/帧，丢弃 ${rejects}`);
    sock.close();
    process.exit(0);
  });
}

// ---------------------------------------------------------------- 10b. 分析模式（--analyze）
// 目的：把"真机有声回放"变成可读结论，并给出 TH 重标定建议。
// 起因：TH={escape:15,back:20,feed:30,groom:20} 是从 game_core.js 的量级搬来的，未在真机
//       有声数据上标定过；静息段用它出现过"进食 59%"的自发误判（合成）与"0%"（真机），
//       两种极端都说明门限没对准。这里用【静息段的读数分布分位数】反推门限，而不是拍脑袋。
function pctl(sorted, q) {
  if (!sorted.length) return 0;
  const i = Math.min(sorted.length - 1, Math.max(0, Math.round((sorted.length - 1) * q)));
  return sorted[i];
}
function parseRawFrames(file) {
  const buf = fs.readFileSync(file);
  if (!buf.length || buf.length % 68 !== 0) return null;
  const out = [];
  for (let off = 0; off < buf.length; off += 68) {
    try { out.push(decodeFeature68(buf.subarray(off, off + 68))); } catch (e) { return null; }
  }
  return out;
}
function loadFrames(file) {
  const raw = parseRawFrames(file);                 // 二进制 68 B 帧优先
  if (raw && raw.length) return { frames: raw, kind: 'binary' };
  return { frames: parseProbeLog(file), kind: 'text' };
}
function runSequence(data, frames, seed, strategy, watchdog) {
  resetOnset();
  const brain = new ConnectomeBrain(data, seed);
  warmBrain(brain, seed);
  const ema = newEma(), st = newChState();
  const rows = [];
  for (let i = 0; i < frames.length; i++) {
    const f = frames[i];
    const intensity = featuresToIntensity(f);
    const o = stepFrame(brain, strategy, st, ema, intensity, f.seq, i * STEP_MS, { watchdog });
    rows.push({ i, t: i * STEP_MS, tsUs: f.send_ts_us, intensity, ...o });
  }
  return { rows, st };
}

function analyze(data, argv) {
  const at = argv.indexOf('--analyze');
  const nextTok = argv[at + 1];
  const file = (nextTok && !nextTok.startsWith('--')) ? nextTok : PROBE_LOG;
  const seed = argv.includes('--seed') ? +argv[argv.indexOf('--seed') + 1] : 1;
  const strategy = argv.includes('--strategy') ? argv[argv.indexOf('--strategy') + 1] : 'sum';
  const watchdog = !argv.includes('--no-watchdog');

  if (!fs.existsSync(file)) { console.log('!! 找不到文件 ' + file); process.exit(1); }
  const { frames, kind } = loadFrames(file);
  if (!frames.length) { console.log('!! ' + file + ' 里没有可解析的特征帧'); process.exit(1); }

  const A = runSequence(data, frames, seed, strategy, watchdog);
  const rows = A.rows, n = rows.length;
  const stim = rows.map((r) => Math.max(r.intensity.audio, r.intensity.jo, r.intensity.touch));
  const base = pctl([...stim].sort((a, b) => a - b), 0.10);
  const hi = pctl([...stim].sort((a, b) => a - b), 0.95);
  const activeTh = Math.max(base + 0.10, (base + hi) / 2);

  const shares = (rs) => ({
    n: rs.length,
    esc: rs.filter((x) => x.cmd.flags & FLAG.ESCAPE).length,
    dir: rs.filter((x) => x.cmd.flags & DIR_MASK).length,
    groom: rs.filter((x) => x.cmd.flags & FLAG.GROOM).length,
    feed: rs.filter((x) => x.cmd.flags & FLAG.FEED).length,
    rest: rs.filter((x) => x.dead).length,
  });
  const all = shares(rows);

  console.log('\n[A] 来源 %s（%s，%d 帧 = %s s，种子 %d，策略 %s，看门狗 %s）',
    file, kind, n, (n * STEP_MS / 1000).toFixed(1), seed, strategy, watchdog ? '开' : '关');
  console.log('    刺激基线 p10 %s   p95 %s   ⇒ 有声判据 stim > %s',
    fmt(base, 3), fmt(hi, 3), fmt(activeTh, 3));
  console.log('    整体: 逃逸 %s%%  转向 %s%%  梳理 %s%%  进食 %s%%  静息 %s%%',
    pct(all.esc, n), pct(all.dir, n), pct(all.groom, n), pct(all.feed, n), pct(all.rest, n));
  console.log('    脑步 %s ms/帧（预算 50 ms）',
    fmt(rows.reduce((s, x) => s + x.brainMs, 0) / n, 2));

  // [B] 定长块时间线 —— 用于和歌曲时间轴对齐
  const block = argv.includes('--block') ? +argv[argv.indexOf('--block') + 1] : 5;
  const perBlock = Math.max(1, Math.round(block * 1000 / STEP_MS));
  console.log('\n[B] 时间线（每 %d s 一块；stim/audio/jo/touch 为块内均值）', block);
  console.log('      t(s)    stim  audio     jo  touch | 逃逸 转向 梳理 进食 静息 | 主要');
  for (let i = 0; i < n; i += perBlock) {
    const rs = rows.slice(i, i + perBlock), s = shares(rs);
    const mean = (k) => rs.reduce((a, x) => a + x.intensity[k], 0) / rs.length;
    const ms = rs.reduce((a, x) => a + Math.max(x.intensity.audio, x.intensity.jo, x.intensity.touch), 0) / rs.length;
    const best = [['逃逸', s.esc], ['转向', s.dir], ['梳理', s.groom], ['进食', s.feed]].sort((a, b) => b[1] - a[1])[0];
    console.log('    %s  %s %s %s %s | %s %s %s %s %s | %s',
      fmt(i * STEP_MS / 1000, 1), fmt(ms, 3), fmt(mean('audio'), 3), fmt(mean('jo'), 3), fmt(mean('touch'), 3),
      pct(s.esc, s.n), pct(s.dir, s.n), pct(s.groom, s.n), pct(s.feed, s.n), pct(s.rest, s.n),
      s.rest / s.n > 0.6 ? '静息' : (best[1] > 0 ? best[0] : '-'));
  }

  // [C] 自动分段：连续"有声/静息"运行段（丢掉 <2 s 碎片）
  // 平滑 ±1 s 后再分段：阈值附近的逐帧噪声会把时间线切成无数碎片
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
  }
  if (cur) segs.push(cur);
  const big = segs.filter((s) => s.rows.length >= Math.max(1, Math.round(2000 / STEP_MS)));
  console.log('\n[C] 自动分段（仅列 ≥2 s 的段）');
  console.log('      段     时长(s)    帧  逃逸% 转向% 梳理% 进食% 静息%');
  for (const s of big) {
    const sh = shares(s.rows);
    console.log('    %s %s %s  %s  %s  %s  %s  %s',
      (s.active ? '有声' : '静息').padEnd(8), fmt(s.rows.length * STEP_MS / 1000, 1).padStart(7),
      String(sh.n).padStart(5), pct(sh.esc, sh.n), pct(sh.dir, sh.n),
      pct(sh.groom, sh.n), pct(sh.feed, sh.n), pct(sh.rest, sh.n));
  }


  // [E] 响度剂量-反应：直接按 rms_dbfs 五分位分箱（与 stim 无关）。
  //      用于 onset 映射：此时 stim 已不再代表响度，[C] 的静息/有声标签不可用。
  const rmsArr = frames.map((f) => f.rms_dbfs);
  const srtR = [...rmsArr].sort((a, b) => a - b);
  const cuts = [pctl(srtR, 0.2), pctl(srtR, 0.4), pctl(srtR, 0.6), pctl(srtR, 0.8)];
  const dBins = [[], [], [], [], []];
  for (let i = 0; i < n; i++) { let b = 0; while (b < 4 && rmsArr[i] >= cuts[b]) b++; dBins[b].push(rows[i]); }
  console.log('');
  console.log('[E] 响度剂量-反应（按 rms_dbfs 五分位；静息=最小一档，很响=最大一档）');
  console.log('      响度区间(dBFS)        帧   逃逸% 转向% 梳理% 进食% 静息%');
  for (let b = 0; b < 5; b++) {
    const rs = dBins[b]; if (!rs.length) continue;
    const s = shares(rs);
    const lo = (b === 0) ? '(最小)' : fmt(cuts[b - 1], 1);
    const hi = (b === 4) ? '(最大)' : fmt(cuts[b], 1);
    console.log('    %s ~ %s  %s  %s  %s  %s  %s  %s',
      String(lo).padStart(7), String(hi).padStart(7), String(s.n).padStart(6),
      pct(s.esc, s.n), pct(s.dir, s.n), pct(s.groom, s.n), pct(s.feed, s.n), pct(s.rest, s.n));
  }

  // [D] TH 重标定：只有"静息 vs 有声"真的分得开时才给建议，否则保持原值（不乱降门限）
  const quietRows = big.filter((s) => !s.active).flatMap((s) => s.rows);
  const actRows = big.filter((s) => s.active).flatMap((s) => s.rows);
  const haveActive = actRows.length >= Math.max(1, Math.round(2000 / STEP_MS));
  const dist = (rs, key) => {
    const v = rs.map((x) => x.ema[key]).sort((a, b) => a - b);
    return { p50: pctl(v, 0.5), p90: pctl(v, 0.9), p99: pctl(v, 0.99), max: v.length ? v[v.length - 1] : 0 };
  };
  console.log('\n[D] 读出分布（EMA，Hz）—— 静息段 vs 有声段');
  console.log('      通道     静息p50  静息p99  静息max |  有声p50  有声p90  有声max | 分离 |   建议TH');
  const suggest = {};
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
      (haveActive ? (separable ? '有' : '无') : '无段').padStart(4),
      (separable ? fmt(suggest[k], 1) : (TH[k] + '(保持)')).padStart(8));
  }
  if (!haveActive) console.log('    ⚠️ 本段全是静息、没有"有声段" ⇒ 无从标定，TH 保持原值；需要一段真实有声录音。');
  const r1 = (x) => Math.round(x * 10) / 10;
  console.log('\n    当前 TH  %s', JSON.stringify(TH));
  console.log('    建议 TH  const TH = { escape: %s, back: %s, feed: %s, groom: %s };',
    r1(suggest.escape), r1(TH.back), r1(suggest.feed), r1(suggest.groom));
  console.log('    （back 本轮无真实输入，沿用原值；若某通道在有声段也毫无响应，则该通道保持保守值。）');
  console.log('\n完成。');
}

// ---------------------------------------------------------------- 11. main
function fmt(x, n = 1) { return x.toFixed(n).padStart(6); }
function pct(a, b) { return b ? (100 * a / b).toFixed(0).padStart(4) : '   0'; }

const HELP = `用法：node stage1_loop.js <模式> [选项]
  --replay            回放 /root/probe.log（任务 A 存档，文本特征）
  --synth             合成"有声"上行二进制帧 → 全链闭环自测
  --emit-dl FILE      把 --synth 的真实 MPRC 下行流写成文件（32 B/帧）
  --grid              死区看门狗验证（二维强度网格）
  --selftest          MPRC/MPRB 编解码自检
  --emit FILE         生成合成上行二进制帧文件
  --blast HOST:PORT   经 UDP 发送合成帧（配合 --serve 联测）
  --serve             常驻：UDP 收 68 B → 跑脑 → 回 32 B MPRC
选项：--strategy sum|wta|soft_wta|round_robin   --no-watchdog   --port N   --quiet`;

function synthRun(data, opts) {
  const list = synthUplink(200, 20);          // 10 s
  const brain = new ConnectomeBrain(data, 3);
  warmBrain(brain, 3);
  const ema = newEma(), st = newChState();
  const rows = [];
  for (let i = 0; i < list.length; i++) {
    const f = decodeFeature68(list[i].buf);    // ← 走真实二进制解码（含 CRC 校验）
    const intensity = featuresToIntensity(f);
    const o = stepFrame(brain, 'sum', st, ema, intensity, f.seq, list[i].tMs, opts);
    rows.push({ i, t: list[i].tMs, kind: list[i].kind, intensity, ...o });
  }
  return rows;
}

function main() {
  const argv = process.argv.slice(2);
  const strategy = argv.includes('--strategy') ? argv[argv.indexOf('--strategy') + 1] : 'sum';
  const opts = { watchdog: !argv.includes('--no-watchdog') };

  if (argv.includes('--help') || argv.length === 0) { console.log(HELP); return; }

  // --- selftest（不需要子回路）
  if (argv.includes('--selftest')) {
    const cmd = { seq: 42, echo_ts_us: 123456, turn: -0.4, thrust: 0.8, motor: 0.5, arousal: 0.7, flags: FLAG.ESCAPE | FLAG.LEFT, groom: 0.1, feed: 0.2, link_q: 0.99, brain_load: 0.35 };
    const back = decodeDownlink(encodeDownlink(cmd));
    const up = decodeFeature68(encodeFeature68({ seq: 7, send_ts_us: 555, rms_dbfs: -30, band_low: -35, band_mid: -38, band_high: -50, accel: [0.1, 0, 1.0] }));
    const ok = Math.abs(back.turn - cmd.turn) <= 1 / 127 && back.flags === cmd.flags && up.seq === 7 && Math.abs(up.band_low + 35) < 1e-3;
    console.log('MPRC 自检:', encodeDownlink(cmd).toString('hex'));
    console.log('MPRB 回读:', JSON.stringify(up));
    console.log(ok ? 'SELFTEST PASS' : 'SELFTEST FAIL');
    process.exit(ok ? 0 : 1);
  }

  // --- emit（不需要子回路）
  if (argv.includes('--emit')) {
    const file = argv[argv.indexOf('--emit') + 1];
    const list = synthUplink(200, 20);
    const buf = Buffer.concat(list.map((x) => x.buf));
    fs.writeFileSync(file, buf);
    console.log(`已写出 ${list.length} 帧 × 68 B = ${buf.length} B → ${file}`);
    return;
  }

  const data = JSON.parse(fs.readFileSync(DATA_PATH, 'utf8'));
  console.log('=== 子回路:', path.basename(DATA_PATH), '神经元', data.fids.length, '边', data.meta && data.meta.n_edges);
  console.log('=== 端口组规模 (L/R):', JSON.stringify(assertPorts(data)));

  if (argv.includes('--serve')) { startServer(data, argv); return; }

  if (argv.includes('--blast')) {
    const [host, portS] = argv[argv.indexOf('--blast') + 1].split(':');
    const port = +(portS || 33333);
    const list = synthUplink(200, 20);
    const sock = dgram.createSocket('udp4');
    let i = 0, acked = 0;
    sock.on('message', () => { acked++; });
    const tick = setInterval(() => {
      if (i >= list.length) {
        clearInterval(tick);
        setTimeout(() => { console.log(`blast 完成：发 ${list.length} 帧，收到 ${acked} 帧下行回包`); sock.close(); process.exit(0); }, 500);
        return;
      }
      sock.send(list[i].buf, port, host); i++;
    }, 50);
    console.log(`向 ${host}:${port} 以 20 Hz 发送 ${list.length} 帧合成上行数据…`);
    return;
  }

  if (argv.includes('--synth') || argv.includes('--emit-dl')) {
    const rows = synthRun(data, opts);
    const n = rows.length;

    if (argv.includes('--emit-dl')) {
      const file = argv[argv.indexOf('--emit-dl') + 1];
      const buf = Buffer.concat(rows.map((x) => encodeDownlink(x.cmd)));
      fs.writeFileSync(file, buf);
      const nFlag = { esc: rows.filter((x) => x.cmd.flags & FLAG.ESCAPE).length, dir: rows.filter((x) => x.cmd.flags & DIR_MASK).length };
      console.log(`已写出 ${n} 帧 × 32 B = ${buf.length} B → ${file}（逃逸 ${nFlag.esc} 帧，含转向 ${nFlag.dir} 帧）`);
      return;
    }

    const cnt = (pred) => rows.filter(pred).length;
    console.log(`\n[E] 合成"有声"上行闭环（${n} 帧 = ${(n * STEP_MS / 1000).toFixed(1)} s，看门狗 ${opts.watchdog ? '开' : '关'}）`);
    console.log('    分段行为占比（按输入类型统计）:');
    console.log('      段       帧   逃逸%  左转%  右转%  梳理%  进食%  静息%');
    for (const k of ['quiet', 'speech', 'clap', 'bump', 'wind']) {
      const s = rows.filter((x) => x.kind === k);
      if (!s.length) continue;
      console.log('      %s %s  %s  %s  %s  %s  %s  %s',
        k.padEnd(8), String(s.length).padStart(3),
        pct(s.filter((x) => x.cmd.flags & FLAG.ESCAPE).length, s.length),
        pct(s.filter((x) => x.cmd.flags & FLAG.LEFT).length, s.length),
        pct(s.filter((x) => x.cmd.flags & FLAG.RIGHT).length, s.length),
        pct(s.filter((x) => x.cmd.flags & FLAG.GROOM).length, s.length),
        pct(s.filter((x) => x.cmd.flags & FLAG.FEED).length, s.length),
        pct(s.filter((x) => x.dead).length, s.length));
    }
    console.log('    平均脑步 %s ms/帧（预算 50 ms）；全段静息 %s%%',
      fmt(rows.reduce((s, x) => s + x.brainMs, 0) / n, 2), pct(cnt((x) => x.dead), n));
    console.log('    关键瞬刻下行帧（MPRC 32 B，板端解码回读）:');
    // 每段取"最像该段行为"的一帧：|turn| 最大者。
    // （bump 的峰值在段中间，取首帧会因 lat=0 被误示为"无动作"——0x00）
    const picks = ['quiet', 'speech', 'clap', 'bump', 'wind'].map((k) => {
      const s = rows.filter((x) => x.kind === k);
      if (!s.length) return null;
      return s.reduce((a, b) => (Math.abs(b.cmd.turn) > Math.abs(a.cmd.turn) ? b : a));
    }).filter(Boolean);
    for (const x of picks) {
      const b = encodeDownlink(x.cmd), d = decodeDownlink(b);
      console.log('      t=%ss %s  turn %s thrust %s motor %s flags 0x%s(%s)',
        fmt(x.t / 1000, 1), x.kind.padEnd(6), fmt(d.turn, 2), fmt(d.thrust, 2), fmt(d.motor, 2),
        d.flags.toString(16).padStart(2, '0'), flagNames(d.flags));
    }
    console.log('    说明：合成数据只证明"链路与阈值可用"，真机需补一段真实有声录音复测。');
    return;
  }

  if (argv.includes('--grid')) {
    const audioList = [0.30, 0.40, 0.50], touchList = [0.25, 0.35, 0.45];
    console.log('\n[D] 死区看门狗验证（策略 sum；每格 2.0 s = 40 帧）');
    console.log('    audio touch |  无看门狗          |  有看门狗');
    console.log('                |  死区%  逃逸%       |  死区%  逃逸%   唤醒  唤醒通道');
    let traps = 0, rescued = 0;
    for (const a of audioList) for (const t of touchList) {
      const off = runCell(data, 'sum', a, t, 40, false, 11);
      const on = runCell(data, 'sum', a, t, 40, true, 11);
      const dOff = 100 * off.dead / off.n, dOn = 100 * on.dead / on.n;
      if (dOff >= 50) { traps++; if (dOn < dOff) rescued++; }
      console.log('    %s  %s |  %s  %s       |  %s  %s  %s   %s',
        a.toFixed(2), t.toFixed(2),
        pct(off.dead, off.n), pct(off.esc, off.n),
        pct(on.dead, on.n), pct(on.esc, on.n), String(on.st.wakeEvents).padStart(4),
        String(on.st.wakeCh || '-').padEnd(6));
    }
    console.log('    ⇒ 识别出 %d 个"无看门狗会卡死"的格，其中 %d 个被看门狗唤醒。', traps, rescued);
    return;
  }

  if (argv.includes('--analyze')) { analyze(data, argv); return; }

  // --- replay（默认）
  const frames = parseProbeLog(PROBE_LOG);
  if (!frames.length) { console.log('!! 没有特征帧，跳过回放'); process.exit(1); }
  console.log(`=== probe.log 可回放特征帧: ${frames.length} 帧 (${(frames.length * STEP_MS / 1000).toFixed(1)} s)`);

  const A = runReplay(data, frames, argv.includes('--seed') ? +argv[argv.indexOf('--seed') + 1] : 1);
  const n = A.rows.length, act = (pred) => A.rows.filter(pred).length;
  console.log(`\n[A] 真实数据回放（sum，看门狗 ${opts.watchdog ? '开' : '关'}，${n} 帧）`);
  console.log('    输入强度(均值): audio=%s jo=%s touch=%s lat=%s',
    fmt(A.rows.reduce((s, x) => s + x.intensity.audio, 0) / n, 3),
    fmt(A.rows.reduce((s, x) => s + x.intensity.jo, 0) / n, 3),
    fmt(A.rows.reduce((s, x) => s + x.intensity.touch, 0) / n, 3),
    fmt(A.rows.reduce((s, x) => s + x.intensity.lat, 0) / n, 3));
  console.log('    行为帧占比: 逃逸 %s%%  转向 %s%%  梳理 %s%%  进食 %s%%  后退 %s%%  静息 %s%%',
    pct(act((x) => x.cmd.flags & FLAG.ESCAPE), n), pct(act((x) => x.cmd.flags & DIR_MASK), n),
    pct(act((x) => x.cmd.flags & FLAG.GROOM), n), pct(act((x) => x.cmd.flags & FLAG.FEED), n),
    pct(act((x) => x.ema.back > TH.back), n), pct(act((x) => x.dead), n));
  console.log('    平均脑步耗时 %s ms/帧（预算 50 ms）；看门狗触发 %d 次',
    fmt(A.rows.reduce((s, x) => s + x.brainMs, 0) / n, 2), A.st.wakeEvents);

  console.log('\n[B] 合成重叠场景（巨响 × 右侧撞击），4 种仲裁策略对比（看门狗关闭）：');
  console.log('    策略          重叠帧  逃逸%   转向%   死区%');
  for (const s of ['sum', 'wta', 'soft_wta', 'round_robin']) {
    const R = runScenario(data, s, { watchdog: false });
    const q = R.stats;
    console.log('    %s %s  %s  %s  %s',
      s.padEnd(12), String(q.overlap).padStart(5),
      pct(q.oEsc, q.overlap), pct(q.oSteer, q.overlap), pct(q.oDead, q.overlap));
  }

  console.log('\n[C] 下行命令帧 MPRC（32 B，CRC 校验通过）：');
  for (const i of [0, Math.floor(n / 4), Math.floor(n / 2)]) {
    const x = A.rows[i], b = encodeDownlink(x.cmd), d = decodeDownlink(b);
    console.log('    t=%ss turn=%s thrust=%s motor=%s flags=0x%s(%s)',
      fmt(x.t / 1000, 1), fmt(d.turn, 2), fmt(d.thrust, 2), fmt(d.motor, 2),
      d.flags.toString(16).padStart(2, '0'), flagNames(d.flags));
    console.log('    hex=%s', b.toString('hex'));
  }
  console.log('\n完成。');
}
main();
