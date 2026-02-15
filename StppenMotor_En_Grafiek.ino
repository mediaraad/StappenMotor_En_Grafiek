#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <math.h>

// ---------------- WiFi ----------------
const char* ssid     = WIFI_TP_SSID;
const char* password = WIFI_TP_PASSWORD;
WebServer server(80);

// ---------------- Motor ----------------
const int motorPins[] = {14,12,13,15}; // ULN2003
int stepIndex = 0;
long motorPosition = 0;       // huidige positie in stappen
long targetPosition = 0;      // gewenste positie volgens grafiek
unsigned long previousStepMicros = 0;
float MAX_STEPS_PER_SEC = 800; // max snelheid 28BYJ-48

const int steps[8][4] = {
  {1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},
  {0,0,1,0},{0,0,1,1},{0,0,0,1},{1,0,0,1}
};

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
<title>Stepper Motor Envelope</title>
<style>
body{background:#121212;color:#eee;font-family:Arial;text-align:center;}
canvas{background:#1e1e1e;border:1px solid #444;margin-top:20px;}
.instructions{color:#00bfff;margin-top:10px;}
</style>
</head>
<body>
<h2>Stepper Motor Envelope Control</h2>
<p class="instructions">Klik = punt toevoegen/selecteren, sleep = verplaatsen, dubbelklik = verwijderen</p>

<canvas id="envelopeCanvas" width="800" height="400"></canvas>

<div style="margin-top:20px;color:#00bfff;">
<h3>Status</h3>
<p>Snelheid: <span id="speedVal">0</span> stappen/sec</p>
<p>Draairichting: <span id="dirVal">Stop</span></p>
</div>

<script>
const canvas=document.getElementById("envelopeCanvas");
const ctx=canvas.getContext("2d");
let keyframes=[{time:0,value:0},{time:8000,value:0}];
let selectedPoint=null;
const maxValue=100;
let playStartTime=Date.now();

function valueToY(val){return canvas.height/2 - val*(canvas.height/2)/maxValue;}
function yToValue(y){return (canvas.height/2 - y)*maxValue/(canvas.height/2);}

function getValueAt(t){
  for(let i=0;i<keyframes.length-1;i++){
    if(t>=keyframes[i].time && t<=keyframes[i+1].time){
      let f=(t-keyframes[i].time)/(keyframes[i+1].time-keyframes[i].time);
      return keyframes[i].value + f*(keyframes[i+1].value-keyframes[i].value);
    }
  }
  return keyframes[keyframes.length-1].value;
}

function drawEnvelope(){
 ctx.clearRect(0,0,canvas.width,canvas.height);
 ctx.strokeStyle="#555";
 ctx.beginPath();
 ctx.moveTo(0,canvas.height/2);
 ctx.lineTo(canvas.width,canvas.height/2);
 ctx.stroke();

 ctx.strokeStyle="#00bfff";
 ctx.beginPath();
 keyframes.forEach((p,i)=>{
   let x=p.time*canvas.width/keyframes[keyframes.length-1].time;
   let y=valueToY(p.value);
   i?ctx.lineTo(x,y):ctx.moveTo(x,y);
 });
 ctx.stroke();

 ctx.fillStyle="#ff9900";
 keyframes.forEach(p=>{
   let x=p.time*canvas.width/keyframes[keyframes.length-1].time;
   let y=valueToY(p.value);
   ctx.beginPath();ctx.arc(x,y,5,0,2*Math.PI);ctx.fill();
 });

 let elapsed=(Date.now()-playStartTime)%keyframes[keyframes.length-1].time;
 let currentX=elapsed*canvas.width/keyframes[keyframes.length-1].time;
 ctx.strokeStyle="#ff0000";
 ctx.lineWidth=2;
 ctx.beginPath();
 ctx.moveTo(currentX,0);ctx.lineTo(currentX,canvas.height);
 ctx.stroke();ctx.lineWidth=1;
}

// ---------- Canvas interactie ----------
canvas.addEventListener("mousedown",(e)=>{
 const rect=canvas.getBoundingClientRect();
 const x=e.clientX-rect.left,y=e.clientY-rect.top;
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
   sendKeyframes();drawEnvelope();
 }
});
canvas.addEventListener("mousemove",(e)=>{
 if(!selectedPoint)return;
 const rect=canvas.getBoundingClientRect();
 selectedPoint.time=(e.clientX-rect.left)*keyframes[keyframes.length-1].time/canvas.width;
 selectedPoint.value=yToValue(e.clientY-rect.top);
 keyframes.sort((a,b)=>a.time-b.time);
 sendKeyframes();drawEnvelope();
});
canvas.addEventListener("mouseup",()=>{selectedPoint=null;});
canvas.addEventListener("dblclick",(e)=>{
 const rect=canvas.getBoundingClientRect();
 const x=e.clientX-rect.left,y=e.clientY-rect.top;
 keyframes=keyframes.filter(p=>{
   let px=p.time*canvas.width/keyframes[keyframes.length-1].time;
   let py=valueToY(p.value);
   return Math.hypot(px-x,py-y)>=8;
 });
 drawEnvelope();sendKeyframes();
});

function sendKeyframes(){
 fetch('/envelope',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(keyframes)});
}

function updateStatus(){
 let elapsed=(Date.now()-playStartTime)%keyframes[keyframes.length-1].time;
 let val=getValueAt(elapsed);
 let dir = val>0?"Vooruit":val<0?"Achteruit":"Stop";
 document.getElementById("speedVal").textContent=Math.round(Math.abs(val*800/100));
 document.getElementById("dirVal").textContent=dir;
 drawEnvelope();
 requestAnimationFrame(updateStatus);
}
requestAnimationFrame(updateStatus);
</script>
</body>
</html>
)rawliteral";

// ---------------- Setup ----------------
void setup(){
  Serial.begin(115200);
  for(int i=0;i<4;i++) pinMode(motorPins[i],OUTPUT);

  WiFi.setHostname("ESP32-MOTOR");
  WiFi.begin(ssid,password);
  Serial.print("Verbinden met WiFi");
  while(WiFi.status()!=WL_CONNECTED){delay(500);Serial.print(".");}
  Serial.println("\nWiFi verbonden!");
  Serial.println(WiFi.localIP());

  server.on("/",[](){server.send(200,"text/html",htmlPage);});
  server.on("/envelope", HTTP_POST, handleEnvelope);
  server.on("/position", handlePosition);
  server.begin();

  startTime = millis();
  previousStepMicros = micros();
}

// ---------------- Loop ----------------
void loop(){
  server.handleClient();
  if(envelopeSize<2) return;

  unsigned long now = micros();
  float deltaSec = (now - previousStepMicros) / 1000000.0;
  previousStepMicros = now;

  int val = getCurrentValue(); // -100..100
  targetPosition = long(val * 16); // schaalfactor 16 stappen per 100

  long delta = targetPosition - motorPosition;
  if(delta==0) return; // niets te doen

  float stepDelay = 1000000.0 / MAX_STEPS_PER_SEC; // μs per stap
  static float stepAccum = 0;

  // Bereken hoeveel stappen we mogen zetten
  float stepsThisInterval = deltaSec * MAX_STEPS_PER_SEC;
  stepAccum += stepsThisInterval;
  while(stepAccum >= 1.0){
    if(delta>0){
      stepIndex = (stepIndex + 1) % 8;
      motorPosition++;
    }else{
      stepIndex = (stepIndex + 7) % 8;
      motorPosition--;
    }
    for(int i=0;i<4;i++) digitalWrite(motorPins[i],steps[stepIndex][i]);
    stepAccum -= 1.0;
  }
}

// ---------------- Envelope ----------------
void handleEnvelope(){
  if(!server.hasArg("plain")){ server.send(400); return; }
  String body = server.arg("plain");
  DynamicJsonDocument doc(4096);
  deserializeJson(doc, body);
  envelopeSize = doc.size();
  for(int i=0;i<envelopeSize;i++){
    envelope[i].time = doc[i]["time"];
    envelope[i].value = doc[i]["value"];
  }
  if(envelope[envelopeSize-1].time==0) envelope[envelopeSize-1].time=8000;
  startTime = millis();
  server.send(200,"text/plain","OK");
}

int getCurrentValue(){
  if(envelopeSize<2) return 0;
  unsigned long t = (millis()-startTime) % envelope[envelopeSize-1].time;
  for(int i=0;i<envelopeSize-1;i++){
    if(t>=envelope[i].time && t<=envelope[i+1].time){
      float f = float(t-envelope[i].time)/float(envelope[i+1].time-envelope[i].time);
      return envelope[i].value + f*(envelope[i+1].value-envelope[i].value);
    }
  }
  return 0;
}

// ---------------- Motor positie endpoint ----------------
void handlePosition(){
  String json="{\"steps\":" + String(motorPosition) + "}";
  server.send(200,"application/json",json);
}
