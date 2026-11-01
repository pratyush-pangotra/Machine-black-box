/*
  Machine Black Box - full sensor check (final zero PCB board)

  Reads every sensor and prints one block per second to Serial Monitor (115200 baud):
    VIB   MPU6050: acceleration, gyro, and a vibration level (how much |a| shakes)
    CURR  SCT-013-030 clamp through the ADS1115: RMS current, plus the 1.65 V midpoint check
    TEMP  DS18B20 probe

  Library to install (Arduino IDE > Tools > Manage Libraries):
    OneWire by Paul Stoffregen
  Everything else is built in.

  Wiring:
    MPU6050   VCC 3.3 V, GND, SDA GPIO21, SCL GPIO22                (I2C bus 0)
    ADS1115   VDD 3.3 V, GND, SDA GPIO32, SCL GPIO33, ADDR GND      (I2C bus 1, 0x48)
              A0 = clamp red wire
              A1 = midpoint (clamp white wire, junction of the two 10k resistors)
    DS18B20   red 3.3 V, black GND, yellow GPIO4, 4.7k from GPIO4 to 3.3 V

  If a sensor is missing it prints what to check, and retries every 3 s.
*/

#include <Wire.h>
#include <OneWire.h>

// ---------- pins and addresses ----------
const int     MPU_SDA = 21, MPU_SCL = 22;     // bus 0 = Wire
const int     ADS_SDA = 32, ADS_SCL = 33;     // bus 1 = Wire1
const int     PIN_ONEWIRE = 4;
const uint8_t ADS_ADDR = 0x48;

// ---------- current clamp ----------
const float CLAMP_AMPS_PER_VOLT = 30.0f;      // SCT-013-030: 1 V RMS out = 30 A in
const int   CLAMP_TURNS         = 1;          // set to N if the wire loops N times through the clamp

// ---------- timing ----------
const unsigned long REPORT_EVERY_MS = 1000;
const unsigned long RETRY_EVERY_MS  = 3000;

OneWire oneWire(PIN_ONEWIRE);

bool    mpuOk = false, adsOk = false, dsOk = false;
uint8_t mpuAddr = 0, mpuWho = 0;
bool          dsConverting = false;
unsigned long dsStartedAt  = 0;

// Defined here, before any function, so the Arduino IDE's auto-generated declarations can see it.
struct Motion {
  float ax, ay, az;      // last sample, g
  float gx, gy, gz;      // last sample, deg/s
  float mag;             // average |a| over the window, g (about 1.00 when still)
  float vib;             // how much |a| varies over the window, g (the vibration level)
};

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

void scanBus(TwoWire &w, const char *name) {
  Serial.printf("%s: ", name);
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (i2cPresent(w, a)) {
      Serial.printf("%s0x%02X", n ? ", " : "found ", a);
      n++;
    }
  }
  Serial.println(n ? "" : "nothing answers");
}

// =====================================================================
// MPU6050 (vibration)
// =====================================================================

bool mpuInit() {
  mpuAddr = 0;
  if (i2cPresent(Wire, 0x68))      mpuAddr = 0x68;
  else if (i2cPresent(Wire, 0x69)) mpuAddr = 0x69;
  if (!mpuAddr) return false;
  if (!readRegs(Wire, mpuAddr, 0x75, &mpuWho, 1)) return false;   // WHO_AM_I

  bool ok = true;
  ok &= writeReg8(Wire, mpuAddr, 0x6B, 0x80);   // reset
  delay(100);
  ok &= writeReg8(Wire, mpuAddr, 0x6B, 0x01);   // wake up, gyro clock
  delay(10);
  ok &= writeReg8(Wire, mpuAddr, 0x1A, 0x03);   // low-pass filter ~44 Hz
  ok &= writeReg8(Wire, mpuAddr, 0x1B, 0x00);   // gyro  +/-250 deg/s (131 per deg/s)
  ok &= writeReg8(Wire, mpuAddr, 0x1C, 0x00);   // accel +/-2 g       (16384 per g)
  if (!ok) return false;

  Serial.printf("MPU6050: found at 0x%02X, WHO_AM_I 0x%02X%s\n", mpuAddr, mpuWho,
                mpuWho == 0x68 ? " (genuine)" : " (clone chip, works the same)");
  return true;
}

bool mpuReadRaw(int16_t v[7]) {
  uint8_t b[14];
  if (!readRegs(Wire, mpuAddr, 0x3B, b, 14)) return false;
  bool allSame = true;
  for (int i = 1; i < 14; i++) if (b[i] != b[0]) allSame = false;
  if (allSame) return false;                    // all 0x00 / 0xFF: not really answering
  for (int i = 0; i < 7; i++) v[i] = (int16_t)(((uint16_t)b[2 * i] << 8) | b[2 * i + 1]);
  return true;
}

// 50 samples, 5 ms apart (200 per second, 0.25 s window).
bool mpuWindow(Motion &m) {
  const int N = 50;
  const unsigned long STEP_US = 5000;
  double sum = 0, sum2 = 0;
  int16_t v[7];
  unsigned long next = micros();
  for (int i = 0; i < N; i++) {
    while ((long)(micros() - next) < 0) { }
    next += STEP_US;
    if (!mpuReadRaw(v)) return false;
    float ax = v[0] / 16384.0f, ay = v[1] / 16384.0f, az = v[2] / 16384.0f;
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    sum  += mag;
    sum2 += (double)mag * mag;
    if (i == N - 1) {
      m.ax = ax; m.ay = ay; m.az = az;
      m.gx = v[4] / 131.0f; m.gy = v[5] / 131.0f; m.gz = v[6] / 131.0f;
    }
  }
  double mean = sum / N;
  double var  = sum2 / N - mean * mean;
  if (var < 0) var = 0;
  m.mag = (float)mean;
  m.vib = (float)sqrt(var);
  return true;
}

// =====================================================================
// ADS1115 (current clamp)
// =====================================================================

bool adsInit() {
  if (!i2cPresent(Wire1, ADS_ADDR)) return false;
  uint16_t cfg;
  if (!readReg16(Wire1, ADS_ADDR, 0x01, cfg)) return false;
  Serial.printf("ADS1115: found at 0x%02X\n", ADS_ADDR);
  return true;
}

// One single-shot reading of one input against GND, range +/-4.096 V.
// mux: 0x4000 = A0, 0x5000 = A1, 0x6000 = A2, 0x7000 = A3
bool adsSingle(uint16_t mux, float &volts) {
  uint16_t cfg = 0x8000 | mux | 0x0200 | 0x0100 | 0x0080 | 0x0003;   // start, +/-4.096 V, single-shot, 128 SPS, comparator off
  if (!writeReg16(Wire1, ADS_ADDR, 0x01, cfg)) return false;
  unsigned long t0 = millis();
  uint16_t status = 0;
  do {
    delay(2);
    if (!readReg16(Wire1, ADS_ADDR, 0x01, status)) return false;
  } while (!(status & 0x8000) && millis() - t0 < 50);
  if (!(status & 0x8000)) return false;
  uint16_t raw;
  if (!readReg16(Wire1, ADS_ADDR, 0x00, raw)) return false;
  volts = (int16_t)raw * 4.096f / 32768.0f;
  return true;
}

// Clamp signal = A0 - A1 (differential, so midpoint wobble cancels).
// Samples for 0.2 s (10 mains cycles at 50 Hz) and returns the RMS voltage.
bool clampRms(float &vrms, int &n) {
  // continuous mode, A0-A1, +/-2.048 V, 860 SPS, comparator off
  if (!writeReg16(Wire1, ADS_ADDR, 0x01, 0x04E3)) return false;
  delay(5);                                     // let the first conversions settle

  Wire1.beginTransmission(ADS_ADDR);            // point at the conversion register once
  Wire1.write((uint8_t)0x00);
  if (Wire1.endTransmission() != 0) return false;

  double sum = 0, sum2 = 0;
  n = 0;
  unsigned long start = millis(), next = micros();
  while (millis() - start < 200) {
    while ((long)(micros() - next) < 0) { }
    next += 1200;                               // a little slower than 860 SPS, so samples are fresh
    if (Wire1.requestFrom(ADS_ADDR, (uint8_t)2) != 2) return false;
    uint8_t hi = Wire1.read();
    uint8_t lo = Wire1.read();
    double v = (int16_t)(((uint16_t)hi << 8) | lo) * 2.048 / 32768.0;
    sum += v;
    sum2 += v * v;
    n++;
  }
  writeReg16(Wire1, ADS_ADDR, 0x01, 0x0583);    // back to single-shot (idle)

  if (n < 10) return false;
  double mean = sum / n;
  double var  = sum2 / n - mean * mean;         // removes any small DC offset between A0 and A1
  if (var < 0) var = 0;
  vrms = (float)sqrt(var);
  return true;
}

// =====================================================================
// DS18B20 (temperature)
// =====================================================================

bool dsInit() {
  if (!oneWire.reset()) return false;           // no presence pulse
  uint8_t rom[8];
  oneWire.reset_search();
  if (!oneWire.search(rom)) return false;
  if (OneWire::crc8(rom, 7) != rom[7] || rom[0] != 0x28) return false;
  Serial.printf("DS18B20: found (ID %02X-%02X%02X%02X%02X%02X%02X)\n",
                rom[0], rom[6], rom[5], rom[4], rom[3], rom[2], rom[1]);
  return true;
}

bool dsStartConversion() {
  if (!oneWire.reset()) return false;
  oneWire.skip();
  oneWire.write(0x44);                          // convert temperature (takes up to 750 ms)
  dsConverting = true;
  dsStartedAt  = millis();
  return true;
}

bool dsReadC(float &c) {
  if (!oneWire.reset()) return false;
  oneWire.skip();
  oneWire.write(0xBE);                          // read scratchpad
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
// Messages
// =====================================================================

void sayMpuMissing() {
  Serial.println("MPU6050: NOT FOUND on GPIO21/22. Check: VCC 3.3 V and GND at the module,");
  Serial.println("         SDA -> GPIO21, SCL -> GPIO22 (not swapped), wires on SDA/SCL not XDA/XCL.");
}

void sayAdsMissing() {
  Serial.println("ADS1115: NOT FOUND on GPIO32/33 at 0x48. Check: VDD 3.3 V and GND,");
  Serial.println("         SDA -> GPIO32, SCL -> GPIO33 (not swapped), ADDR tied to GND.");
}

void sayDsMissing() {
  Serial.println("DS18B20: NOT FOUND on GPIO4. Check: red 3.3 V, black GND, yellow GPIO4,");
  Serial.println("         4.7k between yellow/GPIO4 and 3.3 V. If the tip gets warm, red and black are swapped.");
}

// =====================================================================
// Setup / loop
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Machine Black Box: full sensor check ===");

  Wire.begin(MPU_SDA, MPU_SCL, 100000);         // MPU6050 is on a cable: keep 100 kHz
  Wire1.begin(ADS_SDA, ADS_SCL, 400000);        // ADS1115 is on the board: 400 kHz for fast sampling

  scanBus(Wire,  "Bus 0 (GPIO21/22)");
  scanBus(Wire1, "Bus 1 (GPIO32/33)");

  mpuOk = mpuInit();  if (!mpuOk) sayMpuMissing();
  adsOk = adsInit();  if (!adsOk) sayAdsMissing();
  dsOk  = dsInit();   if (!dsOk)  sayDsMissing();
  if (dsOk) dsStartConversion();
  Serial.println();
}

void loop() {
  static unsigned long lastReport = 0, lastRetry = 0;

  if ((!mpuOk || !adsOk || !dsOk) && millis() - lastRetry >= RETRY_EVERY_MS) {
    lastRetry = millis();
    if (!mpuOk && (mpuOk = mpuInit())) Serial.println("MPU6050: connected");
    if (!adsOk && (adsOk = adsInit())) Serial.println("ADS1115: connected");
    if (!dsOk  && (dsOk  = dsInit()))  { Serial.println("DS18B20: connected"); dsStartConversion(); }
  }

  if (millis() - lastReport < REPORT_EVERY_MS) return;
  lastReport = millis();

  Serial.printf("[%lu s]\n", millis() / 1000);

  // ---- vibration ----
  if (mpuOk) {
    Motion m;
    if (mpuWindow(m)) {
      Serial.printf("VIB   ax %+5.2f  ay %+5.2f  az %+5.2f g   |a| %4.2f g   vibration %.3f g   gyro %+5.1f %+5.1f %+5.1f deg/s\n",
                    m.ax, m.ay, m.az, m.mag, m.vib, m.gx, m.gy, m.gz);
    } else {
      Serial.println("VIB   read failed");
      mpuOk = false;
      sayMpuMissing();
    }
  }

  // ---- current ----
  if (adsOk) {
    float mid, vrms;
    int n;
    if (adsSingle(0x5000, mid) && clampRms(vrms, n)) {
      float amps = vrms * CLAMP_AMPS_PER_VOLT / CLAMP_TURNS;
      Serial.printf("CURR  %.2f A   (clamp %.1f mV rms, %d samples)   midpoint A1 %.3f V%s\n",
                    amps, vrms * 1000.0f, n, mid,
                    (mid < 1.50f || mid > 1.80f) ? "  <- expected about 1.65 V: check R1/R2 and the midpoint joint" : "");
    } else {
      Serial.println("CURR  read failed");
      adsOk = false;
      sayAdsMissing();
    }
  }

  // ---- temperature ----
  if (dsOk) {
    if (dsConverting && millis() - dsStartedAt >= 800) {
      float c;
      if (dsReadC(c)) {
        Serial.printf("TEMP  %.2f C\n", c);
      } else {
        Serial.println("TEMP  read failed");
        dsOk = false;
        dsConverting = false;
        sayDsMissing();
      }
    } else {
      Serial.println("TEMP  measuring...");
    }
    if (dsOk && !dsStartConversion()) {
      dsOk = false;
      sayDsMissing();
    }
  }

  Serial.println();
}
