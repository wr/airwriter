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
    h='<button class=n disabled><span class=t></span>'
     +'<span class=s>no networks found</span><span class=l></span></button>';
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

  lamp($('ledfault'),s.fault,'r',s.fault?'fault':'no fault');
  lamp($('ledlink'),!setup,'g',setup?'not on a network':'joined “'+(s.ssid||'')+'”');
  lamp($('ledprn'),s.printer,'g',s.printer?'printer connected and online':'no printer');
  lamp($('ledjob'),s.printing,'a',s.printing?'printing':'idle');
  if(!s.printing) $('ledjob').classList.remove('act-a');

  joined=setup?'':(s.ssid||''); ip=s.ip||'';
  $('sec-halftone').classList.toggle('hide',setup);
  $('sec-printer').classList.toggle('hide',setup);

  if(s.host){const h=s.host.replace('.local','');
    const i=h.lastIndexOf('-'); if(i>0) suffix=h.slice(i+1);
    if(!$('host').value) $('host').value=h;}
  if(s.queue&&!$('queue').value){
    const known=MODELS.some(x=>x[0]===s.queue);
    $('queue').value=s.queue; $('devsel').value=known?s.queue:'__other__';
    if(!known){$('queue').classList.remove('hide');$('qlbl').classList.remove('hide');}}
  if(s.dither&&s.dither!==dither&&!$('tabs').dataset.touched){
    dither=s.dither; syncTabs(); render();}

  if(first){ first=0; scan(); if(!setup&&s.ssid) sel=null; }
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
$('savegen').onclick=()=>post('/api/general',
  {queue:$('queue').value,host:$('host').value,dither:dither});

/* ── overlay ───────────────────────────────────────────────────────── */
function overlay(title,text,secs){
  $('ovt').textContent=title; $('over').classList.remove('hide');
  let left=secs;
  const tick=()=>{$('ovs').textContent=text+'  ·  '+left+'s';
    if(left--<=0){clearInterval(h);location.reload();}};
  tick(); const h=setInterval(tick,1000);
}
$('ovbtn').onclick=()=>location.reload();

$('join').onclick=async()=>{
  const name=other?($('nm')?$('nm').value:''):sel;
  if(!name) return;
  const pw=$('pw')?$('pw').value:'';
  $('join').disabled=1;
  await post('/api/wifi',{ssid:name,pass:pw});
  overlay('Restarting','Joining “'+name+'”',25);
};

$('upd').onclick=()=>$('fw').click();
$('fw').onchange=async()=>{
  const f=$('fw').files[0]; if(!f) return;
  overlay('Installing','Uploading '+(f.size>>10)+' KB',45);
  try{ const r=await fetch('/update',{method:'POST',body:f});
    if(!r.ok){$('over').classList.add('hide');alert('Rejected: '+await r.text());}
  }catch(e){ $('over').classList.add('hide'); alert('Upload failed.'); }
};

/* ── keys: press, hold, latch ──────────────────────────────────────── */
document.addEventListener('click',e=>{
  const key=e.target.closest('.key');
  if(!key||e.target.tagName==='BUTTON') return;
  const b=key.querySelector('button'); if(b&&!b.disabled) b.click();
});
function down(e){
  const k=e.target.closest('.key'); if(!k) return;
  const slot=k.querySelector('.slot'), b=k.querySelector('button');
  if(slot&&b&&!b.disabled) slot.classList.add('is-down');
}
function up(){ for(const s of document.querySelectorAll('.slot.is-down'))
  s.classList.remove('is-down'); }
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
    post('/api/general',{dither:dither});
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
let rt; addEventListener('resize',()=>{clearTimeout(rt);rt=setTimeout(render,150)});
drawNets(); buildTabs(); render();
