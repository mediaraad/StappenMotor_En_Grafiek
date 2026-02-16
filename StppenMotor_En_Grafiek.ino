#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// --- Configuratie ---
const char* ssid     = WIFI_TP_SSID;
const char* password = WIFI_TP_PASSWORD;
WebServer server(80);

const int motorPins[] = {14, 12, 13, 15}; 
int stepIndex = 0;
unsigned long lastStepMicros = 0;
const float MAX_STEPS_PER_SEC = 800.0;

const int steps[8][4] = {
  {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
  {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
};

struct Keyframe {
  unsigned long time; 
  int value;          
};
Keyframe envelope[50];
int envelopeSize = 2;
unsigned long startTime = 0;
unsigned long loopDuration = 8000;

// --- HTML Pagina ---
const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stepper Control + Storage</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:20px;}
canvas{background:#1e1e1e;border:2px solid #444;cursor:crosshair;touch-action:none;display:block;margin:20px auto;}
.controls{background:#2a2a2a;padding:15px;border-radius:8px;display:inline-block;margin-bottom:10px;}
input,button,select{padding:8px;margin:5px;border-radius:4px;border:none;background:#444;color:white;}
button{background:#00bfff;cursor:pointer;font-weight:bold;}
button:hover{background:#009cd1;}
</style>
</head>
<body>
    <h2>Motor Speed Envelope</h2>
    
    <div class="controls">
        <input type="text" id="filename" placeholder="Naam (bijv. preset1)">
        <button onclick="savePreset()">Save</button>
        |
        <select id="presetSelect"><option>Laden...</option></select>
        <button onclick="loadPreset()">Load</button>
    </div>

    <canvas id="envelopeCanvas" width="800" height="400"></canvas>

<script>
const canvas=document.getElementById("envelopeCanvas"), ctx=canvas.getContext("2d");
let keyframes=[{time:0,value:0},{time:8000,value:0}], selected=null, playStart=Date.now();

const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function draw(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    ctx.strokeStyle="#333"; ctx.beginPath(); ctx.moveTo(0,canvas.height/2); ctx.lineTo(canvas.width,canvas.height/2); ctx.stroke();
    ctx.strokeStyle="#00bfff"; ctx.lineWidth=3; ctx.beginPath();
    keyframes.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value)));
    ctx.stroke();
    ctx.fillStyle="#ff9900";
    keyframes.forEach(p=>{ctx.beginPath();ctx.arc(toX(p.time),toY(p.value),6,0,7);ctx.fill();});
    let elapsed=(Date.now()-playStart)%keyframes[keyframes.length-1].time;
    ctx.strokeStyle="red"; ctx.beginPath(); ctx.moveTo(toX(elapsed),0); ctx.lineTo(toX(elapsed),canvas.height); ctx.stroke();
    requestAnimationFrame(draw);
}

// Interactie (zoals vorige versie)
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

// --- Storage Functies ---
function sync(){
    fetch('/set_live',{method:'POST',body:JSON.stringify(keyframes)});
}

function savePreset(){
    const name = document.getElementById("filename").value;
    if(!name) return alert("Geef een naam op");
    fetch('/save?name='+name, {method:'POST', body:JSON.stringify(keyframes)})
    .then(()=>updatePresetList());
}

function loadPreset(){
    const name = document.getElementById("presetSelect").value;
    fetch('/load?name='+name).then(r=>r.json()).then(data=>{
        keyframes = data;
        sync();
    });
}

function updatePresetList(){
    fetch('/list').then(r=>r.json()).then(list=>{
        const sel = document.getElementById("presetSelect");
        sel.innerHTML = list.map(f=>`<option value="${f}">${f}</option>`).join('');
    });
}

updatePresetList();
draw();
</script>
</body>
</html>
)rawliteral";

// --- Bestandsbeheer Functies ---

void saveToFile(String filename, String data) {
  File file = LittleFS.open("/" + filename + ".json", "w");
  if (file) {
    file.print(data);
    file.close();
  }
}

void loadFromData(String json) {
  DynamicJsonDocument doc(4096);
  deserializeJson(doc, json);
  envelopeSize = doc.size();
  for (int i = 0; i < envelopeSize; i++) {
    envelope[i].time = doc[i]["time"];
    envelope[i].value = doc[i]["value"];
  }
  loopDuration = envelope[envelopeSize - 1].time;
}

// --- Handlers ---
void handleLive() {
  if (server.hasArg("plain")) loadFromData(server.arg("plain"));
  server.send(200);
}

void handleSave() {
  String name = server.arg("name");
  if (name != "" && server.hasArg("plain")) {
    saveToFile(name, server.arg("plain"));
    server.send(200);
  } else {
    server.send(400);
  }
}

void handleLoad() {
  String name = server.arg("name");
  File file = LittleFS.open("/" + name + ".json", "r");
  if (file) {
    String content = file.readString();
    file.close();
    loadFromData(content); // Direct toepassen op motor
    server.send(200, "application/json", content);
  } else {
    server.send(404);
  }
}

void handleList() {
  String list = "[";
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (name.endsWith(".json")) {
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
  if(!LittleFS.begin(true)) Serial.println("LittleFS Fout!");
  
  for (int i = 0; i < 4; i++) pinMode(motorPins[i], OUTPUT);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  server.on("/", []() { server.send(200, "text/html", htmlPage); });
  server.on("/set_live", HTTP_POST, handleLive);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/load", HTTP_GET, handleLoad);
  server.on("/list", HTTP_GET, handleList);
  server.begin();

  envelope[0] = {0, 0};
  envelope[1] = {8000, 0};
  startTime = millis();
}

void loop() {
  server.handleClient();
  
  // Motor logica (Snelheid op basis van envelope)
  unsigned long t = (millis() - startTime) % loopDuration;
  int val = 0;
  for (int i = 0; i < envelopeSize - 1; i++) {
    if (t >= envelope[i].time && t <= envelope[i + 1].time) {
      float f = (float)(t - envelope[i].time) / (envelope[i + 1].time - envelope[i].time);
      val = envelope[i].value + f * (envelope[i + 1].value - envelope[i].value);
      break;
    }
  }

  float speedFactor = abs(val) / 100.0;
  if (speedFactor > 0.05) {
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