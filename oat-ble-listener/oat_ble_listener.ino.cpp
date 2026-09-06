# 1 "/tmp/tmpyxe8lg78"
#include <Arduino.h>
# 1 "/work/oat-ble-listener/oat_ble_listener.ino"
# 74 "/work/oat-ble-listener/oat_ble_listener.ino"
#include <oat_node_core.h>
#include <oat_measurands.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <decoder.h>

#define TIER "oat-ble-listener"
#define FW_SEMVER "2.0.4"
#define FW_VERSION "OAT-BLE-Listener/2.0.4"
#define NVS_NS "oatble"

#define BLE_SCAN_MS 5000
#define ENABLE_SERVICE_DATA 1
#define ADV_QUEUE_LEN 64
#define RAW_MFG_MAX 31
#define RAW_SVC_MAX 31
#define MAX_BLE_DEVS 24
#define DECODE_STACK 8192
static String toHex(const uint8_t* data, size_t len);
static bool macAllowed(const char* mac);
static void noteUnmappedKey(const char* key);
void foldMeasurement(const char* mac, const char* name, const char* brand, const char* model,
                     const char* measurement, const char* unit, uint8_t kind, double value, int rssi);
void decodeWorker(void*);
void startBLE();
static String statusHtml();
static String statusText();
static String diagLine();
static String getAllow();
static bool setAllow(const String& v, String& why);
static void cmdDevices(const String& rest);
static void sensorSample();
static void sensorCollect();
static void sensorRescan();
void setup();
void loop();
#line 94 "/work/oat-ble-listener/oat_ble_listener.ino"
static String toHex(const uint8_t* data, size_t len) {
  static const char* H = "0123456789abcdef";
  String s; s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) { s += H[data[i] >> 4]; s += H[data[i] & 0x0F]; }
  return s;
}



struct BleDev {
  bool used = false;
  char mac[18] = {0};
  char name[28] = {0};
  char brand[16]= {0};
  char model[20]= {0};
  int rssi = 0;
  unsigned long lastSeenMs = 0;
  bool gone = false;
  int slot = -1;
};
static const unsigned long DEV_GONE_MS = 10UL * 60UL * 1000UL;
static BleDev devs[MAX_BLE_DEVS];
# 128 "/work/oat-ble-listener/oat_ble_listener.ino"
static SemaphoreHandle_t devMutex = nullptr;
static inline void devLock() { if (devMutex) xSemaphoreTake(devMutex, portMAX_DELAY); }
static inline void devUnlock() { if (devMutex) xSemaphoreGive(devMutex); }



struct RawAdv {
  char mac[18];
  char name[28];
  uint8_t mfg[RAW_MFG_MAX]; uint8_t mfgLen;
  uint8_t svc[RAW_SVC_MAX]; uint8_t svcLen;
  char svcUuid[40];
  int rssi;
};
static QueueHandle_t advQueue = nullptr;

static volatile uint32_t g_advSeen = 0, g_advDecoded = 0, g_advMatched = 0;
static String g_allowlist;

static bool macAllowed(const char* mac) {
  if (g_allowlist.length() == 0) return true;
  String hay = g_allowlist; hay.toUpperCase();
  String needle = String(mac); needle.toUpperCase();
  return hay.indexOf(needle) >= 0;
}






static char g_unmapped[160] = {0};
static void noteUnmappedKey(const char* key) {
  if (!key || !key[0] || strstr(g_unmapped, key)) return;
  size_t len = strlen(g_unmapped);
  if (len + strlen(key) + 2 >= sizeof(g_unmapped)) return;
  if (len) strcat(g_unmapped, ",");
  strcat(g_unmapped, key);
}




void foldMeasurement(const char* mac, const char* name, const char* brand, const char* model,
                     const char* measurement, const char* unit, uint8_t kind, double value, int rssi) {
  if (!macAllowed(mac)) return;





  devLock();
  int d = -1, freeSlot = -1;
  for (int i = 0; i < MAX_BLE_DEVS; i++) {
    if (devs[i].used && strcmp(devs[i].mac, mac) == 0) { d = i; break; }
    if (!devs[i].used && freeSlot < 0) freeSlot = i;
  }
  if (d < 0) d = freeSlot;
  if (d < 0) { devUnlock(); return; }

  BleDev &s = devs[d];
  if (!s.used) {
    s = BleDev(); s.used = true;
    strncpy(s.mac, mac, sizeof(s.mac) - 1);


    s.slot = oatcore::slotFor(mac, mac);
  } else if (s.gone || s.slot < 0) { s.slot = oatcore::slotFor(s.mac, s.mac); s.gone = false; }
  if (name && name[0]) strncpy(s.name, name, sizeof(s.name) - 1);
  if (brand && brand[0]) strncpy(s.brand, brand, sizeof(s.brand) - 1);
  if (model && model[0]) strncpy(s.model, model, sizeof(s.model) - 1);
  s.rssi = rssi;
  s.lastSeenMs = millis();
  int slot = s.slot;
  char devName[32]; strncpy(devName, s.name, sizeof(devName) - 1); devName[sizeof(devName)-1] = 0;
  devUnlock();

  oatcore::slotName(slot, devName);
  oatcore::slotMeta(slot, brand, model);
  oatcore::slotLink(slot, rssi, -1);
  oatcore::fold(slot, measurement, unit, kind, value);
}


void decodeWorker(void*) {
  static RawAdv raw;
  static TheengsDecoder decoder;
  static JsonDocument in;
  for (;;) {
    if (xQueueReceive(advQueue, &raw, portMAX_DELAY) != pdTRUE) continue;
    vTaskDelay(1);
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
    if (in["model"].isNull() && in["brand"].isNull()) continue;
    g_advMatched++;

    const char* nm = in["name"].isNull() ? "" : in["name"].as<const char*>();
    const char* brand = in["brand"].isNull() ? "" : in["brand"].as<const char*>();
    const char* model = in["model"].isNull() ? "" : in["model"].as<const char*>();






    if (g_allowlist.length() > 0) {
      if (!macAllowed(macUp.c_str())) continue;
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
        } else { noteUnmappedKey(k); }
      }
      if (!hasPhysical) continue;
    }




    uint8_t folded = 0;
    for (JsonPair kv : inObj) {
      const char* key = kv.key().c_str();
      if (oat::isMetaKey(key)) continue;
      JsonVariant v = kv.value();
      if (v.isNull() || v.is<const char*>() || v.is<JsonObject>() || v.is<JsonArray>()) continue;
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

    foldMeasurement(macUp.c_str(), nm, brand, model, "rssi", "dBm", oat::KIND_GAUGE, (double)raw.rssi, raw.rssi);


    if (folded == 0)
      foldMeasurement(macUp.c_str(), nm, brand, model, "presence", "", oat::KIND_STATE, 1.0, raw.rssi);
  }
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {



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
    xQueueSend(advQueue, &raw, 0);
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    NimBLEDevice::getScan()->start(BLE_SCAN_MS, false, true);
  }
} scanCallbacks;




void startBLE() {
  devMutex = xSemaphoreCreateMutex();
  advQueue = xQueueCreate(ADV_QUEUE_LEN, sizeof(RawAdv));
#if CONFIG_FREERTOS_UNICORE


  xTaskCreate(decodeWorker, "oat_decode", DECODE_STACK, nullptr, 1, nullptr);
#else
  xTaskCreatePinnedToCore(decodeWorker, "oat_decode", DECODE_STACK, nullptr, 1, nullptr, 1);
#endif
  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks);
  scan->setActiveScan(false);
  scan->setMaxResults(0);
  scan->start(BLE_SCAN_MS, false, true);
}






static String statusHtml() {
  String p = "<table class='tbl'><tr><th>Device</th><th>Model</th><th>Signal</th><th>Last heard</th></tr>";
  int shown = 0;
  devLock();
  for (int i = 0; i < MAX_BLE_DEVS; i++) {
    BleDev &s = devs[i];
    if (!s.used) continue;
    shown++;
    unsigned long age = (millis() - s.lastSeenMs) / 1000;
    p += "<tr><td>" + String(s.mac) + (s.gone ? " <span class='bad'>silent</span>" : "") + (s.name[0] ? ("<br><span class='muted'>" + String(s.name) + "</span>") : "") +
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
    s += "device " + String(d.mac) + (d.gone ? " SILENT" : "") + " " + String(d.model[0] ? d.model : "?") +
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




static String getAllow() { return g_allowlist; }
static bool setAllow(const String& v, String& why) { g_allowlist = v; return true; }

static const oatcore::Field FIELDS[] = {
  { "allowlist", "MAC allow-list (optional)",
    "Comma-separated MACs. Empty means every device emitting a known physical measurement is reported. "
    "Naming a device here reports it in full, including values the 'real sensors only' default would skip.",
    getAllow, setAllow },
};


static void cmdDevices(const String& rest) { Serial.print(statusText()); }

static const oatcore::Command COMMANDS[] = {
  { "devices", "list the BLE devices heard, with signal and age", cmdDevices },
};







static void sensorSample() { }


static void sensorCollect() {
  static unsigned long last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  devLock();
  for (int i = 0; i < MAX_BLE_DEVS; i++) {
    BleDev &d = devs[i];
    if (!d.used || d.gone || millis() - d.lastSeenMs < DEV_GONE_MS) continue;
    if (d.slot >= 0) oatcore::release(d.slot);
    d.slot = -1; d.gone = true;
  }
  devUnlock();
}
static void sensorRescan() { NimBLEDevice::getScan()->stop(); startBLE(); }

static const oatcore::Driver DRIVER = {
  TIER, "Sensors heard", FW_VERSION, FW_SEMVER, NVS_NS,
  startBLE, sensorSample, sensorCollect, nullptr, sensorRescan,
  statusHtml, statusText, nullptr, diagLine,
  FIELDS, (int)(sizeof(FIELDS) / sizeof(FIELDS[0])),
  COMMANDS, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
};

void setup() { oatcore::begin(DRIVER); }
void loop() { oatcore::loop(); }