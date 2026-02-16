#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// --- WiFi Instellingen ---
const char* ssid_home = WIFI_TP_SSID;
const char* pass_home = WIFI_TP_PASSWORD;
const char* ap_ssid = "Motor-Control-Pro";
const char* ap_pass = "12345678"; 

WebServer server(80);

// --- Motor Config ---
const int motorPins[] = {14, 12, 13, 15}; 
int stepIndex = 0;
unsigned long lastStepMicros = 0;
const float MAX_STEPS_PER_SEC = 800.0;
const int steps[8][4] = {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,1,1},{0,0,0,1},{1,0,0,1}};

// --- Envelope Data ---
struct Keyframe { unsigned long time; float value; };
Keyframe envelope[50];
int envelopeSize = 2;
unsigned long startTime = 0;
unsigned long loopDuration = 8000;

// --- HTML / Interface ---
const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stepper Pro v15 - Mediaraad</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:20px;}
canvas{background:#1e1e1e;border:2px solid #444;cursor:crosshair;touch-action:none;display:block;margin:20px auto;border-radius:8px;}
.controls{background:#2a2a2a;padding:15px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;gap:10px;margin-bottom:10px;border:1px solid #444;flex-wrap:wrap;}
input,button,select{height:42px; padding:0 10px; border-radius:4px;border:none;background:#444;color:white;outline:none;font-size:14px;}
button{background:#00bfff;cursor:pointer;font-weight:bold;}
button:hover{background:#009cd1;}
.sync-indicator{width:12px; height:12px; border-radius:50%; background:#444; display:inline-block; margin-right:5px;}
.sync-active{background:#00ff00; box-shadow: 0 0 8px #00ff00;}
.status-bar{color:#00bfff;font-family:monospace;margin-bottom:10px;font-size:1.1em;}
.unsaved{color:#ff9900 !important; font-weight:bold;}
textarea{width:800px; height:150px; background:#1e1e1e; color:#00ff00; border:1px solid #444; font-family:monospace; padding:10px; border-radius:4px; margin-top:10px;}
.footer{margin-top:20px; opacity:0.5; font-size:13px;}
</style>
</head>
<body>
    <h2>Motor Envelope Control <small>v15</small></h2>
    <div class="status-bar">
        <div id="syncDot" class="sync-indicator"></div>
        Sync Status: <span id="syncText">Gekoppeld</span> | Preset: <span id="curStatus">-</span>
    </div>
    
    <div class="controls">
        <input type="text" id="filename" placeholder="Naam preset" style="width:120px;">
        <label>Duur (s):</label>
        <input type="number" id="totalTime" value="8" min="1" style="width:65px;" onchange="updateDuration()">
        <button onclick="savePreset()">Opslaan</button>
        <button onclick="hardResetSync()" style="background:#ff9900;">Reset Sync</button>
        <select id="presetSelect" onchange="autoLoadPreset()"><option>Laden...</option></select>
    </div>

    <canvas id="envelopeCanvas" width="800" height="400"></canvas>
    <textarea id="jsonEditor" spellcheck="false" oninput="applyJson()"></textarea>

    <div class="footer">
        &copy; <a href='https://github.com/mediaraad' target='_blank' style='color:#00bfff; text-decoration:none;'>Mediaraad</a>
    </div>

<script>
const canvas=document.getElementById("envelopeCanvas"), ctx=canvas.getContext("2d"), editor=document.getElementById("jsonEditor");
let keyframes=[{time:0,value:0},{time:8000,value:0}], selected=null, playStart=Date.now(), lastAutoSync=0;
let currentLoadedName = "";

const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function markUnsaved() {
    const txt = document.getElementById("syncText");
    txt.innerHTML = "⚠️ Niet opgeslagen!";
    txt.classList.add("unsaved");
}

function markSaved() {
    const txt = document.getElementById("syncText");
    txt.innerHTML = "Gekoppeld";
    txt.classList.remove("unsaved");
}

function hardResetSync() {
    playStart = Date.now();
    fetch('/reset_clock');
    flashSyncDot();
}

function flashSyncDot() {
    const dot = document.getElementById("syncDot");
    dot.classList.add("sync-active");
    setTimeout(() => dot.classList.remove("sync-active"), 200);
}

function updateDuration(){
    keyframes[keyframes.length-1].time = document.getElementById("totalTime").value * 1000;
    markUnsaved();
    sync();
}

function draw(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    ctx.strokeStyle="#333"; ctx.beginPath(); ctx.moveTo(0,canvas.height/2); ctx.lineTo(canvas.width,canvas.height/2); ctx.stroke();
    ctx.strokeStyle="#00bfff"; ctx.lineWidth=3; ctx.beginPath();
    keyframes.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value)));
    ctx.stroke();
    ctx.fillStyle="#ff9900";
    keyframes.forEach(p=>{ctx.beginPath();ctx.arc(toX(p.time),toY(p.value),6,0,7);ctx.fill();});
    let duration = keyframes[keyframes.length-1].time;
    let now = Date.now();
    let elapsed = (now - playStart) % duration;
    if (elapsed < 50 && (now - lastAutoSync > 1000)) { 
        fetch('/reset_clock');
        lastAutoSync = now;
        flashSyncDot();
    }
    ctx.strokeStyle="red"; ctx.lineWidth=2; ctx.beginPath();
    ctx.moveTo(toX(elapsed),0); ctx.lineTo(toX(elapsed),canvas.height); ctx.stroke();
    requestAnimationFrame(draw);
}

canvas.onmousedown=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left, y=e.clientY-rect.top;
    selected=keyframes.find(p=>Math.hypot(toX(p.time)-x,toY(p.value)-y)<15);
    if(!selected){
        keyframes.push({time:fromX(x),value:fromY(y)});
        keyframes.sort((a,b)=>a.time-b.time);
        markUnsaved();
        sync();
    }
};

window.onmousemove=(e)=>{
    if(!selected)return;
    const rect=canvas.getBoundingClientRect();
    if(selected!==keyframes[0] && selected!==keyframes[keyframes.length-1]) selected.time=Math.max(1, fromX(e.clientX-rect.left));
    selected.value=Math.max(-100,Math.min(100,fromY(e.clientY-rect.top)));
    keyframes.sort((a,b)=>a.time-b.time);
    markUnsaved();
    sync();
};
window.onmouseup=()=>selected=null;

function sync(){ 
    editor.value = JSON.stringify(keyframes, null, 2);
    document.getElementById("totalTime").value = Math.round(keyframes[keyframes.length-1].time / 1000);
    fetch('/set_live',{method:'POST',body:JSON.stringify(keyframes)}); 
}

function applyJson(){
    try { keyframes = JSON.parse(editor.value); markUnsaved(); sync(); } catch(e){}
}

function savePreset(){
    const name = document.getElementById("filename").value;
    if(!name) return alert("Naam verplicht");
    fetch('/save?name='+name, {method:'POST', body:JSON.stringify(keyframes)}).then(()=>{
        currentLoadedName = name;
        updatePresetList(name);
        markSaved();
    });
}

function autoLoadPreset(){
    const sel = document.getElementById("presetSelect");
    const name = sel.value;
    
    // Check op ongeslagen wijzigingen
    if(document.getElementById("syncText").classList.contains("unsaved")) {
        if(!confirm("Je hebt ongeslagen wijzigingen. Doorgaan met laden van '" + name + "'?")) {
            sel.value = currentLoadedName; // Zet dropdown terug naar de huidige preset
            return;
        }
    }
    
    fetch('/load?name='+name).then(r=>r.json()).then(data=>{
        keyframes = data; 
        currentLoadedName = name;
        sync(); 
        hardResetSync(); 
        markSaved();
    });
}

async function updatePresetList(targetName = null){
    const list = await fetch('/list').then(r=>r.json());
    if(!targetName) targetName = await fetch('/get_active_name').then(r=>r.text());
    currentLoadedName = targetName;
    const sel = document.getElementById("presetSelect");
    sel.innerHTML = list.length ? "" : "<option>Geen presets</option>";
    list.forEach(f => {
        let opt = document.createElement("option"); opt.value = f; opt.textContent = f;
        if(f === targetName) opt.selected = true;
        sel.appendChild(opt);
    });
    document.getElementById("curStatus").textContent = targetName || "Geen";
}

fetch('/get_active').then(r=>r.json()).then(data=>{
    if(data.length > 0) keyframes = data;
    sync(); updatePresetList(); draw();
});
</script>
</body>
</html>
)rawliteral";

// --- C++ Backend (Onveranderd) ---

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

  server.on("/", []() { server.send(200, "text/html", htmlPage); });
  
  server.on("/reset_clock", [](){
    startTime = millis();
    server.send(200, "text/plain", "Synced");
  });

  server.on("/set_live", HTTP_POST, [](){
    if(server.hasArg("plain")) loadFromJSON(server.arg("plain"));
    server.send(200);
  });
  
  server.on("/save", HTTP_POST, [](){
    String name = server.arg("name"); String data = server.arg("plain");
    File f1 = LittleFS.open("/" + name + ".json", "w"); f1.print(data); f1.close();
    File f2 = LittleFS.open("/active.json", "w"); f2.print(data); f2.close();
    File f3 = LittleFS.open("/active_name.txt", "w"); f3.print(name); f3.close();
    startTime = millis();
    server.send(200);
  });

  server.on("/load", HTTP_GET, [](){
    String name = server.arg("name");
    File file = LittleFS.open("/" + name + ".json", "r");
    if (file) {
      String content = file.readString(); file.close();
      loadFromJSON(content);
      File f2 = LittleFS.open("/active.json", "w"); f2.print(content); f2.close();
      File f3 = LittleFS.open("/active_name.txt", "w"); f3.print(name); f3.close();
      startTime = millis();
      server.send(200, "application/json", content);
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
      File f = LittleFS.open("/active_name.txt", "r");
      String n = f.readString(); f.close(); server.send(200, "text/plain", n);
    } else server.send(200, "text/plain", "Geen");
  });

  server.begin();
  startTime = millis();
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
  } else {
    for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], LOW);
  }
}