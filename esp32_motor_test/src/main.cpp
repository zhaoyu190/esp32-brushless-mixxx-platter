#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <math.h>

// M创动工坊 SimpleFOC mini + ESP32:
// IN1/IN2/IN3 -> GPIO25/GPIO26/GPIO27, EN -> GPIO13.
// AS5600 -> SDA GPIO21, SCL GPIO22.
constexpr int PWM_A = 25;
constexpr int PWM_B = 26;
constexpr int PWM_C = 27;
constexpr int ENABLE_PIN = 13;

constexpr int POLE_PAIRS = 7;
constexpr float SUPPLY_VOLTAGE = 12.0f;
constexpr float SPIN_VELOCITY = 2.0f * PI;
constexpr uint8_t AS5600_ADDR = 0x36;
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
constexpr char WIFI_SSID[] = "YOUR_WIFI_SSID";
constexpr char WIFI_PASS[] = "YOUR_WIFI_PASSWORD";
#endif

enum KnobMode {
  MODE_FREE = 0,
  MODE_FOUR_DETENTS = 1,
  MODE_RATCHET = 2,
  MODE_DAMPING = 3,
  MODE_SPRING = 4,
  MODE_VOLUME = 5,
  MODE_SCROLL = 6,
  MODE_DJAY_TURNTABLE = 7,
  MODE_SPIN_TEST = 9,
};

WebServer server(80);
WiFiUDP udp;
Preferences prefs;
IPAddress target_ip(255, 255, 255, 255);
uint16_t target_port = 4210;
String ap_ssid;
String sta_ssid;
String sta_pass;

KnobMode mode = MODE_FREE;
uint8_t phase_order = 0;
float max_uq = 2.0f;
float torque_sign = 1.0f;
float zero_electric_angle = 0.0f;
bool calibrated = false;
float shaft_angle = 0.0f;
unsigned long open_loop_timestamp = 0;
unsigned long mode_started_ms = 0;

uint16_t raw_angle = 0;
uint16_t last_raw_angle = 0;
bool sensor_ok = false;
bool have_sensor_history = false;
float angle_rad = 0.0f;
float angle_unwrapped = 0.0f;
float velocity_rad_s = 0.0f;
float spring_center = 0.0f;
unsigned long last_sensor_us = 0;
float action_center = 0.0f;
float platter_jog_center = 0.0f;
bool platter_touched = false;
bool platter_ready = false;
unsigned long platter_last_touch_ms = 0;
unsigned long platter_last_step_us = 0;
float platter_target_velocity = 0.0f;
float platter_drive_velocity = 0.0f;
unsigned long platter_last_command_ms = 0;
unsigned long platter_ignore_touch_until_ms = 0;
float platter_event_center = 0.0f;
unsigned long platter_drag_started_ms = 0;
unsigned long platter_last_drag_event_ms = 0;
unsigned long platter_spinup_until_ms = 0;
unsigned long platter_drive_last_us = 0;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Haptic Knob</title>
<style>
:root{color-scheme:dark;--bg:#0f1115;--panel:#181b22;--line:#2a303b;--text:#eef2f8;--muted:#8d98aa;--accent:#37d0a5;--warn:#f5b84b}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
main{max-width:840px;margin:0 auto;padding:18px}
h1{font-size:24px;margin:0 0 12px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px;margin:12px 0}
button{height:44px;border:1px solid var(--line);border-radius:7px;background:#222733;color:var(--text);font-weight:650}
button.active{background:var(--accent);border-color:var(--accent);color:#07110e}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
input{height:38px;border:1px solid var(--line);border-radius:6px;background:#10131a;color:var(--text);padding:0 10px}
.kv{display:grid;grid-template-columns:120px 1fr;gap:6px 10px;color:var(--muted)}
.kv b{color:var(--text);font-weight:600}
.small{font-size:13px;color:var(--muted)}
</style></head><body><main>
<h1>Haptic Knob</h1>
<div class="panel">
  <div class="grid">
    <button data-mode="0">自由</button><button data-mode="1">一圈四档</button><button data-mode="2">棘轮</button>
    <button data-mode="3">阻尼</button><button data-mode="4">弹簧回中</button><button data-mode="5">音量旋钮</button>
    <button data-mode="6">滚轮</button><button data-mode="7">djay 转盘</button><button id="spin">一秒一圈测试</button><button id="stop">停用输出</button>
  </div>
</div>
<div class="panel">
  <div class="row">
    <button id="calibrate">校准</button><button id="down">力度 -</button><button id="up">力度 +</button><button id="invert">力矩反向</button>
    <button id="phase">切相序</button><button id="center">重设中心</button>
  </div>
</div>
<div class="panel">
  <div class="row">
    <label>Mac IP <input id="host" placeholder="例如 192.168.4.2"></label>
    <label>端口 <input id="port" value="4210" size="6"></label>
    <button id="saveTarget">保存接收端</button>
  </div>
  <p class="small">Mac 接入 ESP32 热点后，一般可在系统网络详情里看到 Mac IP；接收程序默认监听 4210。</p>
</div>
<div class="panel">
  <div class="row">
    <label>Wi-Fi <input id="ssid" placeholder="路由器 Wi-Fi 名"></label>
    <label>密码 <input id="pass" type="password"></label>
    <button id="saveWifi">连接网络</button>
  </div>
  <p class="small">ESP32 会保留自己的热点，同时尝试连接这个 Wi-Fi；连上后也能用路由器分配的 IP 访问。</p>
</div>
<div class="panel kv">
  <span>模式</span><b id="mode">-</b><span>角度</span><b id="deg">-</b><span>速度</span><b id="vel">-</b>
  <span>力度上限</span><b id="uq">-</b><span>相序</span><b id="phasev">-</b><span>传感器</span><b id="sensor">-</b>
  <span>校准</span><b id="cal">-</b>
  <span>AP</span><b id="ap">-</b><span>目标</span><b id="target">-</b>
  <span>外部 Wi-Fi</span><b id="sta">-</b>
</div>
</main><script>
const names=["自由","一圈四档","棘轮","阻尼","弹簧回中","音量旋钮","滚轮","djay 转盘","","一秒一圈测试"];
async function set(q){await fetch('/api/set?'+q); await refresh();}
document.querySelectorAll('[data-mode]').forEach(b=>b.onclick=()=>set('mode='+b.dataset.mode));
spin.onclick=()=>set('mode=9'); stop.onclick=()=>set('mode=0'); up.onclick=()=>set('uq=up'); down.onclick=()=>set('uq=down');
calibrate.onclick=()=>set('calibrate=1'); invert.onclick=()=>set('invert=1'); phase.onclick=()=>set('phase=next'); center.onclick=()=>set('center=1');
saveTarget.onclick=()=>set('host='+encodeURIComponent(host.value)+'&port='+encodeURIComponent(port.value));
saveWifi.onclick=()=>set('ssid='+encodeURIComponent(ssid.value)+'&pass='+encodeURIComponent(pass.value));
async function refresh(){
  const s=await (await fetch('/api/status')).json();
  mode.textContent=(names[s.mode]||s.mode)+' ('+s.mode+')'; deg.textContent=s.deg.toFixed(1)+' deg'; vel.textContent=s.vel.toFixed(2);
  uq.textContent=s.uq.toFixed(2)+' V'; phasev.textContent=s.phase; sensor.textContent=s.sensor?'OK':'missing';
  cal.textContent=s.calibrated?'OK':'需要校准';
  ap.textContent=s.ap+' / '+s.ip; target.textContent=s.target+':'+s.port; host.value=s.target; port.value=s.port;
  sta.textContent=(s.staConnected?'已连接 ':'未连接 ')+(s.staSsid||'')+' '+(s.staIp||''); ssid.value=s.staSsid||'';
  document.querySelectorAll('[data-mode]').forEach(b=>b.classList.toggle('active', Number(b.dataset.mode)===s.mode));
}
setInterval(refresh,700); refresh();
</script></body></html>
)HTML";

float constrainFloat(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}

float normalizeAngle(float angle) {
  float wrapped = fmod(angle, 2.0f * PI);
  return wrapped >= 0 ? wrapped : wrapped + 2.0f * PI;
}

float wrapPi(float angle) {
  while (angle > PI) angle -= 2.0f * PI;
  while (angle < -PI) angle += 2.0f * PI;
  return angle;
}

float electricalAngle(float mechanical_angle) {
  return mechanical_angle * POLE_PAIRS;
}

void setPwm(float ua, float ub, float uc) {
  float ordered[3] = {ua, ub, uc};
  switch (phase_order) {
    case 1: ordered[0] = ua; ordered[1] = uc; ordered[2] = ub; break;
    case 2: ordered[0] = ub; ordered[1] = ua; ordered[2] = uc; break;
    case 3: ordered[0] = ub; ordered[1] = uc; ordered[2] = ua; break;
    case 4: ordered[0] = uc; ordered[1] = ua; ordered[2] = ub; break;
    case 5: ordered[0] = uc; ordered[1] = ub; ordered[2] = ua; break;
    default: break;
  }
  ledcWrite(0, static_cast<uint32_t>(constrainFloat(ordered[0] / SUPPLY_VOLTAGE, 0.0f, 1.0f) * 255.0f));
  ledcWrite(1, static_cast<uint32_t>(constrainFloat(ordered[1] / SUPPLY_VOLTAGE, 0.0f, 1.0f) * 255.0f));
  ledcWrite(2, static_cast<uint32_t>(constrainFloat(ordered[2] / SUPPLY_VOLTAGE, 0.0f, 1.0f) * 255.0f));
}

void setPhaseVoltageDQ(float uq, float ud, float angle_el) {
  angle_el = normalizeAngle(angle_el);
  float sin_el = sin(angle_el);
  float cos_el = cos(angle_el);
  float ualpha = ud * cos_el - uq * sin_el;
  float ubeta = ud * sin_el + uq * cos_el;
  setPwm(ualpha + SUPPLY_VOLTAGE / 2.0f,
         (sqrt(3.0f) * ubeta - ualpha) / 2.0f + SUPPLY_VOLTAGE / 2.0f,
         (-ualpha - sqrt(3.0f) * ubeta) / 2.0f + SUPPLY_VOLTAGE / 2.0f);
}

void setPhaseVoltage(float uq, float angle_el) {
  setPhaseVoltageDQ(uq, 0.0f, angle_el);
}

bool readAs5600Raw(uint16_t &value) {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(0x0C);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(AS5600_ADDR, static_cast<uint8_t>(2)) != 2) return false;
  uint8_t high = Wire.read();
  uint8_t low = Wire.read();
  value = ((high & 0x0F) << 8) | low;
  return true;
}

bool updateSensor() {
  uint16_t value = 0;
  if (!readAs5600Raw(value)) {
    sensor_ok = false;
    return false;
  }
  unsigned long now_us = micros();
  raw_angle = value;
  angle_rad = raw_angle * 2.0f * PI / 4096.0f;
  if (!have_sensor_history) {
    last_raw_angle = raw_angle;
    angle_unwrapped = angle_rad;
    last_sensor_us = now_us;
    have_sensor_history = true;
    sensor_ok = true;
    return true;
  }
  int delta_counts = static_cast<int>(raw_angle) - static_cast<int>(last_raw_angle);
  if (delta_counts > 2048) delta_counts -= 4096;
  if (delta_counts < -2048) delta_counts += 4096;
  float dt = (now_us - last_sensor_us) * 1e-6f;
  if (dt > 0.0001f && dt < 0.5f) {
    float delta_rad = delta_counts * 2.0f * PI / 4096.0f;
    angle_unwrapped += delta_rad;
    velocity_rad_s = velocity_rad_s * 0.94f + (delta_rad / dt) * 0.06f;
  }
  last_raw_angle = raw_angle;
  last_sensor_us = now_us;
  sensor_ok = true;
  return true;
}

void disableMotor() {
  digitalWrite(ENABLE_PIN, LOW);
  ledcWrite(0, 0);
  ledcWrite(1, 0);
  ledcWrite(2, 0);
}

void enableMotor() {
  digitalWrite(ENABLE_PIN, HIGH);
}

void calibrateElectricalZero() {
  Serial.println("Calibrating electrical zero...");
  enableMotor();
  for (int i = 0; i < 400; i++) {
    setPhaseVoltageDQ(0.0f, 0.8f, 0.0f);
    delay(2);
    updateSensor();
  }
  delay(120);
  updateSensor();
  zero_electric_angle = normalizeAngle(-electricalAngle(angle_rad));
  calibrated = true;
  disableMotor();
  Serial.print("Zero electric angle: ");
  Serial.println(zero_electric_angle, 4);
}

void sendUdpEvent(const char *type, long delta) {
  char payload[96];
  snprintf(payload, sizeof(payload), "{\"type\":\"%s\",\"delta\":%ld,\"raw\":%u}", type, delta, raw_angle);
  udp.beginPacket(target_ip, target_port);
  udp.write(reinterpret_cast<const uint8_t *>(payload), strlen(payload));
  udp.endPacket();
}

void sendUdpValueEvent(const char *type, long value) {
  char payload[112];
  snprintf(payload, sizeof(payload), "{\"type\":\"%s\",\"value\":%ld,\"raw\":%u}", type, value, raw_angle);
  udp.beginPacket(target_ip, target_port);
  udp.write(reinterpret_cast<const uint8_t *>(payload), strlen(payload));
  udp.endPacket();
}

void resetActionIndex() {
  action_center = angle_unwrapped;
}

void resetPlatterState() {
  platter_jog_center = angle_unwrapped;
  platter_touched = false;
  platter_ready = false;
  platter_last_touch_ms = 0;
  platter_last_step_us = micros();
  platter_target_velocity = 0.0f;
  platter_drive_velocity = 0.0f;
  platter_last_command_ms = millis();
  platter_ignore_touch_until_ms = millis() + 1200;
  platter_event_center = angle_unwrapped;
  platter_spinup_until_ms = 0;
  platter_drive_last_us = micros();
}

void setMode(KnobMode new_mode) {
  updateSensor();
  if (new_mode != MODE_FREE && new_mode != MODE_SPIN_TEST && new_mode != MODE_DJAY_TURNTABLE && !calibrated) {
    Serial.println("Run calibration first: press web Calibrate or send 'a'.");
    disableMotor();
    mode = MODE_FREE;
    return;
  }
  mode = new_mode;
  mode_started_ms = millis();
  velocity_rad_s = 0.0f;
  if (mode == MODE_FREE) {
    disableMotor();
  } else {
    enableMotor();
  }
  if (mode == MODE_SPRING) spring_center = angle_unwrapped;
  if (mode == MODE_SPIN_TEST) open_loop_timestamp = micros();
  if (mode == MODE_VOLUME || mode == MODE_SCROLL) resetActionIndex();
  if (mode == MODE_DJAY_TURNTABLE) {
    open_loop_timestamp = micros();
    shaft_angle = normalizeAngle(angle_rad);
    resetPlatterState();
  }
  Serial.print("Mode: ");
  Serial.println(static_cast<int>(mode));
}

float detentTorque(float detent_width, float strength, float damping) {
  float phase = 2.0f * PI * angle_unwrapped / detent_width;
  float deadband = detent_width * 0.018f;
  float nearest = round(angle_unwrapped / detent_width) * detent_width;
  float error = nearest - angle_unwrapped;
  float velocity = fabs(velocity_rad_s) < 0.18f ? 0.0f : velocity_rad_s;
  if (fabs(error) < deadband) {
    return -damping * velocity;
  }
  return -strength * sin(phase) - damping * velocity;
}

float calculateHapticTorque() {
  float velocity = fabs(velocity_rad_s) < 0.18f ? 0.0f : velocity_rad_s;
  switch (mode) {
    case MODE_FOUR_DETENTS: return detentTorque(PI / 2.0f, 1.8f, 0.028f);
    case MODE_RATCHET: return detentTorque(2.0f * PI / 24.0f, 1.25f, 0.022f);
    case MODE_DAMPING: return -0.095f * velocity;
    case MODE_SPRING: return 0.7f * wrapPi(spring_center - angle_unwrapped) - 0.018f * velocity;
    case MODE_VOLUME:
    case MODE_SCROLL: return detentTorque(2.0f * PI / 24.0f, 1.05f, 0.02f);
    default: return 0.0f;
  }
}

void velocityOpenloop(float velocity, float uq) {
  unsigned long now_us = micros();
  float ts = (now_us - open_loop_timestamp) * 1e-6f;
  if (ts <= 0.0f || ts > 0.5f) ts = 1e-3f;
  shaft_angle = normalizeAngle(shaft_angle + velocity * ts);
  setPhaseVoltage(constrainFloat(uq, 0.25f, 3.0f), electricalAngle(shaft_angle));
  open_loop_timestamp = now_us;
}

void velocityOpenloop(float velocity) {
  velocityOpenloop(velocity, max_uq);
}

float updatePlatterDriveVelocity() {
  unsigned long now_us = micros();
  float dt = (now_us - platter_drive_last_us) * 1e-6f;
  if (dt <= 0.0f || dt > 0.2f) dt = 0.001f;
  platter_drive_last_us = now_us;

  float delta = platter_target_velocity - platter_drive_velocity;
  float max_step = 12.0f * dt;
  if (millis() < platter_spinup_until_ms) max_step = 18.0f * dt;
  delta = constrainFloat(delta, -max_step, max_step);
  platter_drive_velocity += delta;
  if (fabs(platter_target_velocity) < 0.05f) platter_drive_velocity = 0.0f;
  return platter_drive_velocity;
}

void handleActionEvents() {
  if (mode != MODE_VOLUME && mode != MODE_SCROLL) return;
  float step = 2.0f * PI / 24.0f;
  float diff = angle_unwrapped - action_center;
  if (diff > step) {
    action_center += step;
    sendUdpEvent(mode == MODE_VOLUME ? "volume" : "scroll", 1);
  } else if (diff < -step) {
    action_center -= step;
    sendUdpEvent(mode == MODE_VOLUME ? "volume" : "scroll", -1);
  }
}

void handleUdpCommands() {
  int packet_size = udp.parsePacket();
  if (packet_size <= 0) return;
  char payload[160];
  int len = udp.read(payload, sizeof(payload) - 1);
  if (len <= 0) return;
  payload[len] = '\0';

  char *velocity_key = strstr(payload, "\"velocity\"");
  if (!velocity_key) velocity_key = strstr(payload, "velocity");
  if (velocity_key) {
    char *separator = strchr(velocity_key, ':');
    if (separator) {
      float velocity = atof(separator + 1);
      float new_velocity = constrainFloat(velocity, -16.0f, 16.0f);
      unsigned long now_ms = millis();
      if (mode != MODE_DJAY_TURNTABLE) setMode(MODE_DJAY_TURNTABLE);
      bool starting_platter = fabs(platter_target_velocity) < 0.2f && fabs(new_velocity) > 0.2f;
      if (fabs(new_velocity - platter_target_velocity) > 0.05f) {
        platter_ignore_touch_until_ms = now_ms + 1200;
      }
      if (starting_platter) {
        platter_drive_velocity = 0.0f;
        platter_drive_last_us = micros();
        platter_spinup_until_ms = now_ms + 1600;
        platter_ignore_touch_until_ms = now_ms + 1800;
        platter_drag_started_ms = 0;
        platter_event_center = angle_unwrapped;
        open_loop_timestamp = micros();
      }
      platter_target_velocity = new_velocity;
      platter_last_command_ms = now_ms;
      Serial.printf("Platter target velocity: %.3f rad/s\n", platter_target_velocity);
    }
  }
}

void handlePlatterMidiEvents() {
  if (mode != MODE_DJAY_TURNTABLE || !sensor_ok) return;
  unsigned long now_us = micros();
  float dt = (now_us - platter_last_step_us) * 1e-6f;
  platter_last_step_us = now_us;
  if (dt <= 0.0f || dt > 0.2f) dt = 0.001f;

  // Give the sensor velocity filter a short moment to settle after mode entry.
  if (!platter_ready) {
    if (millis() - mode_started_ms > 900) {
      platter_ready = true;
      platter_jog_center = angle_unwrapped;
      platter_event_center = angle_unwrapped;
    }
    return;
  }

  if (millis() < platter_ignore_touch_until_ms) {
    platter_jog_center = angle_unwrapped;
    platter_event_center = angle_unwrapped;
    platter_drag_started_ms = 0;
    return;
  }

  float expected_velocity = fabs(platter_target_velocity) > 0.2f ? platter_target_velocity : 0.0f;
  if (fabs(expected_velocity) > 0.2f) {
    float signed_error = expected_velocity - velocity_rad_s;
    float same_direction_velocity = expected_velocity > 0.0f ? velocity_rad_s : -velocity_rad_s;
    bool dragging_slower = same_direction_velocity < fabs(expected_velocity) - 2.2f;
    bool nearly_stopped_by_hand = same_direction_velocity < fabs(expected_velocity) * 0.45f;
    unsigned long now_ms = millis();

    platter_event_center = angle_unwrapped;
    if (!dragging_slower && !nearly_stopped_by_hand) {
      platter_drag_started_ms = 0;
      return;
    }

    if (platter_drag_started_ms == 0) {
      platter_drag_started_ms = now_ms;
      return;
    }
    if (now_ms - platter_drag_started_ms < 120 || now_ms - platter_last_drag_event_ms < 35) {
      return;
    }

    int drag_ticks = static_cast<int>(fabs(signed_error) * 0.65f);
    drag_ticks = constrain(drag_ticks, 1, 5);
    if (signed_error > 0.0f) drag_ticks = -drag_ticks;
    platter_last_drag_event_ms = now_ms;
    sendUdpEvent("midi_platter", drag_ticks);
    return;
  }

  platter_drag_started_ms = 0;

  float step = 2.0f * PI / 96.0f;
  float diff = angle_unwrapped - platter_event_center;
  int ticks = static_cast<int>(diff / step);
  ticks = constrain(ticks, -8, 8);
  if (ticks != 0) {
    platter_event_center += ticks * step;
    sendUdpEvent("midi_platter", ticks);
  }
}

void applyHaptics() {
  if (mode == MODE_FREE) return;
  if (mode == MODE_SPIN_TEST) {
    if (millis() - mode_started_ms > 3000) {
      setMode(MODE_FREE);
      return;
    }
    velocityOpenloop(SPIN_VELOCITY);
    return;
  }
  if (mode == MODE_DJAY_TURNTABLE) {
    if (!sensor_ok) {
      disableMotor();
      return;
    }
    if (millis() - platter_last_command_ms > 1500) {
      platter_target_velocity = 0.0f;
    }
    if (fabs(platter_target_velocity) < 0.05f) {
      platter_drive_velocity = 0.0f;
      platter_spinup_until_ms = 0;
      disableMotor();
      return;
    }
    enableMotor();
    float drive_velocity = updatePlatterDriveVelocity();
    float drive_uq = max_uq;
    if (millis() < platter_spinup_until_ms) {
      drive_uq = constrainFloat(max_uq + 0.35f, max_uq, 2.6f);
    }
    velocityOpenloop(drive_velocity, drive_uq);
    return;
  }
  if (!sensor_ok) {
    disableMotor();
    return;
  }
  float torque_uq = constrainFloat(calculateHapticTorque() * torque_sign, -max_uq, max_uq);
  setPhaseVoltage(torque_uq, electricalAngle(angle_rad) + zero_electric_angle);
}

void handleSerial() {
  static bool waiting_for_mode_number = false;
  while (Serial.available()) {
    char ch = Serial.read();
    if (waiting_for_mode_number) {
      waiting_for_mode_number = false;
      if (ch >= '0' && ch <= '7') setMode(static_cast<KnobMode>(ch - '0'));
      continue;
    }
    if (ch == 'm' || ch == 'M') waiting_for_mode_number = true;
    else if (ch >= '0' && ch <= '7') setMode(static_cast<KnobMode>(ch - '0'));
    else if (ch == 'r' || ch == 'R') setMode(MODE_SPIN_TEST);
    else if (ch == 's' || ch == 'S') setMode(MODE_FREE);
    else if (ch == 'u' || ch == 'U') { max_uq = constrainFloat(max_uq + 0.25f, 0.25f, 3.0f); Serial.printf("Max Uq: %.2f\n", max_uq); }
    else if (ch == 'd' || ch == 'D') { max_uq = constrainFloat(max_uq - 0.25f, 0.25f, 3.0f); Serial.printf("Max Uq: %.2f\n", max_uq); }
    else if (ch == 'p' || ch == 'P') { phase_order = (phase_order + 1) % 6; Serial.printf("Phase order: %u\n", phase_order); }
    else if (ch == 'i' || ch == 'I') { torque_sign = -torque_sign; Serial.printf("Torque sign: %.0f\n", torque_sign); }
    else if (ch == 'a' || ch == 'A') calibrateElectricalZero();
    else if (ch == 'c' || ch == 'C') { spring_center = angle_unwrapped; Serial.println("Spring center reset"); }
  }
}

String ipToString(IPAddress ip) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return String(buf);
}

void sendStatus() {
  String json = "{";
  json += "\"mode\":" + String(static_cast<int>(mode));
  json += ",\"uq\":" + String(max_uq, 2);
  json += ",\"phase\":" + String(phase_order);
  json += ",\"sign\":" + String(torque_sign, 0);
  json += ",\"raw\":" + String(raw_angle);
  json += ",\"deg\":" + String(angle_rad * 180.0f / PI, 1);
  json += ",\"vel\":" + String(velocity_rad_s, 2);
  json += ",\"targetVel\":" + String(platter_target_velocity, 3);
  json += ",\"sensor\":" + String(sensor_ok ? 1 : 0);
  json += ",\"calibrated\":" + String(calibrated ? 1 : 0);
  json += ",\"zero\":" + String(zero_electric_angle, 4);
  json += ",\"ap\":\"" + ap_ssid + "\"";
  json += ",\"ip\":\"" + ipToString(WiFi.softAPIP()) + "\"";
  json += ",\"target\":\"" + ipToString(target_ip) + "\"";
  json += ",\"port\":" + String(target_port);
  json += ",\"staConnected\":" + String(WiFi.status() == WL_CONNECTED ? 1 : 0);
  json += ",\"staSsid\":\"" + sta_ssid + "\"";
  json += ",\"staIp\":\"" + (WiFi.status() == WL_CONNECTED ? ipToString(WiFi.localIP()) : String("")) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleSet() {
  if (server.hasArg("mode")) {
    int value = server.arg("mode").toInt();
    if ((value >= 0 && value <= 7) || value == 9) setMode(static_cast<KnobMode>(value));
  }
  if (server.hasArg("uq")) {
    String value = server.arg("uq");
    if (value == "up") max_uq = constrainFloat(max_uq + 0.25f, 0.25f, 3.0f);
    else if (value == "down") max_uq = constrainFloat(max_uq - 0.25f, 0.25f, 3.0f);
    else max_uq = constrainFloat(value.toFloat(), 0.25f, 3.0f);
  }
  if (server.hasArg("phase")) phase_order = (phase_order + 1) % 6;
  if (server.hasArg("invert")) torque_sign = -torque_sign;
  if (server.hasArg("calibrate")) calibrateElectricalZero();
  if (server.hasArg("center")) spring_center = angle_unwrapped;
  if (server.hasArg("host")) target_ip.fromString(server.arg("host"));
  if (server.hasArg("port")) target_port = constrain(server.arg("port").toInt(), 1, 65535);
  if (server.hasArg("ssid")) {
    sta_ssid = server.arg("ssid");
    sta_pass = server.arg("pass");
    prefs.putString("ssid", sta_ssid);
    prefs.putString("pass", sta_pass);
    WiFi.disconnect();
    if (sta_ssid.length() > 0) {
      WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());
    }
  }
  sendStatus();
}

void setupWiFi() {
  prefs.begin("knob", false);
  sta_ssid = WIFI_SSID;
  sta_pass = WIFI_PASS;
  prefs.putString("ssid", sta_ssid);
  prefs.putString("pass", sta_pass);
  uint32_t chip = static_cast<uint32_t>(ESP.getEfuseMac());
  char ssid[24];
  snprintf(ssid, sizeof(ssid), "HapticKnob-%04X", chip & 0xFFFF);
  ap_ssid = ssid;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid.c_str(), "12345678");
  if (sta_ssid.length() > 0) {
    Serial.print("Connecting STA Wi-Fi: ");
    Serial.println(sta_ssid);
    WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());
  }
  udp.begin(target_port);
  server.on("/", []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/status", sendStatus);
  server.on("/api/set", handleSet);
  server.begin();
}

void printSensorStatus() {
  static unsigned long last_print = 0;
  if (millis() - last_print < 1000) return;
  String sta_ip = WiFi.status() == WL_CONNECTED ? ipToString(WiFi.localIP()) : "-";
  Serial.printf("mode=%d uq=%.2f sign=%.0f phase=%u cal=%d zero=%.3f raw=%u deg=%.1f vel=%.2f sensor=%d ap=%s ap_ip=%s sta_ip=%s target=%s:%u\n",
                static_cast<int>(mode), max_uq, torque_sign, phase_order,
                calibrated ? 1 : 0, zero_electric_angle, raw_angle,
                angle_rad * 180.0f / PI, velocity_rad_s, sensor_ok ? 1 : 0,
                ap_ssid.c_str(), ipToString(WiFi.softAPIP()).c_str(),
                sta_ip.c_str(),
                ipToString(target_ip).c_str(), target_port);
  last_print = millis();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(21, 22);
  Wire.setClock(400000);
  pinMode(PWM_A, OUTPUT);
  pinMode(PWM_B, OUTPUT);
  pinMode(PWM_C, OUTPUT);
  pinMode(ENABLE_PIN, OUTPUT);
  ledcSetup(0, 30000, 8);
  ledcSetup(1, 30000, 8);
  ledcSetup(2, 30000, 8);
  ledcAttachPin(PWM_A, 0);
  ledcAttachPin(PWM_B, 1);
  ledcAttachPin(PWM_C, 2);
  disableMotor();
  updateSensor();
  setupWiFi();
  Serial.println("ESP32 haptic knob ready.");
  Serial.println("AP password: 12345678, open http://192.168.4.1");
  Serial.println("Modes: m0/free, m1/4-detents, m2/ratchet, m3/damping, m4/spring, m5/volume, m6/scroll, m7/djay-turntable");
  Serial.println("Run calibration first: send 'a' or press Calibrate on web page.");
}

void loop() {
  updateSensor();
  handleSerial();
  server.handleClient();
  handleUdpCommands();
  handleActionEvents();
  handlePlatterMidiEvents();
  applyHaptics();
  printSensorStatus();
}
