// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole UI: one file, no external requests. A printer's config page has to
// work on a phone that has just joined the printer's own access point and has
// no internet at all, so nothing here may reference a CDN or a webfont.
//
// The Wi-Fi panel follows the one every user already knows -- the network list
// in iOS Settings: current network on top with a tick, everything else in a
// scrolling list, each row carrying a signal glyph and a lock. Nothing is
// explained that the list already shows.
//
// The look is Snow White, frog design's 1984 language for Apple: white, crisp
// grey seams, tight radii, no ornament. Apple set product names in Garamond
// Light Condensed and interface text in a grotesque, so headings are serif and
// controls are not. No webfont is loaded, so the serif falls back through what
// Macs and iPhones actually ship.
#pragma once

static const char WEBUI_HTML[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>AirWriter</title><style>"

":root{--bg:#e6e6e4;--card:#fff;--fg:#1c1c1a;--dim:#83817a;--line:#dcdad4;"
"--seam:#c9c6bd;--edge:#fff;--sel:#2f6ab0;--ok:#3f8552;--warn:#c07b1e;"
"--serif:'Apple Garamond','ITC Garamond',Garamond,'Hoefler Text',Palatino,Georgia,serif;"
"--sans:'Helvetica Neue',Helvetica,Univers,Arial,sans-serif}"
"@media(prefers-color-scheme:dark){:root{--bg:#131315;--card:#1f1f22;--fg:#f0efec;"
"--dim:#918f89;--line:#33333a;--seam:#3d3d45;--edge:#2a2a2e;--sel:#4d8fd6}}"

"*{box-sizing:border-box}"
"body{margin:0;padding:26px 16px 72px;background:var(--bg);color:var(--fg);"
"font:15px/1.5 var(--sans);display:flex;flex-direction:column;align-items:center;"
"-webkit-text-size-adjust:100%}"
"main{width:100%;max-width:30rem;display:flex;flex-direction:column;gap:13px}"

"header{display:flex;align-items:center;justify-content:space-between;gap:14px;"
"padding:2px 3px 12px}"
"h1{margin:0;font:300 31px/1 var(--serif);letter-spacing:.015em}"
"h1 span{color:var(--dim)}"
"#model{font:400 15px/1.3 var(--serif);color:var(--dim);text-align:right}"
/* The one Snow White quotation: thin recessed vents, used once. */
"#vent{height:9px;flex:1 1 auto;max-width:80px;"
"background:repeating-linear-gradient(to bottom,var(--seam) 0 1px,transparent 1px 3px)}"

"section{background:var(--card);border:1px solid var(--line);border-top-color:var(--edge);"
"border-radius:6px;padding:14px 15px 15px;box-shadow:0 1px 2px rgba(0,0,0,.04)}"
"h2{font:500 10px/1 var(--sans);text-transform:uppercase;letter-spacing:.13em;"
"color:var(--dim);margin:0 0 11px}"
"label{display:block;font-size:12px;color:var(--dim);margin:12px 0 4px}"
"input,select{width:100%;padding:8px 10px;font:inherit;font-size:14px;color:var(--fg);"
"background:var(--card);border:1px solid var(--seam);border-radius:4px;"
"-webkit-appearance:none;appearance:none}"
"input:focus,select:focus{outline:0;border-color:var(--sel);"
"box-shadow:0 0 0 2px rgba(47,106,176,.18)}"
"select{background-image:linear-gradient(45deg,transparent 50%,var(--dim) 50%),"
"linear-gradient(135deg,var(--dim) 50%,transparent 50%);"
"background-position:calc(100% - 17px) 16px,calc(100% - 12px) 16px;"
"background-size:5px 5px,5px 5px;background-repeat:no-repeat;padding-right:32px}"
".row{display:flex;gap:8px;margin-top:13px;align-items:center}"
"button{padding:8px 15px;font:inherit;font-size:13px;font-weight:500;color:#fff;"
"background:var(--sel);border:1px solid var(--sel);border-radius:4px;cursor:pointer}"
"button.ghost{background:var(--card);color:var(--fg);border-color:var(--seam)}"
"button:disabled{opacity:.35;cursor:default}"
".note{font-size:12px;color:var(--dim);margin:11px 0 0}"
".hide{display:none!important}"

/* The network list, shaped like the one in iOS Settings. Five and a half rows
   so it is visibly scrollable without a scrollbar having to say so. */
"#list{margin-top:6px;border:1px solid var(--seam);border-radius:5px;overflow:hidden}"
"#rows{max-height:236px;overflow-y:auto;-webkit-overflow-scrolling:touch}"
".nr{display:flex;align-items:center;gap:10px;width:100%;padding:0 11px;height:43px;"
"background:var(--card);border:0;border-bottom:1px solid var(--line);border-radius:0;"
"color:var(--fg);font:inherit;font-size:14px;text-align:left;cursor:pointer}"
".nr:last-child{border-bottom:0}"
".nr .tick{width:14px;flex:0 0 14px;color:var(--sel);font-size:13px}"
".nr .nm{flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
".nr .lk{color:var(--dim);font-size:11px}"
".nr[aria-selected=true]{background:var(--bg)}"
".nr.other{color:var(--sel)}"

/* Three arcs and a dot: the glyph everyone reads as signal strength. */
".wf{flex:0 0 17px;height:13px;position:relative}"
".wf i{position:absolute;left:50%;bottom:0;transform:translateX(-50%);"
"border:1.6px solid var(--fg);border-bottom:0;border-radius:50% 50% 0 0/100% 100% 0 0}"
".wf i:nth-child(1){width:5px;height:3px;bottom:0}"
".wf i:nth-child(2){width:11px;height:6px}"
".wf i:nth-child(3){width:17px;height:9px}"
".wf i.off{opacity:.22}"

"#cur{display:flex;align-items:center;gap:10px;padding:2px 1px 11px;"
"border-bottom:1px solid var(--line)}"
"#cur .nm{flex:1;font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
"#cur .ip{font-size:12px;color:var(--dim);font-variant-numeric:tabular-nums}"
"#cur .tick{color:var(--sel);width:14px;flex:0 0 14px}"

/* Status: small tiles rather than a list of sentences. */
".tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(6.6rem,1fr));gap:1px;"
"background:var(--line);border:1px solid var(--seam);border-radius:5px;overflow:hidden}"
".t{background:var(--card);padding:9px 10px;min-width:0}"
".t em{display:block;font-style:normal;font-size:10px;letter-spacing:.1em;"
"text-transform:uppercase;color:var(--dim);margin-bottom:2px}"
".t b{display:block;font-weight:400;font-size:13px;white-space:nowrap;overflow:hidden;"
"text-overflow:ellipsis}"
".dot{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--ok);"
"margin-right:6px;vertical-align:middle}"
".dot.stop{background:var(--warn)}"

".sp{display:inline-block;width:11px;height:11px;margin-right:7px;vertical-align:-1px;"
"border:2px solid rgba(128,128,128,.35);border-top-color:currentColor;border-radius:50%;"
"animation:sp .8s linear infinite}"
"@keyframes sp{to{transform:rotate(360deg)}}"

"#over{position:fixed;inset:0;background:var(--bg);display:flex;flex-direction:column;"
"align-items:center;justify-content:center;gap:15px;text-align:center;padding:24px;z-index:9}"
"#big{width:30px;height:30px;border:2px solid var(--line);border-top-color:var(--fg);"
"border-radius:50%;animation:sp .9s linear infinite}"
"#ovtitle{font:300 23px/1 var(--serif);letter-spacing:.015em}"
"#ovtext{margin:0;color:var(--dim);font-size:13px}"
"a.btn{padding:8px 15px;font-size:13px;font-weight:500;border:1px solid var(--seam);"
"border-radius:4px;color:var(--fg);text-decoration:none;background:var(--card)}"
"</style></head><body><main>"

"<header><h1>Air<span>Writer</span></h1><div id=vent></div>"
"<div id=model>ImageWriter II</div></header>"

"<section><h2>Wi-Fi</h2>"
"<div id=cur class=hide><span class=tick>&#10003;</span>"
"<span class=nm id=curname></span>"
"<span class=wf id=curwf><i></i><i></i><i></i></span>"
"<span class=ip id=curip></span></div>"
"<div id=list class=hide><div id=rows></div></div>"
"<label for=ssid id=netlabel class=hide>Network name</label>"
"<input id=ssid class=hide autocomplete=off placeholder='Network name'>"
"<label for=pw id=pwlabel>Password</label>"
"<input id=pw type=password autocomplete=off placeholder='Type to replace'>"
"<div class=row><button id=scan class=ghost type=button>Scan</button>"
"<button id=savewifi type=button disabled>Save &amp; restart</button></div>"
"<p class=note>2.4 GHz only.</p></section>"

"<section><h2>Printer</h2>"
"<label for=devsel>Model</label><select id=devsel></select>"
"<label for=queue id=queuelabel class=hide>Name</label>"
"<input id=queue class=hide autocomplete=off placeholder='Printer name'>"
"<label for=host>Hostname</label><input id=host autocomplete=off>"
"<label for=dither>Dithering</label>"
"<select id=dither>"
"<option value=atkinson>Atkinson</option>"
"<option value=floyd>Floyd-Steinberg</option>"
"<option value=bayer>Ordered</option>"
"<option value=threshold>Threshold</option>"
"</select>"
"<div class=row><button id=savegen type=button>Save</button>"
"<a class=btn href='http://localhost:631/admin' target=_blank>Open CUPS</a></div>"
"<p class=note id=uri></p></section>"

"<section><h2>Status</h2>"
"<div class=tiles id=tiles></div>"
"<label for=fw>Firmware image</label><input id=fw type=file accept=.bin>"
"<div class=row><button id=doup class=ghost type=button>Install</button></div>"
"<p class=note id=upnote>Only the ImageWriter II is tested.</p></section>"

"</main>"

"<div id=over class=hide><div id=big></div>"
"<div id=ovtitle>Restarting</div><p id=ovtext></p>"
"<button id=ovbtn class=ghost type=button>Reload</button></div>"

"<script>"
"const $=i=>document.getElementById(i);"
"let custom=0,first=1,suffix='',chosen='',nets=[];"

"const MODELS=[['ImageWriter II','imagewriter'],['ImageWriter','imagewriter'],"
"['ImageWriter LQ','imagewriter-lq']];"

"(function(){const d=$('devsel');"
"for(const[n]of MODELS){const o=new Option(n,n);d.append(o);}"
"d.append(new Option('Other\\u2026','__other__'));})();"

"$('devsel').onchange=()=>{const v=$('devsel').value;"
"if(v=='__other__'){$('queue').classList.remove('hide');"
"$('queuelabel').classList.remove('hide');$('queue').focus();return;}"
"$('queue').classList.add('hide');$('queuelabel').classList.add('hide');"
"$('queue').value=v;$('model').textContent=v;"
"const m=MODELS.find(x=>x[0]==v);if(m&&suffix)$('host').value=m[1]+'-'+suffix;};"
"$('queue').oninput=()=>{$('model').textContent=$('queue').value||'Printer';};"

/* Three arcs, lit from the bottom up. -60 and better is full. */
"function level(r){return r>=-60?3:r>=-70?2:1;}"
"function arcs(el,n){[...el.children].forEach((a,i)=>a.classList.toggle('off',i>=n));}"

"function drawList(){const box=$('rows');box.innerHTML='';"
"for(const n of nets){const b=document.createElement('button');"
"b.type='button';b.className='nr';b.setAttribute('aria-selected',n.s==chosen);"
"b.innerHTML='<span class=tick>'+(n.s==chosen?'&#10003;':'')+'</span>'"
"+'<span class=nm></span>'+(n.a?'<span class=lk>&#128274;</span>':'')"
"+'<span class=wf><i></i><i></i><i></i></span>';"
"b.querySelector('.nm').textContent=n.s;"
"arcs(b.querySelector('.wf'),level(n.r));"
"b.onclick=()=>{chosen=n.s;custom=0;"
"$('ssid').classList.add('hide');$('netlabel').classList.add('hide');"
"drawList();mark();$('pw').focus();};"
"box.append(b);}"
"const o=document.createElement('button');o.type='button';o.className='nr other';"
"o.innerHTML='<span class=tick></span><span class=nm>Other\\u2026</span>';"
"o.onclick=()=>{custom=1;chosen='';drawList();"
"$('ssid').classList.remove('hide');$('netlabel').classList.remove('hide');"
"$('ssid').value='';$('ssid').focus();};"
"box.append(o);"
"$('list').classList.remove('hide');}"

"const TILES=[['state','State'],['job','Last job'],['uptime','Uptime'],"
"['heap','Memory'],['version','Firmware']];"

"function draw(s){"
"let h='';for(const[k,label]of TILES){if(!s[k])continue;"
"let v=k=='state'?'<span class=\"dot'+(s.stopped?' stop':'')+'\"></span>'+s[k]:s[k];"
"h+='<div class=t><em>'+label+'</em><b>'+v+'</b></div>';}"
"$('tiles').innerHTML=h;"

/* Connected: one row, the way a phone shows it. No band, no cipher -- if it
   is joined, neither changes anything the reader can act on. */
"if(!s.setup&&s.ssid){$('cur').classList.remove('hide');"
"$('curname').textContent=s.ssid;$('curip').textContent=s.ip||'';"
"arcs($('curwf'),{Excellent:3,Good:3,Fair:2,Weak:1}[s.signal]||1);}"
"else $('cur').classList.add('hide');"

"if(s.host){const h2=s.host.replace('.local','');"
"const i=h2.lastIndexOf('-');if(i>0)suffix=h2.slice(i+1);"
"if(!$('host').value)$('host').value=h2;}"
"if(s.uri)$('uri').textContent=s.uri;"

"if(s.queue){const known=MODELS.some(x=>x[0]==s.queue);"
"if(!$('queue').value){$('queue').value=s.queue;"
"$('devsel').value=known?s.queue:'__other__';"
"if(!known){$('queue').classList.remove('hide');$('queuelabel').classList.remove('hide');}}"
"$('model').textContent=s.queue;}"
"if(s.dither&&!$('dither').dataset.touched)$('dither').value=s.dither;"

"if(first){first=0;if(s.setup)scan();else if(s.ssid)chosen=s.ssid;}}"

"function poll(){fetch('/api/status').then(r=>r.json()).then(draw).catch(()=>{});}"
"poll();setInterval(poll,4000);"

"function mark(){$('savewifi').disabled=0;}"
"$('ssid').oninput=mark;$('pw').oninput=mark;"
"$('dither').onchange=()=>{$('dither').dataset.touched=1;};"

"async function scan(){const b=$('scan');b.disabled=1;"
"b.innerHTML='<span class=sp></span>Scanning';"
"try{const raw=await(await fetch('/api/scan')).json();"
"const best={};for(const n of raw){if(!n.s)continue;"
"if(!best[n.s]||n.r>best[n.s].r)best[n.s]=n;}"
"nets=Object.values(best).sort((x,y)=>y.r-x.r);"
"custom=0;$('ssid').classList.add('hide');$('netlabel').classList.add('hide');"
"drawList();}catch(e){}"
"b.disabled=0;b.textContent='Scan';}"
"$('scan').onclick=scan;"

"function overlay(title,text,secs){$('ovtitle').textContent=title;"
"$('over').classList.remove('hide');let left=secs;"
"const tick=()=>{$('ovtext').textContent=text+' \\u00b7 '+left+'s';"
"if(left--<=0){clearInterval(h);location.reload();}};"
"tick();const h=setInterval(tick,1000);}"
"$('ovbtn').onclick=()=>location.reload();"

"$('savewifi').onclick=async()=>{"
"const name=custom?$('ssid').value:chosen;"
"if(!name){$('ssid').classList.remove('hide');$('ssid').focus();return;}"
"$('savewifi').disabled=1;"
"try{await fetch('/api/wifi',{method:'POST',"
"body:JSON.stringify({ssid:name,pass:$('pw').value})});}catch(e){}"
"overlay('Restarting','Joining \\u201c'+name+'\\u201d',25);};"

"$('savegen').onclick=async()=>{const b=$('savegen');b.disabled=1;"
"try{await fetch('/api/general',{method:'POST',body:JSON.stringify({"
"queue:$('queue').value,host:$('host').value,dither:$('dither').value})});"
"b.textContent='Saved';}catch(e){b.textContent='Failed';}"
"setTimeout(()=>{b.disabled=0;b.textContent='Save';},2500);};"

"$('doup').onclick=async()=>{const f=$('fw').files[0];if(!f)return;"
"$('doup').disabled=1;$('upnote').textContent='Uploading '+(f.size>>10)+' KB\\u2026';"
"try{const r=await fetch('/update',{method:'POST',body:f});"
"if(r.ok)overlay('Installing','Writing firmware',30);"
"else $('upnote').textContent='Rejected: '+await r.text();}"
"catch(e){$('upnote').textContent='Upload failed.'}$('doup').disabled=0;};"
"</script></body></html>";
