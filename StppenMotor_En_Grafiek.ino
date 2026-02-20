#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

/* Script voor ESP32 en  28BYJ-48  en  ULN2003*/

const char* ssid_home = WIFI_TP_SSID;
const char* pass_home = WIFI_TP_PASSWORD;
const char* ap_ssid   = SSID;
const char* ap_pass   = LOCAL_WIFI_PASS; 
const char* mdns_name = MDNS_NAME;

WebServer server(80);

const int motorPins[] = {14, 12, 13, 15}; 
int stepIndex = 0;
unsigned long lastStepMicros = 0;
const float MAX_STEPS_PER_SEC = 800.0;
const int steps[8][4] = {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,1,1},{0,0,0,1},{1,0,0,1}};

// Positie beheer
long motorPosition = 0;
bool isHoming = false;

struct Keyframe { unsigned long time; float value; };
Keyframe envelope[50];
int envelopeSize = 2;
unsigned long startTime = 0;
unsigned long loopDuration = 8000;
int currentSegment = 0; 
bool isPaused = false;

const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stepper Pro v20.8 - Home Position</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:20px;}
canvas{background:#1e1e1e;border:2px solid #444;cursor:pointer;touch-action:none;display:block;margin:5px auto;border-radius:8px;box-shadow: 0 4px 15px rgba(0,0,0,0.5);}
.controls{background:#2a2a2a;padding:15px;border-radius:8px;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:12px;margin:0 auto 10px auto;border:1px solid #444;max-width:950px;box-sizing:border-box;box-shadow: 0 2px 10px rgba(0,0,0,0.3);}
.control-row{display:flex; align-items:center; justify-content:center; gap:10px; flex-wrap:wrap;}
input,button,select{height:42px; padding:0 10px; border-radius:4px;border:none;background:#444;color:white;outline:none;font-size:14px;transition:0.2s;}
button{background:#00bfff;cursor:pointer;font-weight:bold;}
button:hover{background:#009cd1;transform:translateY(-1px);}
.btn-delete{background:#ff4444 !important;}
.btn-alt{background:#555 !important;}
.btn-new{background:#28a745 !important;}
.btn-play{background:#28a745 !important; width:50px;}
.btn-stop{background:#ffc107 !important; color:black !important; width:50px;}
.btn-home{background:#9b59b6 !important; color:white !important;}
.unsaved-container{height: 25px; margin-bottom:5px;}
.unsaved-text{color:#ff9900; font-weight:bold; font-size: 13px; display:none;}
#currentFileDisplay{color:#00ff00; font-weight:bold; margin-left:10px;}
textarea{width:800px; height:120px; background:#1e1e1e; color:#00ff00; border:1px solid #444; font-family:monospace; margin-top:10px; padding:10px; border-radius:8px;}
.slider-group{display:flex; align-items:center; gap:5px; background:#333; padding:0 10px; border-radius:4px; height:42px;}
#customModal{display:none; position:fixed; z-index:1000; left:0; top:0; width:100%; height:100%; background:rgba(0,0,0,0.85); backdrop-filter:blur(5px);}
.modal-content{background:#2a2a2a; margin:15% auto; padding:30px; border:1px solid #00bfff; width:350px; border-radius:16px;}
#modalInput{width:100%; margin-bottom:20px; text-align:center; display:none; background:#1e1e1e; border:1px solid #444; color:white; height:35px;}
.modal-btns{display:flex; justify-content:center; gap:15px;}
</style>
</head>
<body>
    <h2>Motor Control Pro <span id="currentFileDisplay"></span></h2>
    <div class="controls">
        <div class="control-row">
            <button onclick="createNew()" class="btn-new">➕</button>
            <select id="presetSelect" onchange="autoLoadPreset()"></select>
            <button onclick="deletePreset()" class="btn-delete">🗑️</button>
            <span style="border-left:1px solid #444;height:30px;"></span>
            <button onclick="saveCurrent()">Opslaan</button>
            <button onclick="saveAs()" class="btn-alt">Opslaan als...</button>
            <span style="border-left:1px solid #444;height:30px;"></span>
            <button onclick="downloadConfig()" class="btn-alt">💾</button>
            <button onclick="document.getElementById('fileInput').click()" class="btn-alt">📂</button>
            <input type="file" id="fileInput" style="display:none" onchange="handleFileUpload(event)" accept=".json">
        </div>
        <div class="control-row">
            <div class="slider-group">
                <label style="font-size:12px;">Chaos</label>
                <input type="range" id="chaosSlider" min="0" max="100" value="0" oninput="updateChaosLabel(); markUnsaved(); sync(true);">
                <span id="chaosVal" style="font-size:12px;width:30px;">0%</span>
            </div>
            <span style="border-left:1px solid #444;height:30px;"></span>
            <button id="playBtn" onclick="togglePause()" class="btn-play">⏸</button>
            <button onclick="stopAndReset()" class="btn-stop">■</button>
            <button onclick="goHome()" class="btn-home">🏠 Home</button>
            <span style="border-left:1px solid #444;height:30px;"></span>
            <label>Duur (sec):</label>
            <input type="number" id="totalTime" value="8" min="1" style="width:55px;" onchange="updateDuration()">
        </div>
    </div>
    <div class="unsaved-container"><span id="unsavedWarning" class="unsaved-text">⚠️ NIET OPGESLAGEN</span></div>
    <canvas id="envelopeCanvas" width="800" height="400"></canvas>
    <textarea id="jsonEditor" spellcheck="false" oninput="applyJson()"></textarea>

    <div id="customModal">
        <div class="modal-content">
            <div id="modalText" style="margin-bottom:15px;"></div>
            <input type="text" id="modalInput">
            <div class="modal-btns">
                <button id="modalConfirm">Ja</button>
                <button onclick="closeModal()" class="btn-alt">Annuleer</button>
            </div>
        </div>
    </div>

<script>
const canvas=document.getElementById("envelopeCanvas"), ctx=canvas.getContext("2d"), editor=document.getElementById("jsonEditor");
let keyframes=[{time:0,value:0},{time:8000,value:0}], originalKeyframes=[];
let selected=null, playStart=Date.now(), isUnsaved=false, currentOpenFile="";
let isDraggingPlayhead = false, manualTime = 0, ghostPoints = [], lastElapsed = 0, firstCycleDone = false;
let pausedTime = 0, isPaused = false;

let modalResolve;
function openModal(text, showInput=false){ 
    document.getElementById("modalText").innerText=text; 
    const inp = document.getElementById("modalInput");
    inp.style.display = showInput ? "block" : "none";
    if(showInput) inp.value = currentOpenFile || "config";
    document.getElementById("customModal").style.display="block"; 
    return new Promise(r=>modalResolve=r); 
}
function closeModal(){ document.getElementById("customModal").style.display="none"; if(modalResolve) modalResolve(null); }
document.getElementById("modalConfirm").onclick=()=>{ 
    const val = document.getElementById("modalInput").value;
    document.getElementById("customModal").style.display="none"; 
    if(modalResolve) modalResolve(val || true); 
};

function togglePause() {
    isPaused = !isPaused;
    const btn = document.getElementById("playBtn");
    btn.innerText = isPaused ? "▶" : "⏸";
    if(isPaused) {
        pausedTime = (Date.now() - playStart) % keyframes[keyframes.length-1].time;
    } else {
        playStart = Date.now() - pausedTime;
    }
    fetch('/toggle_pause?p=' + (isPaused ? "1" : "0"));
}

async function stopAndReset() {
    isPaused = true;
    document.getElementById("playBtn").innerText = "▶";
    pausedTime = 0;
    playStart = Date.now();
    if(currentOpenFile) {
        await autoLoadPreset(currentOpenFile, true);
    } else {
        fetch('/toggle_pause?p=1&reset=1');
    }
}

function goHome() {
    isPaused = true;
    document.getElementById("playBtn").innerText = "▶";
    fetch('/go_home');
}

function updateChaosLabel(){ document.getElementById("chaosVal").innerText = document.getElementById("chaosSlider").value + "%"; }
const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function generateGhost() {
    const chaos = document.getElementById("chaosSlider").value / 100;
    if(chaos <= 0) { ghostPoints = []; return; }
    const source = originalKeyframes.length > 0 ? originalKeyframes : keyframes;
    const maxDur = source[source.length-1].time;
    const jitterT = maxDur * 0.1 * chaos; 
    const jitterV = 25 * chaos; 

    ghostPoints = source.map((p, i) => {
        if (i === 0 || i === source.length - 1) return { ...p };
        let newTime = p.time + (Math.random() - 0.5) * jitterT;
        const minT = source[i-1].time + 10;
        const maxT = source[i+1].time - 10;
        newTime = Math.max(minT, Math.min(maxT, newTime));
        return { time: newTime, value: Math.max(-100, Math.min(100, p.value + (Math.random() - 0.5) * jitterV)) };
    });
    ghostPoints.sort((a,b) => a.time - b.time);
}

function markUnsaved(){ isUnsaved=true; document.getElementById("unsavedWarning").style.display="inline-block"; originalKeyframes = JSON.parse(JSON.stringify(keyframes)); }

function markSaved(name){ 
    isUnsaved = false; 
    document.getElementById("unsavedWarning").style.display = "none"; 
    const sel = document.getElementById("presetSelect");
    let exists = false;
    for(let i=0; i<sel.options.length; i++) {
        if(sel.options[i].value === name) {
            exists = true;
            sel.selectedIndex = i;
            sel.options[i].setAttribute('selected', 'selected');
        } else {
            sel.options[i].removeAttribute('selected');
        }
    }
    if(exists && name !== "") {
        currentOpenFile = name;
        document.getElementById("currentFileDisplay").innerText = " - " + name;
    } else {
        currentOpenFile = "";
        document.getElementById("currentFileDisplay").innerText = "";
    }
    originalKeyframes = JSON.parse(JSON.stringify(keyframes));
}

function getExportData() { return { chaos: parseInt(document.getElementById("chaosSlider").value), keyframes: originalKeyframes.length > 0 ? originalKeyframes : keyframes }; }

function sync(skipGhost=false){ 
    const data = getExportData();
    editor.value=JSON.stringify(data,null,2);
    document.getElementById("totalTime").value=Math.round(keyframes[keyframes.length-1].time/1000);
    if(firstCycleDone && !skipGhost) generateGhost();
    return fetch('/set_live',{method:'POST',body:JSON.stringify(data)}); 
}

function draw(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    ctx.strokeStyle="#333"; ctx.lineWidth=1; ctx.beginPath(); ctx.moveTo(0,canvas.height/2); ctx.lineTo(canvas.width,canvas.height/2); ctx.stroke();
    let dur=keyframes[keyframes.length-1].time;
    let elapsed = isDraggingPlayhead ? manualTime : (isPaused ? pausedTime : (Date.now()-playStart)%dur);
    
    if (!isPaused && !isDraggingPlayhead && elapsed < lastElapsed) {
        if(firstCycleDone && ghostPoints.length > 0) { 
            keyframes = [...ghostPoints]; 
            const data = { chaos: parseInt(document.getElementById("chaosSlider").value), keyframes: keyframes };
            fetch('/set_live',{method:'POST',body:JSON.stringify(data)}); 
        }
        firstCycleDone = true; 
        generateGhost();
    }
    lastElapsed = elapsed;
    if (firstCycleDone && ghostPoints.length > 0) {
        ctx.strokeStyle="rgba(0, 191, 255, 0.2)"; ctx.lineWidth=2; ctx.setLineDash([5, 5]); ctx.beginPath();
        ghostPoints.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value)));
        ctx.stroke(); ctx.setLineDash([]);
    }
    ctx.strokeStyle="#00bfff"; ctx.lineWidth=3; ctx.beginPath();
    keyframes.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value)));
    ctx.stroke();
    ctx.fillStyle="#ff9900"; keyframes.forEach(p=>{ctx.beginPath();ctx.arc(toX(p.time),toY(p.value),6,0,7);ctx.fill();});
    ctx.strokeStyle="red"; ctx.lineWidth=3; ctx.beginPath(); ctx.moveTo(toX(elapsed),0); ctx.lineTo(toX(elapsed),canvas.height); ctx.stroke();
    requestAnimationFrame(draw);
}

canvas.onmousedown=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left, y=e.clientY-rect.top;
    let dur=keyframes[keyframes.length-1].time;
    let currentPos = isPaused ? pausedTime : (Date.now()-playStart)%dur;
    if(Math.abs(x - toX(currentPos)) < 30) { 
        isDraggingPlayhead = true; 
        manualTime = currentPos; 
        return; 
    }
    selected=keyframes.find(p=>Math.hypot(toX(p.time)-x,toY(p.value)-y)<15);
    if(!selected){ keyframes.push({time:fromX(x),value:fromY(y)}); keyframes.sort((a,b)=>a.time-b.time); markUnsaved(); sync(); }
};

canvas.ondblclick=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left, y=e.clientY-rect.top;
    const pIdx=keyframes.findIndex(p=>Math.hypot(toX(p.time)-x,toY(p.value)-y)<15);
    if(pIdx>0 && pIdx<keyframes.length-1){ keyframes.splice(pIdx,1); markUnsaved(); sync(); }
};

window.onmousemove=(e)=>{
    if(isDraggingPlayhead) { manualTime = Math.max(0, Math.min(keyframes[keyframes.length-1].time, fromX(e.clientX-canvas.getBoundingClientRect().left))); return; }
    if(!selected) return;
    const rect=canvas.getBoundingClientRect();
    if(selected!==keyframes[0] && selected!==keyframes[keyframes.length-1]) {
        let t = fromX(e.clientX-rect.left);
        let idx = keyframes.indexOf(selected);
        selected.time = Math.max(keyframes[idx-1].time + 10, Math.min(keyframes[idx+1].time - 10, t));
    }
    selected.value=Math.max(-100,Math.min(100,fromY(e.clientY-rect.top)));
    keyframes.sort((a,b)=>a.time-b.time); markUnsaved(); sync();
};

window.onmouseup=()=>{ 
    if(isDraggingPlayhead) { 
        if(isPaused) pausedTime = manualTime;
        else playStart = Date.now() - manualTime; 
        fetch('/reset_clock_to?t=' + Math.round(manualTime)); 
    } 
    selected=null; isDraggingPlayhead = false; 
};

async function autoLoadPreset(targetName, forceRevert=false){
    const name = targetName || document.getElementById("presetSelect").value;
    if(!name || name === "") return;
    if(isUnsaved && !targetName && !forceRevert && !await openModal("Wijzigingen gaan verloren. Doorgaan?")){ 
        document.getElementById("presetSelect").value = currentOpenFile || ""; 
        return; 
    }
    const res = await fetch('/load?name='+name); 
    if(!res.ok) return;
    const data = await res.json();
    if(data.keyframes) { keyframes = data.keyframes; document.getElementById("chaosSlider").value = data.chaos || 0; }
    else { keyframes = data; document.getElementById("chaosSlider").value = 0; }
    updateChaosLabel(); firstCycleDone = false; ghostPoints = [];
    markSaved(name); await sync(true); 
    
    if(forceRevert) {
        isPaused = true;
        document.getElementById("playBtn").innerText = "▶";
        pausedTime = 0;
        playStart = Date.now();
        fetch('/toggle_pause?p=1&reset=1');
    }
}

async function updatePresetList(targetName){
    const res = await fetch('/list');
    const list = await res.json();
    const sel = document.getElementById("presetSelect"); sel.innerHTML = "";
    list.forEach(f=>{ 
        let o=document.createElement("option"); 
        o.value=o.textContent=f; 
        if(f === targetName) o.setAttribute('selected', 'selected');
        sel.appendChild(o); 
    });
    return list;
}

async function deletePreset(){
    const name = document.getElementById("presetSelect").value;
    if(!name || name === "" || !await openModal("Verwijder '"+name+"' definitief?")) return;
    await fetch('/delete?name='+name, {method:'DELETE'});
    currentOpenFile = ""; 
    const newList = await updatePresetList();
    if(newList.length > 0) autoLoadPreset(newList[0]); else createNew();
}

function createNew(){ keyframes=[{time:0,value:0},{time:8000,value:0}]; document.getElementById("chaosSlider").value=0; updateChaosLabel(); firstCycleDone=false; ghostPoints=[]; markSaved(""); updatePresetList(); sync(true); stopAndReset(); }
function performSave(n){ fetch('/save?name='+n,{method:'POST',body:JSON.stringify(getExportData())}).then(()=>{markSaved(n);updatePresetList(n);}); }
async function saveCurrent(){ if(!currentOpenFile)saveAs(); else if(await openModal("Bestand '" + currentOpenFile + "' overschrijven?")) performSave(currentOpenFile); }
async function saveAs(){ let n=await openModal("Sla configuratie op als:", true); if(n && typeof n === 'string') performSave(n.trim().replace(/[^a-z0-9_-]/gi,'_')); }
async function handleFileUpload(e){
    const file=e.target.files[0]; if(!file) return;
    const reader=new FileReader();
    reader.onload=async (ev)=>{
        try{
            const data=JSON.parse(ev.target.result);
            let name=file.name.replace(".json","").replace(/[^a-z0-9_-]/gi,'_');
            await fetch('/save?name='+name,{method:'POST',body:JSON.stringify(data)});
            if(data.keyframes) keyframes=data.keyframes; else keyframes=data;
            markSaved(name); await updatePresetList(name); await sync(true); stopAndReset();
        }catch(err){alert("Fout");}
    };
    reader.readAsText(file); e.target.value="";
}
function downloadConfig(){ const a=document.createElement('a'); a.href="data:text/json;charset=utf-8,"+encodeURIComponent(JSON.stringify(getExportData(),null,2)); a.download=(currentOpenFile||"config")+".json"; a.click(); }

function updateDuration(){ 
    const oldDur = keyframes[keyframes.length-1].time;
    const newDur = document.getElementById("totalTime").value * 1000;
    if(oldDur <= 0 || newDur <= 0) return;
    const factor = newDur / oldDur;
    keyframes.forEach(p => { p.time = Math.round(p.time * factor); });
    if(isPaused) pausedTime *= factor;
    markUnsaved(); sync(); 
}

function applyJson(){ try{const data=JSON.parse(editor.value); if(data.keyframes) keyframes=data.keyframes; else keyframes=data; markUnsaved(); sync();}catch(e){} }

async function init(){
    const [dataRes, nameRes, listRes] = await Promise.all([ fetch('/get_active'), fetch('/get_active_name'), fetch('/list') ]);
    const data = await dataRes.json();
    const activeName = await nameRes.text();
    const list = await listRes.json();
    if(data.keyframes) { keyframes = data.keyframes; document.getElementById("chaosSlider").value = data.chaos || 0; }
    else { keyframes = data; }
    updateChaosLabel();
    const validatedName = (list.includes(activeName)) ? activeName : "";
    await updatePresetList(validatedName);
    markSaved(validatedName);
    const tRes = await fetch('/get_time');
    const t = await tRes.text();
    playStart = Date.now() - parseInt(t);
    sync(true);
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
  JsonArray arr;
  if (doc.is<JsonObject>() && doc.containsKey("keyframes")) arr = doc["keyframes"].as<JsonArray>();
  else arr = doc.as<JsonArray>();
  envelopeSize = arr.size();
  for (int i = 0; i < (envelopeSize < 50 ? envelopeSize : 50); i++) {
    envelope[i].time = arr[i]["time"];
    envelope[i].value = (float)arr[i]["value"];
  }
  loopDuration = (envelopeSize > 1) ? envelope[envelopeSize - 1].time : 8000;
  currentSegment = 0;
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(motorPins[i], OUTPUT);
  LittleFS.begin(true);
  if (LittleFS.exists("/active.json")) loadFromJSON(LittleFS.open("/active.json", "r").readString());
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(ap_ssid, ap_pass); WiFi.begin(ssid_home, pass_home); MDNS.begin(mdns_name);
  server.on("/", [](){ server.send(200, "text/html", htmlPage); });
  server.on("/get_time", [](){ server.send(200, "text/plain", String((millis() - startTime) % loopDuration)); });
  server.on("/toggle_pause", [](){ 
    if(server.hasArg("p")) isPaused = (server.arg("p") == "1");
    if(server.hasArg("reset")) { startTime = millis(); currentSegment = 0; }
    server.send(200); 
  });
  server.on("/go_home", [](){ isHoming = true; isPaused = true; server.send(200); });
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
  server.on("/delete", HTTP_DELETE, [](){ 
    String name = server.arg("name"); 
    if(LittleFS.exists("/"+name+".json")) LittleFS.remove("/"+name+".json");
    if (LittleFS.exists("/active_name.txt")) {
        File f = LittleFS.open("/active_name.txt", "r");
        String current = f.readString(); f.close();
        if(current == name) LittleFS.remove("/active_name.txt");
    }
    server.send(200); 
  });
  server.on("/list", HTTP_GET, [](){
    String list = "["; File root = LittleFS.open("/"); File file = root.openNextFile();
    while (file) { String n = file.name(); if (n.endsWith(".json") && n != "active.json") { if (list != "[") list += ","; list += "\"" + n.substring(0, n.length() - 5) + "\""; } file = root.openNextFile(); }
    list += "]"; server.send(200, "application/json", list);
  });
  server.on("/get_active", HTTP_GET, [](){
    File f = LittleFS.open("/active.json", "r"); if(f) { server.send(200, "application/json", f.readString()); f.close(); }
    else server.send(200, "application/json", "{\"chaos\":0,\"keyframes\":[{\"time\":0,\"value\":0},{\"time\":8000,\"value\":0}]}");
  });
  server.on("/get_active_name", HTTP_GET, [](){
    if (LittleFS.exists("/active_name.txt")) { File f = LittleFS.open("/active_name.txt", "r"); String n = f.readString(); f.close(); server.send(200, "text/plain", n); }
    else server.send(200, "text/plain", "Geen");
  });
  server.on("/reset_clock_to", [](){ if(server.hasArg("t")) { startTime = millis() - server.arg("t").toInt(); currentSegment = 0; } server.send(200); });
  server.begin();
}

void loop() {
  server.handleClient();
  
  // Homing logica
  if (isHoming) {
    if (motorPosition == 0) {
      isHoming = false;
      for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], LOW);
    } else {
      if (micros() - lastStepMicros >= 1250) { // Constante snelheid voor homing
        lastStepMicros = micros();
        if (motorPosition > 0) { motorPosition--; stepIndex = (stepIndex + 7) % 8; }
        else { motorPosition++; stepIndex = (stepIndex + 1) % 8; }
        for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], steps[stepIndex][i]);
      }
    }
    return;
  }

  if (isPaused) {
    for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], LOW);
    return;
  }

  unsigned long t = (millis() - startTime) % loopDuration;
  if (t < envelope[currentSegment].time) currentSegment = 0;
  while (currentSegment < envelopeSize - 1 && t > envelope[currentSegment + 1].time) {
    currentSegment++;
  }
  float f = (float)(t - envelope[currentSegment].time) / (envelope[currentSegment + 1].time - envelope[currentSegment].time);
  float currentVal = envelope[currentSegment].value + f * (envelope[currentSegment + 1].value - envelope[currentSegment].value);
  float speedFactor = abs(currentVal) / 100.0;
  
  if (speedFactor > 0.02) {
    unsigned long stepInterval = 1000000.0 / (speedFactor * MAX_STEPS_PER_SEC);
    if (micros() - lastStepMicros >= stepInterval) {
      lastStepMicros = micros();
      if (currentVal > 0) { stepIndex = (stepIndex + 1) % 8; motorPosition++; }
      else { stepIndex = (stepIndex + 7) % 8; motorPosition--; }
      for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], steps[stepIndex][i]);
    }
  } else { for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], LOW); }
}