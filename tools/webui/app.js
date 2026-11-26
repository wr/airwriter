const $=i=>document.getElementById(i), $$=s=>[...document.querySelectorAll(s)];

/* The colour theme is the board's (About). The browser keeps the last one it
   was told, so a reload starts in it and not in the standard colours. */
const THEMES=['snow','yellow','bondi'];   // Platinum is the page's own
function setTheme(t){
  const r=document.documentElement;
  if(!THEMES.includes(t)) t='';
  if(t) r.dataset.theme=t; else delete r.dataset.theme;
  $$('#tp button').forEach(b=>b.setAttribute('aria-checked',b.dataset.t===t));
  try{ localStorage.setItem('theme',t); }catch(e){}
}
try{ setTheme(localStorage.getItem('theme')); }catch(e){ setTheme(''); }

/* ── helpers ───────────────────────────────────────────────────────── */
let SIGNIN=false;
const cap=s=>s?s[0].toUpperCase()+s.slice(1):'';
const plural=(n,one,many)=>n+' '+(n===1?one:many);
/* Every ask has a time limit: a board that has gone away never answers at
   all, and a fetch would wait for minutes. */
function fetchIn(url,ms,opt){
  // W-1156: once the board says sign in, every other ask waits for the reload signing in makes
  if(SIGNIN&&url.indexOf('/api/auth/')!==0) return new Promise(()=>{});
  const c=new AbortController(), t=setTimeout(()=>c.abort(),ms||8000);
  return fetch(url,Object.assign({signal:c.signal,cache:'no-store'},opt)).finally(()=>clearTimeout(t))
    .then(r=>{ if(r.status===401&&url.indexOf('/api/auth/')!==0)
      r.clone().json().then(b=>needSignin(b.label!==false),()=>needSignin(true)); return r; });
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
  }catch(e){ return {error:'Can’t reach AirWriter.'}; }
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
        'scrap of error into its neighbors, so','photographs look their best and small type',
        'starts to thicken. Best for photographs.']},
 bayer:{name:'ordered',title:'Ordered',
  body:['A fixed 8 by 8 screen, the same one every','page. Nothing smears into anything else, so',
        'type stays exactly where it was drawn.','You can see the weave in flat areas.',
        'Best for small text.']},
 bluenoise:{name:'blue noise',title:'Blue noise',
  body:['A tile of noise with no pattern in it, grown','for this printer’s tall dots. Nothing smears',
        'and nothing weaves, so tints come out even','and type stays where it was drawn. Try it',
        'against Atkinson on a photograph.']},
 threshold:{name:'threshold',title:'Threshold',
  body:['No halftone at all. Every dot is on or','off, so line art and logos come out',
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
const PAGE_NOTE='The ImageWriter II prints 144 dots an inch across and 72 down, nine pins at a '
 +'time, and every dot is either there or not. Every tone in a photo is a pattern of dots, and '
 +'the halftone you pick here sets the pattern. Color comes from a four-band ribbon, with yellow, '
 +'magenta and cyan one pass after another, then black.';
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
/* ── the log (W-1136) ──────────────────────────────────────────────── */
/* Opens in place on About, as a printer's details do. Every line goes in as text, never as
   markup: a Wi-Fi library's message or a Mac's reply can hold anything. */
const LG={open:false,lines:[],divs:[],last:0,timer:0,busy:false,began:0,run:null,head:null};
const LOG_KEEP=500, LOG_COPY=300, LV={I:'INFO',W:'WARN',E:'ERROR'};
function logParse(t){
  const out={head:null,lines:[]};
  for(const l of t.split('\n')){
    if(!l) continue;
    const f=l.split('\t');
    if(f[0]==='#'){ out.head={newest:+f[1],errors:+f[2],dropped:+f[3]}; continue; }
    if(f.length>=6) out.lines.push({seq:+f[0],run:+f[1],ms:+f[2],lv:f[3],tag:f[4],text:f.slice(5).join('\t')});
  }
  return out;
}
/* Time since that start: +04:53, +26:14:32. A plus so that nobody reads it as a time of day. */
const logHms=ms=>{ const s=Math.floor(ms/1000), h=s/3600|0, p=v=>String(v).padStart(2,'0'); return '+'+(h?h+':':'')+p((s%3600)/60|0)+':'+p(s%60); };
const logWhy=l=>{ const m=/^reset=(.*) version=/.exec(l.text); return m?m[1]:''; };
function logAgo(){
  const m=/(\d+)h (\d+)m/.exec(S.uptime||''); if(!m) return '';
  const h=+m[1], mi=+m[2];
  return h?h+' h '+mi+' min ago':mi?mi+' min ago':'just now';
}
/* When this start began, by the clock of the phone or computer showing the page. Uptime comes
   in whole minutes, so the sum wanders by up to a minute; only a bigger change moves it. */
function logBegan(){
  const m=/(\d+)h (\d+)m/.exec(S.uptime||''); if(!m) return '';
  const est=Date.now()-(m[1]*3600+m[2]*60+30)*1000;
  if(Math.abs(est-LG.began)>90000) LG.began=est;
  const t=new Date(LG.began), now=new Date(), same=(a,b)=>a.toDateString()===b.toDateString();
  const day=same(t,now)?'today':same(t,new Date(now-864e5))?'yesterday':t.toLocaleDateString([],{month:'short',day:'numeric'});
  return day+' at '+t.toLocaleTimeString([],{hour:'numeric',minute:'2-digit'});
}
/* starts: the start lines, oldest first. The real time is for the screen only: a log gets
   posted, and a time of day gives away a time zone. */
function logLabel(starts,i,screen){
  if(i===starts.length-1){
    const r=logWhy(starts[i]);
    return ['This start',(RESET[r]||r).toLowerCase().replace('usb','USB'),screen?logBegan():logAgo()].filter(Boolean).join(' · ');
  }
  return 'Earlier start'+(RESET[logWhy(starts[i+1])]==='After a crash'?' · ended in a crash':'');
}
function logDiv(l){
  const d=document.createElement('div');
  d.className='lg-div'; LG.divs.push({el:d,line:l});
  return d;
}
function logRow(l){
  const r=document.createElement('div');
  r.className='ln '+l.lv;
  for(const [c,t] of [['t',logHms(l.ms)],['v',LV[l.lv]||l.lv],['g',l.tag],['m',l.text]]){
    const s=document.createElement('span'); s.className=c; s.textContent=t; r.appendChild(s);
  }
  return r;
}
function logLabels(){
  const starts=LG.divs.map(d=>d.line);
  LG.divs.forEach((d,i)=>{ d.el.textContent=logLabel(starts,i,true); });
}
function logWipe(){
  LG.lines=[]; LG.divs=[]; LG.last=0; LG.run=LG.head=null;
  while($('log-drop').nextSibling) $('log-drop').nextSibling.remove();
}
/* Newest first, each start under its own divider. LG.lines stays oldest first, for Copy and
   Download. The list can begin partway through a start, with its first line long gone, so a
   change of run makes the divider rather than the start line. */
function logApply(r){
  const box=$('log-box'), top=box.scrollTop, tall=box.scrollHeight;
  for(const l of r.lines){
    if(l.seq<=LG.last) continue;
    LG.last=l.seq; LG.lines.push(l);
    if(l.run!==LG.run){ LG.run=l.run; LG.head=logDiv(l); $('log-drop').after(LG.head); }
    if(l.lv!=='S'){ l.el=logRow(l); LG.head.after(l.el); }
  }
  if(top>0) box.scrollTop=top+box.scrollHeight-tall;     // someone reading further down stays put
  while(LG.lines.length>LOG_KEEP){
    const l=LG.lines.shift();
    if(l.el) l.el.remove();
    if(LG.lines[0].run!==l.run) LG.divs.shift().el.remove();
  }
  logLabels();
  $('log-empty').textContent='No lines yet.';
  $('log-empty').classList.toggle('hide',LG.lines.length>0);
  const lost=r.head?r.head.dropped:0;
  $('log-drop').textContent=lost+' line'+(lost===1?'':'s')+' not recorded';
  $('log-drop').classList.toggle('hide',!lost);
}
async function logFetch(query,ms){
  try{
    const r=await fetchIn('/api/log'+query,ms||20000);
    return r.ok?logParse(await r.text()):null;
  }catch(e){ return null; }
}
/* Erased, or a power cut took lines only RAM held. Numbering carries on from what flash kept, so
   either the newest number is behind the page's, or a new start's first lines hide behind numbers
   the page has already seen, and the first line it hears of that start isn't the start line. */
function logLost(r){
  if(!LG.last) return false;
  if(r.head&&r.head.newest<LG.last) return true;
  const l=r.lines.find(l=>l.seq>LG.last&&l.run!==LG.run);
  return !!l&&l.lv!=='S';
}
async function logPoll(){
  clearTimeout(LG.timer);
  if(!LG.open) return;
  if(!document.hidden&&!LG.busy){
    LG.busy=true;
    let r=await logFetch(LG.last?'?after='+LG.last:'?n='+LOG_KEEP);
    if(r&&logLost(r)){
      logWipe();
      r=await logFetch('?n='+LOG_KEEP);
    }
    LG.busy=false;
    if(!LG.open) return;
    if(r) logApply(r);
    else if(!LG.lines.length) $('log-empty').textContent='Can’t reach AirWriter.';
  }
  LG.timer=setTimeout(logPoll,S.printing?3000:1000);
}
function logOpen(){
  if(pane!=='about') go('about');
  LG.open=true;
  $('ab-home').classList.add('hide');
  $('ab-log').classList.remove('hide');
  document.querySelector('.back').textContent='‹ About';
  document.querySelector('.detail').scrollTop=0;
  try{ history.replaceState(null,'','/about/log'); }catch(e){}
  logWipe();
  $('log-empty').textContent='Loading…';
  $('log-empty').classList.remove('hide');
  $('log-drop').classList.add('hide');
  say('log-say','');
  logPoll();
}
function logClose(){
  if(!LG.open) return;
  LG.open=false;
  clearTimeout(LG.timer);
  $('ab-log').classList.add('hide');
  $('ab-home').classList.remove('hide');
  document.querySelector('.back').textContent='‹ Settings';
  try{ history.replaceState(null,'','/about'); }catch(e){}
}
function logHeader(){
  // W-1154: each printer by its model, never the name its owner gave it, then where it is and its
  // state, and an AppleTalk printer's own status line, which lwStatus keeps free of job names
  const ps=(PR&&PR.printers||[]).map(p=>{
    const t=LWT&&p.where!=='port'?LWT.find(t=>t.id===p.id):null, own=t&&t.message?lwStatus(t.message):'';
    const w=stWords(p)[1], more=own&&!w.toLowerCase().includes(own.toLowerCase());
    return '  '+prLabel(p)+', '+whereOf(p)+': '+w+(more?'; its status line: '+own:'');
  });
  return ['AirWriter log',
    'Firmware: '+S.version+' (slot '+S.slot+')',
    'Board: '+$('about-rev').textContent.split(' · ')[0],
    'On for: '+S.uptime,
    'Last started: '+(RESET[S.reset]||cap(S.reset||'')),
    'Printers:'+(ps.length?'\n'+ps.join('\n'):' None'),
    'Free memory: '+S.heap,
    'Times count from each start.'].join('\n');
}
function logText(lines){
  const starts=lines.filter(l=>l.lv==='S');
  let n=0;
  return lines.map(l=>l.lv==='S'?'--- '+logLabel(starts,n++)+' ---'
    :logHms(l.ms).padEnd(9)+' '+(LV[l.lv]||l.lv).padEnd(5)+' '+l.tag+'  '+l.text).join('\n');
}
$('log-row').onclick=logOpen;
$('ab-back').onclick=()=>{ logClose(); if(setup){ go('setup'); suStep(2); openNets(); } };
$('su-log').onclick=()=>{ $('d-net').close(); logOpen(); };
$('log-show').onchange=()=>{ $('log-box').dataset.show=$('log-show').value; $('log-box').scrollTop=0; };
$('log-copy').onclick=()=>{
  const f=$('log-show').value;
  const shown=LG.lines.filter(l=>f==='all'||l.lv==='S'||l.lv==='E'||(f==='w'&&l.lv==='W'));
  $('log-buf').value=logHeader()+'\n\n'+logText(shown.slice(-LOG_COPY));
  copyFrom($('log-buf'),$('log-copy'));
};
$('log-dl').onclick=()=>logDownload($('log-dl'),'log-say');
async function logDownload(btn,sayId){
  btn.disabled=true;
  const r=await logFetch('',60000);
  const lw=r&&PR&&PR.printers.some(p=>p.where!=='port')?await getJSON('/api/laser'):null;
  if(lw&&lw.targets) LWT=lw.targets;               // AppleTalk printers' status lines, for the header
  btn.disabled=false;
  if(!r){ say(sayId,'Can’t reach AirWriter.',1,4000); return; }
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([logHeader()+'\n\n'+logText(r.lines)+'\n'],{type:'text/plain'}));
  a.download=logFile();
  a.click();
  setTimeout(()=>URL.revokeObjectURL(a.href),10000);
};

/* One pane at a time; on a phone the sidebar is the first screen and a pane
   slides over it. The pane is in the address, so a reload stays put. */
const PANES=['printers','airprint','appletalk','wifi','about','setup'];
const phone=()=>matchMedia('(max-width:760px)').matches;
let pane='', wantSel=null;
function go(id){
  logClose();
  // printers/2 opens that printer's detail once the list is in; about/log, the log
  const m=/^printers\/(\d+)$/.exec(id||''); if(m){ id='printers'; wantSel=+m[1]; }
  const log=id==='about/log'; if(log) id='about';
  if(!PANES.includes(id)) id='printers';
  pane=id;
  for(const p of $$('.pane')) p.classList.toggle('on',p.id==='p-'+id);
  for(const b of $$('.nav [data-go]'))
    if(b.dataset.go===id) b.setAttribute('aria-current','page'); else b.removeAttribute('aria-current');
  document.body.classList.add('in');
  document.querySelector('.detail').scrollTop=0;
  try{ history.replaceState(null,'','/'+id); }catch(e){}
  document.querySelector('.back').textContent='‹ Settings';
  if(id==='printers'){ prOpen(null); prPoll(); }
  if(id==='printers'&&wantSel!==null&&PR) prOpen(wantSel);
  if(id==='appletalk') atPoll();
  if(id==='about') fwPoll();
  if(log) logOpen();
}
for(const b of $$('[data-go]')) b.onclick=()=>go(b.dataset.go);
document.querySelector('.back').onclick=()=>{
  if(LG.open){ logClose(); return; }
  if(pane==='printers'&&sel!==null){ prOpen(null); return; }   // a phone's one back key
  document.body.classList.remove('in');
  try{ history.replaceState(null,'','/'); }catch(e){}
};

/* dialogs: a key with data-dlg opens one, data-close shuts it, and so does a
   click on the dimmed page around it */
for(const b of $$('[data-dlg]')) b.onclick=()=>$(b.dataset.dlg).showModal();
for(const b of $$('[data-close]')) b.onclick=()=>b.closest('dialog').close();
for(const d of $$('dialog')) d.addEventListener('click',e=>{ if(e.target===d&&d.id!=='d-wait'&&d.id!=='d-signin') d.close(); });
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
          $('wait-s').innerHTML=late||'AirWriter isn’t back yet. Check its lights, then reload this page.';
          $('wait-keys').classList.remove('hide');
        }
        setTimeout(again,3000);
      });
  },6000);
}
$('reload').onclick=()=>location.reload();

/* ── status ────────────────────────────────────────────────────────── */
let S={}, setup=false, first=true, markLc=false, repaintMark=()=>{};
const BARS={Excellent:3,Good:3,Fair:2,Weak:1};
const RESET={'power-on':'Plugged in','software':'After a restart','panic':'After a crash',
  'task watchdog':'After a crash','interrupt watchdog':'After a crash','watchdog':'After a crash',
  'brownout':'After the power dipped','reset pin':'Reset','usb':'Over USB','deep sleep':'Woke up'};

/* W-1179: the words for the sockets, from what the case says on this board. */
const LT_CARD='LocalTalk';   // the AppleTalk card's name on rev 2026.11
function portWords(){
  const two=twoPorts();
  $('lt-name').textContent=two?LT_CARD:'AppleTalk Port';
  document.querySelector('#c-lt [data-way=lt]').setAttribute('aria-label',two?LT_CARD:'AppleTalk Port');
  $('lt-desc').textContent=two?'A Mac plugged into port 1 or 2 with a LocalTalk cable.':'A Mac plugged in with a LocalTalk cable.';
  $('tt-what').textContent=two?LT_CARD:'The AppleTalk Port';
  $('lbl-p2').textContent=two?'port 2':'appletalk';
  $('lbl-p1').textContent=two?'port 1':'serial';
  $('add-title').textContent=two?'Add a printer':'Add a printer on the printer port';
}
function draw(s){
  S=s; setup=!!s.setup; portWords();
  if(!!s.lc!==markLc){ markLc=!!s.lc; repaintMark(); }   // one dot or the panel
  // the port's ImageWriter: its ribbon and gain draw the halftone sample
  const colour=s.ribbon==='colour';
  $('rib-colour').classList.toggle('hide',!colour);
  $('rib').textContent=colour?'Color':s.ribbon==='black'?'Black':'Unknown';
  if(s.ribbon!==ribbon){ ribbon=s.ribbon; render(); }
  const g=+s.gain; if(g>=0&&g<50&&g!==GAIN){ setGain(g); render(); }
  // AirPrint
  $('ap').checked=!!s.airprint;
  $('v-airprint').textContent=s.airprint?'On':'Off';
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
  const hostPending=s.phost&&s.phost+'.local'!==s.host;
  if(hostPending) say('wifi-say','Saved. Restart AirWriter to use the new host name.');
  // About
  $('uptime').textContent=upFor(s.uptime);
  $('reset').textContent=RESET[s.reset]||cap(s.reset||'');
  const logN=+s.log_errors||0;
  $('log-val').textContent=logN?logN+' error'+(logN===1?'':'s'):'';
  $('log-val').classList.toggle('bad',logN>0);
  drawBoard(s);
  $('apname').textContent=s.ap||'AirWriter';
  drawPassword(s.pw||'none');
  // in setup mode only the printer, Wi-Fi and About make sense
  for(const id of ['airprint','appletalk'])
    document.querySelector('.nav [data-go="'+id+'"]').classList.toggle('hide',setup);
  if(first){
    first=false;
    // the address, or what follows the # in a link saved before pages had addresses
    const want=location.pathname.slice(1)||(location.hash||'').slice(1);
    if(setup){
      // W-1088: setup's three steps; back from a join that failed, straight to Wi-Fi
      document.body.classList.add('setupmode'); go('setup');
      if(want==='about/log') logOpen();
      else if(s.ssid){ suStep(2); openNets(); } else { suStep(1); suLook(); }
    }
    else if(PANES.includes(want)||/^printers\/\d+$/.test(want)||want==='about/log') go(want);
    else { go('printers'); if(phone()) document.body.classList.remove('in'); }
  }
  atDraw();
  prDraw();
  finDraw();
}
/* g-wifi's arcs, inner first, one by one so each can light */
const ARCS=['M5.7 10.2a3.25 3.25 0 0 1 4.6 0','M3.58 8.08a6.25 6.25 0 0 1 8.84 0','M1.46 5.96a9.25 9.25 0 0 1 13.08 0'];
const ARC=(cls,d,dim)=>'<path'+(cls?' class='+cls:'')+' fill=none stroke=currentColor stroke-width=1.5 stroke-linecap=round'
  +(dim?' stroke-opacity=.25':'')+' d="'+d+'"/>';
const ARCDOT='<circle cx=8 cy=12.5 r=1.25 fill=currentColor />';
function wifiArcs(n){ return ARCS.map((d,i)=>ARC('',d,i>=n)).reverse().join('')+ARCDOT; }
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

/* ── the panel ─────────────────────────────────────────────────────── */
/* W-1092: the sidebar's mark is the status panel, its dots as they are on the
   box now. GET /api/panel is the frame the board drives the LEDs with; each
   dot is drawn as board 1's panel was photographed showing it, as the wiki's
   GIFs are (tools/lights/render.py): the drive goes back through the gamma to
   a hue, saturation and level, the hue picks between the 24 measured hues and
   the saturation mixes in the measured white. FR4 dots are crisp with a little
   glow, the mask dots a haze. The page asks a few times a second and blends
   from one answer to the next, so a breath stays smooth; with no answer the
   dots go dark, as they would. */
(()=>{
  const cv=$('dots'), P=PANEL, N=P.chain.length*3, CAP={P:48,B:192,L:192};
  const lin=v=>(v/=255)<=.04045?v/12.92:((v+.055)/1.055)**2.4;
  const srgb=x=>Math.round(Math.min(1,Math.max(0,x))**(1/2.2)*255);
  const RING={}, WHITE={};
  for(const m of 'PBL'){ RING[m]=P.ring[m].map(c=>c.map(lin)); WHITE[m]=P.white[m].map(lin); }
  // with the lights off: ENIG gold, the FR4 windows, the two depths of mask
  const BG='#161618', UNLIT={G:[146,122,66],T:[146,122,66],P:[80,77,68],B:[35,38,36],L:[29,31,30]};
  const MAT=P.chain.map(i=>P.dots[i]);
  function look(d,m){                      // a dot's drive -> linear RGB, or null when off
    if(Math.max(...d)<.5) return null;
    const p=d.map(x=>(Math.max(x,0)/CAP[m])**(1/2.2)), v=Math.max(...p);
    const [r,g,b]=p.map(x=>x/v), s=1-Math.min(r,g,b);
    let h=0;
    if(s>0){
      const rc=(1-r)/s, gc=(1-g)/s, bc=(1-b)/s;
      h=(r===1?bc-gc:g===1?2+rc-bc:4+gc-rc)/6; h-=Math.floor(h);
    }
    const x=h*24%24, i=Math.floor(x), f=x-i, a=RING[m][i], c=RING[m][(i+1)%24], w=(1-s)**2.2, k=v**2.2;
    return [0,1,2].map(j=>((a[j]*(1-f)+c[j]*f)*(1-w)+WHITE[m][j]*w)*k);
  }

  let ctx=null, W=0, H=0, sc=1, ox=0, oy=0;
  function size(){
    const dpr=devicePixelRatio||1, gh=2*P.origin+(P.rows-1)*P.pitch;
    W=cv.width=Math.round(cv.clientWidth*dpr); H=cv.height=Math.round(cv.clientHeight*dpr);
    ctx=cv.getContext('2d'); sc=Math.min(W/P.w,H/gh); ox=(W-P.w*sc)/2; oy=(H-gh*sc)/2;
  }
  const at=i=>[ox+(P.origin+i%P.cols*P.pitch)*sc, oy+(P.origin+Math.floor(i/P.cols)*P.pitch)*sc];
  function disc(x,y,r,fill){ ctx.fillStyle=fill; ctx.beginPath(); ctx.arc(x,y,r,0,2*Math.PI); ctx.fill(); }
  function soft(x,y,r,blur,rgb,a){        // a disc blurred by `blur`, added to what is under it
    const R=r+1.5*blur, g=ctx.createRadialGradient(x,y,0,x,y,R), c='rgba('+rgb.join()+',';
    g.addColorStop(0,c+a+')'); g.addColorStop(Math.max(0,r-1.5*blur)/R,c+a+')');
    g.addColorStop(r/R,c+a/2+')'); g.addColorStop(1,c+'0)');
    ctx.fillStyle=g; ctx.fillRect(x-R,y-R,2*R,2*R);
  }
  /* AirWriter LC (W-1109) has one light, under the frosted dot in its clear
     top: the frame's first LED, in its own colours, as the acrylic does not
     shift them. */
  const FROST=[58,58,60];
  function paintOne(f){
    const x=W/2, y=H/2, r=Math.min(W,H)*.16, d=[f[0],f[1],f[2]].map(v=>Math.max(v,0)),
      c=d.map(v=>v/Math.max(48,...d));    // over the cap, the light's hue at full
    ctx.globalCompositeOperation='source-over';
    ctx.fillStyle=BG; ctx.fillRect(0,0,W,H);
    if(Math.max(f[0],f[1],f[2])<.5){ disc(x,y,r,'rgb('+FROST.join()+')'); return; }
    const rgb=c.map(srgb);
    disc(x,y,r,'rgb('+FROST.map((u,j)=>Math.max(u,rgb[j])).join()+')');
    ctx.globalCompositeOperation='lighter';
    soft(x,y,r*1.15,r*.7,rgb,.6);
  }
  function paint(f){                       // f: the drives in chain order
    if(!W||!H) return;
    if(S.lc) return paintOne(f);
    const rad=P.dia*sc/2, lit=[];
    MAT.forEach((m,k)=>{ lit[P.chain[k]]=look([f[3*k],f[3*k+1],f[3*k+2]],m); });
    ctx.globalCompositeOperation='source-over';
    ctx.fillStyle=BG; ctx.fillRect(0,0,W,H);
    for(let i=0;i<P.dots.length;i++){
      const m=P.dots[i], [x,y]=at(i), u=UNLIT[m], c=lit[i];
      disc(x,y,rad,'rgb('+(m==='P'&&c?u.map((v,j)=>Math.max(v,srgb(c[j]))):u).join()+')');
      if(m==='T') disc(x,y,P.hole*sc/2,'#0b0b0c');
    }
    ctx.globalCompositeOperation='lighter';
    for(let i=0;i<P.dots.length;i++){
      const c=lit[i]; if(!c) continue;
      const m=P.dots[i], [x,y]=at(i), rgb=c.map(srgb);
      if(m==='P') soft(x,y,rad,.8*sc,rgb,.35);
      else soft(x,y,rad*(m==='B'?1.25:1.05),.7*sc,rgb,.9);
    }
  }

  // Each answer is blended in over the time since the one before, so the
  // picture runs a fraction of a second behind the box, without steps. Each
  // ask costs board 1 about 12 ms of CPU (6 a second took 7% of a core), and
  // a photo print leaves it a few KB of internal RAM: while a job prints the
  // board is asked once a second.
  const DARK=new Float32Array(N), CALM=matchMedia('(prefers-reduced-motion:reduce)').matches;
  const every=()=>CALM||S.printing?1000:160;
  // Drawn 30 times a second, not at the screen's rate: the dots are a few
  // pixels across, and at 120 Hz the drawing alone took 2% of a Mac's core.
  let from=DARK, to=DARK, t0=0, dur=1, last=0, tick=0;
  function now(){
    const k=Math.min(1,(performance.now()-t0)/dur);
    return k>=1?to:to.map((v,i)=>from[i]+(v-from[i])*k);
  }
  function frame(){ tick=0; paint(now()); if(performance.now()-t0<dur) tick=setTimeout(frame,33); }
  function show(f){
    const t=performance.now();
    from=now(); to=f; dur=Math.min(1000,Math.max(every(),t-last)); last=t0=t;
    if(!tick) frame();
  }

  // Asks only while the mark is on screen: not in a hidden tab, in setup
  // (no sidebar), or behind a pane on a phone. A board with no panel says
  // 404 once and is not asked again.
  let timer=0, busy=false, none=false, misses=0;
  async function ask(){
    timer=0;
    if(none||busy||document.hidden) return;
    if(!cv.offsetParent){ timer=setTimeout(ask,1000); return; }
    busy=true; const t=performance.now();
    try{
      const r=await fetchIn('/api/panel',2000), b=await r.arrayBuffer();
      if(r.status===404||(r.ok&&b.byteLength!==2*N)){ none=true; show(DARK); return; }
      if(!r.ok) throw 0;
      const v=new DataView(b); misses=0;
      show(Float32Array.from({length:N},(_,i)=>v.getUint16(2*i,true)/256));
    }catch(e){ if(++misses===3) show(DARK); }
    finally{ busy=false; }
    if(!none) timer=setTimeout(ask,Math.max(0,every()-(performance.now()-t)));
  }
  document.addEventListener('visibilitychange',()=>{ if(!document.hidden&&!timer) ask(); });
  new ResizeObserver(()=>{ size(); paint(now()); }).observe(cv);
  repaintMark=()=>paint(now());
  ask();
})();

/* ── printers ──────────────────────────────────────────────────────── */
/* W-1088: every printer AirWriter has (/api/printers): notices first, one row
   each, and a printer's detail in the pane when it is opened. A notice is only
   for what asks the person something; a new printer just appears in the list. */
let PR=null, sel=null, LWT=null;
const WHERE={port:'Printer port',cable:'AppleTalk port',network:'Your network',tunnel:'Basilisk II',zone:'Infinite Mac'};
/* W-1179: on rev 2026.11 either socket takes a printer or LocalTalk, and the
   case calls them port 1 (the one every AirWriter has) and port 2. A printer,
   or a LaserWriter on LocalTalk, shows the port it's plugged into. Rev A's
   case still says serial and appletalk, so its page keeps its old words. */
const twoPorts=()=>!!S.board&&S.board!=='rev-a';
const whereOf=p=>(p.where==='port'||p.where==='cable')&&twoPorts()&&p.port?'Port '+p.port:WHERE[p.where]||p.where;
const ST={ready:['g','Ready'],printing:['g','Printing'],cleaning:['g','Cleaning'],attention:['a','Needs attention'],failed:['r','The last job failed'],offline:['','Offline']};
/* W-1103: the port's models, the board's own names for them (sw_model.c). The
   original StyleWriter has no cleaning, and only the Color ones take a color cartridge. */
const MODELS=['ImageWriter II','ImageWriter LQ','ImageWriter','StyleWriter','StyleWriter II','StyleWriter 1200',
  'Color StyleWriter 1500','Color StyleWriter 2200','Color StyleWriter 2400','Color StyleWriter 2500'];
for(const m of MODELS){ $('model').append(new Option(m,m)); $('add-model').append(new Option(m,m)); }
/* A name in a notice title or key keeps its words together: no line ends
   after "StyleWriter" and leaves "II" on a line of its own. A name is 32
   bytes at most; anything longer is not one and may wrap. */
const nb=s=>s.length>32?s:s.replace(/ /g,'\u00a0');
$('model').append(new Option('Something else','none'));
{ const o=new Option('Choose…',''); o.id='model-ask'; o.disabled=true; $('model').prepend(o); }
/* W-1116: the printer port keeps one record per model, so a model already in
   Printers can't be added again: both pickers mark it (the Something else
   record is one per port too). A record's own model stays as it is. */
function markAdded(sel,own){
  const have=new Set(PR.printers.filter(p=>p.where==='port'&&p.model).map(p=>p.model));
  if(PR.said_other) have.add('none');
  for(const o of sel.options){
    if(!o.value) continue;                          // Choose…
    const on=o.value!==own&&have.has(o.value), name=o.value==='none'?'Something else':o.value;
    const label=on?name+' · Added':name;
    if(o.disabled!==on||o.textContent!==label){ o.disabled=on; o.textContent=label; }
  }
}
/* The five screens, and a sixth on a StyleWriter or a Personal LaserWriter 300: Apple's own
   for that printer (W-1103, W-1102). */
function screenList(apple){
  const s=$('screen'); if(s._apple===apple) return;
  const v=s.value; s._apple=apple; s.textContent='';
  for(const k of ORDER) s.append(new Option(MODES[k].title,k));
  if(apple) s.append(new Option('Apple','apple'));
  if(v) s.value=v;
}
/* W-1102: the port printers with their own Apple screen and fault words, and no SELECT key. */
const ownFaults=p=>p.kind==='stylewriter'||p.kind==='qdlaser';
screenList(false);
const prById=id=>PR&&PR.printers.find(p=>p.id===id);
/* A printer's own icon, Apple's, the one AirPrint shows; a printer of no known kind has none. */
function icHTML(p){ const u=ICONS[p.icon]; return u?'<img class="ic pr" src="'+u+'" alt="">':'<span class="ic unk"><svg><use href="#g-printer"/></svg></span>'; }
/* What the box is: the most urgent of its shared printers, else how many. */
function sideWord(){
  if(!PR) return '';
  const ps=PR.printers, sh=ps.filter(p=>p.shared);
  for(const [st,w] of [['failed','Failed'],['attention','Needs attention'],['printing','Printing'],['cleaning','Cleaning']])
    if(sh.some(p=>p.state===st)) return w;
  if(!ps.length) return 'None';
  if(ps.length===1) return ps[0].kind==='unknown'?'Unrecognized':ST[ps[0].state]?ST[ps[0].state][1]:'';
  return plural(ps.length,'printer','printers');
}
/* A StyleWriter's fault, in the words the phone shows for it (sw_status.c), as
   the board names it in /api/printers: "check paper" for a missing cartridge
   would send its owner to the wrong part of the printer. A Personal LaserWriter
   300's are four of them: paper, jam, cover and other (W-1102). */
const SW_FAULT={paper:'check paper',jam:'clear the paper jam',cartridge:'check the ink cartridge',
  'cartridge-jam':'check the ink cartridge',ink:'replace the ink cartridge',cover:'close the cover',
  waste:'the waste ink pad is full',other:'check the printer'};
/* A printer's state in words: the port's as its line says, an AppleTalk
   printer's in its own words when it has any. */
function stWords(p){
  if(p.kind==='unknown') return ['','Unrecognized'];
  const t=LWT&&p.where!=='port'?LWT.find(t=>t.id===p.id):null;
  const low=w=>w?w[0].toLowerCase()+w.slice(1):w;
  // W-1103: a StyleWriter has no SELECT key, nor has a 300 (W-1102)
  const own=ownFaults(p);
  if(p.state==='attention') return ['a','Needs attention: '+(p.where==='port'?(own?SW_FAULT[p.fault]||SW_FAULT.other:'check paper and SELECT'):low(t&&lwStatus(t.message))||'check the printer')];
  // Offline is a status name alone, as on the phone, for every printer (Wells, 2026-10-31):
  // what to check is the wiki's Offline troubleshooting row's
  if(p.state==='offline') return ['','Offline'];
  if(p.state==='printing'&&t&&/busy/i.test(t.message||'')&&!S.printing) return ['g','Busy with another job'];
  return ST[p.state]||['',''];
}
/* A notice: what is wrong, in bold, then what to do. */
const NOTE={
  unknown_port:(p)=>['Unrecognized printer on '+(twoPorts()&&p&&p.port?'port '+p.port:'the printer port'),'If it’s an ImageWriter or StyleWriter, choose its model.',[['Choose Model…','open'],['Dismiss','dismiss']]],
  no_answer:(p)=>[q(p)+' isn’t responding','Make sure it’s connected and switched on.',[['Share Anyway','use_anyway'],['Dismiss','dismiss']]],
  too_many:(p)=>[q(p)+' isn’t shared','You can share up to '+(PR.max_shared||6)+' printers. To share this one, stop sharing another.',[['OK','dismiss']]]};
const q=p=>nb(esc(p?p.name:'A printer'));
function prDraw(){
  $('v-printers').textContent=sideWord();
  if(!PR) return;
  // a notice waiting: the caution mark beside the word, seen from any pane
  const notes=PR.notes;
  if(notes.some(n=>NOTE[n.kind])) $('v-printers').insertAdjacentHTML('afterbegin','<svg class=warn><use href="#g-caution"/></svg>');
  // the notices, each with its keys
  let h='';
  notes.forEach((n,i)=>{
    const p=prById(n.id), f=NOTE[n.kind]; if(!f) return;
    const [title,text,keys]=f(p,n);
    h+='<div class="box note"><svg class=warn><use href="#g-caution"/></svg><p><b>'+title+'</b>'+text+'</p>'
      +(keys.length?'<div class=btnrow>'+keys.map(([l,a])=>
      '<button class=btn type=button data-note='+i+' data-act='+a+' data-id='+n.id+'>'+l+'</button>').join('')+'</div>':'')+'</div>';
  });
  if($('pr-notes')._h!==h){ $('pr-notes')._h=h; $('pr-notes').innerHTML=h; }
  // the rows; W-1102: a QuickDraw LaserWriter on the port that AirWriter can't print to is
  // grayed at the end, with nothing to open, until it's unplugged (Wells, 2026-11-13)
  const off=PR.port_unsupported?'<div class="row prow off"><span class="ic unk"><svg><use href="#g-printer"/></svg></span><span class=txt>'
    +esc(PR.port_unsupported)+'<small>'+whereOf({where:'port',port:PR.port_unsupported_port})+' · Not supported yet</small></span><i class=lamp></i><span class=chev aria-hidden=true>›</span></div>':'';
  h=PR.printers.length||off?PR.printers.map(p=>{ const [cls]=stWords(p);
    return '<button class="row link prow" type=button data-pr='+p.id+'>'+icHTML(p)+'<span class=txt>'+esc(p.name)
      +'<small>'+esc(whereOf(p))+(p.shared?'':' · Not shared')+'</small></span><i class="lamp '+cls+'"></i><span class=chev aria-hidden=true>›</span></button>'; }).join('')+off
    :'<p class=empty>No printers yet. Connect your printer and switch it on, then Look for Printers.</p>';
  if($('pr-list')._h!==h){ $('pr-list')._h=h; $('pr-list').innerHTML=h; }
  // AirPrint: each shared printer's address
  const sh=PR.printers.filter(p=>p.rp), host=(S.host||'airwriter.local');
  h=sh.map(p=>'<div class=row><span class=txt>'+esc(p.name)+'<small class=mono data-uri>ipp://'+esc(host)+':631/'+esc(p.rp)
    +'</small></span><button class=btn type=button data-copyuri>Copy</button></div>').join('');
  $('ap-list').classList.toggle('hide',!sh.length);
  if($('ap-list')._h!==h){ $('ap-list')._h=h; $('ap-list').innerHTML=h; }
  // AppleTalk: what vintage Macs see, in the names their Chooser shows (W-1116: the board's list)
  const old=PR.chooser||[];
  $('oldmacs').textContent=old.length?'In the Chooser on vintage Macs: '+old.join(', '):'No printers in the Chooser yet.';
  markAdded($('add-model'),'');
  if(sel!==null) pdDraw();
  suDraw();
}
/* A printer's detail, in the pane in place of the list. */
function prOpen(id){
  sel=id;
  if(pane==='printers') try{ history.replaceState(null,'','/printers'+(id!==null?'/'+id:'')); }catch(e){}
  $('pr-home').classList.toggle('hide',id!==null);
  $('pr-detail').classList.toggle('hide',id===null);
  document.querySelector('.back').textContent=id!==null?'‹ Printers':'‹ Settings';
  say('prn-say',''); say('pd-clean-say','');
  if(id!==null){ document.querySelector('.detail').scrollTop=0; pdDraw(); requestAnimationFrame(render); lwtPoll(); }
}
/* W-1154: a printer the wiki's Supported Printers page marks Yes*, one AirWriter prints to that
   nobody has confirmed on a real one yet, gets one line under its status. Every other printer
   AirWriter prints to is a Yes. Confirmations come from W-1161's statistics, not from here. */
const CONFIRMED=['ImageWriter II','Color StyleWriter 2500','Personal LaserWriter 300','LaserWriter Personal NTR'];
const prModel=p=>p.where==='port'?p.model:p.product;
/* What to call a printer anywhere it's posted: its model, else what kind it is */
const prLabel=p=>prModel(p)||{unknown:'Unrecognized printer',laserwriter:'LaserWriter','appletalk-imagewriter':'AppleTalk ImageWriter'}[p.kind]||'Printer';
function unconfirmed(p){
  const ours=p.where==='port'?['imagewriter','stylewriter','qdlaser'].includes(p.kind):p.kind==='laserwriter';
  return ours&&!!prModel(p)&&!CONFIRMED.includes(prModel(p));
}
/* A problem, reported as a new GitHub issue filled in from what the page knows: the printer's
   model (never the name its owner gave it), its connection, the firmware and the board. GitHub
   takes no file by address, so the person downloads the log first and drags it in. */
const boardName=()=>!S.board||S.board==='rev-a'?'Rev A':'Rev '+S.board+(S.lc?' LC':'');
const logFile=()=>'airwriter-'+((S.ap||'').split('-')[1]||'board').toLowerCase()+'-log.txt';
function problemURL(p){
  const m=p?prLabel(p):'';
  const lines=['What happened:','','','What you expected:','','',
    ...(p?['Printed from (phone, computer or vintage Mac):','','','Printer: '+m,'Connection: '+whereOf(p)]:[]),
    'Firmware: '+S.version,'Board: '+boardName(),'','Log: drag '+logFile()+' here.'];
  return 'https://github.com/wr/airwriter/issues/new?title='+encodeURIComponent(m?m+': ':'')+'&body='+encodeURIComponent(lines.join('\n'));
}
$('rep-log').onclick=()=>logDownload($('rep-log'),'rep-say');
$('rep-go').onclick=()=>setTimeout(()=>$('d-report').close(),0);
$('log-report').onclick=()=>{ $('log-report').href=problemURL(null); };
let spSaving=false;                 // W-1099: the start page on its way to the printer
function pdDraw(){
  const p=prById(sel);
  if(!p){ prOpen(null); return; }
  const port=p.where==='port', iw=p.kind==='imagewriter', sw=p.kind==='stylewriter', unk=p.kind==='unknown';
  const qd=p.kind==='qdlaser';                      // W-1102: a Personal LaserWriter 300
  { const h=icHTML(p); if($('pd-ic')._h!==h){ $('pd-ic')._h=h; $('pd-ic').innerHTML=h; } }
  $('pd-name').textContent=p.name;
  $('pd-where').textContent=(p.product&&p.product!==p.name?p.product+' · ':'')+whereOf(p);
  const [cls,text]=stWords(p);
  lamp($('prn-lamp'),cls); $('prn-state').textContent=text;
  // W-1102: why the port's last job failed, when the board has a reason (a page that didn't fit in the printer's memory)
  $('prn-why').textContent=port&&p.state==='failed'&&S.job_error||'';
  $('pd-unconf').classList.toggle('hide',!unconfirmed(p));
  // W-1154: Report a Problem…'s sheet names the printer by its model, and its issue is this printer's
  $('rep-model').textContent=prModel(p)?'the '+nb(prModel(p)):'this printer'; $('rep-go').href=problemURL(p);
  $('rib-row').classList.toggle('hide',!(port&&iw));
  // W-1103: a StyleWriter's cartridge, read from the printer; the black-only models always hold
  // black, but not while the printer says the cartridge is missing or jammed
  const cartFault=p.fault==='cartridge'||p.fault==='cartridge-jam';
  const cart=sw&&!cartFault?p.cartridge||(/^Color /.test(p.model)?'':'black'):'';
  $('cart-row').classList.toggle('hide',!cart);
  $('cart-colour').classList.toggle('hide',cart!=='colour');
  $('cart').textContent=cart==='colour'?'Color':'Black';
  if(document.activeElement!==$('pname')){ $('pname').value=p.name; }
  $('model-row').classList.toggle('hide',!port||qd);   // a 300 names itself; there's nothing to choose
  // found, never said what it is: ask; told it is something else: say so
  const ask=unk&&p.found==='auto';
  $('model-ask').hidden=!ask;
  markAdded($('model'),unk?'none':p.model);
  if(document.activeElement!==$('model')) $('model').value=ask?'':unk?'none':p.model;
  const atiw=p.kind==='appletalk-imagewriter';     // W-1088: old Macs' only, for now
  $('share-row').classList.toggle('hide',unk||atiw);
  $('pshare').checked=!!p.shared; $('pshare').disabled=!p.identified;
  // W-1099: a LaserWriter's own start page, where it said whether it prints one
  $('sp-row').classList.toggle('hide',p.kind!=='laserwriter'||typeof p.start_page!=='boolean');
  if(!spSaving){ $('pstart').checked=!!p.start_page; $('pstart').disabled=p.state==='offline'; }
  $('pd-cant').classList.toggle('hide',!unk&&!atiw);
  $('pd-cant').textContent=ask?'Unrecognized printer. If it’s an ImageWriter or StyleWriter, choose its model.'
    :atiw?'Vintage Macs on every AppleTalk connection can use it. Phones and computers can’t.'
    :'AirWriter prints on ImageWriters, StyleWriters and LaserWriters.';
  $('ht-box').classList.toggle('hide',unk||atiw);
  $('ht-paper').classList.toggle('hide',!iw);       // the sample is the ImageWriter's dots
  screenList(sw||qd);                               // W-1103: Apple's screen is a StyleWriter's or a 300's
  if(document.activeElement!==$('screen')&&p.dither&&p.dither!==$('screen').value){
    $('screen').value=p.dither;
    if(iw&&p.dither!==dither){ dither=p.dither; render(); }
  }
  $('pd-cable').classList.toggle('hide',!port||S.pic===false);
  // W-1103: one cleaning of the print head, a card of its own; the original StyleWriter has none
  const clean=sw&&p.model!=='StyleWriter';
  $('clean-box').classList.toggle('hide',!clean);
  $('pd-cleannote').classList.toggle('hide',!clean);
  $('testpage').classList.toggle('hide',unk||p.kind==='appletalk-imagewriter');
}
async function prPoll(){
  const r=await getJSON('/api/printers');
  if(lost||!r) return;
  PR=r; prDraw();
  if(wantSel!==null&&pane==='printers'){ const w=wantSel; wantSel=null; prOpen(w); }
  finDraw();
}
/* An AppleTalk printer's own words (its status line), from laser.c's view. */
async function lwtPoll(){
  const p=prById(sel); if(!p||p.where==='port') return;
  const r=await getJSON('/api/laser'); if(r&&r.targets){ LWT=r.targets; pdDraw(); }
}
prPoll(); setInterval(()=>{ if(!document.hidden){ prPoll(); if(sel!==null) lwtPoll(); } },4000);
async function prPost(body,okText,sayId,ms){
  const r=await postJSON('/api/printers',body);
  if(r.error){ say(sayId||'prn-say',r.error,1); return false; }
  if(r.printers){ PR=r; prDraw(); }
  if(okText) say(sayId||'prn-say',okText,0,ms||2400);
  setTimeout(poll,300);
  return true;
}
$('pr-list').addEventListener('click',e=>{ const b=e.target.closest('[data-pr]'); if(b) prOpen(+b.dataset.pr); });
$('pr-back').onclick=()=>prOpen(null);
$('pr-notes').addEventListener('click',e=>{
  const b=e.target.closest('[data-act]'); if(!b) return;
  const a=b.dataset.act, n=+b.dataset.note, id=+b.dataset.id;
  if(a==='open'){ prOpen(id); return; }
  if(a==='use_anyway') prPost({id:id,use_anyway:true},'Shared.','prs-say');
  else prPost({note:n,dismiss:true},null,'prs-say');
});
$('pr-look').onclick=async()=>{
  const b=$('pr-look'); b.disabled=true;
  say('prs-say','Looking for printers…');
  await prPost({look:true});
  setTimeout(()=>{ b.disabled=false; say('prs-say',''); prPoll(); },6000);
};
/* W-1116: the Add dialog (opened from Printers and from setup) opens on a model
   that can still be added; with every model added, Add is off. */
for(const b of $$('[data-dlg=d-add]')) b.onclick=()=>{
  const s=$('add-model'), o=s.selectedOptions[0];
  if(!o||o.disabled){ const f=[...s.options].find(o=>!o.disabled); if(f) s.value=f.value; }
  $('add-go').disabled=![...s.options].some(o=>!o.disabled);
  say('add-say',''); $('d-add').showModal();
};
$('add-go').onclick=async()=>{
  if(await prPost({add_port:$('add-model').value},null,'add-say')) $('d-add').close();
};
$('pname').onchange=()=>{ const v=$('pname').value.trim(); prPost({id:sel,name:v},'Saved.'); };
$('pname').onkeydown=e=>{ if(e.key==='Enter') $('pname').blur(); };
byteCap($('pname'),32);
$('model').onchange=()=>prPost({id:sel,model:$('model').value},'Saved.');
$('pshare').onchange=async()=>{
  const on=$('pshare').checked;
  if(!await prPost({id:sel,shared:on},on?'Shared. It shows up on phones and computers in a moment.':'Not shared.')) $('pshare').checked=!on;
};
/* The start page is stored in the printer: the board sends it a job and
   answers once the printer says, a few seconds. */
$('pstart').onchange=async()=>{
  const b=$('pstart'), on=b.checked;
  spSaving=true; b.disabled=true; say('prn-say','Saving…');
  const ok=await prPost({id:sel,start_page:on},'Saved.');
  spSaving=false;
  if(!ok) b.checked=!on;
  pdDraw();
};
$('screen').onchange=()=>{
  const p=prById(sel); if(p&&p.kind==='imagewriter'){ dither=$('screen').value; render(); }
  prPost({id:sel,dither:$('screen').value},'Saved.');
};
$('pd-remove').onclick=()=>{ const p=prById(sel); if(!p) return; $('rm-name').textContent=p.name; $('d-remove').showModal(); };
$('rm-go').onclick=async()=>{ const id=sel; $('d-remove').close(); if(await prPost({id:id,remove:true},null,'prs-say')) prOpen(null); };
$('testpage').onclick=async()=>{
  const p=prById(sel); if(!p) return;
  // W-1116: a port printer while another one is connected: the board answers 409 with which
  // to connect (an action's refusal); any other offline printer gets the page's one line
  // W-1179: on rev 2026.11, the printer on this record's own port (port 2's is port2_on)
  const on=p.port===2?PR.port2_on:PR.port_on;
  const other=p.where==='port'&&p.kind!=='unknown'&&!PR.said_other&&on&&on!==p.id;
  if(p.state==='offline'&&!other){
    say('prn-say',p.where==='port'?(ownFaults(p)?'The printer is offline. Check power and cable.':'The printer is offline. Check power, cable and SELECT.'):'The printer is offline. Check that it’s switched on.',1,4000);
    return;
  }
  const r=await postJSON('/api/test',{id:p.id});
  say('prn-say',r.error?r.error.replace(/\.?$/,'.'):'Printing a test page.',!!r.error,4000);
};
/* W-1103: one press is one cleaning. The board says no in a sentence when the
   printer is busy, off or holding a fault. */
$('pd-clean').onclick=async()=>{
  const b=$('pd-clean'); if(b.disabled) return;
  b.disabled=true;
  const ok=await prPost({id:sel,clean:true},'Cleaning the print head.','pd-clean-say',4000);
  if(!ok) say('pd-clean-say',$('pd-clean-say').textContent,1,6000);   // a refusal goes, as the test page's does
  b.disabled=false;
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
  say('cable-say','Flashing TashTalk…');
  const r=await postJSON('/api/localtalk/flash');
  b.disabled=false;
  if(r.error){ say('cable-say',r.error,1); return; }
  if(r.ok){ say('cable-say','TashTalk is installed. Test again.'); b.classList.add('hide'); }
  else say('cable-say',flashErr(r),1);
};

/* ── AirPrint ──────────────────────────────────────────────────────── */
$('ap').onchange=async()=>{
  const on=$('ap').checked, r=await postJSON('/api/general',{airprint:on});
  if(r.error){ $('ap').checked=!on; say('ap-say',r.error,1); }
  else { $('v-airprint').textContent=on?'On':'Off'; setTimeout(poll,300); }
};
$('ap-list').addEventListener('click',e=>{
  const b=e.target.closest('[data-copyuri]'); if(b) copyFrom(b.parentNode.querySelector('[data-uri]'),b);
});

/* ── AppleTalk ─────────────────────────────────────────────────────── */
/* Three cards. The cable is the LocalTalk node's. EtherTalk is two links
   under one switch (W-1189): the board's own Wi-Fi for real Macs, Basilisk
   II's UDP tunnel for emulated ones. Infinite Mac is the third EtherTalk link,
   on a switch of its own because it reaches out to the internet. A Mac is
   "connected" when its link heard it in the last 10 minutes: AppleTalk has
   no connection to watch. */
let LT=null, ET=null, ETS=null;
const MAC='<svg><use href="#g-mac"/></svg>', EMAC='<svg><use href="#g-emac"/></svg>';
const esc=t=>String(t).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c]);
/* A link's Macs for the list: its icon, a real Mac or an emulated one on a
   screen, and its address there. */
const macsOf=(macs,ic,addr)=>(macs||[]).map(m=>({name:m.name,addr:addr(m),ago:m.ago,ic}));
/* A Mac by its name from Sharing Setup when it gave one (System 7 on), its
   address beside the time; else by its address. */
function macList(list){
  const n=list.length;
  let h='<span class=count>'+(n?plural(n,'Mac','Macs')+' connected':'No Macs connected')+'</span>';
  if(n) h+='<ul class=macs>'+list.map(m=>'<li>'+m.ic+'<span>'+esc(m.name||m.addr)+'</span><small>'
    +(m.name?esc(m.addr)+' · ':'')+ago(m.ago)+'</small></li>').join('')+'</ul>';
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
  silent:['r','The LocalTalk chip isn’t responding.'+CHIP_HELP]};
let flashing=false;
/* A flash that failed: the board's own words when it refused to start, else
   the chip's failure (pic_err_str) in the reader's words. The wiki's LocalTalk
   Chip page tells these apart by their wording. */
const FLASH_ERR={'no PIC answered':'Can’t reach the LocalTalk chip.','not a PIC12F1840':'The chip isn’t a PIC12F1840.',
  'verify failed':'TashTalk didn’t install correctly. Try again.'};
function flashErr(r){
  const e=String(r.err||'');
  if(/^[A-Z]/.test(e)) return e+'.';
  for(const k in FLASH_ERR) if(e.startsWith(k)) return FLASH_ERR[k];
  return 'Couldn’t flash TashTalk.';
}
/* W-1186: one port's row on the LocalTalk card when both ports do LocalTalk: the port, then
   its chip's trouble, joining, or its Macs. */
function ltRow(r){
  const chip=r.phase==='off'&&CHIP[r.chip];
  if(chip) return stLine(chip[0],'Port '+r.port)+'<span class=count>'
    +(flashing&&r.port===flashPort?'Flashing TashTalk…':chip[1])+'</span>';
  if(r.phase==='acquiring') return stLine('a','Port '+r.port)+'<span class=count>Joining the AppleTalk network…</span>';
  if(r.phase==='off') return stLine('a','Port '+r.port)+'<span class=count>Off</span>';
  return stLine('g','Port '+r.port)+macList(macsOf(r.macs,MAC,m=>'Node '+m.node));
}
/* The port whose chip the card's Flash TashTalk key is for: 0 for the board's choice. */
let flashPort=0;
function atDraw(){
  let macs=0, on=0;
  // the cable
  {
    const c=$('c-lt'), sw=c.querySelector('[data-way=lt]');
    // W-1186: both ports on LocalTalk, two networks: a row for each. The top level is the
    // lowest port's node, so the switch reads on if either port's does, and a port whose chip
    // needs seeing to shows it whether the switch is on or off, as one port's card does.
    const rows=twoPorts()&&LT&&(LT.ports||[]).length>1?LT.ports:null;
    const live=!!(LT&&(LT.on||rows&&rows.some(r=>r.on)));
    const chip=!rows&&!live&&CHIP[S.lt_chip];
    const trouble=rows?rows.filter(r=>r.phase==='off'&&CHIP[r.chip]):[];
    const need=trouble.find(r=>r.chip==='no_tashtalk');
    flashPort=need?need.port:0;
    c.classList.toggle('hide',S.pic===false||!!S.lc);
    $('tt-credit').classList.toggle('hide',S.pic===false||!!S.lc);
    sw.checked=live; sw.disabled=!S.pic;
    sw.closest('.sw').classList.toggle('hide',!!chip);
    c.classList.toggle('off',!live&&!chip&&!trouble.length);
    $('lt-flash').classList.toggle('hide',!flashing&&!(chip&&S.lt_chip==='no_tashtalk')&&!need);
    if(chip) put(c,flashing?stLine('a','Flashing TashTalk…'):stHTML(chip[0],chip[1]));
    else if(rows&&(live||trouble.length)){
      if(live) on++;
      put(c,(live?rows:trouble).map(r=>{ macs+=(r.macs||[]).length; return ltRow(r); }).join('<hr class=sep>'));
    }
    else if(live&&twoPorts()&&!LT.port){
      // W-1179: no port has LocalTalk on it yet; a Mac plugged into either is heard when it talks
      on++; put(c,macList([]));
    }
    else if(live){
      on++; macs+=(LT.macs||[]).length;
      // W-1179: 'listening', nothing heard on the port yet, is not joining
      put(c,(LT.phase==='acquiring'?stLine('a','Joining the AppleTalk network…'):twoPorts()?stLine('g','On port '+LT.port):'')
        +macList(macsOf(LT.macs,MAC,m=>'Node '+m.node)));
    }
  }
  // EtherTalk: the switch is on while either link is (a board set before
  // W-1189 may have one off), and the list is both links' Macs, newest first
  {
    const c=$('c-et'), sw=c.querySelector('[data-way=et]');
    const set=!!ETS&&!!(ETS.radio||ETS.tunnel), up=set&&ET&&ET.on;
    const r=up&&ETS.radio?ET.radio:null, t=up&&ETS.tunnel?ET.tunnel:null;
    sw.checked=set; sw.disabled=!ETS;
    c.classList.toggle('off',!set);
    if(r||t){
      on++;
      const st=[r,t].filter(Boolean).map(s=>s.state);
      const list=[...macsOf(r&&r.macs,MAC,m=>m.addr),...macsOf(t&&t.macs,EMAC,m=>m.ip||m.addr)]
        .sort((a,b)=>a.ago-b.ago);
      macs+=list.length;
      put(c,(t&&t.state==='no socket'?stLine('r','Couldn’t open UDP port 6066 for Basilisk II'):'')
        +(st.includes('no address')?stLine('a','Waiting for a network address')
          :st.includes('acquiring')?stLine('a','Joining the AppleTalk network…'):'')
        +macList(list));
    }
  }
  // Infinite Mac
  {
    const c=$('c-zone'), sw=c.querySelector('[data-way=zone]');
    const set=!!ETS&&!!ETS.infinite, side=set&&ET&&ET.on?ET.zone:null;
    sw.checked=set; sw.disabled=!ETS;
    c.classList.toggle('off',!set);
    if(side){
      on++;
      const list=macsOf(side.macs,EMAC,m=>m.addr); macs+=list.length;
      put(c,(side.state==='online'?stHTML('g','Connected to '+zoneChip())
        :side.state==='acquiring'?stHTML('a','Joining '+zoneChip()+'…')
        :ET.relay&&ET.relay.last_error?stLine('r','Can’t reach infinitemac.org. Trying again.',ET.relay.last_error)
        :stLine('a','Connecting to infinitemac.org…'))+macList(list));
    }
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
    // EtherTalk's one switch sets both of its links
    const body=w==='et'?{radio:on,tunnel:on}:{infinite:on};
    // Infinite Mac's zone is a made-up name until someone types one: anyone
    // who knows it can print, so it should not be guessable
    if(w==='zone'&&on&&ETS&&!ETS.zone) body.zone=zoneName();
    r=await postJSON('/api/et/settings',body);
    if(!r.error) ETS=r;
  }
  if(r.error){ sw.checked=!on; say('at-say',r.error,1); }
  else { say('at-say',''); atDraw(); setTimeout(atPoll,1500); setTimeout(poll,600); }
};
/* A LaserWriter's own status words, plainly: its error, Busy, or what it says. */
function lwStatus(t){
  t=String(t||'');
  const e=t.match(/PrinterError:\s*([^;\]]*)/i); if(e) return cap(e[1].trim());
  if(/busy|printing|job:/i.test(t)) return 'Busy';
  const s=t.match(/status:\s*([^;\]]*)/i);
  return cap((s?s[1]:t).trim());
}
/* The card's flash key, for a chip that needs TashTalk: once it is on, the
   board starts the port by itself. */
$('lt-flash').onclick=async()=>{
  const b=$('lt-flash'); if(b.disabled) return; b.disabled=true;
  flashing=true; say('at-say',''); atDraw();
  const fp=flashPort;
  const r=await postJSON('/api/localtalk/flash',fp?{port:fp}:undefined);
  flashing=false; b.disabled=false;
  if(r.error||!r.ok) say('at-say',r.error||flashErr(r),1);
  else if(fp&&LT&&LT.ports){
    // W-1186: that port's row starts; the other keeps its Macs until the next answer
    LT.ports=LT.ports.map(x=>x.port===fp?Object.assign({},x,{chip:'ok',
      phase:S.lt_port===1?'acquiring':'off',on:S.lt_port===1}):x);
    setTimeout(atPoll,1500); setTimeout(poll,600);
  }
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
  if(r.error){ say('zone-say','Use up to 32 letters, digits, dashes, underscores or dots.',1); return; }
  ETS=r; atDraw(); setTimeout(atPoll,1500); $('d-inf').close();
};

/* ── setup ───────────────────────────────────────────────────────── */
/* W-1088: first-run setup, on AirWriter's own network: what is plugged in
   (the board looks before it has Wi-Fi), then Wi-Fi, then where to go. */
let suLooking=0;
function suStep(n){
  for(const i of [1,2,3]) $('su-'+i).classList.toggle('hide',i!==n);
  document.querySelector('.detail').scrollTop=0;
}
function suLook(){
  suLooking=Date.now(); suDraw();
  postJSON('/api/printers',{look:true}).then(()=>prPoll());
  setTimeout(prPoll,3500);
  setTimeout(()=>{ suLooking=0; prPoll(); },7000);
}
function suDraw(){
  if(!setup) return;
  const ps=PR?PR.printers:[], looking=!!suLooking;
  $('su-looking').textContent=looking?'Looking for printers…'
    :ps.length?(ps.length===1?'Here’s your printer. Rename it if you like.':'Here are your printers. Rename them if you like.'):'';
  const h=ps.length?ps.map(p=>'<div class="row prow">'+icHTML(p)+'<span class=txt><input class=fld data-rename='+p.id
      +' value="'+esc(p.name)+'" maxlength=32 autocomplete=off spellcheck=false aria-label="Name"><small>'
      +esc(whereOf(p))+'</small></span></div>').join('')
    :'<p class=empty>'+(looking?'Looking…':'No printers yet.')+'</p>';
  // a name being typed is kept
  if(!$('su-list').contains(document.activeElement)&&$('su-list')._h!==h){ $('su-list')._h=h; $('su-list').innerHTML=h; }
  $('su-next-t').textContent=ps.length?'Continue':'Continue Without a Printer';
}
$('su-list').addEventListener('change',e=>{
  const f=e.target.closest('[data-rename]'); if(!f) return;
  prPost({id:+f.dataset.rename,name:f.value.trim()},'Saved.','su-say');
});
$('su-list').addEventListener('keydown',e=>{ if(e.key==='Enter'&&e.target.matches('[data-rename]')) e.target.blur(); });
$('su-look').onclick=suLook;
$('su-next').onclick=()=>{ suStep(2); openNets(); };
$('su-back').onclick=()=>suStep(1);
$('su-nets').onclick=openNets;

/* W-1088: the first visit after a fresh setup opens on this sheet: each
   printer with its state and a test page, and how to print from a phone
   and a Mac. Done closes it for good. */
let finShown=false;
function finDraw(){
  if(finShown||setup||!S.fresh||!PR) return;
  finShown=true;
  const ps=PR.printers, sh=ps.filter(p=>p.shared);
  const name=sh.length===1?'<b>'+esc(sh[0].name)+'</b>':'your printer';
  $('fin-list').innerHTML=ps.length?ps.map(p=>{ const [cls,t]=stWords(p);
      const can=p.where==='port'?p.kind==='imagewriter'||ownFaults(p):p.kind==='laserwriter'&&p.identified;
      return '<div class="row prow">'+icHTML(p)+'<span class=txt>'+esc(p.name)+'<small>'+esc(t)
        +'</small></span><i class="lamp '+cls+'"></i>'+(can?'<button class=btn type=button data-fintest='+p.id+'>Print Test Page</button>':'')+'</div>'; }).join('')
    :'<p class=empty>No printers yet. Add one in Printers.</p>';
  $('fin-how').innerHTML='On an iPhone or iPad: tap Share, then Print, and choose '+name+'.<br>'
    +'On a Mac: choose File &gt; Print, then '+name+' from the Printer menu. If it isn’t listed, add it in System Settings &gt; Printers &amp; Scanners.';
  $('d-finish').showModal();
}
$('fin-list').addEventListener('click',async e=>{
  const b=e.target.closest('[data-fintest]'); if(!b) return;
  const r=await postJSON('/api/test',{id:+b.dataset.fintest});
  say('fin-say',r.error?r.error.replace(/\.?$/,'.'):'Printing a test page.',!!r.error,4000);
});
$('fin-done').onclick=async()=>{ $('d-finish').close(); S.fresh=false; await postJSON('/api/general',{finish:true}); };

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
    +'<span class=glyphs><svg viewBox="0 0 16 16">'+ARCS.map((d,i)=>ARC('a'+(i+1),d)).reverse().join('')+ARCDOT+'</svg></span></div>');
  if(!on&&row) row.remove();
}
let netErr=false;
async function scan(){
  if(netErr) say('net-say',''); $('rescan').disabled=true; scanning(true);
  let raw=null, err='Can’t reach AirWriter.';
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
  $('su-log').classList.toggle('hide',!(setup&&S.ssid));
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
  // W-1088: in setup, the third step says what to do next: the test button,
  // then a print from a phone. The address is for later.
  if(setup){
    $('d-net').close();
    const b='<b>'+esc(ssid)+'</b>', host=esc(S.host||'airwriter.local');
    const ps=PR?PR.printers:[], one=ps.length===1?esc(ps[0].name):'your printer';
    const row=(ic,g,t,s)=>'<div class="row prow"><span class="ic '+ic+'"><svg><use href="#'+g+'"/></svg></span><span class=txt>'+t+'<small>'+s+'</small></span></div>';
    $('su-joining').innerHTML='AirWriter is joining '+b+'. Your phone reconnects to it by itself.';
    $('su-then').innerHTML=ps.length
      ?row('g','g-airprint','Print from your phone or computer','Choose Print (File &gt; Print on a Mac), then '+one+'.')
        +row('k','g-dots','Print a test page','When AirWriter’s lights turn white, press the test button in the middle.')
      :row('k','g-add','Add a printer','Connect it and switch it on. It shows up at <b>'+host+'</b>.');
    $('su-later').innerHTML=ps.length?'AirWriter’s settings page is at <b>'+host+'</b>.':'';
    suStep(3);
    return;
  }
  // From its own network the page cannot follow it: say where it will be.
  const q='“'+esc(ssid)+'”', host=esc(S.host||'airwriter.local'), link='<a href="http://'+host+'/">'+host+'</a>';
  waitFor('Joining “'+ssid+'”','AirWriter restarts on the new network. This page reloads when AirWriter is back.',true,
    'Not back yet. On '+q+', open '+link+'. If AirWriter couldn’t join, join the '
      +esc($('apname').textContent)+' network to try again.');
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

/* ── About: software, restart, erase, Rainbow status lights ────────── */
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
  else if(u.state==='checking'){ cls='a'; text='Checking…'; }
  else if(u.state==='downloading'){ cls='a'; text='Downloading'+(u.progress>=0?' '+u.progress+'%':'…'); }
  else if(u.state==='installing'){ cls='a'; text='Installing…'; }
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
    waitFor('Installing','Restarting with '+esc(u.latest||'the new version')+'. This page reloads when AirWriter is back.',true);
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
    else waitFor('Installing','Restarting with the new version. This page reloads when AirWriter is back.',true);
  }catch(e){ $('d-wait').close(); say('about-say','The upload failed.',1); }
  $('fw').value='';
};
fwPoll(); setInterval(()=>{ if(!document.hidden&&!FW_BUSY.includes(U.state)) fwPoll(); },15000);
$('restart').onclick=async()=>{
  const r=await postJSON('/api/restart');
  if(r.error){ $('d-restart').close(); say('about-say',r.error,1); return; }
  waitFor('Restarting',S.printing?'AirWriter restarts after the current print. This page reloads when it’s back.':'This page reloads when AirWriter is back.',true);
};
$('erase').onclick=async()=>{
  const r=await postJSON('/api/erase');
  if(r.error){ $('d-erase').close(); say('about-say',r.error,1); return; }
  waitFor('Erasing','AirWriter is starting over. Join the '+esc($('apname').textContent)+' network to set it up again.',false);
};

/* ── the password (W-1156) ─────────────────────────────────────────── */
/* The page proves the password with a challenge from the board, so it never
   crosses the network (crypto.js). A first answer of 400 is a challenge the
   board no longer has (it restarted): one more try with a fresh one. */
function needSignin(label){
  if(SIGNIN) return;
  SIGNIN=true;
  for(const d of $$('dialog')) if(d.open) d.close();
  SIGNIN_LABEL=label;
  setLost(false);
  $('d-signin').showModal();
}
$('d-signin').addEventListener('cancel',e=>e.preventDefault());   // Escape doesn't close it
/* Forgot password? answers truly: where the label's password is while it
   still works, how to start over once it's been changed or set. */
let SIGNIN_LABEL=false;
$('si-forgot').onclick=()=>{
  $('si-forgot').classList.add('hide');
  $(SIGNIN_LABEL?'si-label':'si-reset').classList.remove('hide');
};
function pwProblem(p){
  if(p.length<8) return 'Use at least 8 characters.';
  if(p.length>63||!/^[\x20-\x7e]+$/.test(p)) return 'Use letters, numbers and punctuation only.';
  return '';
}
/* A fresh challenge, and the key password pw makes with the board's salt. */
async function pwProve(pw){
  const n=await getJSON('/api/auth/nonce',8000);
  if(!n) return null;
  return {nonce:n.nonce,K:awPbkdf2(awUtf8(pw),awUnhex(n.salt),n.iter)};
}
$('si').onsubmit=async e=>{
  e.preventDefault();
  const pw=$('si-pw').value, go=$('si-go');
  if(!pw||go.disabled) return;
  go.disabled=true; say('si-say','');
  for(let tries=0;tries<2;tries++){
    const p=await pwProve(pw);
    let r=null;
    if(p) r=await fetchIn('/api/auth/signin',30000,{method:'POST',
      body:JSON.stringify({nonce:p.nonce,proof:awSignProof(p.K,'signin',p.nonce)})}).catch(()=>null);
    if(!r){ say('si-say','Can’t reach AirWriter.',1); break; }
    if(r.ok){ location.reload(); return; }
    if(r.status===400&&!tries) continue;
    const t=(await r.text()).trim()||'Try again.';
    say('si-say',t,1);
    if(r.status===429){
      const wait=Math.max(1,+r.headers.get('Retry-After')||60);
      setTimeout(()=>{ go.disabled=false; say('si-say',''); },wait*1000);
      return;
    }
    break;
  }
  go.disabled=false;
  $('si-pw').select();
};
function drawPassword(pw){
  $('pw-btn').textContent=pw==='none'?'Set…':'Change…';
  $('pw-btn').dataset.dlg=pw==='none'?'d-pwset':'d-pwchange';
  $('signout').classList.toggle('hide',pw==='none'||setup);
}
$('pwc-go').onclick=async()=>{
  const cur=$('pwc-cur').value, nw=$('pwc-new').value, bad=pwProblem(nw), go=$('pwc-go');
  if(!cur) return say('pwc-say','Wrong password.',1);
  if(bad) return say('pwc-say',bad,1);
  go.disabled=true; say('pwc-say','');
  let r;
  for(let tries=0;tries<2;tries++){
    const p=await pwProve(cur);
    r=p?await postJSON('/api/auth/password',{nonce:p.nonce,proof:awSignProof(p.K,'change',p.nonce),
      enc:awChangeSeal(p.K,p.nonce,nw)}):{error:'Can’t reach AirWriter.'};
    if(r.error!=='Try again.'||tries) break;
  }
  go.disabled=false;
  if(r.error) return say('pwc-say',r.error,1);
  $('pwc-cur').value=$('pwc-new').value='';
  $('d-pwchange').close(); poll();
};
$('pws-go').onclick=async()=>{
  const nw=$('pws-new').value, bad=pwProblem(nw), go=$('pws-go');
  if(bad) return say('pws-say',bad,1);
  go.disabled=true; say('pws-say','');
  const r=await postJSON('/api/auth/password',{new:nw});
  go.disabled=false;
  if(r.error) return say('pws-say',r.error,1);
  $('pws-new').value='';
  $('d-pwset').close(); poll();
};
$('signout').onclick=async()=>{ await postJSON('/api/auth/signout'); location.reload(); };
/* Which board this is (W-1109), the LC's name, and Rainbow status lights. */
function drawBoard(s){
  $('about-rev').textContent=(!s.board||s.board==='rev-a'?'Rev A':s.board)+' · Wells Workshop';
  const name=s.lc?'AirWriter LC':'AirWriter';
  $('brand').textContent=$('about-name').textContent=document.title=name;
  if(!rbBusy) $('rainbow').checked=!!s.rainbow;
  if(typeof s.theme==='string'&&!thBusy) setTheme(s.theme);
}
/* About's Appearance: the page's colours, kept on the board. */
let thBusy=false;
$('tp').onclick=async e=>{
  const b=e.target.closest('button'), was=document.documentElement.dataset.theme||'';
  if(!b||thBusy||b.dataset.t===was) return;
  thBusy=true; setTheme(b.dataset.t);
  const r=await postJSON('/api/general',{theme:b.dataset.t||'platinum'});
  thBusy=false;
  if(r.error){ setTheme(was); say('about-say',r.error,1); }
};
let rbBusy=false;
$('rainbow').onchange=async()=>{
  const on=$('rainbow').checked; rbBusy=true;
  const r=await postJSON('/api/general',{rainbow:on});
  rbBusy=false;
  if(r.error){ $('rainbow').checked=!on; say('about-say',r.error,1); }
};


let rt; addEventListener('resize',()=>{ clearTimeout(rt); rt=setTimeout(()=>{ if(pane==='printers'&&sel!==null) render(); },150); });
