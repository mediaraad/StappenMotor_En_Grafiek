#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// --- Configuratie ---
const char* ssid     = WIFI_TP_SSID;
const char* password = WIFI_TP_PASSWORD;
WebServer server(80);

// Motor Pinnen (ULN2003). Let op: volgorde kan variëren (vaak 14,13,12,15)
const int motorPins[] = {14, 12, 13, 15}; 
int stepIndex = 0;
long motorPosition = 0;
unsigned long lastStepMicros = 0;
const float MAX_STEPS_PER_SEC = 800.0; // Max voor 28BYJ-48 op 5V

// Half-step tabel voor ULN2003 (8 stappen cycle)
const int steps[8][4] = {
  {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
  {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
};

// --- Envelope Data ---
struct Keyframe {
  unsigned long time; 
  int value;          // -100 (achteruit) tot 100 (vooruit)
};
Keyframe envelope[50];
int envelopeSize = 2;
unsigned long startTime = 0;
unsigned long loopDuration = 8000; // Standaard 8 seconden

// --- HTML Pagina ---
const char* htmlPage PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Stepper Control</title>
<style>
body{background:#121212;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:20px;}
canvas{background:#1e1e1e;border:2px solid #444;cursor:crosshair;touch-action:none;}
.status{color:#00bfff;font-family:monospace;margin:15px;font-size:1.2em;}
.hint{color:#888;font-size:0.9em;}
</style>
</head>
<body>
    <h2>Motor Speed Envelope</h2>
    <div class="status">
        Speed: <span id="speedVal">0</span> st/s | Dir: <span id="dirVal">-</span>
    </div>
    <canvas id="envelopeCanvas" width="800" height="400"></canvas>
    <p class="hint">Klik: punt erbij | Sleep: verplaatsen | Dubbelklik: verwijderen</p>

<script>
const canvas=document.getElementById("envelopeCanvas"), ctx=canvas.getContext("2d");
let keyframes=[{time:0,value:0},{time:8000,value:0}], selected=null, playStart=Date.now();

const toX=(t)=>t*canvas.width/keyframes[keyframes.length-1].time;
const toY=(v)=>canvas.height/2 - v*(canvas.height/2.2)/100;
const fromX=(x)=>x*keyframes[keyframes.length-1].time/canvas.width;
const fromY=(y)=>(canvas.height/2 - y)*100/(canvas.height/2.2);

function draw(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    // Grid
    ctx.strokeStyle="#333"; ctx.beginPath(); ctx.moveTo(0,canvas.height/2); ctx.lineTo(canvas.width,canvas.height/2); ctx.stroke();
    // Lijn
    ctx.strokeStyle="#00bfff"; ctx.lineWidth=3; ctx.beginPath();
    keyframes.forEach((p,i)=> i?ctx.lineTo(toX(p.time),toY(p.value)):ctx.moveTo(toX(p.time),toY(p.value)));
    ctx.stroke();
    // Punten
    ctx.fillStyle="#ff9900";
    keyframes.forEach(p=>{ctx.beginPath();ctx.arc(toX(p.time),toY(p.value),6,0,7);ctx.fill();});
    // Playhead
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
        send();
    }
};
window.onmousemove=(e)=>{
    if(!selected)return;
    const rect=canvas.getBoundingClientRect();
    if(selected!==keyframes[0] && selected!==keyframes[keyframes.length-1]) selected.time=fromX(e.clientX-rect.left);
    selected.value=Math.max(-100,Math.min(100,fromY(e.clientY-rect.top)));
    keyframes.sort((a,b)=>a.time-b.time);
    send();
};
window.onmouseup=()=>selected=null;
canvas.ondblclick=(e)=>{
    const rect=canvas.getBoundingClientRect(), x=e.clientX-rect.left, y=e.clientY-rect.top;
    if(keyframes.length>2) {
        keyframes=keyframes.filter(p=>Math.hypot(toX(p.time)-x,toY(p.value)-y)>12);
        send();
    }
};

function send(){
    fetch('/envelope',{method:'POST',body:JSON.stringify(keyframes)});
    const val=fromY(toY(0)); // Dummy voor status update
}

setInterval(()=>{
    let elapsed=(Date.now()-playStart)%keyframes[keyframes.length-1].time;
    let v=0;
    for(let i=0;i<keyframes.length-1;i++){
        if(elapsed>=keyframes[i].time && elapsed<=keyframes[i+1].time){
            let f=(elapsed-keyframes[i].time)/(keyframes[i+1].time-keyframes[i].time);
            v=keyframes[i].value+f*(keyframes[i+1].value-keyframes[i].value);
        }
    }
    document.getElementById("speedVal").textContent=Math.abs(Math.round(v*8));
    document.getElementById("dirVal").textContent=v>5?"CW":v<-5?"CCW":"IDLE";
},100);

draw();
</script>
</body>
</html>
)rawliteral";

// --- Handlers ---
void handleEnvelope() {
  if (!server.hasArg("plain")) return;
  DynamicJsonDocument doc(4096);
  deserializeJson(doc, server.arg("plain"));
  envelopeSize = doc.size();
  for (int i = 0; i < envelopeSize; i++) {
    envelope[i].time = doc[i]["time"];
    envelope[i].value = doc[i]["value"];
  }
  loopDuration = envelope[envelopeSize - 1].time;
  server.send(200, "text/plain", "OK");
}

int getCurrentSpeedValue() {
  if (envelopeSize < 2) return 0;
  unsigned long t = (millis() - startTime) % loopDuration;
  for (int i = 0; i < envelopeSize - 1; i++) {
    if (t >= envelope[i].time && t <= envelope[i + 1].time) {
      float f = (float)(t - envelope[i].time) / (envelope[i + 1].time - envelope[i].time);
      return envelope[i].value + f * (envelope[i + 1].value - envelope[i].value);
    }
  }
  return 0;
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(motorPins[i], OUTPUT);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println(WiFi.localIP());

  server.on("/", []() { server.send(200, "text/html", htmlPage); });
  server.on("/envelope", HTTP_POST, handleEnvelope);
  server.begin();

  envelope[0] = {0, 0};
  envelope[1] = {8000, 0};
  startTime = millis();
}

void loop() {
  server.handleClient();

  int val = getCurrentSpeedValue(); // -100 tot 100
  float speedFactor = abs(val) / 100.0;
  float currentStepsPerSec = speedFactor * MAX_STEPS_PER_SEC;

  if (currentStepsPerSec > 5.0) { // Drempelwaarde om ruis te voorkomen
    unsigned long stepInterval = 1000000.0 / currentStepsPerSec;
    unsigned long now = micros();

    if (now - lastStepMicros >= stepInterval) {
      lastStepMicros = now;

      // Richting bepalen
      if (val > 0) {
        stepIndex = (stepIndex + 1) % 8;
        motorPosition++;
      } else {
        stepIndex = (stepIndex + 7) % 8;
        motorPosition--;
      }

      // Zet pinnen
      for (int i = 0; i < 4; i++) {
        digitalWrite(motorPins[i], steps[stepIndex][i]);
      }
    }
  } else {
    // Motor stop: zet alle pinnen laag (bespaart stroom/hitte)
    for (int i = 0; i < 4; i++) digitalWrite(motorPins[i], LOW);
  }
}