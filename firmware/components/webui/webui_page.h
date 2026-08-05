// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole UI: one file, no external requests. A printer's config page has to
// work on a phone that has just joined the printer's own access point and has
// no internet at all, so nothing here may reference a CDN or a webfont.
//
// This is a device panel, not a settings form. It is opened in two moments and
// they want different pages:
//
//   Setup, once, on a phone. There is no network, so nothing except choosing
//   one matters. The page IS the network picker; everything else is hidden.
//
//   A glance, later. "Is it working, and did my page print?" That is the hero
//   line, readable across a desk. Everything editable is one collapsed list
//   underneath, opened only when something needs changing.
//
// Hence: one hero, one list of rows, one quiet footer. Rows disclose inline
// the way iOS Settings does, rather than stacking four boxed forms of equal
// weight and letting them compete.
//
// The look is Snow White, frog design's 1984 language for Apple: a white face,
// fine seams, tight radii, a lot of air, no ornament. Apple set product names
// in Garamond Light Condensed and interface text in a grotesque, so the device
// name is serif and the controls are not.
#pragma once

static const char WEBUI_HTML[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>AirWriter</title><style>"

":root{--bg:#f0efec;--face:#fff;--ink:#1b1b19;--dim:#8a877f;--seam:#e3e1da;"
"--hair:#efeee9;--blue:#2f6ab0;--ok:#3f8552;--warn:#c07b1e;"
"--serif:'Apple Garamond','ITC Garamond',Garamond,'Hoefler Text',Palatino,Georgia,serif;"
"--sans:'Helvetica Neue',Helvetica,Univers,Arial,sans-serif}"
"@media(prefers-color-scheme:dark){:root{--bg:#111113;--face:#1c1c1f;--ink:#f2f1ee;"
"--dim:#8f8d87;--seam:#2c2c31;--hair:#232327;--blue:#5896db}}"

"*{box-sizing:border-box}"
"body{margin:0;padding:44px 18px 80px;background:var(--bg);color:var(--ink);"
"font:15px/1.5 var(--sans);-webkit-text-size-adjust:100%;"
"display:flex;flex-direction:column;align-items:center}"
"main{width:100%;max-width:26rem}"

/* Hero: the object, then whether it is working. Nothing else up here. */
".brand{font:500 10px/1 var(--sans);letter-spacing:.19em;text-transform:uppercase;"
"color:var(--dim);margin-bottom:14px}"
"h1{margin:0;font:300 34px/1.05 var(--serif);letter-spacing:.01em}"
".state{display:flex;align-items:center;gap:8px;margin:9px 0 0;font-size:14px;"
"color:var(--dim)}"
".dot{width:7px;height:7px;border-radius:50%;background:var(--ok);flex:0 0 7px}"
".dot.warn{background:var(--warn)}.dot.off{background:var(--dim)}"
".state s{text-decoration:none;color:var(--seam)}"

/* One list. Rows open in place. */
".list{margin-top:26px;background:var(--face);border:1px solid var(--seam);"
"border-radius:8px;overflow:hidden}"
".r{display:flex;align-items:center;gap:12px;width:100%;padding:0 15px;height:50px;"
"background:0;border:0;border-top:1px solid var(--hair);border-radius:0;color:var(--ink);"
"font:inherit;font-size:15px;text-align:left;cursor:pointer}"
".r:first-child{border-top:0}"
".r .k{flex:0 0 auto;color:var(--ink)}"
".r .v{flex:1;text-align:right;color:var(--dim);white-space:nowrap;overflow:hidden;"
"text-overflow:ellipsis}"
".r .c{flex:0 0 9px;color:var(--dim);font-size:11px;transition:transform .15s}"
".r[aria-expanded=true] .c{transform:rotate(90deg)}"
".r[aria-expanded=true] .v{opacity:0}"
".body{padding:2px 15px 17px;border-top:1px solid var(--hair)}"

/* Footer: reference, and the things you do once a year. */
".foot{margin:22px 4px 0;font-size:12.5px;line-height:1.9;color:var(--dim)}"
".foot a{color:var(--blue);text-decoration:none;cursor:pointer}"
".foot .sep{color:var(--seam);margin:0 7px}"

"label{display:block;font-size:12px;color:var(--dim);margin:13px 0 5px}"
"input,select{width:100%;padding:9px 11px;font:inherit;font-size:15px;color:var(--ink);"
"background:var(--bg);border:1px solid var(--seam);border-radius:6px;"
"-webkit-appearance:none;appearance:none}"
"input:focus,select:focus{outline:0;border-color:var(--blue)}"
"select{background-image:linear-gradient(45deg,transparent 50%,var(--dim) 50%),"
"linear-gradient(135deg,var(--dim) 50%,transparent 50%);"
"background-position:calc(100% - 18px) 18px,calc(100% - 13px) 18px;"
"background-size:5px 5px,5px 5px;background-repeat:no-repeat;padding-right:34px}"
".acts{display:flex;gap:9px;align-items:center;margin-top:15px}"
"button{padding:9px 16px;font:inherit;font-size:14px;font-weight:500;color:#fff;"
"background:var(--blue);border:1px solid var(--blue);border-radius:6px;cursor:pointer}"
"button.q{background:0;color:var(--ink);border-color:var(--seam)}"
"button:disabled{opacity:.32;cursor:default}"
".hint{font-size:12px;color:var(--dim);margin:13px 0 0}"
".hide{display:none!important}"

/* The network list everyone already knows how to read. */
".nets{margin-top:4px;border:1px solid var(--seam);border-radius:6px;overflow:hidden}"
".scroll{max-height:236px;overflow-y:auto;-webkit-overflow-scrolling:touch}"
".n{display:flex;align-items:center;gap:11px;width:100%;padding:0 12px;height:43px;"
"background:var(--face);border:0;border-top:1px solid var(--hair);border-radius:0;"
"color:var(--ink);font:inherit;font-size:14.5px;text-align:left;cursor:pointer}"
".n:first-child{border-top:0}"
".n .t{flex:0 0 13px;color:var(--blue)}"
".n .s{flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
".n .l{color:var(--dim);font-size:11px}"
".n.other{color:var(--blue)}"

/* Three arcs and a dot. */
".wf{flex:0 0 17px;height:13px;position:relative}"
".wf i{position:absolute;left:50%;bottom:0;transform:translateX(-50%);"
"border:1.6px solid var(--ink);border-bottom:0;border-radius:50% 50% 0 0/100% 100% 0 0}"
".wf i:nth-child(1){width:5px;height:3px}"
".wf i:nth-child(2){width:11px;height:6px}"
".wf i:nth-child(3){width:17px;height:9px}"
".wf i.off{opacity:.2}"

".sp{display:inline-block;width:11px;height:11px;margin-right:7px;vertical-align:-1px;"
"border:2px solid rgba(128,128,128,.3);border-top-color:currentColor;border-radius:50%;"
"animation:sp .8s linear infinite}"
"@keyframes sp{to{transform:rotate(360deg)}}"

"#over{position:fixed;inset:0;background:var(--bg);display:flex;flex-direction:column;"
"align-items:center;justify-content:center;gap:16px;text-align:center;padding:26px;z-index:9}"
"#big{width:28px;height:28px;border:2px solid var(--seam);border-top-color:var(--ink);"
"border-radius:50%;animation:sp .9s linear infinite}"
"#ovt{font:300 25px/1 var(--serif)}"
"#ovs{margin:0;color:var(--dim);font-size:13.5px}"
"</style></head><body><main>"

"<div class=brand>AirWriter</div>"
"<h1 id=name>ImageWriter II</h1>"
"<p class=state><span class=dot id=dot></span><span id=stateline>Starting</span></p>"

/* Setup: the page is the picker, and nothing else is on it. */
"<div id=setup class=hide>"
"<div class=nets><div class=scroll id=rows2></div></div>"
"<label for=ssid2 id=lbl2 class=hide>Network name</label>"
"<input id=ssid2 class=hide autocomplete=off placeholder='Network name'>"
"<label for=pw2>Password</label>"
"<input id=pw2 type=password autocomplete=off placeholder='Type to replace'>"
"<div class=acts><button id=join type=button disabled>Join</button>"
"<button id=scan2 class=q type=button>Scan</button></div>"
"<p class=hint>2.4 GHz only.</p></div>"

/* Everything else, collapsed until it is wanted. */
"<div id=panel class='list hide'>"

"<button class=r id=r0 type=button aria-expanded=false>"
"<span class=k>Network</span><span class=v id=v0></span><span class=c>&#9654;</span></button>"
"<div class='body hide' id=b0>"
"<div class=nets><div class=scroll id=rows></div></div>"
"<label for=ssid id=lbl class=hide>Network name</label>"
"<input id=ssid class=hide autocomplete=off placeholder='Network name'>"
"<label for=pw>Password</label>"
"<input id=pw type=password autocomplete=off placeholder='Type to replace'>"
"<div class=acts><button id=savewifi type=button disabled>Save &amp; restart</button>"
"<button id=scan class=q type=button>Scan</button></div>"
"<p class=hint>2.4 GHz only.</p></div>"

"<button class=r id=r1 type=button aria-expanded=false>"
"<span class=k>Printer</span><span class=v id=v1></span><span class=c>&#9654;</span></button>"
"<div class='body hide' id=b1>"
"<label for=devsel>Model</label><select id=devsel></select>"
"<label for=queue id=qlbl class=hide>Name</label>"
"<input id=queue class=hide autocomplete=off placeholder='Printer name'>"
"<label for=host>Hostname</label><input id=host autocomplete=off>"
"<div class=acts><button id=savegen type=button>Save</button></div>"
"<p class=hint id=uri></p></div>"

"<button class=r id=r2 type=button aria-expanded=false>"
"<span class=k>Dithering</span><span class=v id=v2></span><span class=c>&#9654;</span></button>"
"<div class='body hide' id=b2>"
"<select id=dither>"
"<option value=atkinson>Atkinson</option>"
"<option value=floyd>Floyd-Steinberg</option>"
"<option value=bayer>Ordered</option>"
"<option value=threshold>Threshold</option>"
"</select>"
"<div class=acts><button id=savedit type=button>Save</button></div>"
"<p class=hint>Atkinson suits photographs. Ordered reads better for small text.</p></div>"

"</div>"

"<p class=foot id=foot></p>"
"<input id=fw type=file accept=.bin class=hide>"

"</main>"

"<div id=over class=hide><div id=big></div>"
"<div id=ovt>Restarting</div><p id=ovs></p>"
"<button id=ovbtn class=q type=button>Reload</button></div>"

"<script>"
"const $=i=>document.getElementById(i);"
"let first=1,setup=0,suffix='',chosen='',custom=0,nets=[],open_=-1;"

"const MODELS=[['ImageWriter II','imagewriter'],['ImageWriter','imagewriter'],"
"['ImageWriter LQ','imagewriter-lq']];"
"(function(){const d=$('devsel');for(const[n]of MODELS)d.append(new Option(n,n));"
"d.append(new Option('Other\\u2026','__other__'));})();"

/* Only one row open at a time: the list stays a list. */
"function row(i){const r=$('r'+i),b=$('b'+i);"
"const now=r.getAttribute('aria-expanded')=='true';"
"for(const j of [0,1,2]){$('r'+j).setAttribute('aria-expanded','false');"
"$('b'+j).classList.add('hide');}"
"if(!now){r.setAttribute('aria-expanded','true');b.classList.remove('hide');open_=i;"
"if(i==0&&!nets.length)scan();}else open_=-1;}"
"for(const i of [0,1,2])$('r'+i).onclick=()=>row(i);"

"function level(r){return r>=-60?3:r>=-70?2:1;}"
"function arcs(el,n){[...el.children].forEach((a,i)=>a.classList.toggle('off',i>=n));}"

"function drawNets(box,onpick){box.innerHTML='';"
"for(const n of nets){const b=document.createElement('button');b.type='button';b.className='n';"
"b.innerHTML='<span class=t>'+(n.s==chosen?'&#10003;':'')+'</span><span class=s></span>'"
"+(n.a?'<span class=l>&#128274;</span>':'')+'<span class=wf><i></i><i></i><i></i></span>';"
"b.querySelector('.s').textContent=n.s;arcs(b.querySelector('.wf'),level(n.r));"
"b.onclick=()=>{chosen=n.s;custom=0;onpick();};box.append(b);}"
"const o=document.createElement('button');o.type='button';o.className='n other';"
"o.innerHTML='<span class=t></span><span class=s>Other\\u2026</span>';"
"o.onclick=()=>{custom=1;chosen='';onpick(1);};box.append(o);}"

"function paintSetup(focus){drawNets($('rows2'),f=>{"
"$('ssid2').classList.toggle('hide',!f);$('lbl2').classList.toggle('hide',!f);"
"if(f){$('ssid2').value='';$('ssid2').focus();}else $('pw2').focus();"
"$('join').disabled=0;paintSetup();});}"
"function paintPanel(){drawNets($('rows'),f=>{"
"$('ssid').classList.toggle('hide',!f);$('lbl').classList.toggle('hide',!f);"
"if(f){$('ssid').value='';$('ssid').focus();}else $('pw').focus();"
"$('savewifi').disabled=0;paintPanel();});}"

"function draw(s){"
"setup=!!s.setup;"
"$('name').textContent=setup?'Set up':(s.queue||'Printer');"
"$('dot').className='dot'+(setup?' off':s.stopped?' warn':'');"
"$('stateline').textContent=setup?'Not connected to a network'"
":[s.state,s.ssid,s.ip].filter(Boolean).join('  \\u00b7  ');"

"$('setup').classList.toggle('hide',!setup);"
"$('panel').classList.toggle('hide',setup);"

"$('v0').textContent=s.ssid||'';"
"$('v1').textContent=s.queue||'';"
"$('v2').textContent={atkinson:'Atkinson',floyd:'Floyd-Steinberg',bayer:'Ordered',"
"threshold:'Threshold'}[s.dither]||'';"

"$('foot').innerHTML='';"
"const bits=[];if(s.job)bits.push('Last job \\u2014 '+s.job);"
"const meta=[s.uptime&&'up '+s.uptime,s.heap&&s.heap+' free',s.version].filter(Boolean);"
"$('foot').textContent=bits.concat(meta.join('  \\u00b7  ')).join('\\n');"
"$('foot').innerHTML=$('foot').innerHTML.replace(/\\n/,'<br>')"
"+'<br><a id=cups href=\"http://localhost:631/admin\" target=_blank>Open CUPS</a>'"
"+'<span class=sep>\\u00b7</span><a id=upd>Update firmware</a>';"
"$('upd').onclick=()=>$('fw').click();"

"if(s.host){const h=s.host.replace('.local','');const i=h.lastIndexOf('-');"
"if(i>0)suffix=h.slice(i+1);if(!$('host').value)$('host').value=h;}"
"if(s.uri)$('uri').textContent=s.uri;"
"if(s.queue&&!$('queue').value){const known=MODELS.some(x=>x[0]==s.queue);"
"$('queue').value=s.queue;$('devsel').value=known?s.queue:'__other__';"
"if(!known){$('queue').classList.remove('hide');$('qlbl').classList.remove('hide');}}"
"if(s.dither&&!$('dither').dataset.touched)$('dither').value=s.dither;"

"if(first){first=0;if(setup)scan();else if(s.ssid)chosen=s.ssid;}}"

"function poll(){fetch('/api/status').then(r=>r.json()).then(draw).catch(()=>{});}"
"poll();setInterval(poll,4000);"

"$('devsel').onchange=()=>{const v=$('devsel').value;"
"const other=v=='__other__';"
"$('queue').classList.toggle('hide',!other);$('qlbl').classList.toggle('hide',!other);"
"if(other){$('queue').focus();return;}"
"$('queue').value=v;$('name').textContent=v;"
"const m=MODELS.find(x=>x[0]==v);if(m&&suffix)$('host').value=m[1]+'-'+suffix;};"
"$('queue').oninput=()=>{$('name').textContent=$('queue').value||'Printer';};"
"$('dither').onchange=()=>{$('dither').dataset.touched=1;};"
"$('ssid').oninput=()=>{$('savewifi').disabled=0;};"
"$('pw').oninput=()=>{$('savewifi').disabled=0;};"
"$('ssid2').oninput=()=>{$('join').disabled=0;};"
"$('pw2').oninput=()=>{$('join').disabled=0;};"

"async function scan(){const bs=[$('scan'),$('scan2')];"
"bs.forEach(b=>{b.disabled=1;b.innerHTML='<span class=sp></span>Scanning';});"
"try{const raw=await(await fetch('/api/scan')).json();"
"const best={};for(const n of raw){if(!n.s)continue;"
"if(!best[n.s]||n.r>best[n.s].r)best[n.s]=n;}"
"nets=Object.values(best).sort((x,y)=>y.r-x.r);"
"custom=0;paintSetup();paintPanel();}catch(e){}"
"bs.forEach(b=>{b.disabled=0;b.textContent='Scan';});}"
"$('scan').onclick=scan;$('scan2').onclick=scan;"

"function overlay(t,s,secs){$('ovt').textContent=t;$('over').classList.remove('hide');"
"let l=secs;const tick=()=>{$('ovs').textContent=s+'  \\u00b7  '+l+'s';"
"if(l--<=0){clearInterval(h);location.reload();}};tick();const h=setInterval(tick,1000);}"
"$('ovbtn').onclick=()=>location.reload();"

"async function join(name,pass,btn){if(!name)return;btn.disabled=1;"
"try{await fetch('/api/wifi',{method:'POST',body:JSON.stringify({ssid:name,pass:pass})});}"
"catch(e){}overlay('Restarting','Joining \\u201c'+name+'\\u201d',25);}"
"$('join').onclick=()=>join(custom?$('ssid2').value:chosen,$('pw2').value,$('join'));"
"$('savewifi').onclick=()=>join(custom?$('ssid').value:chosen,$('pw').value,$('savewifi'));"

"async function save(btn,body){btn.disabled=1;"
"try{await fetch('/api/general',{method:'POST',body:JSON.stringify(body)});"
"btn.textContent='Saved';}catch(e){btn.textContent='Failed';}"
"setTimeout(()=>{btn.disabled=0;btn.textContent='Save';},2200);}"
"$('savegen').onclick=()=>save($('savegen'),{queue:$('queue').value,host:$('host').value});"
"$('savedit').onclick=()=>save($('savedit'),{dither:$('dither').value});"

"$('fw').onchange=async()=>{const f=$('fw').files[0];if(!f)return;"
"overlay('Installing','Uploading '+(f.size>>10)+' KB',45);"
"try{const r=await fetch('/update',{method:'POST',body:f});"
"if(!r.ok){$('over').classList.add('hide');alert('Rejected: '+await r.text());}}"
"catch(e){$('over').classList.add('hide');alert('Upload failed.');}};"
"</script></body></html>";
