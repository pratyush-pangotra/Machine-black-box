/*
  Machine Black Box - main firmware v2.0

  Records what the machine was doing before it failed:
    - Vibration : MPU6050 read 100 times a second, reduced to 10 vibration-level values a second
    - Current   : SCT-013-030 clamp through the ADS1115 (A0 - A1), RMS over each 100 ms
    - Temperature: DS18B20, once a second
  Every 100 ms one record {vibration, current, temperature} goes into a rolling 30 s buffer.
  When a trigger rule fires, the buffer is frozen, 10 s more are recorded, and the whole
  40 s fault record is sent to the dashboard.

  Trigger rules (thresholds are set from the dashboard and kept in flash):
    vibration level > vibration threshold   (set by "Calibrate baseline" or by hand)
    current > current threshold             (0 = off)
    temperature > temperature threshold     (0 = off)
    "Capture now" button on the dashboard   (manual record, for tests and demos)

  Output: one JSON object per line on USB serial (460800 baud), and the same messages over MQTT:
    mbb/<id>/status  "online" / "offline" (Last Will, retained)
    mbb/<id>/live    every 100 ms
    mbb/<id>/health  every 2 s
    mbb/<id>/event   each fault record (retained: the latest one)
    mbb/<id>/msg     trigger, calibration and command replies
    mbb/<id>/cmd     commands in: capture | calibrate | set vib X | set cur X | set temp X | turns N | status
  The same commands can be typed into Serial Monitor.

  Wiring (zero PCB):
    MPU6050  SDA GPIO21, SCL GPIO22                       (I2C bus 0)
    ADS1115  SDA GPIO32, SCL GPIO33, ADDR GND (0x48)      (I2C bus 1)
             A0 = clamp red, A1 = 1.65 V midpoint (clamp white)
    DS18B20  DATA GPIO4, 4.7k to 3.3 V

  Board: ESP32 Dev Module.  Libraries: PubSubClient (Nick O'Leary), OneWire (Paul Stoffregen).
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <OneWire.h>
#include <Preferences.h>
#include <stdarg.h>
#include <math.h>

// =====================================================================
// Settings
// Your WiFi name, password and the laptop's IP go in secrets.h, which Git ignores.
// Copy secrets.example.h to secrets.h (same folder) and fill it in.
// Without secrets.h the firmware builds for the USB cable only.
// =====================================================================
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define MBB_WIFI_SSID     ""                 // empty = USB cable only
  #define MBB_WIFI_PASSWORD ""
  #define MBB_MQTT_HOST     "192.168.43.100"
#endif

const char*    WIFI_SSID     = MBB_WIFI_SSID;
const char*    WIFI_PASSWORD = MBB_WIFI_PASSWORD;
const char*    MQTT_HOST     = MBB_MQTT_HOST;      // the laptop's IP address on the hotspot
const uint16_t MQTT_PORT     = 1883;

const char*    DEVICE_ID     = "mbb-01";
const char*    FW_VERSION    = "2.0";
const unsigned long SERIAL_BAUD = 460800;

// Pins and addresses
const int     MPU_SDA = 21, MPU_SCL = 22;
const int     ADS_SDA = 32, ADS_SCL = 33;
const int     PIN_ONEWIRE = 4;
const uint8_t ADS_ADDR = 0x48;

// Recorder
const int RATE_HZ      = 10;                    // records per second
const int PRE_SECONDS  = 30;                    // kept before a trigger
const int POST_SECONDS = 10;                    // recorded after a trigger
const int PRE_N  = PRE_SECONDS * RATE_HZ;       // 300
const int POST_N = POST_SECONDS * RATE_HZ;      // 100
const unsigned long COOLDOWN_MS = 5000;         // pause after a record before the next trigger
const unsigned long WARMUP_MS   = 3000;         // no triggers right after power-up
const int CAL_SECONDS = 20;                     // baseline calibration length
const float CAL_SIGMAS = 6.0f;                  // threshold = baseline mean + 6 x standard deviation

// Defaults until changed from the dashboard (then kept in flash)
const float DEFAULT_THR_VIB  = 0.05f;           // g
const float DEFAULT_THR_CUR  = 0.0f;            // A, 0 = off
const float DEFAULT_THR_TEMP = 0.0f;            // deg C, 0 = off
const float CLAMP_AMPS_PER_VOLT = 30.0f;        // SCT-013-030: 1 V RMS = 30 A

// =====================================================================
// Types (kept above all functions for the Arduino IDE)
// =====================================================================
struct Rec { float vib; float cur; float temp; };

enum RecState { ST_MONITOR, ST_POST, ST_COOLDOWN, ST_CALIBRATE };

// =====================================================================
// Globals
// =====================================================================
WiFiClient   net;
PubSubClient mqtt(net);
OneWire      oneWire(PIN_ONEWIRE);
Preferences  prefs;

char tStatus[48], tLive[48], tHealth[48], tEvent[48], tMsg[48], tCmd[48];

// sensors
bool    mpuOk = false, adsOk = false, dsOk = false, adsStreaming = false;
uint8_t mpuAddr = 0, mpuWho = 0;
int     mpuFails = 0, adsFails = 0;

// settings
float    thrVib = DEFAULT_THR_VIB, thrCur = DEFAULT_THR_CUR, thrTemp = DEFAULT_THR_TEMP;
int      clampTurns = 1;
uint32_t eventCounter = 0;

// sampling accumulators for the current 100 ms window
unsigned long nextMpuUs = 0, nextAdsUs = 0, windowStartMs = 0;
double accMagSum = 0, accMagSq = 0, axSum = 0, aySum = 0, azSum = 0;
int    accN = 0;
double curSum = 0, curSq = 0;
int    curN = 0;

float lastTemp = NAN, midVolts = NAN;
bool  dsConverting = false;
unsigned long dsStartedAt = 0, lastDsMs = 0, lastMidMs = 0, lastHealthMs = 0, lastRetryMs = 0, lastMqttTry = 0;

// rolling buffer and fault record
Rec ring[PRE_N];
int ringHead = 0, ringCount = 0;
Rec evBuf[PRE_N + POST_N];
int evPre = 0, evPost = 0;
uint32_t evId = 0;
unsigned long evTriggerMs = 0;
char evReason[16] = "";

RecState state = ST_MONITOR;
unsigned long stateSinceMs = 0;
bool manualCapture = false;

// calibration
double calSum = 0, calSq = 0;
int    calN = 0;

// commands
char pendingCmd[64];
bool cmdPending = false;
char serialLine[64];
int  serialLen = 0;

// connectivity
bool wifiEnabled = false, lastWifiUp = false;

// JSON output buffer (a fault record is about 9 KB)
char   jbuf[14000];
size_t jlen = 0;

// =====================================================================
// JSON helpers
// =====================================================================
void jreset() { jlen = 0; jbuf[0] = '\0'; }

void jadd(const char* fmt, ...) {
  if (jlen >= sizeof(jbuf) - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(jbuf + jlen, sizeof(jbuf) - jlen, fmt, ap);
  va_end(ap);
  if (n > 0) {
    jlen += (size_t)n;
    if (jlen > sizeof(jbuf) - 1) jlen = sizeof(jbuf) - 1;
  }
}

void jfloat(float v, int decimals) {
  if (isnan(v) || isinf(v)) jadd("null");
  else jadd("%.*f", decimals, v);
}

// Send the JSON in jbuf to USB serial and, if connected, to MQTT.
void emit(const char* topic, bool retained) {
  Serial.write((const uint8_t*)jbuf, jlen);
  Serial.write('\n');
  if (mqtt.connected()) {
    if (mqtt.beginPublish(topic, jlen, retained)) {
      mqtt.write((const uint8_t*)jbuf, jlen);
      mqtt.endPublish();
    }
  }
}

void sendMsg(bool ok, const char* text) {
  jreset();
  jadd("{\"type\":\"ack\",\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", text);
  emit(tMsg, false);
}

// =====================================================================
// I2C helpers
// =====================================================================
bool i2cPresent(TwoWire &w, uint8_t addr) {
  w.beginTransmission(addr);
  return w.endTransmission() == 0;
}

bool writeReg8(TwoWire &w, uint8_t addr, uint8_t reg, uint8_t val) {
  w.beginTransmission(addr);
  w.write(reg);
  w.write(val);
  return w.endTransmission() == 0;
}

bool writeReg16(TwoWire &w, uint8_t addr, uint8_t reg, uint16_t val) {
  w.beginTransmission(addr);
  w.write(reg);
  w.write((uint8_t)(val >> 8));
  w.write((uint8_t)(val & 0xFF));
  return w.endTransmission() == 0;
}

bool readRegs(TwoWire &w, uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission() != 0) return false;
  if (w.requestFrom(addr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = w.read();
  return true;
}

bool readReg16(TwoWire &w, uint8_t addr, uint8_t reg, uint16_t &val) {
  uint8_t b[2];
  if (!readRegs(w, addr, reg, b, 2)) return false;
  val = ((uint16_t)b[0] << 8) | b[1];
  return true;
}

// =====================================================================
// MPU6050
// =====================================================================
bool mpuInit() {
  mpuAddr = 0;
  if (i2cPresent(Wire, 0x68))      mpuAddr = 0x68;
  else if (i2cPresent(Wire, 0x69)) mpuAddr = 0x69;
  if (!mpuAddr) return false;
  if (!readRegs(Wire, mpuAddr, 0x75, &mpuWho, 1)) return false;
  bool ok = true;
  ok &= writeReg8(Wire, mpuAddr, 0x6B, 0x80);   // reset
  delay(100);
  ok &= writeReg8(Wire, mpuAddr, 0x6B, 0x01);   // wake, gyro clock
  delay(10);
  ok &= writeReg8(Wire, mpuAddr, 0x1A, 0x03);   // low-pass ~44 Hz
  ok &= writeReg8(Wire, mpuAddr, 0x1B, 0x00);   // gyro +/-250 deg/s
  ok &= writeReg8(Wire, mpuAddr, 0x1C, 0x00);   // accel +/-2 g
  if (!ok) return false;
  mpuFails = 0;
  Serial.printf("# MPU6050 ready at 0x%02X (WHO_AM_I 0x%02X)\n", mpuAddr, mpuWho);
  return true;
}

void sampleMpu() {
  uint8_t b[14];
  bool ok = readRegs(Wire, mpuAddr, 0x3B, b, 6);       // accel X, Y, Z only
  if (ok) {
    bool allSame = true;
    for (int i = 1; i < 6; i++) if (b[i] != b[0]) allSame = false;
    if (allSame) ok = false;
  }
  if (!ok) {
    if (++mpuFails >= 5) { mpuOk = false; Serial.println("# MPU6050 lost"); }
    return;
  }
  mpuFails = 0;
  float ax = (int16_t)(((uint16_t)b[0] << 8) | b[1]) / 16384.0f;
  float ay = (int16_t)(((uint16_t)b[2] << 8) | b[3]) / 16384.0f;
  float az = (int16_t)(((uint16_t)b[4] << 8) | b[5]) / 16384.0f;
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  accMagSum += mag;
  accMagSq  += (double)mag * mag;
  axSum += ax; aySum += ay; azSum += az;
  accN++;
}

// =====================================================================
// ADS1115
// =====================================================================

// Continuous mode, A0 - A1, +/-2.048 V, 860 samples/s, comparator off.
bool adsStartStream() {
  if (!writeReg16(Wire1, ADS_ADDR, 0x01, 0x04E3)) return false;
  Wire1.beginTransmission(ADS_ADDR);              // point at the conversion register
  Wire1.write((uint8_t)0x00);
  if (Wire1.endTransmission() != 0) return false;
  adsStreaming = true;
  return true;
}

bool adsInit() {
  if (!i2cPresent(Wire1, ADS_ADDR)) return false;
  if (!adsStartStream()) return false;
  adsFails = 0;
  Serial.println("# ADS1115 ready at 0x48");
  return true;
}

void sampleAds() {
  if (Wire1.requestFrom(ADS_ADDR, (uint8_t)2) != 2) {
    if (++adsFails >= 5) { adsOk = false; adsStreaming = false; Serial.println("# ADS1115 lost"); }
    return;
  }
  adsFails = 0;
  uint8_t hi = Wire1.read();
  uint8_t lo = Wire1.read();
  double v = (int16_t)(((uint16_t)hi << 8) | lo) * 2.048 / 32768.0;
  curSum += v;
  curSq  += v * v;
  curN++;
}

// Single-shot read of A1 (the 1.65 V midpoint), then back to streaming.
void adsReadMidpoint() {
  adsStreaming = false;
  bool ok = writeReg16(Wire1, ADS_ADDR, 0x01, 0xD383);   // start, A1 vs GND, +/-4.096 V, single-shot, 128 SPS
  uint16_t status = 0;
  unsigned long t0 = millis();
  while (ok) {
    delay(2);
    if (!readReg16(Wire1, ADS_ADDR, 0x01, status)) { ok = false; break; }
    if (status & 0x8000) break;
    if (millis() - t0 > 50) { ok = false; break; }
  }
  uint16_t raw = 0;
  if (ok && readReg16(Wire1, ADS_ADDR, 0x00, raw)) midVolts = (int16_t)raw * 4.096f / 32768.0f;
  if (!adsStartStream()) {
    adsOk = false;
    Serial.println("# ADS1115 lost");
  }
}

// =====================================================================
// DS18B20
// =====================================================================
bool dsInit() {
  if (!oneWire.reset()) return false;
  uint8_t rom[8];
  oneWire.reset_search();
  if (!oneWire.search(rom)) return false;
  if (OneWire::crc8(rom, 7) != rom[7] || rom[0] != 0x28) return false;
  Serial.println("# DS18B20 ready");
  return true;
}

bool dsStartConversion() {
  if (!oneWire.reset()) return false;
  oneWire.skip();
  oneWire.write(0x44);
  dsConverting = true;
  dsStartedAt = millis();
  return true;
}

bool dsReadC(float &c) {
  if (!oneWire.reset()) return false;
  oneWire.skip();
  oneWire.write(0xBE);
  uint8_t d[9];
  bool allZero = true;
  for (int i = 0; i < 9; i++) {
    d[i] = oneWire.read();
    if (d[i]) allZero = false;
  }
  if (allZero || OneWire::crc8(d, 8) != d[8]) return false;
  c = (int16_t)(((uint16_t)d[1] << 8) | d[0]) / 16.0f;
  return true;
}

// =====================================================================
// Recorder
// =====================================================================
const char* stateName() {
  switch (state) {
    case ST_POST:      return "recording";
    case ST_COOLDOWN:  return "cooldown";
    case ST_CALIBRATE: return "calibrating";
    default:           return "monitor";
  }
}

float stateLeftSeconds() {
  unsigned long el = millis() - stateSinceMs;
  if (state == ST_POST)      return (POST_N - evPost) / (float)RATE_HZ;
  if (state == ST_COOLDOWN)  return el >= COOLDOWN_MS ? 0 : (COOLDOWN_MS - el) / 1000.0f;
  if (state == ST_CALIBRATE) return el >= (unsigned long)CAL_SECONDS * 1000UL ? 0 : (CAL_SECONDS * 1000UL - el) / 1000.0f;
  return 0;
}

void setState(RecState s) {
  state = s;
  stateSinceMs = millis();
}

void pushRing(const Rec &r) {
  ring[ringHead] = r;
  ringHead = (ringHead + 1) % PRE_N;
  if (ringCount < PRE_N) ringCount++;
}

void startEvent(const char* reason) {
  evPre = ringCount;                                  // the trigger record is the newest one
  int start = (ringHead - ringCount + PRE_N) % PRE_N;
  for (int i = 0; i < evPre; i++) evBuf[i] = ring[(start + i) % PRE_N];
  evPost = 0;
  evId = ++eventCounter;
  prefs.putUInt("events", eventCounter);
  strncpy(evReason, reason, sizeof(evReason) - 1);
  evReason[sizeof(evReason) - 1] = '\0';
  evTriggerMs = millis();
  setState(ST_POST);

  jreset();
  jadd("{\"type\":\"trigger\",\"device\":\"%s\",\"id\":%lu,\"reason\":\"%s\",\"t\":%lu}",
       DEVICE_ID, (unsigned long)evId, evReason, (unsigned long)evTriggerMs);
  emit(tMsg, false);
}

float peakOf(int which) {
  float best = NAN;
  for (int i = 0; i < evPre + evPost; i++) {
    float v = which == 0 ? evBuf[i].vib : which == 1 ? evBuf[i].cur : evBuf[i].temp;
    if (isnan(v)) continue;
    if (isnan(best) || v > best) best = v;
  }
  return best;
}

void sendEvent() {
  int total = evPre + evPost;
  jreset();
  jadd("{\"type\":\"event\",\"device\":\"%s\",\"id\":%lu,\"t\":%lu,\"reason\":\"%s\",\"rate_hz\":%d,\"pre\":%d,\"post\":%d,",
       DEVICE_ID, (unsigned long)evId, (unsigned long)evTriggerMs, evReason, RATE_HZ, evPre, evPost);
  jadd("\"thr\":{\"vib\":%.4f,\"cur\":%.2f,\"temp\":%.1f},", thrVib, thrCur, thrTemp);
  jadd("\"peak\":{\"vib\":");  jfloat(peakOf(0), 4);
  jadd(",\"cur\":");           jfloat(peakOf(1), 3);
  jadd(",\"temp\":");          jfloat(peakOf(2), 2);
  jadd("},\"vib\":[");
  for (int i = 0; i < total; i++) { if (i) jadd(","); jfloat(evBuf[i].vib, 4); }
  jadd("],\"cur\":[");
  for (int i = 0; i < total; i++) { if (i) jadd(","); jfloat(evBuf[i].cur, 3); }
  jadd("],\"temp\":[");
  for (int i = 0; i < total; i++) { if (i) jadd(","); jfloat(evBuf[i].temp, 2); }
  jadd("]}");
  emit(tEvent, true);
  Serial.printf("# Fault record %lu sent (%s, %d values)\n", (unsigned long)evId, evReason, total);
}

void startCalibration() {
  calSum = calSq = 0;
  calN = 0;
  setState(ST_CALIBRATE);
  jreset();
  jadd("{\"type\":\"calibration\",\"state\":\"running\",\"seconds\":%d}", CAL_SECONDS);
  emit(tMsg, false);
}

void finishCalibration() {
  if (calN < CAL_SECONDS * RATE_HZ / 4) {
    setState(ST_MONITOR);
    sendMsg(false, "Calibration failed: not enough vibration data (is the MPU6050 connected?)");
    return;
  }
  double mean = calSum / calN;
  double var  = calSq / calN - mean * mean;
  double sd   = var > 0 ? sqrt(var) : 0;
  float thr = (float)(mean + CAL_SIGMAS * sd);
  if (thr < mean + 0.01) thr = (float)(mean + 0.01);
  thrVib = thr;
  prefs.putFloat("thr_vib", thrVib);
  setState(ST_MONITOR);
  jreset();
  jadd("{\"type\":\"calibration\",\"state\":\"done\",\"mean\":%.4f,\"sd\":%.4f,\"threshold\":%.4f,\"samples\":%d}",
       mean, sd, thrVib, calN);
  emit(tMsg, false);
}

// One 100 ms record: store it, run the state machine, and (normally) publish it live.
void processRecord(const Rec &r, bool publishLive, float ax, float ay, float az) {
  pushRing(r);

  switch (state) {
    case ST_MONITOR: {
      if (millis() < WARMUP_MS) { manualCapture = false; break; }
      const char* reason = nullptr;
      if (manualCapture) reason = "manual";
      else if (thrVib  > 0 && !isnan(r.vib)  && r.vib  > thrVib)  reason = "vibration";
      else if (thrCur  > 0 && !isnan(r.cur)  && r.cur  > thrCur)  reason = "current";
      else if (thrTemp > 0 && !isnan(r.temp) && r.temp > thrTemp) reason = "temperature";
      manualCapture = false;
      if (reason) startEvent(reason);
      break;
    }
    case ST_POST:
      evBuf[evPre + evPost] = r;
      evPost++;
      if (evPost >= POST_N) {
        sendEvent();
        setState(ST_COOLDOWN);
      }
      break;
    case ST_COOLDOWN:
      if (millis() - stateSinceMs >= COOLDOWN_MS) setState(ST_MONITOR);
      break;
    case ST_CALIBRATE:
      if (!isnan(r.vib)) { calSum += r.vib; calSq += (double)r.vib * r.vib; calN++; }
      if (millis() - stateSinceMs >= (unsigned long)CAL_SECONDS * 1000UL) finishCalibration();
      break;
  }

  if (!publishLive) return;
  jreset();
  jadd("{\"type\":\"live\",\"t\":%lu,\"vib\":", (unsigned long)millis());
  jfloat(r.vib, 4);
  jadd(",\"cur\":");  jfloat(r.cur, 3);
  jadd(",\"temp\":"); jfloat(r.temp, 2);
  jadd(",\"ax\":");   jfloat(ax, 3);
  jadd(",\"ay\":");   jfloat(ay, 3);
  jadd(",\"az\":");   jfloat(az, 3);
  jadd(",\"state\":\"%s\",\"left\":%.1f,\"fill\":%.1f}", stateName(), stateLeftSeconds(), ringCount / (float)RATE_HZ);
  emit(tLive, false);
}

// Close the current 100 ms window and turn it into a record.
void finishWindow() {
  Rec r;
  r.vib = NAN; r.cur = NAN; r.temp = lastTemp;
  float ax = NAN, ay = NAN, az = NAN;

  if (accN >= 3) {
    double mean = accMagSum / accN;
    double var  = accMagSq / accN - mean * mean;
    r.vib = (float)sqrt(var > 0 ? var : 0);       // how much |a| shakes inside the window
    ax = axSum / accN; ay = aySum / accN; az = azSum / accN;
  }
  if (curN >= 10) {
    double mean = curSum / curN;
    double var  = curSq / curN - mean * mean;      // remove any DC offset between A0 and A1
    float vrms  = (float)sqrt(var > 0 ? var : 0);
    r.cur = vrms * CLAMP_AMPS_PER_VOLT / clampTurns;
  }

  accMagSum = accMagSq = axSum = aySum = azSum = 0;
  accN = 0;
  curSum = curSq = 0;
  curN = 0;

  processRecord(r, true, ax, ay, az);
}

// =====================================================================
// Health
// =====================================================================
void sendHealth() {
  bool wifiUp = wifiEnabled && WiFi.status() == WL_CONNECTED;
  char ip[16] = "";
  if (wifiUp) WiFi.localIP().toString().toCharArray(ip, sizeof(ip));
  jreset();
  jadd("{\"type\":\"health\",\"device\":\"%s\",\"fw\":\"%s\",\"uptime_s\":%lu,\"wifi\":%s,\"mqtt\":%s,\"rssi\":%d,\"ip\":\"%s\",\"heap\":%u,",
       DEVICE_ID, FW_VERSION, (unsigned long)(millis() / 1000), wifiUp ? "true" : "false",
       mqtt.connected() ? "true" : "false", wifiUp ? (int)WiFi.RSSI() : 0, ip, (unsigned)ESP.getFreeHeap());
  jadd("\"sensors\":{\"mpu6050\":{\"ok\":%s,\"addr\":\"0x%02x\",\"who_am_i\":\"0x%02x\"},",
       mpuOk ? "true" : "false", mpuAddr ? mpuAddr : 0x68, mpuWho);
  jadd("\"ads1115\":{\"ok\":%s,\"mid_v\":", adsOk ? "true" : "false");
  jfloat(adsOk ? midVolts : NAN, 3);
  jadd("},\"ds18b20\":{\"ok\":%s}},", dsOk ? "true" : "false");
  jadd("\"thr\":{\"vib\":%.4f,\"cur\":%.2f,\"temp\":%.1f},\"turns\":%d,\"events\":%lu,\"rate_hz\":%d,\"pre_s\":%d,\"post_s\":%d,\"state\":\"%s\"}",
       thrVib, thrCur, thrTemp, clampTurns, (unsigned long)eventCounter, RATE_HZ, PRE_SECONDS, POST_SECONDS, stateName());
  emit(tHealth, false);
}

// =====================================================================
// Commands (from MQTT or Serial Monitor)
// =====================================================================
void handleCommand(char* cmd) {
  for (char* p = cmd; *p; p++) *p = tolower(*p);
  while (*cmd == ' ') cmd++;
  int n = strlen(cmd);
  while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\r' || cmd[n - 1] == '\n')) cmd[--n] = '\0';
  if (n == 0) return;

  char text[96];
  if (strcmp(cmd, "capture") == 0) {
    if (state != ST_MONITOR) { snprintf(text, sizeof(text), "Busy (%s), try again in a few seconds", stateName()); sendMsg(false, text); return; }
    manualCapture = true;
    sendMsg(true, "Capture requested");
  } else if (strcmp(cmd, "calibrate") == 0) {
    if (state != ST_MONITOR) { snprintf(text, sizeof(text), "Busy (%s), try again in a few seconds", stateName()); sendMsg(false, text); return; }
    if (!mpuOk) { sendMsg(false, "MPU6050 not connected"); return; }
    startCalibration();
  } else if (strncmp(cmd, "set vib ", 8) == 0) {
    float v = strtof(cmd + 8, nullptr);
    if (v < 0.001f || v > 4.0f) { sendMsg(false, "Vibration threshold must be between 0.001 and 4 g"); return; }
    thrVib = v; prefs.putFloat("thr_vib", thrVib);
    snprintf(text, sizeof(text), "Vibration threshold set to %.4f g", thrVib); sendMsg(true, text);
  } else if (strncmp(cmd, "set cur ", 8) == 0) {
    float v = strtof(cmd + 8, nullptr);
    if (v < 0 || v > 100) { sendMsg(false, "Current threshold must be 0 (off) to 100 A"); return; }
    thrCur = v; prefs.putFloat("thr_cur", thrCur);
    if (v == 0) snprintf(text, sizeof(text), "Current trigger off");
    else        snprintf(text, sizeof(text), "Current threshold set to %.2f A", thrCur);
    sendMsg(true, text);
  } else if (strncmp(cmd, "set temp ", 9) == 0) {
    float v = strtof(cmd + 9, nullptr);
    if (v < 0 || v > 125) { sendMsg(false, "Temperature threshold must be 0 (off) to 125 C"); return; }
    thrTemp = v; prefs.putFloat("thr_temp", thrTemp);
    if (v == 0) snprintf(text, sizeof(text), "Temperature trigger off");
    else        snprintf(text, sizeof(text), "Temperature threshold set to %.1f C", thrTemp);
    sendMsg(true, text);
  } else if (strncmp(cmd, "turns ", 6) == 0) {
    int t = atoi(cmd + 6);
    if (t < 1 || t > 100) { sendMsg(false, "Clamp turns must be 1 to 100"); return; }
    clampTurns = t; prefs.putInt("turns", clampTurns);
    snprintf(text, sizeof(text), "Clamp turns set to %d", clampTurns); sendMsg(true, text);
  } else if (strcmp(cmd, "status") == 0) {
    sendHealth();
  } else {
    sendMsg(false, "Unknown command. Use: capture, calibrate, set vib X, set cur X, set temp X, turns N, status");
  }
  sendHealth();
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (cmdPending) return;
  unsigned int n = length < sizeof(pendingCmd) - 1 ? length : sizeof(pendingCmd) - 1;
  memcpy(pendingCmd, payload, n);
  pendingCmd[n] = '\0';
  cmdPending = true;                               // handled in loop(), outside the MQTT callback
}

void readSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLen > 0) {
        serialLine[serialLen] = '\0';
        handleCommand(serialLine);
        serialLen = 0;
      }
    } else if (serialLen < (int)sizeof(serialLine) - 1) {
      serialLine[serialLen++] = c;
    }
  }
}

// =====================================================================
// WiFi / MQTT
// =====================================================================
void connectMqtt() {
  if (!wifiEnabled || WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (lastMqttTry != 0 && millis() - lastMqttTry < 15000) return;
  lastMqttTry = millis();
  Serial.printf("# MQTT: connecting to %s:%u\n", MQTT_HOST, MQTT_PORT);
  if (mqtt.connect(DEVICE_ID, tStatus, 1, true, "offline")) {
    mqtt.publish(tStatus, "online", true);
    mqtt.subscribe(tCmd);
    Serial.println("# MQTT: connected");
    sendHealth();
  } else {
    Serial.printf("# MQTT: failed (state %d), retrying in 15 s\n", mqtt.state());
  }
}

// =====================================================================
// Setup / loop
// =====================================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.printf("# Machine Black Box firmware v%s (%s)\n", FW_VERSION, DEVICE_ID);

  prefs.begin("mbb", false);
  thrVib       = prefs.getFloat("thr_vib", DEFAULT_THR_VIB);
  thrCur       = prefs.getFloat("thr_cur", DEFAULT_THR_CUR);
  thrTemp      = prefs.getFloat("thr_temp", DEFAULT_THR_TEMP);
  clampTurns   = prefs.getInt("turns", 1);
  eventCounter = prefs.getUInt("events", 0);
  Serial.printf("# Thresholds: vibration %.4f g, current %.2f A, temperature %.1f C, clamp turns %d\n",
                thrVib, thrCur, thrTemp, clampTurns);

  snprintf(tStatus, sizeof(tStatus), "mbb/%s/status", DEVICE_ID);
  snprintf(tLive,   sizeof(tLive),   "mbb/%s/live",   DEVICE_ID);
  snprintf(tHealth, sizeof(tHealth), "mbb/%s/health", DEVICE_ID);
  snprintf(tEvent,  sizeof(tEvent),  "mbb/%s/event",  DEVICE_ID);
  snprintf(tMsg,    sizeof(tMsg),    "mbb/%s/msg",    DEVICE_ID);
  snprintf(tCmd,    sizeof(tCmd),    "mbb/%s/cmd",    DEVICE_ID);

  Wire.begin(MPU_SDA, MPU_SCL, 100000);     // MPU6050 on a cable: 100 kHz
  Wire1.begin(ADS_SDA, ADS_SCL, 400000);    // ADS1115 on the board: 400 kHz

  mpuOk = mpuInit();
  if (!mpuOk) Serial.println("# MPU6050 not found on GPIO21/22");
  adsOk = adsInit();
  if (!adsOk) Serial.println("# ADS1115 not found on GPIO32/33");
  if (adsOk) adsReadMidpoint();
  dsOk = dsInit();
  if (!dsOk) Serial.println("# DS18B20 not found on GPIO4");
  if (dsOk) dsStartConversion();

  wifiEnabled = strlen(WIFI_SSID) > 0;
  WiFi.mode(WIFI_STA);
  if (wifiEnabled) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("# WiFi: joining \"%s\"\n", WIFI_SSID);
  } else {
    Serial.println("# WiFi: off (USB only)");
  }
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(5);
  mqtt.setSocketTimeout(2);
  mqtt.setBufferSize(512);

  setState(ST_MONITOR);
  windowStartMs = millis();
  lastDsMs = lastMidMs = lastHealthMs = lastRetryMs = millis();
  nextMpuUs = nextAdsUs = micros();
}

void loop() {
  // ---- sampling: MPU6050 every 10 ms, ADS1115 every 1.2 ms ----
  unsigned long nowUs = micros();
  if (mpuOk && (long)(nowUs - nextMpuUs) >= 0) {
    if ((long)(nowUs - nextMpuUs) > 50000) nextMpuUs = nowUs;     // resync after a long stall
    nextMpuUs += 10000;
    sampleMpu();
  }
  nowUs = micros();
  if (adsOk && adsStreaming && (long)(nowUs - nextAdsUs) >= 0) {
    if ((long)(nowUs - nextAdsUs) > 20000) nextAdsUs = nowUs;
    nextAdsUs += 1200;
    sampleAds();
  }

  // ---- one record every 100 ms; a long stall becomes empty records, so time stays correct ----
  unsigned long nowMs = millis();
  if (nowMs - windowStartMs >= 100) {
    unsigned long missing = (nowMs - windowStartMs) / 100 - 1;
    for (unsigned long i = 0; i < missing && i < (unsigned long)PRE_N; i++) {
      Rec gap;
      gap.vib = NAN; gap.cur = NAN; gap.temp = lastTemp;
      processRecord(gap, false, NAN, NAN, NAN);
    }
    windowStartMs += (missing + 1) * 100;
    finishWindow();
  }

  // ---- temperature: read the finished conversion and start the next, once a second ----
  if (dsOk && millis() - lastDsMs >= 1000) {
    lastDsMs = millis();
    if (dsConverting && millis() - dsStartedAt >= 750) {
      float c;
      if (dsReadC(c)) lastTemp = c;
      else { dsOk = false; dsConverting = false; lastTemp = NAN; Serial.println("# DS18B20 lost"); }
    }
    if (dsOk && !dsStartConversion()) { dsOk = false; lastTemp = NAN; Serial.println("# DS18B20 lost"); }
  }

  // ---- clamp midpoint check every 5 s (not while recording a fault) ----
  if (adsOk && state != ST_POST && millis() - lastMidMs >= 5000) {
    lastMidMs = millis();
    adsReadMidpoint();
  }

  // ---- reconnect missing sensors every 5 s ----
  if ((!mpuOk || !adsOk || !dsOk) && state != ST_POST && millis() - lastRetryMs >= 5000) {
    lastRetryMs = millis();
    if (!mpuOk) mpuOk = mpuInit();
    if (!adsOk) { adsOk = adsInit(); if (adsOk) adsReadMidpoint(); }
    if (!dsOk)  { dsOk = dsInit(); if (dsOk) dsStartConversion(); }
  }

  // ---- health every 2 s ----
  if (millis() - lastHealthMs >= 2000) {
    lastHealthMs = millis();
    sendHealth();
  }

  // ---- connectivity and commands ----
  bool wifiUp = wifiEnabled && WiFi.status() == WL_CONNECTED;
  if (wifiUp != lastWifiUp) {
    lastWifiUp = wifiUp;
    if (wifiUp) Serial.printf("# WiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
    else if (wifiEnabled) Serial.println("# WiFi: not connected");
  }
  if (state != ST_POST) connectMqtt();             // never pause sampling while a fault is being recorded
  if (mqtt.connected()) mqtt.loop();
  if (cmdPending) {
    handleCommand(pendingCmd);
    cmdPending = false;
  }
  readSerialCommands();
}
