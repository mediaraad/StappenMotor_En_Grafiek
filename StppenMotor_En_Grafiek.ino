#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// --- Instellingen ---
const char* ssid_home = WIFI_TP_SSID;
const char* pass_home = WIFI_TP_PASSWORD;
const char* ap_ssid   = "Motor-Control-Pro";
const char* ap_pass   = "12345678"; 
const char* mdns_name = "motor";

WebServer server(80);

// --- Motor Config ---
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
bool manualOverride = false;
float manualValue = 0;

const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stepper Pro v17.5</title>
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
textarea{width:800px; height:100px; background:#1e1e1e; color:#00ff00; border:1px solid #444; font-family:monospace; margin-top:10px;}
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
        <select id="presetSelect" onchange="autoLoadPreset()"><option>Laden...</option></select>
        <button onclick="deletePreset()" class="btn-delete">🗑️</button>
        <span style="border-left:1px solid #444;height:30px;margin:0 5px;"></span>
        <button onclick="saveCurrent()">Opslaan</button>
        <button onclick="saveAs()" class="btn-alt">Opslaan als...</button>
        <span style="border-left:1px solid #444;height:30px;margin:0 5px;"></span>
        <label>Duur (s):</label>
        <input type="number" id="totalTime" value="8" min="1" style="width:50px;" onchange="updateDuration()">
        <button onclick="downloadConfig()" class="btn-alt">💾</button>
        <button onclick="document.getElementById('fileInput').click()" class="btn-alt">📂</button>
        <input type="file" id="fileInput" style="display:none" onchange="handleFileUpload(event)" accept=".json">
    </div>
    <div class="unsaved-container">
        <span id="unsavedWarning" class="unsaved-text">⚠️ WIJZIGINGEN NOG NIET OPGESLAGEN</span>
    </div>
    <canvas id="envelopeCanvas" width="800" height="400"></canvas>
    <textarea id="jsonEditor" spellcheck="false" oninput="applyJson()"></textarea>

    <div id="customModal">
        <div class="modal-content">
            <div id="modalText" class="modal-text"></div>
            <div class="modal-btns">
                <button id="modalConfirm" style="background:#00bfff;">Ja</button>
                <button onclick="closeModal()" class="btn-alt">Annuleer</button>
            </div>
        </div>
    </div>

<script>
const canvas=document.getElementById("envelopeCanvas"), ctx=canvas.getContext("2d"), editor=document.getElementById("jsonEditor");
let keyframes=[{time:0,value:0},{time:8000,value:0}], selected=null, playStart=Date.now();
let isDraggingPlayhead = false, manualTime = 0, isUnsaved = false;
let currentOpenFile = ""; 
let lastFetchTime = 0, isFetching = false;
const FETCH_THROTTLE = 100;

let modalResolve;
function openModal(text) {
    document.getElementById("modalText").innerText = text;
    document.getElementById("customModal").style.display = "block";
    return new Promise(resolve => { modalResolve = resolve; });
}
function closeModal() { document.getElementById("customModal").style.display = "none"; if(modalResolve) modalResolve(false); }
document.getElementById("modalConfirm").onclick = () => { document.getElementById("customModal").style.display = "none"; if(modalResolve) modalResolve(true); };

const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function markUnsaved() { isUnsaved = true; document.getElementById("unsavedWarning").style.display = "inline-block"; }
function markSaved(name) { 
    isUnsaved = false; 
    document.getElementById("unsavedWarning").style.display = "none"; 
    if(name) {
        currentOpenFile = name;
        document.getElementById("currentFileDisplay").innerText = " - " + name;
    }
}

function sync(){ 
    editor.value = JSON.stringify(keyframes, null, 2);
    document.getElementById("totalTime").value = Math.round(keyframes[keyframes.length-1].time / 1000);
    return fetch('/set_live',{method:'POST',body:JSON.stringify(keyframes)}); 
}

async function createNew() {
    keyframes = [{time:0,value:0},{time:8000,value:0}];
    currentOpenFile = "";
    document.getElementById("currentFileDisplay").innerText = "";
    document.getElementById("presetSelect").selectedIndex = -1;
    markSaved(""); // Reset unsaved status bij nieuw
    await sync();
    playStart = Date.now();
    fetch('/reset_clock_to?t=0');
}

async function autoLoadPreset(nameOverride = null){
    const sel = document.getElementById("presetSelect");
    const name = nameOverride || sel.value;
    if(!name || name.includes("...")) return;
    
    // Alleen waarschuwen als we handmatig een nieuwe kiezen in de pulldown
    if(!nameOverride && isUnsaved && !await openModal("Wijzigingen niet opgeslagen. Doorgaan?")) {
        sel.value = currentOpenFile;
        return;
    }

    const response = await fetch('/load?name=' + name);
    keyframes = await response.json();
    markSaved(name);
    sync();
    playStart = Date.now();
    fetch('/reset_clock_to?t=0');
}

async function deletePreset() {
    const name = document.getElementById("presetSelect").value;
    if(!name || name.includes("...")) return;

    if(await openModal("Weet je zeker dat je '" + name + "' wilt verwijderen?")) {
        await fetch('/delete?name=' + name, {method: 'DELETE'});
        
        // Haal de nieuwe lijst op
        const list = await fetch('/list').then(r => r.json());
        await updatePresetList();

        if (list.length > 0) {
            // Laad het eerste bestand uit de nieuwe lijst
            autoLoadPreset(list[0]);
        } else {
            // Geen bestanden meer over? Maak een nieuwe schone lei
            createNew();
        }
    }
}

async function fileExists(name) {
    const list = await fetch('/list').then(r=>r.json());
    return list.includes(name);
}

async function saveCurrent() {
    if(!currentOpenFile) { saveAs(); return; }
    if(await openModal("Weet je zeker dat je '" + currentOpenFile + "' wilt overschrijven?")) {
        performSave(currentOpenFile);
    }
}

async function saveAs() {
    let name = prompt("Voer een nieuwe naam in:", currentOpenFile || "config");
    if (!name) return;
    name = name.trim().replace(/[^a-z0-9_-]/gi, '_');
    if(await fileExists(name)) {
        if(!await openModal("Bestand '" + name + "' bestaat al. Overschrijven?")) return;
    }
    performSave(name);
}

function performSave(name) {
    fetch('/save?name='+name, {method:'POST', body:JSON.stringify(keyframes)}).then(()=>{
        markSaved(name); 
        updatePresetList(name);
    });
}

function draw(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    ctx.strokeStyle="#333"; ctx.lineWidth=1; ctx.beginPath(); ctx.moveTo(0,canvas.height/2); ctx.lineTo(canvas.width,canvas.height/2); ctx.stroke();
    ctx.strokeStyle="#00bfff"; ctx.lineWidth=3; ctx.beginPath();
    keyframes.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value)));
    ctx.stroke();
    ctx.fillStyle="#ff9900";
    keyframes.forEach(p=>{ctx.beginPath();ctx.arc(toX(p.time),toY(p.value),6,0,7);ctx.fill();});
    let dur = keyframes[keyframes.length-1].time;
    let elapsed = isDraggingPlayhead ? manualTime : (Date.now() - playStart) % dur;
    ctx.strokeStyle="red"; ctx.lineWidth=3; ctx.beginPath(); ctx.moveTo(toX(elapsed),0); ctx.lineTo(toX(elapsed),canvas.height); ctx.stroke();
    ctx.fillStyle="red"; ctx.beginPath(); ctx.arc(toX(elapsed), canvas.height-10, 8, 0, 7); ctx.fill();
    requestAnimationFrame(draw);
}

canvas.onmousedown=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left;
    let dur = keyframes[keyframes.length-1].time;
    if(Math.abs(x - toX((Date.now() - playStart) % dur)) < 35) { isDraggingPlayhead = true; updateManualPos(x); return; }
    selected=keyframes.find(p=>Math.hypot(toX(p.time)-x,toY(p.value)-(e.clientY-rect.top))<15);
    if(!selected){ keyframes.push({time:fromX(x),value:fromY(e.clientY-rect.top)}); keyframes.sort((a,b)=>a.time-b.time); markUnsaved(); sync(); }
};

window.onmousemove=(e)=>{
    if(isDraggingPlayhead) { updateManualPos(e.clientX-canvas.getBoundingClientRect().left); return; }
    if(!selected) return;
    const rect=canvas.getBoundingClientRect();
    if(selected!==keyframes[0] && selected!==keyframes[keyframes.length-1]) selected.time=Math.max(1, fromX(e.clientX-rect.left));
    selected.value=Math.max(-100,Math.min(100,fromY(e.clientY-rect.top)));
    keyframes.sort((a,b)=>a.time-b.time); markUnsaved(); sync();
};

window.onmouseup=()=>{
    if(isDraggingPlayhead) { playStart = Date.now() - manualTime; fetch('/reset_clock_to?t=' + Math.round(manualTime)); }
    selected=null; isDraggingPlayhead = false;
};

function updateManualPos(x) {
    let now = Date.now();
    manualTime = Math.max(0, Math.min(keyframes[keyframes.length-1].time, fromX(x)));
    if (!isFetching && (now - lastFetchTime > FETCH_THROTTLE)) {
        let val = 0;
        for (let i = 0; i < keyframes.length - 1; i++) {
            if (manualTime >= keyframes[i].time && manualTime <= keyframes[i+1].time) {
                let f = (manualTime - keyframes[i].time) / (keyframes[i+1].time - keyframes[i].time);
                val = keyframes[i].value + f * (keyframes[i+1].value - keyframes[i].value);
                break;
            }
        }
        isFetching = true; lastFetchTime = now;
        fetch('/set_manual?v=' + val.toFixed(2)).finally(() => { isFetching = false; });
    }
}

function applyJson(){ try { keyframes = JSON.parse(editor.value); markUnsaved(); sync(); } catch(e){} }

async function updatePresetList(targetName = null){
    const list = await fetch('/list').then(r=>r.json());
    const sel = document.getElementById("presetSelect");
    sel.innerHTML = list.length ? "" : "<option>Geen presets</option>";
    list.forEach(f => {
        let opt = document.createElement("option"); opt.value = f; opt.textContent = f;
        if(f === (targetName || currentOpenFile)) opt.selected = true;
        sel.appendChild(opt);
    });
}

function updateDuration(){ keyframes[keyframes.length-1].time = document.getElementById("totalTime").value * 1000; markUnsaved(); sync(); }

canvas.ondblclick=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left, y=e.clientY-rect.top;
    const index = keyframes.findIndex(p => Math.hypot(toX(p.time)-x, toY(p.value)-y) < 15);
    if(index > 0 && index < keyframes.length - 1){ keyframes.splice(index, 1); markUnsaved(); sync(); }
};

function downloadConfig() {
    let name = currentOpenFile || "configuratie";
    const dataStr = "data:text/json;charset=utf-8," + encodeURIComponent(JSON.stringify(keyframes, null, 2));
    const dl = document.createElement('a'); dl.setAttribute("href", dataStr); dl.setAttribute("download", name+".json"); dl.click();
}

async function handleFileUpload(event) {
    const file = event.target.files[0];
    if(!file) return;
    const reader = new FileReader();
    reader.onload = async (e) => {
        try {
            const uploadedData = JSON.parse(e.target.result);
            let baseName = file.name.replace(".json", "");
            await fetch('/save?name=' + baseName, {method: 'POST', body: JSON.stringify(uploadedData)});
            keyframes = uploadedData; 
            sync().then(() => { markSaved(baseName); updatePresetList(baseName); });
        } catch(err) { alert("Fout bij uploaden"); }
    };
    reader.readAsText(file);
    event.target.value = "";
}

async function init() {
    const nameRes = await fetch('/get_active_name');
    const name = await nameRes.text();
    if(name && name !== "Geen") {
        currentOpenFile = name;
        document.getElementById("currentFileDisplay").innerText = " - " + name;
    }
    const dataRes = await fetch('/get_active');
    const data = await dataRes.json();
    if(data.length > 0) keyframes = data;
    await updatePresetList();
    sync(); draw();
}
init();
</script>
</body>
</html>
)rawliteral";

// --- C++ BACKEND --- (Gelijk aan v17.4)
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
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(ap_ssid, ap_pass); WiFi.begin(ssid_home, pass_home);
  if (MDNS.begin(mdns_name)) { Serial.println("mDNS gestart"); }
  server.on("/", []() { server.send(200, "text/html", htmlPage); });
  server.on("/reset_clock_to", [](){ if(server.hasArg("t")) { startTime = millis() - server.arg("t").toInt(); manualOverride = false; } server.send(200); });
  server.on("/set_manual", [](){ if(server.hasArg("v")) { manualValue = server.arg("v").toFloat(); manualOverride = true; } server.send(200); });
  server.on("/set_live", HTTP_POST, [](){ if(server.hasArg("plain")) loadFromJSON(server.arg("plain")); server.send(200); });
  server.on("/save", HTTP_POST, [](){
    String name = server.arg("name"); String data = server.arg("plain");
    File f1 = LittleFS.open("/" + name + ".json", "w"); f1.print(data); f1.close();
    File f2 = LittleFS.open("/active.json", "w"); f2.print(data); f2.close();
    File f3 = LittleFS.open("/active_name.txt", "w"); f3.print(name); f3.close();
    startTime = millis(); server.send(200);
  });
  server.on("/delete", HTTP_DELETE, [](){
    String name = server.arg("name");
    if(LittleFS.exists("/" + name + ".json")) { LittleFS.remove("/" + name + ".json"); server.send(200); } 
    else server.send(404);
  });
  server.on("/load", HTTP_GET, [](){
    String name = server.arg("name");
    File file = LittleFS.open("/" + name + ".json", "r");
    if (file) {
      String content = file.readString(); file.close();
      loadFromJSON(content);
      File f2 = LittleFS.open("/active.json", "w"); f2.print(content); f2.close();
      File f3 = LittleFS.open("/active_name.txt", "w"); f3.print(name); f3.close();
      startTime = millis(); server.send(200, "application/json", content);
    } else server.send(404);
  });
  server.on("/list", HTTP_GET, [](){
    String list = "["; File root = LittleFS.open("/"); File file = root.openNextFile();
    while (file) {
      String n = file.name();
      if (n.endsWith(".json") && n != "active.json") { if (list != "[") list += ","; list += "\"" + n.substring(0, n.length() - 5) + "\""; }
      file = root.openNextFile();
    }
    list += "]"; server.send(200, "application/json", list);
  });
  server.on("/get_active", HTTP_GET, [](){
    File f = LittleFS.open("/active.json", "r"); if(f) { server.send(200, "application/json", f.readString()); f.close(); }
    else server.send(200, "application/json", "[]");
  });
  server.on("/get_active_name", HTTP_GET, [](){
    if (LittleFS.exists("/active_name.txt")) { File f = LittleFS.open("/active_name.txt", "r"); String n = f.readString(); f.close(); server.send(200, "text/plain", n); }
    else server.send(200, "text/plain", "Geen");
  });
  server.begin();
  startTime = millis();
}

void loop() {
  server.handleClient();
  float currentVal = 0;
  if (manualOverride) { currentVal = manualValue; } else {
    unsigned long t = (millis() - startTime) % loopDuration;
    for (int i = 0; i < envelopeSize - 1; i++) {
      if (t >= envelope[i].time && t <= envelope[i + 1].time) {
        float f = (float)(t - envelope[i].time) / (envelope[i + 1].time - envelope[i].time);
        currentVal = envelope[i].value + f * (envelope[i + 1].value - envelope[i].value);
        break;
      }
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