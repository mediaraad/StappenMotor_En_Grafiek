#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

/* Script voor ESP32 en 28BYJ-48 en ULN2003*/

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

long motorPosition = 0;

struct Keyframe { unsigned long time; float value; };
Keyframe envelope[50];
int envelopeSize = 2;
unsigned long startTime = 0;
unsigned long loopDuration = 8000;
int currentSegment = 0; 
bool isPaused = false; 

const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="nl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Stepper Pro v21.22 - Delete Restored</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:20px;}
#canvasContainer{width:100%; max-width:1200px; margin:0 auto; position:relative;}
canvas{background:#1e1e1e;border:2px solid #444;cursor:pointer;touch-action:none;display:block;width:100%;border-radius:8px;box-shadow: 0 4px 15px rgba(0,0,0,0.5);outline:none;}
.controls{background:#2a2a2a;padding:15px;border-radius:8px;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:12px;margin:0 auto 10px auto;border:1px solid #444;max-width:1200px;box-sizing:border-box;box-shadow: 0 2px 10px rgba(0,0,0,0.3);}
.control-row{display:flex; align-items:center; justify-content:center; gap:10px; flex-wrap:wrap;}
input,button,select{height:42px; padding:0 10px; border-radius:4px;border:none;background:#444;color:white;outline:none;font-size:14px;transition:0.2s;}
button{background:#00bfff;cursor:pointer;font-weight:bold;}
button:hover{background:#009cd1;transform:translateY(-1px);}
.btn-delete{background:#ff4444 !important;}
.btn-alt{background:#555 !important;}
.btn-new{background:#28a745 !important;}
.btn-play{background:#28a745 !important; width:50px;}
.btn-stop{background:#ffc107 !important; color:black !important; width:50px;}
.unsaved-container{height: 25px; margin-bottom:5px;}
.unsaved-text{color:#ff9900; font-weight:bold; font-size: 13px; display:none;}
#currentFileDisplay{color:#00ff00; font-weight:bold; margin-left:10px;}
textarea{width:100%; max-width:1200px; height:120px; background:#1e1e1e; color:#00ff00; border:1px solid #444; font-family:monospace; margin-top:10px; padding:10px; border-radius:8px; box-sizing:border-box;}
.slider-group{display:flex; align-items:center; gap:5px; background:#333; padding:0 10px; border-radius:4px; height:42px;}
#customModal{display:none; position:fixed; z-index:1000; left:0; top:0; width:100%; height:100%; background:rgba(0,0,0,0.85); backdrop-filter:blur(5px);}
.modal-content{background:#2a2a2a; margin:15% auto; padding:30px; border:1px solid #00bfff; width:300px; border-radius:16px;}
#modalInput{width:100%; margin-bottom:20px; text-align:center; display:none; background:#1e1e1e; border:1px solid #444; color:white; height:35px;}
.modal-btns{display:flex; justify-content:center; gap:15px;}
.help-text{font-size:11px; color:#888; margin-top:5px;}
.locked-info {color: #ff4444; font-size: 12px; font-weight: bold; margin-bottom: 5px; height: 15px;}
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
            <button id="playBtn" onclick="togglePause()" class="btn-play">▶</button>
            <button onclick="stopAndReset()" class="btn-stop">■</button>
            <span style="border-left:1px solid #444;height:30px;"></span>
            <label>Duur (sec):</label>
            <input type="number" id="totalTime" value="8" min="1" style="width:55px;" onchange="updateDuration()">
        </div>
        <div class="help-text">Klik: Punt toevoegen | Sleep: Kader | Del/Back: Wis | J: Join | X/Y: Lijn uit | Esc: Deselecteer</div>
    </div>
    <div class="locked-info" id="lockedStatus"></div>
    <div class="unsaved-container"><span id="unsavedWarning" class="unsaved-text">⚠️ NIET OPGESLAGEN</span></div>
    <div id="canvasContainer">
        <canvas id="envelopeCanvas" height="400" tabindex="1"></canvas>
    </div>
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
let selectedPoints=[];
let draggingPoint=null, lastMouseX=0, lastMouseY=0, historyStack = [];
let playStart=Date.now(), isUnsaved=false, currentOpenFile="";
let isDraggingPlayhead = false, manualTime = 0, ghostPoints = [], lastElapsed = 0, firstCycleDone = false, pausedTime = 0, isPaused = false; 
let isSelectingBox = false, hasMovedForBox = false, boxStart = {x:0, y:0}, boxEnd = {x:0, y:0};

function isLocked() { return !isPaused || pausedTime !== 0; }

function resizeCanvas() {
    const container = document.getElementById("canvasContainer");
    canvas.width = container.clientWidth;
}
window.addEventListener('resize', resizeCanvas);
resizeCanvas();

function saveToHistory() {
    historyStack.push(JSON.stringify(keyframes));
    if(historyStack.length > 30) historyStack.shift();
}

function undo() {
    if(isLocked()) return;
    if(historyStack.length > 0) {
        keyframes = JSON.parse(historyStack.pop());
        selectedPoints = []; markUnsaved(); sync();
    }
}

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

function updatePlayButtonUI() {
    const btn = document.getElementById("playBtn");
    btn.innerText = isPaused ? "▶" : "⏸";
}

function togglePause() {
    isPaused = !isPaused;
    updatePlayButtonUI();
    if(isPaused) pausedTime = (Date.now() - playStart) % keyframes[keyframes.length-1].time;
    else playStart = Date.now() - pausedTime;
    fetch('/toggle_pause?p=' + (isPaused ? "1" : "0"));
}

async function stopAndReset() {
    isPaused = true; updatePlayButtonUI(); pausedTime = 0; playStart = Date.now();
    fetch('/toggle_pause?p=1&reset=1');
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
    ghostPoints = source.map((p, i) => {
        if (i === 0 || i === source.length - 1) return { ...p };
        const prevT = source[i-1].time, nextT = source[i+1].time;
        const margin = (nextT - prevT) * 0.4 * chaos;
        let newTime = Math.round(Math.max(prevT + 10, Math.min(nextT - 10, p.time + (Math.random() - 0.5) * margin * 2)));
        let newValue = Math.max(-100, Math.min(100, p.value + (Math.random() - 0.5) * 25 * chaos));
        return { time: newTime, value: newValue };
    });
}

function markUnsaved(){ isUnsaved=true; document.getElementById("unsavedWarning").style.display="inline-block"; originalKeyframes = JSON.parse(JSON.stringify(keyframes)); }

function markSaved(name){ 
    isUnsaved = false; document.getElementById("unsavedWarning").style.display = "none"; 
    const sel = document.getElementById("presetSelect");
    let exists = false;
    for(let i=0; i<sel.options.length; i++) {
        if(sel.options[i].value === name) { exists = true; sel.selectedIndex = i; }
    }
    currentOpenFile = name; document.getElementById("currentFileDisplay").innerText = name ? " - " + name : "";
    originalKeyframes = JSON.parse(JSON.stringify(keyframes));
}

function getExportData() { 
    return { chaos: parseInt(document.getElementById("chaosSlider").value), keyframes: keyframes.map(kp => ({time: Math.round(kp.time), value: Math.round(kp.value * 100) / 100})) }; 
}

function sync(skipGhost=false){ 
    const data = getExportData();
    editor.value = JSON.stringify(data, null, 2);
    document.getElementById("totalTime").value = Math.round(keyframes[keyframes.length-1].time/1000);
    if(firstCycleDone && !skipGhost) generateGhost();
    return fetch('/set_live', {method:'POST', body:JSON.stringify(data)}); 
}

function draw(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    const locked = isLocked();
    canvas.style.cursor = locked ? "not-allowed" : "pointer";
    document.getElementById("lockedStatus").innerText = locked ? "LOCKED: Druk eerst op ■ om te editen" : "";
    ctx.strokeStyle="#333"; ctx.lineWidth=1; ctx.beginPath(); ctx.moveTo(0,canvas.height/2); ctx.lineTo(canvas.width,canvas.height/2); ctx.stroke();
    let dur=keyframes[keyframes.length-1].time, elapsed = isDraggingPlayhead ? manualTime : (isPaused ? pausedTime : (Date.now()-playStart)%dur);
    if (!isPaused && !isDraggingPlayhead && elapsed < lastElapsed) {
        if(firstCycleDone && ghostPoints.length > 0) { 
            keyframes = [...ghostPoints]; fetch('/set_live',{method:'POST',body:JSON.stringify(getExportData())}); 
        }
        firstCycleDone = true; generateGhost();
    }
    lastElapsed = elapsed;
    if (firstCycleDone && ghostPoints.length > 0) {
        ctx.strokeStyle="rgba(0, 191, 255, 0.2)"; ctx.lineWidth=2; ctx.setLineDash([5, 5]); ctx.beginPath();
        ghostPoints.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value))); ctx.stroke(); ctx.setLineDash([]);
    }
    ctx.strokeStyle="#00bfff"; ctx.lineWidth=3; ctx.beginPath();
    keyframes.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value))); ctx.stroke();
    keyframes.forEach(p => {
        const isSelected = selectedPoints.includes(p); ctx.beginPath(); ctx.arc(toX(p.time), toY(p.value), 6, 0, 7);
        ctx.fillStyle = isSelected ? "#ffffff" : "#ff9900"; ctx.fill();
        if(isSelected) { ctx.strokeStyle = "#00bfff"; ctx.lineWidth = 2; ctx.stroke(); }
    });
    if(isSelectingBox && hasMovedForBox) {
        ctx.fillStyle = "rgba(0, 191, 255, 0.2)"; ctx.strokeStyle = "#00bfff"; ctx.setLineDash([5, 5]);
        ctx.fillRect(boxStart.x, boxStart.y, boxEnd.x - boxStart.x, boxEnd.y - boxStart.y);
        ctx.strokeRect(boxStart.x, boxStart.y, boxEnd.x - boxStart.x, boxEnd.y - boxStart.y); ctx.setLineDash([]);
    }
    ctx.strokeStyle="red"; ctx.lineWidth=3; ctx.beginPath(); ctx.moveTo(toX(elapsed),0); ctx.lineTo(toX(elapsed),canvas.height); ctx.stroke();
    requestAnimationFrame(draw);
}

canvas.onmousedown=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=(e.clientX-rect.left)*(canvas.width/rect.width), y=e.clientY-rect.top;
    let dur=keyframes[keyframes.length-1].time, currentPos = isPaused ? pausedTime : (Date.now()-playStart)%dur;
    if(Math.abs(x - toX(currentPos)) < 30) { isDraggingPlayhead = true; manualTime = currentPos; return; }
    if(isLocked()) return; 
    lastMouseX = x; lastMouseY = y;
    let hit = keyframes.find(p=>Math.hypot(toX(p.time)-x,toY(p.value)-y)<15);
    if(hit) {
        saveToHistory();
        if(e.shiftKey) { if(selectedPoints.includes(hit)) selectedPoints = selectedPoints.filter(p => p !== hit); else selectedPoints.push(hit); } 
        else { if(!selectedPoints.includes(hit)) selectedPoints = [hit]; }
        draggingPoint = hit;
    } else { isSelectingBox = true; hasMovedForBox = false; boxStart = {x, y}; boxEnd = {x, y}; }
};

window.onmousemove=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=(e.clientX-rect.left)*(canvas.width/rect.width), y=e.clientY-rect.top;
    if(isDraggingPlayhead) { manualTime = Math.max(0, Math.min(keyframes[keyframes.length-1].time, fromX(x))); return; }
    if(isSelectingBox) { boxEnd = {x, y}; if(Math.hypot(x-boxStart.x, y-boxStart.y) > 5) hasMovedForBox = true; return; }
    if(!draggingPoint || isLocked()) return;
    const dx = fromX(x) - fromX(lastMouseX), dy = fromY(y) - fromY(lastMouseY);
    let canMoveX = true;
    selectedPoints.forEach(p => {
        let idx = keyframes.indexOf(p), newT = p.time + dx;
        if(idx === 0 || idx === keyframes.length-1) canMoveX = false;
        if(idx > 0 && !selectedPoints.includes(keyframes[idx-1]) && newT <= keyframes[idx-1].time + 10) canMoveX = false;
        if(idx < keyframes.length-1 && !selectedPoints.includes(keyframes[idx+1]) && newT >= keyframes[idx+1].time - 10) canMoveX = false;
    });
    selectedPoints.forEach(p => { if(canMoveX) p.time = Math.round(p.time + dx); p.value = Math.max(-100, Math.min(100, p.value + dy)); });
    lastMouseX = x; lastMouseY = y; keyframes.sort((a,b)=>a.time-b.time); markUnsaved(); sync();
};

window.onmouseup=(e)=>{ 
    if(isDraggingPlayhead) { if(isPaused) pausedTime = manualTime; else playStart = Date.now() - manualTime; fetch('/reset_clock_to?t=' + Math.round(manualTime)); } 
    if(isSelectingBox) {
        if(!hasMovedForBox) {
            saveToHistory(); const rect=canvas.getBoundingClientRect(), x=(e.clientX-rect.left)*(canvas.width/rect.width), y=e.clientY-rect.top;
            keyframes.push({time:Math.round(fromX(x)), value:fromY(y)}); keyframes.sort((a,b)=>a.time-b.time);
            if(!e.shiftKey) selectedPoints = []; markUnsaved(); sync();
        } else {
            const xMin = Math.min(boxStart.x, boxEnd.x), xMax = Math.max(boxStart.x, boxEnd.x), yMin = Math.min(boxStart.y, boxEnd.y), yMax = Math.max(boxStart.y, boxEnd.y);
            if(!e.shiftKey) selectedPoints = [];
            keyframes.forEach(p => { const px = toX(p.time), py = toY(p.value); if(px >= xMin && px <= xMax && py >= yMin && py <= yMax) if(!selectedPoints.includes(p)) selectedPoints.push(p); });
        }
        isSelectingBox = false;
    }
    draggingPoint=null; isDraggingPlayhead = false; 
};

window.onkeydown=(e)=>{
    if(e.ctrlKey && e.key === 'z') { e.preventDefault(); undo(); return; }
    if(e.key === "Escape" || e.key === "Esc") { selectedPoints = []; return; }
    if(isLocked() || selectedPoints.length === 0 || document.activeElement === editor) return;
    let changed = false; const key = e.key.toLowerCase();
    if(e.key === "Delete" || e.key === "Backspace") {
        saveToHistory(); keyframes = keyframes.filter((p, index) => (index === 0 || index === keyframes.length - 1) ? true : !selectedPoints.includes(p));
        selectedPoints = []; changed = true;
    }
    if(key === "j" && selectedPoints.length >= 2) {
        saveToHistory(); const avgV = selectedPoints.reduce((acc, p) => acc + p.value, 0) / selectedPoints.length, isStart = selectedPoints.some(p => keyframes.indexOf(p) === 0), isEnd = selectedPoints.some(p => keyframes.indexOf(p) === keyframes.length - 1);
        keyframes = keyframes.filter(p => !selectedPoints.includes(p));
        if(isStart) keyframes.push({time: 0, value: avgV}); if(isEnd) keyframes.push({time: Math.max(...keyframes.map(p=>p.time)), value: avgV});
        if(!isStart && !isEnd) keyframes.push({time: Math.round(selectedPoints.reduce((a,p)=>a+p.time,0)/selectedPoints.length), value: avgV});
        selectedPoints = []; changed = true;
    }
    if(key === "y" || key === "x") {
        saveToHistory(); const avg = key === "y" ? Math.round(selectedPoints.reduce((a,p)=>a+p.time,0)/selectedPoints.length) : selectedPoints.reduce((a,p)=>a+p.value,0)/selectedPoints.length;
        selectedPoints.forEach(p => { if(key === "y") { if(keyframes.indexOf(p)>0 && keyframes.indexOf(p)<keyframes.length-1) p.time = avg; } else p.value = avg; });
        changed = true;
    }
    if(changed) { e.preventDefault(); keyframes.sort((a,b)=>a.time-b.time); markUnsaved(); sync(); }
};

async function autoLoadPreset(targetName){
    const name = targetName || document.getElementById("presetSelect").value;
    if(!name || name === "") return;
    if(isUnsaved && !targetName && !await openModal("Wijzigingen gaan verloren. Doorgaan?")){ document.getElementById("presetSelect").value = currentOpenFile || ""; return; }
    const res = await fetch('/load?name='+name); if(!res.ok) return;
    const data = await res.json(); keyframes = data.keyframes || data; document.getElementById("chaosSlider").value = data.chaos || 0;
    updateChaosLabel(); firstCycleDone = false; ghostPoints = []; selectedPoints = []; historyStack = []; markSaved(name); await sync(true); stopAndReset();
}

async function updatePresetList(targetName){
    const res = await fetch('/list'), list = await res.json(), sel = document.getElementById("presetSelect"); sel.innerHTML = "";
    list.forEach(f=>{ let o=document.createElement("option"); o.value=o.textContent=f; if(f === targetName) o.selected = true; sel.appendChild(o); });
    if(!targetName) sel.selectedIndex = -1; return list;
}

// DELETE FUNCTION RESTORED
async function deletePreset(){
    const name = document.getElementById("presetSelect").value;
    if(!name || name === "" || !await openModal("Verwijder '"+name+"' definitief?")) return;
    await fetch('/delete?name='+name, {method:'DELETE'});
    currentOpenFile = ""; const newList = await updatePresetList();
    if(newList.length > 0) autoLoadPreset(newList[0]); else createNew();
}

function createNew(){ keyframes=[{time:0,value:0},{time:8000,value:0}]; selectedPoints=[]; historyStack=[]; markSaved(""); updatePresetList(); sync(true); stopAndReset(); }
function performSave(n){ fetch('/save?name='+n,{method:'POST',body:JSON.stringify(getExportData())}).then(()=>{markSaved(n);updatePresetList(n);}); }
async function saveCurrent(){ if(!currentOpenFile) saveAs(); else if(await openModal("Bestand '" + currentOpenFile + "' overschrijven?")) performSave(currentOpenFile); }
async function saveAs(){ let n=await openModal("Sla configuratie op als:", true); if(n && typeof n === 'string') performSave(n.trim().replace(/[^a-z0-9_-]/gi,'_')); }

async function handleFileUpload(event){
    const file=event.target.files[0]; if(!file) return;
    const reader=new FileReader();
    reader.onload=async (ev)=>{
        try{
            const data=JSON.parse(ev.target.result); let name=file.name.replace(".json","").replace(/[^a-z0-9_-]/gi,'_');
            const list = await (await fetch('/list')).json();
            if(list.includes(name) && !await openModal("Bestand '"+name+"' bestaat al op de ESP. Overschrijven?")) return;
            await fetch('/save?name='+name,{method:'POST',body:JSON.stringify(data)});
            keyframes=data.keyframes || data; selectedPoints=[]; historyStack=[]; markSaved(name); await updatePresetList(name); await sync(true); stopAndReset();
        }catch(err){alert("Fout");}
    };
    reader.readAsText(file); event.target.value="";
}

function downloadConfig(){ const a=document.createElement('a'); a.href="data:text/json;charset=utf-8,"+encodeURIComponent(JSON.stringify(getExportData(),null,2)); a.download=(currentOpenFile || "config")+".json"; a.click(); }
function updateDuration(){ 
    if(!isLocked()) saveToHistory(); const oldDur = keyframes[keyframes.length-1].time, newDur = document.getElementById("totalTime").value * 1000;
    const factor = newDur / oldDur; keyframes.forEach(p => { p.time = Math.round(p.time * factor); }); markUnsaved(); sync(); 
}
function applyJson(){ if(isLocked()) return; try{const data=JSON.parse(editor.value); keyframes=data.keyframes || data; markUnsaved(); sync();}catch(e){} }

async function init(){
    const [dataRes, nameRes, listRes, pauseRes] = await Promise.all([ fetch('/get_active'), fetch('/get_active_name'), fetch('/list'), fetch('/get_pause_state') ]);
    const data = await dataRes.json(), activeName = await nameRes.text(), list = await listRes.json();
    isPaused = (await pauseRes.text() == "1"); updatePlayButtonUI();
    keyframes = data.keyframes || data; document.getElementById("chaosSlider").value = data.chaos || 0; updateChaosLabel();
    const validatedName = list.includes(activeName) ? activeName : ""; await updatePresetList(validatedName); markSaved(validatedName);
    playStart = Date.now() - parseInt(await (await fetch('/get_time')).text()); sync(true); draw();
}
init();
</script>
</body>
</html>
)rawliteral";

void loadFromJSON(String json) {
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, json)) return;
  JsonArray arr = (doc.is<JsonObject>() && doc.containsKey("keyframes")) ? doc["keyframes"].as<JsonArray>() : doc.as<JsonArray>();
  envelopeSize = min((int)arr.size(), 50);
  for (int i = 0; i < envelopeSize; i++) { envelope[i].time = arr[i]["time"]; envelope[i].value = (float)arr[i]["value"]; }
  loopDuration = (envelopeSize > 1) ? envelope[envelopeSize - 1].time : 8000;
  currentSegment = 0;
}

void setup() {
  Serial.begin(115200); for (int i = 0; i < 4; i++) pinMode(motorPins[i], OUTPUT); LittleFS.begin(true);
  if (LittleFS.exists("/active.json")) loadFromJSON(LittleFS.open("/active.json", "r").readString());
  isPaused = false; startTime = millis();
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(ap_ssid, ap_pass); WiFi.begin(ssid_home, pass_home); MDNS.begin(mdns_name);
  server.on("/", [](){ server.send(200, "text/html", htmlPage); });
  server.on("/get_time", [](){ server.send(200, "text/plain", String((millis() - startTime) % loopDuration)); });
  server.on("/get_pause_state", [](){ server.send(200, "text/plain", isPaused ? "1" : "0"); });
  server.on("/toggle_pause", [](){ if(server.hasArg("p")) isPaused = (server.arg("p") == "1"); if(server.hasArg("reset")) { startTime = millis(); currentSegment = 0; } server.send(200); });
  server.on("/set_live", HTTP_POST, [](){ if(server.hasArg("plain")) { loadFromJSON(server.arg("plain")); File f = LittleFS.open("/active.json", "w"); f.print(server.arg("plain")); f.close(); } server.send(200); });
  server.on("/save", HTTP_POST, [](){
    String name = server.arg("name"), data = server.arg("plain");
    File f1 = LittleFS.open("/" + name + ".json", "w"); f1.print(data); f1.close();
    File f2 = LittleFS.open("/active.json", "w"); f2.print(data); f2.close();
    File f3 = LittleFS.open("/active_name.txt", "w"); f3.print(name); f3.close(); server.send(200);
  });
  server.on("/load", HTTP_GET, [](){ String name = server.arg("name"); File f = LittleFS.open("/" + name + ".json", "r"); if(f){ server.send(200, "application/json", f.readString()); f.close(); } else server.send(404); });
  
  // DELETE ROUTE RESTORED
  server.on("/delete", HTTP_DELETE, [](){ 
    String name = server.arg("name"); 
    if(LittleFS.exists("/"+name+".json")) LittleFS.remove("/"+name+".json");
    server.send(200); 
  });

  server.on("/list", HTTP_GET, [](){
    String list = "["; File root = LittleFS.open("/"); File file = root.openNextFile();
    while (file) { String n = file.name(); if (n.endsWith(".json") && n != "active.json") { if (list != "[") list += ","; list += "\"" + n.substring(0, n.length() - 5) + "\""; } file = root.openNextFile(); }
    list += "]"; server.send(200, "application/json", list);
  });
  server.on("/get_active", HTTP_GET, [](){ File f = LittleFS.open("/active.json", "r"); if(f) { server.send(200, "application/json", f.readString()); f.close(); } else server.send(200, "application/json", "[]"); });
  server.on("/get_active_name", HTTP_GET, [](){ if (LittleFS.exists("/active_name.txt")) { File f = LittleFS.open("/active_name.txt", "r"); String n = f.readString(); f.close(); server.send(200, "text/plain", n); } else server.send(200, "text/plain", ""); });
  server.on("/reset_clock_to", [](){ if(server.hasArg("t")) { startTime = millis() - server.arg("t").toInt(); currentSegment = 0; } server.send(200); });
  server.begin();
}

void loop() {
  server.handleClient(); if (isPaused) { for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], LOW); return; }
  unsigned long t = (millis() - startTime) % loopDuration; if (t < envelope[currentSegment].time) currentSegment = 0;
  while (currentSegment < envelopeSize - 1 && t > envelope[currentSegment + 1].time) currentSegment++;
  float f = (float)(t - envelope[currentSegment].time) / (envelope[currentSegment + 1].time - envelope[currentSegment].time);
  float currentVal = envelope[currentSegment].value + f * (envelope[currentSegment + 1].value - envelope[currentSegment].value);
  float speedFactor = abs(currentVal) / 100.0;
  if (speedFactor > 0.02) {
    unsigned long stepInterval = 1000000.0 / (speedFactor * MAX_STEPS_PER_SEC);
    if (micros() - lastStepMicros >= stepInterval) {
      lastStepMicros = micros(); if (currentVal > 0) stepIndex = (stepIndex + 1) % 8; else stepIndex = (stepIndex + 7) % 8;
      for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], steps[stepIndex][i]);
    }
  } else { for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], LOW); }
}