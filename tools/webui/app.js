const $=i=>document.getElementById(i);

/* ── glyphs, struck on the pixel grid ──────────────────────────────── */
const TICK='<svg width=16 height=12 viewBox="0 0 8 6" fill=currentColor>'
 +'<rect x="0" y="2" width="1" height="1"/><rect x="1" y="3" width="1" height="1"/>'
 +'<rect x="2" y="4" width="1" height="1"/>'
 +'<rect x="3" y="3" width="1" height="1"/><rect x="4" y="2" width="1" height="1"/>'
 +'<rect x="5" y="1" width="1" height="1"/><rect x="6" y="0" width="1" height="1"/>'
 +'<rect x="7" y="0" width="1" height="1"/></svg>';
const LOCK='<svg width=14 height=16 viewBox="0 0 7 8" fill=currentColor>'
 +'<rect x="2" y="0" width="3" height="1"/><rect x="1" y="1" width="1" height="2"/>'
 +'<rect x="5" y="1" width="1" height="2"/><rect x="0" y="3" width="7" height="5"/>'
 +'<rect x="3" y="5" width="1" height="2" style="fill:var(--paper)"/></svg>';
function wf(n){
  const g=(lvl,r)=>'<g'+(lvl<=n?'':' class="off"')+'>'+r+'</g>';
  return '<svg width=16 height=14 viewBox="0 0 8 7" fill=currentColor>'
   +g(3,'<rect x="1" y="0" width="6" height="1"/><rect x="0" y="1" width="1" height="1"/>'
       +'<rect x="7" y="1" width="1" height="1"/>')
   +g(2,'<rect x="2" y="2" width="4" height="1"/><rect x="1" y="3" width="1" height="1"/>'
       +'<rect x="6" y="3" width="1" height="1"/>')
   +g(1,'<rect x="3" y="4" width="2" height="1"/>')
   +'<rect x="3" y="6" width="2" height="1"/></svg>';
}
const bars=r=>r>=-60?3:r>=-70?2:1;

/* ── state ─────────────────────────────────────────────────────────── */
let nets=[], joined='', ip='', sel=null, other=0, changing=0, first=1, setup=0,
    suffix='', dither='atkinson';

/* Scanning takes the radio off channel for a second and drops whatever is
   listening, so the last list is kept and reused. An hour is well past the
   point where a scan is worth the interruption. */
const CACHE='aw.nets', CACHE_MAX=3600e3;
function cacheLoad(){
  try{ const c=JSON.parse(localStorage.getItem(CACHE));
    if(c && Date.now()-c.t < CACHE_MAX && Array.isArray(c.nets)) return c.nets;
  }catch(e){}
  return null;
}
function cacheSave(list){
  try{ localStorage.setItem(CACHE,JSON.stringify({t:Date.now(),nets:list})); }catch(e){}
}
const RSSI={Excellent:-50,Good:-62,Fair:-70,Weak:-80};

const MODELS=[['ImageWriter II','imagewriter'],['ImageWriter','imagewriter'],
 ['ImageWriter LQ','imagewriter-lq']];
(function(){const d=$('devsel');
  for(const[n]of MODELS)d.append(new Option(n,n));
  d.append(new Option('Other…','__other__'));})();

/* ── wi-fi list ────────────────────────────────────────────────────── */
function drawNets(){
  let h='';
  for(const n of nets)
    h+='<button class=n data-s="'+n.s+'"><span class=t>'+(n.s===joined?TICK:'')+'</span>'
      +'<span class=s></span>'+(n.s===joined&&ip?'<span class=ip>'+ip+'</span>':'')
      +'<span class=l>'+(n.a?LOCK:'')+'</span>'+wf(bars(n.r))+'</button>';
  if(!nets.length)
    h='<button class="n empty" disabled><span class=t></span>'
     +'<span class=s>press scan to look for networks</span>'
     +'<span class=l></span></button>';
  h+='<button class=n data-s="__other__"><span class=t></span>'
    +'<span class=s>Other…</span><span class=l></span></button>';
  $('rows').innerHTML=h;
  const list=[...$('rows').children];
  list.forEach((b,i)=>{
    const n=nets[i]; if(n) b.querySelector('.s').textContent=n.s;
    if(!b.dataset.s) return;
    b.onclick=()=>{other=b.dataset.s==='__other__';sel=other?null:b.dataset.s;
      changing=0;drawExtra();};
  });
  drawExtra();
}
function drawExtra(){
  const e=$('extra');
  if(other){
    e.innerHTML='<label for=nm>network name</label><input id=nm autocomplete=off>'
      +'<label for=pw>password</label><input id=pw type=password autocomplete=off>';
    enableJoin(1);
  }else if(!sel){ e.innerHTML=''; enableJoin(0); }
  else if(sel===joined&&!changing){
    e.innerHTML='<p class=say>Connected. <a id=chg>change password</a></p>';
    $('chg').onclick=()=>{changing=1;drawExtra();}; enableJoin(0);
  }else{
    const n=nets.find(x=>x.s===sel);
    e.innerHTML=(n&&n.a)?'<label for=pw>password for &ldquo;'+sel+'&rdquo;</label>'
      +'<input id=pw type=password autocomplete=off>'
      :'<p class=say>Open network. No password needed.</p>';
    enableJoin(1);
  }
  for(const b of $('rows').children)
    if(b.dataset.s)
      b.setAttribute('aria-selected', b.dataset.s==='__other__'?!!other:b.dataset.s===sel);
}
function enableJoin(on){
  $('join').disabled=!on;
  $('joinkey').classList.toggle('is-quiet',!on);
}

async function scan(){
  const b=$('scan'), led=$('scanled'), slot=b.closest('.slot');
  b.disabled=1; slot.classList.add('is-held');
  const t=setInterval(()=>led.classList.toggle('act'),160);
  try{
    const raw=await(await fetch('/api/scan')).json();
    const best={};
    for(const n of raw){ if(!n.s) continue;
      if(!best[n.s]||n.r>best[n.s].r) best[n.s]=n; }
    nets=Object.values(best).sort((x,y)=>y.r-x.r);
    cacheSave(nets);
    other=0; drawNets();
  }catch(e){}
  clearInterval(t); led.classList.remove('act');
  slot.classList.remove('is-held'); b.disabled=0;
}
$('scan').onclick=scan;

/* ── status ────────────────────────────────────────────────────────── */
function lamp(el,on,cls,tip){
  el.classList.toggle(cls||'g',!!on);
  if(tip) el.dataset.tip=tip;
}
function draw(s){
  setup=!!s.setup;
  $('model').textContent=s.queue||'';
  $('job').textContent=s.job&&s.job!=='none yet'
    ? 'Last job — '+s.job : 'No job yet';
  $('ver').textContent=s.version||'';
  if(s.uri) $('uri').textContent=s.uri;

  /* A printer that is off or unplugged is not a fault -- the board is fine and
     says so on its panel in warm white. Faults are the two states that need a
     person: a job stalled mid-page, or one that failed. */
  const ps=s.ps||'', offline=ps==='offline';
  const fault=ps==='attention'||ps==='failed';
  lamp($('ledfault'),fault,'r',
    ps==='attention'?'needs attention: paper, jam or SELECT':
    ps==='failed'?'the last job failed':'no fault');
  lamp($('ledlink'),!setup,'g',setup?'not on a network':'joined “'+(s.ssid||'')+'”');
  const prn=$('ledprn');
  prn.classList.toggle('g',!!s.printer);
  prn.classList.remove('r');
  /* The ribbon is what the printer last answered to ESC ?, kept across power
     cycles, so it is known while the printer is off too. AirPrint follows it:
     colour pages print in colour on the colour ribbon and as gray on black. */
  const rib=s.ribbon==='colour'?', colour ribbon':s.ribbon==='black'?', black ribbon':'';
  prn.dataset.tip=(s.printer?'printer connected and ready'
                 :offline?'printer offline: off, unplugged or deselected'
                 :'printer busy')+rib;
  $('ribbon').textContent=
    s.ribbon==='colour'?'Colour ribbon. Colour pages print in colour, one pass per ink.'
   :s.ribbon==='black'?'Black ribbon. Colour pages print as gray.'
   :'Ribbon not read yet: the board asks the printer the next time it is ready.';
  lamp($('ledjob'),s.printing,'a',s.printing?'printing':'idle');
  if(!s.printing) $('ledjob').classList.remove('act-a');

  joined=setup?'':(s.ssid||''); ip=s.ip||'';
  /* Whatever else is known, the network this printer is on belongs in the
     list -- without waiting for a scan to confirm what it is already doing. */
  if(joined && !nets.some(n=>n.s===joined)){
    nets.unshift({s:joined, r:RSSI[s.signal]||-70, a:s.security!=='Open'});
    drawNets();
  }
  $('sec-halftone').classList.toggle('hide',setup);
  $('sec-printer').classList.toggle('hide',setup);
  $('sec-localtalk').classList.toggle('hide',!s.pic);

  if(s.host){const h=s.host.replace('.local','');
    const i=h.lastIndexOf('-'); if(i>0) suffix=h.slice(i+1);
    if(!$('host').value) $('host').value=h;}
  if(s.queue&&!$('queue').value){
    const known=MODELS.some(x=>x[0]===s.queue);
    $('queue').value=s.queue; $('devsel').value=known?s.queue:'__other__';
    if(!known){$('queue').classList.remove('hide');$('qlbl').classList.remove('hide');}}
  if(s.dither&&s.dither!==dither&&!$('tabs').dataset.touched){
    dither=s.dither; syncTabs(); render();}

  if(first){
    first=0;
    const c=cacheLoad();
    if(c && c.length){ nets=c; drawNets(); }
    /* Only interrupt the radio when there is nothing to show: a printer that
       is already on a network has nothing to look for. */
    else if(setup) scan();
  }
}
/* Every poll is a packet: blink the activity LED on each one. */
function poll(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    const a=$('ledact'); a.classList.add('act');
    setTimeout(()=>a.classList.remove('act'),110);
    draw(s);
  }).catch(()=>{});
}
poll(); setInterval(poll,4000);

/* ── printer ───────────────────────────────────────────────────────── */
$('devsel').onchange=()=>{const v=$('devsel').value, other=v==='__other__';
  $('queue').classList.toggle('hide',!other); $('qlbl').classList.toggle('hide',!other);
  if(other){$('queue').focus();return;}
  $('queue').value=v; $('model').textContent=v;
  const m=MODELS.find(x=>x[0]===v); if(m&&suffix) $('host').value=m[1]+'-'+suffix;};
$('queue').oninput=()=>{$('model').textContent=$('queue').value||'Printer';};

async function post(url,body){
  try{ await fetch(url,{method:'POST',body:JSON.stringify(body)}); return 1; }
  catch(e){ return 0; }
}
/* Saving is silent otherwise: say so on the heading, briefly, and put it back. */
function noted(sec,word){
  const h=$(sec).querySelector('h2');
  if(h.dataset.busy) clearTimeout(+h.dataset.busy);
  if(!h.dataset.name) h.dataset.name=h.textContent;
  h.textContent=h.dataset.name+' · '+word;
  h.dataset.busy=setTimeout(()=>{h.textContent=h.dataset.name;
    delete h.dataset.busy;},1800);
}
$('savegen').onclick=async()=>{
  const ok=await post('/api/general',
    {queue:$('queue').value,host:$('host').value,dither:dither});
  noted('sec-printer', ok?'saved':'failed');
};

$('ltflash').onclick=async()=>{
  const b=$('ltflash'); if(b.disabled) return; b.disabled=true;
  $('ltsay').textContent='Flashing… about two seconds.';
  let r=null;
  try{ r=await(await fetch('/api/localtalk/flash',{method:'POST'})).json(); }catch(e){}
  b.disabled=false;
  if(!r){ noted('sec-localtalk','failed'); $('ltsay').textContent='No answer from the dongle.'; return; }
  noted('sec-localtalk', r.ok?'done':'failed');
  $('ltsay').textContent = r.ok
    ? 'TashTalk 2.1.3 written and verified · PIC12F1840 rev '+r.rev+'.'
    : r.err+(r.addr?' at '+r.addr+': wanted '+r.want+', read '+r.got:'')
      +(r.devid&&r.devid!=='0x0000'?' · device ID '+r.devid:'')+'.';
};

$('cabletest').onclick=async()=>{
  const b=$('cabletest'); if(b.disabled) return; b.disabled=true;
  let r=null;
  try{ r=await(await fetch('/api/cable',{method:'POST'})).json(); }catch(e){}
  b.disabled=false;
  if(!r){ noted('sec-localtalk','failed'); $('cablesay').textContent='No answer from the dongle.'; return; }
  const untested=r.ok&&['no_chip','no_tashtalk','chip_silent'].includes(r.verdict);
  noted('sec-localtalk', !r.ok?'failed':untested?'not tested':r.verdict==='pass'?'crossover':'no good');
  $('cablesay').textContent = r.ok ? r.text : 'Could not test: '+r.error+'.';
};

/* ── overlay ───────────────────────────────────────────────────────── */
let ovh=0;
function overlay(title,text,secs){
  $('ovt').textContent=title; $('over').classList.remove('hide');
  let left=secs; clearInterval(ovh);
  const tick=()=>{$('ovs').textContent=text+'  ·  '+left+'s';
    if(left--<=0){clearInterval(ovh);location.reload();}};
  tick(); ovh=setInterval(tick,1000);
}
/* A refusal takes the overlay down, and its countdown with it. */
function unoverlay(){ clearInterval(ovh); $('over').classList.add('hide'); }
$('ovbtn').onclick=()=>location.reload();

$('join').onclick=async()=>{
  const name=other?($('nm')?$('nm').value:''):sel;
  if(!name) return;
  const pw=$('pw')?$('pw').value:'';
  $('join').disabled=1;
  await post('/api/wifi',{ssid:name,pass:pw});
  overlay('Restarting','Joining “'+name+'”',25);
};

/* CUPS' own web interface is disabled by default on macOS, so a link to it is
   a dead end. The address is the thing a person needs to add the printer. */
$('cups').onclick=async()=>{
  const uri=$('uri').textContent.trim(); if(!uri) return;
  try{ await navigator.clipboard.writeText(uri); $('cups').textContent='address copied'; }
  catch(e){ $('cups').textContent=uri; }
  setTimeout(()=>{$('cups').textContent='copy printer address';},2200);
};

$('upd').onclick=()=>$('fw').click();
$('fw').onchange=async()=>{
  const f=$('fw').files[0]; if(!f) return;
  overlay('Installing','Uploading '+(f.size>>10)+' KB',45);
  try{ const r=await fetch('/update',{method:'POST',body:f});
    if(!r.ok){unoverlay();alert('Rejected: '+await r.text());}
  }catch(e){ unoverlay(); alert('Upload failed.'); }
  $('fw').value='';
};

/* ── firmware ──────────────────────────────────────────────────────── */
/* Releases come from GitHub, and the board installs only what is signed with
   the AirWriter key. With auto off it never looks by itself: opening this page
   looks, at most once an hour, and so does the check key. */
let fwu={}, fwfast=0, fwblink=0;
const FW_BUSY=['checking','downloading','installing','restarting'];
const cap=s=>s?s[0].toUpperCase()+s.slice(1):'';
function ago(s){
  if(s<90) return 'just now';
  const m=Math.round(s/60); if(m<90) return m+' minutes ago';
  const h=Math.round(s/3600); if(h<36) return h+(h===1?' hour':' hours')+' ago';
  return Math.round(s/86400)+' days ago';
}
function fwSay(u){
  if(u.setup) return 'In setup mode the board is off the internet. Join a network to look for updates.';
  if(u.key==='other') return 'This firmware is signed with a different key, so it can’t install '
    +'AirWriter releases. Flash a release over USB to switch back.';
  if(u.key==='unsigned') return 'This firmware isn’t signed, so it can’t take updates. '
    +'Flash a release over USB.';
  switch(u.state){
    case 'checking': return 'Looking for a new release…';
    case 'downloading': return 'Downloading'+(u.progress>=0?': '+u.progress+'%.':'…');
    case 'installing': return 'Checking the signature…';
    case 'restarting': return 'Installed. Restarting…';
  }
  let t='Running '+u.version+(u.release?'. ':', a development build. ');
  if(u.error) t+=cap(u.error)+'. ';
  if(u.newer) t+=u.latest+' is available.';
  else if(u.no_target) t+='The newest release has no image for this board.';
  else if(u.checked>=0 && !u.error) t+='Up to date, checked '+ago(u.checked)+'.';
  return t;
}
function fwDraw(u){
  fwu=u;
  $('fwsay').textContent=fwSay(u);
  const net=!u.setup&&u.key==='airwriter', busy=FW_BUSY.includes(u.state);
  $('fwcheck').disabled=!net||busy;
  $('fwinst').disabled=!net||busy||!u.newer;
  $('fwinstkey').classList.toggle('is-quiet',$('fwinst').disabled);
  $('fwauto').disabled=!net;
  $('fwautokey').classList.toggle('is-lit',!!u.auto);
  $('fwauto').setAttribute('aria-pressed',!!u.auto);
  if(busy&&!fwblink) fwblink=setInterval(()=>$('fwled').classList.toggle('act'),160);
  if(!busy&&fwblink){ clearInterval(fwblink); fwblink=0; $('fwled').classList.remove('act'); }
  if(u.state==='restarting') overlay('Installing','Restarting into the new firmware',30);
  clearTimeout(fwfast); if(busy) fwfast=setTimeout(fwPoll,1500);
}
async function fwPoll(){
  try{ fwDraw(await(await fetch('/api/update')).json()); return 1; }catch(e){ return 0; }
}
/* The answer to a press, in the say line: the board's own sentence if it said no. */
async function fwPost(url,body){
  try{
    const r=await fetch(url,{method:'POST',body:body?JSON.stringify(body):''});
    if(!r.ok){ $('fwsay').textContent=cap(await r.text())+'.'; return 0; }
    setTimeout(fwPoll,300); return 1;
  }catch(e){ $('fwsay').textContent='No answer from the dongle.'; return 0; }
}
$('fwcheck').onclick=()=>fwPost('/api/update/check');
$('fwinst').onclick=()=>fwPost('/api/update/install');
$('fwauto').onclick=async()=>{
  const on=!fwu.auto;
  if(await fwPost('/api/update/auto',{auto:on})) noted('sec-firmware', on?'auto on':'auto off');
};
fwPoll().then(ok=>{
  if(ok && !fwu.setup && fwu.key==='airwriter' && !FW_BUSY.includes(fwu.state)
     && (fwu.checked<0 || fwu.checked>3600)) fwPost('/api/update/check');
});
setInterval(()=>{ if(!FW_BUSY.includes(fwu.state)) fwPoll(); },15000);

/* ── keys: press, hold, latch ──────────────────────────────────────── */
document.addEventListener('click',e=>{
  const key=e.target.closest('.key');
  if(!key||e.target.tagName==='BUTTON') return;
  const b=key.querySelector('button'); if(b&&!b.disabled) b.click();
});
/* The panel makes the noise the panel makes. Synthesised rather than sampled:
   nothing here may fetch a file, and a click is a filtered burst of noise
   anyway. A latching key clicks twice -- once going over centre, once seating
   in the detent -- and a momentary one gives a single hollow knock. */
/* Each voice is one filtered burst: freq, Q, gain, decay, delay. */
const SND={
  latch  :[4500,8  ,0.30,0.014,0    ],  /* latching key going over centre */
  strike :[4500,2.6,0.24,0.030,0    ],  /* momentary: the strike */
  body   :[ 150,1.4,0.13,0.075,0.003],  /* and the body under it */
  unlatch:[3000,7  ,0.15,0.018,0    ]   /* latching key seating on release */
};
const audio=(()=>{
  let ctx=null, noise=null;
  function init(){
    if(ctx) return;
    const AC=window.AudioContext||window.webkitAudioContext; if(!AC) return;
    ctx=new AC();
    const n=Math.floor(ctx.sampleRate*0.05);
    noise=ctx.createBuffer(1,n,ctx.sampleRate);
    const d=noise.getChannelData(0);
    for(let i=0;i<n;i++) d[i]=Math.random()*2-1;
  }
  function burst(freq,q,gain,decay,delay){
    if(!ctx) return;
    const t=ctx.currentTime+(delay||0);
    const src=ctx.createBufferSource(); src.buffer=noise;
    const bp=ctx.createBiquadFilter(); bp.type='bandpass';
    bp.frequency.value=freq; bp.Q.value=q;
    const g=ctx.createGain();
    g.gain.setValueAtTime(gain,t);
    g.gain.exponentialRampToValueAtTime(0.0001,t+decay);
    src.connect(bp).connect(g).connect(ctx.destination);
    src.start(t); src.stop(t+decay+0.02);
  }
  return {
    /* iOS will not start an audio context outside a gesture, so this is the
       gesture: the first press builds it. */
    press(latching){
      init(); if(!ctx) return;
      if(ctx.state==='suspended') ctx.resume();
      if(latching) burst(...SND.latch);
      else{ burst(...SND.strike); burst(...SND.body); }
    },
    release(latching){ if(latching) burst(...SND.unlatch); }
  };
})();

function down(e){
  const k=e.target.closest('.key'); if(!k) return;
  const slot=k.querySelector('.slot'), b=k.querySelector('button');
  if(!slot||!b||b.disabled) return;
  slot.classList.add('is-down');
  audio.press(!slot.classList.contains('momentary'));
}
function up(){
  for(const s of document.querySelectorAll('.slot.is-down')){
    s.classList.remove('is-down');
    audio.release(!s.classList.contains('momentary'));
  }
}
document.addEventListener('pointerdown',down);
document.addEventListener('pointerup',up);
document.addEventListener('pointercancel',up);

/* ── tooltips ──────────────────────────────────────────────────────── */
(function tips(){
  const tip=$('tip'); let timer=null;
  const hide=()=>{clearTimeout(timer);tip.classList.remove('on')};
  document.addEventListener('mouseover',e=>{
    const t=e.target.closest('[data-tip]'); if(!t) return;
    clearTimeout(timer);
    timer=setTimeout(()=>{
      const r=t.getBoundingClientRect();
      tip.textContent=t.dataset.tip;
      tip.style.left=(r.left+r.width/2)+'px';
      tip.style.top=(r.top-8)+'px';
      tip.classList.add('on');},1000);
  });
  document.addEventListener('mouseout',e=>{if(e.target.closest('[data-tip]'))hide()});
  addEventListener('scroll',hide,true);
})();

/* ── halftone ──────────────────────────────────────────────────────── */
const MODES={
 atkinson:{name:'atkinson',title:'Atkinson',
  body:['Bill Atkinson wrote this dither at Apple in','1984, for MacPaint on the first Macintosh.',
        'It throws away some of the error, which','keeps midtones open and small type',
        'readable. Best for most pages.']},
 floyd:{name:'floyd',title:'Floyd–Steinberg',
  body:['Published in 1976 and still the smoothest','gradient one ink can make. It pushes every',
        'scrap of error into its neighbours, so','photographs look their best and small type',
        'starts to thicken. Best for photographs.']},
 bayer:{name:'ordered',title:'Ordered',
  body:['A fixed 8 by 8 screen, the same one every','page. Nothing smears into anything else, so',
        'type stays exactly where it was drawn.','You can see the weave in flat areas.',
        'Best for small text.']},
 threshold:{name:'threshold',title:'Threshold',
  body:['No halftone at all. Every dot is simply on','or off, so line art and logos come out',
        'exactly as drawn and anything with tone in','it collapses to black and white.',
        'Line art and logos only.']}};
const ORDER=['atkinson','floyd','bayer','threshold'];
const BAYER=[[0,32,8,40,2,34,10,42],[48,16,56,24,50,18,58,26],
 [12,44,4,36,14,46,6,38],[60,28,52,20,62,30,54,22],
 [3,35,11,43,1,33,9,41],[51,19,59,27,49,17,57,25],
 [15,47,7,39,13,45,5,37],[63,31,55,23,61,29,53,21]];

function sourceAt(W,H,mode){
  const c=document.createElement('canvas');c.width=W;c.height=H;
  const g=c.getContext('2d');
  g.fillStyle='#fff';g.fillRect(0,0,W,H);
  const grd=g.createLinearGradient(0,0,W,0);
  grd.addColorStop(0,'#ffffff');grd.addColorStop(1,'#6b6b6b');
  g.fillStyle=grd;g.fillRect(0,0,W,Math.round(H*.42));
  const r=Math.round(H*.16), cx=Math.round(W*.17), cy=Math.round(H*.21);
  const rg=g.createRadialGradient(cx-r*.35,cy-r*.35,r*.06,cx,cy,r);
  rg.addColorStop(0,'#fff');rg.addColorStop(.5,'#8d8d8d');rg.addColorStop(1,'#161616');
  g.fillStyle=rg;g.beginPath();g.arc(cx,cy,r,0,7);g.fill();
  const m=MODES[mode], fs=22, pad=22;
  g.fillStyle='#111';
  g.font='italic '+Math.round(fs*1.45)+'px "Times New Roman", Times, serif';
  g.fillText(m.title, pad, Math.round(H*.42)+Math.round(fs*2.0));
  g.font=fs+'px "Times New Roman", Times, serif';
  m.body.forEach((t,i)=>g.fillText(t, pad,
    Math.round(H*.42)+Math.round(fs*3.7)+i*Math.round(fs*1.30)));
  const d=g.getImageData(0,0,W,H).data,out=new Float32Array(W*H);
  for(let i=0;i<W*H;i++)
    out[i]=.299*d[i*4]+.587*d[i*4+1]+.114*d[i*4+2]+(Math.random()-.5)*1.6;
  return out;
}

function render(){
  const cv=$('prev'), host=cv.parentElement;
  if(!host.clientWidth) return;
  const dpr=Math.max(1,Math.round(window.devicePixelRatio||1));
  const zoom=2;
  const cssW=Math.floor(host.clientWidth-2);
  const W=Math.floor(cssW*dpr/zoom), H=Math.round(W*0.34);
  /* The printer is 144 dpi across and 72 down: the pipeline box-filters every
     pair of source rows into one printed row, then prints each dot twice as
     tall as it is wide. Square dots at full resolution flatter it badly. */
  const src=sourceAt(W,H*2,dither);
  const p=new Float32Array(W*H);
  for(let y=0;y<H;y++)for(let x=0;x<W;x++)
    p[y*W+x]=(src[(2*y)*W+x]+src[(2*y+1)*W+x])/2;

  const add=(i,e)=>{if(i>=0&&i<W*H)p[i]+=e;};
  const mode=dither, bits=new Uint8Array(W*H);
  for(let y=0;y<H;y++)for(let x=0;x<W;x++){
    const i=y*W+x,v=p[i];let black;
    if(mode=='bayer')black=v<(BAYER[y&7][x&7]+.5)*4;
    else if(mode=='threshold')black=v<128;
    else{
      black=v<128;const err=v-(black?0:255);
      if(mode=='floyd'){
        if(x+1<W)add(i+1,err*7/16);
        if(y+1<H){if(x)add(i+W-1,err*3/16);add(i+W,err*5/16);if(x+1<W)add(i+W+1,err/16);}
      }else{const e=err/8;
        if(x+1<W)add(i+1,e); if(x+2<W)add(i+2,e);
        if(y+1<H){if(x)add(i+W-1,e);add(i+W,e);if(x+1<W)add(i+W+1,e);}
        if(y+2<H)add(i+2*W,e);}
    }
    bits[i]=black?1:0;
  }
  const dw=zoom, dh=zoom*2;
  cv.width=W*dw; cv.height=H*dh;
  cv.style.width=(W*dw/dpr)+'px'; cv.style.height=(H*dh/dpr)+'px';
  const g=cv.getContext('2d'), img=g.createImageData(W*dw,H*dh);
  for(let y=0;y<H;y++)for(let x=0;x<W;x++){
    const on=bits[y*W+x]?0:255;
    for(let sy=0;sy<dh;sy++){
      let o=((y*dh+sy)*W*dw + x*dw)*4;
      for(let sx=0;sx<dw;sx++,o+=4){
        img.data[o]=img.data[o+1]=img.data[o+2]=on; img.data[o+3]=255;}
    }
  }
  g.putImageData(img,0,0);
}
function buildTabs(){
  $('tabs').innerHTML=ORDER.map(k=>
    '<div class=key data-k="'+k+'"><span class=legend>'+MODES[k].name+'</span>'
    +'<span class=slot><button data-k="'+k+'" aria-label="'+MODES[k].name+'">'
    +'</button></span></div>').join('');
  [...$('tabs').children].forEach(d=>d.querySelector('button').onclick=e=>{
    dither=e.currentTarget.dataset.k;
    $('tabs').dataset.touched=1;
    syncTabs(); render();
    post('/api/general',{dither:dither}).then(ok=>
      noted('sec-halftone', ok?'saved':'failed'));
  });
  syncTabs();
}
function syncTabs(){
  for(const d of $('tabs').children){
    const on=d.dataset.k===dither;
    d.classList.toggle('is-lit',on);
    d.querySelector('button').setAttribute('aria-pressed',on);
  }
}
let rt;
const redraw=()=>{clearTimeout(rt);rt=setTimeout(render,120)};
addEventListener('resize',redraw);
/* The sheet's width changes with the layout, not only with the window: a
   phone applies its own metrics after first paint, and a canvas sized before
   that is the wrong width for the rest of its life. */
if(window.ResizeObserver) new ResizeObserver(redraw).observe($('prev').parentElement);
drawNets(); buildTabs(); render();
