/* =============================================================================
   OAT CWSI Node  —  Reference Node v1.0.0
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   PURPOSE
     A self-configuring ESP32 node that reads a non-contact infrared thermometer
     (MLX90614, I2C) pointed at the canopy, and pushes the readings to a user
     endpoint as oat-ods. It reports:
        canopy_temperature  — the leaf/canopy surface temp (IR object temp)
        air_temperature     — the sensor's ambient temp (an air-temp proxy)
        canopy_air_delta    — canopy minus air (the core water-stress signal)

     CWSI (Crop Water Stress Index) is a DERIVED index, not a raw reading: it
     needs the canopy-air delta AND vapor-pressure deficit (from air temp + RH).
     This node provides the canopy ingredient; CWSI is computed in the Use layer
     where VPD is known. A cool canopy (negative delta) = transpiring/well-
     watered; a warm canopy (positive delta) = closing stomata / water stress.

   WIRING
     MLX90614 is I2C (3.3 V): SDA + SCL + 3V3 + GND. Default pins SDA=21, SCL=22
     (configurable on the setup page for boards that differ, e.g. C3/C6).

   LIBRARIES (platformio.ini lib_deps; pinned)
     - Adafruit MLX90614 Library (+ Adafruit BusIO)
     - PubSubClient, ArduinoJson (>=7, used by oat_ods)
     - oat_ods  (firmware/lib/oat_ods — the shared OAT encoder)

   Source release: not yet bench-verified like the live sketches. build.sh is the compile gate.
   ============================================================================= */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_system.h>
#include <Wire.h>

#include <Adafruit_MLX90614.h>
#include <PubSubClient.h>
#include <oat_ods.h>          // shared OAT encoder module (firmware/lib/oat_ods)

#define FW_NAME           "oat-cwsi-node"
#define FW_SEMVER         "1.0.0"
#define FW_VERSION        "OAT-CWSI-Node/1.0.0"
#define ADMIN_USER        "admin"
#define DEFAULT_ADMIN_PW  "oatsetup"
#define DEFAULT_SDA       21
#define DEFAULT_SCL       22
#define LOGO_URL "https://openagriculturetechnology.com/assets/img/logo-horizontal.png"

static const byte DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);

struct Config {
  String device_id, device_name, location;
  String stream_id;        // the canopy spot, e.g. "gh2-canopy"
  String ssid, wifi_pw;
  String method, ep_url, ep_auth;
  String mq_host; uint16_t mq_port; String mq_user, mq_pw, mq_topic; bool mq_tls;
  uint32_t interval_s;
  int sda_pin, scl_pin;
  String ntp_server, admin_pw;
};
Config cfg;
Preferences prefs;
uint32_t bootCount = 0;

WebServer        server(80);
DNSServer        dnsServer;
WiFiClient       netPlain;
WiFiClientSecure netTls;
PubSubClient     mqtt;
Adafruit_MLX90614 mlx;
bool mlxOk = false;

unsigned long lastPushMs = 0;
bool          lastPushOk = false;
String        lastPushMsg = "no read yet";
String        lastReading = "no read yet";
uint32_t      pushCount = 0;

static String macFromChip() {
  uint8_t m[6]; WiFi.macAddress(m);
  char buf[20]; snprintf(buf, sizeof(buf), "oat-%02x%02x%02x%02x%02x%02x", m[0],m[1],m[2],m[3],m[4],m[5]);
  return String(buf);
}
static String apSsid() {
  uint8_t m[6]; WiFi.macAddress(m);
  char buf[24]; snprintf(buf, sizeof(buf), "OAT-CWSI-%02X%02X%02X", m[3],m[4],m[5]);
  return String(buf);
}
static String isoNowUTC() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";
  struct tm t; gmtime_r(&now, &t);
  char buf[24]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}
static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron"; case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic"; case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep"; 
    case ESP_RST_SDIO: return "sdio"; case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag"; case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch"; case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "other";
  }
}

void cfgLoad() {
  prefs.begin("oatcwsi", true);
  cfg.device_id   = macFromChip();
  cfg.device_name = prefs.getString("dev_name", "");
  cfg.location    = prefs.getString("location", "");
  cfg.stream_id   = prefs.getString("stream",   "canopy");
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
  cfg.sda_pin     = prefs.getInt   ("sda",      DEFAULT_SDA);
  cfg.scl_pin     = prefs.getInt   ("scl",      DEFAULT_SCL);
  cfg.ntp_server  = prefs.getString("ntp",      "pool.ntp.org");
  cfg.admin_pw    = prefs.getString("admin_pw", DEFAULT_ADMIN_PW);
  prefs.end();
  if (cfg.interval_s < 10) cfg.interval_s = 10;
}
void cfgSave() {
  prefs.begin("oatcwsi", false);
  prefs.putString("dev_name", cfg.device_name); prefs.putString("location", cfg.location);
  prefs.putString("stream", cfg.stream_id);
  prefs.putString("ssid", cfg.ssid); prefs.putString("wifi_pw", cfg.wifi_pw);
  prefs.putString("method", cfg.method); prefs.putString("ep_url", cfg.ep_url); prefs.putString("ep_auth", cfg.ep_auth);
  prefs.putString("mq_host", cfg.mq_host); prefs.putUShort("mq_port", cfg.mq_port);
  prefs.putString("mq_user", cfg.mq_user); prefs.putString("mq_pw", cfg.mq_pw);
  prefs.putString("mq_topic", cfg.mq_topic); prefs.putBool("mq_tls", cfg.mq_tls);
  prefs.putULong("interval", cfg.interval_s); prefs.putInt("sda", cfg.sda_pin); prefs.putInt("scl", cfg.scl_pin);
  prefs.putString("ntp", cfg.ntp_server); prefs.putString("admin_pw", cfg.admin_pw);
  prefs.end();
}

// ---- delivery (shared oat-ods pattern) -------------------------------------
static oat::Device oatDevice() {
  oat::Device d;
  d.gateway_id = cfg.device_name.length() ? cfg.device_name.c_str() : cfg.device_id.c_str();
  d.device_id  = cfg.device_id.c_str();   // hardware provenance (efuse MAC) — rides the status message only
  d.tier = FW_NAME; d.fw = FW_VERSION;
  return d;
}
bool pushWebhook(const String &payload) {
  if (cfg.ep_url.length() == 0) { lastPushMsg = "no webhook URL set"; return false; }
  bool https = cfg.ep_url.startsWith("https");
  HTTPClient http;
  bool began = https ? (netTls.setInsecure(), http.begin(netTls, cfg.ep_url)) : http.begin(netPlain, cfg.ep_url);
  if (!began) { lastPushMsg = "http.begin failed"; return false; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", FW_VERSION);
  if (cfg.ep_auth.length()) http.addHeader("Authorization", cfg.ep_auth);
  int code = http.POST(payload); http.end();
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
  oat::Health hw; String willMsg; oat::encodeStatus(dev, "offline", "", hw, willMsg);
  String cid = cfg.device_id + "-" + String((uint32_t)millis(), HEX);
  bool ok = cfg.mq_user.length()
            ? mqtt.connect(cid.c_str(), cfg.mq_user.c_str(), cfg.mq_pw.c_str(), willTopic.c_str(), 0, true, willMsg.c_str())
            : mqtt.connect(cid.c_str(), willTopic.c_str(), 0, true, willMsg.c_str());
  if (ok) {
    oat::Health hh; hh.uptime_s=(uint32_t)(millis()/1000); hh.free_heap=ESP.getFreeHeap();
    hh.boot_count=bootCount; hh.reset=resetReasonStr(); hh.rssi=WiFi.RSSI();
    String _lan=WiFi.localIP().toString(),_ssid=WiFi.SSID(); hh.lan_ip=_lan.c_str(); hh.ssid=_ssid.c_str();
    String onlineMsg; oat::encodeStatus(dev, "online", isoNowUTC().c_str(), hh, onlineMsg);
    mqtt.publish(willTopic.c_str(), onlineMsg.c_str(), true);
  } else lastPushMsg = "MQTT connect rc=" + String(mqtt.state());
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
// Emit one measurement under the canopy stream.
bool emitOne(const oat::Device &dev, const String &iso, const char* meas, const char* unit, double val) {
  oat::Reading r;
  r.stream_id   = cfg.stream_id.c_str();
  r.location    = cfg.location.c_str();
  r.physical_id = "mlx90614";
  r.observed_at = iso.c_str();
  r.measurement = meas; r.unit = unit; r.value = roundf(val * 100) / 100.0;
  return sendMessage(dev, r);
}

void doSample() {
  if (!mlxOk) { lastReading = "MLX90614 not found"; lastPushOk = false; lastPushMs = millis(); return; }
  double canopy = mlx.readObjectTempC();
  double air    = mlx.readAmbientTempC();
  lastPushMs = millis();
  if (isnan(canopy) || isnan(air)) { lastReading = "read error (NaN)"; lastPushOk = false; return; }
  double delta = canopy - air;
  String iso = isoNowUTC();
  oat::Device dev = oatDevice();
  bool ok = false; uint32_t sent = 0;
  if (emitOne(dev, iso, "canopy_temperature", "Cel", canopy)) { ok = true; sent++; }
  if (emitOne(dev, iso, "air_temperature",    "Cel", air))    { ok = true; sent++; }
  if (emitOne(dev, iso, "canopy_air_delta",   "Cel", delta))  { ok = true; sent++; }
  lastPushOk = ok; if (ok) pushCount++;
  lastReading = "canopy " + String(canopy,1) + "C, air " + String(air,1) + "C, dT " + String(delta,1) + "C — " + lastPushMsg;
  Serial.printf("[cwsi] canopy=%.1f air=%.1f dT=%.1f sent=%u ok=%d\n", canopy, air, delta, sent, ok);
}

// ---- web UI ----------------------------------------------------------------
static const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>OAT CWSI Node</title><style>"
  "body{font-family:system-ui,Arial,sans-serif;background:#0f1413;color:#e7efe9;margin:0;padding:1rem;}"
  ".card{max-width:640px;margin:0 auto;background:#172120;border:1px solid #2a3a37;border-radius:10px;padding:1.2rem;}"
  "h1{font-size:1.2rem;margin:.2rem 0 1rem;color:#bfe3c8;}h2{font-size:.95rem;color:#8fb39a;margin:1.2rem 0 .4rem;border-bottom:1px solid #2a3a37;padding-bottom:.2rem;}"
  "label{display:block;font-size:.8rem;margin:.6rem 0 .15rem;color:#9fb8a6;}"
  "input,select{width:100%;box-sizing:border-box;padding:.5rem;border-radius:6px;border:1px solid #344743;background:#0f1413;color:#e7efe9;font-size:.9rem;}"
  ".row{display:flex;gap:.6rem;}.row>div{flex:1;}"
  ".btn{margin-top:1.2rem;width:100%;padding:.7rem;background:#2e7d54;color:#fff;border:0;border-radius:8px;font-size:1rem;cursor:pointer;}"
  ".muted{font-size:.72rem;color:#6f8a78;margin-top:.15rem;}a{color:#7fd0a0;}"
  ".brand{display:flex;align-items:center;gap:.5rem;margin-bottom:.7rem;}.bname{font-weight:600;font-size:.95rem;color:#bfe3c8;}"
  "</style></head><body><div class='card'><div class='brand'>"
#ifdef LOGO_URL
  "<img src='" LOGO_URL "' height='28' alt='' style='display:block' onerror='this.remove()'>"
#endif
  "<span class='bname'>OpenAgricultureTechnology<span style='color:#6f8a78'>.com</span></span></div>";
static const char PAGE_FOOT[] PROGMEM = "</div></body></html>";

String fieldsForm() {
  String webhookSel = (cfg.method=="webhook")?"selected":"", mqttSel=(cfg.method=="mqtt")?"selected":"", tlsChk=cfg.mq_tls?"checked":"";
  String h;
  h += "<h1>OAT CWSI Node</h1><form method='POST' action='/save'>";
  h += "<h2>Identity</h2>";
  h += "<label>Device ID</label><input value='" + cfg.device_id + "' readonly style='opacity:.7'>";
  h += "<label>Node name (gateway id)</label><input name='dev_name' value='" + cfg.device_name + "' placeholder='GH2-canopy'>";
  h += "<label>Stream id (the canopy spot)</label><input name='stream' value='" + cfg.stream_id + "' placeholder='gh2-canopy'>";
  h += "<label>Location</label><input name='location' value='" + cfg.location + "'>";
  h += "<h2>WiFi</h2>";
  h += "<label>SSID</label><input name='ssid' value='" + cfg.ssid + "'>";
  h += "<label>Password</label><input name='wifi_pw' type='password' placeholder='(unchanged)'>";
  h += "<h2>Delivery</h2>";
  h += "<label>Method</label><select name='method' id='method' onchange='tog()'><option value='webhook' "+webhookSel+">Webhook</option><option value='mqtt' "+mqttSel+">MQTT</option></select>";
  h += "<div id='wh'><label>Webhook URL</label><input name='ep_url' value='" + cfg.ep_url + "' placeholder='https://...'>";
  h += "<label>Authorization (optional)</label><input name='ep_auth' value='" + cfg.ep_auth + "' placeholder='Bearer abc123'></div>";
  h += "<div id='mq'><div class='row'><div><label>MQTT host</label><input name='mq_host' value='" + cfg.mq_host + "'></div><div style='flex:.4'><label>Port</label><input name='mq_port' value='" + String(cfg.mq_port) + "'></div></div>";
  h += "<label>Topic prefix</label><input name='mq_topic' value='" + cfg.mq_topic + "'>";
  h += "<div class='row'><div><label>Username</label><input name='mq_user' value='" + cfg.mq_user + "'></div><div><label>Password</label><input name='mq_pw' type='password' placeholder='(unchanged)'></div></div>";
  h += "<label><input type='checkbox' name='mq_tls' "+tlsChk+" style='width:auto'> Use TLS</label></div>";
  h += "<h2>Sampling</h2>";
  h += "<div class='row'><div><label>Interval (sec)</label><input name='interval' value='" + String(cfg.interval_s) + "'></div></div>";
  h += "<div class='row'><div><label>I2C SDA</label><input name='sda' value='" + String(cfg.sda_pin) + "'></div><div><label>I2C SCL</label><input name='scl' value='" + String(cfg.scl_pin) + "'></div></div>";
  h += "<label>Admin password</label><input name='admin_pw' type='password' placeholder='(unchanged)'>";
  h += "<div class='muted'>Default <code>oatsetup</code>. CWSI is derived downstream from canopy_air_delta + VPD.</div>";
  h += "<button class='btn' type='submit'>Save &amp; Reboot</button></form>";
  h += "<p class='muted' style='text-align:center'><a href='/status'>Live status &amp; read now &rarr;</a></p>";
  h += "<script>function tog(){var m=document.getElementById('method').value;document.getElementById('wh').style.display=(m=='webhook')?'block':'none';document.getElementById('mq').style.display=(m=='mqtt')?'block':'none';}tog();</script>";
  return h;
}
bool requireAuth() { if(!server.authenticate(ADMIN_USER, cfg.admin_pw.c_str())){server.requestAuthentication();return false;} return true; }
void handleRoot()  { if(!requireAuth())return; String p=FPSTR(PAGE_HEAD); p+=fieldsForm(); p+=FPSTR(PAGE_FOOT); server.send(200,"text/html",p); }
void handleSave() {
  if(!requireAuth())return;
  auto arg=[&](const char*k){return server.arg(k);};
  cfg.device_name=arg("dev_name"); cfg.stream_id=arg("stream"); cfg.location=arg("location");
  cfg.ssid=arg("ssid"); if(arg("wifi_pw").length())cfg.wifi_pw=arg("wifi_pw");
  cfg.method=arg("method"); cfg.ep_url=arg("ep_url"); cfg.ep_auth=arg("ep_auth");
  cfg.mq_host=arg("mq_host"); cfg.mq_port=(uint16_t)arg("mq_port").toInt(); cfg.mq_user=arg("mq_user");
  if(arg("mq_pw").length())cfg.mq_pw=arg("mq_pw"); cfg.mq_topic=arg("mq_topic"); cfg.mq_tls=server.hasArg("mq_tls");
  cfg.interval_s=(uint32_t)arg("interval").toInt(); if(cfg.interval_s<10)cfg.interval_s=10;
  cfg.sda_pin=(int)arg("sda").toInt(); cfg.scl_pin=(int)arg("scl").toInt();
  if(cfg.stream_id.length()==0)cfg.stream_id="canopy";
  cfg.ntp_server=cfg.ntp_server.length()?cfg.ntp_server:"pool.ntp.org";
  if(arg("admin_pw").length())cfg.admin_pw=arg("admin_pw");
  cfgSave();
  String p=FPSTR(PAGE_HEAD); p+="<h1>Saved</h1><p>Rebooting in 3 seconds&hellip;</p>"; p+=FPSTR(PAGE_FOOT);
  server.send(200,"text/html",p); delay(3000); ESP.restart();
}
void handleStatus() {
  if(!requireAuth())return;
  String p=FPSTR(PAGE_HEAD);
  p+="<h1>Live Status</h1>";
  p+="<div class='muted'>ID "+cfg.device_id+" &middot; fw "+FW_VERSION+" &middot; sensor "+String(mlxOk?"MLX90614 OK":"NOT FOUND")+"</div>";
  p+="<div class='muted'>WiFi: "+String(WiFi.isConnected()?"connected ":"down ")+WiFi.localIP().toString()+" (RSSI "+String(WiFi.RSSI())+")</div>";
  p+="<div class='muted'>Uptime "+String(millis()/1000)+"s &middot; boots "+String(bootCount)+" &middot; sent "+String(pushCount)+"</div>";
  p+="<div class='muted'>Last read: "+lastReading+"</div>";
  p+="<p style='text-align:center'><a href='/readnow'>Read now</a> &middot; <a href='/'>&larr; Setup</a></p>";
  p+=FPSTR(PAGE_FOOT); server.send(200,"text/html",p);
}
void handleReadNow(){ if(!requireAuth())return; doSample(); server.sendHeader("Location","/status"); server.send(303); }
void handleNotFound(){ server.sendHeader("Location", String("http://")+AP_IP.toString()); server.send(302,"text/plain",""); }

void startWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255,255,255,0));
  WiFi.softAP(apSsid().c_str());
  if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.wifi_pw.c_str());
  dnsServer.start(DNS_PORT, "*", AP_IP);
}

void setup() {
  Serial.begin(115200); delay(200);
  Serial.println("\n[OAT] " FW_VERSION " booting");
  cfgLoad();
  prefs.begin("oatcwsi", false); bootCount = prefs.getULong("boot_n",0)+1; prefs.putULong("boot_n",bootCount); prefs.end();
  Wire.begin(cfg.sda_pin, cfg.scl_pin);
  mlxOk = mlx.begin();
  startWiFi();
  configTime(0, 0, cfg.ntp_server.c_str());
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/readnow", HTTP_GET, handleReadNow);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("[OAT] AP %s -> http://%s  (MLX %s)\n", apSsid().c_str(), AP_IP.toString().c_str(), mlxOk?"ok":"missing");
  lastPushMs = millis();
}
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  if (cfg.method=="mqtt" && mqtt.connected()) mqtt.loop();
  static unsigned long lastWifiTry=0;
  if (cfg.ssid.length() && !WiFi.isConnected() && millis()-lastWifiTry>15000) { lastWifiTry=millis(); WiFi.begin(cfg.ssid.c_str(), cfg.wifi_pw.c_str()); }
  if (WiFi.isConnected() && millis()-lastPushMs >= cfg.interval_s*1000UL) doSample();
}
