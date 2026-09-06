/* =============================================================================
   OAT Soil-Moisture Node  —  v1.0.2
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   Reads up to six capacitive soil-moisture probes on a classic ESP32's analog
   pins and pushes each one's output voltage, and a percentage against two points
   the operator captured on that very probe, as oat-ods to an endpoint they own.

   This file is ONLY the sensor. Config, NVS, the field registry, WiFi, the setup
   AP, the pages, the Console, the push engine and the heartbeat come from
   oat_node_core. The analog read is the one the LoRa Field Node has carried since
   1.2.0 (attenuation set after the first read, a floor under which a pin is a wire
   and not a probe); the calibration is new, and per probe, because that is the
   whole job here.

   WHAT A CAPACITIVE PROBE ACTUALLY IS
     A copper trace under solder mask, an oscillator, and a low-pass filter. Water
     around the trace raises its capacitance, the oscillator slows, and the output
     voltage FALLS as the soil gets wetter. That is all. There is no datasheet
     number for accuracy because there is no accuracy: two probes from the same
     bag read hundreds of millivolts apart in the same glass of water, and the
     same probe reads differently in sand, peat and clay. So the node never
     pretends. It sends the VOLTAGE as the number of record, and a percentage
     only after the operator has shown THAT probe what dry air and water look
     like. No calibration, no percentage. A gap is honest; a made-up number is
     not.

   ANALOG IS DECLARED, NEVER DISCOVERED
     A DS18B20 announces its serial; an SHT-30 answers at its address. An analog
     pin cannot be discovered: a pin with nothing on it floats at a few hundred
     millivolts and reads exactly like a wet probe. So the pin list ships as the
     one pin on the bench (35) and the operator declares the rest. A declared pin
     under ADC_FLOOR_MV is reported as a wire off, not as saturated soil.

   WHY ONLY GPIO 32-39
     The classic ESP32 has two ADC blocks. ADC2 (GPIO 0, 2, 4, 12-15, 25-27) is
     shared with the Wi-Fi radio and stops answering the moment the radio is up,
     which on this node is always. ADC1 is GPIO 32-39; 37 and 38 are not brought
     out on a devkit. That leaves six pins, and six probes, and the setting refuses
     any other pin with the reason, because a pin choice is persisted and
     re-applied at every boot.

   STREAM IDENTITY
     A capacitive probe has no serial number. Its identity is the pin it is on, so
     the stream id is <this board's id>:a<pin>, the same shape the LoRa Field Node
     uses. Label the wire. Which pin is which pot is recorded at the endpoint.

   CHANGELOG
     1.0.2  Core 1.2.1: a calibration captured on the setup page now survives a
            reboot. The core saved settings BEFORE applying driver fields, so the two
            points lived in RAM only; the node lost them on its fourth boot and went
            back to sending voltage alone. Console `set cal` was never affected.
     1.0.1  Bench numbers replace the guessed ones in the diagnosis line: a v2.0 probe
            on 3V3 read 2876 mV in dry air and 1232 mV in water on the bench (a reading
            reached the Test Endpoint). The old "2200-2600 in air" hint was wrong.
     1.0.0  First release. Cut for the board on the bench: a classic ESP32 with a
            generic "v2.0" capacitive probe on GPIO 35, powered from 3V3.

   LICENSE: openly licensed, like everything in this library. Copy it, change it,
   sell what you build with it.
   ============================================================================= */

#include <oat_node_core.h>
#include <oat_measurands.h>

#define TIER        "oat-soil-moisture-node"
#define FW_SEMVER   "1.0.2"
#define FW_VERSION  "OAT-Soil-Moisture-Node/1.0.2"
#define NVS_NS      "oatsoil"

#define MAX_PROBES    6            // ADC1 pins on a classic ESP32: 32 33 34 35 36 39
#define DEFAULT_PINS  "35"         // the bench wiring; declare more on the setup page
#define ADC_FLOOR_MV  150          // under this a pin is a wire off, not a wet probe
#define ADC_SAMPLES   16           // averaged per read; the ADC is noisy, the soil is not
#define CAL_MIN_SPAN  100          // dry and wet closer than this is not a calibration

struct Probe {
  int      pin   = -1;
  int      slot  = -1;
  char     id[40] = {0};                  // "<device>:a35"
  int      dryMv = 0, wetMv = 0;   // 0 = not captured
  int      lastMv = -1;
  float    lastPct = NAN;
  uint32_t reads = 0, dead = 0;    // dead = reads under the floor
  bool     calibrated() const { return dryMv > 0 && wetMv > 0 && dryMv - wetMv >= CAL_MIN_SPAN; }
};
static Probe  probes[MAX_PROBES];
static int    nProbes = 0;
static String g_pins = DEFAULT_PINS;   // the declared list, as typed
static String lastReadMsg = "no read yet";

// Calibration is kept by PIN, not by probe index, so re-ordering the pin list or
// removing a pin and putting it back never hands one probe another's numbers.
struct Cal { int pin; int dryMv; int wetMv; };
static Cal cals[MAX_PROBES];
static int nCals = 0;

// ---------------------------------------------------------------------------
// Pins. Every wrong choice here fails looking like a broken probe, or worse,
// like a perfectly good one.
// ---------------------------------------------------------------------------
static bool pinOkForAdc(int p, String& why) {
  if (p == 32 || p == 33 || p == 34 || p == 35 || p == 36 || p == 39) { why = "ok"; return true; }
  if (p == 0 || p == 2 || p == 4 || (p >= 12 && p <= 15) || (p >= 25 && p <= 27))
    why = "GPIO " + String(p) + " is on ADC2, which stops working while Wi-Fi is on";
  else if (p >= 6 && p <= 11)
    why = "GPIO 6-11 are the flash chip this firmware runs from";
  else if (p == 37 || p == 38)
    why = "GPIO 37 and 38 are not brought out on a classic ESP32 devkit";
  else
    why = "GPIO " + String(p) + " is not an analog input; use 32, 33, 34, 35, 36 or 39";
  return false;
}

static int readMv(int pin) {
  uint32_t sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) sum += analogReadMilliVolts(pin);
  return (int)(sum / ADC_SAMPLES);
}

static Probe* probeForPin(int pin) {
  for (int i = 0; i < nProbes; i++) if (probes[i].pin == pin) return &probes[i];
  return nullptr;
}
static Cal* calForPin(int pin, bool create) {
  for (int i = 0; i < nCals; i++) if (cals[i].pin == pin) return &cals[i];
  if (!create || nCals >= MAX_PROBES) return nullptr;
  cals[nCals] = { pin, 0, 0 };
  return &cals[nCals++];
}

static float percentOf(const Probe& p, int mv) {
  float pct = (float)(p.dryMv - mv) / (float)(p.dryMv - p.wetMv) * 100.0f;
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

// ---------------------------------------------------------------------------
// The sensor actions
// ---------------------------------------------------------------------------
static void sensorBegin() {
  for (int i = 0; i < nProbes; i++) if (probes[i].slot >= 0) oatcore::release(probes[i].slot);
  nProbes = 0;
  String s = g_pins; s.trim();
  int from = 0;
  while (from < (int)s.length() && nProbes < MAX_PROBES) {
    int comma = s.indexOf(',', from); if (comma < 0) comma = s.length();
    String tok = s.substring(from, comma); tok.trim();
    from = comma + 1;
    if (!tok.length()) continue;
    int pin = tok.toInt();
    String why;
    if (!pinOkForAdc(pin, why) || probeForPin(pin)) continue;   // setPins already refused these
    Probe& p = probes[nProbes];
    p = Probe();
    p.pin = pin;
    // The first read configures the pin as an ADC channel; attenuation can only be
    // set after that. 11 dB gives the full 0-3.3 V range a probe on 3V3 produces.
    analogReadMilliVolts(pin);
    analogSetPinAttenuation(pin, ADC_11db);
    snprintf(p.id, sizeof(p.id), "%s:a%d", oatcore::deviceId().c_str(), pin);
    p.slot = oatcore::slotFor(p.id, p.id);
    if (p.slot < 0) { Serial.printf("[soil] no slot left for GPIO %d\n", pin); continue; }
    oatcore::slotMeta(p.slot, "", "capacitive soil probe");   // no maker to name; naming one would be a guess
    if (Cal* c = calForPin(pin, false)) { p.dryMv = c->dryMv; p.wetMv = c->wetMv; }
    nProbes++;
  }
  Serial.printf("[soil] %d probe pin(s) declared: %s\n", nProbes, g_pins.c_str());
}

static void sensorStartSample() {
  int live = 0, off = 0, uncal = 0;
  for (int i = 0; i < nProbes; i++) {
    Probe& p = probes[i];
    if (p.slot < 0) continue;
    int mv = readMv(p.pin);
    p.lastMv = mv;
    if (mv < ADC_FLOOR_MV) {                   // a wire, not a probe: report nothing
      p.lastPct = NAN; p.dead++; off++;
      oatcore::countRead(false, false);
      continue;
    }
    p.reads++; live++;
    oatcore::countRead(true, false);
    oatcore::fold(p.slot, "voltage", "V", oat::KIND_CONTINUOUS, mv / 1000.0);
    if (p.calibrated()) {
      p.lastPct = percentOf(p, mv);
      oatcore::fold(p.slot, "soil_moisture", "%", oat::KIND_CONTINUOUS, p.lastPct);
    } else { p.lastPct = NAN; uncal++; }
  }
  String m = String(live) + " probe(s) read";
  if (off)   m += ", " + String(off) + " under " + String(ADC_FLOOR_MV) + " mV (wire off?)";
  if (uncal) m += ", " + String(uncal) + " sending voltage only (not calibrated)";
  if (!nProbes) m = "no probe pins declared";
  lastReadMsg = m;
}

static void sensorCollect() { }                          // the ADC answers at once
static void sensorSampleBlocking() { sensorStartSample(); }

// ---------------------------------------------------------------------------
// What the operator sees. The two capture links are the calibration: they put a
// command in the `cal` field and submit the setup form, so the core persists it
// like any other setting. On the status page (no form) they lead to setup.
// ---------------------------------------------------------------------------
static String pctText(const Probe& p) {
  if (p.lastMv < 0) return "no reading yet";
  if (p.lastMv < ADC_FLOOR_MV) return "wire off";
  if (!p.calibrated()) return "not calibrated";
  return String(p.lastPct, 0) + " %";
}

static String sensorStatusHtml() {
  String h;
  if (!nProbes) return "<p class='bad'>No probe pins declared. Enter the GPIO the probe's output is on (32, 33, 34, 35, 36 or 39) above.</p>";
  h += "<script>function oatCal(c){var i=document.querySelector(\"input[name=cal]\");"
       "if(!i){location.href='/';return false;}"
       "var a=c.split(' ');if(!confirm('Record the live reading on GPIO '+a[0]+' as '+(a[1]=='dry'?'DRY AIR':'WATER')+'?'))return false;"
       "i.value=c;i.form.submit();return false;}</script>";
  for (int i = 0; i < nProbes; i++) {
    Probe& p = probes[i];
    h += "<div style='margin:.6rem 0'><span class='pill'>GPIO " + String(p.pin) + "</span><span class='pill'>" + String(p.id) + "</span><br>";
    if (p.lastMv < 0)                h += "<span class='big'>no reading yet</span>";
    else if (p.lastMv < ADC_FLOOR_MV) h += "<span class='bad'>" + String(p.lastMv) + " mV: that is a wire off, not wet soil. Check 3V3, GND and the output wire.</span>";
    else {
      h += "<span class='big'>" + String(p.lastMv / 1000.0, 3) + " V</span> &nbsp; ";
      h += "<span class='big'>" + pctText(p) + "</span>";
    }
    h += "<div class='muted'>";
    if (p.calibrated()) h += "dry air " + String(p.dryMv) + " mV &middot; water " + String(p.wetMv) + " mV";
    else if (p.dryMv || p.wetMv) h += "half calibrated: dry " + (p.dryMv ? String(p.dryMv) + " mV" : String("?")) + " &middot; water " + (p.wetMv ? String(p.wetMv) + " mV" : String("?")) + " &middot; sending voltage only";
    else h += "not calibrated: sending voltage only, no percentage";
    h += " &middot; reads " + String(p.reads) + (p.dead ? " &middot; wire-off reads " + String(p.dead) : "") + "</div>";
    h += "<div class='muted'>Calibrate this probe: hold it in dry air, then <a href='#' onclick=\"return oatCal('" + String(p.pin) + " dry')\">this is dry air</a>; "
         "stand it in a glass of water to the line, then <a href='#' onclick=\"return oatCal('" + String(p.pin) + " wet')\">this is water</a>.</div></div>";
  }
  h += "<div class='muted'>Last read: " + lastReadMsg + "</div>";
  return h;
}

static String sensorStatusText() {
  String s = "pins declared: " + g_pins + "\n";
  for (int i = 0; i < nProbes; i++) {
    Probe& p = probes[i];
    s += "probe gpio " + String(p.pin) + " " + String(p.id) + " ";
    if (p.lastMv < 0) s += "no reading yet";
    else s += String(p.lastMv) + " mV = " + pctText(p);
    s += p.calibrated() ? " (dry " + String(p.dryMv) + " / wet " + String(p.wetMv) + ")" : " (not calibrated)";
    s += " reads=" + String(p.reads) + " wireoff=" + String(p.dead) + "\n";
  }
  if (!nProbes) s += "probe: none declared; set pins 35 (or 32,33,34,36,39)\n";
  return s;
}

static String sensorDiag() {
  if (!nProbes) return "no probe pins declared, so nothing was read";
  String d = "live millivolts:";
  for (int i = 0; i < nProbes; i++) d += " gpio" + String(probes[i].pin) + "=" + String(readMv(probes[i].pin));
  d += ". Under " + String(ADC_FLOOR_MV) + " is a wire off; a v2.0 probe on 3V3 read about 2900 in dry air and about 1200 in water on the bench.";
  return d;
}

// ---------------------------------------------------------------------------
// Settings, as data. Refuse a bad value WITH the reason.
// ---------------------------------------------------------------------------
static String getPins() { return g_pins; }
static bool   setPins(const String& v, String& why) {
  String s = v; s.trim();
  int from = 0, n = 0; int seen[MAX_PROBES];
  String clean;
  while (from < (int)s.length()) {
    int comma = s.indexOf(',', from); if (comma < 0) comma = s.length();
    String tok = s.substring(from, comma); tok.trim();
    from = comma + 1;
    if (!tok.length()) continue;
    for (int i = 0; i < (int)tok.length(); i++)
      if (!isDigit(tok[i])) { why = "pins are GPIO numbers separated by commas, like 35,34"; return false; }
    int pin = tok.toInt();
    if (!pinOkForAdc(pin, why)) return false;
    for (int i = 0; i < n; i++) if (seen[i] == pin) { why = "GPIO " + String(pin) + " is listed twice"; return false; }
    if (n >= MAX_PROBES) { why = "six probes is the limit: a classic ESP32 has six usable analog pins"; return false; }
    seen[n++] = pin;
    if (clean.length()) clean += ",";
    clean += String(pin);
  }
  g_pins = clean;          // "" is allowed: a node with no probe declared sends nothing, honestly
  return true;
}

static bool applyCals() {
  for (int i = 0; i < nProbes; i++) {
    Probe& p = probes[i];
    Cal* c = calForPin(p.pin, false);
    p.dryMv = c ? c->dryMv : 0; p.wetMv = c ? c->wetMv : 0;
  }
  return true;
}

static String calTable() {
  String t;
  for (int i = 0; i < nCals; i++) {
    if (!cals[i].dryMv && !cals[i].wetMv) continue;
    if (t.length()) t += " ";
    t += String(cals[i].pin) + ":" + String(cals[i].dryMv) + "/" + String(cals[i].wetMv);
  }
  return t;
}
static String getCal() { return calTable(); }

// Accepts the table it printed ("35:2480/1120 34:2610/1250"), so the form
// round-trips; or a capture command from the links or the Console:
//   <pin> dry | <pin> wet | <pin> clear | <pin> <dryMv> <wetMv>
static bool setCal(const String& v, String& why) {
  String s = v; s.trim();
  if (!s.length()) { nCals = 0; return applyCals(); }           // an emptied field clears every calibration
  if (s.indexOf(':') >= 0) {                              // the table form
    Cal fresh[MAX_PROBES]; int n = 0;
    int from = 0;
    while (from < (int)s.length()) {
      int sp = s.indexOf(' ', from); if (sp < 0) sp = s.length();
      String tok = s.substring(from, sp); tok.trim();
      from = sp + 1;
      if (!tok.length()) continue;
      int c = tok.indexOf(':'), sl = tok.indexOf('/');
      if (c < 1 || sl < c) { why = "calibration entries look like 35:2480/1120"; return false; }
      int pin = tok.substring(0, c).toInt(), dry = tok.substring(c + 1, sl).toInt(), wet = tok.substring(sl + 1).toInt();
      if (!pinOkForAdc(pin, why)) return false;
      if (dry && wet && dry - wet < CAL_MIN_SPAN) { why = "GPIO " + String(pin) + ": dry and wet are only " + String(dry - wet) + " mV apart; that is not two different states"; return false; }
      if (n >= MAX_PROBES) { why = "too many calibration entries"; return false; }
      fresh[n++] = { pin, dry, wet };
    }
    memcpy(cals, fresh, sizeof(fresh)); nCals = n;
    return applyCals();
  }
  {                                                       // a command
    int sp = s.indexOf(' ');
    if (sp < 0) { why = "say which: '35 dry', '35 wet', '35 clear', or '35 2480 1120'"; return false; }
    int pin = s.substring(0, sp).toInt();
    String rest = s.substring(sp + 1); rest.trim();
    if (!pinOkForAdc(pin, why)) return false;
    if (rest == "clear") {
      for (int i = 0; i < nCals; i++) if (cals[i].pin == pin) { cals[i] = cals[--nCals]; break; }
      return applyCals();
    }
    Cal* c = calForPin(pin, true);
    if (!c) { why = "no room for another calibration entry"; return false; }
    if (rest == "dry" || rest == "wet") {
      if (!probeForPin(pin)) { why = "GPIO " + String(pin) + " is not in the pin list; declare it first, then calibrate"; return false; }
      int mv = readMv(pin);
      if (mv < ADC_FLOOR_MV) { why = "GPIO " + String(pin) + " reads " + String(mv) + " mV: that is a wire off, not a probe. Check 3V3, GND and the output wire, then try again"; return false; }
      if (rest == "dry") {
        if (c->wetMv && mv - c->wetMv < CAL_MIN_SPAN) { why = "dry air read " + String(mv) + " mV, only " + String(mv - c->wetMv) + " above the water reading. Is the probe actually dry? Shake it off and wait a minute"; return false; }
        c->dryMv = mv;
      } else {
        if (c->dryMv && c->dryMv - mv < CAL_MIN_SPAN) { why = "water read " + String(mv) + " mV, only " + String(c->dryMv - mv) + " below the dry reading. Is the probe in the water up to the line?"; return false; }
        c->wetMv = mv;
      }
      Serial.printf("[soil] gpio %d %s = %d mV\n", pin, rest.c_str(), mv);
      return applyCals();
    }
    int sp2 = rest.indexOf(' ');
    if (sp2 > 0) {
      int dry = rest.substring(0, sp2).toInt(), wet = rest.substring(sp2 + 1).toInt();
      if (dry <= 0 || wet <= 0 || dry > 3300 || wet > 3300) { why = "millivolts, dry then wet, each between 1 and 3300"; return false; }
      if (dry - wet < CAL_MIN_SPAN) { why = "dry and wet are only " + String(dry - wet) + " mV apart; that is not two different states"; return false; }
      c->dryMv = dry; c->wetMv = wet;
      return applyCals();
    }
    why = "say which: '35 dry', '35 wet', '35 clear', or '35 2480 1120'";
    return false;
  }
}

static const oatcore::Field FIELDS[] = {
  { "pins", "Probe pins (GPIO, comma-separated)",
    "The pin each probe's output wire is on. 35 is the default; 32, 33, 34, 36 and 39 are the others that work with Wi-Fi on. Nothing is discovered: a pin you do not list is not read.",
    getPins, setPins },
  { "cal",  "Calibration",
    "Filled in by the two links under each probe below, and kept here as pin:dry/wet millivolts. You can also type a line: <code>35 dry</code>, <code>35 wet</code>, <code>35 clear</code>, or <code>35 2480 1120</code>. Until a probe has both points it sends its voltage only.",
    getCal, setCal },
};

// ---------------------------------------------------------------------------
// Console commands (the core already has read / rescan / diag / set)
// ---------------------------------------------------------------------------
static void cmdRaw(const String& rest) {
  if (!nProbes) { Serial.println("[soil] no probe pins declared"); return; }
  int n = rest.length() ? rest.toInt() : 1; if (n < 1) n = 1; if (n > 60) n = 60;
  for (int k = 0; k < n; k++) {
    String line = "[soil]";
    for (int i = 0; i < nProbes; i++) line += " gpio" + String(probes[i].pin) + "=" + String(readMv(probes[i].pin)) + "mV";
    Serial.println(line);
    if (k + 1 < n) delay(1000);
  }
}

static const oatcore::Command COMMANDS[] = {
  { "raw", "[seconds] — live millivolts on every declared pin, once a second; watch a dunk", cmdRaw },
};

// ---------------------------------------------------------------------------
static const oatcore::Driver DRIVER = {
  TIER, "The probes", FW_VERSION, FW_SEMVER, NVS_NS,
  sensorBegin, sensorStartSample, sensorCollect, sensorSampleBlocking, sensorBegin,
  sensorStatusHtml, sensorStatusText, nullptr, sensorDiag,
  FIELDS,   (int)(sizeof(FIELDS)   / sizeof(FIELDS[0])),
  COMMANDS, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
};

void setup() { oatcore::begin(DRIVER); }
void loop()  { oatcore::loop(); }
