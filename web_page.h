// Settings page served at http://192.168.4.1 while "WiFi setup" is active.
// Talks to /api/config, /api/status, /api/settings, /api/reset_pairing, /api/factory_reset,
// /api/logs and /log?f=<file> (SD card ride logs).
#pragma once
#include <Arduino.h>

static const char WEB_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP-TPMS</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:#111;color:#eee}
header{background:#1e3a5f;padding:12px 16px;font-weight:600;font-size:1.1em}
header small{color:#9cf;font-weight:400}
main{padding:12px 16px;max-width:520px;margin:auto}
.cards{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.card{background:#1c1c1c;border-radius:10px;padding:10px;border-left:6px solid #666;min-height:84px}
.card .p{font-size:2em;font-weight:700;margin:4px 0}
.card small{color:#aaa}
#logs a{display:flex;justify-content:space-between;color:#9cf;padding:7px 0;border-bottom:1px solid #222;text-decoration:none}
fieldset{border:1px solid #333;border-radius:10px;margin:14px 0;padding:6px 12px}
legend{color:#9cf;padding:0 4px}
label{display:flex;justify-content:space-between;align-items:center;margin:8px 0;gap:10px}
select,input{background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:7px;font-size:1em;width:9.5em}
button{width:100%;padding:12px;font-size:1.05em;border:0;border-radius:8px;margin:6px 0;background:#2e7d32;color:#fff}
button.warn{background:#7a1f1f}
#msg{text-align:center;min-height:1.3em;margin:4px 0}
</style></head><body>
<header>ESP-TPMS <small id="ver"></small></header>
<main>
<div class="cards" id="cards"></div>
<form id="f">
<fieldset><legend>Units</legend>
<label>Pressure <select name="psi"><option value="0">bar</option><option value="1">psi</option></select></label>
<label>Temperature <select name="degf"><option value="0">°C</option><option value="1">°F</option></select></label>
</fieldset>
<fieldset><legend>Limits</legend><div id="limits"></div></fieldset>
<fieldset><legend>Alarms</legend>
<label>Max temp <select name="tmax"></select></label>
<label>No signal after <select name="stale"></select></label>
<label>Buzzer <select name="buzz"></select></label>
</fieldset>
<fieldset><legend>Display</legend>
<label>Layout <select name="layout"></select></label>
<label>Colors <select name="colors"></select></label>
<label>Rotate <select name="rot"><option value="0">0°</option><option value="1">180°</option></select></label>
<label>Brightness <select name="bright"></select></label>
<label>Dim after <select name="dim"></select></label>
<label>Dim level <select name="diml"></select></label>
<label>Screen off <select name="off"></select></label>
</fieldset>
<fieldset><legend>Power</legend>
<label>BLE scan <select name="scan"></select></label>
</fieldset>
<button type="submit">Save settings</button>
<div id="msg"></div>
</form>
<fieldset><legend>Ride logs (SD card)</legend><div id="logs"><small>loading…</small></div></fieldset>
<fieldset><legend>Maintenance</legend>
<button class="warn" id="rp" type="button">Reset pairing</button>
<button class="warn" id="fr" type="button">Factory reset</button>
</fieldset>
</main>
<script>
let C=null,unitPsi=false;
const $=s=>document.querySelector(s);
const secs=s=>s==0?'Never':s<60?s+' s':(s/60)+' min';
const COL={'OK':'#2ea043','LOW':'#d73a49','HIGH':'#3b82f6','DROPPING!':'#d73a49','HOT!':'#d946ef','NO SIGNAL':'#f59e0b'};
function msg(t,ok){const m=$('#msg');m.textContent=t;m.style.color=ok?'#8f8':'#f88';}
function fill(n,vals,fmt,idx){const e=$('[name='+n+']');e.innerHTML=vals.map((v,i)=>'<option value="'+i+'">'+fmt(v)+'</option>').join('');e.value=idx;}
function degSel(){return $('[name=degf]').value=='1';}
function fillTmax(idx){const cur=(idx===undefined)?$('[name=tmax]').value:idx;
 fill('tmax',C.tmaxOpts,v=>v==0?'Off':(degSel()?Math.round(v*1.8+32)+' °F':v+' °C'),cur);}
function renderLimits(){const k=unitPsi?14.5038:1,st=unitPsi?1:0.05,d=unitPsi?0:2;
 $('#limits').innerHTML=C.positions.map((p,i)=>
  '<label>'+p.name+' MIN <input type="number" name="min'+i+'" step="'+st+'" value="'+(p.min*k).toFixed(d)+'"></label>'+
  '<label>'+p.name+' MAX <input type="number" name="max'+i+'" step="'+st+'" value="'+(p.max*k).toFixed(d)+'"></label>').join('');}
function readLimits(){const k=unitPsi?14.5038:1;
 C.positions.forEach((p,i)=>{p.min=parseFloat($('[name=min'+i+']').value)/k;p.max=parseFloat($('[name=max'+i+']').value)/k;});}
async function load(){
 C=await (await fetch('/api/config')).json();
 $('#ver').textContent='v'+C.version;
 $('[name=psi]').value=C.psi?1:0; unitPsi=C.psi;
 $('[name=degf]').value=C.degf?1:0; $('[name=rot]').value=C.rot?1:0;
 renderLimits(); fillTmax(C.tmax);
 fill('stale',C.staleOpts,v=>v+' min',C.stale);
 fill('buzz',C.buzzOpts,v=>v,C.buzz);
 fill('layout',C.layoutOpts,v=>v,C.layout);
 fill('colors',C.colorOpts,v=>v,C.colors);
 fill('bright',C.brightOpts,v=>v+' %',C.bright);
 fill('dim',C.dimOpts,secs,C.dim);
 fill('diml',C.dimlOpts,v=>v+' %',C.diml);
 fill('off',C.offOpts,secs,C.off);
 fill('scan',C.scanOpts,v=>v+' %',C.scan);
}
$('[name=psi]').onchange=()=>{readLimits();unitPsi=$('[name=psi]').value=='1';renderLimits();};
$('[name=degf]').onchange=()=>fillTmax();
$('#f').onsubmit=async e=>{e.preventDefault();readLimits();
 const fd=new URLSearchParams(new FormData($('#f')));
 C.positions.forEach((p,i)=>{fd.set('min'+i,p.min.toFixed(3));fd.set('max'+i,p.max.toFixed(3));});
 try{const r=await fetch('/api/settings',{method:'POST',body:fd});const t=await r.text();
  msg(r.ok?'Saved ✓':'Error: '+t,r.ok);}catch(x){msg('No connection',false);}};
$('#rp').onclick=async()=>{if(!confirm('Forget both sensors?'))return;
 await fetch('/api/reset_pairing',{method:'POST'});msg('Pairing reset – wake the Front sensor and click the button',true);};
$('#fr').onclick=async()=>{if(!confirm('Erase ALL settings and pairing? The device restarts.'))return;
 await fetch('/api/factory_reset',{method:'POST'});msg('Restarting… start WiFi setup again from the menu',true);};
async function status(){try{const s=await (await fetch('/api/status')).json();
 $('#cards').innerHTML=s.positions.map(p=>{
  if(!p.paired)return '<div class="card"><b>'+p.name+'</b><br><small>not paired</small></div>';
  if(!p.has)return '<div class="card"><b>'+p.name+'</b><br><small>waiting for data</small></div>';
  const v=s.psi?(p.bar*14.5038).toFixed(1):p.bar.toFixed(2);
  const t=s.degf?Math.round(p.temp*1.8+32)+'°F':Math.round(p.temp)+'°C';
  const b=p.volts>=0?p.volts.toFixed(1)+' V':(p.batt>=0?p.batt+' %':'');
  return '<div class="card" style="border-color:'+(COL[p.state]||'#666')+'"><b>'+p.name+'</b> <small>'+p.state+'</small>'+
   '<div class="p">'+v+' <small>'+(s.psi?'psi':'bar')+'</small></div><small>'+t+' · '+b+' · '+p.age+' s ago</small></div>';}).join('');
 }catch(x){}}
async function logs(){try{const l=await (await fetch('/api/logs')).json();
 if(!l.sd){$('#logs').innerHTML='<small>No SD card</small>';return;}
 const kb=s=>s<1024?s+' B':(s/1024).toFixed(0)+' kB';
 $('#logs').innerHTML=l.files.length?l.files.sort((a,b)=>b.n.localeCompare(a.n)).map(f=>
  '<a href="/log?f='+f.n+'" download="tpms-'+f.n+'"><span>'+f.n+(parseInt(f.n.slice(4))==l.ride?' (current)':'')+'</span><small>'+kb(f.s)+'</small></a>').join(''):'<small>No logs yet</small>';
 }catch(x){$('#logs').innerHTML='<small>?</small>';}}
load().then(status).then(logs);setInterval(status,2000);
</script></body></html>)rawliteral";
