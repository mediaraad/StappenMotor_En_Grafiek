#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

const char* ssid     = WIFI_TP_SSID;
const char* password = WIFI_TP_PASSWORD;
WebServer server(80);

// --- Motor ---
const int motorPins[] = {14, 12, 13, 15}; 
int stepIndex = 0;
unsigned long lastStepMicros = 0;
const float MAX_STEPS_PER_SEC = 800.0;

const int steps[8][4] = {
  {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
  {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
};

// --- Envelope Data ---
struct Keyframe {
  unsigned long time; 
  int value;          
};
Keyframe envelope[50];
int envelopeSize = 2;
unsigned long startTime = 0;
unsigned long loopDuration = 8000; // Standaard 8 sec

// --- HTML / Javascript ---
const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stepper Pro v7 - Variabele Tijd</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:20px;}
canvas{background:#1e1e1e;border:2px solid #444;cursor:crosshair;touch-action:none;display:block;margin:20px auto;border-radius:8px;}
.controls{background:#2a2a2a;padding:15px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;gap:10px;margin-bottom:10px;border:1px solid #444;flex-wrap:wrap;}
input,button,select{height:42px; padding:0 10px; border-radius:4px;border:none;background:#444;color:white;outline:none;font-size:14px;box-sizing:border-box;}
button{background:#00bfff;cursor:pointer;font-weight:bold;transition:0.2s;}
button:hover{background:#009cd1;transform:scale(1.03);}
select{background:#333;border:1px solid #555;cursor:pointer;min-width:120px;}
.del-btn{background:#ff4444; font-size:1.8em; padding:0 10px; display:flex; align-items:center; justify-content:center;}
.status-bar{color:#00bfff;font-family:monospace;margin-bottom:10px;font-size:1.1em;}
.divider{width:1px; height:30px; background:#555;}
label{font-size:0.8em; color:#888; margin-right:-5px;}
</style>
</head>
<body>
    <h2>Motor Envelope Control</h2>
    <div class="status-bar" id="statusBox">Actief: <span id="curStatus">-</span></div>
    
    <div class="controls">
        <input type="text" id="filename" placeholder="Naam preset" style="width:120px;">
        <label>Duur (sec):</label>
        <input type="number" id="totalTime" value="8" min="1" max="3600" style="width:70px;" onchange="updateDuration()">
        <button onclick="savePreset()">Opslaan</button>
        <div class="divider"></div>
        <select id="presetSelect" onchange="autoLoadPreset()"><option>Laden...</option></select>
        <button class="del-btn" onclick="deletePreset()">🗑</button>
    </div>

    <canvas id="envelopeCanvas" width="800" height="400"></canvas>
    <p style="color:#666; font-size:0.9em;">Sleep punten om te schalen. De laatste punt bepaalt de totale duur.</p>

<script>
const canvas=document.getElementById("envelopeCanvas"), ctx=canvas.getContext("2d");
let keyframes=[{time:0,value:0},{time:8000,value:0}], selected=null, playStart=Date.now();

const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function updateDuration(){
    const newMs = document.getElementById("totalTime").value * 1000;
    keyframes[keyframes.length-1].time = newMs;
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
    
    let totalTime = keyframes[keyframes.length-1].time;
    let elapsed=(Date.now()-playStart)%totalTime;
    ctx.strokeStyle="red"; ctx.beginPath(); ctx.moveTo(toX(elapsed),0); ctx.lineTo(toX(elapsed),canvas.height); ctx.stroke();
    requestAnimationFrame(draw);
}

canvas.onmousedown=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left, y=e.clientY-rect.top;
    selected=keyframes.find(p=>Math.hypot(toX(p.time)-x,toY(p.value)-y)<12);
    if(!selected){
        keyframes.push({time:fromX(x),value:fromY(y)});
        keyframes.sort((a,b)=>a.time-b.time);
        sync();
    }
};
window.onmousemove=(e)=>{
    if(!selected)return;
    const rect=canvas.getBoundingClientRect();
    if(selected!==keyframes[0] && selected!==keyframes[keyframes.length-1]) {
        selected.time=Math.max(1, fromX(e.clientX-rect.left));
    }
    selected.value=Math.max(-100,Math.min(100,fromY(e.clientY-rect.top)));
    keyframes.sort((a,b)=>a.time-b.time);
    sync();
};
window.onmouseup=()=>selected=null;

function sync(){ 
    document.getElementById("totalTime").value = Math.round(keyframes[keyframes.length-1].time / 1000);
    fetch('/set_live',{method:'POST',body:JSON.stringify(keyframes)}); 
}

function savePreset(){
    const name = document.getElementById("filename").value;
    if(!name) return alert("Naam verplicht");
    fetch('/save?name='+name, {method:'POST', body:JSON.stringify(keyframes)}).then(()=>updatePresetList(name));
}

function autoLoadPreset(){
    const name = document.getElementById("presetSelect").value;
    if(!name) return;
    fetch('/load?name='+name).then(r=>r.json()).then(data=>{
        keyframes = data;
        document.getElementById("totalTime").value = Math.round(keyframes[keyframes.length-1].time / 1000);
        document.getElementById("curStatus").textContent = name;
    });
}

function deletePreset(){
    const name = document.getElementById("presetSelect").value;
    if(!name || !confirm("Verwijder '" + name + "'?")) return;
    fetch('/delete?name='+name, {method: 'DELETE'}).then(r=>r.json()).then(data=>{
        keyframes = data;
        document.getElementById("totalTime").value = Math.round(keyframes[keyframes.length-1].time / 1000);
        updatePresetList();
    });
}

function updatePresetList(selectedName = null){
    fetch('/list').then(r=>r.json()).then(list=>{
        const sel = document.getElementById("presetSelect");
        sel.innerHTML = list.length ? "" : "<option value=''>Geen presets</option>";
        list.forEach(f => {
            let opt = document.createElement("option");
            opt.value = f; opt.textContent = f;
            if(f === selectedName) opt.selected = true;
            sel.appendChild(opt);
        });
        if(sel.value) document.getElementById("curStatus").textContent = sel.value;
    });
}

fetch('/get_active').then(r=>r.json()).then(data=>{
    if(data && data.length > 0) {
        keyframes = data;
        document.getElementById("totalTime").value = Math.round(keyframes[keyframes.length-1].time / 1000);
    }
    updatePresetList();
    draw();
});
</script>
</body>
</html>
)rawliteral";

// --- C++ Backend ---

void loadFromJSON(String json) {
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, json)) return;
  envelopeSize = doc.size();
  for (int i = 0; i < envelopeSize; i++) {
    envelope[i].time = doc[i]["time"];
    envelope[i].value = doc[i]["value"];
  }
  loopDuration = envelope[envelopeSize - 1].time; // De laatste punt bepaalt de loop
}

void handleLive() {
  if (server.hasArg("plain")) loadFromJSON(server.arg("plain"));
  server.send(200);
}

void handleSave() {
  String name = server.arg("name");
  String data = server.arg("plain");
  if (name != "" && data != "") {
    File f1 = LittleFS.open("/" + name + ".json", "w"); f1.print(data); f1.close();
    File f2 = LittleFS.open("/active.json", "w"); f2.print(data); f2.close();
    server.send(200);
  } else server.send(400);
}

void handleLoad() {
  String name = server.arg("name");
  File file = LittleFS.open("/" + name + ".json", "r");
  if (file) {
    String content = file.readString(); file.close();
    loadFromJSON(content);
    File active = LittleFS.open("/active.json", "w"); active.print(content); active.close();
    server.send(200, "application/json", content);
  } else server.send(404);
}

void handleDelete() {
  String name = server.arg("name");
  LittleFS.remove("/" + name + ".json");
  // (Logica voor volgende preset laden...)
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  String toLoadName = "";
  while(file){
    String fn = file.name();
    if(fn.endsWith(".json") && fn != "active.json") { toLoadName = fn.substring(0, fn.length()-5); break; }
    file = root.openNextFile();
  }
  String content = (toLoadName != "") ? LittleFS.open("/"+toLoadName+".json","r").readString() : "[{\"time\":0,\"value\":0},{\"time\":8000,\"value\":0}]";
  loadFromJSON(content);
  File active = LittleFS.open("/active.json", "w"); active.print(content); active.close();
  server.send(200, "application/json", content);
}

void handleGetActive() {
  File file = LittleFS.open("/active.json", "r");
  if (file) { String s = file.readString(); file.close(); server.send(200, "application/json", s); }
  else server.send(200, "application/json", "[]");
}

void handleList() {
  String list = "[";
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String n = file.name();
    if (n.endsWith(".json") && n != "active.json") {
      if (list != "[") list += ",";
      list += "\"" + n.substring(0, n.length() - 5) + "\"";
    }
    file = root.openNextFile();
  }
  list += "]";
  server.send(200, "application/json", list);
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(motorPins[i], OUTPUT);
  LittleFS.begin(true);
  if (LittleFS.exists("/active.json")) loadFromJSON(LittleFS.open("/active.json", "r").readString());
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  server.on("/", []() { server.send(200, "text/html", htmlPage); });
  server.on("/set_live", HTTP_POST, handleLive);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/load", HTTP_GET, handleLoad);
  server.on("/delete", HTTP_DELETE, handleDelete);
  server.on("/list", HTTP_GET, handleList);
  server.on("/get_active", HTTP_GET, handleGetActive);
  server.begin();
  startTime = millis();
}

void loop() {
  server.handleClient();
  unsigned long t = (millis() - startTime) % loopDuration;
  int val = 0;
  for (int i = 0; i < envelopeSize - 1; i++) {
    if (t >= envelope[i].time && t <= envelope[i + 1].time) {
      float f = (float)(t - envelope[i].time) / (envelope[i + 1].time - envelope[i].time);
      val = (int)envelope[i].value + f * (envelope[i + 1].value - envelope[i].value);
      break;
    }
  }
  float speedFactor = abs(val) / 100.0;
  if (speedFactor > 0.02) {
    unsigned long stepInterval = 1000000.0 / (speedFactor * MAX_STEPS_PER_SEC);
    if (micros() - lastStepMicros >= stepInterval) {
      lastStepMicros = micros();
      stepIndex = (val > 0) ? (stepIndex + 1) % 8 : (stepIndex + 7) % 8;
      for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], steps[stepIndex][i]);
    }
  } else {
    for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], LOW);
  }
}