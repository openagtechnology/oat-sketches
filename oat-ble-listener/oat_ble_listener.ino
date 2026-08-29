/* =============================================================================
   OAT BLE Sensor Listener  —  v2.0.2
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   Hears BLE sensors a grower already owns (Govee, Xiaomi, Inkbird, ATC and the rest
   of the Theengs catalogue), harvests every value they broadcast, and pushes them as
   oat-ods to an endpoint the operator owns. Nothing is paired and nothing is bought:
   these devices shout their readings to anyone listening, and this listens.

   v2.0.0 is the reference node ported to oat_node_core — the library that was
   EXTRACTED FROM THIS SKETCH. Everything that was never about BLE (config, NVS, the
   field registry, WiFi, the AP, the pages, the Console, the push engine, the
   heartbeat) now comes from the core, and this file is the radio and the decode.
   Closing that loop is the point: the node that every other sketch was supposed to
   copy is now the node that copies nothing.

   IT GAINS SOMETHING IN THE MOVE. The setup page never listed the devices it had
   heard — you filled in the form and then went hunting on the status page to find
   out whether it had heard anything at all. The core renders a driver's sensor block
   on BOTH pages, so the list now appears where someone is actually deciding whether
   their wiring and placement are right.

   HOW IT WORKS
     The NimBLE scan callback runs in the host task on CPU0 and does the minimum:
     copy the advert into a POD struct and queue it. A worker task on the app core
     does the Theengs model-match and the decode, then folds each harvested value.
     Heavy work in the callback is what used to drop adverts under load.

     Every numeric field a device broadcasts is harvested, not just temperature:
     mapped to a canonical measurand where the shared dictionary knows it, forwarded
     RAW under its decoder key where it does not — so nothing a sensor says is lost
     because we had not thought of it yet, and `status` lists the unmapped keys so
     they can be promoted later.

   THE 'REAL SENSORS ONLY' DEFAULT
     With no allow-list, a device is reported only if it emits a KNOWN physical
     measurement. Without that gate a busy room fills your endpoint with strangers'
     phones and trackers: Apple Continuity decodes a screen-lock as a state change,
     AirPods report three battery levels. Phones also rotate their advertised address
     every ~15 minutes, so one physical device becomes many ephemeral streams and no
     MAC allow-list can ever pin them down. "Is it a real sensor" is the only filter
     that works. The MAC allow-list is the single opt-in: allow-listed devices are
     reported in full.

   CHANGELOG
     2.0.2  Fix a boot loop introduced by the port: the display table's mutex was
            declared and never created, so the first advert decoded (or the first
            page render) took a NULL semaphore and FreeRTOS asserted --
            `xQueueSemaphoreTake queue.c:1709 (( pxQueue ))`, reboot, repeat, on
            every chip. The pre-port sketch created it in its own setup(); when
            setup() moved into the core the create went with it. It is now created
            in startBLE() before the worker task exists, and every use goes through
            devLock()/devUnlock() so a null handle can never panic the node again.
     2.0.1  Keep sending the name the device broadcasts about itself. The port
            dropped stream.name along with the wired sketches' useless copy of the
            id — but a Govee's "GVH5100_484B" is the DEVICE's own label, not one
            this node invented, and losing it on an upgrade left an endpoint with a
            bare MAC where it used to have something a person could recognise.
     2.0.0  Ported to oat_node_core. Same radio, same decode, same payloads.
            Dropped `avg_n`, a setting that was saved and rendered and never read by
            anything. Earlier history is in git.

   LICENSE: openly licensed. Copy it, change it, sell what you build with it.
   ============================================================================= */

#include <oat_node_core.h>
#include <oat_measurands.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <decoder.h>

#define TIER        "oat-ble-listener"
#define FW_SEMVER   "2.0.2"
#define FW_VERSION  "OAT-BLE-Listener/2.0.2"
#define NVS_NS      "oatble"          // unchanged, so a 1.4.x node keeps its settings

#define BLE_SCAN_MS       5000        // length of each scan window (ms)
#define ENABLE_SERVICE_DATA 1         // 0 if your NimBLE build lacks the service-data getters
#define ADV_QUEUE_LEN     64          // raw adverts buffered between worker wakeups
#define RAW_MFG_MAX       31
#define RAW_SVC_MAX       31
#define MAX_BLE_DEVS      24          // tracked unique devices (bounded by design)
#define DECODE_STACK      8192        // worker task stack (Theengs + ArduinoJson are stack-hungry)

// Hex for the decoder's input: it wants manufacturer and service data as a string.
static String toHex(const uint8_t* data, size_t len) {
  static const char* H = "0123456789abcdef";
  String s; s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) { s += H[data[i] >> 4]; s += H[data[i] & 0x0F]; }
  return s;
}

// What the operator sees. The core holds the readings; this holds who they came
// from, which is a different question and a different lifetime.
struct BleDev {
  bool     used = false;
  char     mac[18]  = {0};
  char     name[28] = {0};
  char     brand[16]= {0};
  char     model[20]= {0};
  int      rssi = 0;
  unsigned long lastSeenMs = 0;
  int      slot = -1;                 // its slot in the core's table
};
static BleDev devs[MAX_BLE_DEVS];

// The display table is written by the decode worker and read by the web pages and
// the Console, so it needs its own lock. It is NOT the core's slot mutex: slotFor/
// fold/release take that one themselves, and a non-recursive mutex taken twice from
// one task deadlocks.
//
// Take it through devLock()/devUnlock() rather than directly. xSemaphoreTake() on a
// null handle does not fail, it ASSERTS -- `xQueueSemaphoreTake queue.c:1709
// (( pxQueue ))` -- and the node reboots in a loop with a backtrace that names
// FreeRTOS rather than the sketch. That is exactly what this sketch shipped doing:
// the pre-port version created the mutex in its own setup(), and when setup() moved
// into the core the create went with it and was never re-added here.
static SemaphoreHandle_t devMutex = nullptr;
static inline void devLock()   { if (devMutex) xSemaphoreTake(devMutex, portMAX_DELAY); }
static inline void devUnlock() { if (devMutex) xSemaphoreGive(devMutex); }

// A raw advertisement copied out of the NimBLE callback. POD so it rides the
// FreeRTOS queue by value — no String or heap touched in the host task.
struct RawAdv {
  char    mac[18];
  char    name[28];
  uint8_t mfg[RAW_MFG_MAX]; uint8_t mfgLen;
  uint8_t svc[RAW_SVC_MAX]; uint8_t svcLen;
  char    svcUuid[40];
  int     rssi;
};
static QueueHandle_t advQueue = nullptr;

static volatile uint32_t g_advSeen = 0, g_advDecoded = 0, g_advMatched = 0;
static String g_allowlist;            // "" = accept every recognised device

static bool macAllowed(const char* mac) {
  if (g_allowlist.length() == 0) return true;
  String hay = g_allowlist; hay.toUpperCase();
  String needle = String(mac); needle.toUpperCase();
  return hay.indexOf(needle) >= 0;
}

// ----------------------------------------------------------------------------
// BLE: scan callback (CPU0, trivial) -> queue -> decode worker (app core)
// ----------------------------------------------------------------------------
// Diagnostic: decoder keys we saw but the dictionary hasn't mapped (forwarded raw).
// Single writer (the decode worker); readers only print it, so a plain char buffer is fine.
static char g_unmapped[160] = {0};
static void noteUnmappedKey(const char* key) {
  if (!key || !key[0] || strstr(g_unmapped, key)) return;
  size_t len = strlen(g_unmapped);
  if (len + strlen(key) + 2 >= sizeof(g_unmapped)) return;   // full — stop noting
  if (len) strcat(g_unmapped, ",");
  strcat(g_unmapped, key);
}

// Fold one measurand into its sensor's window accumulator. Generic over measurement
// type — the decode worker calls this once per harvested field (temp, moisture, CO2,
// a raw-forwarded key, ...). Aggregation is picked by `kind` at snapshot time.
void foldMeasurement(const char* mac, const char* name, const char* brand, const char* model,
                     const char* measurement, const char* unit, uint8_t kind, double value, int rssi) {
  if (!macAllowed(mac)) return;

  // Our own table (what the operator sees) is written from the decode worker and
  // read from the web handlers, so it takes ITS OWN lock. It deliberately does not
  // reuse the core's: oatcore::fold() takes the core's lock itself, and taking a
  // non-recursive mutex twice from one task is a deadlock, not a warning.
  devLock();
  int d = -1, freeSlot = -1;
  for (int i = 0; i < MAX_BLE_DEVS; i++) {
    if (devs[i].used && strcmp(devs[i].mac, mac) == 0) { d = i; break; }
    if (!devs[i].used && freeSlot < 0) freeSlot = i;
  }
  if (d < 0) d = freeSlot;
  if (d < 0) { devUnlock(); return; }        // table full: bounded by design

  BleDev &s = devs[d];
  if (!s.used) {
    s = BleDev(); s.used = true;
    strncpy(s.mac, mac, sizeof(s.mac) - 1);
    // A BLE sensor's MAC is its hardware id, so it IS the stream id (oat-ods §3/§4).
    // This is what the reference node always did; the core now enforces it for all.
    s.slot = oatcore::slotFor(mac, mac);
  }
  if (name && name[0])   strncpy(s.name,  name,  sizeof(s.name)  - 1);
  if (brand && brand[0]) strncpy(s.brand, brand, sizeof(s.brand) - 1);
  if (model && model[0]) strncpy(s.model, model, sizeof(s.model) - 1);
  s.rssi = rssi;
  s.lastSeenMs = millis();
  int slot = s.slot;
  char devName[32]; strncpy(devName, s.name, sizeof(devName) - 1); devName[sizeof(devName)-1] = 0;
  devUnlock();

  oatcore::slotName(slot, devName);                 // the name the DEVICE broadcast
  oatcore::slotMeta(slot, brand, model);            // whose device this actually is
  oatcore::slotLink(slot, rssi, -1);                // battery arrives as its own measurand
  oatcore::fold(slot, measurement, unit, kind, value);
}

// Decode worker — one reused JsonDocument + decoder (no per-advert heap churn).
void decodeWorker(void*) {
  static RawAdv raw;
  static TheengsDecoder decoder;
  static JsonDocument in;
  for (;;) {
    if (xQueueReceive(advQueue, &raw, portMAX_DELAY) != pdTRUE) continue;
    vTaskDelay(1);   // yield per advert so the loop (Console/push/web) never starves on a busy airwave
    in.clear();
    String macUp = String(raw.mac); macUp.toUpperCase();
    in["id"] = macUp;
    if (raw.name[0]) in["name"] = raw.name;
    bool haveData = false;
    if (raw.mfgLen) { in["manufacturerdata"] = toHex(raw.mfg, raw.mfgLen); haveData = true; }
#if ENABLE_SERVICE_DATA
    if (raw.svcLen) {
      in["servicedata"] = toHex(raw.svc, raw.svcLen);
      if (raw.svcUuid[0]) in["servicedatauuid"] = raw.svcUuid;
      haveData = true;
    }
#endif
    if (!haveData) continue;
    g_advDecoded++;

    JsonObject inObj = in.as<JsonObject>();
    decoder.decodeBLEJson(inObj);
    if (in["model"].isNull() && in["brand"].isNull()) continue;   // not a sensor Theengs knows
    g_advMatched++;

    const char* nm    = in["name"].isNull()  ? "" : in["name"].as<const char*>();
    const char* brand = in["brand"].isNull() ? "" : in["brand"].as<const char*>();
    const char* model = in["model"].isNull() ? "" : in["model"].as<const char*>();

    // "Real sensors only" gate (v1.4.3). With no allow-list, report a device ONLY if it
    // carries a real physical measurement — this drops ambient phones (Apple Continuity
    // screen-lock -> `unlocked`), strangers' beacons, and state-only advertisers that
    // flood a listener in a populated room. An allow-list is the opt-in: any MAC on it is
    // reported in full (lock state, beacon presence). Empty list => sensors only.
    if (g_allowlist.length() > 0) {
      if (!macAllowed(macUp.c_str())) continue;              // allow-list = hard filter
    } else {
      bool hasPhysical = false;
      for (JsonPair kv : inObj) {
        const char* k = kv.key().c_str();
        if (oat::isMetaKey(k)) continue;
        JsonVariant vv = kv.value();
        if (vv.isNull() || vv.is<const char*>() || vv.is<JsonObject>() || vv.is<JsonArray>()) continue;
        oat::Measurand m2;
        if (oat::lookupMeasurand(k, m2)) {
          if (m2.kind == oat::KIND_CONTINUOUS || m2.kind == oat::KIND_GAUGE || m2.kind == oat::KIND_CUMULATIVE) { hasPhysical = true; break; }
        } else { noteUnmappedKey(k); }   // unknown key: surface for promotion, but it does NOT qualify a device
      }
      if (!hasPhysical) continue;                            // no KNOWN physical measurement -> ambient/consumer/unknown-only -> ignore
    }

    // Harvest EVERY measurand the device emitted. Each numeric decoder key is either
    // mapped to a canonical oat-ods measurement via the dictionary, or (by policy)
    // forwarded raw so nothing the device measured is silently discarded.
    uint8_t folded = 0;
    for (JsonPair kv : inObj) {
      const char* key = kv.key().c_str();
      if (oat::isMetaKey(key)) continue;                    // identity/metadata, never a measurement
      JsonVariant v = kv.value();
      if (v.isNull() || v.is<const char*>() || v.is<JsonObject>() || v.is<JsonArray>()) continue;  // numeric only
      double val = v.is<bool>() ? (v.as<bool>() ? 1.0 : 0.0) : v.as<double>();
      oat::Measurand md;
      if (oat::lookupMeasurand(key, md)) {
        foldMeasurement(macUp.c_str(), nm, brand, model,
                        md.measurement, md.unit, md.kind, oat::applyXform(md.xform, val), raw.rssi);
        folded++;
      } else if (oat::OAT_FORWARD_UNKNOWN) {
        foldMeasurement(macUp.c_str(), nm, brand, model, key, "", oat::KIND_CONTINUOUS, val, raw.rssi);
        noteUnmappedKey(key);
        folded++;
      }
    }
    // Link quality as a first-class viewable metric (best-effort slot; gauge=last).
    foldMeasurement(macUp.c_str(), nm, brand, model, "rssi", "dBm", oat::KIND_GAUGE, (double)raw.rssi, raw.rssi);
    // A recognized device that carried NO real measurand is a beacon/tracker in range —
    // report it as presence rather than a lonely nothing (turns beacons into sensors).
    if (folded == 0)
      foldMeasurement(macUp.c_str(), nm, brand, model, "presence", "", oat::KIND_STATE, 1.0, raw.rssi);
  }
}

class ScanCallbacks : public NimBLEScanCallbacks {   // NIMBLE 2.x
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    // Heavy work is forbidden here — this runs in the NimBLE host task on CPU0.
    // Copy the advert into a POD and hand it to the worker; drop if the queue is
    // full (the adverts are chatty and redundant — losing a few is harmless).
    RawAdv raw; memset(&raw, 0, sizeof(raw));
    strncpy(raw.mac, dev->getAddress().toString().c_str(), sizeof(raw.mac) - 1);
    if (dev->haveName()) strncpy(raw.name, dev->getName().c_str(), sizeof(raw.name) - 1);
    std::string md = dev->getManufacturerData();
    if (md.size()) { raw.mfgLen = md.size() > RAW_MFG_MAX ? RAW_MFG_MAX : md.size();
                     memcpy(raw.mfg, md.data(), raw.mfgLen); }
#if ENABLE_SERVICE_DATA
    if (dev->getServiceDataCount() > 0) {
      std::string sd = dev->getServiceData(0);
      if (sd.size()) { raw.svcLen = sd.size() > RAW_SVC_MAX ? RAW_SVC_MAX : sd.size();
                       memcpy(raw.svc, sd.data(), raw.svcLen);
                       strncpy(raw.svcUuid, dev->getServiceDataUUID(0).toString().c_str(), sizeof(raw.svcUuid) - 1); }
    }
#endif
    if (!raw.mfgLen && !raw.svcLen) return;
    raw.rssi = dev->getRSSI();
    g_advSeen++;
    xQueueSend(advQueue, &raw, 0);     // 0 = never block the host task
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    NimBLEDevice::getScan()->start(BLE_SCAN_MS, false, true);   // continuous coverage
  }
} scanCallbacks;

// ----------------------------------------------------------------------------
// BLE bring-up
// ----------------------------------------------------------------------------
void startBLE() {
  devMutex = xSemaphoreCreateMutex();          // BEFORE the worker task and the queue
  advQueue = xQueueCreate(ADV_QUEUE_LEN, sizeof(RawAdv));
#if CONFIG_FREERTOS_UNICORE
  // Single-core parts (C6/C3/C2/H2) have no core 1 — pinning to it panics at boot
  // (constant reboot). Create with no affinity; the one core runs it.
  xTaskCreate(decodeWorker, "oat_decode", DECODE_STACK, nullptr, 1, nullptr);
#else
  xTaskCreatePinnedToCore(decodeWorker, "oat_decode", DECODE_STACK, nullptr, 1, nullptr, 1);
#endif
  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks);
  scan->setActiveScan(false);                          // PASSIVE: sensors broadcast their data
  scan->setMaxResults(0);
  scan->start(BLE_SCAN_MS, false, true);
}

// ---------------------------------------------------------------------------
// What the operator sees. This block now appears on the SETUP page as well as the
// status page, which is the gain from the port: the question "has it heard
// anything?" is asked while you are setting it up, not afterwards.
// ---------------------------------------------------------------------------
static String statusHtml() {
  String p = "<table class='tbl'><tr><th>Device</th><th>Model</th><th>Signal</th><th>Last heard</th></tr>";
  int shown = 0;
  devLock();
  for (int i = 0; i < MAX_BLE_DEVS; i++) {
    BleDev &s = devs[i];
    if (!s.used) continue;
    shown++;
    unsigned long age = (millis() - s.lastSeenMs) / 1000;
    p += "<tr><td>" + String(s.mac) + (s.name[0] ? ("<br><span class='muted'>" + String(s.name) + "</span>") : "") +
         "</td><td>" + String(s.model[0] ? s.model : "&mdash;") +
         "</td><td>" + String(s.rssi) + " dBm</td><td>" + String(age) + "s ago</td></tr>";
  }
  devUnlock();
  p += "</table>";
  if (!shown)
    p += "<p class='bad'>Nothing heard yet. A BLE sensor broadcasts every few seconds, so give it a "
         "minute. If it stays empty: the sensor may be asleep, out of range, or one this node "
         "deliberately ignores (see the note below).</p>";
  if (g_unmapped[0])
    p += "<div class='muted'>Values arriving under keys the dictionary does not map yet, forwarded raw: <code>" +
         String(g_unmapped) + "</code>. Nothing is lost; these can be promoted to named measurands.</div>";
  p += "<div class='muted'>Adverts heard " + String(g_advSeen) + " &middot; decoded " + String(g_advDecoded) +
       " &middot; matched a known model " + String(g_advMatched) + "</div>";
  if (g_allowlist.length() == 0)
    p += "<div class='muted'>No allow-list, so only devices emitting a known physical measurement are "
         "reported. That is what keeps a busy room's phones and trackers out of your data.</div>";
  return p;
}

static String statusText() {
  String s = "ble adverts seen=" + String(g_advSeen) + " decoded=" + String(g_advDecoded) +
             " matched=" + String(g_advMatched) + "\n";
  devLock();
  for (int i = 0; i < MAX_BLE_DEVS; i++) {
    BleDev &d = devs[i];
    if (!d.used) continue;
    s += "device " + String(d.mac) + " " + String(d.model[0] ? d.model : "?") +
         " rssi=" + String(d.rssi) + " last=" + String((millis() - d.lastSeenMs) / 1000) + "s ago\n";
  }
  devUnlock();
  if (g_unmapped[0]) s += "unmapped decoder keys (forwarded raw): " + String(g_unmapped) + "\n";
  return s;
}

static String diagLine() {
  int n = 0;
  devLock();
  for (int i = 0; i < MAX_BLE_DEVS; i++) if (devs[i].used) n++;
  devUnlock();
  return "heard " + String(g_advSeen) + " adverts, decoded " + String(g_advDecoded) +
         ", from " + String(n) + " device(s). A radio cannot be 'wired wrong' — if this is zero, the "
         "sensor is asleep, out of range, or not one the decoder knows.";
}

// ---------------------------------------------------------------------------
// Settings. avg_n is gone: it was saved, rendered and never read by anything.
// ---------------------------------------------------------------------------
static String getAllow() { return g_allowlist; }
static bool   setAllow(const String& v, String& why) { g_allowlist = v; return true; }

static const oatcore::Field FIELDS[] = {
  { "allowlist", "MAC allow-list (optional)",
    "Comma-separated MACs. Empty means every device emitting a known physical measurement is reported. "
    "Naming a device here reports it in full, including values the 'real sensors only' default would skip.",
    getAllow, setAllow },
};

// ---------------------------------------------------------------------------
static void cmdDevices(const String& rest) { Serial.print(statusText()); }

static const oatcore::Command COMMANDS[] = {
  { "devices", "list the BLE devices heard, with signal and age", cmdDevices },
};

// ---------------------------------------------------------------------------
// The radio is push-driven: adverts arrive when they arrive. So there is nothing to
// trigger on the sample timer and nothing to finish afterwards — the worker folds
// as it decodes. An interface that only fitted poll-driven parts would have needed
// bending here; it did not.
// ---------------------------------------------------------------------------
static void sensorSample()   { }
static void sensorCollect()  { }
static void sensorRescan()   { NimBLEDevice::getScan()->stop(); startBLE(); }

static const oatcore::Driver DRIVER = {
  TIER, "Sensors heard", FW_VERSION, FW_SEMVER, NVS_NS,
  startBLE, sensorSample, sensorCollect, nullptr, sensorRescan,
  statusHtml, statusText, nullptr, diagLine,
  FIELDS,   (int)(sizeof(FIELDS)   / sizeof(FIELDS[0])),
  COMMANDS, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
};

void setup() { oatcore::begin(DRIVER); }
void loop()  { oatcore::loop(); }
