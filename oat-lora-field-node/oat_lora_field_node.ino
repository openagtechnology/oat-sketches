/* =============================================================================
   OAT LoRa Field Node  —  v1.3.0
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   An ESP32 with a LoRa radio (Heltec WiFi LoRa 32 V2 or V3) reads the sensors
   wired to it and, on a schedule, broadcasts one small frame on a private 915 MHz
   channel. No Wi-Fi, no internet, no account, no pairing, no reply expected. The
   OAT LoRa Gateway at the house hears it and pushes oat-ods on this node's behalf,
   exactly as the BLE Listener does for a Bluetooth thermometer. This node is a
   Govee with a longer antenna.

   WHAT IT READS (all optional; it reports whatever it finds)
     DS18B20 temperature probes   one 1-Wire pin, up to 16 probes
     SHT-30 air temp + humidity   I2C, 0x44 and/or 0x45
     photoresistor                one ADC pin, as light level 0-100 %
     capacitive soil probes       up to 8 ADC pins, as soil moisture 0-100 % plus raw mV
     its own battery              volts + a rough %, if a battery is fitted

   WHAT IT SENDS (see lib/oat_lora/oat_lora_frame.h — the whole over-air contract)
     A ROSTER frame at boot and every 5th cycle: sub-id -> hardware id, so the
     gateway can name each stream by the probe's own factory address.
     A DATA frame every cycle: sub-id, measurand code, int16 value. Ten sensors
     fit in one ~60-byte frame; more spill into a second.

   SETUP is the USB serial console (115200): `help` lists it. Settings persist in
   NVS. Cadence, radio plan, and every pin are settings — no code editing. The
   radio plan (frequency, bandwidth, spreading factor, sync word) MUST match the
   gateway's; the default plan matches the gateway's default plan.

   OAT-SKETCH-STANDALONE: no Wi-Fi by design, so it cannot link oat_node_core; it
   owns its own console and NVS. check_sketch_contract.py honours this line.

   NAMING LAW: this node names nothing. Stream ids are the sensors' hardware ids;
   the gateway holds the hardware-to-place map, where it survives a reflash.

   CHANGELOG
     1.3.0  Per-probe soil calibration, shared with the Soil-Moisture Node through
            lib/oat_soil: one dry/wet pair PER PIN, captured live at the bench
            (`set cal 3 dry`, `set cal 3 wet`) or typed (`set cal 3 2876 1232`),
            kept in NVS as the table `show` prints. The single soildry/soilwet
            pair is gone, and so are its baked-in 2800/1200 defaults: a probe
            with no calibration now sends its raw millivolts only, no percentage,
            the same rule the wired node follows. Recalibrate after this update.
     1.2.2  Three from the 9/5 bench queue. (1) A board on USB with no cell read the
            charger rail (4.25 V) and reported battery 100 %: above 4.23 V the node
            now says mains (measurand 14 = 1, battery byte = none) and still sends
            the volts. (2) Every roster entry now spills to a second frame when the
            first is full; the SHT-30 and light-pin entries used to be dropped
            silently past 16 probes, and the gateway counted those readings as
            orphans for good. (3) `scan` re-initialises the 1-Wire pin (pull-up,
            settle, one retry) so a probe wired since boot is found without a
            reset. Uptime is now sent in 1000 s steps (it pinned at 38 days).
     1.2.1  Heltec V4 support: the V4's RF front-end module is powered, detected and
            driven around each transmit (see oat_lora_radio.h). Transmit power
            now defaults per board (10 dBm on the V4, whose amplifier adds ~10 dB).
            The V4's battery-divider control is active HIGH. V4 pin defaults avoid
            the front end's GPIO 2, 5, 7 and 46: DS18B20 on 33, pod port 47/48.
     1.2.0  The serial POD PORT: any board that prints readings in the oat-line
            grammar (M/S/L/H lines, see the page) on the node's second UART becomes
            a cluster of sensors on this node — a weather-station listener, an
            Apogee meter, another OAT sketch. Each pod stream rides the roster
            under the id the pod printed. This node also prints ITS readings as
            oat-line on USB, so it is a pod to anything cabled to it.
            Analog pins now default to NONE: a floating ADC pin reads like a wet
            probe, so analog is declared (`set soilpins 3,4`), never discovered;
            a declared pin under 150 mV reports raw millivolts only (a wire off,
            not saturated soil). The NOT_FOUND log lines on first boot are gone.
     1.1.0  The board's OLED shows what it found and when it last transmitted, so
            a node can be checked standing next to it. The SHT-30 shares the
            OLED's I2C pins by default; moved to other pins it gets its own bus.
     1.0.1  First bench boot (Heltec V3, 2026-09-05): radio ok, frames sent. Two
            log noises fixed: the ADC attenuation call ran before the pin was
            configured (Arduino-ESP32 3.x logs an error and keeps the default
            range), and the NVS namespace did not exist until the first `set`.
     1.0.0  First release.

   LICENSE: openly licensed. Copy it, change it, sell what you build with it.
   ============================================================================= */

#include <Preferences.h>
#include <oat_soil.h>              // the read, the floor, the per-probe calibration (shared with the Soil-Moisture Node)
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <oat_lora_frame.h>
#include <oat_lora_radio.h>
#include <oat_lora_screen.h>

#define FW_SEMVER   "1.3.0"
#define FW_VERSION  "OAT-LoRa-Field-Node/1.3.0"
#define NVS_NS      "oatlfn"

#define MAX_DS      16
#define MAX_SOIL    8
#define SUB_NODE    0
#define SUB_DS0     1      // 1..16
#define SUB_SHT0    17     // 17..18
#define SUB_ADC0    33     // 33..40  (33 = light, 34.. = soil in pin order)
#define SUB_POD0    64     // 64..95  pod streams, allocated first-seen for the run
#define MAX_PODS    32
#define MAX_POD_MEAS 8
#define POD_ID_LEN  24
#define ADC_FLOOR_MV 150   // below this a declared analog pin is a wire off, not a reading

using namespace oatlora;

// Types used as function parameters must precede the first function, because the
// Arduino builder inserts its auto-generated prototypes there.
struct PodMeas { uint8_t code; uint8_t kind; double sum; double last; uint32_t n; };
struct PodStream { bool used; char sid[POD_ID_LEN + 1]; uint8_t sub; PodMeas m[MAX_POD_MEAS]; char brand[16]; char model[20]; int rssi; unsigned long lastMs; uint32_t lines; };

// ---------------------------------------------------------------------------
// Config — every knob a person may need to turn, persisted in NVS.
// ---------------------------------------------------------------------------
struct Config {
  uint16_t cadence   = 180;                 // seconds between transmits (60 floor)
  float    freq      = DEFAULT_PLAN.freqMHz;
  float    bw        = DEFAULT_PLAN.bwKHz;
  uint8_t  sf        = DEFAULT_PLAN.sf;
  uint8_t  sync      = DEFAULT_PLAN.syncWord;
  int8_t   power     = DEF_TX_DBM;          // per board: the V4's front end adds ~10 dB
  int      dsPin     = DEF_DS_PIN;
  int      sda       = DEF_SDA;
  int      scl       = DEF_SCL;
  int      lightPin  = DEF_LIGHT_PIN;
  String   soilPins  = DEF_SOIL_PINS;       // csv of GPIO; "" = none
  String   soilCal   = "";                  // per-pin dry/wet table, "3:2876/1232 4:2610/1250"; "" = none
  int      battPin   = DEF_BATT_PIN;        // -1 = mains, report no battery
  int      battCtrl  = DEF_BATT_CTRL;
  int      podRx     = DEF_POD_RX;          // the serial pod port; -1 disables
  int      podTx     = DEF_POD_TX;
  bool     lineout   = true;                // print this node's own readings as oat-line on USB
} cfg;
static Preferences prefs;

static void loadConfig() {
  // A namespace does not exist until something is written to it, and a read-only
  // open of a missing one logs an error. Create it once, silently, on first boot.
  // Every get is guarded by isKey: the Preferences library logs an error for a
  // missing key even when a default is supplied.
  if (!prefs.begin(NVS_NS, true)) { prefs.end(); prefs.begin(NVS_NS, false); prefs.end(); prefs.begin(NVS_NS, true); }
  #define K(k) prefs.isKey(k)
  if (K("cadence"))  cfg.cadence  = prefs.getUShort("cadence", cfg.cadence);
  if (K("freq"))     cfg.freq     = prefs.getFloat("freq", cfg.freq);
  if (K("bw"))       cfg.bw       = prefs.getFloat("bw", cfg.bw);
  if (K("sf"))       cfg.sf       = prefs.getUChar("sf", cfg.sf);
  if (K("sync"))     cfg.sync     = prefs.getUChar("sync", cfg.sync);
  if (K("power"))    cfg.power    = prefs.getChar("power", cfg.power);
  if (K("dspin"))    cfg.dsPin    = prefs.getInt("dspin", cfg.dsPin);
  if (K("sda"))      cfg.sda      = prefs.getInt("sda", cfg.sda);
  if (K("scl"))      cfg.scl      = prefs.getInt("scl", cfg.scl);
  if (K("lightpin")) cfg.lightPin = prefs.getInt("lightpin", cfg.lightPin);
  if (K("soilpins")) cfg.soilPins = prefs.getString("soilpins", cfg.soilPins);
  if (K("soilcal"))  cfg.soilCal  = prefs.getString("soilcal", cfg.soilCal);
  if (K("battpin"))  cfg.battPin  = prefs.getInt("battpin", cfg.battPin);
  if (K("battctrl")) cfg.battCtrl = prefs.getInt("battctrl", cfg.battCtrl);
  if (K("podrx"))    cfg.podRx    = prefs.getInt("podrx", cfg.podRx);
  if (K("podtx"))    cfg.podTx    = prefs.getInt("podtx", cfg.podTx);
  if (K("lineout"))  cfg.lineout  = prefs.getBool("lineout", cfg.lineout);
  #undef K
  prefs.end();
  if (cfg.cadence < 60) cfg.cadence = 60;
}
static void saveConfig() {
  prefs.begin(NVS_NS, false);
  prefs.putUShort("cadence", cfg.cadence); prefs.putFloat("freq", cfg.freq); prefs.putFloat("bw", cfg.bw);
  prefs.putUChar("sf", cfg.sf); prefs.putUChar("sync", cfg.sync); prefs.putChar("power", cfg.power);
  prefs.putInt("dspin", cfg.dsPin); prefs.putInt("sda", cfg.sda); prefs.putInt("scl", cfg.scl);
  prefs.putInt("lightpin", cfg.lightPin); prefs.putString("soilpins", cfg.soilPins);
  prefs.putString("soilcal", cfg.soilCal);
  prefs.putInt("battpin", cfg.battPin); prefs.putInt("battctrl", cfg.battCtrl);
  prefs.putInt("podrx", cfg.podRx); prefs.putInt("podtx", cfg.podTx); prefs.putBool("lineout", cfg.lineout);
  prefs.end();
}
static RadioPlan planFromConfig() {
  RadioPlan p = DEFAULT_PLAN;
  p.freqMHz = cfg.freq; p.bwKHz = cfg.bw; p.sf = cfg.sf; p.syncWord = cfg.sync; p.txDbm = cfg.power;
  return p;
}

// ---------------------------------------------------------------------------
// Identity: the low 32 bits of the efuse MAC. Hardware-derived, never editable.
// ---------------------------------------------------------------------------
static uint32_t g_unitId = 0;
static uint32_t unitIdFromMac() {
  uint64_t mac = ESP.getEfuseMac();
  return (uint32_t)(mac & 0xFFFFFFFFULL);
}

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------
static Radio radio;
static Screen screen;
static TwoWire* g_bus = &Wire;          // the SHT-30's bus: Wire (shared with the OLED) or Wire1
static unsigned long g_lastTxMs = 0; static bool g_lastTxOk = false; static uint8_t g_lastTxSeq = 0;
static OneWire* ow = nullptr;
static DallasTemperature* ds = nullptr;
struct DsProbe { DeviceAddress rom; bool ok; float t; };
static DsProbe dsProbes[MAX_DS]; static int dsCount = 0;

struct Sht { uint8_t addr; bool present; uint8_t serial[4]; bool haveSerial; float t, h; bool ok; };
static Sht shts[2] = { {0x44, false, {0}, false, 0, 0, false}, {0x45, false, {0}, false, 0, 0, false} };

static int soilPins[MAX_SOIL]; static int soilCount = 0;
static int soilMv[MAX_SOIL]; static int lightMv = -1;
static oatsoil::Table g_soilCal;                     // loaded from cfg.soilCal at boot
static bool soilDeclared(int pin) { for (int i = 0; i < soilCount; i++) if (soilPins[i] == pin) return true; return false; }
static bool soilPinOk(int pin, String& why) { if (pin <= 0 || pin > 48) { why = "pin must be 1-48"; return false; } why = "ok"; return true; }
static float battV = 0;

static uint8_t sht_crc8(const uint8_t* d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) { crc ^= d[i]; for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1); }
  return crc;
}
static bool shtCmd(uint8_t addr, uint16_t cmd) {
  g_bus->beginTransmission(addr); g_bus->write((uint8_t)(cmd >> 8)); g_bus->write((uint8_t)(cmd & 0xFF));
  return g_bus->endTransmission() == 0;
}
static bool shtRead(uint8_t addr, uint8_t* buf, int n) {
  if (g_bus->requestFrom((int)addr, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = g_bus->read();
  return true;
}
static void shtDiscover() {
  for (int i = 0; i < 2; i++) {
    Sht& s = shts[i];
    g_bus->beginTransmission(s.addr); s.present = (g_bus->endTransmission() == 0);
    s.haveSerial = false;
    if (!s.present) continue;
    uint8_t b[6];
    if (shtCmd(s.addr, 0x3780)) { delay(2); if (shtRead(s.addr, b, 6) && sht_crc8(b, 2) == b[2] && sht_crc8(b + 3, 2) == b[5]) {
      s.serial[0] = b[0]; s.serial[1] = b[1]; s.serial[2] = b[3]; s.serial[3] = b[4]; s.haveSerial = true; } }
    if (!s.haveSerial) { s.serial[0] = 0; s.serial[1] = 0; s.serial[2] = 0; s.serial[3] = s.addr; }   // "sht30:000000<addr>"
  }
}
static void shtSample() {
  for (int i = 0; i < 2; i++) {
    Sht& s = shts[i]; s.ok = false;
    if (!s.present) continue;
    if (!shtCmd(s.addr, 0x2400)) continue;          // high repeatability, no clock stretch
    delay(20);
    uint8_t b[6];
    if (!shtRead(s.addr, b, 6)) continue;
    if (sht_crc8(b, 2) != b[2] || sht_crc8(b + 3, 2) != b[5]) continue;
    uint16_t rt = ((uint16_t)b[0] << 8) | b[1], rh = ((uint16_t)b[3] << 8) | b[4];
    s.t = -45.0f + 175.0f * rt / 65535.0f; s.h = 100.0f * rh / 65535.0f; s.ok = true;
  }
}

static void parseSoilPins() {
  soilCount = 0;
  String s = cfg.soilPins; s.trim();
  int from = 0;
  while (from < (int)s.length() && soilCount < MAX_SOIL) {
    int comma = s.indexOf(',', from); if (comma < 0) comma = s.length();
    String tok = s.substring(from, comma); tok.trim();
    if (tok.length()) { int p = tok.toInt(); if (p > 0) soilPins[soilCount++] = p; }
    from = comma + 1;
  }
}
static int readMv(int pin) { oatsoil::configurePin(pin); return oatsoil::readMv(pin); }

static void sensorsBegin() {
  if (ds) { delete ds; ds = nullptr; } if (ow) { delete ow; ow = nullptr; }
  dsCount = 0;
  if (cfg.dsPin >= 0) {
    // Re-init the pin before the bus object owns it: a probe wired after boot was
    // not found by `scan` until a reset, because the pin kept its old state and
    // the first search ran before the line had settled. Pull-up, settle, search;
    // if nothing answers, one more reset-and-search before believing "none".
    pinMode(cfg.dsPin, INPUT_PULLUP); delay(5);
    ow = new OneWire(cfg.dsPin); ds = new DallasTemperature(ow);
    ds->begin(); ds->setWaitForConversion(false); ds->setResolution(12);
    int n = ds->getDeviceCount();
    if (n == 0) { ow->reset(); delay(10); ds->begin(); n = ds->getDeviceCount(); }
    for (int i = 0; i < n && dsCount < MAX_DS; i++) {
      DeviceAddress a;
      if (!ds->getAddress(a, i)) continue;
      if (a[0] != 0x28 && a[0] != 0x10) continue;    // temperature families only
      memcpy(dsProbes[dsCount].rom, a, 8); dsProbes[dsCount].ok = false; dsCount++;
    }
  }
  // The OLED lives on Wire at its fixed pins. The SHT-30 shares that bus when it
  // is on the same pins (the default); anywhere else it gets Wire1, so a sensor
  // pin change can never take the screen down.
  if (cfg.sda == OLED_SDA && cfg.scl == OLED_SCL) { g_bus = &Wire; }
  else { g_bus = &Wire1; Wire1.end(); Wire1.begin(cfg.sda, cfg.scl, 100000); Wire1.setTimeOut(50); }
  shtDiscover();
  parseSoilPins();
}

// A full read: issue the DS conversion, read everything else while it runs, then
// collect. Blocking for ~800 ms is fine here: this node has no web page to serve.
static void sensorsRead() {
  if (ds && dsCount) ds->requestTemperatures();
  shtSample();
  lightMv = cfg.lightPin >= 0 ? readMv(cfg.lightPin) : -1;
  for (int i = 0; i < soilCount; i++) soilMv[i] = readMv(soilPins[i]);
  battV = readBatteryVolts(cfg.battPin, cfg.battCtrl, BATT_MULT);
  if (ds && dsCount) {
    delay(800);
    for (int i = 0; i < dsCount; i++) {
      float t = ds->getTempC(dsProbes[i].rom);
      dsProbes[i].ok = (t != DEVICE_DISCONNECTED_C && t > -55 && t < 125);
      dsProbes[i].t = t;
    }
  }
}

// NAN until that pin has both calibration points: no calibration, no percentage.
static float soilPercent(int pin, int mv) { return g_soilCal.percent(pin, mv); }

// ---------------------------------------------------------------------------
// The serial pod port. Anything that prints oat-line on this node's second UART
// is a cluster of sensors here: a weather-station listener, an Apogee meter, a
// second OAT sketch with its Wi-Fi unconfigured. One record per line:
//   M <stream> <measurement> <value> [unit|-] [c|g|k|s|e]
//   S <stream> brand=<..> model=<..>        (kept for the status page; not sent on the air)
//   L <stream> [rssi=<dBm>] [battery=<pct>] (battery rides as a reading on that stream)
//   H <pod-id> <fw> [uptime=<s>]            (the pod's own heartbeat; shown, not sent)
// Readings arrive whenever the pod has them; this node windows them by kind
// (mean / sum / last) and encodes the window at its cadence. Unknown measurement
// names cannot be put on the air (no codebook code) and are counted instead.
// ---------------------------------------------------------------------------
static PodStream pods[MAX_PODS]; static int podCount = 0;
static uint32_t g_podLines = 0, g_podBad = 0, g_podUnknown = 0;
static char g_podUnknownKeys[120] = {0};
static HardwareSerial PodSerial(1);
static bool g_podOn = false;

static const Code* codeForMeasurement(const char* m) {
  for (size_t i = 0; i < CODES_N; i++) if (!strcmp(CODES[i].measurement, m)) return &CODES[i];
  return nullptr;
}
static PodStream* podFor(const char* sid, bool create) {
  for (int i = 0; i < MAX_PODS; i++) if (pods[i].used && !strcmp(pods[i].sid, sid)) return &pods[i];
  if (!create) return nullptr;
  for (int i = 0; i < MAX_PODS; i++) if (!pods[i].used) {
    PodStream& p = pods[i]; memset(&p, 0, sizeof(p)); p.used = true; strncpy(p.sid, sid, POD_ID_LEN); p.sub = SUB_POD0 + i; p.rssi = 0; podCount++;
    return &p;
  }
  return nullptr;
}
static void podNoteUnknown(const char* k) {
  if (strstr(g_podUnknownKeys, k)) return;
  size_t len = strlen(g_podUnknownKeys); if (len + strlen(k) + 2 >= sizeof(g_podUnknownKeys)) return;
  if (len) strcat(g_podUnknownKeys, ","); strcat(g_podUnknownKeys, k);
}
static void podFold(PodStream& p, const Code& c, uint8_t kind, double v) {
  PodMeas* slot = nullptr;
  for (int i = 0; i < MAX_POD_MEAS; i++) { if (p.m[i].n && p.m[i].code == c.code) { slot = &p.m[i]; break; } }
  if (!slot) for (int i = 0; i < MAX_POD_MEAS; i++) if (!p.m[i].n && !p.m[i].code) { slot = &p.m[i]; break; }
  if (!slot) for (int i = 0; i < MAX_POD_MEAS; i++) if (!p.m[i].n) { slot = &p.m[i]; break; }
  if (!slot) return;
  if (slot->code != c.code) { slot->code = c.code; slot->sum = 0; slot->n = 0; }
  slot->kind = kind; slot->sum += v; slot->last = v; slot->n++;
}
static void podLine(char* line) {
  // tokens
  char* tok[8]; int n = 0;
  for (char* t = strtok(line, " \t"); t && n < 8; t = strtok(nullptr, " \t")) tok[n++] = t;
  if (n < 2) return;
  g_podLines++;
  if (!strcmp(tok[0], "M") && n >= 4) {
    if (strlen(tok[1]) > POD_ID_LEN) { g_podBad++; return; }
    const Code* c = codeForMeasurement(tok[2]);
    if (!c) { g_podUnknown++; podNoteUnknown(tok[2]); return; }
    double v = atof(tok[3]);
    uint8_t kind = c->kind;
    if (n >= 6) { switch (tok[5][0]) { case 'c': kind = oat::KIND_CONTINUOUS; break; case 'g': kind = oat::KIND_GAUGE; break; case 'k': kind = oat::KIND_CUMULATIVE; break; case 's': kind = oat::KIND_STATE; break; case 'e': kind = oat::KIND_EVENT; break; } }
    PodStream* p = podFor(tok[1], true); if (!p) { g_podBad++; return; }
    p->lastMs = millis(); p->lines++;
    podFold(*p, *c, kind, v);
  } else if (!strcmp(tok[0], "S") && n >= 3) {
    PodStream* p = podFor(tok[1], true); if (!p) return;
    for (int i = 2; i < n; i++) {
      if (!strncmp(tok[i], "brand=", 6)) strncpy(p->brand, tok[i] + 6, sizeof(p->brand) - 1);
      else if (!strncmp(tok[i], "model=", 6)) strncpy(p->model, tok[i] + 6, sizeof(p->model) - 1);
    }
  } else if (!strcmp(tok[0], "L") && n >= 3) {
    PodStream* p = podFor(tok[1], true); if (!p) return;
    for (int i = 2; i < n; i++) {
      if (!strncmp(tok[i], "rssi=", 5)) p->rssi = atoi(tok[i] + 5);
      else if (!strncmp(tok[i], "battery=", 8)) { const Code* c = codeForMeasurement("battery"); if (c) podFold(*p, *c, oat::KIND_GAUGE, atof(tok[i] + 8)); }
    }
  } else if (!strcmp(tok[0], "H")) {
    // a pod's heartbeat: noted for the status page only
  } else { g_podBad++; }
}
static void podBegin() {
  if (g_podOn) { PodSerial.end(); g_podOn = false; }
  if (cfg.podRx < 0) return;
  PodSerial.begin(115200, SERIAL_8N1, cfg.podRx, cfg.podTx >= 0 ? cfg.podTx : -1);
  PodSerial.setTimeout(20);
  g_podOn = true;
}
static void podPoll() {
  if (!g_podOn) return;
  static char line[160]; static int len = 0;
  while (PodSerial.available()) {
    char ch = PodSerial.read();
    if (ch == '\r') continue;
    if (ch == '\n') { line[len] = 0; if (len) podLine(line); len = 0; continue; }
    if (len < (int)sizeof(line) - 1) line[len++] = ch; else { len = 0; g_podBad++; }   // overlong: resync at the next newline
  }
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
static uint8_t g_seq = 0;
static uint32_t g_txOk = 0, g_txFail = 0, g_cycles = 0;
static Builder fb;

static void printLines();
static uint8_t battByte() { return cfg.battPin >= 0 ? batteryPercent(battV) : BATT_NA; }

static bool sendFrame() {
  fb.seal();
  digitalWrite(PIN_LED, HIGH);
  int st = radio.send(fb.buf, fb.len);
  digitalWrite(PIN_LED, LOW);
  if (st == RADIOLIB_ERR_NONE) g_txOk++; else g_txFail++;
  g_lastTxMs = millis(); g_lastTxOk = (st == RADIOLIB_ERR_NONE); g_lastTxSeq = fb.buf[6];
  Serial.printf("[tx] seq=%u kind=%u bytes=%u entries=%u -> %s\n", fb.buf[6], fb.buf[7], (unsigned)fb.len, fb.count(), Radio::stateName(st));
  return st == RADIOLIB_ERR_NONE;
}

// One path for every roster entry: when the frame is full, send it and start
// another. Two entries used to bypass this and vanish past sixteen probes.
static void rosterAdd(uint8_t sub, uint8_t kind, const uint8_t* id, uint8_t n) {
  if (fb.addRoster(sub, kind, id, n)) return;
  sendFrame(); delay(100 + (esp_random() % 150));
  fb.begin(g_unitId, g_seq++, FRAME_ROSTER, battByte(), cfg.cadence);
  fb.addRoster(sub, kind, id, n);
}
static void sendRoster() {
  fb.begin(g_unitId, g_seq++, FRAME_ROSTER, battByte(), cfg.cadence);
  uint8_t none = 0;
  rosterAdd(SUB_NODE, SK_NODE, &none, 0);
  for (int i = 0; i < dsCount; i++) rosterAdd(SUB_DS0 + i, SK_DS18B20, dsProbes[i].rom, 8);
  for (int i = 0; i < 2; i++) if (shts[i].present) rosterAdd(SUB_SHT0 + i, SK_SHT30, shts[i].serial, 4);
  if (cfg.lightPin >= 0) { uint8_t p = (uint8_t)cfg.lightPin; rosterAdd(SUB_ADC0, SK_ANALOG, &p, 1); }
  for (int i = 0; i < soilCount; i++) { uint8_t p = (uint8_t)soilPins[i]; rosterAdd(SUB_ADC0 + 1 + i, SK_ANALOG, &p, 1); }
  for (int i = 0; i < MAX_PODS; i++) {
    if (!pods[i].used) continue;
    rosterAdd(pods[i].sub, SK_SERIAL, (const uint8_t*)pods[i].sid, (uint8_t)strlen(pods[i].sid));
  }
  sendFrame();
  delay(150 + (esp_random() % 200));   // let the gateway file the roster before data lands
}

static void addOrFlush(uint8_t sub, uint8_t code, double v) {
  const Code* c = codeFor(code); if (!c) return;
  if (!fb.addData(sub, code, encodeValue(*c, v))) {
    sendFrame(); delay(100 + (esp_random() % 150));
    fb.begin(g_unitId, g_seq++, FRAME_DATA, battByte(), cfg.cadence);
    fb.addData(sub, code, encodeValue(*c, v));
  }
}

static void sendData() {
  fb.begin(g_unitId, g_seq++, FRAME_DATA, battByte(), cfg.cadence);
  for (int i = 0; i < dsCount; i++) if (dsProbes[i].ok) addOrFlush(SUB_DS0 + i, 1, dsProbes[i].t);
  for (int i = 0; i < 2; i++) if (shts[i].ok) { addOrFlush(SUB_SHT0 + i, 1, shts[i].t); addOrFlush(SUB_SHT0 + i, 2, shts[i].h); }
  if (lightMv >= 0) { if (lightMv >= ADC_FLOOR_MV) addOrFlush(SUB_ADC0, 4, lightMv / 3300.0 * 100.0); addOrFlush(SUB_ADC0, 5, lightMv); }
  for (int i = 0; i < soilCount; i++) { float pct = soilMv[i] >= ADC_FLOOR_MV ? soilPercent(soilPins[i], soilMv[i]) : NAN; if (!isnan(pct)) addOrFlush(SUB_ADC0 + 1 + i, 3, pct); addOrFlush(SUB_ADC0 + 1 + i, 5, soilMv[i]); }
  if (cfg.battPin >= 0 && battV > 0.5f) { addOrFlush(SUB_NODE, 6, battV); addOrFlush(SUB_NODE, 14, onMains(battV) ? 1 : 0); }
  addOrFlush(SUB_NODE, 13, millis() / 1000.0);
  // pod streams: encode each window by its kind, then clear it
  for (int i = 0; i < MAX_PODS; i++) {
    PodStream& p = pods[i]; if (!p.used) continue;
    for (int j = 0; j < MAX_POD_MEAS; j++) {
      PodMeas& m = p.m[j]; if (!m.n) continue;
      double v = (m.kind == oat::KIND_CONTINUOUS) ? m.sum / m.n : (m.kind == oat::KIND_EVENT ? m.sum : m.last);
      addOrFlush(p.sub, m.code, v);
      m.sum = 0; m.n = 0;
    }
  }
  if (fb.count()) sendFrame();
  if (cfg.lineout) printLines();
}

// This node's own readings as oat-line on USB, so it is a pod to anything cabled
// to it. Pod streams are not re-printed: a pod's lines are its own to print.
static void printLines() {
  char id[12]; snprintf(id, sizeof(id), "%08lx", (unsigned long)g_unitId);
  for (int i = 0; i < dsCount; i++) if (dsProbes[i].ok) {
    Serial.printf("S ds18b20:"); for (int j = 0; j < 8; j++) Serial.printf("%02x", dsProbes[i].rom[j]); Serial.printf(" brand=Analog-Devices model=DS18B20\n");
    Serial.printf("M ds18b20:"); for (int j = 0; j < 8; j++) Serial.printf("%02x", dsProbes[i].rom[j]); Serial.printf(" temperature %.2f Cel c\n", dsProbes[i].t);
  }
  for (int i = 0; i < 2; i++) if (shts[i].ok) {
    char sid[24]; snprintf(sid, sizeof(sid), "sht30:%02x%02x%02x%02x", shts[i].serial[0], shts[i].serial[1], shts[i].serial[2], shts[i].serial[3]);
    Serial.printf("S %s brand=Sensirion model=SHT30\nM %s temperature %.2f Cel c\nM %s humidity %.1f %%RH c\n", sid, sid, shts[i].t, sid, shts[i].h);
  }
  if (lightMv >= 0) { if (lightMv >= ADC_FLOOR_MV) Serial.printf("M %s:a%d light_level %.1f %% c\n", id, cfg.lightPin, lightMv / 33.0); Serial.printf("M %s:a%d analog_raw %d - c\n", id, cfg.lightPin, lightMv); }
  for (int i = 0; i < soilCount; i++) { float pct = soilMv[i] >= ADC_FLOOR_MV ? soilPercent(soilPins[i], soilMv[i]) : NAN; if (!isnan(pct)) Serial.printf("M %s:a%d soil_moisture %.1f %% c\n", id, soilPins[i], pct); Serial.printf("M %s:a%d analog_raw %d - c\n", id, soilPins[i], soilMv[i]); }
  if (cfg.battPin >= 0 && battV > 0.5f) Serial.printf("M %s voltage %.3f V g\nM %s mains %d - s\n", id, battV, id, onMains(battV) ? 1 : 0);
  Serial.printf("H %s %s uptime=%lu\n", id, FW_VERSION, (unsigned long)(millis() / 1000));
}

static void cycle() {
  g_cycles++;
  sensorsRead();
  if ((g_cycles - 1) % ROSTER_EVERY == 0) sendRoster();
  sendData();
}

// ---------------------------------------------------------------------------
// Screen: six lines a person can read standing next to the node.
// ---------------------------------------------------------------------------
static unsigned long g_nextMs = 0;
static void drawScreen() {
  if (!screen.ok) return;
  char b[40]; String L[6];
  snprintf(b, sizeof(b), "OAT node %08lx", (unsigned long)g_unitId); L[0] = b;
  snprintf(b, sizeof(b), "%.1f SF%u bw%.0f %s", cfg.freq, cfg.sf, cfg.bw, radio.lastState == RADIOLIB_ERR_NONE ? "ok" : "RADIO ERR"); L[1] = b;
  int nsht = (shts[0].present ? 1 : 0) + (shts[1].present ? 1 : 0);
  snprintf(b, sizeof(b), "ds%d sht%d adc%d pod%d", dsCount, nsht, soilCount + (cfg.lightPin >= 0 ? 1 : 0), podCount); L[2] = b;
  if (g_lastTxMs) snprintf(b, sizeof(b), "tx #%u %s ago %s", g_lastTxSeq, ago(g_lastTxMs).c_str(), g_lastTxOk ? "ok" : "FAIL");
  else snprintf(b, sizeof(b), "tx: none yet");
  L[3] = b;
  long next = g_nextMs ? (long)(g_nextMs - millis()) / 1000 : 0; if (next < 0) next = 0;
  snprintf(b, sizeof(b), "next %lds  every %us", next, cfg.cadence); L[4] = b;
  if (dsCount && dsProbes[0].ok) snprintf(b, sizeof(b), "%.1fC", dsProbes[0].t);
  else if (shts[0].ok) snprintf(b, sizeof(b), "%.1fC %.0f%%RH", shts[0].t, shts[0].h);
  else if (shts[1].ok) snprintf(b, sizeof(b), "%.1fC %.0f%%RH", shts[1].t, shts[1].h);
  else b[0] = 0;
  String last = b;
  if (battV > 0.5f) last += (last.length() ? "  " : "") + String(battV, 2) + (onMains(battV) ? "V mains" : "V");
  if (!last.length()) last = "up " + ago(1);
  L[5] = last;
  screen.show(L, 6);
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------
static void printStatus() {
  Serial.printf("%s on %s\nunit id %08lx  cadence %us  seq %u  cycles %lu  tx ok %lu fail %lu  uptime %lus\n",
                FW_VERSION, BOARD_NAME, (unsigned long)g_unitId, cfg.cadence, g_seq, (unsigned long)g_cycles,
                (unsigned long)g_txOk, (unsigned long)g_txFail, (unsigned long)(millis() / 1000));
  Serial.printf("radio %.1f MHz bw %.0f kHz sf %u sync 0x%02x %d dBm -> %s (%s)\n", cfg.freq, cfg.bw, cfg.sf, cfg.sync, cfg.power, Radio::stateName(radio.lastState), Radio::femName(radio.fem));
  Serial.printf("ds18b20 pin %d: %d probe(s)\n", cfg.dsPin, dsCount);
  for (int i = 0; i < dsCount; i++) {
    Serial.printf("  sub %u ds18b20:", SUB_DS0 + i); for (int j = 0; j < 8; j++) Serial.printf("%02x", dsProbes[i].rom[j]);
    if (dsProbes[i].ok) Serial.printf("  %.2f C\n", dsProbes[i].t); else Serial.println("  (no reading yet)");
  }
  Serial.printf("sht30 sda %d scl %d:", cfg.sda, cfg.scl);
  for (int i = 0; i < 2; i++) if (shts[i].present) { Serial.printf("  0x%02x sub %u", shts[i].addr, SUB_SHT0 + i); if (shts[i].ok) Serial.printf(" %.2f C %.1f %%RH", shts[i].t, shts[i].h); }
  Serial.println(shts[0].present || shts[1].present ? "" : "  none found");
  Serial.printf("light pin %d: %d mV\n", cfg.lightPin, lightMv);
  for (int i = 0; i < soilCount; i++) {
    float pct = soilMv[i] >= ADC_FLOOR_MV ? soilPercent(soilPins[i], soilMv[i]) : NAN;
    String pctText = soilMv[i] < ADC_FLOOR_MV ? String("wire off") : (isnan(pct) ? String("no percentage") : String(pct, 0) + " %");
    Serial.printf("soil pin %d sub %u: %d mV = %s (%s)\n", soilPins[i], SUB_ADC0 + 1 + i, soilMv[i], pctText.c_str(), g_soilCal.describe(soilPins[i]).c_str());
  }
  if (onMains(battV)) Serial.printf("battery pin %d: %.2f V = mains (charger rail, no cell to report)\n", cfg.battPin, battV);
  else Serial.printf("battery pin %d: %.2f V (%u %%)\n", cfg.battPin, battV, battByte());
  Serial.printf("pod port rx %d tx %d: %s, %d stream(s), lines %lu bad %lu unknown-measurement %lu%s%s\n", cfg.podRx, cfg.podTx, g_podOn ? "open" : "off", podCount,
                (unsigned long)g_podLines, (unsigned long)g_podBad, (unsigned long)g_podUnknown, g_podUnknownKeys[0] ? " keys: " : "", g_podUnknownKeys);
  for (int i = 0; i < MAX_PODS; i++) if (pods[i].used) {
    Serial.printf("  sub %u %s %s%s%s rssi=%d last=%lus lines=%lu\n", pods[i].sub, pods[i].sid, pods[i].brand, pods[i].model[0] ? " " : "", pods[i].model, pods[i].rssi, (millis() - pods[i].lastMs) / 1000, (unsigned long)pods[i].lines);
  }
  if (lightMv < 0 && !soilCount) Serial.printf("analog: none declared (suggested pins on this board: light %d, soil %s)\n", SUG_LIGHT_PIN, SUG_SOIL_PINS);
}
static void printHelp() {
  Serial.println("commands: help | status | show | set <key> <value> | scan | read | tx | roster | reboot | factory");
  Serial.println("keys: cadence(s) freq(MHz) bw(kHz) sf sync(hex) power(dBm) dspin sda scl lightpin soilpins(csv) cal battpin battctrl podrx podtx lineout(on|off)");
  Serial.println("soil calibration, per probe, at the bench: set cal <pin> dry | set cal <pin> wet | set cal <pin> clear | set cal <pin> <dryMv> <wetMv>; no calibration = raw millivolts only");
  Serial.println("the radio keys must match the gateway; -1 disables a pin; analog pins are declared, never guessed");
}
static void printShow() {
  Serial.printf("cadence=%u freq=%.1f bw=%.0f sf=%u sync=0x%02x power=%d dspin=%d sda=%d scl=%d lightpin=%d soilpins=%s cal=%s battpin=%d battctrl=%d podrx=%d podtx=%d lineout=%s\n",
                cfg.cadence, cfg.freq, cfg.bw, cfg.sf, cfg.sync, cfg.power, cfg.dsPin, cfg.sda, cfg.scl, cfg.lightPin, cfg.soilPins.c_str(), cfg.soilCal.length() ? cfg.soilCal.c_str() : "-", cfg.battPin, cfg.battCtrl, cfg.podRx, cfg.podTx, cfg.lineout ? "on" : "off");
}
static bool setKey(const String& k, const String& v) {
  bool radioChanged = false, pinsChanged = false;
  if      (k == "cadence")  { int n = v.toInt(); if (n < 60 || n > 2550) { Serial.println("cadence must be 60..2550 s"); return false; } cfg.cadence = n; }
  else if (k == "freq")     { cfg.freq = v.toFloat(); radioChanged = true; }
  else if (k == "bw")       { cfg.bw = v.toFloat(); radioChanged = true; }
  else if (k == "sf")       { int n = v.toInt(); if (n < 7 || n > 12) { Serial.println("sf must be 7..12"); return false; } cfg.sf = n; radioChanged = true; }
  else if (k == "sync")     { cfg.sync = (uint8_t)strtol(v.c_str(), nullptr, 16); radioChanged = true; }
  else if (k == "power")    { cfg.power = v.toInt(); radioChanged = true; }
  else if (k == "dspin")    { cfg.dsPin = v.toInt(); pinsChanged = true; }
  else if (k == "sda")      { cfg.sda = v.toInt(); pinsChanged = true; }
  else if (k == "scl")      { cfg.scl = v.toInt(); pinsChanged = true; }
  else if (k == "lightpin") { cfg.lightPin = v.toInt(); pinsChanged = true; }
  else if (k == "soilpins") { cfg.soilPins = v; pinsChanged = true; }
  else if (k == "cal" || k == "soilcal") {
    String why;
    if (!g_soilCal.apply(v, why, soilPinOk, soilDeclared)) { Serial.println(why); return false; }
    cfg.soilCal = g_soilCal.toString();
    Serial.printf("cal: %s\n", cfg.soilCal.length() ? cfg.soilCal.c_str() : "(none)");
  }
  else if (k == "battpin")  { cfg.battPin = v.toInt(); }
  else if (k == "battctrl") { cfg.battCtrl = v.toInt(); }
  else if (k == "podrx")    { cfg.podRx = v.toInt(); saveConfig(); podBegin(); Serial.println(g_podOn ? "pod port open" : "pod port off"); return true; }
  else if (k == "podtx")    { cfg.podTx = v.toInt(); saveConfig(); podBegin(); return true; }
  else if (k == "lineout")  { cfg.lineout = (v == "on" || v == "1" || v == "true"); }
  else { Serial.println("unknown key; `help` lists them"); return false; }
  saveConfig();
  if (radioChanged) { bool ok = radio.begin(planFromConfig()); Serial.printf("radio re-init: %s\n", Radio::stateName(radio.lastState)); (void)ok; }
  if (pinsChanged) { sensorsBegin(); Serial.printf("rescan: %d ds18b20, sht30 %s%s, %d soil pin(s)\n", dsCount, shts[0].present ? "0x44 " : "", shts[1].present ? "0x45" : "", soilCount); }
  Serial.println("saved");
  return true;
}
static void console() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (line.length() < 200) line += c; continue; }
    line.trim();
    if (line.length()) {
      String cmd = line, rest; int sp = line.indexOf(' ');
      if (sp > 0) { cmd = line.substring(0, sp); rest = line.substring(sp + 1); rest.trim(); }
      if      (cmd == "help")    printHelp();
      else if (cmd == "status")  printStatus();
      else if (cmd == "show")    printShow();
      else if (cmd == "set")     { int s2 = rest.indexOf(' '); if (s2 < 0) Serial.println("set <key> <value>"); else setKey(rest.substring(0, s2), rest.substring(s2 + 1)); }
      else if (cmd == "scan")    { sensorsBegin(); printStatus(); }
      else if (cmd == "read")    { sensorsRead(); printStatus(); }
      else if (cmd == "tx")      { sensorsRead(); sendData(); }
      else if (cmd == "roster")  { sendRoster(); }
      else if (cmd == "reboot")  { ESP.restart(); }
      else if (cmd == "factory") { prefs.begin(NVS_NS, false); prefs.clear(); prefs.end(); Serial.println("cleared; rebooting"); delay(200); ESP.restart(); }
      else Serial.println("unknown command; `help`");
    }
    line = "";
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);
  loadConfig();
  { String why; if (!g_soilCal.fromString(cfg.soilCal, why, nullptr)) { Serial.println("[OAT] stored soil calibration unreadable, cleared: " + why); cfg.soilCal = ""; } }
  g_unitId = unitIdFromMac();
  Serial.printf("\n[OAT] %s booting on %s, unit id %08lx\n", FW_VERSION, BOARD_NAME, (unsigned long)g_unitId);
  bool ok = radio.begin(planFromConfig());
  Serial.printf("[OAT] radio %s, %s, %d dBm\n", Radio::stateName(radio.lastState), Radio::femName(radio.fem), cfg.power);
  if (!ok) Serial.println("[OAT] the radio did not start. Check the board type this image was built for, then the antenna. Sensors still read; nothing will transmit.");
  screen.begin();
  Serial.printf("[OAT] screen %s\n", screen.ok ? "ok" : "not found");
  sensorsBegin();
  podBegin();
  Serial.printf("[OAT] pod port %s (rx %d tx %d)\n", g_podOn ? "open" : "off", cfg.podRx, cfg.podTx);
  Serial.printf("[OAT] found %d ds18b20, sht30 %s%s, %d soil pin(s), light pin %d\n", dsCount, shts[0].present ? "0x44 " : "", shts[1].present ? "0x45" : "", soilCount, cfg.lightPin);
  Serial.println("[OAT] type `help` for the console");
  randomSeed(g_unitId ^ micros());
  cycle();                                   // first roster + first readings right away
}

void loop() {
  console();
  podPoll();
  unsigned long now = millis();
  static unsigned long lastDraw = 0;
  if (now - lastDraw > 2000) { lastDraw = now; drawScreen(); }
  if (g_nextMs == 0) g_nextMs = now + (unsigned long)cfg.cadence * 1000UL + (esp_random() % 3000);
  if ((long)(now - g_nextMs) >= 0) {
    cycle();
    g_nextMs = millis() + (unsigned long)cfg.cadence * 1000UL + (esp_random() % 3000);   // jitter keeps units from lock-stepping
    drawScreen();
  }
  delay(10);
}
