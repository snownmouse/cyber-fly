// 果蝇脑 · 中文实时诊断 + 网页看板
//
// 架构（2026-10-06 定案）：
//   板子 --UDP 33333--> relay.js --127.0.0.1--> 大脑 33335
//                     \--> 33336 --> 本进程（中文诊断 + 网页）
//
// 关键教训1：relay 最初把包转发到**公网 IP**，结果大脑一个包都收不到
//   （CPU 时间 0.00）。发到 127.0.0.1 立刻通。
//   云厂商的网络层会丢弃绕出去再回来的流量，**同一台机器内部必须走回环**。
// 关键教训2：relay 的转发和接收必须用同一个 socket，否则大脑回包到不了板子。
// 关键教训3：任何无符号序号差值都要判回绕，否则板子重启一次就显示"丢序百亿"。
//
// 为什么单独一个进程而不是塞进 stage1_loop.js：诊断不该影响闭环，
// 万一它挂了，闭环保住。本进程全程只读。
const dgram = require('dgram');
const http = require('http');

const MAPV = process.env.MAPV || 'v6';
const GATE_DB = +(process.env.GATE_DB || -44);
const GATE_SPAN = +(process.env.GATE_SPAN || 16);
const BRAIN_PORT = +(process.env.FLY_BRAIN_PORT || 33335);
const WATCH_PORT = +(process.env.FLY_WATCH_PORT || 33336);
const HTTP_PORT = +(process.env.FLY_WATCH_HTTP || 33340);
const BRAIN_API = process.env.FLY_BRAIN_API || 'http://127.0.0.1:33338/api';

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

let frames = 0, rejects = 0, gaps = 0, restarts = 0, lastSeq = null, lastAt = 0, fwdN = 0;
const cur = { rms: 0, low: 0, mid: 0, high: 0, ax: 0 };

// 历史曲线（只给网页用；文本诊断不依赖它们）
const hist = { aud: [], jo: [], esc: [], lat: [] };
const pushH = (k, v) => { const a = hist[k]; a.push(v); if (a.length > 240) a.shift(); };

const out = dgram.createSocket('udp4');
out.on('error', (e) => console.log('[转发错误]', e.message));
out.bind(0, '0.0.0.0', () =>
  console.log(`转发到大脑 127.0.0.1:${BRAIN_PORT}（本地 ${out.address().port}）`));

const sock = dgram.createSocket('udp4');
sock.on('message', (msg) => {
  if (msg.length !== 68 || msg.readUInt32LE(0) !== 0x4D505242) { rejects++; return; }
  if (msg.readUInt32LE(64) !== crc32(msg.subarray(0, 64))) { rejects++; return; }
  const seq = msg.readUInt32LE(8);
  if (lastSeq !== null) {
    // Unsigned wraparound: the board restarts and seq jumps backwards. A naive
    // forward-difference then reports billions of lost frames, which buries the
    // real counters. Treat a large backward jump as a restart, not as loss.
    const delta = (seq - lastSeq) >>> 0;
    if (delta > 0 && delta < 1000) gaps += delta - 1;
    else if (delta >= 1000) restarts++;
  }
  lastSeq = seq; frames++; lastAt = Date.now();
  cur.rms = msg.readFloatLE(24); cur.low = msg.readFloatLE(28);
  cur.mid = msg.readFloatLE(32); cur.high = msg.readFloatLE(36);
  cur.ax = msg.readFloatLE(40);
  win.push(cur.rms); if (win.length > 20) win.shift();
  out.send(Buffer.from(msg), BRAIN_PORT, '127.0.0.1', () => { fwdN++; });
});

// 当前的映射值。抽成函数让文本诊断和网页共用同一份口径 ——
// 两处各算一遍必然会出现"日志说 0.3、网页说 0.5"这种无法排查的分歧。
function now() {
  const floor = Math.min.apply(null, win);
  const onset = clamp01(Math.max(0, cur.rms - floor) / 12);
  const level = clamp01((cur.rms - GATE_DB) / GATE_SPAN);
  const band = dbTo01(cur.low + cur.mid);
  const aud = MAPV === 'v4' ? onset
            : MAPV === 'v6' ? Math.max(onset, level)
            : MAPV === 'v7' ? level : band;
  return { floor, onset, level, aud, jo: dbTo01(cur.high), lat: clamp01(Math.abs(cur.ax)) };
}

// ---------------------------------------------------------------- 文本诊断
let tick = 0;
setInterval(() => {
  const alive = lastAt && (Date.now() - lastAt) < 3000;
  if (!frames) { console.log('等待板子数据…（检查热点 / Wi-Fi 密码）'); return; }
  const v = now();
  const bar = (x, n = 20) => '█'.repeat(Math.round(x * n)).padEnd(n, '·');
  let verdict, why;
  if (v.level >= 0.99 && v.aud >= 0.99) {
    verdict = '✗ 饱和：永远在逃逸';
    why = `电平打满。rms 只要再高 ${(GATE_DB + GATE_SPAN - cur.rms).toFixed(1)} dB 就到 1.00。`
        + `降 FLY_MIC_GAIN_DB（现在 30），或把 GATE_DB 抬到 ${(cur.rms - 4).toFixed(0)} 之上。`;
  } else if (v.aud > 0.6) {
    verdict = '! 偏敏感';
    why = `aud=${v.aud.toFixed(2)}，安静时偏高。看 rms 历史范围。`;
  } else {
    verdict = '✓ 正常';
    why = `安静时 aud=${v.aud.toFixed(2)} 合理，放歌时应该明显上升。`;
  }
  const steer = Math.abs(cur.ax) < 0.02
    ? '侧向几乎为 0 —— IMU 没接或板子没动'
    : `侧向 ${cur.ax.toFixed(3)} g，转向应该跟手`;
  console.log(
    `[${String(++tick).padStart(4)}] ${alive ? '在线' : '断流'}  ${verdict}\n` +
    `   听觉 aud ${v.aud.toFixed(2)} ${bar(v.aud)}   梳理 jo ${v.jo.toFixed(2)} ${bar(v.jo)}\n` +
    `   电平 rms ${cur.rms.toFixed(1)} dBFS  低 ${cur.low.toFixed(1)}  中 ${cur.mid.toFixed(1)}  高 ${cur.high.toFixed(1)}\n` +
    `   ${steer}\n` +
    `   帧 ${frames}  丢序 ${gaps}  坏帧 ${rejects}  重启 ${restarts}  已转发 ${fwdN}\n` +
    `   → ${why}`);
}, 2000);

// ---------------------------------------------------------------- 脑侧只读 API
// 神经元活跃度来自大脑进程的 --api（见 stage1_loop.js 的 10c 段）。
// 大脑用一个"影子脑"在独立窗口里跑，所以不会清零主脑的输出端口读数。
let brainOk = false, brain = {}, groups = [];
function pollBrain() {
  const rq = http.get(BRAIN_API, { timeout: 1200 }, (rs) => {
    let b = ''; rs.setEncoding('utf8');
    rs.on('data', (c) => { b += c; if (b.length > 400000) rq.destroy(); });
    rs.on('end', () => {
      try { const j = JSON.parse(b); brainOk = true; brain = j; groups = j.groups || []; }
      catch (e) { brainOk = false; }
    });
  });
  rq.on('error', () => { brainOk = false; });
  rq.on('timeout', () => { rq.destroy(); brainOk = false; });
}

// ---------------------------------------------------------------- 网页面板
const CSS = `body{background:#0d1220;color:#dfe6f5;font-family:"Microsoft YaHei",system-ui,sans-serif;margin:0;padding:14px}
h1{font-size:18px;margin:0 0 2px}.sub{color:#8595b5;font-size:11px;margin-bottom:10px}
.vd{display:inline-block;padding:8px 16px;border-radius:7px;font-size:16px;font-weight:700;margin-bottom:6px}
.ok{background:#12351f;color:#5fd97e;border:1px solid #1f6b3c}
.warn{background:#3d2a0c;color:#ffc14d;border:1px solid #7a5416}
.bad{background:#3a1418;color:#ff7b7b;border:1px solid #7a2830}
.muted{background:#1c2338;color:#8595b5;border:1px solid #26304a}
.dt{color:#a9b6d0;font-size:12px;line-height:1.65;margin-bottom:11px;max-width:1000px}
.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(118px,1fr));gap:7px;margin-bottom:11px}
.c{background:#161d31;border:1px solid #26304a;border-radius:7px;padding:7px 9px}
.k{color:#8595b5;font-size:10.5px}.v{font-size:18px;font-weight:700;font-variant-numeric:tabular-nums}
.b{height:5px;background:#232c44;border-radius:3px;margin-top:5px;overflow:hidden}
.b i{display:block;height:100%}
h2{font-size:12.5px;color:#8595b5;margin:13px 0 6px;font-weight:600}
table{border-collapse:collapse;font-size:12px;width:100%}
td{padding:2px 12px 2px 0;color:#a9b6d0}
td.v{color:#dfe6f5;font-weight:600;font-variant-numeric:tabular-nums}
.nm{font-weight:700;color:#dfe6f5}
svg{background:#101728;border:1px solid #26304a;border-radius:7px;width:100%;height:auto}
.lg{display:flex;gap:12px;flex-wrap:wrap;color:#8595b5;font-size:11px;margin-top:5px}
.lg s{width:10px;height:10px;border-radius:2px;display:inline-block;margin-right:4px;vertical-align:-1px}
.sb{display:flex;gap:6px;align-items:center;margin-bottom:9px;flex-wrap:wrap}
button{background:#1e2942;color:#dfe6f5;border:1px solid #26304a;border-radius:5px;padding:5px 11px;font-size:12px;cursor:pointer}
button.on{background:#1d4a2f;border-color:#2a7a48}`;

const PAGE_JS = `
const $=i=>document.getElementById(i);
let S={},cur='aud';
const CH={aud:{h:()=>S.audioHist||[],c:'#59c2ff',l:'听觉强度 aud'},
 esc:{h:()=>S.escHist||[],c:'#ff6b6b',l:'逃逸 escape'},
 lat:{h:()=>S.latHist||[],c:'#ffd166',l:'侧向 lat'},
 jo:{h:()=>S.joHist||[],c:'#2f6fb8',l:'梳理 jo'}};
function ch(k,b){cur=k;['bA','bE','bL','bJ'].forEach(i=>$(i).className='');b.className='on';}
function bar(v,c){return '<div class="b"><i style="width:'+(Math.max(0,Math.min(1,v))*100).toFixed(1)+'%;background:'+c+'"></i></div>';}
function card(k,v,bv,c){return '<div class="c"><div class="k">'+k+'</div><div class="v">'+v+'</div>'+(bv===undefined?'':bar(bv,c||'#59c2ff'))+'</div>';}
function hzC(h){return h>=60?'#ff6b6b':h>=30?'#ffd166':h>=10?'#59c2ff':h>=2?'#2f6fb8':'#1e2942';}
function draw(sr){
 const W=960,H=190,p=22,all=sr.flat().filter(Number.isFinite);
 if(all.length<2){$('ch').innerHTML='';return;}
 let lo=Math.min.apply(null,all),hi=Math.max.apply(null,all);
 if(hi-lo<1e-6){hi=lo+1;lo=Math.max(0,lo-.5);}
 const n=sr[0].length,y=v=>H-p-((v-lo)/(hi-lo))*(H-2*p),dx=(W-p-8)/(n-1);
 let o='<line x1="'+p+'" y1="'+(H-p)+'" x2="'+(W-8)+'" y2="'+(H-p)+'" stroke="#2a3550"/>'
   +'<line x1="'+p+'" y1="'+p+'" x2="'+(W-8)+'" y2="'+p+'" stroke="#1c2440"/>';
 sr.forEach(s=>{if(s.length<2)return;let d='';
  for(let i=0;i<s.length;i++)d+=(i?' L':'M')+(p+i*dx).toFixed(1)+' '+y(s[i]).toFixed(1);
  o+='<path d="'+d+'" fill="none" stroke="'+CH[cur].c+'" stroke-width="1.7"/>';});
 o+='<text x="3" y="'+(y(hi)+4).toFixed(1)+'" fill="#6b7a99" font-size="11">'+hi.toFixed(2)+'</text>'
  +'<text x="3" y="'+(y(lo)+4).toFixed(1)+'" fill="#6b7a99" font-size="11">'+lo.toFixed(2)+'</text>'
  +'<text x="'+(W-72)+'" y="'+(p-5)+'" fill="#6b7a99" font-size="11">'+CH[cur].l+'</text>';
 $('ch').innerHTML=o;
}
async function tick(){
 try{
  const s=await (await fetch('/api')).json(); S=s;
  const v=$('vd'); v.textContent=s.verdict;
  v.className='vd '+(s.verdict.startsWith('正常')?'ok':(s.verdict.startsWith('偏')||s.verdict.indexOf('没有侧向')>0)?'warn':'bad');
  $('dt').textContent=s.detail;
  const c=s.cmd||{};
  $('cards').innerHTML=
   card('听觉 aud',s.audio.toFixed(2),s.audio)
  +card('梳理 jo',s.jo.toFixed(2),s.jo,'#ffd166')
  +card('侧向 lat',s.lat.toFixed(2),s.lat,'#ff9f45')
  +card('起始 onset',s.onset.toFixed(2),s.onset)
  +card('门限 level',s.level.toFixed(2),s.level,'#ff6b6b')
  +card('逃逸',(c.arousal||0).toFixed(1),(c.arousal||0)/100,'#ff6b6b')
  +card('脑步',(s.brainMs||0).toFixed(1)+' ms')
  +card('脑静息',(s.brainDead||0).toFixed(0)+'%')
  +card('已处理帧',s.processed||0)
  +card('已收帧',s.frames)
  +card('丢序',s.seqGaps)
  +card('重启',s.restarts)
  +card('坏帧',s.rejects);
  draw([CH[cur].h()]);
  const gs=(s.groups||[]).slice().sort((a,b)=>b.hz-a.hz);
  let r='';
  for(const g of gs){
   r+='<tr><td class="nm">'+g.base+(g.side?'<span style="color:#6b7a99">·'+g.side+'</span>':'')+'</td>'
    +'<td class="v">'+g.hz.toFixed(1)+' Hz</td>'
    +'<td style="width:170px">'+bar(Math.min(1,g.hz/80),hzC(g.hz))+'</td>'
    +'<td style="color:#6b7a99">'+g.n+' 个</td></tr>';
  }
  $('neu').innerHTML=r||'<tr><td colspan="4" style="color:#8595b5">'+(s.brainOk?'等待脑侧数据…':'脑侧未连上——检查大脑的 --api 是否开启')+'</td></tr>';
  $('feat').innerHTML=
   '<tr><td>总 RMS</td><td class="v">'+s.rms.toFixed(1)+' dBFS</td><td>云端映射的输入，饱和说明门限太低</td></tr>'
  +'<tr><td>低频 50-150 Hz</td><td class="v">'+s.low.toFixed(1)+' dBFS</td><td>JO-B 类神经元</td></tr>'
  +'<tr><td>中频 250-450 Hz</td><td class="v">'+s.mid.toFixed(1)+' dBFS</td><td>JO-A 类神经元</td></tr>'
  +'<tr><td>高频 600-1500 Hz</td><td class="v">'+s.high.toFixed(1)+' dBFS</td><td>宽带／起始检测</td></tr>'
  +'<tr><td>侧向加速度</td><td class="v">'+s.accel[0].toFixed(3)+' g</td><td>决定左右转向</td></tr>';
 }catch(e){}
 setTimeout(tick,500);
}
tick();`;

const PAGE = '<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">'
  + '<meta name="viewport" content="width=device-width,initial-scale=1">'
  + '<title>果蝇脑 · 实时看板</title><style>' + CSS + '</style></head><body>'
  + '<h1>果蝇脑 · 实时看板</h1>'
  + '<div class="sub">MAPV=' + MAPV + ' ｜ 门限 ' + GATE_DB + ' dBFS ／ 跨度 ' + GATE_SPAN
  + ' dB ｜ 收副本 UDP ' + WATCH_PORT + ' ｜ 网页 ' + HTTP_PORT + '</div>'
  + '<div class="sb"><button id="bA" onclick="ch(\'aud\',this)">听觉 aud</button>'
  + '<button id="bE" onclick="ch(\'esc\',this)">逃逸 esc</button>'
  + '<button id="bL" onclick="ch(\'lat\',this)">侧向 lat</button>'
  + '<button id="bJ" onclick="ch(\'jo\',this)">梳理 jo</button>'
  + '<span style="color:#8595b5;font-size:11px">放歌时切到「逃逸」最容易看清反应</span></div>'
  + '<div id="vd" class="vd muted">连接中…</div><div id="dt" class="dt"></div>'
  + '<div id="cards" class="g"></div>'
  + '<h2>曲线（最近 12 秒）</h2><svg id="ch" viewBox="0 0 960 190" preserveAspectRatio="none"></svg>'
  + '<h2>★ 神经元活跃度（38 组，每 250 ms 采样，按发放率排序）</h2>'
  + '<div class="lg"><s style="background:#1e2942"></s>静息<s style="background:#2f6fb8"></s>轻微'
  + '<s style="background:#59c2ff"></s>活跃<s style="background:#ffd166"></s>强'
  + '<s style="background:#ff6b6b"></s>最强</div>'
  + '<table id="neu"></table>'
  + '<h2>上行特征（云端实际收到的值）</h2><table id="feat"></table>'
  + '<script>' + PAGE_JS + '</script></body></html>';

// 判定口径：这是本文件的核心价值，不是画图。
// 每一条都告诉人"接下来该做什么"，而不是只报一个数字。
function verdict() {
  const alive = lastAt && (Date.now() - lastAt) < 3000;
  if (!frames || !alive) return ['未连接',
    '板子已停止发帧超过 3 秒。检查：① 手机热点是否还开着（2.4 GHz）② 云端 relay 是否在跑。'];
  const v = now();
  if (v.level >= 0.99 && v.aud >= 0.99) return ['饱和 · 永远在逃逸',
    `电平饱和：rms=${cur.rms.toFixed(1)} dBFS 映射成 level=1.00。只要再高 `
    + `${(GATE_DB + GATE_SPAN - cur.rms).toFixed(1)} dB 就打满 ⇒ 降 FLY_MIC_GAIN_DB 或抬 GATE_DB。`];
  if (v.aud > 0.6) return ['偏敏感', `aud=${v.aud.toFixed(2)} 偏高，安静时不该这么高。`];
  if (!brainOk) return ['脑侧未连上',
    `板子链路正常（aud=${v.aud.toFixed(2)}），但读不到脑内部状态。检查大脑是否带 --api 33338 启动。`];
  if (Math.abs(cur.ax) < 0.02) return ['正常 · 但没有侧向加速度',
    `aud=${v.aud.toFixed(2)} 合理。侧向=${cur.ax.toFixed(3)} g 偏小，拿起来侧推看数值变化。`];
  return ['正常', `aud=${v.aud.toFixed(2)}  jo=${v.jo.toFixed(2)}  rms=${cur.rms.toFixed(1)} dBFS  `
    + `escape=${((brain.cmd || {}).arousal || 0).toFixed(1)}  已处理 ${brain.processed || 0} 帧`];
}

http.createServer((req, res) => {
  if (req.url === '/api') {
    const v = now();
    const [vd, dt] = verdict();
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify({
      connected: !!(lastAt && Date.now() - lastAt < 3000),
      frames, rejects, seqGaps: gaps, restarts,
      audio: v.aud, jo: v.jo, lat: v.lat, onset: v.onset, level: v.level,
      rms: cur.rms, low: cur.low, mid: cur.mid, high: cur.high,
      accel: [cur.ax, 0, 0],
      audioHist: hist.aud, joHist: hist.jo, escHist: hist.esc, latHist: hist.lat,
      brainOk, brainMs: brain.brainMs || 0, brainDead: brain.deadPct || 0,
      processed: brain.processed || 0, cmd: brain.cmd || {}, groups,
      verdict: vd, detail: dt,
    }));
    return;
  }
  res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
  res.end(PAGE);
}).listen(HTTP_PORT, '127.0.0.1', () => {
  console.log(`诊断 API：http://127.0.0.1:${HTTP_PORT}/api （只读：音频/侧向/起始/门限）`);
  console.log(`脑侧 API：${BRAIN_API}`);
});

sock.bind(WATCH_PORT);
console.log(`果蝇脑中文诊断 + 网页看板：收 ${WATCH_PORT}，转发大脑 ${BRAIN_PORT}，网页 ${HTTP_PORT}；`
          + `MAPV=${MAPV} GATE_DB=${GATE_DB}/${GATE_SPAN}`);

// 每 250 ms 采样：脑侧行为进 esc 曲线，本侧映射值进 aud/jo/lat 曲线。
// 时间基准与大脑的 --api 一致，这样两条曲线在网页上是对齐的。
setInterval(() => {
  const v = now();
  pushH('aud', v.aud); pushH('jo', v.jo); pushH('lat', v.lat);
  pushH('esc', (brain.cmd || {}).arousal || 0);
}, 250);
pollBrain();
setInterval(pollBrain, 250);