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
let nets=[], joined='', ip='', sel=null, other=0, first=1, setup=0,
    suffix='', dither='atkinson', ribbon='', running={};

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
      drawExtra();};
  });
  drawExtra();
}
/* The fields a join needs go under the list; what to do next goes in the
   row, beside the keys that do it. */
function drawExtra(){
  const e=$('extra'), say=$('wifisay');
  const pw=label=>'<label for=pw>'+label+'</label><input id=pw type=password autocomplete=off>';
  if(other){
    e.innerHTML='<label for=nm>network name</label><input id=nm autocomplete=off>'+pw('password');
    say.textContent='Type the name of a hidden network and its password, then press join.';
    enableJoin(1);
  }else if(!sel){
    e.innerHTML='';
    say.textContent=joined?'Connected to “'+joined+'”.':'Pick your network.';
    enableJoin(0);
  }else{
    const n=nets.find(x=>x.s===sel), open=n&&!n.a;
    e.innerHTML=open?'':pw(sel===joined?'new password':'password');
    say.textContent=sel===joined
      ?(open?'Connected to “'+sel+'”.':'Connected. If its password changed, type the new one and press join.')
      :(open?'Open network. No password needed. Press join.':'Type its password, then press join.');
    enableJoin(sel!==joined||!open);
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
  if(s.uri) $('uri').value=s.uri;
  /* The name and hostname the board is answering to now: a save takes
     effect at the next restart, and until then these are what it uses. */
  running={queue:s.queue||'', host:(s.host||'').replace('.local','')};

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
     cycles, so it is known while the printer is off too. It is named on the
     nameplate, and the halftone sheet prints in its inks. */
  const rib=s.ribbon==='colour'?'colour ribbon':s.ribbon==='black'?'black ribbon':'';
  prn.dataset.tip=(s.printer?'printer connected and ready'
                 :offline?'printer offline: off, unplugged or deselected'
                 :'printer busy')+(rib?', '+rib:'');
  $('rib').textContent=rib?' · '+rib:'';
  const g=+s.gain; let again=s.ribbon!==ribbon;
  if(g>=0&&g<50&&g!==GAIN){ setGain(g); again=1; }
  if(again){ ribbon=s.ribbon; render(); }
  lamp($('ledjob'),s.printing,'a',s.printing?'printing':'idle');
  if(!s.printing) $('ledjob').classList.remove('act-a');

  const was=joined; joined=setup?'':(s.ssid||''); ip=s.ip||'';
  if(joined!==was) drawExtra();
  /* Whatever else is known, the network this printer is on belongs in the
     list -- without waiting for a scan to confirm what it is already doing. */
  if(joined && !nets.some(n=>n.s===joined)){
    nets.unshift({s:joined, r:RSSI[s.signal]||-70, a:s.security!=='Open'});
    drawNets();
  }
  $('sec-halftone').classList.toggle('hide',setup);
  $('sec-printer').classList.toggle('hide',setup);
  $('sec-cable').classList.toggle('hide',!s.pic);

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
  const queue=$('queue').value, host=$('host').value;
  const ok=await post('/api/general',{queue:queue,host:host,dither:dither});
  noted('sec-printer', ok?'saved':'failed');
  /* Nothing on the page changes when a name is saved, and nothing on the
     network does either until the board restarts: say so, or it looks lost. */
  const later=ok&&(queue!==running.queue||host!==running.host);
  $('prnsay').textContent=later?'Saved. AirWriter uses the new name after it restarts: '
    +'unplug it for a few seconds, then plug it back in.':'';
};

/* TashTalk is the LocalTalk chip's firmware. An assembled AirWriter has it
   already, so the flash key stays out of sight until a cable test finds the
   chip without it: blank, or no longer running it. */
/* The port names as the panel prints them. */
const CABLE_HOW='Plug a printer cable into the “Serial” and “AppleTalk” ports and press the '
  +'“Test” button to see if the cable will work with AirWriter.';
$('cablesay').textContent=CABLE_HOW;
$('cabletest').onclick=async()=>{
  const b=$('cabletest'), say=$('cablesay'); if(b.disabled) return; b.disabled=true;
  let r=null;
  try{ r=await(await fetch('/api/cable',{method:'POST'})).json(); }catch(e){}
  b.disabled=false;
  if(!r){ noted('sec-cable','failed'); say.textContent='No answer from AirWriter.'; return; }
  const untested=r.ok&&['no_chip','no_tashtalk','chip_silent'].includes(r.verdict);
  noted('sec-cable', !r.ok?'failed':untested?'not tested':r.verdict==='pass'?'crossover':'no good');
  say.textContent = r.ok ? r.text : 'Could not test: '+r.error+'.';
  const blank=r.ok&&r.verdict==='no_tashtalk';
  $('flashkey').classList.toggle('hide',!blank);
  if(blank) say.textContent+=' Unplug the cable and press flash.';
};
$('ltflash').onclick=async()=>{
  const b=$('ltflash'), say=$('cablesay'); if(b.disabled) return; b.disabled=true;
  say.textContent='Flashing… about two seconds.';
  let r=null;
  try{ r=await(await fetch('/api/localtalk/flash',{method:'POST'})).json(); }catch(e){}
  b.disabled=false;
  if(!r){ noted('sec-cable','failed'); say.textContent='No answer from AirWriter.'; return; }
  noted('sec-cable', r.ok?'flashed':'failed');
  say.textContent = r.ok
    ? 'TashTalk 2.1.3 written and verified · PIC12F1840 rev '+r.rev+'. '
      +'Plug the cable back into both ports and press test again.'
    : cap(r.err)+(r.addr?' at '+r.addr+': wanted '+r.want+', read '+r.got:'')
      +(r.devid&&r.devid!=='0x0000'?' · device ID '+r.devid:'')+'.';
  if(r.ok) $('flashkey').classList.add('hide');
};

/* ── classic macs (W-1076, W-1078) ──────────────────────────────── */
/* Three ways in need nothing set: one sentence names them, and drops or
   qualifies one only when it is off. Infinite Mac is the one setting; its zone
   name works like a password, so the page makes one up rather than asking for
   one; it can be typed over to join a zone someone already uses, and its copy
   button saves while it is edited. Turning it off keeps the name. */
const ET_MAC='https://infinitemac.org/1998/Mac%20OS%208.1?appleTalk=';
const ET_ADJ=['quiet','amber','brisk','calm','dapper','eager','fuzzy','gentle','jolly','lucky','merry',
  'nimble','plucky','rosy','snug','sunny','tidy','witty','zesty','cosmic','dusty','golden','mellow',
  'rustic','silver','velvet','woolly','breezy','cheery','frosty','lively','spry'];
const ET_NOUN=['otter','badger','heron','lynx','marten','newt','owl','puffin','quail','robin','stoat',
  'tern','vole','wren','yak','zebra','bison','crane','dingo','egret','ferret','gecko','hare','ibis',
  'koala','lemur','moose','okapi','panda','raven','seal','tapir'];
const ET_SAY='On the Mac, choose AppleTalk ImageWriter in the Chooser.';
function etName(){
  const r=new Uint32Array(3); crypto.getRandomValues(r);
  return ET_ADJ[r[0]%ET_ADJ.length]+'-'+ET_NOUN[r[1]%ET_NOUN.length]+'-'+(1000+r[2]%9000);
}
let etZone='', etOn=false, etErr='';
function etDraw(e, lt){
  // The cable's node runs in DIP mode 2 (switch 2 on); the other two are on
  // unless turned off through /api/et/settings.
  const ways=[lt&&lt.on?'on the LocalTalk port':'on the LocalTalk port once DIP switch 2 is on'];
  if(!e||!e.on||e.radio.state!=='off') ways.push('from Ethernet Macs and BlueSCSI Wi-Fi Macs on this network');
  if(!e||!e.on||e.tunnel.state!=='off') ways.push('from Basilisk II with “Tunnel MacOS Networking over UDP” turned on');
  const last=ways.pop();
  $('etbody').textContent='Classic Macs print here over AppleTalk: '
    +(ways.length?ways.join(', ')+(ways.length>1?',':'')+' and ':'')+last+'.';
  const z=e&&e.on?e.zone:null;
  $('etzst').textContent = !etOn||!z ? ''
    : z.state==='online' ? 'Connected. '
    : z.state==='acquiring' ? 'Joining the zone… '
    : e.relay.last_error ? 'Can’t reach Infinite Mac ('+e.relay.last_error+'). Trying again. '
    : 'Connecting… ';
  $('etsay').textContent=etErr||ET_SAY;
}
function etZoneDraw(){
  $('etinf').checked=etOn;
  $('etzonebox').classList.toggle('hide',!etOn);
  $('etzone').value=etZone;
  $('etcopy').textContent='copy';
  $('etopen').href=ET_MAC+encodeURIComponent(etZone);
}
async function etPoll(){
  let e=null, lt=null;
  try{ e=await(await fetch('/api/et')).json(); }catch(x){}
  try{ lt=await(await fetch('/api/lt')).json(); }catch(x){}
  etDraw(e, lt);
}
async function etPost(body){
  etErr='';
  try{
    const r=await fetch('/api/et/settings',{method:'POST',body:JSON.stringify(body)});
    if(r.status===400) etErr='Zone names are 1 to 32 letters, digits, dashes, underscores and dots.';
    else if(!r.ok) etErr='Not saved.';
    else{ const s=await r.json(); etOn=!!s.infinite; etZone=s.zone||''; etZoneDraw(); }
  }catch(x){ etErr='No answer from AirWriter.'; }
  $('etsay').textContent=etErr||ET_SAY;
  if(!etErr) setTimeout(etPoll,1500);
  return !etErr;
}
fetch('/api/et/settings').then(r=>r.json()).then(s=>{
  etOn=!!s.infinite; etZone=s.zone||''; etZoneDraw(); etPoll();
}).catch(()=>etPoll());
$('etinf').onchange=async()=>{
  const on=$('etinf').checked, body={infinite:on};
  if(on&&!etZone) body.zone=etName();
  if(await etPost(body)) noted('sec-et', on?'Infinite Mac on':'Infinite Mac off');
  else $('etinf').checked=!on;
};
$('etnew').onclick=async()=>{
  if(!confirm('Make a new zone name? Infinite Macs using the old one will need the new one.')) return;
  if(await etPost({zone:etName()})) noted('sec-et','new name');
};
const etEdited=()=>$('etzone').value.trim()!==etZone;
async function etSaveName(){
  const z=$('etzone').value.trim();
  if(!z){ etZoneDraw(); return; }
  if(await etPost({zone:z})) noted('sec-et','saved');
}
$('etzone').oninput=()=>{ $('etcopy').textContent=etEdited()?'save':'copy'; };
$('etzone').onkeydown=e=>{ if(e.key==='Enter'){ e.preventDefault(); if(etEdited()) etSaveName(); } };
$('etcopy').onclick=async()=>{
  if(etEdited()){ etSaveName(); return; }
  const f=$('etzone'); if(!f.value) return;
  let ok=0;
  try{ await navigator.clipboard.writeText(f.value); ok=1; }
  catch(x){
    f.focus(); f.select(); f.setSelectionRange(0,f.value.length);
    try{ ok=document.execCommand('copy'); }catch(x2){}
  }
  noted('sec-et', ok?'name copied':'name selected');
};
setInterval(etPoll,5000);

/* ── footer ────────────────────────────────────────────────────────── */
/* The project's stars, asked of GitHub by the browser, not the board, and
   kept an hour like the network list. With no internet (setup mode, or a
   network without it) the link just says GitHub. */
const STAR='<svg aria-hidden="true" viewBox="0 0 16 16"><path fill="currentColor" d="M8 .25a.75.75 0 0 1 .673.418l1.882 3.815 4.21.612a.75.75 0 0 1 .416 1.279l-3.046 2.97.719 4.192a.751.751 0 0 1-1.088.791L8 12.347l-3.766 1.98a.75.75 0 0 1-1.088-.79l.72-4.194L.818 6.374a.75.75 0 0 1 .416-1.28l4.21-.611L7.327.668A.75.75 0 0 1 8 .25Z"/></svg>';
(async function stars(){
  const KEY='aw.stars'; let n=null;
  try{ const c=JSON.parse(localStorage.getItem(KEY));
    if(c && Date.now()-c.t < CACHE_MAX && typeof c.n==='number') n=c.n; }catch(e){}
  if(n===null) try{
    const ctl=new AbortController(); setTimeout(()=>ctl.abort(),6000);
    const r=await fetch('https://api.github.com/repos/wr/airwriter',{signal:ctl.signal});
    if(r.ok){ n=(await r.json()).stargazers_count;
      try{ localStorage.setItem(KEY,JSON.stringify({t:Date.now(),n:n})); }catch(e){} }
  }catch(e){}
  if(typeof n!=='number') return;
  $('stars').innerHTML=STAR+n.toLocaleString();
  $('gh').setAttribute('aria-label','AirWriter on GitHub, '+n+(n===1?' star':' stars'));
})();

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

/* The address is for adding the printer by hand, where AirPrint does not
   find it by itself. CUPS' own web interface is off by default on macOS, so a
   link to it would be a dead end. A page on plain http gets no clipboard API,
   so the field is selected and copied the old way. */
$('copy').onclick=async()=>{
  const f=$('uri'); if(!f.value) return;
  let ok=0;
  try{ await navigator.clipboard.writeText(f.value); ok=1; }
  catch(e){
    f.focus(); f.select(); f.setSelectionRange(0,f.value.length);
    try{ ok=document.execCommand('copy'); }catch(e2){}
  }
  noted('sec-printer', ok?'address copied':'address selected');
};

$('upd').onclick=()=>{ if(!FW_BUSY.includes(fwu.state)) $('fw').click(); };
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
  if(u.newer) t+=u.latest+' is available. Press update to install it.';
  else if(u.no_target) t+='The newest release has no image for this board.';
  else if(u.checked>=0 && !u.error) t+='Up to date, checked '+ago(u.checked)+'.';
  return t;
}
function fwDraw(u){
  fwu=u;
  $('fwsay').textContent=fwSay(u);
  const net=!u.setup&&u.key==='airwriter', busy=FW_BUSY.includes(u.state);
  $('fwupd').disabled=!net||busy;
  $('fwupdkey').classList.toggle('is-quiet',!net);
  $('fwupdkey').classList.toggle('is-lit',net&&!!u.newer);
  $('fwauto').disabled=!net;
  $('fwauto').checked=!!u.auto;
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
  }catch(e){ $('fwsay').textContent='No answer from AirWriter.'; return 0; }
}
$('fwupd').onclick=()=>fwPost(fwu.newer?'/api/update/install':'/api/update/check');
$('fwauto').onchange=async()=>{
  const box=$('fwauto'), on=box.checked;
  if(await fwPost('/api/update/auto',{auto:on}))
    noted('sec-firmware', on?'automatic updates on':'automatic updates off');
  else box.checked=!on;
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
 bluenoise:{name:'blue noise',title:'Blue noise',
  body:['A tile of noise with no pattern in it, grown','for this printer\'s tall dots. Nothing smears',
        'and nothing weaves, so tints come out even','and type stays where it was drawn. Try it',
        'against Atkinson on a photograph.']},
 threshold:{name:'threshold',title:'Threshold',
  body:['No halftone at all. Every dot is simply on','or off, so line art and logos come out',
        'exactly as drawn and anything with tone in','it collapses to black and white.',
        'Line art and logos only.']}};
const ORDER=['atkinson','floyd','bluenoise','bayer','threshold'];
const BAYER=[[0,32,8,40,2,34,10,42],[48,16,56,24,50,18,58,26],
 [12,44,4,36,14,46,6,38],[60,28,52,20,62,30,54,22],
 [3,35,11,43,1,33,9,41],[51,19,59,27,49,17,57,25],
 [15,47,7,39,13,45,5,37],[63,31,55,23,61,29,53,21]];

/* The sheet is drawn as a page would arrive, then split into ink the way
   the board splits it (ripjob/ink.c): on a black ribbon one plane of Rec. 601
   luma, on the colour ribbon yellow, magenta, cyan and black, with what the
   three colours would print in common going to black instead. 255 is bare
   paper, 0 full ink, as in the firmware. */
const INKS=[[250,215,0],[225,30,110],[0,150,210],[38,36,34]];
/* The page itself, drawn at the printer's 144 dpi: a sky, a ball and a few
   lines about the screen in use. Kept apart from the split so the very same
   art can be sent to the printer and held up against the sheet. */
function art(W,H,mode,colour){
  const c=document.createElement('canvas');c.width=W;c.height=H;
  const g=c.getContext('2d');
  g.fillStyle='#fff';g.fillRect(0,0,W,H);
  const grd=g.createLinearGradient(0,0,W,0);
  if(colour){
    /* a sky at dusk: tints the ribbon makes from a few dots of each ink,
       yellow through the yellow+magenta reds to a magenta+cyan blue */
    grd.addColorStop(0,'#fff6dc');grd.addColorStop(.22,'#ffd25c');
    grd.addColorStop(.46,'#ff8a52');grd.addColorStop(.7,'#e27aa6');
    grd.addColorStop(1,'#86b4e6');
  }else{grd.addColorStop(0,'#ffffff');grd.addColorStop(1,'#6b6b6b');}
  g.fillStyle=grd;g.fillRect(0,0,W,Math.round(H*.42));
  const r=Math.round(H*.16), cx=Math.round(W*.17), cy=Math.round(H*.21);
  const rg=g.createRadialGradient(cx-r*.35,cy-r*.35,r*.06,cx,cy,r);
  if(colour){rg.addColorStop(0,'#fff');rg.addColorStop(.45,'#ff2000');rg.addColorStop(1,'#500000');}
  else{rg.addColorStop(0,'#fff');rg.addColorStop(.5,'#8d8d8d');rg.addColorStop(1,'#161616');}
  g.fillStyle=rg;g.beginPath();g.arc(cx,cy,r,0,7);g.fill();
  /* the type scales with the page, about 15 points on paper, and the text
     is set to the measure, so a wider sheet shows more of it */
  const m=MODES[mode], fs=Math.round(W/36), pad=fs, lead=Math.round(fs*1.3);
  g.fillStyle='#111';
  g.font='italic '+Math.round(fs*1.45)+'px "Times New Roman", Times, serif';
  let y=Math.round(H*.42)+Math.round(fs*2.0);
  g.fillText(m.title, pad, y);
  g.font=fs+'px "Times New Roman", Times, serif';
  y+=Math.round(fs*1.7);
  const wrap=text=>{const lines=[]; let line='';
    for(const word of text.split(' ')){
      const t=line?line+' '+word:word;
      if(g.measureText(t).width>W-2*pad&&line){ lines.push(line); line=word; }
      else line=t;}
    if(line) lines.push(line); return lines;};
  for(const para of [m.body.join(' '), PAGE_NOTE]){
    /* as many whole sentences as fit on the sheet, never a cut one */
    const room=Math.floor((H-fs-y)/lead)+1, sentences=para.match(/[^.]+\.\s*/g)||[para];
    let lines=[], n=0;
    for(;n<sentences.length;n++){
      const l=wrap(sentences.slice(0,n+1).join('').trim());
      if(l.length>room) break;
      lines=l;
    }
    for(const l of lines){ g.fillText(l,pad,y); y+=lead; }
    y+=Math.round(lead*.5);
    if(n<sentences.length||y>H-fs) break;
  }
  return c;
}
const PAGE_NOTE='The ImageWriter II puts down 144 dots an inch across and 72 down, nine pins '
 +'at a time, and every one of them is either there or not. Everything with a tone in it, a '
 +'photograph, a shadow, the sky above, is made of dots the printer was told to leave out, and '
 +'the screen you pick here decides which ones. Colour comes from a four-band ribbon: yellow, '
 +'magenta and cyan laid one pass after another, and black on its own.';
function sourceAt(W,H,mode,colour){
  const d=art(W,H,mode,colour).getContext('2d').getImageData(0,0,W,H).data, n=colour?4:1,
        out=Array.from({length:n},()=>new Float32Array(W*H));
  for(let i=0;i<W*H;i++){
    const r=d[i*4],gg=d[i*4+1],b=d[i*4+2];
    if(colour){
      const mx=Math.max(r,gg,b);
      out[0][i]=255-(mx-b); out[1][i]=255-(mx-gg); out[2][i]=255-(mx-r); out[3][i]=mx;
    }else out[0][i]=.299*r+.587*gg+.114*b;
    /* a grain of noise keeps the diffusion screens from locking into a
       pattern on a smooth ramp; a mask screen would print it on bare paper */
    if(mode=='floyd'||mode=='atkinson')
      for(let k=0;k<n;k++) out[k][i]+=(Math.random()-.5)*1.6;
  }
  return out;
}

/* One plane through the chosen screen, as halftone.c does it. The tone
   curve first: a dot is as wide as its row, so a tint prints darker than its
   bits, and the curve asks for fewer dots in the midtones (GAIN is the dot
   gain at a half tint the board reports in its status). The error-diffusion
   screens scan serpentine and, told to `avoid` another ink's dots, print on
   one only when a quarter darker. The mask screens start their tile at a
   `phase` and read it upside down if asked, which keeps two inks apart. */
let GAIN=0;
const TONE=new Uint8Array(256);
function setGain(g){
  GAIN=g; const k=Math.log(1/(0.5-GAIN/100))/Math.log(2);
  for(let v=0;v<256;v++)TONE[v]=g>0?Math.min(255,Math.round(255*Math.pow(v/255,1/k))):v;
  TONE[0]=0;TONE[255]=255;
}
setGain(GAIN);
const MASK=Uint8Array.from(atob(BLUENOISE),c=>c.charCodeAt(0));
function halftone(p,W,H,mode,avoid,phase){
  const px=phase?phase[0]:0, py=phase?phase[1]:0, inv=phase?phase[2]:0;
  const add=(i,e)=>{if(i>=0&&i<W*H)p[i]+=e;};
  const bits=new Uint8Array(W*H), ed=mode=='floyd'||mode=='atkinson';
  for(let y=0;y<H;y++){
    const dir=ed&&(y&1)?-1:1;
    for(let i=0;i<W;i++){
      const x=dir>0?i:W-1-i, j=y*W+x;
      const v=TONE[Math.max(0,Math.min(255,Math.round(p[j])))];let on;
      if(mode=='bayer'){let b=BAYER[(y+py)&7][(x+px)&7]; if(inv)b=63-b; on=v<(b+.5)*4;}
      else if(mode=='bluenoise'){let t=MASK[((y+py)&63)*64+((x+px)&63)]; if(inv)t=255-t; on=v<255&&v<=t;}
      else if(mode=='threshold')on=v<128;
      else{
        const cut=avoid&&avoid[j]?64:128;
        on=v<cut;const err=v-(on?0:255);
        if(mode=='floyd'){
          add(x+dir>=0&&x+dir<W?j+dir:-1,err*7/16);
          if(y+1<H){add(x-dir>=0&&x-dir<W?j+W-dir:-1,err*3/16);add(j+W,err*5/16);
                    add(x+dir>=0&&x+dir<W?j+W+dir:-1,err/16);}
        }else{const e=err/8;
          add(x+dir>=0&&x+dir<W?j+dir:-1,e); add(x+2*dir>=0&&x+2*dir<W?j+2*dir:-1,e);
          if(y+1<H){add(x-dir>=0&&x-dir<W?j+W-dir:-1,e);add(j+W,e);add(x+dir>=0&&x+dir<W?j+W+dir:-1,e);}
          if(y+2<H)add(j+2*W,e);}
      }
      bits[j]=on?1:0;
    }
  }
  return bits;
}

function render(){
  const cv=$('prev'), host=cv.parentElement;
  if(!host.clientWidth) return;
  const dpr=Math.max(1,Math.round(window.devicePixelRatio||1));
  /* one device pixel per dot across: on a retina screen the sheet is about
     half the size of the page, which is how a print is looked at, and its
     width is nearly the printer's 1152 dots */
  const zoom=1, colour=ribbon==='colour';
  const cssW=Math.floor(host.clientWidth-2);
  const W=Math.floor(cssW*dpr/zoom), H=Math.round(W*0.34);
  /* The printer is 144 dpi across and 72 down: the pipeline box-filters every
     pair of source rows into one printed row, then prints each dot twice as
     tall as it is wide. Square dots at full resolution flatter it badly. */
  const src=sourceAt(W,H*2,dither,colour), n=src.length, planes=[];
  /* planes are yellow, magenta, cyan, black; screened black first, each
     keeping off the dots before it, on tiles black/cyan and magenta/yellow
     upside down to each other (ripjob.c ink_order, ink_phase) */
  const order=colour?[3,2,1,0]:[0];
  const phase=[[36,36,1],[36,36,0],[0,0,1],[0,0,0]];
  let avoid=colour?new Uint8Array(W*H):null;
  for(const k of order){
    const s=src[k], p=new Float32Array(W*H);
    for(let y=0;y<H;y++)for(let x=0;x<W;x++)
      p[y*W+x]=(s[(2*y)*W+x]+s[(2*y+1)*W+x])/2;
    const bits=halftone(p,W,H,dither,avoid,colour?phase[k]:null);
    planes[k]=bits;
    if(avoid)for(let i=0;i<W*H;i++)avoid[i]|=bits[i];
  }
  press(planes,W,H,colour?INKS:[INKS[3]],zoom,cv,dpr);
}

/* The sheet as it comes off the platen, not as the bitmap. A pin's dot is a
   round blob wider than its step: about 0.35 mm against 0.18 mm across and
   0.35 mm down. So neighbours run together, a fill comes out solid with a
   faint ribbing, and a tone is smoother than its bits. The ribbon never
   gives quite the same ink twice, each colour pass lands a hair off the
   last, and the paper is not quite white. */
const PAPER=[252,250,245];
function press(planes,W,H,inks,zoom,cv,dpr){
  const dw=zoom, dh=zoom*2, Wd=W*dw, Hd=H*dh;
  const r=dh*0.5, R=Math.ceil(r+1), kw=dw+2*R, kh=dh+2*R;
  const n=planes.length;
  const px=new Float32Array(Wd*Hd*3);
  for(let i=0;i<Wd*Hd;i++){
    const g=(Math.random()-.5)*6;
    px[i*3]=PAPER[0]+g; px[i*3+1]=PAPER[1]+g; px[i*3+2]=PAPER[2]+g;
  }
  const cov=new Float32Array(Wd*Hd);
  for(let k=0;k<n;k++){
    /* the colour passes are each a little off the black one */
    const ox=n>1&&k<n-1?(Math.random()-.5)*1.2:0, oy=n>1&&k<n-1?(Math.random()-.5)*.6:0;
    const kern=new Float32Array(kw*kh);
    for(let j=0;j<kh;j++)for(let i=0;i<kw;i++){
      const dx=i-R+.5-dw/2-ox, dy=j-R+.5-dh/2-oy;
      kern[j*kw+i]=Math.min(1,Math.max(0,r+.5-Math.hypot(dx,dy)));
    }
    cov.fill(0);
    const bits=planes[k];
    for(let y=0;y<H;y++)for(let x=0;x<W;x++){
      if(!bits[y*W+x]) continue;
      const dens=.8+.2*Math.random(), x0=x*dw-R, y0=y*dh-R;
      for(let j=0;j<kh;j++){const py=y0+j; if(py<0||py>=Hd) continue;
        for(let i=0;i<kw;i++){const qx=x0+i; if(qx<0||qx>=Wd) continue;
          cov[py*Wd+qx]+=kern[j*kw+i]*dens;}}
    }
    const tr=1-inks[k][0]/255, tg=1-inks[k][1]/255, tb=1-inks[k][2]/255;
    for(let i=0;i<Wd*Hd;i++){
      const a=cov[i]>1?1:cov[i]; if(!a) continue;
      px[i*3]*=1-a*tr; px[i*3+1]*=1-a*tg; px[i*3+2]*=1-a*tb;
    }
  }
  cv.width=Wd; cv.height=Hd;
  cv.style.width=(Wd/dpr)+'px'; cv.style.height=(Hd/dpr)+'px';
  const g=cv.getContext('2d'), img=g.createImageData(Wd,Hd), d=img.data;
  for(let i=0,o=0;i<Wd*Hd;i++,o+=4){
    d[o]=px[i*3]; d[o+1]=px[i*3+1]; d[o+2]=px[i*3+2]; d[o+3]=255;}
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
