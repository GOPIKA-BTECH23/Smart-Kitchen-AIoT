#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <HX711.h>
#include "model.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/micro/system_setup.h"

// WiFi credentials
const char* ssid = "One+";
const char* password = "banajena240905";

// Pin definitions
#define DHT_PIN     4
#define DHT_TYPE    DHT22
#define MQ2_PIN     34
#define HX711_DT    16
#define HX711_SCK   5
#define TRIG_PIN    13
#define ECHO_PIN    12
#define RELAY_PIN   27
#define LED_PIN     2

DHT dht(DHT_PIN, DHT_TYPE);
HX711 scale;
WebServer server(80);

// TensorFlow Lite globals
constexpr int kTensorArenaSize = 50 * 1024;
static uint8_t tensor_arena[kTensorArenaSize];
const tflite::Model* tfModel = nullptr;
tflite::MicroInterpreter* tfInterpreter = nullptr;
TfLiteTensor* tfInput = nullptr;
TfLiteTensor* tfOutput = nullptr;
bool model_initialized = false;
String last_error = "";

// Normalization (update as per training)
const float min_vals[5] = {20.005821, 30.029383, 100.000000, 0.237705, 2.087296};
const float max_vals[5] = {44.992456, 89.934978, 1299.947479, 1999.771729, 399.991531};

int baselineGas = 0;

struct SensorStats {
  float last_temp = 0;
  float last_hum = 0;
  int last_gas = 0;
  float last_weight = 0;
  float last_dist = 0;
  float last_pred = 0;
} stats;

// Utility functions
float normalize_feature(float v, int idx) {
  float mn = min_vals[idx], mx = max_vals[idx];
  if (isnan(v)) v = mn;
  if (v < mn) v = mn;
  if (v > mx) v = mx;
  return (v - mn) / (mx - mn);
}

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 0.0;
  return (duration * 0.034) / 2.0;
}

float run_inference(float norm_inputs[5]) {
  if (!model_initialized) {
    last_error = "Model not initialized";
    return 0.0;
  }
  if (tfInput->type == kTfLiteInt8) {
    float scale = tfInput->params.scale;
    int zp = tfInput->params.zero_point;
    for (int i = 0; i < 5; i++) {
      int8_t q = (int8_t)(norm_inputs[i] / scale + zp);
      tfInput->data.int8[i] = q;
    }
  } else {
    for (int i = 0; i < 5; i++) tfInput->data.f[i] = norm_inputs[i];
  }
  TfLiteStatus invoke_status = tfInterpreter->Invoke();
  if (invoke_status != kTfLiteOk) {
    last_error = "Invoke failed";
    return 0.0;
  }
  float prediction = 0.0;
  if (tfOutput->type == kTfLiteInt8) {
    int int8_val = tfOutput->data.int8[0];
    prediction = (int8_val - tfOutput->params.zero_point) * tfOutput->params.scale;
  } else {
    prediction = tfOutput->data.f[0];
  }
  last_error = "OK";
  return prediction;
}

String jsonResponse(float temp, float hum, int gas, float weight, float dist, float pred, int relay, int led) {
  String s = "{";
  s += "\"temperature\":" + String(temp, 2) + ",";
  s += "\"humidity\":" + String(hum, 2) + ",";
  s += "\"gas_raw\":" + String(gas) + ",";
  s += "\"weight\":" + String(weight, 2) + ",";
  s += "\"distance\":" + String(dist, 2) + ",";
  s += "\"prediction\":" + String(pred, 4) + ",";
  s += "\"relay\":" + String(relay) + ",";
  s += "\"led\":" + String(led) + ",";
  s += "\"model_status\":\"" + last_error + "\"";
  s += "}";
  return s;
}

// --- Web UI ---
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Smart Kitchen IoT</title>
<style>
  body { background:#000; color:#fff; font-family:'Poppins',sans-serif; margin:0; display:flex; justify-content:center; align-items:center; min-height:100vh; }
  .container { text-align:center; width:90%; max-width:900px; padding:30px; border-radius:20px; background:linear-gradient(145deg,#0c0c0c,#1a1a1a); box-shadow:0 0 25px rgba(255,20,147,0.3); }
  h1 { color:#ff1d8e; font-size:2.4em; margin-bottom:8px; text-shadow:0 0 10px #ff1d8e; }
  .subtitle { color:#bbb; margin-bottom:30px; font-size:1.05em; }
  .metrics { display:grid; grid-template-columns:repeat(auto-fit,minmax(130px,1fr)); gap:20px; }
  .metric { background:#111; border-radius:15px; padding:15px; box-shadow:inset 0 0 10px rgba(255,255,255,0.05),0 0 10px rgba(255,20,147,0.2); transition:transform 0.3s, box-shadow 0.3s; }
  .metric:hover { transform:translateY(-5px); box-shadow:0 0 15px rgba(255,20,147,0.4); }
  .label { font-size:0.9em; color:#aaa; margin-bottom:5px; }
  .value { font-size:1.3em; font-weight:700; color:#00ffb3; text-shadow:0 0 10px #00ffb3; }
  .pred-card { margin-top:30px; padding:25px; border-radius:15px; background:radial-gradient(circle at center,#111,#000); box-shadow:0 0 20px rgba(255,20,147,0.2); }
  .score { font-size:2.2em; font-weight:900; transition:color 0.3s, text-shadow 0.3s; }
  .safe { color:#00ffb3; text-shadow:0 0 15px #00ffb3; }
  .hazard { color:#ff4b4b; text-shadow:0 0 20px #ff4b4b; }
  .status { font-weight:600; margin-top:10px; font-size:1.1em; }
  .actuators { display:flex; justify-content:center; gap:20px; margin-top:20px; }
  .act { padding:10px 25px; border-radius:12px; font-weight:600; transition:background 0.3s, transform 0.3s; }
  .on { background:#00ffb3; color:#000; box-shadow:0 0 10px #00ffb3; }
  .off { background:#ff4b4b; color:#fff; box-shadow:0 0 10px #ff4b4b; }
  .act:hover { transform:scale(1.05); }
</style>
</head>
<body>
  <div class='container'>
    <h1>Smart Kitchen IoT</h1>
    <div class='subtitle'>AI-Powered Real-Time Hazard Dashboard</div>
    <div class='metrics'>
      <div class='metric'><div class='label'>Temp</div><div class='value' id='temp'>-- °C</div></div>
      <div class='metric'><div class='label'>Humidity</div><div class='value' id='hum'>-- %</div></div>
      <div class='metric'><div class='label'>Gas</div><div class='value' id='gas'>-- ppm</div></div>
      <div class='metric'><div class='label'>Weight</div><div class='value' id='weight'>-- g</div></div>
      <div class='metric'><div class='label'>Distance</div><div class='value' id='dist'>-- cm</div></div>
    </div>
    <div class='pred-card'>
      <div class='label'>Hazard Score</div>
      <div id='pred' class='score safe'>0.0000</div>
      <div id='status' class='status'>Environment Safe</div>
      <div class='actuators'>
        <div id='relay' class='act off'>Relay: OFF</div>
        <div id='led' class='act off'>LED: OFF</div>
      </div>
    </div>
  </div>
<script>
function fetchData(){
  fetch('/data').then(r=>r.json()).then(j=>{
    document.getElementById('temp').innerHTML = j.temperature.toFixed(1)+' °C';
    document.getElementById('hum').innerHTML = j.humidity.toFixed(1)+' %';
    document.getElementById('gas').innerHTML = j.gas_raw+' ppm';
    document.getElementById('weight').innerHTML = j.weight.toFixed(1)+' g';
    document.getElementById('dist').innerHTML = j.distance.toFixed(1)+' cm';
    let pred = j.prediction;
    let predDiv = document.getElementById('pred');
    let stat = document.getElementById('status');
    if(pred>0.5){
      predDiv.textContent = pred.toFixed(4);
      predDiv.className = 'score hazard';
      stat.textContent = '⚠️ Hazard Detected!';
      stat.style.color = '#ff4b4b';
    } else {
      predDiv.textContent = pred.toFixed(4);
      predDiv.className = 'score safe';
      stat.textContent = '✅ Environment Safe';
      stat.style.color = '#00ffb3';
    }
    ['relay','led'].forEach(id=>{
      const el=document.getElementById(id);
      if(j[id]){
        el.className='act on';
        el.textContent=id.toUpperCase()+': ON';
      } else {
        el.className='act off';
        el.textContent=id.toUpperCase()+': OFF';
      }
    });
  });
}
setInterval(fetchData,2000);
fetchData();
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

// Handle /data endpoint
void handleData() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int g = analogRead(MQ2_PIN);
  float w = scale.is_ready() ? scale.get_units(5) : 0.0;
  float d = readDistanceCm();

  float norm_inputs[5] = {
    normalize_feature(t,0),
    normalize_feature(h,1),
    normalize_feature(g,2),
    normalize_feature(w,3),
    normalize_feature(d,4)
  };

  float pred = run_inference(norm_inputs);
  int relayState = pred > 0.5;
  int ledState = pred > 0.5;

  digitalWrite(RELAY_PIN, relayState);
  digitalWrite(LED_PIN, ledState);

  String js = jsonResponse(t,h,g,w,d,pred,relayState,ledState);
  server.send(200, "application/json", js);
}

// Setup
void setup() {
  Serial.begin(115200);
  dht.begin();
  scale.begin(HX711_DT, HX711_SCK);
  scale.set_scale(); scale.tare();

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println(WiFi.localIP());

  tflite::InitializeTarget();
  tfModel = tflite::GetModel(_content_model_tflite);
  static tflite::MicroMutableOpResolver<10> resolver;
  resolver.AddFullyConnected(); resolver.AddLogistic(); resolver.AddMul(); resolver.AddAdd();
  resolver.AddReshape(); resolver.AddSoftmax(); resolver.AddRelu(); resolver.AddAveragePool2D(); resolver.AddConv2D(); resolver.AddConcatenation();
  static tflite::MicroInterpreter staticInterpreter(tfModel, resolver, tensor_arena, kTensorArenaSize);
  tfInterpreter = &staticInterpreter;
  tfInterpreter->AllocateTensors();
  tfInput = tfInterpreter->input(0);
  tfOutput = tfInterpreter->output(0);
  model_initialized = true;

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void loop() { server.handleClient(); }