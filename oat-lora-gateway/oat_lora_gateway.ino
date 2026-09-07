/* =============================================================================
   OAT LoRa Gateway  —  v1.1.4
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   The far-field twin of the BLE Listener. An ESP32 with a LoRa radio (Heltec WiFi
   LoRa 32 V3 or V2) sits where Wi-Fi lives, hears every OAT LoRa Field Node on
   its private 915 MHz channel, claims a slot per sensor, folds the readings, and
   pushes oat-ods to the endpoint you own — the same setup page, Console, push
   engine, heartbeat and signature as every other OAT gateway, all from
   oat_node_core. This file is the radio and the frame decode, nothing else.

   HOW IT WORKS
     The radio sits in continuous receive. Its interrupt raises a flag; collect()
     reads the frame on the main loop, validates it (magic, version, CRC), and:
       ROSTER  files sub-id -> hardware id for that unit and claims a slot per
               sensor under the id the wired sketch for that part would have used
               (ds18b20:<rom>, sht30:<serial>, <unit>:a<pin>), so a probe moved
               from a Wi-Fi node keeps its series.
       DATA    folds each reading into its sensor's slot via the codebook; stamps
               the unit's link quality (RSSI, SNR, battery) on every slot it owns.
     A unit that goes silent for 3 of its announced intervals has its slots
     released, so the endpoint sees the absence instead of a frozen last value.
     Readings for a sub-id with no roster entry yet are counted, not filed: a
     stream must never be created under a placeholder id.

   NAMING LAW: the gateway names nothing. Stream ids are hardware ids; the
   endpoint owns the hardware-to-place map.

   CHANGELOG
     1.1.5  Core 1.2.2: a pod that reports no signal level no longer reaches the
            endpoint as rssi -1 (the pod port passed -1 for "unset"; the core's
            "no level" is oat::NO_RSSI and the line output now omits it). Text
            and provenance only, no behaviour change on the radio.
     1.1.4  Core 1.2.1: a setting changed on the setup page now survives a reboot
            (the core saved before applying driver fields, so the web page was one
            save behind and a reboot reverted it). No behaviour change otherwise.
     1.1.3  (1) Rosters persist through the core's blob door (oatcore::blobSave /
            blobLoad, core 1.2.0) instead of this sketch owning NVS; the sketch
            contract checker flagged 1.1.2 for including Preferences, and it was
            right. A factory reset now clears the rosters with everything else.
            (2) A POD PORT: a board cabled to the gateway's second UART that prints
            readings in the oat-line grammar (a Weather-Station Listener, a Field
            Node on the bench, an Apogee meter) files its streams here directly,
            no radio in between. Pods silent for ten minutes are released.
            (3) Uptime from a node decodes at the new 1000 s scale (frame header).
     1.1.2  Bench 2026-09-05 pm. (1) Heltec V4: the RF front end is powered and
            driven (oat_lora_radio.h) — a V4 on a V3 image is deaf and mute.
            (2) A gateway reboot went deaf for up to five node cycles because
            rosters lived only in RAM; each node's roster is now saved to NVS and
            restored at boot, and the node's OWN stream (its unit id) is filed
            without any roster. (3) Heap: the campus tables plus the screen left a
            31 KB largest block, too small for TLS; the S3 builds are now 64 slots
            / 16 nodes x 24 sensors, still four times a bench. (4) Diagnostics:
            `nodes` shows the radio interrupt count and the front-end type, and
            `rssi` prints the chip's instantaneous signal level for five seconds
            so "hears nothing" splits into "no RF arrives" and "frames fail".
     1.1.1  Streams a field node carries from its serial POD PORT (sensor kind
            SK_SERIAL: the id string a pod printed) file under that string.
     1.1.0  The board's OLED shows Wi-Fi, nodes heard, the last node's signal and
            the last push, so a range walk needs no laptop: watch the dBm fall.
     1.0.0  First release. Bench 2026-09-05: radio ok on a Heltec V3, first farm
            (lora-1) live on the Test Endpoint.

   LICENSE: openly licensed. Copy it, change it, sell what you build with it.
   ============================================================================= */

#include <oat_node_core.h>
#include <oat_measurands.h>
#include <oat_lora_frame.h>
#include <oat_lora_radio.h>
#include <oat_lora_screen.h>

#define TIER        "oat-lora-gateway"
#define FW_SEMVER   "1.1.5"
#define FW_VERSION  "OAT-LoRa-Gateway/1.1.5"
#define NVS_NS      "oatlgw"

// Table sizes. The campus defaults (32 nodes x 40 sensors, 96 slots) fit the S3;
// the classic ESP32's static RAM does not hold them, so the V2 env builds a
// smaller table in platformio.ini. Override with -DOAT_MAX_UNITS / -DOAT_MAX_SUBS.
#ifndef OAT_MAX_UNITS
  #define OAT_MAX_UNITS 16    // field nodes tracked
#endif
#ifndef OAT_MAX_SUBS
  #define OAT_MAX_SUBS  24    // sensors per node (sub-ids)
#endif
#define MAX_UNITS   OAT_MAX_UNITS
#define MAX_SUBS    OAT_MAX_SUBS

using namespace oatlora;

struct SubEntry { bool used; uint8_t subId; uint8_t kind; int slot; char streamId[oatcore::ID_LEN]; };
// Types used as function parameters or returns must precede the first function:
// the Arduino builder inserts its auto-generated prototypes there (BUILDING.md).
#define MAX_GWPODS   16
#define POD_GONE_MS  (10UL * 60UL * 1000UL)
struct GwPod { bool used; bool gone; char sid[oatcore::ID_LEN]; char brand[16]; char model[20]; int rssi; bool haveRssi; unsigned long lastMs; uint32_t lines; int slot; };
struct Unit {
  bool     used = false;
  uint32_t id = 0;
  uint8_t  lastSeq = 0; bool haveSeq = false;
  uint16_t intervalSec = 0;
  unsigned long lastSeenMs = 0;
  float    rssi = 0, snr = 0;
  uint8_t  battery = BATT_NA;
  uint32_t frames = 0, lost = 0, orphanReadings = 0;
  bool     gone = false;
  SubEntry subs[MAX_SUBS];
};
static Unit units[MAX_UNITS];
static Radio radio;
static Screen screen;
static uint32_t g_lastUnit = 0; static float g_lastRssi = 0; static unsigned long g_lastFrameMs = 0;
static volatile bool g_rxFlag = false;
static volatile uint32_t g_irqs = 0;
static uint32_t g_rxFrames = 0, g_rxBad = 0, g_rxRoster = 0, g_rxData = 0;
static char g_lastBad[24] = {0};

static void IRAM_ATTR onRx() { g_rxFlag = true; g_irqs++; }

// ---------------------------------------------------------------------------
// Radio plan as driver fields: the core renders, persists and routes them.
// ---------------------------------------------------------------------------
static RadioPlan g_plan = DEFAULT_PLAN;
static String getFreq()  { return String(g_plan.freqMHz, 1); }
static String getBw()    { return String((int)g_plan.bwKHz); }
static String getSf()    { return String(g_plan.sf); }
static String getSync()  { char b[8]; snprintf(b, sizeof(b), "%02x", g_plan.syncWord); return String(b); }
static bool setFreq(const String& v, String& why) { float f = v.toFloat(); if (f < 902 || f > 928) { why = "frequency must be 902..928 MHz (US915)"; return false; } g_plan.freqMHz = f; return true; }
static bool setBw(const String& v, String& why)   { float b = v.toFloat(); if (b != 125 && b != 250 && b != 500) { why = "bandwidth must be 125, 250 or 500 kHz"; return false; } g_plan.bwKHz = b; return true; }
static bool setSf(const String& v, String& why)   { int s = v.toInt(); if (s < 7 || s > 12) { why = "spreading factor must be 7..12"; return false; } g_plan.sf = s; return true; }
static bool setSync(const String& v, String& why) { long s = strtol(v.c_str(), nullptr, 16); if (s < 0 || s > 255 || s == 0x34) { why = "sync word is one hex byte, and never 34 (LoRaWAN)"; return false; } g_plan.syncWord = (uint8_t)s; return true; }

static String getPodRx(); static String getPodTx();
static bool setPodRx(const String& v, String& why); static bool setPodTx(const String& v, String& why);
static const oatcore::Field FIELDS[] = {
  { "freq", "Frequency (MHz)", "Must match every field node. 915.0 by default.", getFreq, setFreq },
  { "bw",   "Bandwidth (kHz)", "125 reaches farther; 500 uses a quarter of the airtime. Must match the nodes.", getBw, setBw },
  { "sf",   "Spreading factor", "7 = fast, campus range. 9 = a few km. Must match the nodes.", getSf, setSf },
  { "sync", "Sync word (hex)", "Your private network's word, 12 by default. Must match the nodes.", getSync, setSync },
  { "podrx", "Pod port RX (GPIO)", "A board cabled here that prints oat-line files its streams directly. -1 disables.", getPodRx, setPodRx },
  { "podtx", "Pod port TX (GPIO)", "Optional; -1 for none.", getPodTx, setPodTx },
};

// ---------------------------------------------------------------------------
// Units and their sensors
// ---------------------------------------------------------------------------
static Unit* unitFor(uint32_t id, bool create) {
  int freeIdx = -1;
  for (int i = 0; i < MAX_UNITS; i++) {
    if (units[i].used && units[i].id == id) return &units[i];
    if (!units[i].used && freeIdx < 0) freeIdx = i;
  }
  if (!create || freeIdx < 0) return nullptr;
  Unit& u = units[freeIdx]; u = Unit(); u.used = true; u.id = id;
  return &u;
}
static SubEntry* subFor(Unit& u, uint8_t subId, bool create) {
  int freeIdx = -1;
  for (int i = 0; i < MAX_SUBS; i++) {
    if (u.subs[i].used && u.subs[i].subId == subId) return &u.subs[i];
    if (!u.subs[i].used && freeIdx < 0) freeIdx = i;
  }
  if (!create || freeIdx < 0) return nullptr;
  SubEntry& s = u.subs[freeIdx]; s.used = true; s.subId = subId; s.kind = 0; s.slot = -1; s.streamId[0] = 0;
  return &s;
}
static void stampProvenance(int slot, uint8_t kind) {
  switch (kind) {
    case SK_DS18B20: oatcore::slotMeta(slot, "Analog Devices", "DS18B20"); break;
    case SK_SHT30:   oatcore::slotMeta(slot, "Sensirion", "SHT30"); break;
    case SK_ANALOG:  oatcore::slotMeta(slot, "", "ADC"); break;
    case SK_NODE:    oatcore::slotMeta(slot, "Heltec", "LoRa field node"); break;
    case SK_SERIAL:  oatcore::slotMeta(slot, "", "serial pod"); break;
  }
}

// Rosters survive a reboot, through the core's blob door (the core owns NVS).
// Key "r<unit hex>" = a blob of (sub, kind, idLen, id...) entries; key "units" =
// CSV of the unit ids known. Unchanged blobs cost no flash write: NVS compares.
static void rosterKey(uint32_t id, char* key, size_t n) { snprintf(key, n, "r%08lx", (unsigned long)id); }
static void rosterSave(Unit& u) {
  uint8_t blob[512]; size_t n = 0;
  for (int j = 0; j < MAX_SUBS; j++) {
    SubEntry& s = u.subs[j]; if (!s.used || !s.streamId[0]) continue;
    uint8_t l = strlen(s.streamId); if (n + 3 + l > sizeof(blob)) break;
    blob[n++] = s.subId; blob[n++] = s.kind; blob[n++] = l; memcpy(blob + n, s.streamId, l); n += l;
  }
  char key[12]; rosterKey(u.id, key, sizeof(key));
  oatcore::blobSave(key, blob, n);
  char list[MAX_UNITS * 10 + 1]; size_t ln = oatcore::blobLoad("units", list, sizeof(list) - 1); list[ln] = 0;
  if (!strstr(list, key + 1)) {
    if (ln + 10 < sizeof(list)) { if (ln) strcat(list, ","); strcat(list, key + 1); oatcore::blobSave("units", list, strlen(list)); }
  }
}
static void rosterRestoreAll() {
  char list[MAX_UNITS * 10 + 1]; size_t ln = oatcore::blobLoad("units", list, sizeof(list) - 1); list[ln] = 0;
  int restored = 0;
  for (char* hex = strtok(list, ","); hex; hex = strtok(nullptr, ",")) {
    uint32_t id = (uint32_t)strtoul(hex, nullptr, 16);
    char key[12]; rosterKey(id, key, sizeof(key));
    uint8_t blob[512]; size_t n = oatcore::blobLoad(key, blob, sizeof(blob));
    if (!n) continue;
    Unit* u = unitFor(id, true); if (!u) continue;
    u->lastSeenMs = millis(); u->gone = true;              // known, not yet heard this boot
    size_t p = 0;
    while (p + 3 <= n) {
      uint8_t sub = blob[p], kind = blob[p+1], l = blob[p+2]; p += 3;
      if (p + l > n || l >= oatcore::ID_LEN) break;
      SubEntry* s = subFor(*u, sub, true); if (!s) break;
      memcpy(s->streamId, blob + p, l); s->streamId[l] = 0; s->kind = kind; s->slot = -1;   // claimed on first frame
      p += l;
    }
    restored++;
  }
  if (restored) Serial.printf("[lora] restored %d node roster(s) from flash\n", restored);
}

static void fileRoster(Unit& u, const uint8_t* d, size_t n, const Header& h) {
  eachRoster(d, n, h, [&](uint8_t subId, uint8_t kind, const uint8_t* id, uint8_t idLen) {
    SubEntry* s = subFor(u, subId, true);
    if (!s) return;
    char sid[oatcore::ID_LEN];
    streamIdFor(u.id, kind, id, idLen, sid, sizeof(sid));
    if (s->slot >= 0 && strcmp(s->streamId, sid) == 0) return;        // unchanged
    if (s->slot >= 0) oatcore::release(s->slot);                        // sub-id re-used for a different part
    strncpy(s->streamId, sid, sizeof(s->streamId) - 1); s->streamId[sizeof(s->streamId) - 1] = 0;
    s->kind = kind;
    s->slot = oatcore::slotFor(s->streamId, s->streamId);
    if (s->slot >= 0) stampProvenance(s->slot, kind);
  });
  rosterSave(u);
}

static void fileData(Unit& u, const uint8_t* d, size_t n, const Header& h) {
  int batt = h.battery == BATT_NA ? -1 : (int)h.battery;
  eachData(d, n, h, [&](uint8_t subId, uint8_t code, int16_t raw) {
    const Code* c = codeFor(code);
    SubEntry* s = subFor(u, subId, false);
    if (!c) return;                                                     // a code this build does not know: skip, never guess
    if (!s || s->slot < 0) { u.orphanReadings++; return; }              // no roster yet: counted, not filed
    oatcore::slotLink(s->slot, (int)lroundf(u.rssi), batt);
    oatcore::fold(s->slot, c->measurement, c->unit, c->kind, decodeValue(*c, raw));
  });
  // Link quality is the gateway's own measurement of this unit, folded onto the unit's node stream.
  SubEntry* node = subFor(u, 0, false);
  if (node && node->slot >= 0) {
    oatcore::fold(node->slot, "rssi", "dBm", oat::KIND_GAUGE, u.rssi);
    oatcore::fold(node->slot, "snr",  "dB",  oat::KIND_GAUGE, u.snr);
  }
}

static void handleFrame(const uint8_t* d, size_t n) {
  Header h; const char* why = "";
  if (!parseHeader(d, n, h, why)) { g_rxBad++; strncpy(g_lastBad, why, sizeof(g_lastBad) - 1); return; }
  Unit* up = unitFor(h.unitId, true);
  if (!up) { g_rxBad++; strncpy(g_lastBad, "unit table full", sizeof(g_lastBad) - 1); return; }
  Unit& u = *up;
  // The node's own stream needs no roster: its id IS the unit id, always.
  { SubEntry* node = subFor(u, 0, true);
    if (node && node->slot < 0) { snprintf(node->streamId, sizeof(node->streamId), "%08lx", (unsigned long)u.id); node->kind = SK_NODE; node->slot = oatcore::slotFor(node->streamId, node->streamId); if (node->slot >= 0) stampProvenance(node->slot, SK_NODE); } }
  if (u.haveSeq) { uint8_t gap = (uint8_t)(h.seq - u.lastSeq); if (gap > 1 && gap < 128) u.lost += gap - 1; }
  u.lastSeq = h.seq; u.haveSeq = true;
  u.lastSeenMs = millis(); u.frames++; u.gone = false;
  u.rssi = radio.rssi(); u.snr = radio.snr(); u.battery = h.battery;
  g_lastUnit = u.id; g_lastRssi = u.rssi; g_lastFrameMs = millis();
  if (h.intervalSec) u.intervalSec = h.intervalSec;
  g_rxFrames++;
  if (h.kind == FRAME_ROSTER)    { g_rxRoster++; fileRoster(u, d, n, h); }
  else if (h.kind == FRAME_DATA) { g_rxData++;   fileData(u, d, n, h); }
  else { g_rxBad++; strncpy(g_lastBad, "kind", sizeof(g_lastBad) - 1); }
}

// Silence handling: after MISSED_BEFORE_GONE announced intervals, release the
// unit's slots. The roster is kept, so the next frame files straight back in.
static void sweepAbsent() {
  static unsigned long last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  for (int i = 0; i < MAX_UNITS; i++) {
    Unit& u = units[i];
    if (!u.used || u.gone || !u.intervalSec) continue;
    if (millis() - u.lastSeenMs > (unsigned long)u.intervalSec * 1000UL * MISSED_BEFORE_GONE) {
      for (int j = 0; j < MAX_SUBS; j++) if (u.subs[j].used && u.subs[j].slot >= 0) oatcore::release(u.subs[j].slot);
      u.gone = true;
      Serial.printf("[lora] unit %08lx silent for %u intervals: released\n", (unsigned long)u.id, MISSED_BEFORE_GONE);
    }
  }
  // A released slot needs re-claiming on return.
}
static void reclaimIfBack(Unit& u) {
  for (int j = 0; j < MAX_SUBS; j++) {
    SubEntry& s = u.subs[j];
    if (!s.used || !s.streamId[0]) continue;
    s.slot = oatcore::slotFor(s.streamId, s.streamId);
    if (s.slot >= 0) stampProvenance(s.slot, s.kind);
  }
}

// ---------------------------------------------------------------------------
// The pod port. A board cabled to this gateway's second UART that prints the
// oat-line grammar (M/S/L/H lines, see the Field Node page) files its streams
// here directly, under the ids the pod printed — the same grammar the Field Node
// accepts over ITS pod port, without a radio in between. A Weather-Station
// Listener on the porch, a Field Node on the bench, an Apogee meter.
//   M <stream> <measurement> <value> [unit|-] [c|g|k|s|e]
//   S <stream> brand=<..> model=<..>
//   L <stream> [rssi=<dBm>] [battery=<pct>]
//   H <pod-id> <fw> [uptime=<s>]
// ---------------------------------------------------------------------------
static GwPod gpods[MAX_GWPODS];
static int g_podRx = DEF_POD_RX, g_podTx = DEF_POD_TX;
static HardwareSerial PodSerial(1);
static bool g_podOn = false;
static uint32_t g_podLines = 0, g_podBad = 0;

static void podBegin() {
  if (g_podOn) { PodSerial.end(); g_podOn = false; }
  if (g_podRx < 0) return;
  PodSerial.begin(115200, SERIAL_8N1, g_podRx, g_podTx >= 0 ? g_podTx : -1);
  PodSerial.setTimeout(20);
  g_podOn = true;
}
static GwPod* gwPodFor(const char* sid, bool create) {
  int freeIdx = -1;
  for (int i = 0; i < MAX_GWPODS; i++) {
    if (gpods[i].used && !strcmp(gpods[i].sid, sid)) return &gpods[i];
    if (!gpods[i].used && freeIdx < 0) freeIdx = i;
  }
  if (!create || freeIdx < 0) return nullptr;
  GwPod& p = gpods[freeIdx]; memset(&p, 0, sizeof(p)); p.used = true;
  strncpy(p.sid, sid, sizeof(p.sid) - 1);
  p.slot = oatcore::slotFor(p.sid, p.sid);
  if (p.slot >= 0) oatcore::slotMeta(p.slot, "", "serial pod");
  return &p;
}
static GwPod* gwPodTouch(const char* sid) {
  if (strlen(sid) >= oatcore::ID_LEN) { g_podBad++; return nullptr; }
  GwPod* p = gwPodFor(sid, true); if (!p) { g_podBad++; return nullptr; }
  if (p->gone || p->slot < 0) { p->slot = oatcore::slotFor(p->sid, p->sid); p->gone = false; }   // back after silence
  p->lastMs = millis(); p->lines++;
  return p;
}
static void podLine(char* line) {
  char* tok[8]; int n = 0;
  for (char* t = strtok(line, " \t"); t && n < 8; t = strtok(nullptr, " \t")) tok[n++] = t;
  if (n < 2) return;
  g_podLines++;
  if (!strcmp(tok[0], "M") && n >= 4) {
    GwPod* p = gwPodTouch(tok[1]); if (!p || p->slot < 0) return;
    // The codebook lends the unit and the fold kind when the line omits them;
    // a measurement it does not know still rides, unitless and averaged.
    const Code* c = nullptr; for (size_t i = 0; i < CODES_N; i++) if (!strcmp(CODES[i].measurement, tok[2])) { c = &CODES[i]; break; }
    const char* unit = (n >= 5 && strcmp(tok[4], "-")) ? tok[4] : (c ? c->unit : "");
    uint8_t kind = c ? c->kind : (uint8_t)oat::KIND_CONTINUOUS;
    if (n >= 6) { switch (tok[5][0]) { case 'c': kind = oat::KIND_CONTINUOUS; break; case 'g': kind = oat::KIND_GAUGE; break; case 'k': kind = oat::KIND_CUMULATIVE; break; case 's': kind = oat::KIND_STATE; break; case 'e': kind = oat::KIND_EVENT; break; } }
    oatcore::fold(p->slot, tok[2], unit, kind, atof(tok[3]));
  } else if (!strcmp(tok[0], "S") && n >= 3) {
    GwPod* p = gwPodTouch(tok[1]); if (!p) return;
    for (int i = 2; i < n; i++) {
      if (!strncmp(tok[i], "brand=", 6)) strncpy(p->brand, tok[i] + 6, sizeof(p->brand) - 1);
      else if (!strncmp(tok[i], "model=", 6)) strncpy(p->model, tok[i] + 6, sizeof(p->model) - 1);
    }
    if (p->slot >= 0) oatcore::slotMeta(p->slot, p->brand, p->model);
  } else if (!strcmp(tok[0], "L") && n >= 3) {
    GwPod* p = gwPodTouch(tok[1]); if (!p || p->slot < 0) return;
    int batt = -1;
    for (int i = 2; i < n; i++) {
      if (!strncmp(tok[i], "rssi=", 5)) { p->rssi = atoi(tok[i] + 5); p->haveRssi = true; }
      else if (!strncmp(tok[i], "battery=", 8)) batt = atoi(tok[i] + 8);
    }
    oatcore::slotLink(p->slot, p->haveRssi ? p->rssi : oat::NO_RSSI, batt);
  } else if (!strcmp(tok[0], "H")) {
    // the pod's own heartbeat: shown, not filed
  } else { g_podBad++; }
}
static void podPoll() {
  if (!g_podOn) return;
  static char line[160]; static int len = 0;
  while (PodSerial.available()) {
    char ch = PodSerial.read();
    if (ch == '\r') continue;
    if (ch == '\n') { line[len] = 0; if (len) podLine(line); len = 0; continue; }
    if (len < (int)sizeof(line) - 1) line[len++] = ch; else { len = 0; g_podBad++; }
  }
  static unsigned long lastSweep = 0;
  if (millis() - lastSweep < 5000) return;
  lastSweep = millis();
  for (int i = 0; i < MAX_GWPODS; i++) {
    GwPod& p = gpods[i];
    if (!p.used || p.gone || millis() - p.lastMs < POD_GONE_MS) continue;
    if (p.slot >= 0) oatcore::release(p.slot);
    p.slot = -1; p.gone = true;
    Serial.printf("[pod] %s silent for %lu min: released\n", p.sid, POD_GONE_MS / 60000UL);
  }
}
static String getPodRx() { return String(g_podRx); }
static String getPodTx() { return String(g_podTx); }
static bool setPodRx(const String& v, String& why) { int p = v.toInt(); if (p < -1 || p > 48) { why = "a GPIO number, or -1 to disable the pod port"; return false; } g_podRx = p; podBegin(); return true; }
static bool setPodTx(const String& v, String& why) { int p = v.toInt(); if (p < -1 || p > 48) { why = "a GPIO number, or -1 for none"; return false; } g_podTx = p; podBegin(); return true; }

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------
static void drawScreen() {
  if (!screen.ok) return;
  oatcore::NodeStatus st = oatcore::status();
  char b[40]; String L[6];
  L[0] = "OAT gw " + oatcore::gatewayId();
  L[1] = st.wifi ? ("WiFi " + st.ip) : ("no WiFi  AP " + st.apSsid.substring(st.apSsid.length() > 6 ? st.apSsid.length() - 6 : 0));
  int n = 0, gone = 0; for (int i = 0; i < MAX_UNITS; i++) if (units[i].used) { n++; if (units[i].gone) gone++; }
  snprintf(b, sizeof(b), "nodes %d%s fr %lu bad %lu", n, gone ? (String("(") + gone + " quiet)").c_str() : "", (unsigned long)g_rxFrames, (unsigned long)g_rxBad); L[2] = b;
  if (g_lastFrameMs) snprintf(b, sizeof(b), "last %08lx %.0fdB %s", (unsigned long)g_lastUnit, g_lastRssi, ago(g_lastFrameMs).c_str());
  else snprintf(b, sizeof(b), "last: nothing heard");
  L[3] = b;
  if (st.lastPushMs) snprintf(b, sizeof(b), "push %s %s ago (%lu)", st.lastPushOk ? "ok" : "FAIL", ago(st.lastPushMs).c_str(), (unsigned long)st.pushOk);
  else snprintf(b, sizeof(b), "push: none yet");
  L[4] = b;
  snprintf(b, sizeof(b), "%.1f SF%u %s%s", g_plan.freqMHz, g_plan.sf, radio.lastState == RADIOLIB_ERR_NONE ? "ok" : "ERR", radio.fem == FEM_GC1109 ? " GC1109" : radio.fem == FEM_KCT8103L ? " KCT8103" : ""); L[5] = b;
  screen.show(L, 6);
}

// ---------------------------------------------------------------------------
// Driver hooks
// ---------------------------------------------------------------------------
static void radioBegin() {
  static bool restored = false;
  if (!restored) { rosterRestoreAll(); restored = true; }
  bool ok = radio.begin(g_plan);
  if (!screen.ok) { screen.begin(); Serial.printf("[lora] screen %s\n", screen.ok ? "ok" : "not found"); }
  Serial.printf("[lora] %s: radio %s, %s (%.1f MHz, bw %.0f, sf %u, sync 0x%02x)\n", BOARD_NAME, Radio::stateName(radio.lastState), Radio::femName(radio.fem), g_plan.freqMHz, g_plan.bwKHz, g_plan.sf, g_plan.syncWord);
  podBegin();
  Serial.printf("[pod] port %s (rx %d tx %d)\n", g_podOn ? "open" : "off", g_podRx, g_podTx);
  if (!ok) return;
  radio.onReceive(onRx);
  radio.startReceive();
}
static void sensorSample()  { }
static void sensorCollect() {
  if (g_rxFlag) {
    g_rxFlag = false;
    static uint8_t buf[MAX_FRAME + 4];
    size_t n = radio.packetLength();
    if (n > 0 && n <= sizeof(buf)) {
      int st = radio.readData(buf, n);
      if (st == RADIOLIB_ERR_NONE) {
        // A unit coming back from 'gone' re-claims before its readings are filed.
        Header h; const char* why;
        if (parseHeader(buf, n, h, why)) { Unit* u = unitFor(h.unitId, false); if (u && u->gone) reclaimIfBack(*u); }
        handleFrame(buf, n);
      } else { g_rxBad++; strncpy(g_lastBad, Radio::stateName(st), sizeof(g_lastBad) - 1); }
    }
    radio.startReceive();
    drawScreen();                                   // a frame is an event worth showing at once
  }
  sweepAbsent();
  podPoll();
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw > 2000) { lastDraw = millis(); drawScreen(); }
}
static void sensorRescan()  { radioBegin(); }

static String statusHtml() {
  String p = "<div class='muted'>Radio " + String(Radio::stateName(radio.lastState)) + " &middot; " + String(g_plan.freqMHz, 1) + " MHz &middot; bw " + String((int)g_plan.bwKHz) + " &middot; SF" + String(g_plan.sf) + " &middot; sync " + getSync() + "</div>";
  p += "<table class='tbl'><tr><th>Field node</th><th>Sensors</th><th>Signal</th><th>Battery</th><th>Every</th><th>Last heard</th><th>Frames</th></tr>";
  int shown = 0;
  for (int i = 0; i < MAX_UNITS; i++) {
    Unit& u = units[i]; if (!u.used) continue; shown++;
    int nsub = 0; for (int j = 0; j < MAX_SUBS; j++) if (u.subs[j].used && u.subs[j].subId != 0) nsub++;
    char id[12]; snprintf(id, sizeof(id), "%08lx", (unsigned long)u.id);
    unsigned long age = (millis() - u.lastSeenMs) / 1000;
    p += "<tr><td>" + String(id) + (u.gone ? " <span class='bad'>silent</span>" : "") + "</td><td>" + String(nsub) +
         (u.orphanReadings ? " <span class='muted'>(" + String(u.orphanReadings) + " waiting for roster)</span>" : "") +
         "</td><td>" + String(u.rssi, 0) + " dBm / " + String(u.snr, 1) + " dB</td><td>" + (u.battery == BATT_NA ? String("&mdash;") : String(u.battery) + "%") +
         "</td><td>" + (u.intervalSec ? String(u.intervalSec) + " s" : String("?")) + "</td><td>" + String(age) + "s ago</td><td>" + String(u.frames) +
         (u.lost ? " <span class='muted'>(" + String(u.lost) + " lost)</span>" : "") + "</td></tr>";
  }
  p += "</table>";
  int npods = 0; for (int i = 0; i < MAX_GWPODS; i++) if (gpods[i].used) npods++;
  if (npods) {
    p += "<table class='tbl'><tr><th>Pod stream (cabled)</th><th>Model</th><th>Signal</th><th>Last line</th><th>Lines</th></tr>";
    for (int i = 0; i < MAX_GWPODS; i++) {
      GwPod& q = gpods[i]; if (!q.used) continue;
      p += "<tr><td>" + String(q.sid) + (q.gone ? " <span class='bad'>silent</span>" : "") + "</td><td>" + String(q.model[0] ? q.model : "&mdash;") + "</td><td>" + (q.haveRssi ? String(q.rssi) + " dBm" : String("&mdash;")) +
           "</td><td>" + String((millis() - q.lastMs) / 1000) + "s ago</td><td>" + String(q.lines) + "</td></tr>";
    }
    p += "</table>";
  }
  if (!shown) p += "<p class='bad'>Nothing heard yet. A field node transmits on its cadence (three minutes by default) and sends its roster first, so give it one cycle. If it stays empty: the node's radio plan (frequency, bandwidth, spreading factor, sync word) must match the four settings above exactly, then check both antennas.</p>";
  p += "<div class='muted'>Frames " + String(g_rxFrames) + " &middot; roster " + String(g_rxRoster) + " &middot; data " + String(g_rxData) + " &middot; rejected " + String(g_rxBad) + (g_lastBad[0] ? " (last: " + String(g_lastBad) + ")" : "") +
       " &middot; pod port " + (g_podOn ? "rx " + String(g_podRx) + ", " + String(g_podLines) + " line(s), " + String(g_podBad) + " bad" : String("off")) + "</div>";
  return p;
}
static String statusText() {
  String s = "lora radio=" + String(Radio::stateName(radio.lastState)) + " (" + String(Radio::femName(radio.fem)) + ") irqs=" + String(g_irqs) + " frames=" + String(g_rxFrames) + " roster=" + String(g_rxRoster) + " data=" + String(g_rxData) + " bad=" + String(g_rxBad) + "\n";
  for (int i = 0; i < MAX_UNITS; i++) {
    Unit& u = units[i]; if (!u.used) continue;
    char id[12]; snprintf(id, sizeof(id), "%08lx", (unsigned long)u.id);
    s += "unit " + String(id) + (u.gone ? " SILENT" : "") + " rssi=" + String(u.rssi, 0) + " snr=" + String(u.snr, 1) + " batt=" + (u.battery == BATT_NA ? String("-") : String(u.battery)) +
         " every=" + String(u.intervalSec) + "s last=" + String((millis() - u.lastSeenMs) / 1000) + "s frames=" + String(u.frames) + " lost=" + String(u.lost) + " orphans=" + String(u.orphanReadings) + "\n";
    for (int j = 0; j < MAX_SUBS; j++) if (u.subs[j].used) s += "  sub " + String(u.subs[j].subId) + " " + String(u.subs[j].streamId) + " slot " + String(u.subs[j].slot) + "\n";
  }
  s += "pod port " + (g_podOn ? "rx=" + String(g_podRx) + " tx=" + String(g_podTx) + " lines=" + String(g_podLines) + " bad=" + String(g_podBad) : String("off")) + "\n";
  for (int i = 0; i < MAX_GWPODS; i++) if (gpods[i].used)
    s += "  pod " + String(gpods[i].sid) + (gpods[i].gone ? " SILENT" : "") + " " + String(gpods[i].model) + " rssi=" + (gpods[i].haveRssi ? String(gpods[i].rssi) : String("-")) + " last=" + String((millis() - gpods[i].lastMs) / 1000) + "s lines=" + String(gpods[i].lines) + " slot " + String(gpods[i].slot) + "\n";
  return s;
}
static String diagLine() {
  int n = 0; for (int i = 0; i < MAX_UNITS; i++) if (units[i].used) n++;
  return "radio " + String(Radio::stateName(radio.lastState)) + ", heard " + String(g_rxFrames) + " frame(s) from " + String(n) +
         " node(s), rejected " + String(g_rxBad) + ". A radio cannot be 'wired wrong' — if frames is zero the plan does not match the nodes, or an antenna is missing.";
}

static void cmdNodes(const String&) { Serial.print(statusText()); }
// Is RF arriving at the chip at all? Ten instantaneous RSSI readings over five
// seconds. Have a node `tx` meanwhile: the number must jump by tens of dB.
static void cmdRssi(const String&) {
  Serial.println("instantaneous rssi, 10 x 0.5 s -- send a frame from a node now:");
  for (int i = 0; i < 10; i++) {
    Serial.printf("  %.0f dBm   irqs=%lu frames=%lu\n", radio.rssiNow(), (unsigned long)g_irqs, (unsigned long)g_rxFrames);
    unsigned long t = millis(); while (millis() - t < 500) { sensorCollect(); delay(5); }
  }
}
static const oatcore::Command COMMANDS[] = {
  { "nodes", "list the field nodes heard (and cabled pods), their sensors, signal and loss, and the radio interrupt count", cmdNodes },
  { "rssi",  "print the chip's live signal level for 5 s: does RF reach this board at all?", cmdRssi },
};

static const oatcore::Driver DRIVER = {
  TIER, "Field nodes heard", FW_VERSION, FW_SEMVER, NVS_NS,
  radioBegin, sensorSample, sensorCollect, nullptr, sensorRescan,
  statusHtml, statusText, nullptr, diagLine,
  FIELDS,   (int)(sizeof(FIELDS)   / sizeof(FIELDS[0])),
  COMMANDS, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
};

void setup() { oatcore::begin(DRIVER); }
void loop()  { oatcore::loop(); }
