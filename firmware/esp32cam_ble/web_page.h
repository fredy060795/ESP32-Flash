#pragma once

// Seite, die die Kamera selbst unter http://192.168.4.1 ausliefert
// (funktioniert ohne Internet, direkt im Kamera-Hotspot).
static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM</title>
<style>
:root{color-scheme:light dark;--bg:#f4f5f7;--fg:#16181d;--card:#fff;--mut:#5d6470;--acc:#0b6bcb;--bd:#d9dce1}
@media (prefers-color-scheme:dark){:root{--bg:#111317;--fg:#e8eaee;--card:#1b1e24;--mut:#9aa1ad;--acc:#4ea1ff;--bd:#2c3038}}
*{box-sizing:border-box}body{margin:0;font:16px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:900px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:0 0 12px}
.view{background:#000;border-radius:10px;overflow:hidden;aspect-ratio:4/3;display:flex;align-items:center;justify-content:center}
.view img{width:100%;height:100%;object-fit:contain}
.row{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px;align-items:center}
button,select{font:inherit;padding:10px 14px;border-radius:8px;border:1px solid var(--bd);background:var(--card);color:var(--fg)}
button.pri{background:var(--acc);border-color:var(--acc);color:#fff}
label{color:var(--mut);font-size:14px}
input[type=range]{flex:1;min-width:140px}
#info{color:var(--mut);font-size:14px;margin-top:12px}
</style></head><body><main>
<h1 id="title">ESP32-CAM</h1>
<div class="view"><img id="cam" alt="Live-Bild"></div>
<div class="row">
<button class="pri" id="toggle">Stopp</button>
<button id="photo">Foto speichern</button>
<select id="fs">
<option value="5">320x240</option><option value="8">640x480</option><option value="9">800x600</option>
<option value="10">1024x768</option><option value="11">1280x720</option><option value="13">1600x1200</option>
</select>
</div>
<div class="row"><label for="flash">Licht</label><input type="range" id="flash" min="0" max="255" value="0"></div>
<div id="info"></div>
</main><script>
const host=location.hostname,streamUrl='http://'+host+':81/stream';
const img=document.getElementById('cam'),tg=document.getElementById('toggle');
let on=true;img.src=streamUrl;
tg.onclick=()=>{on=!on;img.src=on?streamUrl:'';tg.textContent=on?'Stopp':'Start'};
document.getElementById('photo').onclick=()=>{const a=document.createElement('a');a.href='/capture?t='+Date.now();a.download='foto.jpg';a.click()};
const ctl=(v,x)=>fetch('/control?var='+v+'&val='+x);
document.getElementById('fs').onchange=e=>ctl('framesize',e.target.value);
document.getElementById('flash').onchange=e=>ctl('flash',e.target.value);
fetch('/status').then(r=>r.json()).then(s=>{
document.getElementById('title').textContent=s.name;
document.getElementById('fs').value=s.framesize;
document.getElementById('flash').value=s.flash;
document.getElementById('info').textContent='Hotspot: '+s.ap_ssid+(s.sta_connected?' · Heim-WLAN: '+s.sta_ssid+' ('+s.sta_ip+')':'')+' · Firmware '+s.fw;
});
</script></body></html>)HTML";
