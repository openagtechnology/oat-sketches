/* =============================================================================
   oat_ods.h  —  OAT Observation Data Standard, the shared encoder module
   -----------------------------------------------------------------------------
   Build once, deploy many (Doctrine #7). EVERY OAT device sketch links this and
   emits identical oat-ods — a BLE listener, a single 1-Wire probe, a board with
   five wired sensors. Only the transducer differs; the envelope comes from here.

   It implements the device-side Output Format Catalog from the oat-ods Core
   Contract (https://openagriculturetechnology.com/standard/reference/):

     encodeNative()  — the canonical oat-ods message (the default)
     encodeSenml()   — strict SenML (RFC 8428) array, for SenML-aware servers
     encodeFlat()    — {stream, measurement, value, ts} for custom/AI endpoints
     encodeStatus()  — the liveness/birth-death message (Sparkplug-flavored)
     ha*()           — Home Assistant MQTT discovery (topics + config payload)
     mqttTopic()     — the native per-stream topic, keyed on the LOGICAL stream

   Design rules carried from the contract:
     - ONE measurement per message.
     - stream.id is the primary key (the place/role); it never changes.
     - source.physical_id is swappable provenance (the gadget; MAC/serial).
     - gateway_id is ASSIGNABLE (a site/node name), never the raw chip MAC.
     - Consumers ignore unknown fields, so adding fields is non-breaking.

   Depends on ArduinoJson >= 7 (elastic JsonDocument — no buffer sizing needed;
   per-message payloads are small, so the old MQTT-buffer cliff is gone) and the
   Arduino String/core. Header-only; all functions inline. No transport here —
   the sketch owns the HTTP/MQTT send. This module only builds the bytes.
   ============================================================================= */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace oat {

constexpr const char* ODS_SCHEMA = "oat-ods/0.3";
constexpr long NO_RSSI = 2147483647L;   // sentinel: omit rssi when unset

// Node identity. Set once at boot. gateway_id is assignable — NOT the chip MAC —
// so a replacement board re-announces the same id and its streams resume.
struct Device {
  const char* gateway_id = "";   // e.g. "oat-greenhouse-2"
  const char* farm_id    = "";   // the OPERATOR this node belongs to. One grower with ten
                                 // nodes is one farm_id and ten gateway_ids, which is the
                                 // grouping an endpoint needs to answer "whose is this?"
                                 // without inferring it from node names. ("" omits.)
  const char* tier       = "";   // "oat-ble-listener" | "oat-1wire" | "home_assistant" ...
  const char* fw         = "";   // "oat-ble-listener/1.0.0"
  // Hardware provenance — the node-level twin of source.physical_id (the efuse-MAC
  // id, e.g. "oat-3c8a1fa77d14"). INFORMATIVE ONLY, status message only: gateway_id
  // stays the identity that streams/topics key on; this just says which physical
  // board is doing the job, so a swapped board is visible and a user can match the
  // console to the unit in their hand.
  const char* device_id  = "";
};

// One reading. Empty/sentinel optional fields are omitted from the output.
struct Reading {
  // logical identity — the primary key
  const char* stream_id   = "";  // stable; the place/role, e.g. "gh2-north-air"
  const char* stream_name = "";  // friendly label
  const char* location    = "";  // free-text place / area
  // the measurement
  const char* measurement = "";  // device_class-style: "temperature", "humidity" ...
  const char* unit        = "";  // SenML symbol: "Cel", "%RH", "%", "ppm" ... ("" omits)
  double      value       = 0.0;
  // physical provenance — swappable
  const char* physical_id = "";  // MAC/serial of the gadget now filling the slot ("" omits)
  const char* brand       = "";
  const char* model       = "";
  int         battery_pct = -1;  // <0 omits
  long        rssi        = NO_RSSI;
  // aggregation (optional)
  uint32_t    window_s    = 0;   // 0 omits
  uint32_t    samples     = 0;   // 0 omits
  const char* agg_method  = "";  // "mean"|"last"|"min"|"max"|"sum" ("" omits)
  // time
  const char* observed_at = "";  // ISO-8601 UTC, e.g. "2026-06-25T14:30:00Z" ("" omits)
};

// Node health, carried by the status message.
struct Health {
  uint32_t    uptime_s   = 0;
  uint32_t    free_heap  = 0;
  uint32_t    boot_count = 0;
  const char* reset      = "";
  long        rssi       = NO_RSSI;
  // Optional node diagnostics — the fleet beacon. Emitted only when set, so older
  // sketches that fill just the five above keep producing identical status JSON.
  uint32_t    min_free_heap   = 0;   // ESP.getMinFreeHeap() — leak high-water mark
  uint32_t    largest_block   = 0;   // largest contiguous free block — TLS/JSON headroom
  uint32_t    wifi_reconnects = 0;
  uint32_t    push_ok         = 0;
  uint32_t    push_fail       = 0;
  const char* transport       = "";  // "https" | "http"
  int         tls_ok          = -1;  // -1 unknown, 0 no, 1 yes
  const char* chip            = "";
  // Node-generic debug telemetry (additive; emitted only when set, so older
  // sketches that never fill these keep producing identical status JSON).
  float       temp_c          = -1000.0f; // internal die temp C; <-900 omits (unreliable on classic ESP32)
  uint32_t    loops_per_sec   = 0;        // main-loop iterations/sec — liveness / overwork signal
  // Where-to-find-me + workload (additive, 2026-07-10): lets the endpoint show how
  // to reach the node on its site LAN and how busy its airwaves are. Same rule —
  // emitted only when set, so older sketches produce identical status JSON.
  const char* lan_ip          = "";  // station IP on the site LAN, e.g. "192.168.1.37"
  const char* ssid            = "";  // network currently joined
  const char* mdns            = "";  // reachable name, e.g. "oat-gh2.local"
  uint32_t    ble_seen        = 0;   // BLE adverts heard since boot
  uint32_t    ble_decoded     = 0;   //   … decoded by Theengs
  uint32_t    ble_matched     = 0;   //   … that matched a reportable sensor
};

// ---- internal helpers -------------------------------------------------------
inline bool _has(const char* s) { return s && s[0]; }

inline void _putSource(JsonObject src, const Device& d, const Reading& r) {
  if (_has(d.tier))        src["tier"]        = d.tier;
  if (_has(d.gateway_id))  src["gateway_id"]  = d.gateway_id;
  if (_has(d.farm_id))     src["farm_id"]     = d.farm_id;
  if (_has(r.physical_id)) src["physical_id"] = r.physical_id;
  if (_has(r.brand))       src["brand"]       = r.brand;
  if (_has(r.model))       src["model"]       = r.model;
  if (r.battery_pct >= 0)  src["battery_pct"] = r.battery_pct;
  if (r.rssi != NO_RSSI)   src["rssi"]        = r.rssi;
}

// ---- NATIVE (canonical oat-ods observation) ---------------------------------
inline void encodeNative(const Device& d, const Reading& r, String& out) {
  JsonDocument doc;
  doc["schema"]   = ODS_SCHEMA;
  doc["msg_type"] = "observation";
  if (_has(r.observed_at)) doc["observed_at"] = r.observed_at;

  JsonObject s = doc["stream"].to<JsonObject>();
  s["id"] = r.stream_id;
  if (_has(r.stream_name)) s["name"]     = r.stream_name;
  if (_has(r.location))    s["location"] = r.location;

  doc["measurement"] = r.measurement;
  doc["value"]       = r.value;
  if (_has(r.unit)) doc["unit"] = r.unit;

  if (r.window_s || r.samples || _has(r.agg_method)) {
    JsonObject a = doc["agg"].to<JsonObject>();
    if (r.window_s)        a["window_s"] = r.window_s;
    if (r.samples)         a["samples"]  = r.samples;
    if (_has(r.agg_method)) a["method"]  = r.agg_method;
  }

  _putSource(doc["source"].to<JsonObject>(), d, r);
  out = "";
  serializeJson(doc, out);
}

// ---- SENML (RFC 8428 array) -------------------------------------------------
// Lean by design: SenML carries the value, not the provenance. bn groups records
// by stream (trailing ':' separator per the spec). Numeric time only — we set it
// from observed_at only if it is already a Unix-epoch string; ISO is left to the
// native format. Use for SenML-aware servers (ThingsBoard, CoAP, historians).
inline void encodeSenml(const Device&, const Reading& r, String& out) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  JsonObject rec = arr.add<JsonObject>();
  String bn = String(r.stream_id) + ":";
  rec["bn"] = bn;                       // String contents are copied into the doc
  rec["n"]  = r.measurement;
  if (_has(r.unit)) rec["u"] = r.unit;
  rec["v"] = r.value;
  out = "";
  serializeJson(doc, out);
}

// ---- FLAT (the easy target for a custom / AI-built endpoint) ----------------
inline void encodeFlat(const Device&, const Reading& r, String& out) {
  JsonDocument doc;
  doc["stream"]      = r.stream_id;
  doc["measurement"] = r.measurement;
  doc["value"]       = r.value;
  if (_has(r.unit))        doc["unit"] = r.unit;
  if (_has(r.observed_at)) doc["ts"]   = r.observed_at;
  out = "";
  serializeJson(doc, out);
}

// ---- BATCH (one envelope, many per-measurement records) ---------------------
// Efficient webhook delivery: ONE signed POST per cycle instead of one POST per
// measurement. The batch carries the shared device source once; each record
// carries its own stream + measurement + value + the swappable physical
// provenance. A consumer reconstructs a standalone observation by merging the
// batch-level source into a record. "seq" is a per-device monotonic counter the
// endpoint uses for replay protection (reject seq <= the last one it stored).
inline JsonArray beginBatch(JsonDocument& doc, const Device& d,
                            const char* sent_at, uint64_t seq) {
  doc["schema"]   = ODS_SCHEMA;
  doc["msg_type"] = "batch";
  if (_has(sent_at)) doc["sent_at"] = sent_at;
  doc["seq"] = seq;
  JsonObject src = doc["source"].to<JsonObject>();
  if (_has(d.tier))       src["tier"]       = d.tier;
  if (_has(d.gateway_id)) src["gateway_id"] = d.gateway_id;
  if (_has(d.farm_id))    src["farm_id"]    = d.farm_id;
  if (_has(d.fw))         src["fw"]         = d.fw;
  return doc["messages"].to<JsonArray>();
}

inline void addReading(JsonArray msgs, const Reading& r) {
  JsonObject m = msgs.add<JsonObject>();
  if (_has(r.observed_at)) m["observed_at"] = r.observed_at;
  JsonObject s = m["stream"].to<JsonObject>();
  s["id"] = r.stream_id;
  if (_has(r.stream_name)) s["name"]     = r.stream_name;
  if (_has(r.location))    s["location"] = r.location;
  m["measurement"] = r.measurement;
  m["value"]       = r.value;
  if (_has(r.unit)) m["unit"] = r.unit;
  if (r.window_s || r.samples || _has(r.agg_method)) {
    JsonObject a = m["agg"].to<JsonObject>();
    if (r.window_s)         a["window_s"] = r.window_s;
    if (r.samples)          a["samples"]  = r.samples;
    if (_has(r.agg_method)) a["method"]   = r.agg_method;
  }
  if (_has(r.physical_id) || _has(r.brand) || _has(r.model) ||
      r.battery_pct >= 0 || r.rssi != NO_RSSI) {
    JsonObject src = m["source"].to<JsonObject>();
    if (_has(r.physical_id)) src["physical_id"] = r.physical_id;
    if (_has(r.brand))       src["brand"]       = r.brand;
    if (_has(r.model))       src["model"]       = r.model;
    if (r.battery_pct >= 0)  src["battery_pct"] = r.battery_pct;
    if (r.rssi != NO_RSSI)   src["rssi"]        = r.rssi;
  }
}

// ---- STATUS (liveness / birth-death) ----------------------------------------
// msg_type=status. state is "online" (boot/heartbeat) or "offline" (MQTT Last
// Will). This is how a node with no live sensors still reports it is alive.
inline void encodeStatus(const Device& d, const char* state,
                         const char* observed_at, const Health& h, String& out) {
  JsonDocument doc;
  doc["schema"]   = ODS_SCHEMA;
  doc["msg_type"] = "status";
  if (_has(observed_at)) doc["observed_at"] = observed_at;
  doc["state"] = state;

  JsonObject src = doc["source"].to<JsonObject>();
  if (_has(d.tier))       src["tier"]       = d.tier;
  if (_has(d.gateway_id)) src["gateway_id"] = d.gateway_id;
  if (_has(d.farm_id))    src["farm_id"]    = d.farm_id;
  if (_has(d.device_id))  src["device_id"]  = d.device_id;   // status only — hardware provenance
  if (_has(d.fw))         src["fw"]         = d.fw;

  JsonObject hh = doc["health"].to<JsonObject>();
  hh["uptime_s"]  = h.uptime_s;
  hh["free_heap"] = h.free_heap;
  if (h.boot_count) hh["boot_count"] = h.boot_count;
  if (_has(h.reset)) hh["reset"]     = h.reset;
  if (h.rssi != NO_RSSI) hh["rssi"]  = h.rssi;
  if (h.min_free_heap)   hh["min_free_heap"] = h.min_free_heap;
  if (h.largest_block)   hh["largest_block"] = h.largest_block;
  if (h.wifi_reconnects) hh["wifi_reconnects"] = h.wifi_reconnects;
  if (h.push_ok || h.push_fail) { hh["push_ok"] = h.push_ok; hh["push_fail"] = h.push_fail; }
  if (_has(h.transport)) hh["transport"] = h.transport;
  if (h.tls_ok >= 0)     hh["tls_ok"] = (bool)h.tls_ok;
  if (_has(h.chip))      hh["chip"] = h.chip;
  if (h.temp_c > -900.0f) hh["temp_c"]        = h.temp_c;
  if (h.loops_per_sec)    hh["loops_per_sec"] = h.loops_per_sec;
  if (_has(h.lan_ip))     hh["lan_ip"]        = h.lan_ip;
  if (_has(h.ssid))       hh["ssid"]          = h.ssid;
  if (_has(h.mdns))       hh["mdns"]          = h.mdns;
  if (h.ble_seen)         hh["ble_seen"]      = h.ble_seen;
  if (h.ble_decoded)      hh["ble_decoded"]   = h.ble_decoded;
  if (h.ble_matched)      hh["ble_matched"]   = h.ble_matched;
  out = "";
  serializeJson(doc, out);
}

// ---- MQTT topics (keyed on the LOGICAL stream, so they survive a swap) ------
inline String mqttTopic(const char* prefix, const Device& d, const Reading& r) {
  String t = _has(prefix) ? String(prefix) : String("oat");
  t += "/"; t += d.gateway_id; t += "/"; t += r.stream_id; t += "/"; t += r.measurement;
  return t;
}
inline String statusTopic(const char* prefix, const Device& d) {
  String t = _has(prefix) ? String(prefix) : String("oat");
  t += "/"; t += d.gateway_id; t += "/status";
  return t;
}

// ---- HOME ASSISTANT MQTT discovery ------------------------------------------
// SenML units are not HA's unit symbols; translate the common ones.
inline const char* haUnit(const char* senml) {
  if (!senml || !senml[0]) return "";
  if (!strcmp(senml, "Cel"))  return "\xC2\xB0""C";   // °C (UTF-8)
  if (!strcmp(senml, "%RH"))  return "%";
  return senml;                                        // %, ppm, Pa, lx, dBm pass through
}
inline String haObjectId(const Device& d, const Reading& r) {
  String id = String(d.gateway_id) + "_" + r.stream_id + "_" + r.measurement;
  for (size_t i = 0; i < id.length(); i++) {
    char c = id[i];
    bool ok = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';
    if (!ok) id.setCharAt(i, '_');
  }
  return id;
}
inline String haConfigTopic(const Device& d, const Reading& r) {
  return String("homeassistant/sensor/") + haObjectId(d, r) + "/config";
}
// The discovery config. State goes to the native per-stream topic; HA pulls the
// reading with val_tpl from the native message's .value. avail_topic (the node's
// status topic) wires HA availability to our online/offline (the LWT pattern).
inline void haDiscoveryConfig(const Device& d, const Reading& r,
                              const char* state_topic, const char* avail_topic, String& out) {
  JsonDocument doc;
  doc["name"]     = _has(r.stream_name) ? r.stream_name : r.stream_id;
  doc["uniq_id"]  = haObjectId(d, r);
  doc["stat_t"]   = state_topic;
  doc["val_tpl"]  = "{{ value_json.value }}";
  const char* u = haUnit(r.unit);
  if (u && u[0])              doc["unit_of_meas"] = u;
  if (_has(r.measurement))    doc["dev_cla"]      = r.measurement;
  if (_has(avail_topic)) {
    JsonObject av = doc["avty"].to<JsonObject>();
    av["t"]            = avail_topic;
    av["val_tpl"]      = "{{ value_json.state }}";
    av["pl_avail"]     = "online";
    av["pl_not_avail"] = "offline";
  }
  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add(String(d.gateway_id) + "_" + r.stream_id);
  dev["name"] = _has(r.stream_name) ? r.stream_name : r.stream_id;
  if (_has(r.model)) dev["mdl"] = r.model;
  if (_has(r.brand)) dev["mf"]  = r.brand;
  out = "";
  serializeJson(doc, out);
}

} // namespace oat
