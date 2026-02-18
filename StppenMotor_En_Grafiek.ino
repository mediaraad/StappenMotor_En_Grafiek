#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

const char* ssid_home = WIFI_TP_SSID;
const char* pass_home = WIFI_TP_PASSWORD;
const char* ap_ssid   = "Motor-Control-Pro";
const char* ap_pass   = "12345678"; 
const char* mdns_name = "motor";

WebServer server(80);

const int motorPins[] = {14, 12, 13, 15}; 
int stepIndex = 0;
unsigned long lastStepMicros = 0;
const float MAX_STEPS_PER_SEC = 800.0;
const int steps[8][4] = {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,1,1},{0,0,0,1},{1,0,0,1}};

struct Keyframe { unsigned long time; float value; };
Keyframe envelope[50];
int envelopeSize = 2;
unsigned long startTime = 0;
unsigned long loopDuration = 8000;

const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stepper Pro v18.9.1 - Refresh Fix</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:20px;}
canvas{background:#1e1e1e;border:2px solid #444;cursor:pointer;touch-action:none;display:block;margin:5px auto;border-radius:8px;}
.controls{background:#2a2a2a;padding:15px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;gap:10px;margin:0 auto 10px auto;border:1px solid #444;flex-wrap:wrap;max-width:900px;box-sizing:border-box;}
input,button,select{height:42px; padding:0 10px; border-radius:4px;border:none;background:#444;color:white;outline:none;font-size:14px;}
button{background:#00bfff;cursor:pointer;font-weight:bold;transition:0.2s;}
button:hover{background:#009cd1;}
.btn-delete{background:#ff4444 !important;}
.btn-alt{background:#666 !important;}
.btn-new{background:#28a745 !important;}
.unsaved-container{height: 25px; margin-bottom:5px;}
.unsaved-text{color:#ff9900; font-weight:bold; font-size: 13px; display:none;}
#currentFileDisplay{color:#00ff00; font-weight:bold; margin-left:10px;}
textarea{width:800px; height:150px; background:#1e1e1e; color:#00ff00; border:1px solid #444; font-family:monospace; margin-top:10px; padding:10px; tab-size: 2;}
#customModal{display:none; position:fixed; z-index:1000; left:0; top:0; width:100%; height:100%; background:rgba(0,0,0,0.8); backdrop-filter:blur(4px);}
.modal-content{background:#2a2a2a; margin:15% auto; padding:25px; border:1px solid #00bfff; width:320px; border-radius:12px;}
.modal-btns{display:flex; justify-content:center; gap:15px; margin-top:20px;}
</style>
</head>
<body>
    <h2>Motor Control Pro <span id="currentFileDisplay"></span></h2>
    <div class="controls">
        <button onclick="createNew()" class="btn-new">➕ Nieuw</button>
        <span style="border-left:1px solid #444;height:30px;margin:0 5px;"></span>
        <select id="presetSelect" onchange="autoLoadPreset()"></select>
        <button onclick="deletePreset()" class="btn-delete">🗑️</button>
        <span style="border-left:1px solid #444;height:30px;margin:0 5px;"></span>
        <button onclick="saveCurrent()">Opslaan</button>
        <button onclick="saveAs()" class="btn-alt">Opslaan als...</button>
        <span style="border-left:1px solid #444;height:30px;margin:0 5px;"></span>
        <label>Duur (s):</label>
        <input type="number" id="totalTime" value="8" min="1" style="width:50px;" onchange="updateDuration()">
        <span style="border-left:1px solid #444;height:30px;margin:0 5px;"></span>
        <button onclick="downloadConfig()" class="btn-alt" title="Download naar PC">💾</button>
        <button onclick="document.getElementById('fileInput').click()" class="btn-alt" title="Upload naar ESP">📂</button>
        <input type="file" id="fileInput" style="display:none" onchange="handleFileUpload(event)" accept=".json">
    </div>
    <div class="unsaved-container">
        <span id="unsavedWarning" class="unsaved-text">⚠️ WIJZIGINGEN NOG NIET OPGESLAGEN</span>
    </div>
    <canvas id="envelopeCanvas" width="800" height="400"></canvas>
    <textarea id="jsonEditor" spellcheck="false" oninput="applyJson()"></textarea>

    <div id="customModal">
        <div class="modal-content"><div id="modalText"></div><div class="modal-btns">
            <button id="modalConfirm" style="background:#00bfff;">Ja</button>
            <button onclick="closeModal()" class="btn-alt">Annuleer</button>
        </div></div>
    </div>

<script>
const canvas=document.getElementById("envelopeCanvas"), ctx=canvas.getContext("2d"), editor=document.getElementById("jsonEditor");
let keyframes=[{time:0,value:0},{time:8000,value:0}], selected=null, playStart=Date.now(), isUnsaved=false, currentOpenFile="";
let isDraggingPlayhead = false, manualTime = 0;

let modalResolve;
function openModal(text){ document.getElementById("modalText").innerText=text; document.getElementById("customModal").style.display="block"; return new Promise(r=>modalResolve=r); }
function closeModal(){ document.getElementById("customModal").style.display="none"; if(modalResolve) modalResolve(false); }
document.getElementById("modalConfirm").onclick=()=>{ document.getElementById("customModal").style.display="none"; if(modalResolve) modalResolve(true); };

const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function markUnsaved(){ isUnsaved=true; document.getElementById("unsavedWarning").style.display="inline-block"; }
function markSaved(name){ isUnsaved=false; document.getElementById("unsavedWarning").style.display="none"; if(name!==undefined){ currentOpenFile=name; document.getElementById("currentFileDisplay").innerText=name?" - "+name:""; }}

function sync(){ 
    editor.value=JSON.stringify(keyframes,null,2);
    document.getElementById("totalTime").value=Math.round(keyframes[keyframes.length-1].time/1000);
    return fetch('/set_live',{method:'POST',body:JSON.stringify(keyframes)}); 
}

canvas.ondblclick = (e) => {
    const rect = canvas.getBoundingClientRect();
    const index = keyframes.findIndex((p, i) => i !== 0 && i !== keyframes.length - 1 && Math.hypot(toX(p.time) - (e.clientX - rect.left), toY(p.value) - (e.clientY - rect.top)) < 20);
    if (index !== -1) { keyframes.splice(index, 1); markUnsaved(); sync(); }
};

async function handleFileUpload(e){
    const file=e.target.files[0]; if(!file) return;
    const reader=new FileReader();
    reader.onload=async (ev)=>{
        try{
            const data=JSON.parse(ev.target.result);
            let name=file.name.replace(".json","").replace(/[^a-z0-9_-]/gi,'_');
            const list = await fetch('/list').then(r=>r.json());
            if(list.includes(name) && !await openModal("Bestand '"+name+"' bestaat al. Overschrijven?")) return;
            await fetch('/save?name='+name,{method:'POST',body:JSON.stringify(data)});
            keyframes=data; markSaved(name); await updatePresetList(name); await sync();
        }catch(err){alert("Fout");}
    };
    reader.readAsText(file); e.target.value="";
}

function draw(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    ctx.strokeStyle="#333"; ctx.lineWidth=1; ctx.beginPath(); ctx.moveTo(0,canvas.height/2); ctx.lineTo(canvas.width,canvas.height/2); ctx.stroke();
    ctx.strokeStyle="#00bfff"; ctx.lineWidth=3; ctx.beginPath();
    keyframes.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value)));
    ctx.stroke();
    ctx.fillStyle="#ff9900"; keyframes.forEach(p=>{ctx.beginPath();ctx.arc(toX(p.time),toY(p.value),6,0,7);ctx.fill();});
    let dur=keyframes[keyframes.length-1].time;
    let elapsed = isDraggingPlayhead ? manualTime : (Date.now()-playStart)%dur;
    ctx.strokeStyle="red"; ctx.lineWidth=3; ctx.beginPath(); ctx.moveTo(toX(elapsed),0); ctx.lineTo(toX(elapsed),canvas.height); ctx.stroke();
    requestAnimationFrame(draw);
}

canvas.onmousedown=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left, y=e.clientY-rect.top;
    let dur=keyframes[keyframes.length-1].time;
    let currentX = toX((Date.now()-playStart)%dur);
    if(Math.abs(x - currentX) < 30) { isDraggingPlayhead = true; manualTime = (Date.now()-playStart)%dur; return; }
    selected=keyframes.find(p=>Math.hypot(toX(p.time)-x,toY(p.value)-y)<15);
    if(!selected){ keyframes.push({time:fromX(x),value:fromY(y)}); keyframes.sort((a,b)=>a.time-b.time); markUnsaved(); sync(); }
};

window.onmousemove=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left;
    if(isDraggingPlayhead) { manualTime = Math.max(0, Math.min(keyframes[keyframes.length-1].time, fromX(x))); return; }
    if(!selected) return;
    if(selected!==keyframes[0] && selected!==keyframes[keyframes.length-1]) selected.time=Math.max(1, fromX(x));
    selected.value=Math.max(-100,Math.min(100,fromY(e.clientY-rect.top)));
    keyframes.sort((a,b)=>a.time-b.time); markUnsaved(); sync();
};

window.onmouseup=()=>{
    if(isDraggingPlayhead) { playStart = Date.now() - manualTime; fetch('/reset_clock_to?t=' + Math.round(manualTime)); }
    selected=null; isDraggingPlayhead = false;
};

async function autoLoadPreset(targetName){
    const name = targetName || document.getElementById("presetSelect").value;
    if(!name) return;
    if(isUnsaved && !targetName && !await openModal("Wijzigingen gaan verloren. Doorgaan?")){ document.getElementById("presetSelect").value = currentOpenFile; return; }
    const res = await fetch('/load?name='+name); keyframes = await res.json();
    markSaved(name); await sync(); playStart=Date.now(); fetch('/reset_clock_to?t=0');
}

async function updatePresetList(targetName){
    const list = await fetch('/list').then(r=>r.json());
    const sel = document.getElementById("presetSelect"); sel.innerHTML = "";
    list.forEach(f=>{ let o=document.createElement("option"); o.value=o.textContent=f; sel.appendChild(o); });
    if(targetName) sel.value = targetName; else if(currentOpenFile) sel.value = currentOpenFile;
}

async function deletePreset(){
    const name = document.getElementById("presetSelect").value;
    if(!name || !await openModal("Verwijder '"+name+"' definitief?")) return;
    await fetch('/delete?name='+name, {method:'DELETE'});
    const list = await fetch('/list').then(r=>r.json());
    if(list.length > 0) { await updatePresetList(list[0]); await autoLoadPreset(list[0]); } else { createNew(); await updatePresetList(); }
}

function createNew(){ keyframes=[{time:0,value:0},{time:8000,value:0}]; markSaved(""); sync(); fetch('/reset_clock_to?t=0'); }
function performSave(n){ fetch('/save?name='+n,{method:'POST',body:JSON.stringify(keyframes)}).then(()=>{markSaved(n);updatePresetList(n);}); }
function saveCurrent(){ if(!currentOpenFile)saveAs(); else if(confirm("Overschrijven?")) performSave(currentOpenFile); }
function saveAs(){ let n=prompt("Naam:",currentOpenFile||"config"); if(n) performSave(n.trim().replace(/[^a-z0-9_-]/gi,'_')); }
function downloadConfig(){ const a=document.createElement('a'); a.href="data:text/json;charset=utf-8,"+encodeURIComponent(JSON.stringify(keyframes,null,2)); a.download=(currentOpenFile||"config")+".json"; a.click(); }
function updateDuration(){ keyframes[keyframes.length-1].time=document.getElementById("totalTime").value*1000; markUnsaved(); sync(); }
function applyJson(){ try{keyframes=JSON.parse(editor.value);markUnsaved();sync();}catch(e){} }

async function init(){
    const res = await fetch('/get_active');
    keyframes = await res.json();
    const n = await fetch('/get_active_name').then(r=>r.text());
    markSaved(n==="Geen"?"":n);
    await updatePresetList();
    sync(); // FIX: Vul editor direct bij opstarten
    draw();
}
init();
</script>
</body>
</html>
)rawliteral";

// --- C++ BACKEND ---
void loadFromJSON(String json) {
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, json)) return;
  envelopeSize = doc.size();
  for (int i = 0; i < envelopeSize; i++) {
    envelope[i].time = doc[i]["time"];
    envelope[i].value = (float)doc[i]["value"];
  }
  loopDuration = (envelopeSize > 1) ? envelope[envelopeSize - 1].time : 8000;
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(motorPins[i], OUTPUT);
  LittleFS.begin(true);
  if (LittleFS.exists("/active.json")) loadFromJSON(LittleFS.open("/active.json", "r").readString());
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(ap_ssid, ap_pass); WiFi.begin(ssid_home, pass_home); MDNS.begin(mdns_name);
  server.on("/", [](){ server.send(200, "text/html", htmlPage); });
  server.on("/get_time", [](){ server.send(200, "text/plain", String((millis() - startTime) % loopDuration)); });
  server.on("/set_live", HTTP_POST, [](){
    if(server.hasArg("plain")) { loadFromJSON(server.arg("plain")); File f = LittleFS.open("/active.json", "w"); f.print(server.arg("plain")); f.close(); }
    server.send(200);
  });
  server.on("/save", HTTP_POST, [](){
    String name = server.arg("name"); String data = server.arg("plain");
    File f1 = LittleFS.open("/" + name + ".json", "w"); f1.print(data); f1.close();
    File f2 = LittleFS.open("/active.json", "w"); f2.print(data); f2.close();
    File f3 = LittleFS.open("/active_name.txt", "w"); f3.print(name); f3.close();
    server.send(200);
  });
  server.on("/load", HTTP_GET, [](){
    String name = server.arg("name"); File f = LittleFS.open("/" + name + ".json", "r");
    if(f){ server.send(200, "application/json", f.readString()); f.close(); } else server.send(404);
  });
  server.on("/delete", HTTP_DELETE, [](){ String name = server.arg("name"); if(LittleFS.exists("/"+name+".json")) LittleFS.remove("/"+name+".json"); server.send(200); });
  server.on("/list", HTTP_GET, [](){
    String list = "["; File root = LittleFS.open("/"); File file = root.openNextFile();
    while (file) { String n = file.name(); if (n.endsWith(".json") && n != "active.json") { if (list != "[") list += ","; list += "\"" + n.substring(0, n.length() - 5) + "\""; } file = root.openNextFile(); }
    list += "]"; server.send(200, "application/json", list);
  });
  server.on("/get_active", HTTP_GET, [](){
    File f = LittleFS.open("/active.json", "r"); if(f) { server.send(200, "application/json", f.readString()); f.close(); }
    else server.send(200, "application/json", "[{\"time\":0,\"value\":0},{\"time\":8000,\"value\":0}]");
  });
  server.on("/get_active_name", HTTP_GET, [](){
    if (LittleFS.exists("/active_name.txt")) { File f = LittleFS.open("/active_name.txt", "r"); String n = f.readString(); f.close(); server.send(200, "text/plain", n); }
    else server.send(200, "text/plain", "Geen");
  });
  server.on("/reset_clock_to", [](){ if(server.hasArg("t")) { startTime = millis() - server.arg("t").toInt(); } server.send(200); });
  server.begin();
}

void loop() {
  server.handleClient();
  unsigned long t = (millis() - startTime) % loopDuration;
  float currentVal = 0;
  for (int i = 0; i < envelopeSize - 1; i++) {
    if (t >= envelope[i].time && t <= envelope[i + 1].time) {
      float f = (float)(t - envelope[i].time) / (envelope[i + 1].time - envelope[i].time);
      currentVal = envelope[i].value + f * (envelope[i + 1].value - envelope[i].value);
      break;
    }
  }
  float speedFactor = abs(currentVal) / 100.0;
  if (speedFactor > 0.02) {
    unsigned long stepInterval = 1000000.0 / (speedFactor * MAX_STEPS_PER_SEC);
    if (micros() - lastStepMicros >= stepInterval) {
      lastStepMicros = micros();
      stepIndex = (currentVal > 0) ? (stepIndex + 1) % 8 : (stepIndex + 7) % 8;
      for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], steps[stepIndex][i]);
    }
  } else { for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], LOW); }
}