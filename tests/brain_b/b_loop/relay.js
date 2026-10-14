// 板子帧双向转发（云端旁路）
//
// 架构（2026-10-07 修正后定案）：
//   板子 --UDP 33333--> relay.js --127.0.0.1--> 大脑 33335
//             ^                |
//             |                +--127.0.0.1--> 中文诊断 33336
//             +-- 回包（32 B MPRC）由 relay 转发回来
//
// ★ 双向是关键。第一版只做上行转发（板子 → 大脑），结果：
//   板子 tx 一路上涨、云端 20 万帧零丢弃、诊断"已转发 10 万"，
//   而板子 rx 恒为 0。
//
// ★★ 为什么必须用**同一个 socket**收发
// 大脑用 `sock.send(dl, rinfo.port, rinfo.address)` 回包，而 rinfo 来自
// **收到帧的那个 socket 的源地址**。若转发用独立的出口 socket，回包就只回到
// 那个独立 socket，主 socket 永远收不到 —— 症状是"云端在收在回、relay 下行 0、
// 板子 rx=0"。改成收发共用一个 socket 后立刻正常（up=300 down=284, errs=0）。
//
// ★★★ 目标必须用 127.0.0.1，不能用公网 IP。
// 最初写成 <公网IP>:33335，症状极具误导性：
//   send 回调成功、errs 0、日志显示 "forwarded 250"，
//   但大脑进程 CPU 时间 0:00.00，一个包都没收到；
//   iptables INPUT 规则数为 0（不是本机防火墙）；
//   改成 127.0.0.1 后立刻收到回包。
// ⇒ 腾讯云等厂商的网络层会丢弃"绕出去再回来"的流量。
const dgram = require('dgram');

const IN_PORT = +(process.env.FLY_IN_PORT || 33333);
const BRAIN = +(process.env.FLY_BRAIN_PORT || 33335);
const WATCH = +(process.env.FLY_WATCH_PORT || 33336);

const sock = dgram.createSocket('udp4');
sock.on('error', (e) => console.log('[socket 错误]', e.message));

let board = null;        // 当前板子地址（收到上行帧时更新）
let up = 0, down = 0, errs = 0;

sock.on('message', (msg, rinfo) => {
  const c = Buffer.from(msg);   // Node 可能复用 msg 的内存

  if (msg.length === 68) {
    // 上行：记住板子地址，用同一个 socket 转发给大脑和诊断
    board = { host: rinfo.address, port: rinfo.port };
    sock.send(c, BRAIN, '127.0.0.1', (e) => { if (e) errs++; });
    sock.send(c, WATCH, '127.0.0.1', (e) => { if (e) errs++; });
    up++;
    if (up % 100 === 0) {
      console.log(`[relay] up=${up} (board ${board.host}:${board.port}) down=${down} err=${errs}`);
    }
  } else if (msg.length === 32) {
    // 下行：大脑回包，转回板子
    if (!board) return;
    sock.send(c, board.port, board.host, (e) => {
      if (e) { errs++; return; }
      down++;
      if (down % 100 === 0) console.log(`[relay] up=${up} down=${down} err=${errs}`);
    });
  }
});

sock.bind(IN_PORT, '0.0.0.0', () =>
  console.log(`[relay] ${IN_PORT} <-> 127.0.0.1:${BRAIN}(大脑) + ${WATCH}(诊断)`));

setInterval(() => { if (up) console.log(`[relay] up=${up} down=${down} err=${errs}`); }, 6000);

process.on('SIGINT', () => {
  console.log(`\n[relay] 汇总：上行 ${up} 下行 ${down} 错误 ${errs}`);
  process.exit(0);
});
