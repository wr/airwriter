const $=i=>document.getElementById(i), $$=s=>[...document.querySelectorAll(s)];

/* ── helpers ───────────────────────────────────────────────────────── */
const cap=s=>s?s[0].toUpperCase()+s.slice(1):'';
const plural=(n,one,many)=>n+' '+(n===1?one:many);
/* Every ask has a time limit: a board that has gone away never answers at
   all, and a fetch would wait for minutes. */
function fetchIn(url,ms,opt){
  const c=new AbortController(), t=setTimeout(()=>c.abort(),ms||8000);
  return fetch(url,Object.assign({signal:c.signal,cache:'no-store'},opt)).finally(()=>clearTimeout(t));
}
async function getJSON(url,ms){
  try{ const r=await fetchIn(url,ms); return r.ok?await r.json():null; }catch(e){ return null; }
}
/* A POST and its answer: the parsed JSON, or {error} with the board's own
   sentence when it said no. */
async function postJSON(url,body){
  try{
    const r=await fetchIn(url,30000,{method:'POST',body:body===undefined?'':JSON.stringify(body)});
    const t=await r.text();
    if(!r.ok) return {error:cap(t.trim()||('error '+r.status))};
    try{ return JSON.parse(t); }catch(e){ return {ok:true}; }
  }catch(e){ return {error:'No answer from AirWriter.'}; }
}
/* What a pane has to say, for a moment, or until the next thing it says. */
const sayTimers={};
function say(id,text,err,ms){
  const el=$(id); if(!el) return;
  el.textContent=text||''; el.classList.toggle('err',!!err);
  clearTimeout(sayTimers[id]);
  if(ms) sayTimers[id]=setTimeout(()=>{ el.textContent=''; el.classList.remove('err'); },ms);
}
function lamp(el,cls){ el.className='lamp'+(cls?' '+cls:''); }
/* A page on plain http gets no clipboard API: select the text instead.
   Says 'Copied', or 'Selected' when it could only select it. */
async function copyText(el){
  const v=el.value!==undefined?el.value:el.textContent; if(!v) return '';
  let ok=0;
  try{ await navigator.clipboard.writeText(v); ok=1; }
  catch(e){
    if(el.select){ el.focus(); el.select(); }
    else{ const r=document.createRange(); r.selectNodeContents(el); getSelection().removeAllRanges(); getSelection().addRange(r); }
    try{ ok=document.execCommand('copy'); }catch(e2){}
  }
  return ok?'Copied':'Selected';
}
async function copyFrom(el,btn){
  const said=await copyText(el); if(!said) return;
  btn.textContent=said;
  setTimeout(()=>btn.textContent='Copy',1500);
}
/* A field's limit is the board's, in bytes, and an emoji is four of them. */
const enc=new TextEncoder();
function byteCap(el,n){
  el.addEventListener('input',()=>{
    let v=el.value; while(enc.encode(v).length>n) v=[...v].slice(0,-1).join('');
    if(v!==el.value) el.value=v;
  });
}
function ago(s){
  if(s<60) return 'now';
  const m=Math.round(s/60); return m<60?m+' min ago':Math.round(m/60)+' h ago';
}

/* ── halftone ──────────────────────────────────────────────────────── */
/* The sample sheet is the old page's own renderer, unchanged: the art, the
   split into inks, the screens and the press, as the board does them. */
let dither='atkinson', ribbon='';
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

/* ── panes ─────────────────────────────────────────────────────────── */
/* One pane at a time; on a phone the sidebar is the first screen and a pane
   slides over it. The pane is in the address, so a reload stays put. */
const PANES=['printer','airprint','appletalk','wifi','about'];
const phone=()=>matchMedia('(max-width:760px)').matches;
let pane='';
function go(id){
  if(!PANES.includes(id)) id='printer';
  pane=id;
  for(const p of $$('.pane')) p.classList.toggle('on',p.id==='p-'+id);
  for(const b of $$('.nav [data-go]'))
    if(b.dataset.go===id) b.setAttribute('aria-current','page'); else b.removeAttribute('aria-current');
  document.body.classList.add('in');
  document.querySelector('.detail').scrollTop=0;
  try{ history.replaceState(null,'','#'+id); }catch(e){}
  if(id==='printer') requestAnimationFrame(render);
  if(id==='appletalk') atPoll();
  if(id==='about') fwPoll();
}
for(const b of $$('[data-go]')) b.onclick=()=>go(b.dataset.go);
document.querySelector('.back').onclick=()=>{
  document.body.classList.remove('in');
  try{ history.replaceState(null,'',location.pathname); }catch(e){}
};

/* dialogs: a key with data-dlg opens one, data-close shuts it, and so does a
   click on the dimmed page around it */
for(const b of $$('[data-dlg]')) b.onclick=()=>$(b.dataset.dlg).showModal();
for(const b of $$('[data-close]')) b.onclick=()=>b.closest('dialog').close();
for(const d of $$('dialog')) d.addEventListener('click',e=>{ if(e.target===d&&d.id!=='d-wait') d.close(); });
/* While AirWriter restarts: the page waits for it to answer, then reloads.
   After 90 s it says what to do instead, and keeps listening. */
function waitFor(title,html,reload,late){
  $('wait-t').textContent=title; $('wait-s').innerHTML=html; $('wait-keys').classList.add('hide');
  for(const d of $$('dialog')) if(d.open) d.close();
  $('d-wait').showModal();
  if(!reload) return;
  const t0=Date.now();
  setTimeout(function again(){
    fetchIn('/api/status',4000).then(r=>{ if(r.ok) location.reload(); else throw 0; })
      .catch(()=>{
        if(Date.now()-t0>90000&&$('wait-keys').classList.contains('hide')){
          $('wait-s').innerHTML=late||'AirWriter hasn’t come back. Check its lights, then reload this page.';
          $('wait-keys').classList.remove('hide');
        }
        setTimeout(again,3000);
      });
  },6000);
}
$('reload').onclick=()=>location.reload();

/* ── status ────────────────────────────────────────────────────────── */
let S={}, setup=false, first=true;
/* Offline is off, unplugged or deselected: the board can't tell them apart. */
const PS={
  ready:['g','Ready'], printing:['g','Printing'], attention:['a','Check paper and SELECT'],
  failed:['r','The last job failed'], offline:['','Check power, cable and SELECT'], booting:['','Starting'],
  joining:['','Joining Wi-Fi']};
const SIDE_PS={attention:'Check paper', failed:'Failed', offline:'Offline', printing:'Printing'};
const MODELS=['ImageWriter II','ImageWriter','ImageWriter LQ'];
const BARS={Excellent:3,Good:3,Fair:2,Weak:1};
const RESET={'power-on':'Plugged in','software':'After a restart','panic':'After a crash',
  'task watchdog':'After a crash','interrupt watchdog':'After a crash','watchdog':'After a crash',
  'brownout':'After the power dipped','reset pin':'Reset','usb':'Over USB','deep sleep':'Woke up'};

function draw(s){
  S=s; setup=!!s.setup;
  // the printer; in setup mode the board's state is the setup, so the line says
  const ps=s.ps==='setup'?(s.printing?'printing':s.printer?'ready':'offline'):s.ps;
  const [cls,text]=PS[ps]||['',cap(s.state||'')];
  lamp($('prn-lamp'),cls); $('prn-state').textContent=text;
  $('v-printer').textContent=SIDE_PS[ps]||(ps==='ready'?'Ready':'');
  const colour=s.ribbon==='colour';
  $('rib-colour').classList.toggle('hide',!colour);
  $('rib').textContent=colour?'Colour':s.ribbon==='black'?'Black':'Not known yet';
  if(s.ribbon!==ribbon){ ribbon=s.ribbon; render(); }
  const g=+s.gain; if(g>=0&&g<50&&g!==GAIN){ setGain(g); render(); }
  if(document.activeElement!==$('model')){
    const m=$('model'), want=s.pmodel||MODELS[0];
    if(![...m.options].some(o=>o.value===want)) m.append(new Option(want,want));
    m.value=want;
  }
  $('pname').placeholder=s.pmodel||MODELS[0];
  if(document.activeElement!==$('pname')) $('pname').value=s.pname||'';
  if(document.activeElement!==$('screen')&&s.dither&&s.dither!==dither){ dither=s.dither; $('screen').value=dither; render(); }
  // AirPrint
  $('ap').checked=!!s.airprint;
  $('v-airprint').textContent=s.airprint?'On':'Off';
  $('uri').textContent=s.uri||'';
  // Wi-Fi; in setup mode a network name is the one it could not join
  const missed=setup&&s.ssid;
  $('ssid').textContent=missed?'Couldn’t join “'+s.ssid+'”':setup?'Not set up':(s.ssid||'Not connected');
  $('ssid').title=$('ssid').textContent;
  lamp($('wifi-lamp'),missed?'r':setup||s.signal==='Weak'?'a':s.ssid?'g':'');
  const n=BARS[s.signal]||0, locked=s.security&&s.security!=='Open';
  $('wifi-glyphs').innerHTML=setup?'':(locked?'<svg><use href="#g-lock"/></svg>':'')
    +(n?'<svg viewBox="0 0 16 16">'+wifiArcs(n)+'</svg>':'');
  $('change').textContent=setup?'Choose…':'Change…';
  $('iprow').classList.toggle('hide',setup);
  $('ip').textContent=s.ip||'';
  if(document.activeElement!==$('host')) $('host').value=s.phost||(s.host||'').replace(/\.local$/,'');
  $('v-wifi').textContent=missed?'Couldn’t join':setup?'Not set up':(s.ssid||'');
  // what waits for a restart
  const pending=(s.pname||s.pmodel)&&((s.pname||s.pmodel)!==s.queue);
  const hostPending=s.phost&&s.phost+'.local'!==s.host;
  if(pending) say('prn-say','Saved. Macs and iPhones see the new name after a restart.');
  else if($('prn-say').dataset.pending) say('prn-say','');
  $('prn-say').dataset.pending=pending?1:'';
  if(hostPending) say('wifi-say','Saved. The new host name works after a restart.');
  // About
  $('uptime').textContent=upFor(s.uptime);
  $('reset').textContent=RESET[s.reset]||cap(s.reset||'');
  drawDips(s.dip,s.lt_port);
  $('apname').textContent=s.ap||'AirWriter';
  // in setup mode only the printer, Wi-Fi and About make sense
  for(const id of ['airprint','appletalk'])
    document.querySelector('.nav [data-go="'+id+'"]').classList.toggle('hide',setup);
  if(first){
    first=false;
    const want=(location.hash||'').slice(1);
    if(setup){ go('wifi'); openNets(); }
    else if(PANES.includes(want)) go(want);
    else { go('printer'); if(phone()) document.body.classList.remove('in'); }
  }
  atDraw();
}
function wifiArcs(n){
  const arc=(lvl,d)=>'<path fill=none stroke=currentColor stroke-width=1.7 stroke-linecap=round'
    +(lvl<=n?'':' stroke-opacity=.25')+' d="'+d+'"/>';
  return arc(3,'M1.07 6.67A9.8 9.8 0 0 1 14.93 6.67')+arc(2,'M3.33 8.93A6.6 6.6 0 0 1 12.67 8.93')
    +arc(1,'M5.6 11.2A3.4 3.4 0 0 1 10.4 11.2')+'<circle cx=8 cy=13.6 r=1.05 fill=currentColor />';
}
function upFor(u){
  const m=/(\d+)h (\d+)m/.exec(u||''); if(!m) return '';
  const h=+m[1], mi=+m[2], d=Math.floor(h/24);
  if(d) return plural(d,'day','days')+(h%24?' '+plural(h%24,'hour','hours'):'');
  return (h?plural(h,'hour','hours')+' ':'')+plural(mi,'minute','minutes');
}
/* Two asks in a row with no answer, and the page says so rather than show
   what it last heard as if it were now. */
let misses=0, lost=false;
function setLost(on){ lost=on; $('lost').classList.toggle('hide',!on); }
function poll(){
  getJSON('/api/status',6000).then(s=>{
    if(s){ misses=0; if(lost) setLost(false); draw(s); }
    else if(++misses>=2&&!$('d-wait').open) setLost(true);
  });
}
poll(); setInterval(()=>{ if(!document.hidden) poll(); },4000);
setTimeout(()=>document.body.classList.add('ready'),1500);

/* ── printer ───────────────────────────────────────────────────────── */
for(const m of MODELS) $('model').append(new Option(m,m));
async function savePrinter(body,okText){
  const r=await postJSON('/api/general',body);
  if(r.error) say('prn-say',r.error,1);
  else { say('prn-say',okText,0,1600); setTimeout(poll,300); }
}
$('model').onchange=()=>{ $('pname').placeholder=$('model').value; savePrinter({model:$('model').value},'Saved.'); };
/* A blank name is the model's name. */
$('pname').onchange=()=>savePrinter({name:$('pname').value.trim()},'Saved.');
$('pname').onkeydown=e=>{ if(e.key==='Enter') $('pname').blur(); };
byteCap($('pname'),31);
for(const k of ['atkinson','floyd','bluenoise','bayer','threshold']) $('screen').append(new Option(MODES[k].title,k));
$('screen').onchange=()=>{ dither=$('screen').value; render(); savePrinter({dither:dither},'Saved. The next page prints with it.'); };
$('testpage').onclick=async()=>{
  if(S.ps==='offline'||S.ps==='setup'&&!S.printer){ say('prn-say','The printer is offline. Check power, cable and SELECT.',1,4000); return; }
  const r=await postJSON('/api/test');
  say('prn-say',r.error?r.error+'.':'Printing a test page.',!!r.error,4000);
};
/* The cable test borrows both ports. TashTalk, the AppleTalk chip's firmware,
   comes on an assembled AirWriter, so its flash key shows only once a test
   finds the chip without it. */
/* The picture runs the test as the panel shows it: a pulse along the cable
   while it tests, then the cable green for a crossover or red for anything else. */
function ports(state){ $('ports').setAttribute('class','ports'+(state?' '+state:'')); }
$('cabletest').onclick=async()=>{
  const b=$('cabletest'); if(b.disabled) return; b.disabled=true;
  say('cable-say',''); ports('testing');
  const r=await postJSON('/api/cable');
  b.disabled=false;
  if(r.error||!r.ok){
    ports('fail');
    say('cable-say',r.error&&!/^ESP_/.test(r.error)?cap(r.error).replace(/\.?$/,'.'):'Couldn’t run the test.',1);
    return;
  }
  ports(r.verdict==='pass'?'pass':'fail');
  say('cable-say',r.text,r.verdict!=='pass');
  $('ltflash').classList.toggle('hide',r.verdict!=='no_tashtalk');
};
document.querySelector('[data-dlg=d-cable]').onclick=()=>{ ports(''); say('cable-say',''); $('d-cable').showModal(); };
$('ltflash').onclick=async()=>{
  const b=$('ltflash'); if(b.disabled) return; b.disabled=true;
  say('cable-say','Flashing… about two seconds.');
  const r=await postJSON('/api/localtalk/flash');
  b.disabled=false;
  if(r.error){ say('cable-say',r.error,1); return; }
  if(r.ok){ say('cable-say','TashTalk is on the chip. Plug the cable back into both ports and test again.'); b.classList.add('hide'); }
  else say('cable-say',cap(r.err)+(r.addr?' at '+r.addr:'')+'.',1);
};

/* ── AirPrint ──────────────────────────────────────────────────────── */
$('ap').onchange=async()=>{
  const on=$('ap').checked, r=await postJSON('/api/general',{airprint:on});
  if(r.error){ $('ap').checked=!on; say('ap-say',r.error,1); }
  else { $('v-airprint').textContent=on?'On':'Off'; setTimeout(poll,300); }
};
$('copyuri').onclick=()=>copyFrom($('uri'),$('copyuri'));

/* ── AppleTalk ─────────────────────────────────────────────────────── */
/* Four ways in, one card each. The cable is the LocalTalk node's; the other
   three are EtherTalk links. A Mac is "connected" when its link heard it in
   the last 10 minutes: AppleTalk has no connection to watch. */
let LT=null, ET=null, ETS=null;
const WAYS={
  lt:   {card:'c-lt'},
  radio:{card:'c-radio', key:'radio', side:'radio'},
  tun:  {card:'c-tun', key:'tunnel', side:'tunnel'},
  zone: {card:'c-zone', key:'infinite', side:'zone'}};
const MAC='<svg><use href="#g-mac"/></svg>';
const esc=t=>String(t).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c]);
/* A Mac by its name from Sharing Setup when it gave one (System 7 on), its
   address beside the time; else by its address. */
function macList(macs,addr){
  const n=macs.length;
  let h='<span class=count>'+(n?plural(n,'Mac','Macs')+' connected':'No Macs connected')+'</span>';
  if(n) h+='<ul class=macs>'+macs.map(m=>'<li>'+MAC+'<span>'+esc(m.name||addr(m))+'</span><small>'
    +(m.name?esc(addr(m))+' · ':'')+ago(m.ago)+'</small></li>').join('')+'</ul>';
  return h;
}
function stHTML(cls,html,why){ return '<span class=st'+(why?' title="'+esc(why)+'"':'')+'><i class="lamp '+cls+'"></i><span>'+html+'</span></span>'; }
function stLine(cls,text,why){ return stHTML(cls,esc(text),why); }
/* A card's body is redrawn only when it changes, so a selection in it lasts. */
function put(card,h){ const b=card.querySelector('.body'); if(b._h!==h){ b._h=h; b.innerHTML=h; } }
/* Infinite Mac's zone name, in a chip that copies it: it is what a friend
   types into Infinite Mac to print here. */
function zoneChip(){
  if(!ETS||!ETS.zone) return 'the zone';
  return '<span class=chip role=button tabindex=0 title="Copy the zone name" data-zone><span>'+esc(ETS.zone)
    +'</span><svg aria-hidden=true><use href="#g-copy"/></svg></span>';
}
/* The note goes in beside the chip, not through a redraw, which would lose
   the selection; 'Selected' stays up longer, waiting for a Command-C. */
let zoneT=0;
async function copyZone(chip){
  const said=await copyText(chip.firstChild); if(!said) return;
  let n=chip.parentNode.querySelector('.copied');
  if(!n){ n=document.createElement('small'); n.className='copied'; chip.after(' ',n); }
  n.textContent=said;
  clearTimeout(zoneT); zoneT=setTimeout(()=>n.remove(),said==='Copied'?1500:6000);
}
$('c-zone').addEventListener('click',e=>{ const c=e.target.closest('[data-zone]'); if(c) copyZone(c); });
$('c-zone').addEventListener('keydown',e=>{
  const c=e.target.closest('[data-zone]'); if(c&&(e.key==='Enter'||e.key===' ')){ e.preventDefault(); copyZone(c); }
});
/* W-1081: the board asks U7 at boot whether it runs TashTalk. Without it
   the port can't run, so the card says why in place of a switch, and offers
   the flash where the chip only needs TashTalk. A build with no LocalTalk
   chip pins has no port to show. */
const CHIP_HELP=' <a href="https://github.com/wr/airwriter/wiki/LocalTalk-Chip-(TashTalk)" target=_blank rel=noopener>Learn&nbsp;More…</a>';
const CHIP={
  no_chip:['r','No LocalTalk chip.'+CHIP_HELP],
  no_tashtalk:['a','The LocalTalk chip needs TashTalk.'],
  silent:['r','The LocalTalk chip isn’t answering.'+CHIP_HELP]};
let flashing=false;
function atDraw(){
  let macs=0, on=0;
  // the cable
  {
    const c=$('c-lt'), sw=c.querySelector('[data-way=lt]'), live=!!(LT&&LT.on);
    const chip=!live&&CHIP[S.lt_chip];
    c.classList.toggle('hide',S.pic===false);
    $('tt-credit').classList.toggle('hide',S.pic===false);
    sw.checked=live; sw.disabled=!S.pic;
    sw.closest('.sw').classList.toggle('hide',!!chip);
    c.classList.toggle('off',!live&&!chip);
    $('lt-flash').classList.toggle('hide',!flashing&&!(chip&&S.lt_chip==='no_tashtalk'));
    if(chip) put(c,flashing?stLine('a','Flashing TashTalk…'):stHTML(chip[0],chip[1]));
    else if(live){
      on++; macs+=(LT.macs||[]).length;
      put(c,(LT.phase!=='online'?stLine('a','Joining the AppleTalk network…'):'')
        +macList(LT.macs||[],m=>'Node '+m.node));
    }
  }
  for(const w of ['radio','tun','zone']){
    const W=WAYS[w], c=$(W.card), sw=c.querySelector('[data-way='+w+']');
    const set=ETS?!!ETS[W.key]:false, side=ET&&ET.on?ET[W.side]:null;
    sw.checked=set; sw.disabled=!ETS;
    c.classList.toggle('off',!set);
    if(!set||!side) continue;
    on++;
    let h='';
    if(w==='zone'){
      h+=side.state==='online'?stHTML('g','Connected to '+zoneChip())
        :side.state==='acquiring'?stHTML('a','Joining '+zoneChip()+'…')
        :ET.relay&&ET.relay.last_error?stLine('r','Can’t reach infinitemac.org. Trying again.',ET.relay.last_error)
        :stLine('a','Connecting to infinitemac.org…');
    }else if(side.state==='no address') h+=stLine('a','Waiting for a network address');
    else if(side.state==='no socket') h+=stLine('r','Couldn’t open UDP port 6066');
    else if(side.state==='acquiring') h+=stLine('a','Joining the AppleTalk network…');
    const list=side.macs||[]; macs+=list.length;
    h+=macList(list,m=>m.ip?m.ip:m.addr);
    put(c,h);
  }
  $('v-appletalk').textContent=macs?plural(macs,'Mac','Macs'):on?'On':'Off';
}
async function atPoll(){
  const [lt,et]=await Promise.all([getJSON('/api/lt'),getJSON('/api/et')]);
  // no answer from either: keep what they last said (the board may be gone)
  if(lost||(!lt&&!et&&!setup)) return;
  LT=lt; ET=et;
  if(!ETS&&!setup) return etsLoad();
  atDraw();
}
async function etsLoad(){ ETS=await getJSON('/api/et/settings'); atDraw(); }
etsLoad(); atPoll();
setInterval(()=>{ if(!document.hidden&&!setup) atPoll(); },5000);
for(const sw of $$('[data-way]')) sw.onchange=async()=>{
  const w=sw.dataset.way, on=sw.checked;
  let r;
  if(w==='lt'){
    r=await postJSON('/api/general',{localtalk:on});
    // the port's node takes a moment: show it starting, not the last answer
    if(!r.error) LT=on?(LT&&LT.on?LT:{on:true,phase:'acquiring',macs:[]}):{on:false,quiet:true};
  }
  else{
    const body={}; body[WAYS[w].key]=on;
    // Infinite Mac's zone is a made-up name until someone types one: anyone
    // who knows it can print, so it should not be guessable
    if(w==='zone'&&on&&ETS&&!ETS.zone) body.zone=zoneName();
    r=await postJSON('/api/et/settings',body);
    if(!r.error) ETS=r;
  }
  if(r.error){ sw.checked=!on; say('at-say',r.error,1); }
  else { say('at-say',''); atDraw(); setTimeout(atPoll,1500); setTimeout(poll,600); }
};
/* The card's flash key, for a chip that needs TashTalk: once it is on, the
   board starts the port by itself. */
$('lt-flash').onclick=async()=>{
  const b=$('lt-flash'); if(b.disabled) return; b.disabled=true;
  flashing=true; say('at-say',''); atDraw();
  const r=await postJSON('/api/localtalk/flash');
  flashing=false; b.disabled=false;
  if(r.error||!r.ok) say('at-say',r.error||cap(r.err)+(r.addr?' at '+r.addr:'')+'.',1);
  else {
    S.lt_chip='ok';
    if(S.lt_port===1) LT={on:true,phase:'acquiring',macs:[]};
    setTimeout(atPoll,1500); setTimeout(poll,600);
  }
  atDraw();
};
/* Infinite Mac's zone: like a password, so it is only ever on this page. */
const IM='https://infinitemac.org/1998/Mac%20OS%208.1?appleTalk=';
function zoneLink(){ $('imopen').href=IM+encodeURIComponent($('zone').value.trim()); }
document.querySelector('[data-dlg=d-inf]').onclick=()=>{
  $('zone').value=(ETS&&ETS.zone)||zoneName();
  zoneLink(); say('zone-say',''); $('d-inf').showModal();
};
$('zone').oninput=zoneLink;
$('copyzone').onclick=()=>copyFrom($('zone'),$('copyzone'));
const ADJ=['quiet','amber','brisk','calm','dapper','eager','fuzzy','gentle','jolly','lucky','merry',
  'nimble','plucky','rosy','snug','sunny','tidy','witty','zesty','cosmic','dusty','golden','mellow',
  'rustic','silver','velvet','woolly','breezy','cheery','frosty','lively','spry'];
const NOUN=['otter','badger','heron','lynx','marten','newt','owl','puffin','quail','robin','stoat',
  'tern','vole','wren','yak','zebra','bison','crane','dingo','egret','ferret','gecko','hare','ibis',
  'koala','lemur','moose','okapi','panda','raven','seal','tapir'];
function zoneName(){
  const r=new Uint32Array(3); crypto.getRandomValues(r);
  return ADJ[r[0]%ADJ.length]+'-'+NOUN[r[1]%NOUN.length]+'-'+(1000+r[2]%9000);
}
$('newzone').onclick=()=>{ $('zone').value=zoneName(); zoneLink(); };
$('zonedone').onclick=async()=>{
  const z=$('zone').value.trim();
  if(!z||(ETS&&z===ETS.zone)){ $('d-inf').close(); return; }
  const r=await postJSON('/api/et/settings',{zone:z});
  if(r.error){ say('zone-say','Zone names are 1 to 32 letters, digits, dashes, underscores and dots.',1); return; }
  ETS=r; atDraw(); setTimeout(atPoll,1500); $('d-inf').close();
};

/* ── Wi-Fi ─────────────────────────────────────────────────────────── */
/* A scan takes the radio off its channel for a second or two, so the last
   list is kept for an hour and reused. */
const CACHE='aw.nets';
function cacheLoad(){ try{ const c=JSON.parse(localStorage.getItem(CACHE)); if(c&&Date.now()-c.t<3600e3) return c.nets; }catch(e){} return null; }
function cacheSave(n){ try{ localStorage.setItem(CACHE,JSON.stringify({t:Date.now(),nets:n})); }catch(e){} }
let nets=[], pick=null, scanned=false;
/* the board's own words for a signal, as bars (main.c status_json) */
const barsOf=r=>r>-67?3:r>-75?2:1;
function drawNets(){
  const cur=setup?'':S.ssid;
  let h='';
  for(const n of nets){
    h+='<button class="row link" type=button data-s="'+esc(n.s)+'"><span class=txt>'+esc(n.s)+'</span>'
      +(n.s===cur?'<span class=val>Connected</span>':'')
      +'<span class=glyphs>'+(n.a?'<svg><use href="#g-lock"/></svg>':'')
      +'<svg viewBox="0 0 16 16">'+wifiArcs(barsOf(n.r))+'</svg></span></button>';
  }
  if(!nets.length&&scanned) h+='<div class="row none"><span class=txt>No networks found</span></div>';
  h+='<button class="row link" type=button data-s=""><span class=txt>Other Network…</span></button>';
  $('nets').innerHTML=h;
  for(const b of $$('#nets [data-s]')) b.onclick=()=>choose(b.dataset.s,b);
}
function choose(ssid,btn){
  for(const b of $$('#nets [data-s]')) b.style.background='';
  btn.style.background='var(--hover)';
  pick=ssid;
  const n=nets.find(x=>x.s===ssid), other=ssid==='';
  $('joinform').classList.toggle('hide',!other&&n&&!n.a);
  $('nm').classList.toggle('hide',!other);
  $('pw').value=''; $('nm').value='';
  canJoin();
  (other?$('nm'):n&&n.a?$('pw'):$('join')).focus();
}
/* A locked network needs its password, unless it is the one AirWriter is on:
   a blank password there keeps the one it has. */
function canJoin(){
  const n=nets.find(x=>x.s===pick), cur=setup?'':S.ssid;
  $('join').disabled=pick===null||(pick===''?!$('nm').value.trim():!!(n&&n.a&&pick!==cur&&!$('pw').value));
}
$('pw').oninput=canJoin; $('nm').oninput=canJoin;
byteCap($('nm'),32);
/* While it looks: a row at the top whose arcs light one after another. */
function scanning(on){
  const box=$('nets'); box.classList.toggle('busy',on);
  const row=box.querySelector('.scanning');
  if(on&&!row) box.insertAdjacentHTML('afterbegin','<div class="row scanning"><span class=txt>Looking for networks…</span>'
    +'<span class=glyphs><svg viewBox="0 0 16 16"><path class=a3 fill=none stroke=currentColor stroke-width=1.7 stroke-linecap=round d="M1.07 6.67A9.8 9.8 0 0 1 14.93 6.67"/>'
    +'<path class=a2 fill=none stroke=currentColor stroke-width=1.7 stroke-linecap=round d="M3.33 8.93A6.6 6.6 0 0 1 12.67 8.93"/>'
    +'<path class=a1 fill=none stroke=currentColor stroke-width=1.7 stroke-linecap=round d="M5.6 11.2A3.4 3.4 0 0 1 10.4 11.2"/>'
    +'<circle cx=8 cy=13.6 r=1.05 fill=currentColor /></svg></span></div>');
  if(!on&&row) row.remove();
}
let netErr=false;
async function scan(){
  if(netErr) say('net-say',''); $('rescan').disabled=true; scanning(true);
  let raw=null, err='No answer from AirWriter.';
  try{ const r=await fetchIn('/api/scan',20000); if(r.ok) raw=await r.json(); else err='Couldn’t look for networks. Try again.'; }catch(e){}
  $('rescan').disabled=false; scanning(false);
  netErr=!raw;
  if(!raw){ say('net-say',err,1); return; }
  const best={};
  for(const n of raw){ if(n.s&&(!best[n.s]||n.r>best[n.s].r)) best[n.s]=n; }
  nets=Object.values(best).sort((x,y)=>y.r-x.r); scanned=true;
  if(nets.length) cacheSave(nets);
  drawNets();
}
function openNets(){
  pick=null; $('join').disabled=true; $('joinform').classList.add('hide');
  const c=cacheLoad(); nets=c||[]; scanned=!!c;
  say('net-say',setup&&S.ssid?'Couldn’t join “'+S.ssid+'”. Check the password, or choose another network.':'',setup&&S.ssid);
  netErr=false;
  drawNets(); $('d-net').showModal();
  if(!c) scan();
}
$('change').onclick=openNets;
$('rescan').onclick=scan;
$('join').onclick=async()=>{
  const ssid=pick===''?$('nm').value.trim():pick; if(!ssid) return;
  $('join').disabled=true;
  const r=await postJSON('/api/wifi',{ssid:ssid,pass:$('pw').value});
  if(r.error){ say('net-say',r.error,1); $('join').disabled=false; return; }
  // From its own network the page cannot follow it: say where it will be.
  const q='“'+esc(ssid)+'”', host=esc(S.host||'airwriter.local'), link='<a href="http://'+host+'/">'+host+'</a>';
  waitFor('Joining “'+ssid+'”',setup?'AirWriter restarts on it. Join '+q+' here too, then open '+link+'.'
      :'AirWriter restarts on the new network. This page reloads when it answers there.',true,
    'No answer here yet. On '+q+', open '+link+'. If AirWriter couldn’t join, it makes its own network, '
      +esc($('apname').textContent)+', to try again.');
};
$('pw').onkeydown=e=>{ if(e.key==='Enter') $('join').click(); };
/* A host name is letters, digits and dashes. */
$('host').oninput=()=>{ const h=$('host'), v=h.value.toLowerCase().replace(/[^a-z0-9-]/g,''); if(v!==h.value) h.value=v; };
$('host').onchange=async()=>{
  const v=$('host').value.replace(/^-+|-+$/g,''); if(!v) return;
  const r=await postJSON('/api/general',{host:v});
  if(r.error) say('wifi-say',r.error,1); else setTimeout(poll,300);
};
$('host').onkeydown=e=>{ if(e.key==='Enter') $('host').blur(); };

/* ── About: software, restart, erase, DIP switches ─────────────────── */
/* Releases come from GitHub, and the board installs only what is signed with
   the AirWriter key. Opening About looks for one at most once an hour. */
let U={}, fwFast=0, fwChecked=false;
const FW_BUSY=['checking','downloading','installing','restarting'];
function fwDraw(u){
  U=u;
  $('fw-ver').textContent=(u.version||'').replace(/-dirty$/,'');
  const busy=FW_BUSY.includes(u.state), net=!u.setup&&u.key==='airwriter';
  let cls='g', text='Up to date';
  if(u.setup){ cls=''; text='Join a network to look for updates'; }
  else if(u.key!=='airwriter'){ cls='a'; text=u.key==='other'?'Signed with another key':'Not signed'; }
  else if(u.state==='checking'){ cls='a'; text='Looking…'; }
  else if(u.state==='downloading'){ cls='a'; text='Downloading'+(u.progress>=0?' '+u.progress+'%':'…'); }
  else if(u.state==='installing'){ cls='a'; text='Checking the signature…'; }
  else if(u.state==='restarting'){ cls='a'; text='Restarting…'; }
  else if(u.state==='failed'&&u.error){ cls='r'; text=cap(u.error); }
  else if(u.newer){ cls='a'; text=u.latest+' is available'; }
  else if(u.error){ cls='r'; text=cap(u.error); }
  else if(u.checked<0){ cls=''; text='Not checked yet'; }
  lamp($('fw-lamp'),cls); $('fw-state').textContent=text;
  $('fw-btn').textContent=u.newer?'Install':'Check Now';
  $('fw-btn').disabled=!net||busy;
  // a build signed with another key never updates from GitHub
  $('fwauto').checked=!!u.auto&&(u.setup||u.key==='airwriter'); $('fwauto').disabled=!net;
  $('v-about').textContent=u.newer?'Update':'';
  if(u.state==='restarting'&&!$('d-wait').open)
    waitFor('Installing','Restarting into '+esc(u.latest||'the new version')+'. This page reloads when AirWriter is back.',true);
  clearTimeout(fwFast); if(busy) fwFast=setTimeout(fwPoll,1500);
}
async function fwPoll(){
  const u=await getJSON('/api/update'); if(!u) return;
  fwDraw(u);
  if(!fwChecked&&pane==='about'&&!u.setup&&u.key==='airwriter'&&!FW_BUSY.includes(u.state)&&(u.checked<0||u.checked>3600)){
    fwChecked=true; fwPost('/api/update/check');
  }
}
async function fwPost(url,body){
  const r=await postJSON(url,body);
  if(r.error) say('about-say',r.error+'.',1); else setTimeout(fwPoll,300);
  return !r.error;
}
$('fw-btn').onclick=()=>fwPost(U.newer?'/api/update/install':'/api/update/check');
$('fwauto').onchange=async()=>{ const on=$('fwauto').checked; if(!await fwPost('/api/update/auto',{auto:on})) $('fwauto').checked=!on; };
$('fw-file').onclick=()=>$('fw').click();
$('fw').onchange=async()=>{
  const f=$('fw').files[0]; if(!f) return;
  waitFor('Installing','Uploading '+(f.size>>10)+' KB…',false);
  try{
    const r=await fetch('/update',{method:'POST',body:f});
    if(!r.ok){ $('d-wait').close(); say('about-say',cap((await r.text()).trim())+'.',1); }
    else waitFor('Installing','Restarting into the new version. This page reloads when AirWriter is back.',true);
  }catch(e){ $('d-wait').close(); say('about-say','The upload failed.',1); }
  $('fw').value='';
};
fwPoll(); setInterval(()=>{ if(!document.hidden&&!FW_BUSY.includes(U.state)) fwPoll(); },15000);
$('restart').onclick=async()=>{
  const r=await postJSON('/api/restart');
  if(r.error){ $('d-restart').close(); say('about-say',r.error,1); return; }
  waitFor('Restarting',S.printing?'After this page. This page reloads when AirWriter is back.':'This page reloads when AirWriter is back.',true);
};
$('erase').onclick=async()=>{
  const r=await postJSON('/api/erase');
  if(r.error){ $('d-erase').close(); say('about-say',r.error,1); return; }
  waitFor('Erasing','AirWriter is starting over. Join the '+esc($('apname').textContent)+' network to set it up again.',false);
};
/* The bank as it sits on the board, switch 1 on the left, and what each
   group of switches does now. */
function drawDips(bits,ltPort){
  $('dipgrp').classList.toggle('hide',!bits||bits.length!==8);
  if(!bits||bits.length!==8) return;
  const on=i=>bits[i]==='1';
  $('dips').innerHTML='<div class=dipbody><b>ON</b>'
    +[...bits].map((b,i)=>'<span class="dip'+(b==='1'?' on':'')+'"><i></i>'+(i+1)+'</span>').join('')+'</div>';
  /* what each switch does, then what it is set to: the AppleTalk port's pair
     stops counting once the port's own switch has been used */
  const mode=(on(0)?1:0)|(on(1)?2:0), yn=x=>x?'On':'Off';
  const rows=[
    ['AppleTalk port','1, 2', ltPort>=0?'<a href="#appletalk" data-goto=appletalk>Set in AppleTalk</a>':yn(mode===2)],
    ['Developer mode','3', yn(on(2))],
    ['Test page at power-on','4', yn(on(3))],
    ['Halftone','5, 6', 'Not used yet'],
    ['Rainbow status lights','7', yn(on(6))],
    ['Spare','8', 'Not used']];
  $('dip-rows').innerHTML=rows.map(r=>'<div class=row><span class=txt>'+r[0]+'<span class=num>'+r[1]+'</span></span><span class=val>'+r[2]+'</span></div>').join('');
  for(const a of $$('#dip-rows [data-goto]')) a.onclick=e=>{ e.preventDefault(); go(a.dataset.goto); };
}


let rt; addEventListener('resize',()=>{ clearTimeout(rt); rt=setTimeout(()=>{ if(pane==='printer') render(); },150); });
