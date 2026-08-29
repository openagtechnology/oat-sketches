/* =============================================================================
   OAT Load-Cell Water  —  Reference Node v1.0.0
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   PURPOSE
     A self-configuring ESP32 node that weighs a pot/basket on a load cell (via an
     HX711 amplifier), pushes the readings to a user endpoint as oat-ods, and runs
     a LOCAL weigh-and-water loop: water when the weight drops below a target, stop
     at a refill target or a hard pump-time cap.

     It emits two things downstream:
        weight         — grams on the cell
        transpiration  — drydown slope between samples (g/h; the plant's water use)

   NO-CLOUD-ACTUATION (the doctrine test case)
     The watering DECISION lives entirely on this device. oat-ods carries readings
     OUT for monitoring; nothing ever commands the valve from the cloud. Failure is
     designed for: the relay is OFF on boot, every watering is capped at a maximum
     pump time, and a cooldown prevents rapid re-watering. A lost network changes
     nothing about whether the plant gets watered.

   CALIBRATION (on the live-status page)
     1. Empty cell  -> "Tare (zero)".
     2. Put a known weight on -> "Set scale" with its grams. Done.

   WIRING
     HX711: DOUT + SCK to two GPIOs (default 16 / 17), plus VCC/GND; the load cell's
     four wires to the HX711. Relay/valve on the relay GPIO (default 25). The pump
     loop usually switches a 12 V solenoid or pump through the relay — separate
     supply from the ESP32.

   LIBRARIES (platformio.ini lib_deps; pinned)
     - HX711  (bogde/HX711)
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

#include <HX711.h>
#include <PubSubClient.h>
#include <oat_ods.h>          // shared OAT encoder module (firmware/lib/oat_ods)

#define FW_NAME           "oat-loadcell-water"
#define FW_SEMVER         "1.0.0"
#define FW_VERSION        "OAT-LoadCell-Water/1.0.0"
#define ADMIN_USER        "admin"
#define DEFAULT_ADMIN_PW  "oatsetup"
#define DEFAULT_DOUT      16
#define DEFAULT_SCK       17
#define DEFAULT_RELAY     25
#define LOGO_URL "https://openagriculturetechnology.com/assets/img/logo-horizontal.png"

static const byte DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);

struct Config {
  String device_id, device_name, location;
  String stream_id;        // e.g. "bench-pot-3"
  String ssid, wifi_pw;
  String method, ep_url, ep_auth;
  String mq_host; uint16_t mq_port; String mq_user, mq_pw, mq_topic; bool mq_tls;
  uint32_t interval_s;
  int dout_pin, sck_pin;
  float scale_factor;      // HX711 counts per gram
  long  offset;            // HX711 tare offset
  // local water loop
  bool   water_enable;
  int    relay_pin; bool relay_active_high;
  double target_g;         // water when weight < target
  double refill_g;         // water until weight >= refill (or pump cap)
  uint32_t pump_max_s;     // hard cap per watering (safety)
  uint32_t cooldown_s;     // minimum gap between waterings
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
HX711            scale;

unsigned long lastPushMs = 0;
bool          lastPushOk = false;
String        lastPushMsg = "no read yet";
String        lastReading = "no read yet";
uint32_t      pushCount = 0;
// transpiration baseline + watering state
double        prevW = 0; unsigned long prevMs = 0; bool prevValid = false;
unsigned long lastWaterMs = 0; uint32_t waterCount = 0; String lastWater = "never";

static String macFromChip() { uint8_t m[6]; WiFi.macAddress(m); char b[20]; snprintf(b,sizeof(b),"oat-%02x%02x%02x%02x%02x%02x",m[0],m[1],m[2],m[3],m[4],m[5]); return String(b); }
static String apSsid()      { uint8_t m[6]; WiFi.macAddress(m); char b[24]; snprintf(b,sizeof(b),"OAT-Scale-%02X%02X%02X",m[3],m[4],m[5]); return String(b); }
static String isoNowUTC()   { time_t n=time(nullptr); if(n<1700000000)return""; struct tm t; gmtime_r(&n,&t); char b[24]; strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&t); return String(b); }
static const char* resetReasonStr(){ switch(esp_reset_reason()){case ESP_RST_POWERON:return"poweron";case ESP_RST_SW:return"sw";case ESP_RST_PANIC:return"panic";case ESP_RST_BROWNOUT:return"brownout";case ESP_RST_SDIO:return"sdio";case ESP_RST_USB:return"usb";case ESP_RST_JTAG:return"jtag";case ESP_RST_EFUSE:return"efuse";case ESP_RST_PWR_GLITCH:return"pwr_glitch";case ESP_RST_CPU_LOCKUP:return"cpu_lockup";default:return"other";} }

static void relayWrite(bool on) { digitalWrite(cfg.relay_pin, cfg.relay_active_high ? (on?HIGH:LOW) : (on?LOW:HIGH)); }

void cfgLoad() {
  prefs.begin("oatscale", true);
  cfg.device_id   = macFromChip();
  cfg.device_name = prefs.getString("dev_name", "");
  cfg.location    = prefs.getString("location", "");
  cfg.stream_id   = prefs.getString("stream",   "pot");
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
  cfg.dout_pin    = prefs.getInt   ("dout",     DEFAULT_DOUT);
  cfg.sck_pin     = prefs.getInt   ("sck",      DEFAULT_SCK);
  cfg.scale_factor= prefs.getFloat ("scale",    1.0f);
  cfg.offset      = prefs.getLong  ("offset",   0);
  cfg.water_enable= prefs.getBool  ("water",    false);
  cfg.relay_pin   = prefs.getInt   ("relay",    DEFAULT_RELAY);
  cfg.relay_active_high = prefs.getBool("rhi",  true);
  cfg.target_g    = prefs.getDouble("target",   0);
  cfg.refill_g    = prefs.getDouble("refill",   0);
  cfg.pump_max_s  = prefs.getULong ("pumpmax",  20);
  cfg.cooldown_s  = prefs.getULong ("cooldown", 600);
  cfg.ntp_server  = prefs.getString("ntp",      "pool.ntp.org");
  cfg.admin_pw    = prefs.getString("admin_pw", DEFAULT_ADMIN_PW);
  prefs.end();
  if (cfg.interval_s < 10) cfg.interval_s = 10;
}
void cfgSave() {
  prefs.begin("oatscale", false);
  prefs.putString("dev_name",cfg.device_name); prefs.putString("location",cfg.location); prefs.putString("stream",cfg.stream_id);
  prefs.putString("ssid",cfg.ssid); prefs.putString("wifi_pw",cfg.wifi_pw);
  prefs.putString("method",cfg.method); prefs.putString("ep_url",cfg.ep_url); prefs.putString("ep_auth",cfg.ep_auth);
  prefs.putString("mq_host",cfg.mq_host); prefs.putUShort("mq_port",cfg.mq_port); prefs.putString("mq_user",cfg.mq_user);
  prefs.putString("mq_pw",cfg.mq_pw); prefs.putString("mq_topic",cfg.mq_topic); prefs.putBool("mq_tls",cfg.mq_tls);
  prefs.putULong("interval",cfg.interval_s); prefs.putInt("dout",cfg.dout_pin); prefs.putInt("sck",cfg.sck_pin);
  prefs.putFloat("scale",cfg.scale_factor); prefs.putLong("offset",cfg.offset);
  prefs.putBool("water",cfg.water_enable); prefs.putInt("relay",cfg.relay_pin); prefs.putBool("rhi",cfg.relay_active_high);
  prefs.putDouble("target",cfg.target_g); prefs.putDouble("refill",cfg.refill_g);
  prefs.putULong("pumpmax",cfg.pump_max_s); prefs.putULong("cooldown",cfg.cooldown_s);
  prefs.putString("ntp",cfg.ntp_server); prefs.putString("admin_pw",cfg.admin_pw);
  prefs.end();
}

// ---- delivery (shared oat-ods pattern) -------------------------------------
static oat::Device oatDevice() {
  oat::Device d;
  d.gateway_id = cfg.device_name.length() ? cfg.device_name.c_str() : cfg.device_id.c_str();
  d.device_id  = cfg.device_id.c_str();   // hardware provenance (efuse MAC) — rides the status message only
  d.tier = FW_NAME; d.fw = FW_VERSION; return d;
}
bool pushWebhook(const String &payload) {
  if (cfg.ep_url.length()==0){lastPushMsg="no webhook URL set";return false;}
  bool https=cfg.ep_url.startsWith("https"); HTTPClient http;
  bool began = https ? (netTls.setInsecure(), http.begin(netTls,cfg.ep_url)) : http.begin(netPlain,cfg.ep_url);
  if(!began){lastPushMsg="http.begin failed";return false;}
  http.addHeader("Content-Type","application/json"); http.addHeader("User-Agent",FW_VERSION);
  if(cfg.ep_auth.length()) http.addHeader("Authorization",cfg.ep_auth);
  int code=http.POST(payload); http.end(); lastPushMsg="HTTP "+String(code);
  return code>=200&&code<300;
}
bool ensureMqtt() {
  if(cfg.mq_host.length()==0){lastPushMsg="no MQTT host set";return false;}
  mqtt.setClient(cfg.mq_tls?(Client&)netTls:(Client&)netPlain); if(cfg.mq_tls)netTls.setInsecure();
  mqtt.setServer(cfg.mq_host.c_str(),cfg.mq_port); mqtt.setBufferSize(2048);
  if(mqtt.connected())return true;
  oat::Device dev=oatDevice(); String willTopic=oat::statusTopic(cfg.mq_topic.c_str(),dev);
  oat::Health hw; String willMsg; oat::encodeStatus(dev,"offline","",hw,willMsg);
  String cid=cfg.device_id+"-"+String((uint32_t)millis(),HEX);
  bool ok=cfg.mq_user.length()
          ? mqtt.connect(cid.c_str(),cfg.mq_user.c_str(),cfg.mq_pw.c_str(),willTopic.c_str(),0,true,willMsg.c_str())
          : mqtt.connect(cid.c_str(),willTopic.c_str(),0,true,willMsg.c_str());
  if(ok){ oat::Health hh; hh.uptime_s=(uint32_t)(millis()/1000); hh.free_heap=ESP.getFreeHeap(); hh.boot_count=bootCount; hh.reset=resetReasonStr(); hh.rssi=WiFi.RSSI();
    String _lan=WiFi.localIP().toString(),_ssid=WiFi.SSID(); hh.lan_ip=_lan.c_str(); hh.ssid=_ssid.c_str();
    String om; oat::encodeStatus(dev,"online",isoNowUTC().c_str(),hh,om); mqtt.publish(willTopic.c_str(),om.c_str(),true); }
  else lastPushMsg="MQTT connect rc="+String(mqtt.state());
  return ok;
}
bool mqttPublish(const String &topic,const String &payload){ if(!ensureMqtt())return false; bool ok=mqtt.publish(topic.c_str(),payload.c_str(),true); lastPushMsg=ok?("published "+topic):"mqtt publish failed"; return ok; }
bool sendMessage(const oat::Device &dev,const oat::Reading &r){ String p; oat::encodeNative(dev,r,p); if(cfg.method=="mqtt")return mqttPublish(oat::mqttTopic(cfg.mq_topic.c_str(),dev,r),p); return pushWebhook(p); }
bool emitOne(const oat::Device &dev,const String &iso,const char* meas,const char* unit,double val){
  oat::Reading r; r.stream_id=cfg.stream_id.c_str(); r.location=cfg.location.c_str(); r.physical_id="loadcell";
  r.observed_at=iso.c_str(); r.measurement=meas; r.unit=unit; r.value=roundf(val*100)/100.0;
  return sendMessage(dev,r);
}

static bool calibrated() { return cfg.scale_factor != 0 && cfg.scale_factor != 1.0f; }
static double readGrams() { if(!scale.is_ready()) return NAN; return scale.get_units(5); }

// ---- read + emit (no actuation here) ---------------------------------------
void doSample() {
  lastPushMs = millis();
  double w = readGrams();
  if (isnan(w)) { lastReading = "HX711 not ready"; lastPushOk = false; return; }
  String iso = isoNowUTC();
  oat::Device dev = oatDevice();
  bool ok=false;
  if (emitOne(dev, iso, "weight", "g", w)) { ok=true; pushCount++; }
  // transpiration: drydown slope since the last sample, only across a non-watered gap
  if (prevValid) {
    double dtH = (millis() - prevMs) / 3600000.0;
    if (dtH > 0) { double slope = (prevW - w) / dtH; emitOne(dev, iso, "transpiration", "g/h", slope); }
  }
  prevW = w; prevMs = millis(); prevValid = true;
  lastPushOk = ok;
  lastReading = String(w,1) + " g — " + lastPushMsg;
  Serial.printf("[scale] weight=%.1f g ok=%d\n", w, ok);
}

// ---- LOCAL weigh-and-water loop (no cloud, fail-safe) ----------------------
void maybeWater() {
  if (!cfg.water_enable || !calibrated()) return;
  if (cfg.target_g <= 0) return;                                  // not configured
  if (millis() - lastWaterMs < cfg.cooldown_s * 1000UL) return;   // cooldown (and boot grace)
  double w = readGrams();
  if (isnan(w) || w >= cfg.target_g) return;                      // not dry, or no valid reading

  Serial.printf("[water] %.1f g < target %.1f g — watering (max %us)\n", w, cfg.target_g, cfg.pump_max_s);
  relayWrite(true);
  uint32_t t0 = millis();
  while (millis() - t0 < cfg.pump_max_s * 1000UL) {               // HARD safety cap
    delay(500);
    double cur = readGrams();
    if (!isnan(cur) && cfg.refill_g > 0 && cur >= cfg.refill_g) break;   // reached refill target
  }
  relayWrite(false);                                              // ALWAYS off after
  lastWaterMs = millis();
  waterCount++;
  prevValid = false;                                              // re-baseline transpiration after a refill
  lastWater = isoNowUTC();
  Serial.println("[water] done (relay off)");
}

// ---- web UI ----------------------------------------------------------------
static const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>OAT Load-Cell Water</title><style>"
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
  String webhookSel=(cfg.method=="webhook")?"selected":"", mqttSel=(cfg.method=="mqtt")?"selected":"", tlsChk=cfg.mq_tls?"checked":"";
  String waterChk=cfg.water_enable?"checked":"", rhiChk=cfg.relay_active_high?"checked":"";
  String h;
  h += "<h1>OAT Load-Cell Water</h1><form method='POST' action='/save'>";
  h += "<h2>Identity</h2>";
  h += "<label>Device ID</label><input value='"+cfg.device_id+"' readonly style='opacity:.7'>";
  h += "<label>Node name</label><input name='dev_name' value='"+cfg.device_name+"' placeholder='bench-scale-1'>";
  h += "<label>Stream id (the pot/basket)</label><input name='stream' value='"+cfg.stream_id+"' placeholder='bench-pot-3'>";
  h += "<label>Location</label><input name='location' value='"+cfg.location+"'>";
  h += "<h2>WiFi</h2>";
  h += "<label>SSID</label><input name='ssid' value='"+cfg.ssid+"'>";
  h += "<label>Password</label><input name='wifi_pw' type='password' placeholder='(unchanged)'>";
  h += "<h2>Delivery</h2>";
  h += "<label>Method</label><select name='method' id='method' onchange='tog()'><option value='webhook' "+webhookSel+">Webhook</option><option value='mqtt' "+mqttSel+">MQTT</option></select>";
  h += "<div id='wh'><label>Webhook URL</label><input name='ep_url' value='"+cfg.ep_url+"' placeholder='https://...'><label>Authorization (optional)</label><input name='ep_auth' value='"+cfg.ep_auth+"'></div>";
  h += "<div id='mq'><div class='row'><div><label>MQTT host</label><input name='mq_host' value='"+cfg.mq_host+"'></div><div style='flex:.4'><label>Port</label><input name='mq_port' value='"+String(cfg.mq_port)+"'></div></div>";
  h += "<label>Topic prefix</label><input name='mq_topic' value='"+cfg.mq_topic+"'><div class='row'><div><label>Username</label><input name='mq_user' value='"+cfg.mq_user+"'></div><div><label>Password</label><input name='mq_pw' type='password' placeholder='(unchanged)'></div></div>";
  h += "<label><input type='checkbox' name='mq_tls' "+tlsChk+" style='width:auto'> Use TLS</label></div>";
  h += "<h2>Load cell</h2>";
  h += "<div class='row'><div><label>HX711 DOUT</label><input name='dout' value='"+String(cfg.dout_pin)+"'></div><div><label>HX711 SCK</label><input name='sck' value='"+String(cfg.sck_pin)+"'></div></div>";
  h += "<div class='row'><div><label>Interval (sec)</label><input name='interval' value='"+String(cfg.interval_s)+"'></div></div>";
  h += "<div class='muted'>Calibrate the scale on the <a href='/status'>status page</a> (tare, then set with a known weight).</div>";
  h += "<h2>Watering (local loop)</h2>";
  h += "<label><input type='checkbox' name='water' "+waterChk+" style='width:auto'> Enable local watering</label>";
  h += "<div class='row'><div><label>Relay GPIO</label><input name='relay' value='"+String(cfg.relay_pin)+"'></div><div><label><input type='checkbox' name='rhi' "+rhiChk+" style='width:auto'> Active high</label></div></div>";
  h += "<div class='row'><div><label>Water below (g)</label><input name='target' value='"+String(cfg.target_g,0)+"'></div><div><label>Refill to (g)</label><input name='refill' value='"+String(cfg.refill_g,0)+"'></div></div>";
  h += "<div class='row'><div><label>Pump max (sec)</label><input name='pumpmax' value='"+String(cfg.pump_max_s)+"'></div><div><label>Cooldown (sec)</label><input name='cooldown' value='"+String(cfg.cooldown_s)+"'></div></div>";
  h += "<div class='muted'>Local loop: waters when below target, stops at refill or the pump-max cap (a stuck reading can't flood). Decision is on-device &mdash; no cloud.</div>";
  h += "<h2>Advanced</h2>";
  h += "<label>NTP server</label><input name='ntp' value='"+cfg.ntp_server+"'>";
  h += "<label>Admin password</label><input name='admin_pw' type='password' placeholder='(unchanged)'>";
  h += "<button class='btn' type='submit'>Save &amp; Reboot</button></form>";
  h += "<p class='muted' style='text-align:center'><a href='/status'>Live status, calibration &amp; read now &rarr;</a></p>";
  h += "<script>function tog(){var m=document.getElementById('method').value;document.getElementById('wh').style.display=(m=='webhook')?'block':'none';document.getElementById('mq').style.display=(m=='mqtt')?'block':'none';}tog();</script>";
  return h;
}
bool requireAuth(){ if(!server.authenticate(ADMIN_USER,cfg.admin_pw.c_str())){server.requestAuthentication();return false;} return true; }
void handleRoot(){ if(!requireAuth())return; String p=FPSTR(PAGE_HEAD); p+=fieldsForm(); p+=FPSTR(PAGE_FOOT); server.send(200,"text/html",p); }
void handleSave(){
  if(!requireAuth())return; auto arg=[&](const char*k){return server.arg(k);};
  cfg.device_name=arg("dev_name"); cfg.stream_id=arg("stream"); cfg.location=arg("location");
  cfg.ssid=arg("ssid"); if(arg("wifi_pw").length())cfg.wifi_pw=arg("wifi_pw");
  cfg.method=arg("method"); cfg.ep_url=arg("ep_url"); cfg.ep_auth=arg("ep_auth");
  cfg.mq_host=arg("mq_host"); cfg.mq_port=(uint16_t)arg("mq_port").toInt(); cfg.mq_user=arg("mq_user");
  if(arg("mq_pw").length())cfg.mq_pw=arg("mq_pw"); cfg.mq_topic=arg("mq_topic"); cfg.mq_tls=server.hasArg("mq_tls");
  cfg.interval_s=(uint32_t)arg("interval").toInt(); if(cfg.interval_s<10)cfg.interval_s=10;
  cfg.dout_pin=(int)arg("dout").toInt(); cfg.sck_pin=(int)arg("sck").toInt();
  cfg.water_enable=server.hasArg("water"); cfg.relay_pin=(int)arg("relay").toInt(); cfg.relay_active_high=server.hasArg("rhi");
  cfg.target_g=arg("target").toFloat(); cfg.refill_g=arg("refill").toFloat();
  cfg.pump_max_s=(uint32_t)arg("pumpmax").toInt(); cfg.cooldown_s=(uint32_t)arg("cooldown").toInt();
  if(cfg.stream_id.length()==0)cfg.stream_id="pot";
  cfg.ntp_server=arg("ntp"); if(cfg.ntp_server.length()==0)cfg.ntp_server="pool.ntp.org";
  if(arg("admin_pw").length())cfg.admin_pw=arg("admin_pw");
  cfgSave();
  String p=FPSTR(PAGE_HEAD); p+="<h1>Saved</h1><p>Rebooting in 3 seconds&hellip;</p>"; p+=FPSTR(PAGE_FOOT);
  server.send(200,"text/html",p); delay(3000); ESP.restart();
}
void handleStatus(){
  if(!requireAuth())return;
  double w = readGrams();
  String p=FPSTR(PAGE_HEAD);
  p+="<h1>Live Status</h1>";
  p+="<div class='muted'>ID "+cfg.device_id+" &middot; fw "+FW_VERSION+" &middot; "+String(calibrated()?"calibrated":"NOT calibrated")+"</div>";
  p+="<div class='muted'>WiFi: "+String(WiFi.isConnected()?"connected ":"down ")+WiFi.localIP().toString()+"</div>";
  p+="<div class='muted'>Weight now: "+ (isnan(w)?String("(HX711 not ready)"):String(w,1)+" g") +" &middot; scale "+String(cfg.scale_factor,3)+"</div>";
  p+="<div class='muted'>Watering: "+String(cfg.water_enable?"ENABLED":"off")+" &middot; below "+String(cfg.target_g,0)+"g, refill "+String(cfg.refill_g,0)+"g &middot; waterings "+String(waterCount)+" &middot; last "+lastWater+"</div>";
  p+="<div class='muted'>Last read: "+lastReading+"</div>";
  p+="<h2>Calibrate</h2>";
  p+="<p class='muted'>1) Empty the cell, then Tare. 2) Put a known weight on, enter its grams, Set scale.</p>";
  p+="<p><a class='btn' style='display:block;text-align:center;text-decoration:none' href='/tare'>Tare (zero)</a></p>";
  p+="<form method='get' action='/calibrate'><label>Known weight on cell (g)</label><input name='g' value='1000'><button class='btn' type='submit'>Set scale</button></form>";
  p+="<p style='text-align:center'><a href='/readnow'>Read &amp; send now</a> &middot; <a href='/'>&larr; Setup</a></p>";
  p+=FPSTR(PAGE_FOOT); server.send(200,"text/html",p);
}
void handleTare(){
  if(!requireAuth())return;
  scale.tare(15); cfg.offset = scale.get_offset(); cfgSave();
  server.sendHeader("Location","/status"); server.send(303);
}
void handleCalibrate(){
  if(!requireAuth())return;
  double g = server.arg("g").toFloat();
  if (g > 0) {
    long raw = scale.read_average(15);
    float factor = (float)((raw - cfg.offset) / g);
    if (factor != 0) { cfg.scale_factor = factor; scale.set_scale(cfg.scale_factor); cfgSave(); }
  }
  server.sendHeader("Location","/status"); server.send(303);
}
void handleReadNow(){ if(!requireAuth())return; doSample(); server.sendHeader("Location","/status"); server.send(303); }
void handleNotFound(){ server.sendHeader("Location", String("http://")+AP_IP.toString()); server.send(302,"text/plain",""); }

void startWiFi(){
  WiFi.mode(WIFI_AP_STA); WiFi.softAPConfig(AP_IP,AP_IP,IPAddress(255,255,255,0)); WiFi.softAP(apSsid().c_str());
  if(cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(),cfg.wifi_pw.c_str());
  dnsServer.start(DNS_PORT,"*",AP_IP);
}
void setup(){
  Serial.begin(115200); delay(200);
  Serial.println("\n[OAT] " FW_VERSION " booting");
  cfgLoad();
  prefs.begin("oatscale",false); bootCount=prefs.getULong("boot_n",0)+1; prefs.putULong("boot_n",bootCount); prefs.end();
  pinMode(cfg.relay_pin, OUTPUT); relayWrite(false);          // fail-safe: valve closed on boot
  scale.begin(cfg.dout_pin, cfg.sck_pin);
  scale.set_scale(cfg.scale_factor); scale.set_offset(cfg.offset);
  startWiFi();
  configTime(0,0,cfg.ntp_server.c_str());
  server.on("/",HTTP_GET,handleRoot);
  server.on("/save",HTTP_POST,handleSave);
  server.on("/status",HTTP_GET,handleStatus);
  server.on("/tare",HTTP_GET,handleTare);
  server.on("/calibrate",HTTP_GET,handleCalibrate);
  server.on("/readnow",HTTP_GET,handleReadNow);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("[OAT] AP %s -> http://%s  (relay GPIO %d, watering %s)\n", apSsid().c_str(), AP_IP.toString().c_str(), cfg.relay_pin, cfg.water_enable?"on":"off");
  lastPushMs = millis();
}
void loop(){
  dnsServer.processNextRequest();
  server.handleClient();
  if(cfg.method=="mqtt" && mqtt.connected()) mqtt.loop();
  static unsigned long lastWifiTry=0;
  if(cfg.ssid.length() && !WiFi.isConnected() && millis()-lastWifiTry>15000){lastWifiTry=millis(); WiFi.begin(cfg.ssid.c_str(),cfg.wifi_pw.c_str());}
  if(millis()-lastPushMs >= cfg.interval_s*1000UL){
    if (WiFi.isConnected()) doSample();   // emit readings (needs network)
    maybeWater();                         // LOCAL loop — runs even with no network
  }
}
