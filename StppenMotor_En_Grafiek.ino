#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// --- WiFi & Server ---
const char* ssid     = WIFI_TP_SSID;
const char* password = WIFI_TP_PASSWORD;
WebServer server(80);

// --- Motor Configuratie (ULN2003) ---
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
unsigned long loopDuration = 8000;

// --- HTML / Javascript ---
const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stepper Control Pro v6</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:20px;}
canvas{background:#1e1e1e;border:2px solid #444;cursor:crosshair;touch-action:none;display:block;margin:20px auto;border-radius:8px;}
.controls{background:#2a2a2a;padding:15px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;gap:10px;margin-bottom:10px;border:1px solid #444;}
input,button,select{height:42px; padding:0 15px; border-radius:4px;border:none;background:#444;color:white;outline:none;font-size:14px;box-sizing:border-box;}
button{background:#00bfff;cursor:pointer;font-weight:bold;transition:0.2s;}
button:hover{background:#009cd1;transform:scale(1.03);}
select{background:#333;border:1px solid #555;cursor:pointer;min-width:120px;}
.del-btn{background:#ff4444; font-size:1.8em; padding:0 10px; display:flex; align-items:center; justify-content:center; line-height:1;}
.status-bar{color:#00bfff;font-family:monospace;margin-bottom:10px;font-size:1.1em; transition: 0.3s; min-height:1.2em;}
.warning{color:#ff4444; font-weight:bold;}
.divider{width:1px; height:30px; background:#555; margin:0 5px;}
</style>
</head>
<body>
    <h2>Motor Envelope Control</h2>
    <div class="status-bar" id="statusBox">Actief: <span id="curStatus">-</span></div>
    
    <div class="controls">
        <input type="text" id="filename" placeholder="Naam preset">
        <button onclick="savePreset()">Opslaan</button>
        <div class="divider"></div>
        <select id="presetSelect" onchange="autoLoadPreset()"><option>Laden...</option></select>
        <button class="del-btn" onclick="deletePreset()">🗑</button>
    </div>

    <canvas id="envelopeCanvas" width="800" height="400"></canvas>
    <p style="color:#666; font-size:0.9em;">Dubbelklik punt om te wissen. Alles wordt direct uitgevoerd.</p>

<script>
const canvas=document.getElementById("envelopeCanvas"), ctx=canvas.getContext("2d");
let keyframes=[{time:0,value:0},{time:8000,value:0}], selected=null, playStart=Date.now();

const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function setStatus(msg, isError=false){
    const box = document.getElementById("statusBox");
    const status = document.getElementById("curStatus");
    status.textContent = msg;
    box.className = isError ? "status-bar warning" : "status-bar";
}

function draw(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    ctx.strokeStyle="#333"; ctx.lineWidth=1; ctx.beginPath(); ctx.moveTo(0,canvas.height/2); ctx.lineTo(canvas.width,canvas.height/2); ctx.stroke();
    ctx.strokeStyle="#00bfff"; ctx.lineWidth=3; ctx.beginPath();
    keyframes.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value)));
    ctx.stroke();
    ctx.fillStyle="#ff9900";
    keyframes.forEach(p=>{ctx.beginPath();ctx.arc(toX(p.time),toY(p.value),6,0,7);ctx.fill();});
    let elapsed=(Date.now()-playStart)%keyframes[keyframes.length-1].time;
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
    if(selected!==keyframes[0] && selected!==keyframes[keyframes.length-1]) selected.time=fromX(e.clientX-rect.left);
    selected.value=Math.max(-100,Math.min(100,fromY(e.clientY-rect.top)));
    keyframes.sort((a,b)=>a.time-b.time);
    sync();
};
window.onmouseup=()=>selected=null;
canvas.ondblclick=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left, y=e.clientY-rect.top;
    if(keyframes.length>2) {
        keyframes=keyframes.filter(p=>Math.hypot(toX(p.time)-x,toY(p.value)-y)>12);
        sync();
    }
};

function sync(){ fetch('/set_live',{method:'POST',body:JSON.stringify(keyframes)}); }

function savePreset(){
    const name = document.getElementById("filename").value;
    if(!name) return alert("Naam verplicht");
    fetch('/save?name='+name, {method:'POST', body:JSON.stringify(keyframes)}).then(()=>{
        updatePresetList(name);
        document.getElementById("filename").value = "";
        setStatus("Preset '" + name + "' opgeslagen");
    });
}

function autoLoadPreset(){
    const name = document.getElementById("presetSelect").value;
    if(!name) return;
    fetch('/load?name='+name).then(r=>r.json()).then(data=>{
        keyframes = data;
        setStatus("Actief: " + name);
    });
}

function deletePreset(){
    const name = document.getElementById("presetSelect").value;
    if(!name || name === "Laden...") return;
    if(!confirm("Verwijder '" + name + "'?")) return;
    fetch('/delete?name='+name, {method: 'DELETE'}).then(r=>r.json()).then(data=>{
        keyframes = data;
        updatePresetList();
        setStatus("Verwijderd. Vorige config geladen.", true);
    });
}

function updatePresetList(selectedName = null){
    fetch('/list').then(r=>r.json()).then(list=>{
        const sel = document.getElementById("presetSelect");
        sel.innerHTML = "";
        if(!list.length) {
            sel.innerHTML = "<option value=''>Geen presets</option>";
            return;
        }
        list.forEach(f => {
            let opt = document.createElement("option");
            opt.value = f; opt.textContent = f;
            if(f === selectedName) opt.selected = true;
            sel.appendChild(opt);
        });
        if(selectedName) setStatus("Actief: " + selectedName);
        else if(sel.value) setStatus("Actief: " + sel.value);
    });
}

fetch('/get_active').then(r=>r.json()).then(data=>{
    if(data && data.length > 0) keyframes = data;
    updatePresetList();
    draw();
});
</script>
</body>
</html>
)rawliteral";

// --- C++ Handlers (Gelijk aan v5) ---

void loadFromJSON(String json) {
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, json)) return;
  envelopeSize = doc.size();
  for (int i = 0; i < envelopeSize; i++) {
    envelope[i].time = doc[i]["time"];
    envelope[i].value = doc[i]["value"];
  }
  loopDuration = envelope[envelopeSize - 1].time;
}

void handleLive() {
  if (server.hasArg("plain")) loadFromJSON(server.arg("plain"));
  server.send(200);
}

void handleSave() {
  String name = server.arg("name");
  String data = server.arg("plain");
  if (name != "" && data != "") {
    File file = LittleFS.open("/" + name + ".json", "w");
    if (file) { file.print(data); file.close(); }
    File active = LittleFS.open("/active.json", "w");
    if (active) { active.print(data); active.close(); }
    server.send(200);
  } else { server.send(400); }
}

void handleLoad() {
  String name = server.arg("name");
  File file = LittleFS.open("/" + name + ".json", "r");
  if (file) {
    String content = file.readString();
    file.close();
    loadFromJSON(content);
    File active = LittleFS.open("/active.json", "w");
    if (active) { active.print(content); active.close(); }
    server.send(200, "application/json", content);
  } else { server.send(404); }
}

void handleDelete() {
  String nameToDelete = server.arg("name");
  String path = "/" + nameToDelete + ".json";
  if (LittleFS.exists(path)) LittleFS.remove(path);

  String nextPreset = "", prevPreset = "", firstFound = "", toLoad = "";
  bool foundCurrent = false;
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String fName = String(file.name());
    if (fName.endsWith(".json") && fName != "active.json") {
      String sName = fName.substring(0, fName.length() - 5);
      if (firstFound == "") firstFound = sName;
      if (foundCurrent && nextPreset == "") nextPreset = sName;
      if (sName == nameToDelete) foundCurrent = true;
      else if (!foundCurrent) prevPreset = sName;
    }
    file = root.openNextFile();
  }

  String content = "[{\"time\":0,\"value\":0},{\"time\":8000,\"value\":0}]";
  toLoad = (prevPreset != "") ? prevPreset : (nextPreset != "") ? nextPreset : firstFound;
  if (toLoad != "") {
    File f = LittleFS.open("/" + toLoad + ".json", "r");
    content = f.readString(); f.close();
  }
  loadFromJSON(content);
  File active = LittleFS.open("/active.json", "w");
  active.print(content); active.close();
  server.send(200, "application/json", content);
}

void handleGetActive() {
  File file = LittleFS.open("/active.json", "r");
  if (file) {
    String s = file.readString(); file.close();
    server.send(200, "application/json", s);
  } else { server.send(200, "application/json", "[]"); }
}

void handleList() {
  String list = "[";
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (name.endsWith(".json") && name != "active.json") {
      if (list != "[") list += ",";
      list += "\"" + name.substring(0, name.length() - 5) + "\"";
    }
    file = root.openNextFile();
  }
  list += "]";
  server.send(200, "application/json", list);
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(motorPins[i], OUTPUT);
  if(!LittleFS.begin(true)) Serial.println("LittleFS Mount Failed");

  if (LittleFS.exists("/active.json")) {
    File f = LittleFS.open("/active.json", "r");
    loadFromJSON(f.readString()); f.close();
  } else {
    envelope[0] = {0, 0}; envelope[1] = {8000, 0};
  }

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