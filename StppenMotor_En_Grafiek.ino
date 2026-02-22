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
<title>Stepper Pro v22.6 - Auto-Play</title>
<style>
  :root {
    --bg-dark: #121212;
    --bg-panel: #2a2a2a;
    --accent: #00bfff;
    --accent-hover: #009cd1;
    --danger: #ff4444;
    --success: #28a745;
    --warning: #ff9900;
    --text: #eee;
    --border: #444;
  }

  body { background: var(--bg-dark); color: var(--text); font-family: sans-serif; text-align: center; margin: 0; padding: 20px; font-weight: normal; }
  #canvasContainer { width: 100%; max-width: 1200px; margin: 20px auto; position: relative; }
  canvas { background: #1e1e1e; border: 2px solid var(--border); cursor: pointer; touch-action: none; display: block; width: 100%; border-radius: 8px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); outline: none; }
  
  .controls { background: var(--bg-panel); padding: 15px; border-radius: 8px; display: flex; flex-direction: column; align-items: center; gap: 12px; margin: 0 auto; border: 1px solid var(--border); max-width: 1200px; box-sizing: border-box; box-shadow: 0 2px 10px rgba(0,0,0,0.3); transition: box-shadow 0.3s ease; }
  .control-row { display: flex; align-items: center; justify-content: center; gap: 10px; flex-wrap: wrap; }
  
  /* Rood gloei effect voor niet opgeslagen wijzigingen */
  .unsaved-glow { 
    border-color: var(--danger) !important;
    box-shadow: 0 0 15px rgba(255, 68, 68, 0.4);
    animation: pulse-red 2s infinite;
  }
  @keyframes pulse-red {
    0% { box-shadow: 0 0 5px rgba(255, 68, 68, 0.2); }
    50% { box-shadow: 0 0 20px rgba(255, 68, 68, 0.6); }
    100% { box-shadow: 0 0 5px rgba(255, 68, 68, 0.2); }
  }

  input, button, select { height: 42px; padding: 0 10px; border-radius: 4px; border: none; background: var(--border); color: white; outline: none; font-size: 14px; transition: 0.2s; font-weight: normal; }
  button { background: var(--accent); cursor: pointer; }
  button:hover { background: var(--accent-hover); transform: translateY(-1px); }
  
  .btn-delete { background: var(--danger) !important; }
  .btn-alt { background: #555 !important; }
  .btn-new { background: var(--success) !important; }
  .btn-play { background: var(--success) !important; width: 50px; }
  .btn-stop { background: #ffc107 !important; color: black !important; width: 50px; }

  #currentFileDisplay { color: #00ff00; margin-left: 10px; }
  
  textarea { width: 100%; max-width: 1200px; height: 120px; background: #1e1e1e; color: #00ff00; border: 1px solid var(--border); font-family: monospace; margin: 20px auto 0 auto; padding: 10px; border-radius: 8px; box-sizing: border-box; display: block; }
  .slider-group { display: flex; align-items: center; gap: 5px; background: #333; padding: 0 10px; border-radius: 4px; height: 42px; }
  
  .locked-ui { opacity: 0.5; pointer-events: none; filter: grayscale(50%); }
  .locked-canvas { pointer-events: none; }

  label { font-size: 14px; }
  #customModal { display: none; position: fixed; z-index: 1000; left: 0; top: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.85); backdrop-filter: blur(5px); }
  .modal-content { background: var(--bg-panel); margin: 15% auto; padding: 30px; border: 1px solid var(--accent); width: 300px; border-radius: 16px; }
</style>
</head>
<body>
    <h2 style="margin-bottom:10px;">Motor Control Pro <span id="currentFileDisplay"></span></h2>
    
    <div id="mainControls" class="controls">
        <div id="fileControls" class="control-row">
            <button onclick="createNew()" class="btn-new" title="Nieuwe configuratie">➕</button>
            <select id="presetSelect" onchange="autoLoadPreset()"></select>
            <button onclick="deletePreset()" class="btn-delete" title="Verwijder preset">🗑️</button>
            <span style="border-left:1px solid var(--border);height:30px;"></span>
            <button onclick="saveCurrent()">Opslaan</button>
            <button onclick="saveAs()" class="btn-alt">Opslaan als...</button>
            <span style="border-left:1px solid var(--border);height:30px;"></span>
            <button onclick="downloadConfig()" class="btn-alt" title="Download naar PC">💾</button>
            <button onclick="document.getElementById('fileInput').click()" class="btn-alt" title="Upload van PC">📂</button>
            <input type="file" id="fileInput" style="display:none" onchange="handleFileUpload(event)" accept=".json">
        </div>
        
        <div class="control-row">
            <div class="slider-group">
                <label style="font-size:12px;">Chaos</label>
                <input type="range" id="chaosSlider" min="0" max="100" value="0" oninput="updateChaosLabel(); markUnsaved(); sync(true);">
                <span id="chaosVal" style="font-size:12px;width:30px;">0%</span>
            </div>
            <span style="border-left:1px solid var(--border);height:30px;"></span>
            <button id="playBtn" onclick="togglePause()" class="btn-play">▶</button>
            <button onclick="stopAndReset()" class="btn-stop">■</button>
            <span style="border-left:1px solid var(--border);height:30px;"></span>
            <label>Duur (sec):</label>
            <input type="number" id="totalTime" value="8" min="0.1" step="0.1" style="width:70px;" onchange="updateDuration()">
        </div>
    </div>
    
    <div id="canvasContainer">
        <canvas id="envelopeCanvas" height="400" tabindex="1"></canvas>
    </div>
    
    <textarea id="jsonEditor" spellcheck="false" oninput="applyJson()"></textarea>

    <div id="customModal">
        <div class="modal-content">
            <div id="modalText" style="margin-bottom:15px;"></div>
            <input type="text" id="modalInput" style="width:100%; margin-bottom:20px; text-align:center; display:none; background:#1e1e1e; border:1px solid var(--border); color:white; height:35px;">
            <div class="modal-btns" style="display:flex; justify-content:center; gap:15px;">
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
let ghostPoints = [], lastElapsed = 0, firstCycleDone = false, pausedTime = 0, isPaused = false; 
let isSelectingBox = false, hasMovedForBox = false, boxStart = {x:0, y:0}, boxEnd = {x:0, y:0};

function isLocked() { return !isPaused; }

function updateUIState() {
    const isPlaying = !isPaused;
    document.getElementById("fileControls").classList.toggle("locked-ui", isPlaying);
    document.getElementById("jsonEditor").classList.toggle("locked-ui", isPlaying);
    document.getElementById("envelopeCanvas").classList.toggle("locked-canvas", isPlaying);
}

function resizeCanvas() { const container = document.getElementById("canvasContainer"); canvas.width = container.clientWidth; }
window.addEventListener('resize', resizeCanvas); resizeCanvas();

function saveToHistory() { historyStack.push(JSON.stringify(keyframes)); if(historyStack.length > 30) historyStack.shift(); }
function undo() { if(isLocked()) return; if(historyStack.length > 0) { keyframes = JSON.parse(historyStack.pop()); selectedPoints = []; markUnsaved(); sync(); } }

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
    updateUIState();
}

function togglePause() { 
    isPaused = !isPaused; 
    updatePlayButtonUI(); 
    if(isPaused) pausedTime = (Date.now() - playStart) % keyframes[keyframes.length-1].time; 
    else playStart = Date.now() - pausedTime; 
    fetch('/toggle_pause?p=' + (isPaused ? "1" : "0")); 
}

async function stopAndReset() { 
    isPaused = true; 
    updatePlayButtonUI(); 
    pausedTime = 0; 
    playStart = Date.now(); 
    if(originalKeyframes.length > 0) { keyframes = JSON.parse(JSON.stringify(originalKeyframes)); }
    ghostPoints = [];
    firstCycleDone = false;
    sync(true); 
    fetch('/toggle_pause?p=1&reset=1'); 
}

function updateChaosLabel(){ document.getElementById("chaosVal").innerText = document.getElementById("chaosSlider").value + "%"; }

const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function generateGhost() {
    const chaos = document.getElementById("chaosSlider").value / 100;
    if(chaos <= 0 || isPaused) { ghostPoints = []; return; }
    const source = originalKeyframes.length > 0 ? originalKeyframes : keyframes;
    ghostPoints = source.map((p, i) => {
        if (i === 0 || i === source.length - 1) return { ...p };
        const prevT = source[i-1].time, nextT = source[i+1].time;
        const safeSpanLeft = (p.time - prevT) * 0.9;
        const safeSpanRight = (nextT - p.time) * 0.9;
        const maxShiftT = Math.min(safeSpanLeft, safeSpanRight) * chaos;
        let newTime = Math.round(p.time + (Math.random() - 0.5) * maxShiftT * 2);
        let newValue = Math.max(-100, Math.min(100, p.value + (Math.random() - 0.5) * 40 * chaos));
        return { time: newTime, value: newValue };
    });
}

function markUnsaved(){ 
    isUnsaved=true; 
    document.getElementById("mainControls").classList.add("unsaved-glow");
    originalKeyframes = JSON.parse(JSON.stringify(keyframes)); 
}

function markSaved(name){ 
    isUnsaved = false; 
    document.getElementById("mainControls").classList.remove("unsaved-glow");
    const sel = document.getElementById("presetSelect");
    for(let i=0; i<sel.options.length; i++) { if(sel.options[i].value === name) sel.selectedIndex = i; }
    currentOpenFile = name; document.getElementById("currentFileDisplay").innerText = name ? " - " + name : "";
    originalKeyframes = JSON.parse(JSON.stringify(keyframes));
}

function getExportData() { return { chaos: parseInt(document.getElementById("chaosSlider").value), keyframes: keyframes.map(kp => ({time: Math.round(kp.time), value: Math.round(kp.value * 100) / 100})) }; }
function sync(skipGhost=false){ 
    const data = getExportData();
    editor.value = JSON.stringify(data, null, 2);
    document.getElementById("totalTime").value = (keyframes[keyframes.length-1].time/1000).toFixed(1);
    if(firstCycleDone && !skipGhost) generateGhost();
    return fetch('/set_live', {method:'POST', body:JSON.stringify(data)}); 
}

function draw(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    ctx.strokeStyle="#333"; ctx.lineWidth=1; ctx.beginPath(); ctx.moveTo(0,canvas.height/2); ctx.lineTo(canvas.width,canvas.height/2); ctx.stroke();
    let dur=keyframes[keyframes.length-1].time, elapsed = (isPaused ? pausedTime : (Date.now()-playStart)%dur);
    if (!isPaused && elapsed < lastElapsed) {
        if(firstCycleDone && ghostPoints.length > 0) { keyframes = [...ghostPoints]; fetch('/set_live',{method:'POST',body:JSON.stringify(getExportData())}); }
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
    if(isLocked()) return; 
    const rect=canvas.getBoundingClientRect(), x=(e.clientX-rect.left)*(canvas.width/rect.width), y=e.clientY-rect.top;
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
    if(isSelectingBox) { const rect=canvas.getBoundingClientRect(), x=(e.clientX-rect.left)*(canvas.width/rect.width), y=e.clientY-rect.top; boxEnd = {x, y}; if(Math.hypot(x-boxStart.x, y-boxStart.y) > 5) hasMovedForBox = true; return; }
    if(!draggingPoint || isLocked()) return;
    const rect=canvas.getBoundingClientRect(), x=(e.clientX-rect.left)*(canvas.width/rect.width), y=e.clientY-rect.top;
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
    draggingPoint=null; 
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
    if(!name || name === "" || isLocked()) return;
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

async function deletePreset(){
    const name = document.getElementById("presetSelect").value;
    if(!name || name === "" || isLocked() || !await openModal("Verwijder '"+name+"' definitief?")) return;
    await fetch('/delete?name='+name, {method:'DELETE'});
    currentOpenFile = ""; const newList = await updatePresetList();
    if(newList.length > 0) autoLoadPreset(newList[0]); else createNew();
}

function createNew(){ if(isLocked()) return; keyframes=[{time:0,value:0},{time:8000,value:0}]; selectedPoints=[]; historyStack=[]; markSaved(""); updatePresetList(); sync(true); stopAndReset(); }
function performSave(n){ fetch('/save?name='+n,{method:'POST',body:JSON.stringify(getExportData())}).then(()=>{markSaved(n);updatePresetList(n);}); }
async function saveCurrent(){ if(isLocked()) return; if(!currentOpenFile) saveAs(); else if(await openModal("Bestand '" + currentOpenFile + "' overschrijven?")) performSave(currentOpenFile); }
async function saveAs(){ if(isLocked()) return; let n=await openModal("Sla configuratie op als:", true); if(n && typeof n === 'string') performSave(n.trim().replace(/[^a-z0-9_-]/gi,'_')); }

async function handleFileUpload(event){
    if(isLocked()) return;
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
    saveToHistory(); const oldDur = keyframes[keyframes.length-1].time, newDur = document.getElementById("totalTime").value * 1000;
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
  Serial.begin(115200); 
  for (int i = 0; i < 4; i++) pinMode(motorPins[i], OUTPUT); 
  LittleFS.begin(true);

  if (LittleFS.exists("/active.json")) {
    loadFromJSON(LittleFS.open("/active.json", "r").readString());
  }
  
  isPaused = false; 
  startTime = millis();

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
    File f3 = LittleFS.open("/active_name.txt", "w"); f3.print(name); f3.close(); 
    server.send(200);
  });

  server.on("/load", HTTP_GET, [](){ String name = server.arg("name"); File f = LittleFS.open("/" + name + ".json", "r"); if(f){ server.send(200, "application/json", f.readString()); f.close(); } else server.send(404); });
  server.on("/delete", HTTP_DELETE, [](){ String name = server.arg("name"); if(LittleFS.exists("/"+name+".json")) LittleFS.remove("/"+name+".json"); server.send(200); });
  
  server.on("/list", HTTP_GET, [](){
    String list = "["; File root = LittleFS.open("/"); File file = root.openNextFile();
    while (file) { String n = file.name(); if (n.endsWith(".json") && n != "active.json") { if (list != "[") list += ","; list += "\"" + n.substring(0, n.length() - 5) + "\""; } file = root.openNextFile(); }
    list += "]"; server.send(200, "application/json", list);
  });

  server.on("/get_active", HTTP_GET, [](){ File f = LittleFS.open("/active.json", "r"); if(f) { server.send(200, "application/json", f.readString()); f.close(); } else server.send(200, "application/json", "{\"chaos\":0,\"keyframes\":[{\"time\":0,\"value\":0},{\"time\":8000,\"value\":0}]}"); });
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