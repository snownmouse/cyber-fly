// 果蝇脑 · 实时看板（单一服务：生成页面 + 代理脑侧 API）
// 启动：setsid nohup node fly_dashboard.js > /root/tdash/dash.log 2>&1 < /dev/null &
// 页面：http://<公网IP>:8080/
//
// 为什么自包含：页面直接在进程里拼出来，只改这一个文件即可。
//
// [rev 2026-10-07b] 重做：脑区图 + 数据汇报，并对音频字段做防御式渲染（音频缺失时优雅降级为 —）。
// [rev 2026-10-07c] 接线修正：DIAG_API 默认指向 fly_watch2 的 33340（只读诊断：音频/侧向/起始/门限）；
//   脑区图按「发放率 OR 行为旗」点亮（逃逸旗亮 ⟺ 逃逸区亮）；顶部判定由行为旗+脑发放率派生，与数据汇报一致。
//
// 数据来源（默认，全部来自脑侧）：
//   BRAIN_API = stage1_loop 的 --api（默认 33338）
//      → regions（9 大功能区）、groups（38 组）、cmd（arousal/turn/motor/flags）、
//        gates、frame、brainMs、deadPct、processed
//   DIAG_API  = 可选。fly_watch2 的诊断 HTTP（若将来给它加 /api，
//              可补上 audio/jo/lat/verdict/audioHist 等音频字段）。不设置则忽略。
//
// 页面布局（用户要求）：
//   顶部：判定 + 副标题
//   左大区：13 个指标卡 + 曲线（默认显示「神经活动」）+ 上行特征表
//   右下 1/4：数据汇报（行为状态/逃逸强度/转向/振翅 + 最活跃组）
//   左下 1/4：脑区神经图（9 大功能区，按发放率上色，激活时发光）
const http = require('http');

const DIR = __dirname;
const HTTP_PORT = +(process.env.FLY_DASH_HTTP || 8080);
const BRAIN_API = process.env.FLY_BRAIN_API || 'http://127.0.0.1:33338/api';
const DIAG_API = process.env.FLY_DIAG_API || 'http://127.0.0.1:33340/api';

// ---- 取 JSON（带超时，错误返回 null，绝不抛异常） ----
function getJSON(url) {
  return new Promise((resolve) => {
    const rq = http.get(url, { timeout: 2500 }, (rs) => {
      let b = '';
      rs.setEncoding('utf8');
      rs.on('data', (c) => { b += c; if (b.length > 400000) rq.destroy(); });
      rs.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { resolve(null); } });
    });
    rq.on('error', () => resolve(null));
    rq.on('timeout', () => { rq.destroy(); resolve(null); });
  });
}

let lastBrain = null, brainFail = 0;
let lastDiag = null;

async function loopBrain() {
  const d = await getJSON(BRAIN_API);
  if (d) { lastBrain = d; brainFail = 0; } else { brainFail++; }
}
async function loopDiag() {
  if (!DIAG_API) return;
  lastDiag = await getJSON(DIAG_API);
}
loopBrain(); loopDiag();
setInterval(loopBrain, 300);
setInterval(loopDiag, 300);

// ---------------------------------------------------------------- 页面（内联生成）
const PAGE = '<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">'
  + '<meta name="viewport" content="width=device-width,initial-scale=1">'
  + '<title>果蝇脑 · 实时看板</title><style>'
  + 'body{background:#0b0f1a;color:#dfe6f5;font-family:"Microsoft YaHei",system-ui,sans-serif;margin:0}'
  + '#top{display:flex;justify-content:space-between;align-items:center;padding:8px 14px;background:#101728;border-bottom:1px solid #26304a}'
  + '#top h1{font-size:16px;margin:0}#top .sub{color:#8595b5;font-size:11px}'
  + '#body{display:flex;height:calc(100vh - 46px)}'
  + '#main{flex:1;display:flex;flex-direction:column;overflow:auto;padding:12px}'
  + '#side{width:360px;background:#101728;padding:12px;overflow:auto;border-left:1px solid #26304a}'
  + '.vd{display:inline-block;padding:7px 14px;border-radius:6px;font-size:15px;font-weight:700}'
  + '.ok{background:#12351f;color:#5fd97e;border:1px solid #1f6b3c}'
  + '.warn{background:#3d2a0c;color:#ffc14d;border:1px solid #7a5416}'
  + '.bad{background:#3a1418;color:#ff7b7b;border:1px solid #7a2830}'
  + '.muted{background:#1c2338;color:#8595b5;border:1px solid #26304a}'
  + 'h2{font-size:12px;color:#8595b5;margin:0 0 8px;font-weight:600;letter-spacing:.5px}'
  + '.dt{color:#a9b6d0;font-size:11.5px;line-height:1.6;margin-bottom:10px}'
  + '.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(108px,1fr));gap:7px;margin-bottom:12px}'
  + '.c{background:#161d31;border:1px solid #26304a;border-radius:6px;padding:6px 8px}'
  + '.k{color:#8595b5;font-size:10px}.v{font-size:16px;font-weight:700;font-variant-numeric:tabular-nums}'
  + '.b{height:4px;background:#232c44;border-radius:2px;margin-top:4px;overflow:hidden}.b i{display:block;height:100%}'
  + 'svg{background:#0d1220;border:1px solid #26304a;border-radius:6px;width:100%;height:auto}'
  + '.sb{display:flex;gap:5px;margin-bottom:9px;flex-wrap:wrap}'
  + 'button{background:#1e2942;color:#dfe6f5;border:1px solid #26304a;border-radius:5px;padding:4px 9px;font-size:11.5px;cursor:pointer}'
  + 'button.on{background:#1d4a2f;border-color:#2a7a48}'
  + '#brainmap{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-bottom:10px}'
  + '.rg{background:#161d31;border:1px solid #26304a;border-radius:6px;padding:7px 8px;position:relative;overflow:hidden}'
  + '.rg .nm{font-size:12px;font-weight:700;display:flex;justify-content:space-between;align-items:baseline}'
  + '.rg .hz{font-size:12px;font-weight:700;font-variant-numeric:tabular-nums}'
  + '.rg .hint{color:#6b7a99;font-size:9.5px;margin-top:2px}'
  + '.rg .fill{position:absolute;left:0;bottom:0;height:3px;transition:width .2s}'
  + '.rg.act{border-color:#59c2ff;box-shadow:0 0 0 1px #59c2ff inset,0 0 10px rgba(89,194,255,.35)}'
  + '.lgd{display:flex;gap:10px;flex-wrap:wrap;color:#8595b5;font-size:10px;margin-top:6px}'
  + '.lgd s{width:9px;height:9px;border-radius:2px;display:inline-block;margin-right:3px;vertical-align:-1px}'
  + 'table{border-collapse:collapse;font-size:11px;width:100%}'
  + 'td{padding:2px 8px 2px 0;color:#a9b6d0}'
  + 'td.v{color:#dfe6f5;font-weight:600;font-variant-numeric:tabular-nums}'
  + '.nm2{font-weight:700;color:#dfe6f5}'
  + '.row{display:flex;justify-content:space-between;padding:2px 0;border-bottom:1px solid #1c2338}'
  + '</style></head><body>'
  + '<div id="top"><h1>果蝇脑 · 实时看板</h1><div class="sub" id="sub"></div></div>'
  + '<div id="body">'
  + '<div id="main">'
  + '<div id="vd" class="vd muted">连接中…</div><div id="dt" class="dt"></div>'
  + '<div class="sb"><button id="bA" onclick="ch(\'act\',this)">神经 act</button>'
  + '<button id="bE" onclick="ch(\'aud\',this)">听觉 aud</button>'
  + '<button id="bF" onclick="ch(\'esc\',this)">逃逸 esc</button>'
  + '<button id="bL" onclick="ch(\'lat\',this)">侧向 lat</button>'
  + '<button id="bJ" onclick="ch(\'jo\',this)">梳理 jo</button>'
  + '<span style="color:#8595b5;font-size:11px">放歌时看「神经 act / 逃逸」最容易看清反应</span></div>'
  + '<div id="cards" class="g"></div>'
  + '<h2>曲线（最近 12 秒）</h2><svg id="ch" viewBox="0 0 960 200" preserveAspectRatio="none"></svg>'
  + '<h2>★ 神经元活跃度 · 脑区图（9 大功能区，激活时发光）</h2>'
  + '<div id="brainmap"></div>'
  + '<div class="lgd"><s style="background:#1e2942"></s>静息<s style="background:#2f6fb8"></s>轻微'
  + '<s style="background:#59c2ff"></s>活跃<s style="background:#ffd166"></s>强'
  + '<s style="background:#ff6b6b"></s>最强</div>'
  + '<h2>上行特征（云端实际收到的值）</h2><table id="feat"></table>'
  + '</div>'
  + '<div id="side"><h2>数据汇报</h2><div id="rep"></div>'
  + '<h2>★ 全 38 组神经元（按发放率排序）</h2><table id="neu"></table></div>'
  + '</div>'
  + '<script>'
  + 'const $=i=>document.getElementById(i);'
  + 'let S={},cur=\'act\',actHist=[];'
  + 'const CH={act:{h:()=>actHist,c:\'#59c2ff\',l:\'神经活动(最大发放率 Hz)\'},'
  + ' aud:{h:()=>S.audioHist||[],c:\'#59c2ff\',l:\'听觉强度 aud\'},'
  + ' esc:{h:()=>S.escHist||[],c:\'#ff6b6b\',l:\'逃逸 escape\'},'
  + ' lat:{h:()=>S.latHist||[],c:\'#ffd166\',l:\'侧向 lat\'},'
  + ' jo:{h:()=>S.joHist||[],c:\'#2f6fb8\',l:\'梳理 jo\'}};'
  + 'function ch(k,b){cur=k;[\'bA\',\'bE\',\'bF\',\'bL\',\'bJ\'].forEach(i=>$(i).className=\'\');b.className=\'on\';}'
  + 'function n(x,d){return (x==null||isNaN(+x))?\'—\':(+x).toFixed(d==null?2:d);}'
  + 'function bar(v,c){return \'<div class="b"><i style="width:\'+(Math.max(0,Math.min(1,v||0))*100).toFixed(1)+\'%;background:\'+c+\'"></i></div>\';}'
  + 'function card(k,v,bv,c){return \'<div class="c"><div class="k">\'+k+\'</div><div class="v">\'+v+\'</div>\'+(bv===undefined?\'\':bar(bv,c||\'#59c2ff\'))+\'</div>\';}'
  + 'function hzC(h){return h>=60?\'#ff6b6b\':h>=30?\'#ffd166\':h>=10?\'#59c2ff\':h>=2?\'#2f6fb8\':\'#1e2942\';}'
  + 'function draw(sr){'
  + ' const W=960,H=200,p=22,all=sr.flat().filter(Number.isFinite);'
  + ' if(all.length<2){$(\'ch\').innerHTML=\'\';return;}'
  + ' let lo=Math.min.apply(null,all),hi=Math.max.apply(null,all);'
  + ' if(hi-lo<1e-6){hi=lo+1;lo=Math.max(0,lo-.5);}'
  + ' const n0=sr[0].length,y=v=>H-p-((v-lo)/(hi-lo))*(H-2*p),dx=(W-p-8)/(n0-1);'
  + ' let o=\'<line x1="\'+p+\'" y1="\'+(H-p)+\'" x2="\'+(W-8)+\'" y2="\'+(H-p)+\'" stroke="#2a3550"/>\''
  + '   +\'<line x1="\'+p+\'" y1="\'+p+\'" x2="\'+(W-8)+\'" y2="\'+p+\'" stroke="#1c2440"/>\';'
  + ' sr.forEach(s=>{if(s.length<2)return;let d=\'\';'
  + '  for(let i=0;i<s.length;i++)d+=(i?\' L\':\'M\')+(p+i*dx).toFixed(1)+\' \'+y(s[i]).toFixed(1);'
  + '  o+=\'<path d="\'+d+\'" fill="none" stroke="\'+CH[cur].c+\'" stroke-width="1.7"/>\';});'
  + ' o+=\'<text x="3" y="\'+(y(hi)+4).toFixed(1)+\'" fill="#6b7a99" font-size="11">\'+hi.toFixed(2)+\'</text>\''
  + '  +\'<text x="3" y="\'+(y(lo)+4).toFixed(1)+\'" fill="#6b7a99" font-size="11">\'+lo.toFixed(2)+\'</text>\''
  + '  +\'<text x="\'+(W-70)+\'" y="\'+(p-5)+\'" fill="#6b7a99" font-size="11">\'+CH[cur].l+\'</text>\';'
  + ' $(\'ch\').innerHTML=o;'
  + '}'
  + 'function flagsText(f){const m=[\'逃逸\',\'左转\',\'右转\',\'梳理\'];let a=[];for(let i=0;i<4;i++)if(f&(1<<i))a.push(m[i]);return a.length?a.join(\' \'):\'静息（无行为）\';}'
  + 'async function tick(){'
  + ' try{'
  + '  const s=await (await fetch(\'/api\')).json();S=s;'
  + '  const c=s.cmd||{};'
  + '  const rs=(s.regions||[]).slice().sort((a,b)=>b.hz-a.hz);'
  + '  const maxHz=rs.length?rs[0].hz:0;'
  + '  actHist.push(maxHz);if(actHist.length>240)actHist.shift();'
  + '  let vd,vcls,dt;'
  + '  if(!s.brainOk){vd=\'脑侧未连接\';vcls=\'bad\';dt=\'检查大脑的 --api 33338 是否开启\';}'
  + '  else if(s.verdict){vd=s.verdict;vcls=s.verdict.indexOf(\'正常\')>=0?\'ok\':(s.verdict.indexOf(\'饱和\')>=0?\'bad\':\'warn\');dt=s.detail||\'\';}'
  + '  else if(maxHz>=30){vd=\'神经活动强\';vcls=\'warn\';dt=\'有脑区发放率偏高，放歌/拍打时应更明显\';}'
  + '  else if(maxHz>=2){vd=\'有神经活动\';vcls=\'ok\';dt=\'脑在正常工作\';}'
  + '  else {vd=\'静息\';vcls=\'muted\';dt=\'暂无明显神经发放\';}'
  + '  $(\'vd\').textContent=vd;$(\'vd\').className=\'vd \'+vcls;$(\'dt\').textContent=dt;'
  + '  $(\'sub\').textContent=\'MAPV=\'+(s.gates?s.gates.MAPV:\'—\')+\' ｜ 已处理 \'+(s.processed==null?\'—\':s.processed)+\' 帧 ｜ 脑静息 \'+(s.brainDead==null?\'—\':(+s.brainDead).toFixed(0))+\'%\';'
  + '  const hasAud=s.audio!=null;'
  + '  $(\'cards\').innerHTML='
  + '   card(\'神经活动\',n(maxHz,1)+\' Hz\',Math.min(1,maxHz/80))'
  + '  +card(\'听觉 aud\',n(s.audio),hasAud?s.audio:undefined)'
  + '  +card(\'梳理 jo\',n(s.jo),hasAud?s.jo:undefined,\'#ffd166\')'
  + '  +card(\'侧向 lat\',n(s.lat),hasAud?s.lat:undefined,\'#ff9f45\')'
  + '  +card(\'起始 onset\',n(s.onset),hasAud?s.onset:undefined)'
  + '  +card(\'门限 level\',n(s.level),hasAud?s.level:undefined,\'#ff6b6b\')'
  + '  +card(\'逃逸\',n(c.arousal,1),(c.arousal||0)/100,\'#ff6b6b\')'
  + '  +card(\'脑步\',n(s.brainMs,1)+\' ms\')'
  + '  +card(\'脑静息\',n(s.brainDead,0)+\'%\')'
  + '  +card(\'已处理帧\',s.processed==null?\'—\':s.processed)'
  + '  +card(\'脑帧\',s.frame==null?\'—\':s.frame)'
  + '  +card(\'组数\',(s.groups||[]).length)'
  + '  +card(\'MAPV\',s.gates?(s.gates.MAPV||\'—\'):\'—\')'
  + '  +card(\'最大发放\',n(maxHz,1)+\' Hz\');'
  + '  draw([CH[cur].h()]);'
  + '  const bflags2=(c.flags||0); const flagLit=(k)=>{if(k===\'escape\')return bflags2&1;if(k===\'steer\')return bflags2&(2|4);if(k===\'groom\')return bflags2&8;if(k===\'feed\')return bflags2&16;return false;};'
  + '  let r=\'\';'
  + '  for(const g of rs){const lit=g.hz>=2||flagLit(g.key);'
  + '   r+=\'<div class="rg\'+(lit?\' act\':\'\')+\'"><div class="nm"><span>\'+g.label+\'</span><span class="hz">\'+g.hz.toFixed(1)+\' Hz</span></div>\''
  + '    +\'<div class="hint">\'+g.hint+\'</div>\''
  + '    +\'<div class="fill" style="width:\'+Math.min(100,g.hz/80*100).toFixed(0)+\'%;background:\'+hzC(g.hz)+\'"></div></div>\';}'
  + '  $(\'brainmap\').innerHTML=r;'
  + '  const gs=(s.groups||[]).slice().sort((a,b)=>b.hz-a.hz);'
  + '  $(\'rep\').innerHTML='
  + '   \'<div class="c">行为状态<span class="v">\'+flagsText(c.flags||0)+\'</span></div>\''
  + '  +\'<div class="c">逃逸强度<span class="v">\'+n(c.arousal,1)+\'</span></div>\''
  + '  +\'<div class="c">转向<span class="v">\'+n(c.turn,2)+\'</span></div>\''
  + '  +\'<div class="c">振翅<span class="v">\'+((c.motor||0)?\'开\':\'关\')+\'</span></div>\''
  + '  +gs.slice(0,8).map(g=>\'<div class="row"><span class="nm2">\'+g.base+(g.side?\'·\'+g.side:\'\')+\'</span><span class="v">\'+g.hz.toFixed(1)+\' Hz</span></div>\').join(\'\');'
  + '  let t=\'\';'
  + '  for(const g of gs){'
  + '   t+=\'<tr><td class="nm2">\'+g.base+(g.side?\'<span style="color:#6b7a99">·\'+g.side+\'</span>\':\'\')+\'</td>\''
  + '    +\'<td class="v">\'+g.hz.toFixed(1)+\' Hz</td>\''
  + '    +\'<td style="width:160px">\'+bar(Math.min(1,g.hz/80),hzC(g.hz))+\'</td>\''
  + '    +\'<td style="color:#6b7a99">\'+g.n+\' 个</td></tr>\';}'
  + '  $(\'neu\').innerHTML=t||\'<tr><td colspan="4" style="color:#8595b5">\'+(s.brainOk?\'等待脑侧数据…\':\'脑侧未连上——检查大脑的 --api 是否开启\')+\'</td></tr>\';'
  + '  const R=s.rms,L=s.low,M=s.mid,Hh=s.high,AX=(s.accel&&s.accel[0]);'
  + '  $(\'feat\').innerHTML='
  + '   \'<tr><td>总 RMS</td><td class="v">\'+(R==null?\'—\':R.toFixed(1))+\' dBFS</td><td>云端映射的输入，饱和说明门限太低</td></tr>\''
  + '  +\'<tr><td>低频 50-150 Hz</td><td class="v">\'+(L==null?\'—\':L.toFixed(1))+\' dBFS</td><td>JO-B 类神经元</td></tr>\''
  + '  +\'<tr><td>中频 250-450 Hz</td><td class="v">\'+(M==null?\'—\':M.toFixed(1))+\' dBFS</td><td>JO-A 类神经元</td></tr>\''
  + '  +\'<tr><td>高频 600-1500 Hz</td><td class="v">\'+(Hh==null?\'—\':Hh.toFixed(1))+\' dBFS</td><td>宽带／起始检测</td></tr>\''
  + '  +\'<tr><td>侧向加速度</td><td class="v">\'+(AX==null?\'—\':AX.toFixed(3))+\' g</td><td>决定左右转向</td></tr>\';'
  + ' }catch(e){}'
  + ' setTimeout(tick,500);'
  + '}'
  + 'tick();'
  + '</script></body></html>';

http.createServer((req, res) => {
  if (req.url === '/api' || req.url.startsWith('/api?')) {
    res.writeHead(200, {
      'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': 'no-store',
    });
    const merged = Object.assign({}, lastDiag || {}, lastBrain || {});
    // 诊断侧（fly_watch2）的 verdict 是「音频健康」判语，不用于顶部脑活动判定；
    // 顶部判定由脑侧 regions/flags 本地派生，避免两套口径打架。
    delete merged.verdict;
    merged.audioHealth = (lastDiag && lastDiag.verdict) || '';
    merged.audioDetail = (lastDiag && lastDiag.detail) || '';
    if (!lastBrain) {
      res.end(JSON.stringify(Object.assign({}, merged, {
        brainOk: false, groups: [], regions: [], cmd: {},
        detail: '脑侧 API 无响应——检查大脑的 --api 33338 是否开启',
      })));
      return;
    }
    res.end(JSON.stringify(Object.assign({}, merged, { brainOk: brainFail < 3 })));
    return;
  }
  res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
  res.end(PAGE);
}).listen(HTTP_PORT, '0.0.0.0', () => {
  console.log(`[看板] 网页 http://0.0.0.0:${HTTP_PORT}/`);
  console.log(`[看板] 脑侧 API ${BRAIN_API}` + (DIAG_API ? ` ｜ 诊断 API ${DIAG_API}` : ''));
});
