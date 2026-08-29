/* =============================================================================
   OAT SDI-12 Reader  —  Reference Node v1.0.0
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   PURPOSE
     A self-configuring ESP32 node that reads SDI-12 sensors — the open digital
     bus on professional instruments (Apogee SQ-521, soil/water probes, etc.) —
     and pushes each reading to a user-designated endpoint as oat-ods, by WEBHOOK
     (HTTP POST) or MQTT. It is the bridge that lands a grower's existing
     research-grade sensors directly in their own data lake.

     No code editing required. All setup is done at runtime on a password-
     protected page the device serves. Settings persist in NVS across reboots
     and reflashes.

   THE SENSOR MAP (how the node knows what it's reading)
     SDI-12 returns bare numbers — it doesn't say what they mean. So you tell the
     node, one line per sensor, on the setup page:

         <addr> <stream_id> <measure1>:<unit1>,<measure2>:<unit2>,...

     e.g.   0 gh2-soil moisture:%,temperature:Cel,ec:dS/m
            1 canopy-par par:umol/m2/s

     The node polls each address (aM! then aD0!), and maps the returned values,
     in order, to the measurement/unit labels you gave. Each value becomes one
     oat-ods message (stream = stream_id, physical_id = sdi12:<addr>).

   WIRING
     SDI-12 is a single data wire (plus power + ground). The data line is 5 V
     logic (idles low, signals swing to 5 V); the ESP32 is 3.3 V, so use a level
     interface on the data line for reliable reads: one channel of a common
     BSS138 logic-level converter board works.
     Many sensors also need 12 V on their power line — separate from the ESP32.

   REQUIRED LIBRARIES (managed by platformio.ini lib_deps; pinned)
     - SDI-12          EnviroDIY/SDI-12          (the bus protocol)
     - PubSubClient    knolleary/PubSubClient    (MQTT)
     - ArduinoJson     bblanchon/ArduinoJson     (>= 7.x; used by oat_ods)
     - oat_ods         firmware/lib/oat_ods       (the shared OAT encoder)
     Built into the ESP32 core: WiFi, WebServer, DNSServer, Preferences,
     HTTPClient, WiFiClientSecure, time.h, esp_system.h

   Source release: not yet bench-verified like the live sketches. build.sh is the
   compile gate before any chip is flashed.
   ============================================================================= */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_system.h>

#include <SDI12.h>
#include <PubSubClient.h>
#include <oat_ods.h>          // shared OAT encoder module (firmware/lib/oat_ods)

// ----------------------------------------------------------------------------
// Compile-time constants
// ----------------------------------------------------------------------------
#define FW_NAME           "oat-sdi12-reader"
#define FW_SEMVER         "1.0.0"
#define FW_VERSION        "OAT-SDI12-Reader/1.0.0"
#define ADMIN_USER        "admin"
#define DEFAULT_ADMIN_PW  "oatsetup"
#define MAX_SENSORS       12        // configured SDI-12 sensors
#define MAX_VALS          8         // values mapped per sensor
#define DEFAULT_SDI12_PIN 16
#define LOGO_URL "https://openagriculturetechnology.com/assets/img/logo-horizontal.png"

static const byte DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);

// ----------------------------------------------------------------------------
// Configuration (persisted in NVS)
// ----------------------------------------------------------------------------
struct Config {
  String device_id;        // permanent, hardware-derived (MAC); not editable
  String device_name;      // assignable gateway/node name
  String location;         // free-text zone

  String ssid, wifi_pw;

  String method;           // "webhook" | "mqtt"
  String ep_url, ep_auth;
  String mq_host; uint16_t mq_port; String mq_user, mq_pw, mq_topic; bool mq_tls;

  uint32_t interval_s;     // poll interval
  int      sdi12_pin;      // GPIO for the SDI-12 data line
  String   sensor_map;     // one line per sensor (see header)

  String ntp_server, admin_pw;
};

Config cfg;
Preferences prefs;
uint32_t bootCount = 0;

// ----------------------------------------------------------------------------
// Runtime state
// ----------------------------------------------------------------------------
WebServer        server(80);
DNSServer        dnsServer;
WiFiClient       netPlain;
WiFiClientSecure netTls;
PubSubClient     mqtt;
SDI12*           sdi = nullptr;     // created on the configured pin in setup()

struct Sensor {
  char addr;
  char stream_id[28];
  int  nVals;
  char meas[MAX_VALS][16];
  char unit[MAX_VALS][12];
};
Sensor sensors[MAX_SENSORS];
int    nSensors = 0;

unsigned long lastPushMs = 0;
bool          lastPushOk = false;
String        lastPushMsg = "no poll yet";
String        lastScan = "no poll yet";
uint32_t      pushCount = 0;

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------
static String macFromChip() {
  uint8_t m[6]; WiFi.macAddress(m);
  char buf[20];
  snprintf(buf, sizeof(buf), "oat-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}
static String apSsid() {
  uint8_t m[6]; WiFi.macAddress(m);
  char buf[24];
  snprintf(buf, sizeof(buf), "OAT-SDI12-%02X%02X%02X", m[3], m[4], m[5]);
  return String(buf);
}
static String isoNowUTC() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";
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
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                return "other";
  }
}

// ----------------------------------------------------------------------------
// NVS load / save
// ----------------------------------------------------------------------------
void cfgLoad() {
  prefs.begin("oatsdi", true);
  cfg.device_id   = macFromChip();
  cfg.device_name = prefs.getString("dev_name", "");
  cfg.location    = prefs.getString("location", "");
  cfg.ssid        = prefs.getString("ssid",     "");
  cfg.wifi_pw     = prefs.getString("wifi_pw",  "");
  cfg.method      = prefs.getString("method",   "webhook");
  cfg.ep_url      = prefs.getString("ep_url",   "");
  cfg.ep_auth     = prefs.getString("ep_auth",  "");
  cfg.mq_host     = prefs.getString("mq_host",  "");
  cfg.mq_port     = prefs.getUShort("mq_port",  1883);
  cfg.mq_user     = prefs.getString("mq_user",  "");
  cfg.mq_pw       = prefs.getString("mq_pw",    "");
  cfg.mq_topic    = prefs.getString("mq_topic", "oat");
  cfg.mq_tls      = prefs.getBool  ("mq_tls",   false);
  cfg.interval_s  = prefs.getULong ("interval", 60);
  cfg.sdi12_pin   = prefs.getInt   ("sdi_pin",  DEFAULT_SDI12_PIN);
  cfg.sensor_map  = prefs.getString("map",      "");
  cfg.ntp_server  = prefs.getString("ntp",      "pool.ntp.org");
  cfg.admin_pw    = prefs.getString("admin_pw", DEFAULT_ADMIN_PW);
  prefs.end();
  if (cfg.interval_s < 10) cfg.interval_s = 10;
}

void cfgSave() {
  prefs.begin("oatsdi", false);
  prefs.putString("dev_name", cfg.device_name);
  prefs.putString("location", cfg.location);
  prefs.putString("ssid",     cfg.ssid);
  prefs.putString("wifi_pw",  cfg.wifi_pw);
  prefs.putString("method",   cfg.method);
  prefs.putString("ep_url",   cfg.ep_url);
  prefs.putString("ep_auth",  cfg.ep_auth);
  prefs.putString("mq_host",  cfg.mq_host);
  prefs.putUShort("mq_port",  cfg.mq_port);
  prefs.putString("mq_user",  cfg.mq_user);
  prefs.putString("mq_pw",    cfg.mq_pw);
  prefs.putString("mq_topic", cfg.mq_topic);
  prefs.putBool  ("mq_tls",   cfg.mq_tls);
  prefs.putULong ("interval", cfg.interval_s);
  prefs.putInt   ("sdi_pin",  cfg.sdi12_pin);
  prefs.putString("map",      cfg.sensor_map);
  prefs.putString("ntp",      cfg.ntp_server);
  prefs.putString("admin_pw", cfg.admin_pw);
  prefs.end();
}

// ----------------------------------------------------------------------------
// Sensor map parsing  ("addr stream_id m1:u1,m2:u2,...")
// ----------------------------------------------------------------------------
void parseSensorMap() {
  nSensors = 0;
  const String& s = cfg.sensor_map;
  int start = 0;
  while (start < (int)s.length() && nSensors < MAX_SENSORS) {
    int nl = s.indexOf('\n', start);
    String line = (nl < 0) ? s.substring(start) : s.substring(start, nl);
    start = (nl < 0) ? s.length() : nl + 1;
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;

    int sp1 = line.indexOf(' ');                if (sp1 < 0) continue;
    int sp2 = line.indexOf(' ', sp1 + 1);       if (sp2 < 0) continue;
    String a    = line.substring(0, sp1);       a.trim();
    String sid  = line.substring(sp1 + 1, sp2); sid.trim();
    String pairs= line.substring(sp2 + 1);      pairs.trim();
    if (a.length() == 0 || sid.length() == 0 || pairs.length() == 0) continue;

    Sensor &S = sensors[nSensors];
    S.addr = a[0];
    strncpy(S.stream_id, sid.c_str(), sizeof(S.stream_id) - 1); S.stream_id[sizeof(S.stream_id) - 1] = 0;
    S.nVals = 0;
    int ps = 0;
    while (ps < (int)pairs.length() && S.nVals < MAX_VALS) {
      int comma = pairs.indexOf(',', ps);
      String pair = (comma < 0) ? pairs.substring(ps) : pairs.substring(ps, comma);
      ps = (comma < 0) ? pairs.length() : comma + 1;
      pair.trim(); if (!pair.length()) continue;
      int colon = pair.indexOf(':');
      String m = (colon < 0) ? pair : pair.substring(0, colon);
      String u = (colon < 0) ? ""   : pair.substring(colon + 1);
      m.trim(); u.trim();
      strncpy(S.meas[S.nVals], m.c_str(), 15); S.meas[S.nVals][15] = 0;
      strncpy(S.unit[S.nVals], u.c_str(), 11); S.unit[S.nVals][11] = 0;
      S.nVals++;
    }
    if (S.nVals > 0) nSensors++;
  }
}

// ----------------------------------------------------------------------------
// SDI-12 transaction helpers
// ----------------------------------------------------------------------------
String sdiCmd(const String &cmd) {
  if (!sdi) return "";
  sdi->clearBuffer();
  String c = cmd;              // the SDI-12 lib's sendCommand takes a non-const String&
  sdi->sendCommand(c);
  delay(300);                                   // let the sensor answer
  String r;
  while (sdi->available()) { char c = sdi->read(); if (c != '\r' && c != '\n') r += c; delay(10); }
  return r;
}

// Parse "a+1.23-4.50+42" -> doubles (first char is the address; +/- both sign and delimit).
int parseSdiValues(const String &resp, double *out, int maxOut) {
  int n = 0, i = 1, len = resp.length();
  while (i < len && n < maxOut) {
    char c = resp[i];
    if (c == '+' || c == '-') {
      int j = i + 1;
      while (j < len && resp[j] != '+' && resp[j] != '-') j++;
      String tok = resp.substring(i, j);
      if (tok.length() && tok[0] == '+') tok = tok.substring(1);  // toFloat dislikes a leading +
      out[n++] = tok.toFloat();
      i = j;
    } else i++;
  }
  return n;
}

// aM! (start) -> wait the stated seconds -> aD0..aDn (collect) -> value count.
int readSensor(char addr, double *vals, int maxVals) {
  if (!sdi) return 0;
  String a = String(addr);
  String m = sdiCmd(a + "M!");                  // "atttn"
  if (m.length() < 5) return 0;
  int waitS = m.substring(1, 4).toInt();
  int count = m.substring(4, 5).toInt();
  if (waitS > 30) waitS = 30;                   // safety cap (most sensors: 1-3 s)
  delay((uint32_t)waitS * 1000UL);
  int n = 0;
  for (int d = 0; d <= 9 && n < count && n < maxVals; d++) {
    String resp = sdiCmd(a + "D" + String(d) + "!");
    if (resp.length() <= 1) break;
    n += parseSdiValues(resp, vals + n, maxVals - n);
  }
  return n;
}

// ----------------------------------------------------------------------------
// Delivery: oat-ods over webhook + mqtt (shared with the BLE listener pattern)
// ----------------------------------------------------------------------------
static oat::Device oatDevice() {
  oat::Device d;
  d.gateway_id = cfg.device_name.length() ? cfg.device_name.c_str() : cfg.device_id.c_str();
  d.device_id  = cfg.device_id.c_str();   // hardware provenance (efuse MAC) — rides the status message only
  d.tier       = FW_NAME;
  d.fw         = FW_VERSION;
  return d;
}

bool pushWebhook(const String &payload) {
  if (cfg.ep_url.length() == 0) { lastPushMsg = "no webhook URL set"; return false; }
  bool https = cfg.ep_url.startsWith("https");
  HTTPClient http;
  bool began = https ? (netTls.setInsecure(), http.begin(netTls, cfg.ep_url))
                     : http.begin(netPlain, cfg.ep_url);
  if (!began) { lastPushMsg = "http.begin failed"; return false; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", FW_VERSION);
  if (cfg.ep_auth.length()) http.addHeader("Authorization", cfg.ep_auth);
  int code = http.POST(payload);
  http.end();
  lastPushMsg = "HTTP " + String(code);
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
    hh.uptime_s = (uint32_t)(millis() / 1000);
    hh.free_heap = ESP.getFreeHeap();
    hh.boot_count = bootCount;
    hh.reset = resetReasonStr();
    hh.rssi = WiFi.RSSI();
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
  bool ok = mqtt.publish(topic.c_str(), payload.c_str(), true);   // retain last value per stream
  lastPushMsg = ok ? ("published " + topic) : "mqtt publish failed";
  return ok;
}

bool sendMessage(const oat::Device &dev, const oat::Reading &r) {
  String payload; oat::encodeNative(dev, r, payload);
  if (cfg.method == "mqtt") return mqttPublish(oat::mqttTopic(cfg.mq_topic.c_str(), dev, r), payload);
  return pushWebhook(payload);
}

// ----------------------------------------------------------------------------
// Poll: read every configured sensor, emit one oat-ods message per value
// ----------------------------------------------------------------------------
void doPoll() {
  String iso = isoNowUTC();
  oat::Device dev = oatDevice();
  uint32_t sent = 0; bool anyOk = false; int sensorsRead = 0;

  for (int i = 0; i < nSensors; i++) {
    Sensor &S = sensors[i];
    double vals[MAX_VALS];
    int n = readSensor(S.addr, vals, MAX_VALS);
    if (n <= 0) continue;
    sensorsRead++;
    char pid[20]; snprintf(pid, sizeof(pid), "sdi12:%c", S.addr);
    for (int v = 0; v < n && v < S.nVals; v++) {
      oat::Reading r;
      r.stream_id   = S.stream_id;
      r.location    = cfg.location.c_str();
      r.physical_id = pid;
      r.observed_at = iso.c_str();
      r.measurement = S.meas[v];
      if (S.unit[v][0]) r.unit = S.unit[v];
      r.value = vals[v];
      if (sendMessage(dev, r)) { anyOk = true; sent++; }
    }
  }

  lastPushOk = anyOk;
  lastPushMs = millis();
  if (anyOk) pushCount++;
  lastScan = String(sensorsRead) + " sensor(s) read, " + String(sent) + " message(s) — " + lastPushMsg;
  Serial.printf("[poll] sensors=%d messages=%u ok=%d msg=%s\n", sensorsRead, sent, anyOk, lastPushMsg.c_str());
}

// ----------------------------------------------------------------------------
// Web UI
// ----------------------------------------------------------------------------
static const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>OAT SDI-12 Reader</title><style>"
  "body{font-family:system-ui,Arial,sans-serif;background:#0f1413;color:#e7efe9;margin:0;padding:1rem;}"
  ".card{max-width:640px;margin:0 auto;background:#172120;border:1px solid #2a3a37;border-radius:10px;padding:1.2rem;}"
  "h1{font-size:1.2rem;margin:.2rem 0 1rem;color:#bfe3c8;}h2{font-size:.95rem;color:#8fb39a;margin:1.2rem 0 .4rem;border-bottom:1px solid #2a3a37;padding-bottom:.2rem;}"
  "label{display:block;font-size:.8rem;margin:.6rem 0 .15rem;color:#9fb8a6;}"
  "input,select,textarea{width:100%;box-sizing:border-box;padding:.5rem;border-radius:6px;border:1px solid #344743;background:#0f1413;color:#e7efe9;font-size:.9rem;font-family:inherit;}"
  "textarea{min-height:5rem;font-family:ui-monospace,monospace;font-size:.82rem;}"
  ".row{display:flex;gap:.6rem;}.row>div{flex:1;}"
  ".btn{margin-top:1.2rem;width:100%;padding:.7rem;background:#2e7d54;color:#fff;border:0;border-radius:8px;font-size:1rem;cursor:pointer;}"
  ".muted{font-size:.72rem;color:#6f8a78;margin-top:.15rem;}a{color:#7fd0a0;}"
  ".brand{display:flex;align-items:center;gap:.5rem;margin-bottom:.7rem;}"
  ".bname{font-weight:600;font-size:.95rem;color:#bfe3c8;letter-spacing:.2px;}"
  "</style></head><body><div class='card'>"
  "<div class='brand'>"
#ifdef LOGO_URL
  "<img src='" LOGO_URL "' height='28' alt='' style='display:block' onerror='this.remove()'>"
#endif
  "<span class='bname'>OpenAgricultureTechnology<span style='color:#6f8a78'>.com</span></span>"
  "</div>";

static const char PAGE_FOOT[] PROGMEM = "</div></body></html>";

String fieldsForm() {
  String webhookSel = (cfg.method == "webhook") ? "selected" : "";
  String mqttSel    = (cfg.method == "mqtt")    ? "selected" : "";
  String tlsChk     = cfg.mq_tls ? "checked" : "";
  String h;
  h += "<h1>OAT SDI-12 Reader</h1>";
  h += "<form method='POST' action='/save'>";

  h += "<h2>Identity</h2>";
  h += "<label>Device ID</label><input value='" + cfg.device_id + "' readonly style='opacity:.7'>";
  h += "<label>Node name (gateway id)</label><input name='dev_name' value='" + cfg.device_name + "' placeholder='GH2-soil-station'>";
  h += "<label>Location</label><input name='location' value='" + cfg.location + "' placeholder='Greenhouse 2 / North bench'>";

  h += "<h2>WiFi</h2>";
  h += "<label>SSID</label><input name='ssid' value='" + cfg.ssid + "'>";
  h += "<label>Password</label><input name='wifi_pw' type='password' placeholder='(unchanged)'>";

  h += "<h2>Delivery</h2>";
  h += "<label>Method</label><select name='method' id='method' onchange='tog()'>";
  h += "<option value='webhook' " + webhookSel + ">Webhook (HTTP POST)</option>";
  h += "<option value='mqtt' " + mqttSel + ">MQTT</option></select>";
  h += "<div id='wh'>";
  h += "<label>Webhook URL</label><input name='ep_url' value='" + cfg.ep_url + "' placeholder='https://...'>";
  h += "<label>Authorization header (optional)</label><input name='ep_auth' value='" + cfg.ep_auth + "' placeholder='Bearer abc123'>";
  h += "</div>";
  h += "<div id='mq'>";
  h += "<div class='row'><div><label>MQTT host</label><input name='mq_host' value='" + cfg.mq_host + "'></div>";
  h += "<div style='flex:.4'><label>Port</label><input name='mq_port' value='" + String(cfg.mq_port) + "'></div></div>";
  h += "<label>Topic prefix</label><input name='mq_topic' value='" + cfg.mq_topic + "'>";
  h += "<div class='muted'>Published to: prefix/node/stream/measurement (one topic per reading)</div>";
  h += "<div class='row'><div><label>Username (optional)</label><input name='mq_user' value='" + cfg.mq_user + "'></div>";
  h += "<div><label>Password</label><input name='mq_pw' type='password' placeholder='(unchanged)'></div></div>";
  h += "<label><input type='checkbox' name='mq_tls' " + tlsChk + " style='width:auto'> Use TLS</label>";
  h += "</div>";

  h += "<h2>SDI-12</h2>";
  h += "<div class='row'><div><label>Data pin (GPIO)</label><input name='sdi_pin' value='" + String(cfg.sdi12_pin) + "'></div>";
  h += "<div><label>Poll interval (sec)</label><input name='interval' value='" + String(cfg.interval_s) + "'></div></div>";
  h += "<label>Sensor map</label><textarea name='map' placeholder='0 gh2-soil moisture:%,temperature:Cel,ec:dS/m'>" + cfg.sensor_map + "</textarea>";
  h += "<div class='muted'>One line per sensor: <code>addr stream_id m1:u1,m2:u2,&hellip;</code> &mdash; values map in order. <a href='/scan'>Scan the bus &rarr;</a></div>";

  h += "<h2>Advanced</h2>";
  h += "<label>NTP server</label><input name='ntp' value='" + cfg.ntp_server + "'>";
  h += "<label>Admin password</label><input name='admin_pw' type='password' placeholder='(unchanged)'>";
  h += "<div class='muted'>Default is <code>oatsetup</code>. Change it once the node is online.</div>";

  h += "<button class='btn' type='submit'>Save &amp; Reboot</button></form>";
  h += "<p class='muted' style='text-align:center'><a href='/status'>Live status &amp; poll now &rarr;</a></p>";
  h += "<script>function tog(){var m=document.getElementById('method').value;"
       "document.getElementById('wh').style.display=(m=='webhook')?'block':'none';"
       "document.getElementById('mq').style.display=(m=='mqtt')?'block':'none';}tog();</script>";
  return h;
}

bool requireAuth() {
  if (!server.authenticate(ADMIN_USER, cfg.admin_pw.c_str())) { server.requestAuthentication(); return false; }
  return true;
}

void handleRoot() {
  if (!requireAuth()) return;
  String p = FPSTR(PAGE_HEAD); p += fieldsForm(); p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
}

void handleSave() {
  if (!requireAuth()) return;
  auto arg = [&](const char* k){ return server.arg(k); };
  cfg.device_name = arg("dev_name");
  cfg.location    = arg("location");
  cfg.ssid        = arg("ssid");
  if (arg("wifi_pw").length()) cfg.wifi_pw = arg("wifi_pw");
  cfg.method      = arg("method");
  cfg.ep_url      = arg("ep_url");
  cfg.ep_auth     = arg("ep_auth");
  cfg.mq_host     = arg("mq_host");
  cfg.mq_port     = (uint16_t) arg("mq_port").toInt();
  cfg.mq_user     = arg("mq_user");
  if (arg("mq_pw").length()) cfg.mq_pw = arg("mq_pw");
  cfg.mq_topic    = arg("mq_topic");
  cfg.mq_tls      = server.hasArg("mq_tls");
  cfg.interval_s  = (uint32_t) arg("interval").toInt(); if (cfg.interval_s < 10) cfg.interval_s = 10;
  cfg.sdi12_pin   = (int) arg("sdi_pin").toInt();
  cfg.sensor_map  = arg("map");
  cfg.ntp_server  = arg("ntp"); if (cfg.ntp_server.length() == 0) cfg.ntp_server = "pool.ntp.org";
  if (arg("admin_pw").length()) cfg.admin_pw = arg("admin_pw");

  cfgSave();
  String p = FPSTR(PAGE_HEAD);
  p += "<h1>Saved</h1><p>Rebooting in 3 seconds&hellip;</p>";
  p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
  delay(3000);
  ESP.restart();
}

void handleStatus() {
  if (!requireAuth()) return;
  String p = FPSTR(PAGE_HEAD);
  p += "<h1>Live Status</h1>";
  p += "<div class='muted'>ID " + cfg.device_id + " &middot; fw " + FW_VERSION + " &middot; chip " + String(ESP.getChipModel()) + "</div>";
  p += "<div class='muted'>WiFi: " + String(WiFi.isConnected() ? "connected " : "down ") + WiFi.localIP().toString() + " (RSSI " + String(WiFi.RSSI()) + ")</div>";
  p += "<div class='muted'>Uptime " + String(millis() / 1000) + "s &middot; boots " + String(bootCount) + " &middot; free heap " + String(ESP.getFreeHeap()) + "</div>";
  p += "<div class='muted'>Last poll: " + String(lastPushOk ? "OK" : "&mdash;") + " &middot; " + lastScan + " &middot; sent " + String(pushCount) + "</div>";

  p += "<h2>Configured sensors (" + String(nSensors) + ")</h2>";
  p += "<table style='width:100%;font-size:.78rem;border-collapse:collapse'>";
  p += "<tr style='color:#8fb39a;text-align:left'><th>Addr</th><th>Stream</th><th>Values</th></tr>";
  for (int i = 0; i < nSensors; i++) {
    String vlist;
    for (int v = 0; v < sensors[i].nVals; v++) { if (v) vlist += ", "; vlist += String(sensors[i].meas[v]); }
    p += "<tr><td>" + String(sensors[i].addr) + "</td><td>" + String(sensors[i].stream_id) + "</td><td>" + vlist + "</td></tr>";
  }
  p += "</table>";
  p += "<p style='text-align:center'><a href='/pushnow'>Poll now</a> &middot; <a href='/scan'>Scan the bus</a> &middot; <a href='/'>&larr; Setup</a></p>";
  p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
}

void handlePushNow() {
  if (!requireAuth()) return;
  doPoll();
  server.sendHeader("Location", "/status");
  server.send(303);
}

void handleScan() {
  if (!requireAuth()) return;
  String p = FPSTR(PAGE_HEAD);
  p += "<h1>SDI-12 scan</h1><p class='muted'>Querying addresses 0&ndash;9 on pin " + String(cfg.sdi12_pin) + "&hellip;</p>";
  p += "<table style='width:100%;font-size:.78rem;border-collapse:collapse'>";
  p += "<tr style='color:#8fb39a;text-align:left'><th>Addr</th><th>Identify (aI!)</th></tr>";
  for (char a = '0'; a <= '9'; a++) {
    String id = sdiCmd(String(a) + "I!");
    if (id.length() > 1) p += "<tr><td>" + String(a) + "</td><td>" + id + "</td></tr>";
  }
  p += "</table><div class='muted'>Empty = nothing answered. Put a responding address in your sensor map.</div>";
  p += "<p style='text-align:center'><a href='/'>&larr; Setup</a></p>";
  p += FPSTR(PAGE_FOOT);
  server.send(200, "text/html", p);
}

void handleNotFound() {
  server.sendHeader("Location", String("http://") + AP_IP.toString());
  server.send(302, "text/plain", "");
}

// ----------------------------------------------------------------------------
// Setup / loop
// ----------------------------------------------------------------------------
void startWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid().c_str());
  if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.wifi_pw.c_str());
  dnsServer.start(DNS_PORT, "*", AP_IP);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[OAT] " FW_VERSION " booting");

  cfgLoad();

  prefs.begin("oatsdi", false);
  bootCount = prefs.getULong("boot_n", 0) + 1;
  prefs.putULong("boot_n", bootCount);
  prefs.end();

  sdi = new SDI12(cfg.sdi12_pin);
  sdi->begin();
  parseSensorMap();

  startWiFi();
  configTime(0, 0, cfg.ntp_server.c_str());

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/save",    HTTP_POST, handleSave);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/pushnow", HTTP_GET,  handlePushNow);
  server.on("/scan",    HTTP_GET,  handleScan);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("[OAT] setup AP: %s  ->  http://%s  (%d sensors mapped)\n",
                apSsid().c_str(), AP_IP.toString().c_str(), nSensors);
  lastPushMs = millis();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  if (cfg.method == "mqtt" && mqtt.connected()) mqtt.loop();

  static unsigned long lastWifiTry = 0;
  if (cfg.ssid.length() && !WiFi.isConnected() && millis() - lastWifiTry > 15000) {
    lastWifiTry = millis();
    WiFi.begin(cfg.ssid.c_str(), cfg.wifi_pw.c_str());
  }

  if (WiFi.isConnected() && nSensors > 0 && millis() - lastPushMs >= cfg.interval_s * 1000UL) {
    doPoll();
  }
}
