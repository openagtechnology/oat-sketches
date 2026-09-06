/* =============================================================================
   oat_node_core.cpp — the shared node. See oat_node_core.h for why this exists.

   This file is EXTRACTED, not rewritten: it is the code that ran on the bench in
   the BLE Listener, the SHT-30 node and the DS18B20 node, moved to one place so a
   fix reaches every node instead of one of three copies. The sensor-specific parts
   became Driver hooks; nothing else was retyped.
   ============================================================================= */
#include "oat_node_core.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <time.h>
#include <oat_ods.h>
#include <oat_measurands.h>
#include <oat_sign.h>

namespace oatcore {

static const Driver* g_drv = nullptr;

// ---------------------------------------------------------------------------
// The slot table: one entry per sensor the driver has found, holding the window
// accumulators the push is built from. The driver keeps its own bookkeeping (what
// answered, what failed, last value for display); the core only needs identity and
// the fold, which is why this table is small and the same for every bus.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Driver hooks. Null-safe so a driver only implements what its bus needs.
// ---------------------------------------------------------------------------
static void driverSample()         { if (g_drv->startSample)    g_drv->startSample(); }
static void driverCollect()        { if (g_drv->collect)        g_drv->collect(); }
static void driverSampleBlocking() { if (g_drv->sampleBlocking) g_drv->sampleBlocking();
                                     else driverSample(); }
static bool dispatchDriverCommand(const String& cmd, const String& rest) {
  for (int i = 0; i < g_drv->nCommands; i++)
    if (cmd == g_drv->commands[i].name) { g_drv->commands[i].run(rest); return true; }
  return false;
}

// ----------------------------------------------------------------------------
// Compile-time constants
// ----------------------------------------------------------------------------
#define ADMIN_USER        "admin"                 // Basic Auth username (password is configurable)
#define DEFAULT_ADMIN_PW  "oatsetup"              // default admin password (changeable in setup)



// Transport headroom: a TLS handshake needs a big contiguous block. Below this,
// skip TLS and use signed http (the HMAC keeps it safe). Tunable.
#define TLS_MIN_HEAP      45000
#define BEACON_EVERY_MS   60000UL                  // heartbeat cadence: 60s — a FIXED CONSTANT of the oat-ods standard (in the spec, NOT the payload)
#define TLS_REPROBE_MS    600000UL                 // after a fallback, retry https this often (10 min)
#define AP_GRACE_MS       300000UL                 // keep AP this long after STA connects, then drop it
#define WIFI_RETRY_MS     15000UL                  // station reconnect cadence
#define BOOT_BTN_PIN      0                        // GPIO0 (BOOT) — held at power-on on this board family

#define MAX_BATCH_MEAS    160                      // hard cap on measurements per webhook batch (heap guard)

// Branded setup page logo (viewer fetches; device stores nothing; degrades on AP).
#define LOGO_URL "https://openagriculturetechnology.com/assets/img/logo-horizontal.png"

static const byte  DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);

// Forward declarations (functions referenced before their definition below).
void scanWifi();
void startMdns();
void startAP();
void stopAP();
void startNetworking();
void staBegin();
void reconnectWifi();
void pushAll();
void pushBeacon();
static void driverSample();
static void driverCollect();

// ----------------------------------------------------------------------------
// Configuration (persisted in NVS)
// ----------------------------------------------------------------------------
struct Config {
  // Identity
  String device_id;        // permanent, hardware-derived (full MAC); not editable
  String device_name;      // friendly label (also seeds the mDNS hostname)
  String farm_id;          // operator / site id
  String location;         // free-text zone, e.g. "Greenhouse 2 / North bench"

  // WiFi
  String ssid;
  String wifi_pw;

  // Delivery
  String method;           // "webhook" | "mqtt"
  // -- webhook --
  String ep_url;           // full https/http URL (https preferred; auto-falls-back to http if TLS won't fit)
  String ep_auth;          // optional: full Authorization header value (e.g. "Bearer abc123")
  String ep_key;           // optional shared secret: signs each push (HMAC). Never sent. Empty = unsigned/sandbox.
  // -- mqtt --
  String mq_host;
  uint16_t mq_port;
  String mq_user;
  String mq_pw;
  String mq_topic;         // topic prefix; final topic = prefix/gateway_id/stream/measurement
  bool   mq_tls;

  // Timing & sampling
  uint32_t interval_s;     // push interval to endpoint
  uint32_t sample_s;       // how often to read the sensor within a push window


  // Misc
  String ntp_server;       // for ISO8601 timestamps
  String admin_pw;         // setup page password (default oatsetup)
  bool   lineout;          // print every pushed reading on Serial in the oat-line grammar
                           // (so this node is a POD to anything cabled to its serial port)
};

Config cfg;
Preferences prefs;
uint32_t bootCount = 0;     // NVS-persisted, incremented each boot (telemetry)
// The boot counter is read at boot and written only after the node has been up
// this long, so a reset loop never writes flash.
//
// The honest edge: the shortest push interval is 10 s, so a node CAN push a few
// messages and then lose power before 45 s, leaving NVS a boot behind. The next
// cold boot then reuses that sequence base and the endpoint answers 409 (replay:
// seq not increasing, ER #164) until a boot survives 45 s and commits. That is a
// self-healing annoyance; hammering flash during a reset loop is not, which is why
// the trade goes this way. Soft resets do not hit this at all: the RTC-retained
// counter below keeps climbing without flash.
static const unsigned long BOOT_COUNT_COMMIT_MS = 45000UL;
static bool g_bootCountCommitted = false;
// Retained across soft resets, cleared on power-up. NOINIT on purpose: the magic
// is how we tell a retained value from uninitialised RTC RAM.
#define RTC_SEQ_MAGIC 0x0A7B0075UL
RTC_NOINIT_ATTR static uint32_t rtcBootCount;
RTC_NOINIT_ATTR static uint32_t rtcSeqMagic;

// ----------------------------------------------------------------------------
// Runtime state
// ----------------------------------------------------------------------------
WebServer       server(80);
DNSServer       dnsServer;
WiFiClient      netPlain;
WiFiClientSecure netTls;
PubSubClient    mqtt;

SemaphoreHandle_t sensorMutex;

// Network / AP lifecycle
bool          apActive    = false;
unsigned long apDropAtMs  = 0;       // when (millis) to drop the AP after a connect; 0 = keep
bool          apSticky    = false;   // AP summoned by serial — don't auto-drop
bool          staWasUp    = false;
unsigned long lastWifiTry = 0;
unsigned long staDownSince = 0;
uint32_t      wifiReconnects = 0;
String        g_ssidOptions;         // cached <option> list from the last WiFi scan
String        g_mdnsHost;            // current advertised hostname (no .local)

// Transport state
String        lastTransport = "";    // "https" | "http"
bool          tlsFailed     = false; // last https attempt failed -> fell back to http
unsigned long lastTlsProbe  = 0;

// Push bookkeeping
unsigned long lastPushMs = 0;
bool          lastPushOk = false;
String        lastPushMsg = "no push yet";
time_t        lastPushEpoch = 0;
uint32_t      pushCount = 0;
uint32_t      pushOk = 0, pushFail = 0;
unsigned long lastBeaconMs = 0;
uint64_t      g_seq = 0;                 // monotonic per-POST sequence (seeded from bootCount); endpoint replay guard

// Sampling / diagnostics
unsigned long lastSampleMs = 0;
bool          firstSample = true;   // take one reading straight away, don't wait a whole interval
uint32_t      g_loopCount   = 0;    // ++ every loop(); a rate is derived from the delta
uint32_t      g_loopsPerSec     = 0;
unsigned long g_loopSampleMs    = 0;
uint32_t      g_loopSampleCount = 0;
uint32_t      g_readsOk = 0, g_readsFail = 0, g_crcFail = 0;   // whole-node totals
uint32_t      g_busResets = 0;
String        lastReadMsg = "no read yet";


// One measurand's window accumulator. kind picks the fold: continuous averages
// (sum/n), event counts (n), everything else keeps the latest (last). Identical
// shape to the reference node, so the snapshot + emit code below is the same.
struct MeasAcc {
  bool     used = false;
  uint8_t  kind = 0;                        // oat::MeasKind
  char     measurement[20] = {0};
  char     unit[12] = {0};                   // SenML symbol ("" = unitless)
  double   sum = 0;                          // continuous: running sum
  uint32_t n = 0;                            // sample count (continuous) / occurrences (event)
  double   last = 0;                         // gauge/state/cumulative: latest value
};

// One sensor's worth of window, and its identity. The driver keeps its own
// bookkeeping (what answered, what failed, last value for display); the core only
// needs identity plus the fold, which is why this is the same for every bus.
struct Slot {
  bool     used = false;
  char     id[ID_LEN] = {0};
  char     physical[ID_LEN] = {0};
  char     name[32] = {0};             // supplied BY THE DEVICE, never invented here
  char     brand[20] = {0};
  char     model[24] = {0};
  int      rssi = oat::NO_RSSI;        // absent, not "0 dBm" — 0 is a very strong signal,
  int      battery_pct = -1;           // and a wired probe has no radio link at all
  uint32_t samples = 0;
  unsigned long windowStartMs = 0;
  MeasAcc  meas[MEAS_PER_SLOT];
};
static Slot slots[MAX_SLOTS];




// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------
static String macFromChip() {
  // Read the factory MAC straight from efuse — stable the instant the chip powers
  // on, no WiFi init required. WiFi.macAddress() read before the radio is up
  // returns a different MAC on some boots, which spawns phantom device ids.
  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
  char buf[20];
  snprintf(buf, sizeof(buf), "oat-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

static String apSsid() {
  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);   // same efuse MAC as the device id
  char buf[20];
  snprintf(buf, sizeof(buf), "OAT-Setup-%02X%02X%02X", m[3], m[4], m[5]);
  return String(buf);
}

// Stable mDNS hostname from the friendly name (sanitized) or the MAC id. Lower-
// case, [a-z0-9-] only — so oat-greenhouse2.local resolves and stays unique.
static String mdnsHost() {
  String base = cfg.device_name.length() ? cfg.device_name : cfg.device_id;
  String h; h.reserve(base.length() + 4);
  for (size_t i = 0; i < base.length(); i++) {
    char c = base[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) h += c;
    else if (c == ' ' || c == '-' || c == '_') { if (h.length() && h[h.length()-1] != '-') h += '-'; }
  }
  while (h.length() && h[h.length()-1] == '-') h.remove(h.length()-1);
  if (h.length() == 0) h = cfg.device_id;
  if (!h.startsWith("oat")) h = "oat-" + h;
  if (h.length() > 32) h = h.substring(0, 32);
  return h;
}

static String localUrls() {              // "http://192.168.1.57 · http://oat-x.local"
  String s = "http://" + WiFi.localIP().toString();
  if (g_mdnsHost.length()) s += " &middot; http://" + g_mdnsHost + ".local";
  return s;
}

static String localUrlsPlain() {         // the same, for the serial log (no HTML entity)
  String s = "http://" + WiFi.localIP().toString();
  if (g_mdnsHost.length()) s += " | http://" + g_mdnsHost + ".local";
  return s;
}

static String isoNowUTC() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";          // clock not yet synced
  struct tm t; gmtime_r(&now, &t);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}

static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                return "unknown";
  }
}

static uint32_t largestFreeBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

static String signalWords() {            // RSSI in plain language for the status page
  if (!WiFi.isConnected()) return "down";
  long r = WiFi.RSSI();
  const char* w = r >= -55 ? "Good" : r >= -67 ? "OK" : r >= -78 ? "Weak" : "Very weak";
  return String(w) + " (" + String(r) + " dBm)";
}

// The stream id for a slot: the configured place/role, or a sane derived default
// so a node that was never fully configured still emits a usable, stable key.
// The stream id is the probe's own factory address, and nothing else.
//
// oat-ods §3/§4: stream.id is the primary key and "defaults to the MAC if the
// grower assigned no name" — the endpoint owns the mapping from hardware id to a
// place, because the endpoint is the system of record for bindings. The reference
// node (BLE Listener) has always done exactly this: stream_id = the sensor's MAC.
//
// This node briefly invented "<device>-t1" instead, which was worse than either
// option: a POSITIONAL label. Pull the first probe off the wire and every probe
// behind it shifts up a slot and inherits an id that used to mean a different
// physical place. Silent history corruption, which is the exact thing the
// stream/physical split exists to prevent.
// oat-ods §3/§4: stream.id is the sensor's own hardware id. The node names nothing;
// the endpoint owns the hardware-to-place mapping, because that is where it survives
// a reflash and can be edited without visiting the node.
static String streamIdFor(int i) {
  if (i >= 0 && i < MAX_SLOTS && slots[i].used && slots[i].id[0]) return String(slots[i].id);
  return cfg.device_id;
}

// ----------------------------------------------------------------------------
// Config field registry — the one place both the web form and the serial Console
// read/write a setting, so the two surfaces can never drift. Secrets only change
// when a value is given, and are never echoed back.
// ----------------------------------------------------------------------------
static bool isSecretKey(const String& k) {
  return k == "wifi_pw" || k == "ep_key" || k == "ep_auth" || k == "mq_pw" || k == "admin_pw";
}

// Gatekeeper for the two pin fields. A wrong pin here is not a typo you discover
// and correct — it is persisted, re-applied at every boot, and some of these pins
// take the board down before the setup page exists to fix it. So the invalid ones
// are refused with the reason, not accepted and regretted. (See the WIRING note at
// the top of this file for the full map.)

// Set one field. Empty value on a secret = leave unchanged. Returns false (with
// msg) on an unknown/invalid key. Caller persists with cfgSave().
static bool applyField(const String& k, const String& v, String& msg) {
  if      (k == "dev_name" || k == "device_name") cfg.device_name = v;
  else if (k == "farm_id")   cfg.farm_id = v;
  else if (k == "location")  cfg.location = v;
  else if (k == "ssid")      cfg.ssid = v;
  else if (k == "wifi_pw")  { if (v.length()) cfg.wifi_pw = v; }
  else if (k == "method")   { if (v == "webhook" || v == "mqtt") cfg.method = v; else { msg = "method must be webhook|mqtt"; return false; } }
  else if (k == "ep_url")    cfg.ep_url = v;
  else if (k == "ep_auth")  { if (v.length()) cfg.ep_auth = v; }
  else if (k == "ep_key")   { if (v.length()) cfg.ep_key = v; }
  else if (k == "mq_host")   cfg.mq_host = v;
  else if (k == "mq_port")   cfg.mq_port = (uint16_t) v.toInt();
  else if (k == "mq_user")   cfg.mq_user = v;
  else if (k == "mq_pw")    { if (v.length()) cfg.mq_pw = v; }
  else if (k == "mq_topic")  cfg.mq_topic = v.length() ? v : String("oat");
  else if (k == "mq_tls")    cfg.mq_tls = (v == "1" || v == "true" || v == "on" || v == "yes");
  else if (k == "interval") { cfg.interval_s = (uint32_t) v.toInt(); if (cfg.interval_s < 10) cfg.interval_s = 10; }
  else if (k == "sample")   { cfg.sample_s = (uint32_t) v.toInt(); if (cfg.sample_s < 2) cfg.sample_s = 2; if (cfg.sample_s > 600) cfg.sample_s = 600; }
  else if (k == "ntp")       cfg.ntp_server = v.length() ? v : String("pool.ntp.org");
  else if (k == "lineout")   cfg.lineout = (v == "1" || v == "true" || v == "on" || v == "yes");
  else if (k == "admin_pw") { if (v.length()) cfg.admin_pw = v; }
  else {
    // Not a core field: offer it to the driver's own table before giving up. This is
    // why a driver never edits the registry, the form, the Console or NVS to add a
    // setting — one table entry reaches all four.
    for (int i = 0; i < g_drv->nFields; i++)
      if (k == g_drv->fields[i].key) {
        String why;
        if (!g_drv->fields[i].set(v, why)) { msg = why; return false; }
        msg = "ok";
        return true;
      }
    msg = "unknown field: " + k;
    return false;
  }
  msg = "ok";
  return true;
}

// Read one field for display. Secrets come back masked, never as the value.
static String readField(const String& k) {
  if (k == "device_id") return cfg.device_id;
  if (isSecretKey(k)) {
    String cur = (k == "wifi_pw") ? cfg.wifi_pw : (k == "ep_key") ? cfg.ep_key :
                 (k == "ep_auth") ? cfg.ep_auth : (k == "mq_pw")  ? cfg.mq_pw  : cfg.admin_pw;
    if (cur.length() == 0) return "(not set)";
    if (k == "ep_key" && cur.length() >= 4) return "(set, ..." + cur.substring(cur.length() - 4) + ")";
    return "(set)";
  }
  if (k == "dev_name" || k == "device_name") return cfg.device_name;
  if (k == "farm_id")   return cfg.farm_id;
  if (k == "location")  return cfg.location;
  if (k == "ssid")      return cfg.ssid;
  if (k == "method")    return cfg.method;
  if (k == "ep_url")    return cfg.ep_url;
  if (k == "mq_host")   return cfg.mq_host;
  if (k == "mq_port")   return String(cfg.mq_port);
  if (k == "mq_user")   return cfg.mq_user;
  if (k == "mq_topic")  return cfg.mq_topic;
  if (k == "mq_tls")    return cfg.mq_tls ? "on" : "off";
  if (k == "interval")  return String(cfg.interval_s);
  if (k == "sample")    return String(cfg.sample_s);
  if (k == "ntp")       return cfg.ntp_server;
  if (k == "lineout")   return cfg.lineout ? "on" : "off";
  for (int i = 0; i < g_drv->nFields; i++)
    if (k == g_drv->fields[i].key) return g_drv->fields[i].get();
  return "(unknown)";
}

// ----------------------------------------------------------------------------
// NVS load / save
// ----------------------------------------------------------------------------
// Preferences logs an ERROR for every key that does not exist yet, so a first boot
// prints two dozen red NOT_FOUND lines before the node has done anything wrong.
// Ask whether the key is there first: same result, no false alarm.
static String prefStr(const char* k, const char* def) { return prefs.isKey(k) ? prefs.getString(k, def) : String(def); }
static float  prefFloat(const char* k, float def)     { return prefs.isKey(k) ? prefs.getFloat(k, def) : def; }

void cfgLoad() {
  prefs.begin(g_drv->nvs, true);
  cfg.device_id   = macFromChip();
  cfg.device_name = prefStr("dev_name", "");
  cfg.farm_id     = prefStr("farm_id",  "");
  cfg.location    = prefStr("location", "");
  cfg.ssid        = prefStr("ssid",     "");
  cfg.wifi_pw     = prefStr("wifi_pw",  "");
  cfg.method      = prefStr("method",   "webhook");
  cfg.ep_url      = prefStr("ep_url",   "");
  cfg.ep_auth     = prefStr("ep_auth",  "");
  cfg.ep_key      = prefStr("ep_key",   "");
  cfg.mq_host     = prefStr("mq_host",  "");
  cfg.mq_port     = prefs.getUShort("mq_port",  1883);
  cfg.mq_user     = prefStr("mq_user",  "");
  cfg.mq_pw       = prefStr("mq_pw",    "");
  cfg.mq_topic    = prefStr("mq_topic", "oat");
  cfg.mq_tls      = prefs.getBool  ("mq_tls",   false);
  cfg.interval_s  = prefs.getULong ("interval", 60);
  cfg.sample_s    = prefs.getULong ("sample",   10);

  cfg.ntp_server  = prefStr("ntp",      "pool.ntp.org");
  cfg.admin_pw    = prefStr("admin_pw", DEFAULT_ADMIN_PW);
  cfg.lineout     = prefs.getBool  ("lineout",  true);
  // A driver's own fields ride the same registry, so a driver never touches NVS and
  // a new field costs one table entry rather than four edits in four places.
  for (int i = 0; i < g_drv->nFields; i++) {
    const Field &f = g_drv->fields[i];
    if (prefs.isKey(f.key)) { String why; f.set(prefs.getString(f.key, ""), why); }
  }
  prefs.end();
  if (cfg.interval_s < 10) cfg.interval_s = 10;
  if (cfg.sample_s < 2)    cfg.sample_s = 2;
}

void cfgSave() {
  prefs.begin(g_drv->nvs, false);
  prefs.putString("dev_name", cfg.device_name);
  prefs.putString("farm_id",  cfg.farm_id);
  prefs.putString("location", cfg.location);
  prefs.putString("ssid",     cfg.ssid);
  prefs.putString("wifi_pw",  cfg.wifi_pw);
  prefs.putString("method",   cfg.method);
  prefs.putString("ep_url",   cfg.ep_url);
  prefs.putString("ep_auth",  cfg.ep_auth);
  prefs.putString("ep_key",   cfg.ep_key);
  prefs.putString("mq_host",  cfg.mq_host);
  prefs.putUShort("mq_port",  cfg.mq_port);
  prefs.putString("mq_user",  cfg.mq_user);
  prefs.putString("mq_pw",    cfg.mq_pw);
  prefs.putString("mq_topic", cfg.mq_topic);
  prefs.putBool  ("mq_tls",   cfg.mq_tls);
  prefs.putULong ("interval", cfg.interval_s);
  prefs.putULong ("sample",   cfg.sample_s);

  prefs.putString("ntp",      cfg.ntp_server);
  prefs.putString("admin_pw", cfg.admin_pw);
  prefs.putBool  ("lineout",  cfg.lineout);
  for (int i = 0; i < g_drv->nFields; i++) prefs.putString(g_drv->fields[i].key, g_drv->fields[i].get());
  prefs.end();
}

void factoryReset() {
  prefs.begin(g_drv->nvs, false);
  prefs.clear();
  prefs.end();
}

// ----------------------------------------------------------------------------
// Delivery: transport choice (https-preferred, signed-http fallback) + mqtt
// ----------------------------------------------------------------------------
static oat::Device oatDevice() {
  oat::Device d;
  d.gateway_id = cfg.device_name.length() ? cfg.device_name.c_str() : cfg.device_id.c_str();
  d.farm_id    = cfg.farm_id.c_str();     // the operator: one grower, many nodes
  d.device_id  = cfg.device_id.c_str();   // hardware provenance (efuse MAC) — rides the status message only
  d.tier       = g_drv->tier;
  d.fw         = g_drv->fw_version;
  return d;
}

struct OutItem {
  char stream[40]; char physical[32];        // physical must hold a full 64-bit address
  char name[32];                            // the device's own label, when it has one
  char brand[20]; char model[24];            // whose part this is, per slot, from the driver
  int  rssi; int battery_pct;
  uint32_t window_s;                        // the window actually covered, not the setting
  uint8_t nMeas;
  struct { char measurement[20]; char unit[12]; uint8_t kind; double value; uint32_t samples; } m[MEAS_PER_SLOT];
};

// Add the OAT signing headers (no-op when no key set).
static void addSignHeaders(HTTPClient& http, const String& ts, const String& body) {
  if (cfg.ep_key.length() == 0) return;
  String sig = oat::hmacSha256Hex(cfg.ep_key, oat::signString(ts, body));
  http.addHeader("X-OAT-Key-Id",    cfg.device_id);
  http.addHeader("X-OAT-Timestamp", ts);
  http.addHeader("X-OAT-Signature", "oat1=" + sig);
}

// POST one body to a concrete URL with the configured headers. Returns the
// HTTPClient code: >=200 = server replied; <0 = transport/TLS failure.
static int postTo(const String& url, const String& body) {
  bool https = url.startsWith("https");
  HTTPClient http;
  bool began = https ? (netTls.setInsecure(), http.begin(netTls, url)) : http.begin(netPlain, url);
  if (!began) return HTTPC_ERROR_CONNECTION_REFUSED;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", g_drv->fw_version);
  if (cfg.ep_auth.length()) http.addHeader("Authorization", cfg.ep_auth);
  String ts = String((uint32_t)time(nullptr));
  addSignHeaders(http, ts, body);
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  http.end();
  return code;
}

// The push engine. Prefer https; if the TLS handshake won't fit the heap, or it
// fails at the transport layer, fall back to SIGNED http (safe — the HMAC proves
// it) for this push and re-probe https later. A 4xx/5xx is a *reply*, not a TLS
// failure, so we never downgrade on one.
bool pushWebhook(const String &payload) {
  if (cfg.ep_url.length() == 0) { lastPushMsg = "no webhook URL set"; return false; }
  bool wantHttps = cfg.ep_url.startsWith("https");
  String httpUrl = cfg.ep_url; if (httpUrl.startsWith("https")) httpUrl = "http" + httpUrl.substring(5);

  // Periodically clear the fallback flag so a recovered heap lets https return.
  if (tlsFailed && millis() - lastTlsProbe > TLS_REPROBE_MS) tlsFailed = false;

  bool canTls = wantHttps && !tlsFailed && largestFreeBlock() >= TLS_MIN_HEAP;
  int code;
  if (canTls) {
    lastTlsProbe = millis();
    code = postTo(cfg.ep_url, payload);
    if (code < 0) {                                   // TLS/transport failure -> fall back, loudly
      tlsFailed = true;
      code = postTo(httpUrl, payload);
      lastTransport = "http";
      lastPushMsg = "TLS failed (low heap?) — sent signed http; HTTP " + String(code);
    } else {
      lastTransport = "https";
      lastPushMsg = "HTTP " + String(code) + " (https)";
    }
  } else {
    code = postTo(wantHttps ? httpUrl : cfg.ep_url, payload);
    lastTransport = "http";
    lastPushMsg = wantHttps ? ("signed http (TLS deferred); HTTP " + String(code))
                            : ("HTTP " + String(code));
  }
  return code >= 200 && code < 300;
}

bool ensureMqtt() {
  if (cfg.mq_host.length() == 0) { lastPushMsg = "no MQTT host set"; return false; }
  mqtt.setClient(cfg.mq_tls ? (Client&)netTls : (Client&)netPlain);
  if (cfg.mq_tls) netTls.setInsecure();
  mqtt.setServer(cfg.mq_host.c_str(), cfg.mq_port);
  mqtt.setBufferSize(2048);
  if (mqtt.connected()) return true;

  oat::Device dev = oatDevice();
  String willTopic = oat::statusTopic(cfg.mq_topic.c_str(), dev);
  oat::Health hw;
  String willMsg; oat::encodeStatus(dev, "offline", "", hw, willMsg);
  String cid = cfg.device_id + "-" + String((uint32_t)millis(), HEX);
  bool ok = cfg.mq_user.length()
            ? mqtt.connect(cid.c_str(), cfg.mq_user.c_str(), cfg.mq_pw.c_str(),
                           willTopic.c_str(), 0, true, willMsg.c_str())
            : mqtt.connect(cid.c_str(), willTopic.c_str(), 0, true, willMsg.c_str());
  if (ok) {
    oat::Health hh;
    hh.uptime_s   = (uint32_t)(millis() / 1000);
    hh.free_heap  = ESP.getFreeHeap();
    hh.boot_count = bootCount;
    hh.reset      = resetReasonStr();
    hh.rssi       = WiFi.RSSI();
    String _lan = WiFi.localIP().toString(), _ssid = WiFi.SSID();
    hh.lan_ip = _lan.c_str(); hh.ssid = _ssid.c_str();
    String onlineMsg; oat::encodeStatus(dev, "online", isoNowUTC().c_str(), hh, onlineMsg);
    mqtt.publish(willTopic.c_str(), onlineMsg.c_str(), true);
  } else {
    lastPushMsg = "MQTT connect rc=" + String(mqtt.state());
  }
  return ok;
}

bool mqttPublish(const String &topic, const String &payload) {
  if (!ensureMqtt()) return false;
  bool ok = mqtt.publish(topic.c_str(), payload.c_str(), true);
  lastPushMsg = ok ? ("published " + topic) : "mqtt publish failed";
  return ok;
}

bool sendMessage(const oat::Device &dev, const oat::Reading &r) {
  String payload; oat::encodeNative(dev, r, payload);
  if (cfg.method == "mqtt") return mqttPublish(oat::mqttTopic(cfg.mq_topic.c_str(), dev, r), payload);
  return pushWebhook(payload);
}

// Measurements per webhook POST — bounded so the JSON body + ArduinoJson DOM + the
// TLS handshake block coexist in heap. This node's batch is small (four
// measurements at most), so the cap never bites; it is kept identical to the
// reference node so the push engine stays one shared, proven piece of code.
static uint32_t batchChunkSize() {
  if (ESP.getPsramSize() > 0) return 200;
  uint32_t largest = largestFreeBlock();
  if (largest < 24000) return 4;
  uint32_t n = (largest - 20000) / 1000;
  return n < 4 ? 4 : (n > 20 ? 20 : n);
}

void pushAll() {
  String iso = isoNowUTC();

  // 1) snapshot the window under the lock; fold each measurand to a single value
  //    by its kind (mean for temperature and humidity); reset the window; release.
  static OutItem items[MAX_SLOTS];
  int nItems = 0;
  xSemaphoreTake(sensorMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_SLOTS; i++) {
    Slot &s = slots[i];
    if (!s.used || s.samples == 0) continue;
    OutItem &it = items[nItems++];
    memset(&it, 0, sizeof(it));
    strncpy(it.stream,   streamIdFor(i).c_str(), sizeof(it.stream) - 1);
    strncpy(it.physical, s.physical,             sizeof(it.physical) - 1);
    strncpy(it.name,     s.name,                 sizeof(it.name) - 1);
    strncpy(it.brand,    s.brand,                sizeof(it.brand) - 1);
    strncpy(it.model,    s.model,                sizeof(it.model) - 1);
    it.rssi        = s.rssi;
    it.battery_pct = s.battery_pct;
    // Report the window this mean actually covers. Sampling continues while the
    // network is down, so after an outage one push can carry hours of readings —
    // labelling that "60 s" because 60 is the setting would be a quiet lie.
    it.window_s = (uint32_t)((millis() - s.windowStartMs + 500UL) / 1000UL);
    if (it.window_s == 0) it.window_s = 1;
    it.nMeas = 0;
    for (int j = 0; j < MEAS_PER_SLOT; j++) {
      MeasAcc &m = s.meas[j];
      if (!m.used || m.n == 0) continue;
      double v;
      if      (m.kind == oat::KIND_CONTINUOUS) v = m.sum / m.n;   // mean
      else if (m.kind == oat::KIND_EVENT)      v = (double)m.n;
      else                                     v = m.last;
      auto &om = it.m[it.nMeas++];
      strncpy(om.measurement, m.measurement, sizeof(om.measurement) - 1);
      strncpy(om.unit,        m.unit,        sizeof(om.unit) - 1);
      om.kind    = m.kind;
      om.value   = round(v * 100.0) / 100.0;
      om.samples = m.n;
      m.sum = 0; m.n = 0;                    // reset window; keep last + identity for gauges
    }
    s.samples = 0;
  }
  xSemaphoreGive(sensorMutex);

  if (nItems == 0) {
    lastPushMsg = "no sensor data to send"; lastPushOk = false; lastPushMs = millis(); return;
  }

  // 1b) the oat-line output: every reading this push carries, one text line each,
  //     on the USB serial port. This is what makes ANY core sketch a POD — cable
  //     its serial port to a LoRa Field Node (or another gateway) and the readings
  //     ride on with the same stream ids, no Wi-Fi needed here. Grammar:
  //       S <stream> brand=<..> model=<..>       provenance (sticky)
  //       L <stream> rssi=<dBm> battery=<pct>    link health (latest wins)
  //       M <stream> <measurement> <value> <unit|-> <c|g|k|s|e>
  //     Written before the network push so a pod with no Wi-Fi still emits.
  if (cfg.lineout) {
    static const char KINDCH[] = { 'c', 'g', 'k', 's', 'e' };
    for (int i = 0; i < nItems; i++) {
      OutItem &it = items[i];
      if (it.brand[0] || it.model[0]) Serial.printf("S %s brand=%s model=%s\n", it.stream, it.brand[0] ? it.brand : "-", it.model[0] ? it.model : "-");
      if (it.rssi != 0 || it.battery_pct >= 0) {
        Serial.printf("L %s", it.stream);
        if (it.rssi != 0) Serial.printf(" rssi=%d", it.rssi);
        if (it.battery_pct >= 0) Serial.printf(" battery=%d", it.battery_pct);
        Serial.print("\n");
      }
      for (int j = 0; j < it.nMeas; j++) {
        auto &om = it.m[j];
        Serial.printf("M %s %s %.4g %s %c\n", it.stream, om.measurement, om.value, om.unit[0] ? om.unit : "-", KINDCH[om.kind < 5 ? om.kind : 0]);
      }
    }
  }

  // 2) emit OUTSIDE the lock. The node REPORTS raw observations; the endpoint
  //    derives dewpoint / VPD from temperature + humidity.
  oat::Device dev = oatDevice();
  pushCount++;
  bool webhook = (cfg.method != "mqtt");
  uint32_t chunkMax = batchChunkSize();

  JsonDocument batch; JsonArray msgs;
  uint32_t sent = 0, chunks = 0, inChunk = 0;
  bool haveBatch = false, anyOk = false, allOk = true;

  // serialize + send the current batch, guarding against a truncated (unsigned-safe) body.
  auto flushBatch = [&]() {
    if (!haveBatch || inChunk == 0) return;
    size_t want = measureJson(batch);
    String body; body.reserve(want + 1);
    size_t got = serializeJson(batch, body);
    bool ok;
    if (want == 0 || got != want || body.length() != want) {
      lastPushMsg = "batch serialize short (low heap) — dropped"; ok = false;  // never sign a truncated body
    } else {
      ok = pushWebhook(body);
    }
    anyOk = anyOk || ok; allOk = allOk && ok; chunks++;
    batch.clear(); haveBatch = false; inChunk = 0;
  };

  for (int i = 0; i < nItems && sent < MAX_BATCH_MEAS; i++) {
    OutItem &it = items[i];
    oat::Reading r;
    r.stream_id   = it.stream;
    // stream.name carries a label the DEVICE supplied about itself, and nothing else.
    // The node never invents one: when a driver has no device-supplied name, this
    // field is absent rather than echoing the id back under a second key.
    if (it.name[0]) r.stream_name = it.name;
    r.location    = cfg.location.c_str();
    r.physical_id = it.physical;
    if (it.brand[0]) r.brand = it.brand;    // whatever the driver said this part is,
    if (it.model[0]) r.model = it.model;    // and nothing at all if it said nothing
    r.battery_pct = it.battery_pct;
    r.rssi        = it.rssi;
    r.window_s    = it.window_s;
    r.observed_at = iso.c_str();

    for (int j = 0; j < it.nMeas && sent < MAX_BATCH_MEAS; j++) {
      auto& om = it.m[j];
      const char* unit = om.unit[0] ? om.unit : "";
      const char* agg  = oat::kindAgg(om.kind);
      r.samples = om.samples; r.agg_method = agg;
      r.measurement = om.measurement; r.unit = unit; r.value = om.value;
      if (webhook) {
        if (!haveBatch) { msgs = oat::beginBatch(batch, dev, iso.c_str(), ++g_seq); haveBatch = true; inChunk = 0; }
        oat::addReading(msgs, r);
        sent++; inChunk++;
        if (inChunk >= chunkMax) flushBatch();
      } else {
        if (sendMessage(dev, r)) { anyOk = true; sent++; }
      }
    }
  }
  if (webhook) flushBatch();

  lastPushOk = webhook ? (chunks > 0 && allOk) : anyOk;
  if (lastPushOk) pushOk++; else pushFail++;
  lastPushMs = millis();
  lastPushEpoch = time(nullptr);
  Serial.printf("[push] method=%s measurements=%u chunks=%u ok=%d msg=%s\n",
                cfg.method.c_str(), sent, chunks, lastPushOk, lastPushMsg.c_str());
}

// Health beacon — the node self-reports the failures we used to read off a log.
// Sent BEFORE the sensor push every cycle: a tiny liveness packet must reach the
// endpoint even when the sensor is dead or a big batch is failing on a weak link.
void pushBeacon() {
  oat::Device dev = oatDevice();
  oat::Health h;
  h.uptime_s        = (uint32_t)(millis() / 1000);
  h.free_heap       = ESP.getFreeHeap();
  h.min_free_heap   = ESP.getMinFreeHeap();
  h.largest_block   = largestFreeBlock();
  h.boot_count      = bootCount;
  h.reset           = resetReasonStr();
  h.rssi            = WiFi.isConnected() ? WiFi.RSSI() : oat::NO_RSSI;
  h.wifi_reconnects = wifiReconnects;
  h.push_ok         = pushOk;
  h.push_fail       = pushFail;
  h.transport       = lastTransport.length() ? lastTransport.c_str() : "";
  h.tls_ok          = (lastTransport == "https") ? 1 : (tlsFailed ? 0 : -1);
  h.chip            = ESP.getChipModel();
  float _tc         = temperatureRead();      // internal die temp (uncalibrated on classic ESP32)
  h.temp_c          = isnan(_tc) ? -1000.0f : _tc;
  h.loops_per_sec   = g_loopsPerSec;
  // Where-to-find-me: the first question in every field support call. Strings
  // stay in scope through encode.
  String _lan  = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  String _ssid = WiFi.isConnected() ? WiFi.SSID() : String("");
  String _mdns = g_mdnsHost.length() ? g_mdnsHost + ".local" : String("");
  h.lan_ip      = _lan.c_str();
  h.ssid        = _ssid.c_str();
  h.mdns        = _mdns.c_str();
  String msg; oat::encodeStatus(dev, "online", isoNowUTC().c_str(), h, msg);
  if (cfg.method == "mqtt") {
    if (ensureMqtt()) mqtt.publish(oat::statusTopic(cfg.mq_topic.c_str(), dev).c_str(), msg.c_str(), true);
  } else {
    pushWebhook(msg);
  }
  lastBeaconMs = millis();
}

// ----------------------------------------------------------------------------
// Web UI
// ----------------------------------------------------------------------------
static const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>OAT Node Setup</title><style>"
  "body{font-family:system-ui,Arial,sans-serif;background:#0f1413;color:#e7efe9;margin:0;padding:1rem;}"
  ".card{max-width:640px;margin:0 auto;background:#172120;border:1px solid #2a3a37;border-radius:10px;padding:1.2rem;}"
  "h1{font-size:1.2rem;margin:.2rem 0 1rem;color:#bfe3c8;}h2{font-size:.95rem;color:#8fb39a;margin:1.2rem 0 .4rem;border-bottom:1px solid #2a3a37;padding-bottom:.2rem;}"
  "label{display:block;font-size:.8rem;margin:.6rem 0 .15rem;color:#9fb8a6;}"
  "input,select{width:100%;box-sizing:border-box;padding:.5rem;border-radius:6px;border:1px solid #344743;background:#0f1413;color:#e7efe9;font-size:.9rem;}"
  ".row{display:flex;gap:.6rem;}.row>div{flex:1;}"
  ".btn{margin-top:1.2rem;width:100%;padding:.7rem;background:#2e7d54;color:#fff;border:0;border-radius:8px;font-size:1rem;cursor:pointer;}"
  ".muted{font-size:.72rem;color:#6f8a78;margin-top:.15rem;}a{color:#7fd0a0;}"
  ".brand{display:flex;align-items:center;gap:.5rem;margin-bottom:.7rem;}"
  ".bname{font-weight:600;font-size:.95rem;color:#bfe3c8;letter-spacing:.2px;}"
  ".ok{color:#7fd0a0;}.bad{color:#e58f8f;}.pill{display:inline-block;font-size:.7rem;padding:.1rem .4rem;border:1px solid #344743;border-radius:99px;margin-right:.3rem;}"
  ".big{font-size:1.6rem;color:#bfe3c8;font-weight:600;}"
  ".show{font-size:.7rem;color:#7fd0a0;cursor:pointer;float:right;margin-top:-1.4rem;margin-right:.4rem;position:relative;}"
  "</style></head><body><div class='card'>"
  "<div class='brand'>"
#ifdef LOGO_URL
  "<img src='" LOGO_URL "' height='28' alt='' style='display:block' onerror='this.remove()'>"
#endif
  "<span class='bname'>OpenAgricultureTechnology<span style='color:#6f8a78'>.com</span></span>"
  "</div>";

static const char PAGE_FOOT[] PROGMEM = "</div></body></html>";

// show/hide toggle for password fields
static const char JS_SHOWPW[] PROGMEM =
  "<script>function sp(id,el){var f=document.getElementById(id);"
  "if(f.type=='password'){f.type='text';el.textContent='hide';}else{f.type='password';el.textContent='show';}}</script>";

String fieldsForm() {
  String webhookSel = (cfg.method == "webhook") ? "selected" : "";
  String mqttSel    = (cfg.method == "mqtt")    ? "selected" : "";
  String tlsChk     = cfg.mq_tls ? "checked" : "";

  String h;
  h += "<h1>OAT Node Setup</h1>";
  if (WiFi.isConnected())
    h += "<div class='muted'>This device: <b class='ok'>" + cfg.device_id + "</b> &middot; " + localUrls() +
         " &middot; signal " + signalWords() + "</div>";
  h += "<form method='POST' action='/save'>";

  h += "<h2>Name this node first</h2>";
  h += "<label>Device name <span class='muted'>(becomes its address: oat-&lt;name&gt;.local)</span></label>";
  h += "<input name='dev_name' value='" + cfg.device_name + "' placeholder='GH2-North'>";
  h += "<div class='row'><div><label>Farm / user ID</label><input name='farm_id' value='" + cfg.farm_id + "'></div>";
  h += "<div><label>Location</label><input name='location' value='" + cfg.location + "'></div></div>";
  h += "<label>Device ID</label><input value='" + cfg.device_id + "' readonly style='opacity:.6'>";

  h += "<h2>WiFi</h2>";
  h += "<label>Network</label><input name='ssid' list='ssids' value='" + cfg.ssid + "' placeholder='pick or type'>";
  h += "<datalist id='ssids'>" + g_ssidOptions + "</datalist>";
  h += "<div class='muted'><a href='/rescan'>rescan networks</a></div>";
  h += "<label>Password</label><span class='show' onclick=\"sp('wpw',this)\">show</span>";
  h += "<input id='wpw' name='wifi_pw' type='password' placeholder='(unchanged)'>";

  h += "<h2>Delivery</h2>";
  h += "<label>Method</label><select name='method' id='method' onchange='tog()'>";
  h += "<option value='webhook' " + webhookSel + ">Webhook (HTTP POST)</option>";
  h += "<option value='mqtt' " + mqttSel + ">MQTT</option></select>";

  h += "<div id='wh'>";
  h += "<label>Webhook URL</label><input name='ep_url' value='" + cfg.ep_url + "' placeholder='https://...'>";
  h += "<div class='muted'>https is preferred; if this board is low on memory for TLS it auto-uses signed http (your data is signed either way).</div>";
  h += "<label>Endpoint key (optional)</label><span class='show' onclick=\"sp('epk',this)\">show</span>";
  h += "<input id='epk' name='ep_key' type='password' placeholder='" + String(cfg.ep_key.length() ? "(unchanged)" : "paste the key your endpoint gave you") + "'>";
  h += "<div class='muted'>Signs every push; the key is never sent, so it stays safe even over plain http. Blank = open/sandbox.</div>";
  h += "<label>Authorization header (optional)</label><input name='ep_auth' placeholder='" + String(cfg.ep_auth.length() ? "(unchanged)" : "Bearer abc123") + "'>";
  h += "</div>";

  h += "<div id='mq'>";
  h += "<div class='row'><div><label>MQTT host</label><input name='mq_host' value='" + cfg.mq_host + "'></div>";
  h += "<div style='flex:.4'><label>Port</label><input name='mq_port' value='" + String(cfg.mq_port) + "'></div></div>";
  h += "<label>Topic prefix</label><input name='mq_topic' value='" + cfg.mq_topic + "'>";
  h += "<div class='row'><div><label>Username (optional)</label><input name='mq_user' value='" + cfg.mq_user + "'></div>";
  h += "<div><label>Password</label><input name='mq_pw' type='password' placeholder='(unchanged)'></div></div>";
  h += "<label><input type='checkbox' name='mq_tls' " + tlsChk + " style='width:auto'> Use TLS</label>";
  h += "</div>";

  h += "<h2>The sensors</h2>";
  for (int i = 0; i < g_drv->nFields; i++) {
    const Field &f = g_drv->fields[i];
    h += "<label>" + String(f.label) + "</label><input name='" + String(f.key) + "' value='" + f.get() + "'>";
    if (f.help && f.help[0]) h += "<div class='muted'>" + String(f.help) + "</div>";
  }
  if (slotCount() == 0) {
    h += "<div class='muted'>Nothing is answering yet.";
    if (g_drv->rescan) h += " <a href='/rescansensors'>Look again</a>.";
    h += "</div>";
  } else {
    h += "<div class='muted'>Each sensor reports under its own hardware id, and that is what gets pushed. There is nothing to name here: naming a sensor after a place is the endpoint's job, because that is where the mapping survives a reflash and can be edited without visiting the node.</div>";
    if (g_drv->statusHtml) h += g_drv->statusHtml();
    if (g_drv->rescan) h += "<div class='muted'><a href='/rescansensors'>Look again</a> after adding a sensor.</div>";
  }

  h += "<h2>Sampling</h2>";
  h += "<div class='row'><div><label>Push interval (sec)</label><input name='interval' value='" + String(cfg.interval_s) + "'></div>";
  h += "<div><label>Read the sensor every (sec)</label><input name='sample' value='" + String(cfg.sample_s) + "'></div></div>";
  h += "<div class='muted'>The node averages every reading it takes inside a push window, so a short read interval means a steadier number.</div>";

  h += "<h2>Advanced</h2>";
  h += "<label>NTP server</label><input name='ntp' value='" + cfg.ntp_server + "'>";
  h += "<label>Admin password</label><input name='admin_pw' type='password' placeholder='(unchanged)'>";
  h += "<div class='muted'>Default is <code>oatsetup</code>. Change it once the node is online.</div>";

  // The label should describe what the button will do. On a node already on the
  // network, this saves settings and nothing else; promising to connect implies a
  // disruption that no longer happens.
  h += String("<button class='btn' type='submit'>") +
       (WiFi.isConnected() ? "Save" : "Save &amp; Connect") + "</button></form>";
  h += "<p class='muted' style='text-align:center'><a href='/status'>Live status &amp; test push &rarr;</a></p>";
  h += "<script>function tog(){var m=document.getElementById('method').value;"
       "document.getElementById('wh').style.display=(m=='webhook')?'block':'none';"
       "document.getElementById('mq').style.display=(m=='mqtt')?'block':'none';}tog();</script>";
  h += FPSTR(JS_SHOWPW);
  return h;
}

bool requireAuth() {
  if (!server.authenticate(ADMIN_USER, cfg.admin_pw.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void handleRoot() {
  if (!requireAuth()) return;
  String p = FPSTR(PAGE_HEAD); p += fieldsForm(); p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
}

// Connect-then-confirm: apply, save, join the new WiFi with the AP still up,
// then show the LAN IP + name.local so the user is never stranded.
void handleSave() {
  if (!requireAuth()) return;
  String m;
  const char* keys[] = {"dev_name","farm_id","location","ssid","wifi_pw","method","ep_url",
                        "ep_auth","ep_key","mq_host","mq_port","mq_user","mq_pw","mq_topic",
                        "interval","sample","ntp","admin_pw"};
  String beforeDriver;
  for (int i = 0; i < g_drv->nFields; i++) beforeDriver += g_drv->fields[i].get() + "\x1f";
  const String oldSsid = cfg.ssid;
  String rejected;
  for (auto k : keys)
    if (server.hasArg(k) && !applyField(k, server.arg(k), m)) {
      if (rejected.length()) rejected += "<br>";
      rejected += m;                       // tell them WHY, don't silently drop the field
    }
  cfg.mq_tls = server.hasArg("mq_tls");
  cfgSave();
  configTime(0, 0, cfg.ntp_server.c_str());
  // Re-scan if the pin moved or the resolution changed — no reboot needed to fix a
  // wiring typo, and a resolution change has to reach every probe on the wire.
  // A driver field changed (a pin moved, a resolution changed): let the driver
  // re-open its bus. No reboot needed to fix a wiring typo.
  for (int i = 0; i < g_drv->nFields; i++) {
    const Field &f = g_drv->fields[i];
    if (!server.hasArg(f.key)) continue;
    String why;
    if (!f.set(server.arg(f.key), why)) { if (rejected.length()) rejected += "<br>"; rejected += why; }
  }
  String afterDriver;
  for (int i = 0; i < g_drv->nFields; i++) afterDriver += g_drv->fields[i].get() + "\x1f";
  if (afterDriver != beforeDriver && g_drv->rescan) g_drv->rescan();

  // Join ONLY if the radio's settings actually changed, or we are not on the
  // network yet.
  //
  // Re-joining unconditionally was written for first setup, where the browser is on
  // the setup AP and connecting IS the point. For every later edit it is pure harm:
  // someone editing a sensor offset from the LAN had their station connection torn
  // down underneath the page they were reading, which killed the socket carrying the
  // confirmation ("write(): fail on fd 49, errno 113"), dropped any push in flight,
  // and cost twelve seconds for a reconnect nobody asked for. Worse, the page that
  // never arrived is the one that says which fields were REJECTED — so a refused pin
  // looked like a save that worked.
  bool wifiTouched = (cfg.ssid != oldSsid) ||
                     (server.hasArg("wifi_pw") && server.arg("wifi_pw").length() > 0);
  bool connected   = WiFi.isConnected();
  bool justJoined  = false;
  if (cfg.ssid.length() && (wifiTouched || !connected)) {
    justJoined = true;
    staBegin();
    unsigned long t0 = millis();
    connected = false;
    while (millis() - t0 < 12000) { if (WiFi.status() == WL_CONNECTED) { connected = true; break; } delay(200); }
    if (connected) { startMdns(); apDropAtMs = millis() + AP_GRACE_MS; apSticky = false; }
  }

  String p = FPSTR(PAGE_HEAD);
  if (rejected.length())
    p += "<p class='bad'>Some settings were not applied:<br>" + rejected + "</p>";
  if (connected) {
    String ip = WiFi.localIP().toString();
    // Say which of the two things happened. A node that was already on the network
    // and had a sensor offset edited did not "join" anything, and claiming it did is
    // the same fault as a diagnostic reporting a conclusion it never measured — it
    // just happens to be a friendly-sounding one.
    if (wifiTouched || justJoined) {
      p += "<h1 class='ok'>Connected &#10003;</h1>";
      p += "<p>Your device joined <b>" + cfg.ssid + "</b>. It's now reachable on your network at:</p>";
    } else {
      p += "<h1 class='ok'>Saved &#10003;</h1>";
      p += "<p>Settings updated. The device stayed on <b>" + cfg.ssid + "</b> throughout, and is still at:</p>";
    }
    p += "<p style='font-size:1.05rem'><a href='http://" + ip + "' target='_blank'>http://" + ip + "</a>";
    if (g_mdnsHost.length()) p += "<br><a href='http://" + g_mdnsHost + ".local' target='_blank'>http://" + g_mdnsHost + ".local</a>";
    if (wifiTouched || justJoined)
      p += "</p><p class='muted'>Bookmark one of those. The setup WiFi will switch off shortly.</p>";
    else
      p += "</p>";
    p += "<p><a class='btn' style='display:block;text-align:center;text-decoration:none' href='/status'>" +
         String(wifiTouched || justJoined ? "Finish setup here &rarr;" : "See it working &rarr;") + "</a></p>";
    p += "<p class='muted' style='text-align:center'>or switch your phone back to your home WiFi and open the address above.</p>";
  } else if (cfg.ssid.length()) {
    p += "<h1 class='bad'>Couldn't join " + cfg.ssid + "</h1>";
    p += "<p>Settings were saved, but the device couldn't connect — most often a wrong WiFi password.</p>";
    p += "<p><a class='btn' style='display:block;text-align:center;text-decoration:none' href='/'>&larr; Check the password</a></p>";
  } else {
    p += "<h1>Saved</h1><p>No WiFi network set yet.</p><p><a href='/'>&larr; Back to setup</a></p>";
  }
  p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
}

void handleStatus() {
  if (!requireAuth()) return;
  String p = FPSTR(PAGE_HEAD);
  p += "<h1>Live Status</h1>";
  p += "<div class='muted'>ID " + cfg.device_id + " &middot; fw " + g_drv->fw_version + " &middot; chip " + String(ESP.getChipModel()) + "</div>";
  p += "<div class='muted'>WiFi: " + String(WiFi.isConnected() ? "connected " : "down ") + localUrls() + " &middot; signal " + signalWords() + "</div>";
  p += "<div class='muted'>Uptime " + String(millis() / 1000) + "s &middot; boots " + String(bootCount) +
       " &middot; reset " + String(resetReasonStr()) + "</div>";
  p += "<div class='muted'>Heap: free " + String(ESP.getFreeHeap()) + " &middot; min " + String(ESP.getMinFreeHeap()) +
       " &middot; largest block " + String(largestFreeBlock()) + "</div>";
  p += "<div class='muted'>Push: " + String(lastPushOk ? "OK" : "FAIL") + " &middot; " + lastPushMsg +
       " &middot; ok " + String(pushOk) + " / fail " + String(pushFail) +
       " &middot; transport " + (lastTransport.length() ? lastTransport : "&mdash;") + "</div>";

  p += "<h2>" + String(g_drv->what) + "</h2>";
  if (slotCount() == 0) {
    p += "<p class='bad'>Nothing is answering.</p>";
    if (g_drv->diag) p += "<p class='bad'>" + g_drv->diag() + "</p>";
  } else if (g_drv->statusHtml) {
    p += g_drv->statusHtml();
  }
  p += "<div class='muted'>Last read: " + lastReadMsg + "</div>";
  if (g_busResets) p += "<div class='muted'>Bus re-opens since boot: " + String(g_busResets) + "</div>";

  p += "<p style='text-align:center'><a href='/readnow'>Read now</a>";
  if (g_drv->diagFull) p += " &middot; <a href='/diag'>Diagnose the bus</a>";
  if (g_drv->rescan)   p += " &middot; <a href='/rescansensors'>Look again</a>";
  p += " &middot; <a href='/pushnow'>Send a test reading</a> &middot; <a href='/'>&larr; Setup</a></p>";
  p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
}

void handleReadNow() {
  if (!requireAuth()) return;
  driverSampleBlocking();
  server.sendHeader("Location", "/status");
  server.send(303);
}

void handleDiag() {
  if (!requireAuth()) return;
  String p = FPSTR(PAGE_HEAD);
  p += "<h1>Bus diagnosis</h1>";
  p += "<pre class='mono'>" + (g_drv->diagFull ? g_drv->diagFull() : String("this driver offers no diagnosis")) + "</pre>";
  p += "<p style='text-align:center'><a href='/status'>&larr; Status</a></p>";
  p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
}

void handlePushNow() {
  if (!requireAuth()) return;
  if (slotCount() && slots[0].samples == 0) driverSampleBlocking();  // don't make them wait a whole window
  pushAll();
  String p = FPSTR(PAGE_HEAD);
  // Plain-language result — not "HTTP 200".
  if (lastPushOk)             p += "<h1 class='ok'>&#10003; Your endpoint received the reading</h1>";
  else if (lastPushMsg.indexOf("400") >= 0) p += "<h1 class='bad'>Endpoint rejected the reading</h1>";
  else if (lastPushMsg.indexOf("no sensor") >= 0) p += "<h1>No sensor reading yet</h1><p>Check the wiring on the status page, then try again.</p>";
  else                        p += "<h1 class='bad'>Couldn't reach the endpoint</h1>";
  p += "<div class='muted'>" + lastPushMsg + " &middot; transport " + (lastTransport.length() ? lastTransport : "&mdash;") + "</div>";
  p += "<p style='text-align:center'><a href='/status'>&larr; Status</a></p>";
  p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
}

void handleRescanSensors() {
  if (!requireAuth()) return;
  if (g_drv->rescan) g_drv->rescan();
  server.sendHeader("Location", "/status");
  server.send(303);
}

void handleRescan() {
  if (!requireAuth()) return;
  scanWifi();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleReset() {
  if (!requireAuth()) return;
  if (server.arg("confirm") != "1") {
    String p = FPSTR(PAGE_HEAD);
    p += "<h1>Factory reset?</h1><p>This erases WiFi, endpoint, and all settings.</p>";
    p += "<p><a class='btn' style='display:block;text-align:center;text-decoration:none' href='/reset?confirm=1'>Yes, erase everything</a></p>";
    p += "<p style='text-align:center'><a href='/'>Cancel</a></p>" ;
    p += FPSTR(PAGE_FOOT);
    server.send(200, "text/html", p); return;
  }
  String p = FPSTR(PAGE_HEAD); p += "<h1>Erased</h1><p>Rebooting to first-run setup&hellip;</p>"; p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
  factoryReset(); delay(800); ESP.restart();
}

void handlePortal() {                    // captive-portal probe endpoints -> bounce to setup
  server.sendHeader("Location", String("http://") + AP_IP.toString() + "/");
  server.send(302, "text/plain", "");
}
void handleNotFound() { handlePortal(); }

// ----------------------------------------------------------------------------
// Serial Console — the USB log box is two-way. Every setting is reachable here.
// Physical USB = trusted; secrets are writable but never echoed.
// ----------------------------------------------------------------------------
void printReadyBanner() {
  if (WiFi.isConnected())
    Serial.printf("[OAT] Ready -> %s   |   type 'help' for commands\n", localUrlsPlain().c_str());
  else if (apActive)
    Serial.printf("[OAT] setup AP: %s -> http://%s   |   type 'help' for commands\n",
                  apSsid().c_str(), AP_IP.toString().c_str());
  else
    Serial.printf("[OAT] connecting to %s ...   |   type 'help' for commands\n", cfg.ssid.c_str());
}

void consoleHelp() {
  Serial.println(F(
    "OAT Console commands:\n"
    "  help                 this list\n"
    "  status               IP, signal, uptime, heap, push, sensor\n"
    "  ip                   the device address(es)\n"
    "  heap                 memory diagnostics\n"
    "  read                 take a reading now and show it\n"
    "  rescan               run this sketch's discovery again (a part wired since boot)\n"
    "  scan                 list nearby WiFi networks\n"
    "  wifi <ssid> <pass>   set WiFi and reconnect (SSID has spaces? use: set ssid <name>)\n"
    "  endpoint <url> [key] set webhook URL (and optional key)\n"
    "  name <label>         set device name (and mDNS host)\n"
    "  set <field> <value>  set any field (see the setup page)\n"
    "  set lineout on|off   print every pushed reading on this port as oat-line (default on)\n"
    "  get <field>          show a field (secrets masked)\n"
    "  push                 send a test reading now\n"
    "  ap on | ap off       raise / drop the setup WiFi\n"
    "  reconnect            re-apply WiFi now\n"
    "  reboot               restart the device\n"
    "  factory-reset CONFIRM  erase all settings"));
  // The fixed list above is the core's. What follows is the sketch's own, from the
  // driver's command table, so `help` never advertises a command this board lacks
  // (the 1-Wire `bus` and `pins` used to be printed by radio-only sketches).
  if (g_drv->nCommands) {
    Serial.printf("%s adds:\n", g_drv->tier);
    for (int i = 0; i < g_drv->nCommands; i++) Serial.printf("  %-20s %s\n", g_drv->commands[i].name, g_drv->commands[i].help);
  }
  if (!g_drv->rescan) Serial.println("(no bus on this sketch: `rescan` restarts its radio or does nothing)");
}

void consoleStatus() {
  Serial.printf("id=%s fw=%s chip=%s\n", cfg.device_id.c_str(), g_drv->fw_version, ESP.getChipModel());
  Serial.printf("wifi=%s %s signal=%s\n", WiFi.isConnected() ? "up" : "down",
                localUrlsPlain().c_str(), signalWords().c_str());
  Serial.printf("uptime=%lus boots=%u reset=%s reconnects=%u\n",
                millis()/1000, bootCount, resetReasonStr(), wifiReconnects);
  Serial.printf("heap free=%u min=%u largest=%u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap(), largestFreeBlock());
  Serial.printf("push ok=%u fail=%u transport=%s last=%s\n",
                pushOk, pushFail, lastTransport.c_str(), lastPushMsg.c_str());
  if (g_drv->statusText) Serial.print(g_drv->statusText());
  Serial.printf("diag loops/s=%u die-temp=%.1fC reads ok/fail/crc=%u/%u/%u bus-resets=%u\n",
                g_loopsPerSec, temperatureRead(), g_readsOk, g_readsFail, g_crcFail, g_busResets);
}

// Every (re)connect goes through here. A begin() issued while a previous attempt
// is still in flight fails with ESP_ERR_WIFI_STATE ("sta is connecting, cannot set
// config") and the new SSID and password never reach the radio — disconnect first so the
// config always takes. Auto-reconnect is off (startNetworking); this + the 15 s
// loop cadence are the only connect paths.
void staBegin() {
  WiFi.disconnect();
  delay(100);
  lastWifiTry = millis();
  WiFi.begin(cfg.ssid.c_str(), cfg.wifi_pw.c_str());
}

void reconnectWifi() {
  if (!cfg.ssid.length()) { Serial.println("[OAT] no SSID set"); return; }
  staBegin();
  Serial.printf("[OAT] joining %s ...\n", cfg.ssid.c_str());
}

void handleConsoleLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  int sp = line.indexOf(' ');
  String cmd = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? ""  : line.substring(sp + 1);
  rest.trim();
  cmd.toLowerCase();
  if (cmd.startsWith("improv")) return;   // ESP Web Tools serial probe, not a user command
  String m;

  if (cmd == "help")            consoleHelp();
  else if (cmd == "status")     consoleStatus();
  else if (cmd == "ip")         Serial.println(WiFi.isConnected() ? localUrlsPlain() : String("AP http://") + AP_IP.toString());
  else if (cmd == "heap")       Serial.printf("free=%u min=%u largest=%u (TLS needs >= %u)\n",
                                              ESP.getFreeHeap(), ESP.getMinFreeHeap(), largestFreeBlock(), (uint32_t)TLS_MIN_HEAP);
  else if (cmd == "read")     { driverSampleBlocking(); Serial.println("[oat] " + lastReadMsg); }
  else if (cmd == "rescan")   { if (g_drv->rescan) { g_drv->rescan(); Serial.printf("[oat] %d sensor(s)\n", slotCount()); }
                                else Serial.println("[oat] this driver has nothing to re-scan"); }
  else if (cmd == "diag")     { Serial.println(g_drv->diagFull ? g_drv->diagFull()
                                                               : (g_drv->diag ? g_drv->diag() : String("no diagnosis available"))); }
  else if (cmd == "scan")     { scanWifi(); Serial.println("nearby: " + g_ssidOptions); }
  else if (cmd == "push")     { pushAll(); Serial.println("[OAT] " + lastPushMsg); }
  else if (dispatchDriverCommand(cmd, rest)) { /* handled by the sensor driver */ }
  else if (cmd == "reboot")   { Serial.println("[OAT] rebooting"); delay(200); ESP.restart(); }
  else if (cmd == "reconnect"){ reconnectWifi(); }
  else if (cmd == "name")     { if (rest.length()) { applyField("dev_name", rest, m); cfgSave(); startMdns(); Serial.println("[OAT] name set; " + localUrlsPlain()); } }
  else if (cmd == "wifi")     {
    int s2 = rest.indexOf(' ');
    String ssid = (s2 < 0) ? rest : rest.substring(0, s2);
    String pass = (s2 < 0) ? ""   : rest.substring(s2 + 1);
    applyField("ssid", ssid, m); if (pass.length()) applyField("wifi_pw", pass, m);
    cfgSave();
    // Echo the stored name in quotes: the first-space split truncates an SSID
    // containing spaces, and a silent truncation reads as "wrong password".
    Serial.printf("[OAT] ssid='%s'%s\n", cfg.ssid.c_str(),
                  pass.length() ? " (password saved)" : "");
    reconnectWifi();
  }
  else if (cmd == "endpoint") {
    int s2 = rest.indexOf(' ');
    String url = (s2 < 0) ? rest : rest.substring(0, s2);
    String key = (s2 < 0) ? ""   : rest.substring(s2 + 1);
    applyField("ep_url", url, m); if (key.length()) applyField("ep_key", key, m);
    cfgSave(); Serial.println("[OAT] endpoint set: " + cfg.ep_url);
  }
  else if (cmd == "ap")       {
    if (rest == "on")  { startAP(); apSticky = true;  Serial.println("[OAT] AP up: " + apSsid()); }
    if (rest == "off") { stopAP();  apSticky = false; Serial.println("[OAT] AP down"); }
  }
  else if (cmd == "set")      {
    int s2 = rest.indexOf(' ');
    String key = (s2 < 0) ? rest : rest.substring(0, s2);
    String val = (s2 < 0) ? ""   : rest.substring(s2 + 1);
    if (applyField(key, val, m)) {
      cfgSave();
      for (int i = 0; i < g_drv->nFields; i++)
        if (key == g_drv->fields[i].key && g_drv->rescan) { g_drv->rescan(); break; }
      Serial.println("[OAT] " + key + " = " + readField(key));
    }
    else Serial.println("[OAT] " + m);
  }
  else if (cmd == "get")      { Serial.println(rest + " = " + readField(rest)); }
  else if (cmd == "factory-reset") {
    if (rest == "CONFIRM") { Serial.println("[OAT] erasing + rebooting"); factoryReset(); delay(400); ESP.restart(); }
    else Serial.println("[OAT] type: factory-reset CONFIRM");
  }
  else Serial.println("[OAT] ? '" + cmd + "' — type help");
}

void pollConsole() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) { handleConsoleLine(line); line = ""; } }
    else if (c >= 0x20 && c <= 0x7e && line.length() < 160) line += c;   // printable only — drop improv probe bytes
  }
}

// ----------------------------------------------------------------------------
// Networking: AP + station + mDNS
// ----------------------------------------------------------------------------
void scanWifi() {
  // A scan is refused while a connect attempt is in flight — and a station hunting
  // a missing SSID is ALWAYS mid-attempt, which made `scan` return an empty list
  // exactly when it was needed. Clear the attempt first; the retry loop resumes.
  if (!WiFi.isConnected()) { WiFi.disconnect(); delay(100); }
  int n = WiFi.scanNetworks(false, false);
  String opts;
  for (int i = 0; i < n && i < 20; i++) {
    String s = WiFi.SSID(i); s.replace("'", "");
    if (s.length()) opts += "<option value='" + s + "'>";
  }
  g_ssidOptions = opts;
  WiFi.scanDelete();
}

void startMdns() {
  // Start ONCE. Re-running MDNS.end()/begin() on every WiFi reconnect churns the
  // lwIP timer list and trips "Required to lock TCPIP core functionality" -> panic
  // (it bit us on a weak link that reconnects a lot). Only re-init on a name change.
  static bool mdnsUp = false;
  String want = mdnsHost();
  if (mdnsUp && want == g_mdnsHost) return;
  if (mdnsUp) MDNS.end();
  g_mdnsHost = want;
  if (MDNS.begin(g_mdnsHost.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("oat", "tcp", 80);            // _oat._tcp — discover a fleet at once
    MDNS.addServiceTxt("oat", "tcp", "id", cfg.device_id.c_str());
    MDNS.addServiceTxt("oat", "tcp", "name", cfg.device_name.c_str());
    MDNS.addServiceTxt("oat", "tcp", "loc", cfg.location.c_str());
    MDNS.addServiceTxt("oat", "tcp", "fw", g_drv->fw_version);
    mdnsUp = true;
  }
}

void startAP() {
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid().c_str());
  dnsServer.start(DNS_PORT, "*", AP_IP);
  apActive = true;
}

void stopAP() {
  if (!apActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  apDropAtMs = 0;
}

// First-boot / power-on networking decision.
void startNetworking() {
  // Come up in AP_STA and STAY there — the arrangement that ran rock-solid on a
  // weak link. An "on-demand AP" that switches WiFi modes and rescans on every
  // disconnect churns the netif and amplifies the very dropouts it reacts to. The
  // AP staying up costs a few KB and some beacons; stability beats tidiness.
  WiFi.persistent(false);
  // Auto-reconnect OFF: with the stored SSID absent (node carried to a new site),
  // the core's auto-reconnect holds the station in a PERPETUAL connecting state —
  // every later begin() fails ESP_ERR_WIFI_STATE (a new SSID and password never take),
  // scans are refused, and the endless channel-hunt drags the shared radio off the
  // softAP's channel so the setup AP disappears. The sketch owns the cadence.
  WiFi.setAutoReconnect(false);
  startAP();                                            // AP_STA + softAP + captive DNS, once, for good
  if (cfg.ssid.length()) staBegin();
}

// ----------------------------------------------------------------------------
// Setup / loop
// ----------------------------------------------------------------------------
void begin(const Driver& driver) {
  g_drv = &driver;
  Serial.begin(115200);
  delay(200);
  Serial.println(String("\n[OAT] ") + g_drv->fw_version + " booting (reset: " + resetReasonStr() + ")");

  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  cfgLoad();

  // Read the boot counter, but DO NOT write it here. The write is deferred to
  // loop(), once the node has stayed up long enough to prove it is not in a reset
  // loop (see commitBootCount below).
  //
  // Writing it here is what the extracted core inherited, and it is a trap: a node
  // that resets several times a second then writes NVS on every iteration of that
  // loop. Thousands of flash writes a minute, each one liable to be cut in half by
  // the next reset, which is how a recoverable crash becomes a chip that will not
  // read back. The sketch this core was extracted FROM opened NVS read-only at boot
  // and only ever wrote when someone saved a setting. That property was worth
  // keeping and the port dropped it.
  // RTC slow memory survives every SOFTWARE reset (panic, watchdog, brownout-reset)
  // and is cleared by a real power cycle. So a reset loop keeps counting upward here,
  // in RAM, and the push sequence stays monotonic without touching flash at all.
  // Only a cold boot has to consult NVS.
  if (rtcSeqMagic == RTC_SEQ_MAGIC) {
    bootCount = rtcBootCount + 1;                 // soft reset: no flash read, no write
  } else {
    prefs.begin(g_drv->nvs, true);                // cold boot: read only
    bootCount = prefs.getULong("boot_n", 0) + 1;
    prefs.end();
    rtcSeqMagic = RTC_SEQ_MAGIC;
  }
  rtcBootCount = bootCount;
  g_seq = (uint64_t)bootCount * 1000000ULL;   // monotonic seq base, unique across boots

  sensorMutex = xSemaphoreCreateMutex();   // before the driver's begin(): it claims slots

  g_drv->begin();
  startNetworking();
  configTime(0, 0, cfg.ntp_server.c_str());

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/save",    HTTP_POST, handleSave);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/readnow", HTTP_GET,  handleReadNow);
  server.on("/pushnow", HTTP_GET,  handlePushNow);
  server.on("/rescan",  HTTP_GET,  handleRescan);       // WiFi networks
  server.on("/rescansensors", HTTP_GET, handleRescanSensors);
  server.on("/diag",    HTTP_GET,  handleDiag);
  server.on("/reset",   HTTP_GET,  handleReset);
  // captive-portal probes (iOS / Android / Windows) -> bounce to the setup page
  server.on("/generate_204",       handlePortal);
  server.on("/gen_204",            handlePortal);
  server.on("/hotspot-detect.html",handlePortal);
  server.on("/ncsi.txt",           handlePortal);
  server.on("/connecttest.txt",    handlePortal);
  server.on("/canonical.html",     handlePortal);
  server.on("/success.txt",        handlePortal);
  server.onNotFound(handleNotFound);
  server.begin();

  printReadyBanner();
  lastPushMs = millis();
  lastBeaconMs = millis();
  lastSampleMs = 0;            // sample immediately on the first loop
  g_loopSampleMs = millis();
  staDownSince = millis();   // configured-but-not-yet-joined
}

// Persist the boot counter once the node has proved it is staying up. A board in a
// reset loop never reaches this, so it never writes, which is the whole point: the
// flash stays untouched while something is wrong instead of being hammered by it.
static void commitBootCount() {
  if (g_bootCountCommitted || millis() < BOOT_COUNT_COMMIT_MS) return;
  g_bootCountCommitted = true;
  prefs.begin(g_drv->nvs, false);
  prefs.putULong("boot_n", bootCount);
  prefs.end();
}

void loop() {
  g_loopCount++;
  commitBootCount();
  if (millis() - g_loopSampleMs >= 2000) {                 // refresh the loop-rate gauge ~every 2s
    uint32_t n = g_loopCount;
    unsigned long dt = millis() - g_loopSampleMs;
    g_loopsPerSec = dt ? (uint32_t)((uint64_t)(n - g_loopSampleCount) * 1000UL / dt) : 0;
    g_loopSampleCount = n; g_loopSampleMs = millis();
  }
  if (apActive) dnsServer.processNextRequest();
  server.handleClient();
  pollConsole();

  if (cfg.method == "mqtt" && mqtt.connected()) mqtt.loop();

  bool up = WiFi.isConnected();

  // STA transition handling: announce on (re)connect.
  static bool everUp = false;
  if (up && !staWasUp) {
    if (everUp) wifiReconnects++;        // count reconnections, not the first connect
    everUp = true; staWasUp = true; staDownSince = 0;
    startMdns();
    printReadyBanner();
  } else if (!up && staWasUp) {
    staWasUp = false; staDownSince = millis();
  }

  // Station reconnect attempts. Back off 8x while a phone is on the setup AP —
  // each attempt's scan drags the shared radio off the AP's channel, which is how
  // the AP kept "vanishing" mid-setup right when someone was using it.
  unsigned long retryEvery = WiFi.softAPgetStationNum() > 0 ? WIFI_RETRY_MS * 8 : WIFI_RETRY_MS;
  if (cfg.ssid.length() && !up && millis() - lastWifiTry > retryEvery) {
    staBegin();
  }

  // The sensor runs on its own cadence, network or no network: a node that comes
  // back from a dropout should have a full window of readings to average, not an
  // empty one.
  driverCollect();                // finish any async read whose window has passed
  if (firstSample || millis() - lastSampleMs >= cfg.sample_s * 1000UL) {
    firstSample = false;
    lastSampleMs = millis();
    driverSample();
  }

  // Heartbeat FIRST (sacred — a tiny liveness packet must get out even when the
  // sensor is dead or a batch is struggling), then the sensor push.
  if (up && millis() - lastBeaconMs >= BEACON_EVERY_MS)       pushBeacon();
  if (up && millis() - lastPushMs >= cfg.interval_s * 1000UL) pushAll();
}
// ---------------------------------------------------------------------------
// Slot API (declared in the header)
// ---------------------------------------------------------------------------
int slotFor(const char* id, const char* physical) {
  if (!id || !id[0]) return -1;
  // Claiming a slot mutates the table the push reads, so it takes the lock too: a
  // BLE listener claims slots as devices appear, at any moment.
  if (sensorMutex) xSemaphoreTake(sensorMutex, portMAX_DELAY);
  struct Unlock { ~Unlock() { if (sensorMutex) xSemaphoreGive(sensorMutex); } } _unlock;
  for (int i = 0; i < MAX_SLOTS; i++)
    if (slots[i].used && strncmp(slots[i].id, id, ID_LEN - 1) == 0) return i;
  for (int i = 0; i < MAX_SLOTS; i++) {
    if (slots[i].used) continue;
    slots[i] = Slot();
    slots[i].used = true;
    strncpy(slots[i].id, id, ID_LEN - 1);
    strncpy(slots[i].physical, physical && physical[0] ? physical : id, ID_LEN - 1);
    return i;
  }
  return -1;                                  // full: bounded by design, never grows
}

void fold(int slot, const char* measurement, const char* unit, uint8_t kind, double value) {
  if (slot < 0 || slot >= MAX_SLOTS || !slots[slot].used) return;
  // The push snapshot holds this lock while it drains the windows. A driver that
  // folds from its own task — the BLE listener decodes in a worker — would race it
  // otherwise, and a torn accumulator is a wrong number rather than a missing one.
  // Locking here means no driver has to remember to.
  if (sensorMutex) xSemaphoreTake(sensorMutex, portMAX_DELAY);
  Slot &s = slots[slot];
  int use = -1, freeSlot = -1;
  for (int j = 0; j < MEAS_PER_SLOT; j++) {
    if (s.meas[j].used && strcmp(s.meas[j].measurement, measurement) == 0) { use = j; break; }
    if (!s.meas[j].used && freeSlot < 0) freeSlot = j;
  }
  if (use < 0) use = freeSlot;
  if (use < 0) return;
  MeasAcc &m = s.meas[use];
  if (!m.used) {
    m = MeasAcc(); m.used = true; m.kind = kind;
    strncpy(m.measurement, measurement, sizeof(m.measurement) - 1);
    strncpy(m.unit, unit ? unit : "", sizeof(m.unit) - 1);
  }
  if      (kind == oat::KIND_CONTINUOUS) { m.sum += value; m.n++; }
  else if (kind == oat::KIND_EVENT)      { m.n++; m.last = value; }
  else                                   { m.last = value; m.n++; }
  if (s.samples == 0) s.windowStartMs = millis();
  s.samples++;
  if (sensorMutex) xSemaphoreGive(sensorMutex);
}

void release(int slot) {
  if (slot < 0 || slot >= MAX_SLOTS) return;
  if (sensorMutex) xSemaphoreTake(sensorMutex, portMAX_DELAY);
  slots[slot] = Slot();                       // clears the window too: an average of
  if (sensorMutex) xSemaphoreGive(sensorMutex);
}                                             // readings from a vanished sensor has no owner

int slotCount() {
  int n = 0;
  for (int i = 0; i < MAX_SLOTS; i++) if (slots[i].used) n++;
  return n;
}

void slotName(int slot, const char* name) {
  if (slot < 0 || slot >= MAX_SLOTS || !slots[slot].used) return;
  if (name && name[0]) strncpy(slots[slot].name, name, sizeof(slots[slot].name) - 1);
}

void slotMeta(int slot, const char* brand, const char* model) {
  if (slot < 0 || slot >= MAX_SLOTS || !slots[slot].used) return;
  if (brand && brand[0]) strncpy(slots[slot].brand, brand, sizeof(slots[slot].brand) - 1);
  if (model && model[0]) strncpy(slots[slot].model, model, sizeof(slots[slot].model) - 1);
}

void slotLink(int slot, int rssi, int battery_pct) {
  if (slot < 0 || slot >= MAX_SLOTS || !slots[slot].used) return;
  slots[slot].rssi = rssi;
  slots[slot].battery_pct = battery_pct;
}

const char* slotId(int slot) {
  return (slot >= 0 && slot < MAX_SLOTS && slots[slot].used) ? slots[slot].id : "";
}

// Driver blobs ride the driver's own namespace. A missing key is asked about
// first (Preferences logs an error otherwise), and NVS itself skips a write whose
// bytes equal what is stored, so a driver may save on every event without wear.
size_t blobLoad(const char* key, void* buf, size_t max) {
  if (!g_drv || !key || !buf) return 0;
  prefs.begin(g_drv->nvs, true);
  size_t n = prefs.isKey(key) ? prefs.getBytes(key, buf, max) : 0;
  prefs.end();
  return n;
}
bool blobSave(const char* key, const void* data, size_t n) {
  if (!g_drv || !key) return false;
  prefs.begin(g_drv->nvs, false);
  size_t w = prefs.putBytes(key, data, n);
  prefs.end();
  return w == n;
}
void blobErase(const char* key) {
  if (!g_drv || !key) return;
  prefs.begin(g_drv->nvs, false);
  if (prefs.isKey(key)) prefs.remove(key);
  prefs.end();
}

const String& deviceId()  { return cfg.device_id; }
const String& gatewayId() { return cfg.device_name.length() ? cfg.device_name : cfg.device_id; }
uint32_t sampleSeconds()  { return cfg.sample_s; }
void pushNow()            { pushAll(); }

void countRead(bool ok, bool crcFail) {
  if (ok) g_readsOk++;
  else { g_readsFail++; if (crcFail) g_crcFail++; }
}
void countBusReset() { g_busResets++; }

NodeStatus status() {
  NodeStatus st;
  st.wifi = WiFi.status() == WL_CONNECTED;
  st.ip = st.wifi ? WiFi.localIP().toString() : String("");
  st.apSsid = apSsid();
  st.lastPushOk = lastPushOk; st.pushOk = pushOk; st.pushFail = pushFail;
  st.lastPushMs = lastPushMs; st.lastPushMsg = lastPushMsg;
  return st;
}

}  // namespace oatcore
