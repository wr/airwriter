// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole UI: one file, no external requests. A printer's config page has to
// work on a phone that has just joined the printer's own access point and has
// no internet at all, so nothing here may reference a CDN or a webfont.
//
// The look is Snow White, the language frog design gave Apple in 1984: white
// and fog grey, thin recessed lines, tight radii, no ornament. Apple set its
// product names in Garamond Light Condensed and its interface text in a
// grotesque, so the headings here are serif and the controls are not. No
// webfont is loaded, so the serif falls back through what Macs and iPhones
// actually ship.
//
// Wi-Fi is first because on a new device it is the only thing that matters.
// Status is last: it is reference, not an instruction.
#pragma once

static const char WEBUI_HTML[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>AirWriter</title><style>"

":root{--bg:#eceae5;--card:#fbfaf8;--fg:#26251f;--dim:#7c786e;--line:#d6d3cb;"
"--edge:#fff;--ok:#4a7a52;--warn:#b0762c;"
"--serif:'Apple Garamond','ITC Garamond',Garamond,'Hoefler Text',Palatino,Georgia,serif;"
"--sans:'Helvetica Neue',Helvetica,Univers,Arial,sans-serif}"
"@media(prefers-color-scheme:dark){:root{--bg:#1b1a17;--card:#242320;--fg:#e8e5dd;"
"--dim:#928d81;--line:#37352f;--edge:#2e2c28}}"

"*{box-sizing:border-box}"
"body{margin:0;padding:28px 16px 72px;background:var(--bg);color:var(--fg);"
"font:15px/1.55 var(--sans);display:flex;flex-direction:column;align-items:center}"
"main{width:100%;max-width:30rem;display:flex;flex-direction:column;gap:14px}"

/* The one Snow White quotation: the thin recessed vent lines. */
"header{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;"
"padding:0 2px 14px}"
"h1{margin:0;font:300 30px/1 var(--serif);letter-spacing:.02em}"
"h1 span{color:var(--dim)}"
"#model{font:300 13px/1.4 var(--serif);letter-spacing:.06em;text-transform:uppercase;"
"color:var(--dim);text-align:right}"
"#vent{height:9px;flex:0 0 42px;align-self:center;"
"background:repeating-linear-gradient(to bottom,var(--line) 0 1px,transparent 1px 3px)}"

"section{background:var(--card);border:1px solid var(--line);border-top-color:var(--edge);"
"border-radius:6px;padding:15px 16px 16px}"
"h2{font:500 10px/1 var(--sans);text-transform:uppercase;letter-spacing:.13em;"
"color:var(--dim);margin:0 0 12px}"
"label{display:block;font-size:12px;color:var(--dim);margin:11px 0 4px}"
"input,select{width:100%;padding:8px 10px;font:inherit;font-size:14px;color:var(--fg);"
"background:var(--bg);border:1px solid var(--line);border-radius:4px;"
"-webkit-appearance:none;appearance:none}"
"input:focus,select:focus{outline:0;border-color:var(--dim)}"
"select{background-image:linear-gradient(45deg,transparent 50%,var(--dim) 50%),"
"linear-gradient(135deg,var(--dim) 50%,transparent 50%);"
"background-position:calc(100% - 17px) 16px,calc(100% - 12px) 16px;"
"background-size:5px 5px,5px 5px;background-repeat:no-repeat;padding-right:32px}"
".row{display:flex;gap:8px;margin-top:14px}"
"button{padding:8px 15px;font:inherit;font-size:13px;font-weight:500;color:var(--card);"
"background:var(--fg);border:1px solid var(--fg);border-radius:4px;cursor:pointer}"
"button.ghost{background:0;color:var(--fg);border-color:var(--line)}"
"button:disabled{opacity:.35;cursor:default}"
"a{color:var(--fg);text-decoration:none;border-bottom:1px solid var(--line)}"

/* Compact meters, not a table of words. */
".meters{display:flex;align-items:center;gap:16px;padding:2px 0 12px;"
"border-bottom:1px solid var(--line);margin-bottom:4px}"
".m{display:flex;flex-direction:column;gap:3px;min-width:0}"
".m em{font-style:normal;font-size:10px;letter-spacing:.1em;text-transform:uppercase;"
"color:var(--dim)}"
".m b{font-weight:400;font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
".bars{display:flex;align-items:flex-end;gap:2px;height:13px}"
".bars i{width:3px;background:var(--line);border-radius:1px}"
".bars i:nth-child(1){height:4px}.bars i:nth-child(2){height:7px}"
".bars i:nth-child(3){height:10px}.bars i:nth-child(4){height:13px}"
".bars i.on{background:var(--fg)}"

/* Status: small tiles rather than a list of sentences. */
".tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(6.6rem,1fr));gap:1px;"
"background:var(--line);border:1px solid var(--line);border-radius:4px;overflow:hidden}"
".t{background:var(--card);padding:9px 10px;min-width:0}"
".t em{display:block;font-style:normal;font-size:10px;letter-spacing:.1em;"
"text-transform:uppercase;color:var(--dim);margin-bottom:2px}"
".t b{display:block;font-weight:400;font-size:13px;white-space:nowrap;overflow:hidden;"
"text-overflow:ellipsis}"
".dot{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--ok);"
"margin-right:6px;vertical-align:middle}"
".dot.stop{background:var(--warn)}"

".note{font-size:12px;color:var(--dim);margin:11px 0 0}"
".hide{display:none!important}"
"#fw{font-size:12px}"

"#over{position:fixed;inset:0;background:var(--bg);display:flex;flex-direction:column;"
"align-items:center;justify-content:center;gap:16px;text-align:center;padding:24px;z-index:9}"
"#spin{width:30px;height:30px;border:2px solid var(--line);border-top-color:var(--fg);"
"border-radius:50%;animation:sp .9s linear infinite}"
"@keyframes sp{to{transform:rotate(360deg)}}"
"#ovtitle{font:300 22px/1 var(--serif);letter-spacing:.02em}"
"#ovtext{margin:0;color:var(--dim);font-size:13px}"
"</style></head><body><main>"

"<header><h1>Air<span>Writer</span></h1><div id=vent></div>"
"<div id=model>ImageWriter II</div></header>"

"<section><h2>Network</h2>"
"<div id=meters class='meters hide'>"
"<div class=m><em>Signal</em><span class=bars id=bars><i></i><i></i><i></i><i></i></span></div>"
"<div class=m><em>Band</em><b id=mband>&mdash;</b></div>"
"<div class=m><em>Security</em><b id=msec>&mdash;</b></div>"
"<div class=m style=flex:1><em>Address</em><b id=mip>&mdash;</b></div>"
"</div>"
"<label for=ssid id=netlabel>Network</label>"
"<select id=netsel class=hide></select>"
"<input id=ssid autocomplete=off placeholder='Network name'>"
"<label for=pw>Password</label>"
"<input id=pw type=password autocomplete=off placeholder='Unchanged'>"
"<div class=row><button id=scan class=ghost type=button>Scan</button>"
"<button id=savewifi type=button disabled>Save &amp; restart</button></div>"
"<p class=note>2.4 GHz only. Open, WPA2 or WPA3.</p></section>"

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
"<a id=cups class=ghost href='http://localhost:631/admin' target=_blank "
"style='padding:8px 15px;font-size:13px;border:1px solid var(--line);border-radius:4px'>"
"Open CUPS</a></div>"
"<p class=note id=uri></p></section>"

"<section><h2>Status</h2>"
"<div class=tiles id=tiles></div>"
"<label for=fw>Firmware image</label><input id=fw type=file accept=.bin>"
"<div class=row><button id=doup class=ghost type=button>Install</button></div>"
"<p class=note id=upnote>Only the ImageWriter II is tested.</p></section>"

"</main>"

"<div id=over class=hide><div id=spin></div>"
"<div id=ovtitle>Restarting</div><p id=ovtext></p>"
"<button id=ovbtn class=ghost type=button>Reload</button></div>"

"<script>"
"const $=i=>document.getElementById(i);"
"let custom=0,first=1,dirty=0,suffix='';"

/* Every printer this bridge knows how to name. The model decides the queue
   name the print sheet shows and the hostname it answers to. */
"const MODELS=[['ImageWriter II','imagewriter'],['ImageWriter','imagewriter'],"
"['ImageWriter LQ','imagewriter-lq']];"

"function fillModels(){const d=$('devsel');d.innerHTML='';"
"for(const[n]of MODELS){const o=document.createElement('option');o.value=n;"
"o.textContent=n;d.append(o);}"
"const o=document.createElement('option');o.value='__other__';o.textContent='Other\\u2026';"
"d.append(o);}"
"fillModels();"

"$('devsel').onchange=()=>{const v=$('devsel').value;"
"if(v=='__other__'){$('queue').classList.remove('hide');"
"$('queuelabel').classList.remove('hide');$('queue').focus();$('model').textContent='Printer';"
"return;}"
"$('queue').classList.add('hide');$('queuelabel').classList.add('hide');"
"$('queue').value=v;$('model').textContent=v;"
"const m=MODELS.find(x=>x[0]==v);if(m&&suffix)$('host').value=m[1]+'-'+suffix;};"

"$('queue').oninput=()=>{$('model').textContent=$('queue').value||'Printer';};"

"function bars(sig){const n={Excellent:4,Good:3,Fair:2,Weak:1}[sig]||0;"
"[...$('bars').children].forEach((b,i)=>b.classList.toggle('on',i<n));}"

"const TILES=[['state','State'],['job','Last job'],['uptime','Uptime'],"
"['heap','Memory'],['version','Firmware']];"

"function draw(s){"
"let h='';for(const[k,label]of TILES){if(!s[k])continue;"
"let v=k=='state'?'<span class=\"dot'+(s.stopped?' stop':'')+'\"></span>'+s[k]:s[k];"
"h+='<div class=t><em>'+label+'</em><b>'+v+'</b></div>';}"
"$('tiles').innerHTML=h;"

"if(s.setup){$('meters').classList.add('hide');}"
"else{$('meters').classList.remove('hide');bars(s.signal);"
"$('mband').textContent=s.channel||'\\u2014';$('msec').textContent=s.security||'\\u2014';"
"$('mip').textContent=s.ip||'\\u2014';}"

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

/* No network means picking one is the only thing to do, so scan on arrival.
   A device already joined shows what it is on and leaves the radio alone. */
"if(first){first=0;if(s.setup)scan();else if(s.ssid)$('ssid').value=s.ssid;}}"

"function poll(){fetch('/api/status').then(r=>r.json()).then(draw).catch(()=>{});}"
"poll();setInterval(poll,4000);"

"function mark(){dirty=1;$('savewifi').disabled=0;}"
"$('ssid').oninput=mark;$('pw').oninput=mark;$('netsel').onchange=()=>{"
"if($('netsel').value=='__other__'){showText(1);}mark();};"
"$('dither').onchange=()=>{$('dither').dataset.touched=1;};"

"function showSelect(){custom=0;$('netsel').classList.remove('hide');"
"$('ssid').classList.add('hide');$('netlabel').setAttribute('for','netsel');}"
"function showText(f){custom=1;$('netsel').classList.add('hide');"
"$('ssid').classList.remove('hide');$('netlabel').setAttribute('for','ssid');"
"if(f){$('ssid').value='';$('ssid').focus();}}"

"async function scan(){const b=$('scan');b.disabled=1;b.textContent='Scanning\\u2026';"
"try{const n=await(await fetch('/api/scan')).json();"
"const seen=[];for(const x of n)if(x&&!seen.includes(x))seen.push(x);"
"const cur=custom?'':$('ssid').value;"
"const d=$('netsel');d.innerHTML='';"
"for(const x of seen){const o=document.createElement('option');o.value=x;"
"o.textContent=x;d.append(o);}"
"if(!seen.length){const o=document.createElement('option');o.textContent='No networks found';"
"o.disabled=1;d.append(o);}"
"const o=document.createElement('option');o.value='__other__';o.textContent='Other\\u2026';"
"d.append(o);"
/* Scan always returns to the dropdown. That is what makes it the way back
   from a name typed by hand. */
"showSelect();if(cur&&seen.includes(cur))d.value=cur;"
"}catch(e){showText(0);}"
"b.disabled=0;b.textContent='Scan';}"
"$('scan').onclick=scan;"

"function overlay(title,text,secs){$('ovtitle').textContent=title;"
"$('over').classList.remove('hide');let left=secs;"
"const tick=()=>{$('ovtext').textContent=text+' \\u00b7 '+left+'s';"
"if(left--<=0){clearInterval(h);location.reload();}};"
"tick();const h=setInterval(tick,1000);}"
"$('ovbtn').onclick=()=>location.reload();"

"$('savewifi').onclick=async()=>{"
"const name=custom?$('ssid').value:$('netsel').value;"
"if(!name||name=='__other__'){$('ssid').focus();return;}"
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
