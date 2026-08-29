/* =============================================================================
   OAT Analog Reader  —  Reference Node v1.0.0
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   PURPOSE
     A self-configuring ESP32 node that reads analog sensors through an ADS1115
     (a clean 16-bit I2C ADC), applies a per-channel linear calibration, and
     pushes each as oat-ods, by webhook or MQTT.

       value = m * volts + b

     That one linear map covers amplified 0-2.5 V / 0-5 V outputs AND 4-20 mA
     (with a sense resistor turning current into a voltage) — pick m and b to
     match the sensor's range. Example: Apogee SQ-512 is 0-2.5 V = 0-2000 umol,
     so m = 800, b = 0.

   THE CHANNEL MAP (one line per ADS1115 channel)
         <ch> <stream_id> <measure>:<unit>:<m>:<b>
     e.g.  0 canopy-par par:umol/m2/s:800:0
           1 tank       level:%:41.667:-25       (4-20mA via 150 ohm: 0.6-3.0 V -> 0-100 %)

   WIRING
     ADS1115 is I2C (3.3 V): SDA, SCL, 3V3, GND, addr 0x48 default. Sensor output
     to A0-A3. For 0-5 V use a 2:1 divider first; for 4-20 mA put a precision
     resistor across the loop (150 ohm -> ~0.6-3.0 V). Set the ADS gain to fit the
     voltage you feed it.

   LIBRARIES (platformio.ini lib_deps; pinned)
     - Adafruit ADS1X15
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

#include <Adafruit_ADS1X15.h>
#include <PubSubClient.h>
#include <oat_ods.h>          // shared OAT encoder module (firmware/lib/oat_ods)

#define FW_NAME           "oat-analog-reader"
#define FW_SEMVER         "1.0.0"
#define FW_VERSION        "OAT-Analog-Reader/1.0.0"
#define ADMIN_USER        "admin"
#define DEFAULT_ADMIN_PW  "oatsetup"
#define MAX_CH            4
#define DEFAULT_SDA       21
#define DEFAULT_SCL       22
#define LOGO_URL "https://openagriculturetechnology.com/assets/img/logo-horizontal.png"

static const byte DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);

struct Config {
  String device_id, device_name, location;
  String ssid, wifi_pw;
  String method, ep_url, ep_auth;
  String mq_host; uint16_t mq_port; String mq_user, mq_pw, mq_topic; bool mq_tls;
  uint32_t interval_s;
  int sda_pin, scl_pin, gain;     // gain index 0..5
  String channel_map;
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
Adafruit_ADS1115 ads;
bool adsOk = false;

struct Chan { int ch; char stream_id[28]; char meas[16]; char unit[12]; float m; float b; };
Chan chans[MAX_CH];
int  nChans = 0;

unsigned long lastPushMs = 0;
bool          lastPushOk = false;
String        lastPushMsg = "no read yet";
String        lastScan = "no read yet";
uint32_t      pushCount = 0;

static const char* GAIN_LABEL[] = {"+-6.144V","+-4.096V","+-2.048V","+-1.024V","+-0.512V","+-0.256V"};
adsGain_t gainFromIndex(int g) {
  switch (g) { case 0:return GAIN_TWOTHIRDS; case 2:return GAIN_TWO; case 3:return GAIN_FOUR;
               case 4:return GAIN_EIGHT; case 5:return GAIN_SIXTEEN; default:return GAIN_ONE; }
}

static String macFromChip(){ uint8_t m[6]; WiFi.macAddress(m); char b[20]; snprintf(b,sizeof(b),"oat-%02x%02x%02x%02x%02x%02x",m[0],m[1],m[2],m[3],m[4],m[5]); return String(b); }
static String apSsid()     { uint8_t m[6]; WiFi.macAddress(m); char b[24]; snprintf(b,sizeof(b),"OAT-Analog-%02X%02X%02X",m[3],m[4],m[5]); return String(b); }
static String isoNowUTC()  { time_t n=time(nullptr); if(n<1700000000)return""; struct tm t; gmtime_r(&n,&t); char b[24]; strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&t); return String(b); }
static const char* resetReasonStr(){ switch(esp_reset_reason()){case ESP_RST_POWERON:return"poweron";case ESP_RST_SW:return"sw";case ESP_RST_PANIC:return"panic";case ESP_RST_BROWNOUT:return"brownout";case ESP_RST_SDIO:return"sdio";case ESP_RST_USB:return"usb";case ESP_RST_JTAG:return"jtag";case ESP_RST_EFUSE:return"efuse";case ESP_RST_PWR_GLITCH:return"pwr_glitch";case ESP_RST_CPU_LOCKUP:return"cpu_lockup";default:return"other";} }

void cfgLoad() {
  prefs.begin("oatadc", true);
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
  cfg.sda_pin     = prefs.getInt   ("sda",      DEFAULT_SDA);
  cfg.scl_pin     = prefs.getInt   ("scl",      DEFAULT_SCL);
  cfg.gain        = prefs.getInt   ("gain",     1);
  cfg.channel_map = prefs.getString("map",      "");
  cfg.ntp_server  = prefs.getString("ntp",      "pool.ntp.org");
  cfg.admin_pw    = prefs.getString("admin_pw", DEFAULT_ADMIN_PW);
  prefs.end();
  if (cfg.interval_s < 10) cfg.interval_s = 10;
  if (cfg.gain < 0 || cfg.gain > 5) cfg.gain = 1;
}
void cfgSave() {
  prefs.begin("oatadc", false);
  prefs.putString("dev_name",cfg.device_name); prefs.putString("location",cfg.location);
  prefs.putString("ssid",cfg.ssid); prefs.putString("wifi_pw",cfg.wifi_pw);
  prefs.putString("method",cfg.method); prefs.putString("ep_url",cfg.ep_url); prefs.putString("ep_auth",cfg.ep_auth);
  prefs.putString("mq_host",cfg.mq_host); prefs.putUShort("mq_port",cfg.mq_port); prefs.putString("mq_user",cfg.mq_user);
  prefs.putString("mq_pw",cfg.mq_pw); prefs.putString("mq_topic",cfg.mq_topic); prefs.putBool("mq_tls",cfg.mq_tls);
  prefs.putULong("interval",cfg.interval_s); prefs.putInt("sda",cfg.sda_pin); prefs.putInt("scl",cfg.scl_pin); prefs.putInt("gain",cfg.gain);
  prefs.putString("map",cfg.channel_map); prefs.putString("ntp",cfg.ntp_server); prefs.putString("admin_pw",cfg.admin_pw);
  prefs.end();
}

// channel map: "ch stream meas:unit:m:b"
void parseMap() {
  nChans = 0; const String &s = cfg.channel_map; int start = 0;
  while (start < (int)s.length() && nChans < MAX_CH) {
    int nl = s.indexOf('\n', start);
    String line = (nl < 0) ? s.substring(start) : s.substring(start, nl);
    start = (nl < 0) ? s.length() : nl + 1; line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    int sp1 = line.indexOf(' '); if (sp1 < 0) continue;
    int sp2 = line.indexOf(' ', sp1 + 1); if (sp2 < 0) continue;
    String chT = line.substring(0, sp1);          chT.trim();
    String stream = line.substring(sp1 + 1, sp2); stream.trim();
    String t = line.substring(sp2 + 1);           t.trim();   // meas:unit:m:b
    if (!chT.length() || !stream.length() || !t.length()) continue;
    int c1 = t.indexOf(':'); int c2 = (c1<0)?-1:t.indexOf(':', c1+1); int c3 = (c2<0)?-1:t.indexOf(':', c2+1);
    if (c1 < 0 || c2 < 0 || c3 < 0) continue;      // meas:unit:m:b
    Chan &C = chans[nChans];
    C.ch = chT.toInt();
    String m_ = t.substring(0, c1);  strncpy(C.meas, m_.c_str(), 15); C.meas[15] = 0;
    String u_ = t.substring(c1+1, c2); strncpy(C.unit, u_.c_str(), 11); C.unit[11] = 0;
    C.m = t.substring(c2+1, c3).toFloat();
    C.b = t.substring(c3+1).toFloat();
    nChans++;
  }
}

// ---- delivery (shared oat-ods pattern) -------------------------------------
static oat::Device oatDevice(){ oat::Device d; d.gateway_id=cfg.device_name.length()?cfg.device_name.c_str():cfg.device_id.c_str(); d.device_id=cfg.device_id.c_str(); d.tier=FW_NAME; d.fw=FW_VERSION; return d; }
bool pushWebhook(const String &payload){
  if(cfg.ep_url.length()==0){lastPushMsg="no webhook URL set";return false;}
  bool https=cfg.ep_url.startsWith("https"); HTTPClient http;
  bool began=https?(netTls.setInsecure(),http.begin(netTls,cfg.ep_url)):http.begin(netPlain,cfg.ep_url);
  if(!began){lastPushMsg="http.begin failed";return false;}
  http.addHeader("Content-Type","application/json"); http.addHeader("User-Agent",FW_VERSION);
  if(cfg.ep_auth.length())http.addHeader("Authorization",cfg.ep_auth);
  int code=http.POST(payload); http.end(); lastPushMsg="HTTP "+String(code); return code>=200&&code<300;
}
bool ensureMqtt(){
  if(cfg.mq_host.length()==0){lastPushMsg="no MQTT host set";return false;}
  mqtt.setClient(cfg.mq_tls?(Client&)netTls:(Client&)netPlain); if(cfg.mq_tls)netTls.setInsecure();
  mqtt.setServer(cfg.mq_host.c_str(),cfg.mq_port); mqtt.setBufferSize(2048);
  if(mqtt.connected())return true;
  oat::Device dev=oatDevice(); String wt=oat::statusTopic(cfg.mq_topic.c_str(),dev);
  oat::Health hw; String wm; oat::encodeStatus(dev,"offline","",hw,wm);
  String cid=cfg.device_id+"-"+String((uint32_t)millis(),HEX);
  bool ok=cfg.mq_user.length()?mqtt.connect(cid.c_str(),cfg.mq_user.c_str(),cfg.mq_pw.c_str(),wt.c_str(),0,true,wm.c_str())
                              :mqtt.connect(cid.c_str(),wt.c_str(),0,true,wm.c_str());
  if(ok){ oat::Health hh; hh.uptime_s=(uint32_t)(millis()/1000); hh.free_heap=ESP.getFreeHeap(); hh.boot_count=bootCount; hh.reset=resetReasonStr(); hh.rssi=WiFi.RSSI();
    String _lan=WiFi.localIP().toString(),_ssid=WiFi.SSID(); hh.lan_ip=_lan.c_str(); hh.ssid=_ssid.c_str();
    String om; oat::encodeStatus(dev,"online",isoNowUTC().c_str(),hh,om); mqtt.publish(wt.c_str(),om.c_str(),true); }
  else lastPushMsg="MQTT connect rc="+String(mqtt.state());
  return ok;
}
bool mqttPublish(const String &topic,const String &payload){ if(!ensureMqtt())return false; bool ok=mqtt.publish(topic.c_str(),payload.c_str(),true); lastPushMsg=ok?("published "+topic):"mqtt publish failed"; return ok; }
bool sendMessage(const oat::Device &dev,const oat::Reading &r){ String p; oat::encodeNative(dev,r,p); if(cfg.method=="mqtt")return mqttPublish(oat::mqttTopic(cfg.mq_topic.c_str(),dev,r),p); return pushWebhook(p); }

void doPoll() {
  if (!adsOk) { lastScan = "ADS1115 not found"; lastPushOk = false; lastPushMs = millis(); return; }
  String iso = isoNowUTC(); oat::Device dev = oatDevice();
  uint32_t sent = 0; bool anyOk = false;
  for (int i = 0; i < nChans; i++) {
    Chan &C = chans[i];
    if (C.ch < 0 || C.ch > 3) continue;
    int16_t raw = ads.readADC_SingleEnded(C.ch);
    float volts = ads.computeVolts(raw);
    double val = (double)C.m * volts + C.b;
    char pid[16]; snprintf(pid, sizeof(pid), "ads1115:%d", C.ch);
    oat::Reading r;
    r.stream_id = C.stream_id; r.location = cfg.location.c_str(); r.physical_id = pid;
    r.observed_at = iso.c_str(); r.measurement = C.meas; if (C.unit[0]) r.unit = C.unit;
    r.value = roundf(val * 1000) / 1000.0;
    if (sendMessage(dev, r)) { anyOk = true; sent++; }
  }
  lastPushOk = anyOk; lastPushMs = millis(); if (anyOk) pushCount++;
  lastScan = String(nChans) + " channel(s), " + String(sent) + " msg(s) — " + lastPushMsg;
  Serial.printf("[analog] channels=%d sent=%u ok=%d\n", nChans, sent, anyOk);
}

// ---- web UI ----------------------------------------------------------------
static const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>OAT Analog Reader</title><style>"
  "body{font-family:system-ui,Arial,sans-serif;background:#0f1413;color:#e7efe9;margin:0;padding:1rem;}"
  ".card{max-width:640px;margin:0 auto;background:#172120;border:1px solid #2a3a37;border-radius:10px;padding:1.2rem;}"
  "h1{font-size:1.2rem;margin:.2rem 0 1rem;color:#bfe3c8;}h2{font-size:.95rem;color:#8fb39a;margin:1.2rem 0 .4rem;border-bottom:1px solid #2a3a37;padding-bottom:.2rem;}"
  "label{display:block;font-size:.8rem;margin:.6rem 0 .15rem;color:#9fb8a6;}"
  "input,select,textarea{width:100%;box-sizing:border-box;padding:.5rem;border-radius:6px;border:1px solid #344743;background:#0f1413;color:#e7efe9;font-size:.9rem;font-family:inherit;}"
  "textarea{min-height:4.5rem;font-family:ui-monospace,monospace;font-size:.82rem;}"
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
  String h;
  h += "<h1>OAT Analog Reader</h1><form method='POST' action='/save'>";
  h += "<h2>Identity</h2>";
  h += "<label>Device ID</label><input value='"+cfg.device_id+"' readonly style='opacity:.7'>";
  h += "<label>Node name (gateway id)</label><input name='dev_name' value='"+cfg.device_name+"'>";
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
  h += "<h2>ADS1115</h2>";
  h += "<div class='row'><div><label>I2C SDA</label><input name='sda' value='"+String(cfg.sda_pin)+"'></div><div><label>I2C SCL</label><input name='scl' value='"+String(cfg.scl_pin)+"'></div></div>";
  h += "<label>Gain (full scale)</label><select name='gain'>";
  for (int g=0; g<6; g++) { h += "<option value='"+String(g)+"'"+(cfg.gain==g?" selected":"")+">"+GAIN_LABEL[g]+"</option>"; }
  h += "</select>";
  h += "<div class='row'><div><label>Poll interval (sec)</label><input name='interval' value='"+String(cfg.interval_s)+"'></div></div>";
  h += "<label>Channel map</label><textarea name='map' placeholder='0 canopy-par par:umol/m2/s:800:0'>"+cfg.channel_map+"</textarea>";
  h += "<div class='muted'>One line per channel: <code>ch stream meas:unit:m:b</code> &middot; value = m&times;volts + b. 4-20mA: add a sense resistor, fold it into m/b.</div>";
  h += "<h2>Advanced</h2>";
  h += "<label>NTP server</label><input name='ntp' value='"+cfg.ntp_server+"'>";
  h += "<label>Admin password</label><input name='admin_pw' type='password' placeholder='(unchanged)'>";
  h += "<button class='btn' type='submit'>Save &amp; Reboot</button></form>";
  h += "<p class='muted' style='text-align:center'><a href='/status'>Live status &amp; read now &rarr;</a></p>";
  h += "<script>function tog(){var m=document.getElementById('method').value;document.getElementById('wh').style.display=(m=='webhook')?'block':'none';document.getElementById('mq').style.display=(m=='mqtt')?'block':'none';}tog();</script>";
  return h;
}
bool requireAuth(){ if(!server.authenticate(ADMIN_USER,cfg.admin_pw.c_str())){server.requestAuthentication();return false;} return true; }
void handleRoot(){ if(!requireAuth())return; String p=FPSTR(PAGE_HEAD); p+=fieldsForm(); p+=FPSTR(PAGE_FOOT); server.send(200,"text/html",p); }
void handleSave(){
  if(!requireAuth())return; auto arg=[&](const char*k){return server.arg(k);};
  cfg.device_name=arg("dev_name"); cfg.location=arg("location");
  cfg.ssid=arg("ssid"); if(arg("wifi_pw").length())cfg.wifi_pw=arg("wifi_pw");
  cfg.method=arg("method"); cfg.ep_url=arg("ep_url"); cfg.ep_auth=arg("ep_auth");
  cfg.mq_host=arg("mq_host"); cfg.mq_port=(uint16_t)arg("mq_port").toInt(); cfg.mq_user=arg("mq_user");
  if(arg("mq_pw").length())cfg.mq_pw=arg("mq_pw"); cfg.mq_topic=arg("mq_topic"); cfg.mq_tls=server.hasArg("mq_tls");
  cfg.interval_s=(uint32_t)arg("interval").toInt(); if(cfg.interval_s<10)cfg.interval_s=10;
  cfg.sda_pin=(int)arg("sda").toInt(); cfg.scl_pin=(int)arg("scl").toInt(); cfg.gain=(int)arg("gain").toInt();
  if(cfg.gain<0||cfg.gain>5)cfg.gain=1;
  cfg.channel_map=arg("map");
  cfg.ntp_server=arg("ntp"); if(cfg.ntp_server.length()==0)cfg.ntp_server="pool.ntp.org";
  if(arg("admin_pw").length())cfg.admin_pw=arg("admin_pw");
  cfgSave();
  String p=FPSTR(PAGE_HEAD); p+="<h1>Saved</h1><p>Rebooting in 3 seconds&hellip;</p>"; p+=FPSTR(PAGE_FOOT);
  server.send(200,"text/html",p); delay(3000); ESP.restart();
}
void handleStatus(){
  if(!requireAuth())return;
  String p=FPSTR(PAGE_HEAD);
  p+="<h1>Live Status</h1>";
  p+="<div class='muted'>ID "+cfg.device_id+" &middot; fw "+FW_VERSION+" &middot; "+String(adsOk?"ADS1115 OK":"ADS1115 NOT FOUND")+" &middot; gain "+String(GAIN_LABEL[cfg.gain])+"</div>";
  p+="<div class='muted'>WiFi: "+String(WiFi.isConnected()?"connected ":"down ")+WiFi.localIP().toString()+"</div>";
  p+="<div class='muted'>Uptime "+String(millis()/1000)+"s &middot; boots "+String(bootCount)+" &middot; sent "+String(pushCount)+"</div>";
  p+="<div class='muted'>Last read: "+lastScan+"</div>";
  if(adsOk){ p+="<h2>Live volts</h2><table style='width:100%;font-size:.78rem'><tr style='color:#8fb39a;text-align:left'><th>Ch</th><th>Volts</th></tr>";
    for(int c=0;c<4;c++){ float v=ads.computeVolts(ads.readADC_SingleEnded(c)); p+="<tr><td>A"+String(c)+"</td><td>"+String(v,4)+" V</td></tr>"; }
    p+="</table><div class='muted'>Use these to compute m/b for your channel map.</div>"; }
  p+="<p style='text-align:center'><a href='/readnow'>Read &amp; send now</a> &middot; <a href='/'>&larr; Setup</a></p>";
  p+=FPSTR(PAGE_FOOT); server.send(200,"text/html",p);
}
void handleReadNow(){ if(!requireAuth())return; doPoll(); server.sendHeader("Location","/status"); server.send(303); }
void handleNotFound(){ server.sendHeader("Location", String("http://")+AP_IP.toString()); server.send(302,"text/plain",""); }

void startWiFi(){
  WiFi.mode(WIFI_AP_STA); WiFi.softAPConfig(AP_IP,AP_IP,IPAddress(255,255,255,0)); WiFi.softAP(apSsid().c_str());
  if(cfg.ssid.length())WiFi.begin(cfg.ssid.c_str(),cfg.wifi_pw.c_str());
  dnsServer.start(DNS_PORT,"*",AP_IP);
}
void setup(){
  Serial.begin(115200); delay(200);
  Serial.println("\n[OAT] " FW_VERSION " booting");
  cfgLoad();
  prefs.begin("oatadc",false); bootCount=prefs.getULong("boot_n",0)+1; prefs.putULong("boot_n",bootCount); prefs.end();
  Wire.begin(cfg.sda_pin, cfg.scl_pin);
  adsOk = ads.begin();
  if (adsOk) ads.setGain(gainFromIndex(cfg.gain));
  parseMap();
  startWiFi();
  configTime(0,0,cfg.ntp_server.c_str());
  server.on("/",HTTP_GET,handleRoot);
  server.on("/save",HTTP_POST,handleSave);
  server.on("/status",HTTP_GET,handleStatus);
  server.on("/readnow",HTTP_GET,handleReadNow);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("[OAT] AP %s -> http://%s  (ADS %s, %d channels)\n", apSsid().c_str(), AP_IP.toString().c_str(), adsOk?"ok":"missing", nChans);
  lastPushMs = millis();
}
void loop(){
  dnsServer.processNextRequest();
  server.handleClient();
  if(cfg.method=="mqtt" && mqtt.connected()) mqtt.loop();
  static unsigned long lastWifiTry=0;
  if(cfg.ssid.length() && !WiFi.isConnected() && millis()-lastWifiTry>15000){lastWifiTry=millis(); WiFi.begin(cfg.ssid.c_str(),cfg.wifi_pw.c_str());}
  if(WiFi.isConnected() && nChans>0 && millis()-lastPushMs >= cfg.interval_s*1000UL) doPoll();
}
