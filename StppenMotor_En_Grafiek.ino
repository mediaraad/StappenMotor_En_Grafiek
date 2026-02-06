#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <math.h> // fmax

// ---------------- WiFi ----------------
const char* ssid     = WIFI_TP_SSID;
const char* password = WIFI_TP_PASSWORD;
WebServer server(80);

// ---------------- Motor ----------------
const int motorPins[] = {14,12,13,15}; // ULN2003 groene draadjes
int stepIndex = 0;
unsigned long previousMicros = 0;
float stepDelay = 1000; // μs per stap
bool direction = true;  // true=vooruit, false=achteruit

const int steps[8][4] = {
  {1,0,0,0},
  {1,1,0,0},
  {0,1,0,0},
  {0,1,1,0},
  {0,0,1,0},
  {0,0,1,1},
  {0,0,0,1},
  {1,0,0,1}
};

#define MAX_STEPS_PER_SEC 3000.0  // Hogere max snelheid

// Motor positie
long motorPosition = 0; // absolute stappositie

// ---------------- Envelope ----------------
struct Keyframe {
  unsigned long time; // ms
  int value;          // -100 .. +100
};
Keyframe envelope[50];
int envelopeSize = 0;
unsigned long startTime = 0;

// ---------------- HTML + JS ----------------
const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stappenmotor Envelope</title>
<style>
  body { background: #121212; color: #eee; font-family: Arial, sans-serif; text-align:center; }
  canvas { background: #1e1e1e; border: 1px solid #444; margin-top: 20px; }
  .instructions { margin-top: 10px; color: #00bfff; }
  .canvas-container { display:flex; justify-content:center; flex-wrap:wrap; gap:20px; }
</style>
</head>
<body>
<h2>Stappenmotor Envelope Control</h2>
<p class="instructions">
Klik = punt toevoegen / selecteren. Sleep om te verplaatsen. Dubbelklik = verwijderen.
</p>

<div class="canvas-container">
  <!-- Envelope canvas -->
  <canvas id="envelopeCanvas" width="800" height="400"></canvas>
  <!-- Motorwijzer canvas -->
  <canvas id="motorCanvas" width="200" height="200"></canvas>
</div>

<div style="margin-top:20px; color:#00bfff;">
  <h3>Status</h3>
  <p>Snelheid: <span id="speedVal">0</span> stappen/sec</p>
  <p>Draairichting: <span id="dirVal">Vooruit</span></p>
</div>

<script>
const canvas = document.getElementById("envelopeCanvas");
const ctx = canvas.getContext("2d");
const motorCanvas = document.getElementById("motorCanvas");
const ctxMotor = motorCanvas.getContext("2d");

let keyframes = [
  {time:0,value:0},
  {time:8000,value:0}
];
let selectedPoint = null;
const maxValue = 100;
let playStartTime = Date.now();

// ---------- Envelope helpers ----------
function valueToY(val){ return canvas.height/2 - val*(canvas.height/2)/maxValue; }
function yToValue(y){ return (canvas.height/2 - y) * maxValue/(canvas.height/2); }

function drawEnvelope(){
  ctx.clearRect(0,0,canvas.width,canvas.height);

  // Middenlijn
  ctx.strokeStyle="#555";
  ctx.beginPath();
  ctx.moveTo(0,canvas.height/2);
  ctx.lineTo(canvas.width,canvas.height/2);
  ctx.stroke();

  // Keyframe-lijn
  ctx.strokeStyle="#00bfff";
  ctx.beginPath();
  keyframes.forEach((p,i)=>{
    let x = p.time * canvas.width / keyframes[keyframes.length-1].time;
    let y = valueToY(p.value);
    if(i==0) ctx.moveTo(x,y);
    else ctx.lineTo(x,y);
  });
  ctx.stroke();

  // Punten
  ctx.fillStyle="#ff9900";
  keyframes.forEach(p=>{
    let x = p.time * canvas.width / keyframes[keyframes.length-1].time;
    let y = valueToY(p.value);
    ctx.beginPath();
    ctx.arc(x,y,5,0,2*Math.PI);
    ctx.fill();
  });

  // Rode tijd-indicator
  let elapsed = (Date.now() - playStartTime) % keyframes[keyframes.length-1].time;
  let currentX = elapsed * canvas.width / keyframes[keyframes.length-1].time;
  ctx.strokeStyle = "#ff0000";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(currentX,0);
  ctx.lineTo(currentX,canvas.height);
  ctx.stroke();
  ctx.lineWidth = 1;
}

// ---------- Motor wijzer ----------
function drawMotor(degrees){
  const centerX = motorCanvas.width/2;
  const centerY = motorCanvas.height/2;
  const radius = motorCanvas.width/2 - 10;

  ctxMotor.clearRect(0,0,motorCanvas.width,motorCanvas.height);

  // Cirkel
  ctxMotor.strokeStyle = "#00bfff";
  ctxMotor.lineWidth = 4;
  ctxMotor.beginPath();
  ctxMotor.arc(centerX, centerY, radius, 0, 2*Math.PI);
  ctxMotor.stroke();

  // Wijzer
  let rad = (degrees-90)*Math.PI/180;
  ctxMotor.strokeStyle = "#ff0000";
  ctxMotor.lineWidth = 3;
  ctxMotor.beginPath();
  ctxMotor.moveTo(centerX, centerY);
  ctxMotor.lineTo(centerX + radius * Math.cos(rad), centerY + radius * Math.sin(rad));
  ctxMotor.stroke();

  // Middenpunt
  ctxMotor.fillStyle = "#fff";
  ctxMotor.beginPath();
  ctxMotor.arc(centerX, centerY, 5,0,2*Math.PI);
  ctxMotor.fill();
}

// ---------- Canvas interactie ----------
canvas.addEventListener("mousedown",(e)=>{
  const rect=canvas.getBoundingClientRect();
  const x=e.clientX-rect.left;
  const y=e.clientY-rect.top;
  selectedPoint=null;
  keyframes.forEach(p=>{
    let px=p.time*canvas.width/keyframes[keyframes.length-1].time;
    let py=valueToY(p.value);
    if(Math.hypot(px-x,py-y)<8) selectedPoint=p;
  });
  if(!selectedPoint){
    let t=x*keyframes[keyframes.length-1].time/canvas.width;
    let val=yToValue(y);
    keyframes.push({time:t,value:val});
    keyframes.sort((a,b)=>a.time-b.time);
    drawEnvelope();
    sendKeyframes();
  }
});

canvas.addEventListener("mousemove",(e)=>{
  if(!selectedPoint) return;
  const rect=canvas.getBoundingClientRect();
  const x=e.clientX-rect.left;
  const y=e.clientY-rect.top;
  selectedPoint.time=x*keyframes[keyframes.length-1].time/canvas.width;
  selectedPoint.value=yToValue(y);
  keyframes.sort((a,b)=>a.time-b.time);
  drawEnvelope();
  sendKeyframes();
});

canvas.addEventListener("mouseup",()=>{selectedPoint=null;});
canvas.addEventListener("dblclick",(e)=>{
  const rect=canvas.getBoundingClientRect();
  const x=e.clientX-rect.left;
  const y=e.clientY-rect.top;
  keyframes=keyframes.filter(p=>{
    let px=p.time*canvas.width/keyframes[keyframes.length-1].time;
    let py=valueToY(p.value);
    return Math.hypot(px-x,py-y)>=8;
  });
  drawEnvelope();
  sendKeyframes();
});

function sendKeyframes(){
  fetch('/envelope',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(keyframes)
  });
}

// ---------- Realtime status ----------
function updateStatus(){
  let elapsed = (Date.now() - playStartTime) % keyframes[keyframes.length-1].time;
  let val = 0;
  for(let i=0;i<keyframes.length-1;i++){
    if(elapsed >= keyframes[i].time && elapsed <= keyframes[i+1].time){
      let f = (elapsed - keyframes[i].time) / (keyframes[i+1].time - keyframes[i].time);
      val = keyframes[i].value + f*(keyframes[i+1].value - keyframes[i].value);
      break;
    }
  }
  let dir = val>=0 ? "Vooruit" : "Achteruit";
  let stepsPerSec = Math.round(Math.abs(val)*3000/100);
  document.getElementById("speedVal").textContent = stepsPerSec;
  document.getElementById("dirVal").textContent = dir;

  // Motor wijzer update
  fetch('/position').then(r=>r.json()).then(data=>{
    drawMotor(data.degrees);
  });
}

setInterval(drawEnvelope,40);
setInterval(updateStatus,40);
</script>
</body>
</html>
)rawliteral";

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  for(int i=0;i<4;i++) pinMode(motorPins[i],OUTPUT);
  WiFi.setHostname("ESP32-JOEPIE_DE_POEPIE");
  WiFi.begin(ssid,password);
  Serial.print("Verbinden met WiFi");
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi verbonden!");
  Serial.println(WiFi.localIP());
 
  server.on("/",[](){server.send(200,"text/html",htmlPage);});
  server.on("/envelope",HTTP_POST,handleEnvelope);
  server.on("/position",handlePosition);
  server.begin();
  startTime = millis();
}

// ---------------- Loop ----------------
void loop() {
  server.handleClient();

  int val = getCurrentValue();           // huidig keyframe value (-100..+100)
  direction = val>=0;

  // Snelheid: min 300, max 3000 stappen/sec
  float stepsPerSec = map(abs(val),0,100,300,3000);
  stepDelay = 1000000.0 / stepsPerSec;

  unsigned long now = micros();
  if(now - previousMicros >= stepDelay){
    previousMicros = now;
    for(int i=0;i<4;i++) digitalWrite(motorPins[i],steps[stepIndex][i]);
    stepIndex += direction?1:-1;
    if(stepIndex>7) stepIndex=0;
    if(stepIndex<0) stepIndex=7;

    // Motor positie bijhouden
    motorPosition += direction?1:-1;
  }
}

// ---------------- Envelope ----------------
void handleEnvelope(){
  if(!server.hasArg("plain")){ server.send(400); return; }
  String body = server.arg("plain");
  DynamicJsonDocument doc(2048);
  deserializeJson(doc,body);
  envelopeSize = doc.size();
  for(int i=0;i<envelopeSize;i++){
    envelope[i].time = doc[i]["time"];
    envelope[i].value = doc[i]["value"];
  }
  startTime = millis();
  server.send(200,"text/plain","OK");
}

int getCurrentValue(){
  if(envelopeSize<2) return 0;
  unsigned long t = (millis() - startTime) % envelope[envelopeSize-1].time;
  for(int i=0;i<envelopeSize-1;i++){
    if(t>=envelope[i].time && t<=envelope[i+1].time){
      float f = (t - envelope[i].time) / float(envelope[i+1].time - envelope[i].time);
      int val = envelope[i].value + f * (envelope[i+1].value - envelope[i].value);
      return val;
    }
  }
  return envelope[envelopeSize-1].value;
}

// ---------------- Motor position endpoint ----------------
void handlePosition(){
  float deg = fmod((motorPosition % 2048) * (360.0 / 2048.0) + 360.0,360.0); // 0-360°
  String json = "{";
  json += "\"steps\":" + String(motorPosition) + ",";
  json += "\"degrees\":" + String(deg,1);
  json += "}";
  server.send(200,"application/json",json);
}
