/* =============================================================================
   OAT SHT-30 Node  —  v2.0.6
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   Reads one or two SHT-30 air sensors over I2C and pushes temperature and humidity
   as oat-ods to an endpoint the operator owns.

   v2.0.0 is the same firmware as 1.1.0 with the plumbing removed: config, NVS, the
   field registry, WiFi, the setup AP, the pages, the Console, the push engine and
   the heartbeat now come from oat_node_core. This file is ONLY the sensor. The I2C
   layer moved across unchanged, including the fixes its own bench work produced:
   probe an address before commanding it (a command to an empty address is logged by
   the ESP32 driver as a bus ERROR, which made a healthy one-sensor node describe
   itself as broken), a heater whose OFF is CONFIRMED rather than assumed, and a bus
   rebuild that clocks a stuck slave off the line by hand.

   WHY THE HEATER IS OFF BY DEFAULT, AND WHY OFF IS VERIFIED
     The SHT-30 has a heater for driving condensation off the sensing element. A
     HEATED sensor still answers perfectly well — it just answers warm and dry. So
     an OFF command that quietly NACKed on a damp connector (exactly the condition
     the heater exists for) would leave the node publishing plausible fiction
     forever, with every health counter green. The status register is therefore
     read back, and a sensor whose heater will not confirm off STOPS BEING READ.
     Skipping a reading beats sending one known to be wrong.

   CHANGELOG
     2.0.6  Rebuilt against the current core (the wired-node rssi fix and
            slotName landed after 2.0.5's binary was published, so the file on
            the site was two core commits behind its own source). No behaviour
            change for this sketch. The bump exists so one version number never
            names two different binaries.
     2.0.1  Core fix: saving a setting no longer re-joins WiFi unless the WiFi
            settings changed. Editing anything from the LAN used to tear down the
            station connection under the page you were reading.
     2.0.0  Ported to oat_node_core. No behaviour change intended; bench-verify
            against 1.1.0 before trusting it.
     1.1.0  The node stopped naming anything: stream.id is the chip's serial
            (oat-ods §3/§4). Earlier history is in git.

   LICENSE: openly licensed. Copy it, change it, sell what you build with it.
   ============================================================================= */

#include <oat_node_core.h>
#include <oat_measurands.h>
#include <Wire.h>

#define TIER        "oat-sht30-node"
#define FW_SEMVER   "2.0.6"
#define FW_VERSION  "OAT-SHT30-Node/2.0.6"
#define NVS_NS      "oatsht"          // unchanged, so a 1.1.0 node keeps its settings

#define DEFAULT_SDA       21          // see the pin note in pinOkForI2c below
#define DEFAULT_SCL       22
#define I2C_HZ            100000      // 100 kHz — the forgiving speed on a long wire

#define MAX_SHT           2           // the SHT-30 has exactly two addresses
#define SHT_ADDR_A        0x44        // ADDR open / low  (the default)
#define SHT_ADDR_B        0x45        // ADDR tied to 3V3 (the second sensor)

// SHT-3x command words (datasheet §4). Single-shot, high repeatability, clock
// stretching DISABLED — issue, wait, read, which behaves far better with Arduino's
// Wire library than holding the bus.
#define SHT_MEASURE_HI    0x2400
#define SHT_SOFT_RESET    0x30A2
#define SHT_HEATER_ON     0x306D
#define SHT_HEATER_OFF    0x3066
#define SHT_READ_STATUS   0xF32D
#define SHT_CLEAR_STATUS  0x3041
#define SHT_READ_SERIAL   0x3780      // SHT3x-DIS; some clones do not answer
#define SHT_CONV_MS       20          // high-repeatability conversion is 15 ms max
#define SHT_STATUS_HEATER 0x2000      // status register bit 13: the heater is on

#define BUS_FAIL_RESET    5
#define BUS_RECOVER_MIN_MS 30000UL
#define BLIND_RECOVER_MS  60000UL
#define PRESENT_FAIL_DROP 6
#define HEATER_SETTLE_MS  30000UL     // discard samples this long after a heater pulse

// Settings this driver adds to the shared registry
static int      g_sda = DEFAULT_SDA, g_scl = DEFAULT_SCL;
static float    g_toff = 0.0f, g_hoff = 0.0f;
static uint32_t g_heat_s = 0, g_heat_min = 60;

struct ShtSensor {
  bool     present = false;
  bool     everSeen = false;
  bool     heaterStuck = false;        // heater-off could not be CONFIRMED: do not trust it
  uint8_t  addr = 0;
  char     serial[32] = {0};           // "sht30:0123abcd", or "sht30:0x44" if it won't tell us
  int      slot = -1;                  // this sensor's slot in the core's table
  uint32_t readsOk = 0, readsFail = 0, crcFail = 0;
  uint8_t  consecFail = 0;
  float    lastT = NAN, lastH = NAN;
  unsigned long lastOkMs = 0;
};
static ShtSensor sensors[MAX_SHT];

static bool          heaterOn = false;
static unsigned long heaterOnAtMs = 0, heaterPulseMs = 0, heaterOffAtMs = 0, lastHeaterCycleMs = 0;
static uint32_t      g_busResets = 0;
static unsigned long lastBusRecoverMs = 0, lastBlindRecoverMs = 0;
static String        lastReadMsg = "no read yet";

// Which pins can carry the bus, and WHY not otherwise: every wrong choice fails
// looking like a broken sensor. GPIO 6-11 are the flash this firmware runs from;
// 0/2/12/15 are strapping pins and an I2C bus idles HIGH through its pull-ups,
// which is exactly the pull that flips one into the wrong boot mode; 1/3 are the
// USB console; 34-39 are input-only and can never drive a bidirectional bus.
static bool pinOkForI2c(int p, String& why) {
  if (p < 0 || p > 39)              { why = "pin must be 0-39 on a classic ESP32"; return false; }
  if (p >= 6 && p <= 11)            { why = "GPIO 6-11 are the flash chip this firmware runs from"; return false; }
  if (p >= 34)                      { why = "GPIO 34-39 are input-only and cannot drive a bus"; return false; }
  if (p == 1 || p == 3)             { why = "GPIO 1 and 3 are the USB console"; return false; }
  if (p == 0 || p == 2 || p == 12 || p == 15)
                                    { why = "GPIO " + String(p) + " is a strapping pin; an idle-high bus would change how the board boots"; return false; }
  if (p == 20 || p == 24 || (p >= 28 && p <= 31))
                                    { why = "GPIO " + String(p) + " is not brought out on a classic ESP32"; return false; }
  why = "ok";
  return true;
}

// ----------------------------------------------------------------------------
// SHT-30 driver — the whole sensor, in about a hundred lines
// ----------------------------------------------------------------------------
// CRC-8 as the datasheet specifies it: polynomial 0x31, initialised to 0xFF, no
// final XOR. This is the difference between "the wire is fine" and "the wire is
// fine as far as I can tell".
static uint8_t shtCrc8(const uint8_t* data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

// Ask the bus whether anything answers at an address, without saying anything to
// it. A zero-length write is an address-only probe in the ESP32 driver, and it is
// the QUIET path: the driver logs a probe miss at verbose, but logs a real command
// sent to an address with nothing on it as a bus ERROR. That error is wrong twice
// over on an empty second-sensor slot — the slot is empty on purpose, and the line
// lands in the operator's console looking exactly like a fault on the sensor that
// IS working. Three states, not two: ok, failing, and absent by design.
static bool shtOnBus(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool shtCommand(uint8_t addr, uint16_t cmd) {
  if (!shtOnBus(addr)) return false;         // quiet miss — see shtOnBus
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

// Read `n` bytes back from the sensor. Returns false if the sensor did not supply
// them all (a disconnected or hung device).
static bool shtRead(uint8_t addr, uint8_t* buf, int n) {
  if (Wire.requestFrom((int)addr, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// One single-shot measurement. Returns true only when BOTH CRCs check out; on a
// CRC failure it reports crcBad so the caller can count it separately from a
// sensor that simply did not answer.
static bool shtMeasure(uint8_t addr, float& tC, float& rh, bool& crcBad) {
  crcBad = false;
  if (!shtCommand(addr, SHT_MEASURE_HI)) return false;
  delay(SHT_CONV_MS);
  uint8_t b[6];
  if (!shtRead(addr, b, 6)) return false;
  if (shtCrc8(b, 2) != b[2] || shtCrc8(b + 3, 2) != b[5]) { crcBad = true; return false; }
  uint16_t rawT = ((uint16_t)b[0] << 8) | b[1];
  uint16_t rawH = ((uint16_t)b[3] << 8) | b[4];
  tC = -45.0f + 175.0f * ((float)rawT / 65535.0f);
  rh = 100.0f * ((float)rawH / 65535.0f);
  if (rh < 0.0f)   rh = 0.0f;               // the conversion can graze the rails
  if (rh > 100.0f) rh = 100.0f;
  return true;
}

// The status register (bit 13 = heater on). This is how the node CONFIRMS the
// heater is off instead of assuming a command landed — see heaterStopSensor().
static bool shtStatus(uint8_t addr, uint16_t& status) {
  if (!shtCommand(addr, SHT_READ_STATUS)) return false;
  delay(2);
  uint8_t b[3];
  if (!shtRead(addr, b, 3)) return false;
  if (shtCrc8(b, 2) != b[2]) return false;
  status = ((uint16_t)b[0] << 8) | b[1];
  return true;
}

// The chip's own serial number, used as oat-ods source.physical_id — the
// swappable provenance for "which gadget is filling this slot". Some clone
// breakouts do not implement the command; those fall back to the bus address,
// which is still honest, just less specific.
static bool shtSerial(uint8_t addr, char* out, size_t outLen) {
  if (!shtCommand(addr, SHT_READ_SERIAL)) return false;
  delay(2);
  uint8_t b[6];
  if (!shtRead(addr, b, 6)) return false;
  if (shtCrc8(b, 2) != b[2] || shtCrc8(b + 3, 2) != b[5]) return false;
  snprintf(out, outLen, "sht30:%02x%02x%02x%02x", b[0], b[1], b[3], b[4]);
  return true;
}

static void busBegin() {
  Wire.begin(g_sda, g_scl, I2C_HZ);
  Wire.setTimeOut(50);         // never let a stuck slave hang the main loop
}

// Rebuild the bus and soft-reset both addresses. This is what recovers a node
// after a cable tug or a moisture-glitched connector, with no power cycle.
//
// Releasing and re-taking the peripheral is not enough on its own. The nastier
// wedge is a sensor left holding the data line low after a transfer was cut
// short: the ESP32 lets go of the pins, the sensor keeps pulling, and the
// re-initialised bus can never issue a start condition again — every command
// after that fails forever. So before handing the pins back to the I2C
// peripheral, clock the line manually until the sensor lets go, then issue a stop.
// Rate-limited, because a sensor that has been unplugged for good would otherwise
// have the node tearing its own bus down every few seconds.
static void busRecover() {
  if (lastBusRecoverMs && millis() - lastBusRecoverMs < BUS_RECOVER_MIN_MS) return;
  lastBusRecoverMs = millis();
  oatcore::countBusReset();
  g_busResets++;
  Wire.end();
  delay(20);

  // Manual unwedge: up to 9 clock pulses (one byte plus the ack) with the data
  // line released, which walks any half-delivered byte out of the sensor.
  pinMode(g_scl, OUTPUT_OPEN_DRAIN);
  pinMode(g_sda, INPUT_PULLUP);
  digitalWrite(g_scl, HIGH);
  delayMicroseconds(10);
  for (int i = 0; i < 9 && digitalRead(g_sda) == LOW; i++) {
    digitalWrite(g_scl, LOW);  delayMicroseconds(10);
    digitalWrite(g_scl, HIGH); delayMicroseconds(10);
  }
  // A stop condition, by hand: data rises while the clock is already high.
  pinMode(g_sda, OUTPUT_OPEN_DRAIN);
  digitalWrite(g_sda, LOW);  delayMicroseconds(10);
  digitalWrite(g_scl, HIGH); delayMicroseconds(10);
  digitalWrite(g_sda, HIGH); delayMicroseconds(10);

  busBegin();
  for (int i = 0; i < MAX_SHT; i++)
    if (sensors[i].addr) { shtCommand(sensors[i].addr, SHT_SOFT_RESET); }
  delay(5);
  for (int i = 0; i < MAX_SHT; i++) sensors[i].consecFail = 0;
  Serial.printf("[sht] bus rebuilt (rebuild #%u)\n", g_busResets);
}

// Probe both addresses at boot and claim whichever answer. No setting to get
// wrong: plug in one sensor or two and the node finds them.
void startSensors() {
  busBegin();
  const uint8_t addrs[MAX_SHT] = { SHT_ADDR_A, SHT_ADDR_B };
  for (int i = 0; i < MAX_SHT; i++) {
    ShtSensor &s = sensors[i];
    s = ShtSensor();
    s.addr = addrs[i];
    shtCommand(s.addr, SHT_SOFT_RESET);
    delay(5);
    float t, h; bool crcBad;
    s.present = shtMeasure(s.addr, t, h, crcBad);
    if (s.present) {
      s.everSeen = true;
      s.lastT = t + g_toff; s.lastH = h + g_hoff; s.lastOkMs = millis();
      if (!shtSerial(s.addr, s.serial, sizeof(s.serial)))
        snprintf(s.serial, sizeof(s.serial), "sht30:0x%02x", s.addr);
      // The serial IS the stream id (oat-ods §3/§4), so the slot cannot be claimed
      // until we have it — claiming on an empty id would silently get nothing.
      s.slot = oatcore::slotFor(s.serial, s.serial);
      oatcore::slotMeta(s.slot, "Sensirion", "SHT30");        // wired and mains powered
      Serial.printf("[sht] found 0x%02x (%s) %.2fC %.1f%%RH\n", s.addr, s.serial, t, h);
    } else {
      snprintf(s.serial, sizeof(s.serial), "sht30:0x%02x", s.addr);
    }
  }
  if (!sensors[0].present && !sensors[1].present) {
    Serial.printf("[sht] no sensor on SDA=%d SCL=%d — check wiring and pull-ups\n", g_sda, g_scl);
  } else {
    // Say out loud that the other address is empty. A one-sensor node is the
    // normal case, and naming the empty slot beats leaving the operator to wonder
    // what the node did about the address it never mentions.
    for (int i = 0; i < MAX_SHT; i++)
      if (!sensors[i].present)
        Serial.printf("[sht] 0x%02x empty — single-sensor node (a second SHT-30 with ADDR to 3V3 is picked up live)\n",
                      sensors[i].addr);
  }
}

// Read every present sensor once and fold the result. Called on the sample timer,
// and by "read now" on the status page / `read` on the Console.
void sampleSensors() {
  // A heated sensor reads warm and dry. Skip the pulse and a settle window after
  // it rather than publish a number we know is wrong.
  if (heaterOn) { lastReadMsg = "heater on — sampling paused"; return; }
  if (heaterOffAtMs && millis() - heaterOffAtMs < HEATER_SETTLE_MS) {
    lastReadMsg = "settling after heater — sampling paused"; return;
  }

  String msg;
  bool anyOk = false, anyFail = false, anyPresent = false;
  for (int i = 0; i < MAX_SHT; i++) {
    ShtSensor &s = sensors[i];
    if (!s.addr) continue;

    // A sensor whose heater we could not confirm off is not read at all: it would
    // answer, and the answer would be wrong. Keep retrying the off, quietly.
    if (s.heaterStuck) {
      heaterStopSensor(i);
      if (s.heaterStuck) {
        if (msg.length()) msg += " | ";
        msg += "0x" + String(s.addr, HEX) + " heater stuck on, not read";
        continue;
      }
    }

    float t, h; bool crcBad = false;
    bool ok = shtMeasure(s.addr, t, h, crcBad);
    if (ok) {
      t += g_toff; h += g_hoff;
      if (h < 0.0f)   h = 0.0f;              // clamp AFTER the offset: there is no
      if (h > 100.0f) h = 100.0f;            // such thing as 103 %RH to publish
      // Newly appeared: someone wired a sensor up while the node was running, or
      // swapped a failed one. Claim its serial now so physical_id is the chip, not
      // just the bus address.
      if (!s.present && !shtSerial(s.addr, s.serial, sizeof(s.serial)))
        snprintf(s.serial, sizeof(s.serial), "sht30:0x%02x", s.addr);
      if (s.slot < 0) s.slot = oatcore::slotFor(s.serial, s.serial);
      s.present = true; s.everSeen = true;
      s.readsOk++; s.consecFail = 0;
      s.lastT = t; s.lastH = h; s.lastOkMs = millis();
      oatcore::countRead(true, false);
      oatcore::fold(s.slot, "temperature", "Cel", oat::KIND_CONTINUOUS, t);
      oatcore::fold(s.slot, "humidity",    "%RH", oat::KIND_CONTINUOUS, h);
      anyOk = true; anyPresent = true;
      if (msg.length()) msg += " | ";
      msg += "0x" + String(s.addr, HEX) + " " + String(t, 2) + "C " + String(h, 1) + "%RH";
    } else if (s.everSeen) {
      // Only count a failure for a sensor we have actually heard from — an empty
      // 0x45 slot is a node with one sensor, not a node that is broken. (The
      // three-state rule: ok, failing, and intentionally absent.)
      s.readsFail++;
      if (crcBad) s.crcFail++;
      oatcore::countRead(false, crcBad);
      if (s.consecFail < 255) s.consecFail++;
      // Stop calling it present once it has clearly gone. Otherwise the status page
      // keeps showing an hours-old number in live-reading type, which is its own
      // kind of lie.
      if (s.consecFail >= PRESENT_FAIL_DROP && s.present) {
        s.present = false; s.lastT = NAN; s.lastH = NAN;
        oatcore::release(s.slot);            // stop pushing a sensor that has gone
        s.slot = -1;
        Serial.printf("[sht] 0x%02x stopped answering — dropped from the live view\n", s.addr);
      }
      anyFail = true;
      if (msg.length()) msg += " | ";
      msg += "0x" + String(s.addr, HEX) + (crcBad ? " CRC fail" : " no answer");
    }
    if (s.present) anyPresent = true;
  }

  if (anyFail) {
    for (int i = 0; i < MAX_SHT; i++)
      if (sensors[i].consecFail >= BUS_FAIL_RESET) { busRecover(); break; }
  }
  // Nothing answering at all. The failure counters above cannot help here — a slot
  // that has never answered is deliberately not counted — so a wedged bus after a
  // brownout would otherwise sit dead until someone drove out and power-cycled it.
  // Retry a rebuild on a slow cadence instead. It also finds a sensor wired up
  // later, and costs nothing on a node that genuinely has no sensor attached yet.
  if (!anyPresent && millis() - lastBlindRecoverMs >= BLIND_RECOVER_MS) {
    lastBlindRecoverMs = millis();
    busRecover();
  }

  if (!anyOk && !anyFail && msg.length() == 0) msg = "no sensor found on the bus";
  lastReadMsg = msg;
}

// Heater lifecycle — off by default. One start/stop pair drives both the
// scheduled pulse and a manual one from the Console, so a hand-run dry-out can
// never be cut short by the scheduler or left on forever: every pulse carries its
// own length and expires on its own.
// Turn one sensor's heater off and CONFIRM it, because assuming is how a node ends
// up publishing warm, dry fiction. A heated SHT-30 still answers perfectly well; it
// just answers wrong. So an off command that quietly NACKed on a damp connector —
// exactly the condition the heater exists for — would otherwise leave the node
// reporting good-looking readings forever, with no failure counter to notice it.
// Escalates: off, off again, soft reset. If the heater bit still will not clear (or
// the chip won't tell us), the sensor is marked stuck and STOPS being read.
static bool heaterStopSensor(int i) {
  ShtSensor &s = sensors[i];
  if (!s.addr) return true;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt == 2) { shtCommand(s.addr, SHT_SOFT_RESET); delay(5); }
    else              { shtCommand(s.addr, SHT_HEATER_OFF); delay(2); }
    uint16_t st;
    if (shtStatus(s.addr, st) && !(st & SHT_STATUS_HEATER)) {
      if (s.heaterStuck) Serial.printf("[sht] 0x%02x heater confirmed off, resuming reads\n", s.addr);
      s.heaterStuck = false;
      return true;
    }
  }
  if (!s.heaterStuck)
    Serial.printf("[sht] 0x%02x heater will NOT confirm off — reads paused (a heated sensor reads warm and dry)\n", s.addr);
  s.heaterStuck = true;
  return false;
}

static void heaterStop() {
  for (int i = 0; i < MAX_SHT; i++) if (sensors[i].present) heaterStopSensor(i);
  heaterOn = false; heaterOffAtMs = millis(); heaterPulseMs = 0;
}

static bool heaterStart(uint32_t secs) {
  if (secs == 0) secs = 10;
  if (secs > 30) secs = 30;                      // hard cap: a heater is never left running
  bool any = false;
  for (int i = 0; i < MAX_SHT; i++)
    if (sensors[i].present && shtCommand(sensors[i].addr, SHT_HEATER_ON)) any = true;
  if (any) { heaterOn = true; heaterOnAtMs = millis(); heaterPulseMs = secs * 1000UL; }
  return any;
}

static void serviceHeater() {
  if (heaterOn) {                                // expire whatever pulse is running
    if (millis() - heaterOnAtMs >= heaterPulseMs) { heaterStop(); Serial.println("[sht] heater off"); }
    return;
  }
  if (g_heat_s == 0) return;                 // no scheduled pulses (a manual one is still allowed)
  unsigned long every = (g_heat_min ? g_heat_min : 60) * 60000UL;
  if (lastHeaterCycleMs && millis() - lastHeaterCycleMs < every) return;
  lastHeaterCycleMs = millis();                  // set before the attempt, so a node with no
                                                 // sensor doesn't retry every loop
  if (heaterStart(g_heat_s)) Serial.printf("[sht] heater on for %us\n", g_heat_s);
}

// Which addresses answer at all — the wiring-troubleshooting answer, on the
// status page and at the Console.
String busScan() {
  String found;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (found.length()) found += ", ";
      found += "0x" + String(a, HEX);
      if (a == SHT_ADDR_A) found += " (SHT-30 A)";
      if (a == SHT_ADDR_B) found += " (SHT-30 B)";
    }
  }
  return found.length() ? found : String("nothing answered");
}

// ---------------------------------------------------------------------------
// What the operator sees
// ---------------------------------------------------------------------------
static String statusHtml() {
  String p;
  bool any = false;
  for (int i = 0; i < MAX_SHT; i++) {
    ShtSensor &s = sensors[i];
    if (s.heaterStuck) {
      any = true;
      p += "<p class='bad'>0x" + String(s.addr, HEX) + ": the heater will not confirm as off, so this sensor is not being read. "
           "A heated sensor reads warm and dry, and a wrong number is worse than a missing one. "
           "The node keeps retrying; if it persists, power-cycle the node.</p>";
      continue;
    }
    if (!s.present) {
      if (s.everSeen) {
        any = true;
        p += "<p class='bad'>0x" + String(s.addr, HEX) + " (" + String(s.serial) + "): stopped answering after " +
             String(s.readsOk) + " good reads. Last one " + String((millis() - s.lastOkMs) / 1000) +
             "s ago. Failed " + String(s.readsFail) + " times since (CRC " + String(s.crcFail) + ").</p>";
      }
      continue;
    }
    any = true;
    p += "<div style='margin:.6rem 0'><span class='pill'>0x" + String(s.addr, HEX) + "</span><br>";
    p += "<span class='big'>" + String(s.lastT, 2) + " &deg;C</span> &nbsp; ";
    p += "<span class='big'>" + String(s.lastH, 1) + " %RH</span>";
    p += "<div class='muted'>" + String(s.serial) + " &middot; reads ok " + String(s.readsOk) +
         " / failed " + String(s.readsFail) + " (CRC " + String(s.crcFail) + ")" +
         " &middot; last good " + String((millis() - s.lastOkMs) / 1000) + "s ago</div></div>";
  }
  if (!any)
    p += "<p class='bad'>No SHT-30 answering on SDA=" + String(g_sda) + " / SCL=" + String(g_scl) +
         ". Check 3V3 and GND first, then that SDA and SCL are not swapped, then that the breakout has pull-up resistors.</p>";
  if (g_heat_s) p += "<div class='muted'>Heater: " + String(heaterOn ? "ON" : "idle") + " &middot; " +
                     String(g_heat_s) + "s every " + String(g_heat_min) + " min</div>";
  p += "<div class='muted'>Last read: " + lastReadMsg + "</div>";
  return p;
}

static String statusText() {
  String s = "i2c sda=" + String(g_sda) + " scl=" + String(g_scl) + " bus-rebuilds=" + String(g_busResets) + "\n";
  for (int i = 0; i < MAX_SHT; i++) {
    ShtSensor &d = sensors[i];
    if (d.heaterStuck) { s += "sensor 0x" + String(d.addr, HEX) + " HEATER STUCK ON — not being read (would report warm and dry)\n"; continue; }
    if (d.present)
      s += "sensor 0x" + String(d.addr, HEX) + " " + String(d.serial) + " temp=" + String(d.lastT, 2) +
           "C rh=" + String(d.lastH, 1) + "% ok=" + String(d.readsOk) + " fail=" + String(d.readsFail) +
           " crc=" + String(d.crcFail) + "\n";
    else if (d.everSeen)
      s += "sensor 0x" + String(d.addr, HEX) + " GONE — " + String(d.readsOk) + " good reads, last " +
           String((millis() - d.lastOkMs) / 1000) + "s ago\n";
  }
  if (!sensors[0].present && !sensors[1].present)
    s += "sensor: none answering — check 3V3/GND, SDA/SCL, and pull-ups; try 'bus'\n";
  return s;
}

static String diagLine() {
  return "i2c on SDA=" + String(g_sda) + " SCL=" + String(g_scl) + "; addresses answering: " + busScan();
}
static String diagFull() {
  return "i2c bus scan on SDA=" + String(g_sda) + " SCL=" + String(g_scl) + "\n" + busScan() +
         "\nAn SHT-30 answers at 0x44 with ADDR open, or 0x45 with ADDR tied to 3V3. Nothing at all "
         "usually means power, swapped wires, or missing pull-up resistors.";
}

// ---------------------------------------------------------------------------
// Settings, as data
// ---------------------------------------------------------------------------
static String getSda() { return String(g_sda); }
static bool   setSda(const String& v, String& why) {
  int p = (int) v.toInt();
  if (!pinOkForI2c(p, why)) { why = "SDA rejected: " + why; return false; }
  if (p == g_scl) { why = "SDA and SCL must be different pins"; return false; }
  g_sda = p; return true;
}
static String getScl() { return String(g_scl); }
static bool   setScl(const String& v, String& why) {
  int p = (int) v.toInt();
  if (!pinOkForI2c(p, why)) { why = "SCL rejected: " + why; return false; }
  if (p == g_sda) { why = "SDA and SCL must be different pins"; return false; }
  g_scl = p; return true;
}
static String getToff() { return String(g_toff, 2); }
static bool   setToff(const String& v, String& why) { g_toff = v.toFloat(); return true; }
static String getHoff() { return String(g_hoff, 2); }
static bool   setHoff(const String& v, String& why) { g_hoff = v.toFloat(); return true; }
static String getHeatS() { return String(g_heat_s); }
static bool   setHeatS(const String& v, String& why) {
  uint32_t s = (uint32_t) v.toInt();
  if (s > 30) { why = "a heater pulse longer than 30 s cooks the reading it is meant to save"; return false; }
  g_heat_s = s; return true;
}
static String getHeatMin() { return String(g_heat_min); }
static bool   setHeatMin(const String& v, String& why) {
  uint32_t m = (uint32_t) v.toInt();
  if (m && m < 5) { why = "pulsing more often than every 5 minutes leaves no honest readings between pulses"; return false; }
  g_heat_min = m; return true;
}

static const oatcore::Field FIELDS[] = {
  { "sda",     "I2C SDA pin", "Defaults 21 and 22 on a classic ESP32. Change only if your board differs.", getSda, setSda },
  { "scl",     "I2C SCL pin", "", getScl, setScl },
  { "t_off",   "Temperature offset (&deg;C)", "Added to every reading. Use it to line this node up against a reference you trust.", getToff, setToff },
  { "h_off",   "Humidity offset (%RH)", "", getHoff, setHoff },
  { "heater_s","Heater pulse (sec, 0 = off)",
    "Only for a sensor in condensing air. The pulse dries the element; readings during it and for 30 s after are discarded, because a heated sensor reads warm and dry.",
    getHeatS, setHeatS },
  { "heater_m","Heater every (min)", "", getHeatMin, setHeatMin },
};

// ---------------------------------------------------------------------------
// Console commands (the core already has read / rescan / diag)
// ---------------------------------------------------------------------------
static void cmdBus(const String& rest) { Serial.println("[sht] i2c: " + busScan()); }
static void cmdHeater(const String& rest) {
  if (rest == "on") {
    lastHeaterCycleMs = millis();                 // a manual pulse also resets the schedule
    if (heaterStart(g_heat_s))
      Serial.printf("[sht] heater on for %lus, then sampling settles for %lus\n",
                    heaterPulseMs / 1000UL, HEATER_SETTLE_MS / 1000UL);
    else Serial.println("[sht] no sensor to heat");
  } else if (rest == "off") {
    heaterStop(); Serial.println("[sht] heater off");
  } else Serial.println("[OAT] heater on | heater off");
}

static const oatcore::Command COMMANDS[] = {
  { "bus",    "scan the I2C bus for addresses",              cmdBus },
  { "heater", "on | off — run the heater pulse now, or stop it", cmdHeater },
};

// ---------------------------------------------------------------------------
static const oatcore::Driver DRIVER = {
  TIER, "The sensors", FW_VERSION, FW_SEMVER, NVS_NS,
  startSensors, sampleSensors, serviceHeater, sampleSensors, startSensors,
  statusHtml, statusText, diagFull, diagLine,
  FIELDS,   (int)(sizeof(FIELDS)   / sizeof(FIELDS[0])),
  COMMANDS, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
};

void setup() { oatcore::begin(DRIVER); }
void loop()  { oatcore::loop(); }
