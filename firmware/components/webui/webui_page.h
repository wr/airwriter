// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole UI: one file, no external requests. A printer's config page has to
// work on a phone that has just joined the printer's own access point and has
// no internet at all, so nothing here may reference a CDN.
//
// Wi-Fi comes first because on a new device it is the only thing that matters;
// status sits at the bottom, where it is reference rather than an instruction.
#pragma once

static const char WEBUI_HTML[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>AirWriter</title><style>"
":root{--bg:#faf9f7;--fg:#1a1917;--dim:#6b6862;--line:#e0ddd6;--card:#fff;--ok:#2d7a4f;--warn:#a8601b}"
"@media(prefers-color-scheme:dark){:root{--bg:#16150f;--fg:#eae7e0;--dim:#9a968c;--line:#2e2c26;--card:#1e1d16}}"
"*{box-sizing:border-box}"
"body{margin:0;padding:24px 16px 64px;background:var(--bg);color:var(--fg);"
"font:16px/1.5 ui-sans-serif,-apple-system,system-ui,sans-serif;display:flex;"
"flex-direction:column;align-items:center;gap:20px}"
"main{width:100%;max-width:33rem;display:flex;flex-direction:column;gap:20px}"
"h1{font-size:1.3rem;margin:0;letter-spacing:-.01em}"
"h1 small{display:block;font-weight:400;font-size:.8rem;color:var(--dim);letter-spacing:0}"
"section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:18px}"
"h2{font-size:.72rem;text-transform:uppercase;letter-spacing:.09em;color:var(--dim);"
"margin:0 0 14px;font-weight:600}"
"label{display:block;font-size:.83rem;color:var(--dim);margin:12px 0 5px}"
"input,select{width:100%;padding:9px 11px;font:inherit;font-size:.95rem;color:var(--fg);"
"background:var(--bg);border:1px solid var(--line);border-radius:7px;"
"-webkit-appearance:none;appearance:none}"
"select{background-image:linear-gradient(45deg,transparent 50%,var(--dim) 50%),"
"linear-gradient(135deg,var(--dim) 50%,transparent 50%);"
"background-position:calc(100% - 18px) 18px,calc(100% - 13px) 18px;"
"background-size:5px 5px,5px 5px;background-repeat:no-repeat;padding-right:34px}"
"button{margin-top:16px;padding:10px 18px;font:inherit;font-weight:550;color:var(--bg);"
"background:var(--fg);border:0;border-radius:7px;cursor:pointer}"
"button:disabled{opacity:.45;cursor:default}"
"button.ghost{background:0;color:var(--fg);border:1px solid var(--line)}"
"dl{display:grid;grid-template-columns:auto 1fr;gap:7px 20px;margin:0;font-size:.9rem}"
"dt{color:var(--dim)}dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}"
".dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--ok);"
"margin-right:7px;vertical-align:middle}"
".dot.stop{background:var(--warn)}"
".note{font-size:.8rem;color:var(--dim);margin:12px 0 0}"
".note b{font-weight:600;color:var(--fg)}"
".hide{display:none!important}"
"#over{position:fixed;inset:0;background:var(--bg);display:flex;flex-direction:column;"
"align-items:center;justify-content:center;gap:18px;text-align:center;padding:24px;z-index:9}"
"#spin{width:34px;height:34px;border:3px solid var(--line);border-top-color:var(--fg);"
"border-radius:50%;animation:sp 1s linear infinite}"
"@keyframes sp{to{transform:rotate(360deg)}}"
"#over p{margin:0;max-width:22rem;color:var(--dim);font-size:.9rem}"
"#over strong{font-size:1.05rem;color:var(--fg)}"
"</style></head><body><main>"

"<h1>AirWriter<small id=sub>Apple ImageWriter II</small></h1>"

"<section><h2>Wi-Fi</h2>"
"<div id=joined class=hide><dl id=wd></dl></div>"
"<label for=ssid id=netlabel>Network</label>"
"<select id=netsel class=hide></select>"
"<input id=ssid autocomplete=off placeholder='Network name'>"
"<label for=pw>Password</label>"
"<input id=pw type=password autocomplete=off placeholder='Leave blank to keep'>"
"<button id=scan class=ghost type=button>Scan</button> "
"<button id=savewifi type=button>Save &amp; restart</button>"
"<p class=note><b>2.4 GHz only.</b> This printer has no 5 GHz radio, so a "
"network broadcasting only on 5 GHz will not appear. Open, WPA2 and WPA3 "
"personal networks all work. Enterprise networks that ask for a username "
"(802.1X) do not. For a hidden network, choose <b>Other\\u2026</b> and type "
"the name exactly.</p></section>"

"<section><h2>Printer</h2>"
"<label for=queue>Name in the print sheet</label>"
"<input id=queue placeholder=AirWriter>"
"<label for=host>Hostname</label>"
"<input id=host placeholder=imagewriter-a7fc>"
"<label for=dither>Dithering</label>"
"<select id=dither>"
"<option value=atkinson>Atkinson</option>"
"<option value=floyd>Floyd-Steinberg</option>"
"<option value=bayer>Ordered (Bayer 8&times;8)</option>"
"<option value=threshold>Threshold</option>"
"</select>"
"<button id=savegen type=button>Save</button>"
"<p class=note>Atkinson suits photographs. Ordered dithering usually reads "
"better for small text, which is most of what gets printed.</p></section>"

"<section><h2>Firmware</h2>"
"<label for=fw>Upload a new .bin</label><input id=fw type=file accept=.bin>"
"<button id=doup type=button>Install</button>"
"<p class=note id=upnote>The printer restarts once the image is written.</p>"
"</section>"

"<section><h2>Status</h2><dl id=stat></dl></section>"

"</main>"

"<div id=over class=hide>"
"<div id=spin></div>"
"<strong id=ovtitle>Restarting</strong>"
"<p id=ovtext></p>"
"<button id=ovbtn class=ghost type=button>Reload now</button>"
"</div>"

"<script>"
"const $=i=>document.getElementById(i);"
"let custom=0,firstLoad=1;"

/* Status panel, at the bottom. */
"const rows={state:'State',job:'Last job',queue:'Queue',host:'Address',"
"ip:'IP',uptime:'Uptime',version:'Firmware',heap:'Free memory'};"
"const wrows={ssid:'Network',signal:'Signal',channel:'Band',security:'Security',ip:'IP'};"

"function table(el,defs,s){let h='';for(const k in defs){"
"if(!s[k]&&s[k]!==0)continue;let v=s[k];"
"if(k=='state')v='<span class=\"dot'+(s.stopped?' stop':'')+'\"></span>'+v;"
"h+='<dt>'+defs[k]+'</dt><dd>'+v+'</dd>';}el.innerHTML=h;}"

"function draw(s){"
"$('sub').textContent=s.model||'Apple ImageWriter II';"
"table($('stat'),rows,s);"
"if(s.setup){$('joined').classList.add('hide');}"
"else{$('joined').classList.remove('hide');table($('wd'),wrows,s);}"
"if(s.queue&&!$('queue').value)$('queue').placeholder=s.queue;"
"if(s.host&&!$('host').value)$('host').placeholder=s.host.replace('.local','');"
"if(s.dither&&!$('dither').dataset.touched)$('dither').value=s.dither;"

/* A device with no network auto-scans, because picking one is the only thing
   there is to do. A device already on a network shows what it is joined to in
   a plain field and leaves the radio alone until Scan is pressed. */
"if(firstLoad){firstLoad=0;"
"if(s.setup){scan();}else if(s.ssid){$('ssid').value=s.ssid;}}}"

"function poll(){fetch('/api/status').then(r=>r.json()).then(draw).catch(()=>{});}"
"poll();setInterval(poll,4000);"

/* Dropdown when we have a list, text field when the user asks for one. */
"function showSelect(){custom=0;$('netsel').classList.remove('hide');"
"$('ssid').classList.add('hide');$('netlabel').setAttribute('for','netsel');}"
"function showText(focus){custom=1;$('netsel').classList.add('hide');"
"$('ssid').classList.remove('hide');$('netlabel').setAttribute('for','ssid');"
"if(focus){$('ssid').value='';$('ssid').focus();}}"

"$('netsel').onchange=()=>{if($('netsel').value=='__other__')showText(1);};"

"async function scan(){const b=$('scan');b.disabled=1;b.textContent='Scanning\\u2026';"
"try{const n=await(await fetch('/api/scan')).json();"
"const seen=[];for(const x of n)if(x&&!seen.includes(x))seen.push(x);"
"const cur=$('ssid').value;"
"$('netsel').innerHTML=seen.map(x=>'<option></option>').join('');"
"seen.forEach((x,i)=>{$('netsel').children[i].value=x;"
"$('netsel').children[i].textContent=x;});"
"const o=document.createElement('option');o.value='__other__';"
"o.textContent='Other\\u2026';$('netsel').append(o);"
"if(!seen.length){$('netsel').children[0].textContent='No networks found';}"
/* Scan always returns to the dropdown, even if the user had switched to
   typing a name -- that is what makes Scan a way back. */
"showSelect();"
"if(cur&&seen.includes(cur))$('netsel').value=cur;"
"}catch(e){showText(0);$('netsel').classList.add('hide');}"
"b.disabled=0;b.textContent='Scan';}"
"$('scan').onclick=scan;"

"$('dither').onchange=()=>{$('dither').dataset.touched=1;};"

/* Full-screen while the device restarts. In the captive portal the sheet
   closes itself once the printer leaves its own network, so this mostly
   matters on a desktop browser, where nothing else would ever say what
   happened. */
"function overlay(title,text,secs){"
"$('ovtitle').textContent=title;$('over').classList.remove('hide');"
"let left=secs;const tick=()=>{"
"$('ovtext').textContent=text+' Reloading in '+left+'s\\u2026';"
"if(left--<=0){clearInterval(h);location.reload();}};"
"tick();const h=setInterval(tick,1000);}"
"$('ovbtn').onclick=()=>location.reload();"

"async function post(u,b,btn,done){btn.disabled=1;"
"try{await fetch(u,{method:'POST',body:JSON.stringify(b)});btn.textContent=done;}"
"catch(e){btn.textContent='Failed'}setTimeout(()=>{btn.disabled=0},3000);}"

"$('savewifi').onclick=async()=>{"
"const name=custom?$('ssid').value:$('netsel').value;"
"if(!name||name=='__other__'){$('ssid').focus();return;}"
"$('savewifi').disabled=1;"
"try{await fetch('/api/wifi',{method:'POST',"
"body:JSON.stringify({ssid:name,pass:$('pw').value})});}catch(e){}"
"overlay('Restarting','The printer is joining \\u201c'+name+'\\u201d. If it "
"succeeds it will be on that network, not this page.',25);};"

"$('savegen').onclick=()=>post('/api/general',{queue:$('queue').value,"
"host:$('host').value,dither:$('dither').value},$('savegen'),'Saved');"

"$('doup').onclick=async()=>{const f=$('fw').files[0];if(!f)return;"
"$('doup').disabled=1;$('upnote').textContent='Uploading '+(f.size>>10)+' KB\\u2026';"
"try{const r=await fetch('/update',{method:'POST',body:f});"
"if(r.ok)overlay('Installing','The new firmware is written and the printer is "
"restarting.',30);else $('upnote').textContent='Rejected: '+await r.text();}"
"catch(e){$('upnote').textContent='Upload failed.'}$('doup').disabled=0;};"
"</script></body></html>";
