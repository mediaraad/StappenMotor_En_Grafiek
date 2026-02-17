#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

const char* ssid_home = WIFI_TP_SSID;
const char* pass_home = WIFI_TP_PASSWORD;
const char* ap_ssid = "Motor-Control-Pro";
const char* ap_pass = "12345678"; 

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
bool manualOverride = false;
float manualValue = 0;

const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stepper Pro v15.9</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:20px;}
canvas{background:#1e1e1e;border:2px solid #444;cursor:pointer;touch-action:none;display:block;margin:5px auto;border-radius:8px;}
.controls{background:#2a2a2a;padding:15px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;gap:10px;margin:0 auto 10px auto;border:1px solid #444;flex-wrap:wrap;max-width:800px;box-sizing:border-box;}
input,button,select{height:42px; padding:0 10px; border-radius:4px;border:none;background:#444;color:white;outline:none;font-size:14px;}
button{background:#00bfff;cursor:pointer;font-weight:bold;transition:0.2s;}
button:hover{background:#009cd1;transform:translateY(-1px);}
.btn-delete{background:#ff4444 !important; font-size:18px;}
.btn-alt{background:#666 !important;}
.unsaved-container{height: 20px;}
.unsaved-text{color:#ff9900; font-weight:bold; font-size: 11px; display:none;}
textarea{width:800px; height:150px; background:#1e1e1e; color:#00ff00; border:1px solid #444; font-family:monospace; padding:10px; border-radius:4px; margin-top:10px;}
#customModal{display:none; position:fixed; z-index:1000; left:0; top:0; width:100%; height:100%; background:rgba(0,0,0,0.8); backdrop-filter:blur(4px);}
.modal-content{background:#2a2a2a; margin:15% auto; padding:25px; border:1px solid #00bfff; width:320px; border-radius:12px; box-shadow:0 0 20px rgba(0,191,255,0.2); animation: slideIn 0.3s ease-out;}
@keyframes slideIn { from{transform:translateY(-20px);opacity:0;} to{transform:translateY(0);opacity:1;} }
.modal-btns{display:flex; justify-content:center; gap:15px; margin-top:20px;}
.modal-text{font-size:16px; margin-bottom:10px; line-height:1.4;}
</style>
</head>
<body>
    <h2>Motor Envelope Control</h2>
    <div class="controls">
        <select id="presetSelect" onchange="autoLoadPreset()"><option>Laden...</option></select>
        <button onclick="confirmAction('delete')" class="btn-delete" title="Verwijder van ESP">🗑️</button>
        <span style="border-left:1px solid #444;height:30px;margin:0 5px;"></span>
        <input type="text" id="filename" placeholder="Naam..." style="width:100px;">
        <label>Duur:</label>
        <input type="number" id="totalTime" value="8" min="1" style="width:50px;" onchange="updateDuration()">
        <button onclick="savePreset()">Opslaan</button>
        <span style="border-left:1px solid #444;height:30px;margin:0 5px;"></span>
        <button onclick="downloadConfig()" class="btn-alt" title="Download naar PC">💾</button>
        <button onclick="document.getElementById('fileInput').click()" class="btn-alt" title="Upload vanaf PC">📂</button>
        <input type="file" id="fileInput" style="display:none" onchange="handleFileUpload(event)" accept=".json">
    </div>
    <div class="unsaved-container"><span id="unsavedWarning" class="unsaved-text">WIJZIGINGEN NOG NIET OPGESLAGEN</span></div>
    <canvas id="envelopeCanvas" width="800" height="400"></canvas>
    <textarea id="jsonEditor" spellcheck="false" oninput="applyJson()"></textarea>

    <div id="customModal">
        <div class="modal-content">
            <div id="modalText" class="modal-text">Weet je het zeker?</div>
            <div class="modal-btns">
                <button id="modalConfirm" style="background:#00bfff;">Ja</button>
                <button onclick="closeModal()" class="btn-alt">Annuleer</button>
            </div>
        </div>
    </div>

<script>
const canvas=document.getElementById("envelopeCanvas"), ctx=canvas.getContext("2d"), editor=document.getElementById("jsonEditor");
let keyframes=[{time:0,value:0},{time:8000,value:0}], selected=null, playStart=Date.now();
let isDraggingPlayhead = false, manualTime = 0, isUnsaved = false, lastFetchTime = 0;
const UPDATE_INTERVAL = 60; 

let modalResolve;
function openModal(text) {
    document.getElementById("modalText").innerText = text;
    document.getElementById("customModal").style.display = "block";
    return new Promise(resolve => { modalResolve = resolve; });
}
function closeModal() {
    document.getElementById("customModal").style.display = "none";
    if(modalResolve) modalResolve(false);
}
document.getElementById("modalConfirm").onclick = () => {
    document.getElementById("customModal").style.display = "none";
    if(modalResolve) modalResolve(true);
};

const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function markUnsaved() { isUnsaved = true; document.getElementById("unsavedWarning").style.display = "inline-block"; }
function markSaved() { isUnsaved = false; document.getElementById("unsavedWarning").style.display = "none"; }

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
    if (now - lastFetchTime > UPDATE_INTERVAL) {
        lastFetchTime = now;
        let val = 0;
        for (let i = 0; i < keyframes.length - 1; i++) {
            if (manualTime >= keyframes[i].time && manualTime <= keyframes[i+1].time) {
                let f = (manualTime - keyframes[i].time) / (keyframes[i+1].time - keyframes[i].time);
                val = keyframes[i].value + f * (keyframes[i+1].value - keyframes[i].value);
                break;
            }
        }
        fetch('/set_manual?v=' + val.toFixed(2));
    }
}

function downloadConfig() {
    let fileName = document.getElementById("filename").value.trim();
    if (!fileName) {
        const sel = document.getElementById("presetSelect");
        if (sel.value && sel.value !== "Geen presets" && sel.value !== "Laden...") fileName = sel.value;
    }
    if (!fileName) fileName = "motor_preset";
    const dataStr = "data:text/json;charset=utf-8," + encodeURIComponent(JSON.stringify(keyframes, null, 2));
    const dl = document.createElement('a');
    dl.setAttribute("href", dataStr);
    dl.setAttribute("download", fileName + ".json");
    dl.click();
    dl.remove();
}

async function handleFileUpload(event) {
    const file = event.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = async (e) => {
        try {
            const uploadedData = JSON.parse(e.target.result);
            let baseName = file.name.replace(".json", "");
            const existingPresets = await fetch('/list').then(r => r.json());
            let finalName = baseName;
            let counter = 1;
            while (existingPresets.includes(finalName)) {
                finalName = baseName + "_" + counter;
                counter++;
            }
            await fetch('/save?name=' + finalName, {method: 'POST', body: JSON.stringify(uploadedData)});
            keyframes = uploadedData;
            markSaved();
            sync();
            updatePresetList(finalName);
            document.getElementById("filename").value = ""; 
        } catch(err) { alert("Fout bij uploaden: " + err); }
    };
    reader.readAsText(file);
    event.target.value = "";
}

function sync(){ 
    editor.value = JSON.stringify(keyframes, null, 2);
    document.getElementById("totalTime").value = Math.round(keyframes[keyframes.length-1].time / 1000);
    fetch('/set_live',{method:'POST',body:JSON.stringify(keyframes)}); 
}

function applyJson(){ try { keyframes = JSON.parse(editor.value); markUnsaved(); sync(); } catch(e){} }

async function savePreset(){
    const name = document.getElementById("filename").value.trim();
    if(!name) return alert("Naam verplicht");
    const list = await fetch('/list').then(r=>r.json());
    if(list.includes(name)) {
        const ok = await openModal("Naam '" + name + "' bestaat al. Overschrijven?");
        if(!ok) return;
    }
    fetch('/save?name='+name, {method:'POST', body:JSON.stringify(keyframes)}).then(()=>{
        document.getElementById("filename").value = ""; updatePresetList(name); markSaved();
    });
}

async function deletePreset() {
    const name = document.getElementById("presetSelect").value;
    if(!name || name === "Geen presets" || name === "Laden...") return;
    const ok = await openModal("Zeker weten dat je '" + name + "' wilt wissen?");
    if(ok) {
        fetch('/delete?name=' + name, {method:'DELETE'}).then(() => {
            updatePresetList().then(() => {
                autoLoadPreset(true); 
            });
        });
    }
}

async function confirmAction(type) { if(type === 'delete') await deletePreset(); }

let lastSelectedValue = "";
async function autoLoadPreset(force = false){
    const sel = document.getElementById("presetSelect");
    const val = sel.value;
    if(!val || val === "Geen presets" || val === "Laden...") return;

    if(!force && isUnsaved) {
        const ok = await openModal("Wijzigingen niet opgeslagen. Doorgaan?");
        if(!ok) {
            sel.value = lastSelectedValue; // Reset dropdown naar vorige stand
            return;
        }
    }

    lastSelectedValue = val;
    fetch('/load?name=' + val).then(r=>r.json()).then(data=>{
        keyframes = data; sync(); playStart = Date.now(); fetch('/reset_clock_to?t=0'); markSaved();
        document.getElementById("filename").value = ""; // Veld leegmaken na laden
    });
}

async function updatePresetList(targetName = null){
    const list = await fetch('/list').then(r=>r.json());
    if(!targetName) targetName = await fetch('/get_active_name').then(r=>r.text());
    const sel = document.getElementById("presetSelect");
    sel.innerHTML = list.length ? "" : "<option>Geen presets</option>";
    list.forEach(f => {
        let opt = document.createElement("option"); opt.value = f; opt.textContent = f;
        if(f === targetName) { opt.selected = true; lastSelectedValue = f; }
        sel.appendChild(opt);
    });
}

function updateDuration(){
    keyframes[keyframes.length-1].time = document.getElementById("totalTime").value * 1000;
    markUnsaved(); sync();
}

canvas.ondblclick=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left, y=e.clientY-rect.top;
    const index = keyframes.findIndex(p => Math.hypot(toX(p.time)-x, toY(p.value)-y) < 15);
    if(index > 0 && index < keyframes.length - 1){ keyframes.splice(index, 1); markUnsaved(); sync(); }
};

fetch('/get_active').then(r=>r.json()).then(data=>{ if(data.length > 0) keyframes = data; sync(); updatePresetList(); draw(); });
</script>
</body>
</html>
)rawliteral";

// --- C++ BACKEND (ongewijzigd) ---
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
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_pass);
  WiFi.begin(ssid_home, pass_home);
  if (MDNS.begin("motor")) { Serial.println("mDNS gestart"); }
  server.on("/", []() { server.send(200, "text/html", htmlPage); });
  server.on("/reset_clock_to", [](){
    if(server.hasArg("t")) { startTime = millis() - server.arg("t").toInt(); manualOverride = false; }
    server.send(200);
  });
  server.on("/set_manual", [](){
    if(server.hasArg("v")) { manualValue = server.arg("v").toFloat(); manualOverride = true; }
    server.send(200);
  });
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
    File f = LittleFS.open("/active.json", "r");
    if(f) { server.send(200, "application/json", f.readString()); f.close(); }
    else server.send(200, "application/json", "[]");
  });
  server.on("/get_active_name", HTTP_GET, [](){
    if (LittleFS.exists("/active_name.txt")) {
      File f = LittleFS.open("/active_name.txt", "r"); String n = f.readString(); f.close(); server.send(200, "text/plain", n);
    } else server.send(200, "text/plain", "Geen");
  });
  server.begin();
  startTime = millis();
}

void loop() {
  server.handleClient();
  float currentVal = 0;
  if (manualOverride) {
    currentVal = manualValue;
  } else {
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