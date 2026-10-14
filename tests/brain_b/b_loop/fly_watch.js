// 果蝇脑 · 中文实时诊断（极简版）
//
// 为什么不用网页：调试"一直飞起来"只需要三件事——
//   1. 板子上来的电平到底是多少 dBFS（决定该调增益还是门限）
//   2. 云端映射出来的 aud/jo/esc 是多少
//   3. 侧向加速度有没有变化（决定转向接没接上）
// 这三件事用 `ssh 服务器 'node fly_watch.js'` 每秒刷一屏就够，
// 而网页版本要多传 20 KB base64 上服务器，不值。
//
// 用法：
//   setsid nohup node fly_watch.js > /root/watch.log 2>&1 < /dev/null &
//   tail -f /root/watch.log
//
// 它是 stage1_loop.js 的**旁路**：自己开一个 UDP 口收板子帧，不干扰闭环。
const dgram = require('dgram');
const argv = process.argv.slice(2);
const arg = (n, d) => { const i = argv.indexOf('--' + n); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };
const PORT = +arg('port', 33334);
const MAPV = process.env.MAPV || 'v6';
const GATE_DB = +(process.env.GATE_DB || -44);
const GATE_SPAN = +(process.env.GATE_SPAN || 16);

function crc32(buf) {
  let c, crc = 0xFFFFFFFF;
  for (let i = 0; i < buf.length; i++) {
    c = (crc ^ buf[i]) & 0xFF;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

const win = [];
const clamp01 = (v) => Math.max(0, Math.min(1, v));
const dbTo01 = (d) => clamp01((d + 80) / 80);
let frames = 0, rejects = 0, gaps = 0, lastSeq = null, lastAt = 0;
let cur = { rms: 0, low: 0, mid: 0, high: 0, ax: 0 };

const sock = dgram.createSocket('udp4');
sock.on('message', (msg) => {
  if (msg.length !== 68 || msg.readUInt32LE(0) !== 0x4D505242) { rejects++; return; }
  if (msg.readUInt32LE(64) !== crc32(msg.subarray(0, 64))) { rejects++; return; }
  const seq = msg.readUInt32LE(8);
  if (lastSeq !== null) { const d = (seq - lastSeq) >>> 0; if (d > 1) gaps += d - 1; }
  lastSeq = seq; frames++; lastAt = Date.now();
  cur = { rms: msg.readFloatLE(24), low: msg.readFloatLE(28), mid: msg.readFloatLE(32),
          high: msg.readFloatLE(36), ax: msg.readFloatLE(40) };
  win.push(cur.rms); if (win.length > 20) win.shift();
});

let tick = 0;
setInterval(() => {
  const alive = lastAt && (Date.now() - lastAt) < 3000;
  if (!frames) { console.log('等待板子数据…（检查热点 / Wi-Fi 密码 / 云端是否在听）'); return; }
  const floor = Math.min.apply(null, win);
  const onset = clamp01(Math.max(0, cur.rms - floor) / 12);
  const level = clamp01((cur.rms - GATE_DB) / GATE_SPAN);
  const MAP = { v4: [[cur.low, cur.mid], [cur.high]], v6: [[cur.low, cur.mid], [cur.high]],
                v7: [[cur.low, cur.mid], [cur.high]] }[MAPV] || [[cur.low, cur.mid], [cur.high]];
  const band = dbTo01(MAP[0][0] + MAP[0][1]);
  const aud = MAPV === 'v4' ? onset : MAPV === 'v6' ? Math.max(onset, level) : MAPV === 'v7' ? level : band;
  const jo = dbTo01(MAP[1][0]);
  const bar = (v, n = 20) => '█'.repeat(Math.round(v * n)).padEnd(n, '·');

  let verdict, why;
  if (level >= 0.99 && aud >= 0.99) {
    verdict = '✗ 饱和：永远在逃逸';
    why = `电平打满。门限 GATE_DB=${GATE_DB}、跨度 ${GATE_SPAN}，rms 只要再高 ${(GATE_DB + GATE_SPAN - cur.rms).toFixed(1)} dB 就到 1.00。`
       + ` ⇒ 降 FLY_MIC_GAIN_DB（现在 30），或把 GATE_DB 抬到 ${(cur.rms - 4).toFixed(0)} 之上。`;
  } else if (aud > 0.6) {
    verdict = '! 偏敏感'; why = `aud=${aud.toFixed(2)}，安静时不该这么高。`;
  } else {
    verdict = '✓ 正常'; why = `安静时 aud=${aud.toFixed(2)} 是对的。放歌时它应该明显上升。`;
  }

  console.log(
    `[${String(++tick).padStart(5)}] ${alive ? '在线' : '已断流'}  ${verdict}\n` +
    `        听觉 aud ${aud.toFixed(2)} ${bar(aud)}   梳理 jo ${jo.toFixed(2)} ${bar(jo)}\n` +
    `        起始 onset ${onset.toFixed(2)}  门限 level ${level.toFixed(2)}\n` +
    `        电平 rms ${cur.rms.toFixed(1)} dBFS  低 ${cur.low.toFixed(1)}  中 ${cur.mid.toFixed(1)}  高 ${cur.high.toFixed(1)}\n` +
    `        侧向 ${cur.ax.toFixed(3)} g   帧 ${frames}  丢序 ${gaps}  坏帧 ${rejects}\n` +
    `        → ${why}`);
}, 1000);

sock.bind(PORT);
console.log(`果蝇脑诊断已启动：UDP ${PORT}，MAPV=${MAPV}，GATE_DB=${GATE_DB}/${GATE_SPAN}`);
