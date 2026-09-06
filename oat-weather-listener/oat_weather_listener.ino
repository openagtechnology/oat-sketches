/* =============================================================================
   OAT Weather-Station Listener  —  v1.0.2
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   Hears the weather stations and outdoor sensors a grower already owns — the
   AcuRite Iris (5-in-1) and Atlas, La Crosse, Oregon Scientific, and the Fine
   Offset family sold as Ecowitt and Ambient Weather (WH65/WS-2902 arrays, WH51
   soil probes, WN30 temperature probes, WN35 leaf wetness, WH57 lightning, WH41
   particulates, WH55 leak) — every one of which shouts its readings into the air
   on 433 or 915 MHz for anyone to hear. This listens, harvests every value each
   device broadcasts, and pushes them as oat-ods to an endpoint the operator owns.
   The BLE Listener's sibling, tuned to the band where the outdoor sensors live.

   THE DECODERS ARE NOT OURS. They are the rtl_433 project's, ported to the ESP32
   as rtl_433_ESP (GPL-3.0): ~220 on-off-keying device types, refreshed from the
   rtl_433 tree. We wrap them as a driver on oat_node_core; this file is the map
   from a decoder's JSON to the OAT vocabulary, and the station table. Nothing
   here decodes a radio protocol, and nothing here should: a new station is a
   decoder upstream, not a sketch release.

   THE RADIO. A CC1101 transceiver on SPI (any ESP32), or the SX1276 already on a
   Heltec WiFi LoRa 32 V2 — OR, built with -DOAT_RX_DATAPIN=<gpio>, a bare 433 MHz
   superheterodyne receiver whose DATA line drives one pin (the IBC v1.02 board's
   GPIO 33). The bare-receiver path has no RSSI to gate on, so the rtl_433 library
   cannot use it; it runs this sketch's own pulse-width decoder instead, which
   knows the AcuRite 5-in-1 (Iris) and is the lift of the IBC sketch that first
   decoded it on that board, with rtl_433's pulse tolerances. Fewer stations, same
   streams, same everything downstream. Both demodulate OOK and FSK, but not at the same time;
   the shipped images are OOK (Acurite, Fine Offset OOK, La Crosse, Oregon). The
   FSK arrays (Ecowitt WS80/WS90) are the same source built with
   -DOOK_MODULATION=false. The receive frequency is a SETTING (band): 433.92 for
   Acurite/La Crosse/Oregon, 915.00 for the Fine Offset family in the US.

   TWO OUTPUTS, ONE FIRMWARE. With Wi-Fi and an endpoint configured, this is an OAT
   gateway and pushes oat-ods. With nothing configured it still prints every
   reading on the USB serial port in the oat-line grammar (the core's line output),
   so cabled to a LoRa Field Node's pod port it is a pod. Same decoder, same ids.

   NAMING LAW: the station's own id is the stream id — "<model-slug>:<id>[-<ch>]",
   e.g. acurite-5n1:2716. The listener names nothing; the endpoint maps ids to
   places. A station whose battery is swapped keeps its id on most models; the few
   that re-roll on battery change fragment their history and the page says so.

   CHANGELOG
     1.0.2  (1) The bare-receiver path still stamped 0 dBm on every stream's link
            metadata (1.0.1 only dropped the rssi measurement); no level is now
            "unset", and the table shows a dash. (2) A station that goes quiet for
            ten minutes has its stream released and is marked silent, so a
            neighbour's sensor that drifted past, or a dead battery, no longer
            holds a slot for ever; it files straight back in when heard again.
     1.0.1  Bench 2026-09-05 (AcuRite Iris on a bare 433 receiver, GPIO 33): worked
            first time. A bare receiver has no signal level, so the data-pin path
            no longer reports rssi (it sent 0 dBm); link metadata is left unset.
     1.0.0  First release.

   LICENSE: this sketch is openly licensed; the bundled decoders are GPL-3.0 via
   rtl_433_ESP, so a built image is GPL-governed. Copy it, change it, share it.
   ============================================================================= */

#include <oat_node_core.h>
#include <oat_measurands.h>
#include <ArduinoJson.h>
#ifndef OAT_RX_DATAPIN
  #include <ArduinoLog.h>
  #include <rtl_433_ESP.h>
#endif

#define TIER        "oat-weather-listener"
#define FW_SEMVER   "1.0.2"
#define FW_VERSION  "OAT-Weather-Listener/1.0.2"
#define NVS_NS      "oatwx"
#ifndef OAT_BOARD_NAME
  #define OAT_BOARD_NAME "ESP32"
#endif
#ifndef RF_MODULE_FREQUENCY
  #define RF_MODULE_FREQUENCY 433.92
#endif

#define JSON_MSG_BUFFER   1024
#define MAX_STATIONS      24
#define ID_LEN            40
#define STATION_GONE_MS   (10UL * 60UL * 1000UL)   // stations broadcast every 16-60 s; ten silent minutes is gone

#ifndef OAT_RX_DATAPIN
static rtl_433_ESP rf;
#endif
static char msgBuf[JSON_MSG_BUFFER];
static float g_freq = RF_MODULE_FREQUENCY;
static String g_allow;                        // "" = every station heard
static uint32_t g_signals = 0, g_decoded = 0, g_skipped = 0;   // written only from the decoder task
static char g_unmapped[160] = {0};
static bool g_radioOk = false;

// ---------------------------------------------------------------------------
// The map from rtl_433's field names to the OAT vocabulary. rtl_433 reports the
// unit in the key (temperature_C, wind_avg_km_h, rain_in) so the conversion to
// the canonical unit is part of the row. Anything numeric that is not here and
// not identity is forwarded RAW under its own key, the BLE Listener's rule, and
// `status` lists it so it can be promoted.
// ---------------------------------------------------------------------------
enum Xf : uint8_t { X_NONE, X_F_TO_C, X_KMH_MS, X_MPH_MS, X_IN_MM, X_MV_V, X_INVERT, X_KNOT_MS };
struct Map { const char* key; const char* measurement; const char* unit; uint8_t kind; uint8_t xf; };
static const Map MAP[] = {
  { "temperature_C",   "temperature",        "Cel",   oat::KIND_CONTINUOUS, X_NONE },
  { "temperature_F",   "temperature",        "Cel",   oat::KIND_CONTINUOUS, X_F_TO_C },
  { "temperature_1_C", "temperature",        "Cel",   oat::KIND_CONTINUOUS, X_NONE },
  { "temperature_2_C", "temperature_2",      "Cel",   oat::KIND_CONTINUOUS, X_NONE },
  { "humidity",        "humidity",           "%RH",   oat::KIND_CONTINUOUS, X_NONE },
  { "wind_avg_m_s",    "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_NONE },
  { "wind_avg_km_h",   "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_KMH_MS },
  { "wind_avg_mi_h",   "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_MPH_MS },
  { "wind_speed_m_s",  "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_NONE },
  { "wind_speed_km_h", "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_KMH_MS },
  { "wind_max_m_s",    "wind_gust",          "m/s",   oat::KIND_GAUGE,      X_NONE },
  { "wind_max_km_h",   "wind_gust",          "m/s",   oat::KIND_GAUGE,      X_KMH_MS },
  { "wind_max_mi_h",   "wind_gust",          "m/s",   oat::KIND_GAUGE,      X_MPH_MS },
  { "gust_speed_m_s",  "wind_gust",          "m/s",   oat::KIND_GAUGE,      X_NONE },
  { "wind_dir_deg",    "wind_direction",     "deg",   oat::KIND_GAUGE,      X_NONE },
  { "rain_mm",         "rain_total",         "mm",    oat::KIND_CUMULATIVE, X_NONE },
  { "rain_in",         "rain_total",         "mm",    oat::KIND_CUMULATIVE, X_IN_MM },
  { "rain_rate_mm_h",  "rain_rate",          "mm/h",  oat::KIND_CONTINUOUS, X_NONE },
  { "rain_rate_in_h",  "rain_rate",          "mm/h",  oat::KIND_CONTINUOUS, X_IN_MM },
  { "uv",              "uv_index",           "",      oat::KIND_CONTINUOUS, X_NONE },
  { "uvi",             "uv_index",           "",      oat::KIND_CONTINUOUS, X_NONE },
  { "light_lux",       "illuminance",        "lx",    oat::KIND_CONTINUOUS, X_NONE },
  { "lux",             "illuminance",        "lx",    oat::KIND_CONTINUOUS, X_NONE },
  { "radiation_W_m2",  "solar_radiation",    "W/m2",  oat::KIND_CONTINUOUS, X_NONE },
  { "moisture",        "soil_moisture",      "%",     oat::KIND_CONTINUOUS, X_NONE },
  { "leafwetness",     "leaf_wetness",       "%",     oat::KIND_CONTINUOUS, X_NONE },
  { "wetness",         "leaf_wetness",       "%",     oat::KIND_CONTINUOUS, X_NONE },
  { "strike_count",    "lightning_total",    "",      oat::KIND_CUMULATIVE, X_NONE },
  { "storm_dist_km",   "lightning_distance", "km",    oat::KIND_GAUGE,      X_NONE },
  { "storm_dist",      "lightning_distance", "km",    oat::KIND_GAUGE,      X_NONE },
  { "pressure_hPa",    "pressure",           "hPa",   oat::KIND_CONTINUOUS, X_NONE },
  { "pm2_5_ug_m3",     "pm25",               "ug/m3", oat::KIND_CONTINUOUS, X_NONE },
  { "pm10_ug_m3",      "pm10",               "ug/m3", oat::KIND_CONTINUOUS, X_NONE },
  { "co2_ppm",         "co2",                "ppm",   oat::KIND_CONTINUOUS, X_NONE },
  { "depth_cm",        "water_level",        "cm",    oat::KIND_CONTINUOUS, X_NONE },
  { "battery_ok",      "battery_low",        "",      oat::KIND_STATE,      X_INVERT },
  { "battery_mV",      "voltage",            "V",     oat::KIND_GAUGE,      X_MV_V },
  { "battery_V",       "voltage",            "V",     oat::KIND_GAUGE,      X_NONE },
  { "leak",            "water_leak",         "",      oat::KIND_STATE,      X_NONE },
  { "alarm",           "water_leak",         "",      oat::KIND_STATE,      X_NONE },
};
static const size_t MAP_N = sizeof(MAP) / sizeof(MAP[0]);

// Identity and radio bookkeeping rtl_433 emits: never a measurement.
static const char* const META[] = {
  "model", "id", "channel", "mic", "protocol", "rssi", "snr", "noise", "sequence_num", "message_type",
  "subtype", "duration", "pulses", "freq", "freq1", "freq2", "mod", "brand", "exception", "time",
  "battery", "battery_pct", "battery_level", "type", "flags", "raw_message", "data", "sensor_id",
};
static bool isMeta(const char* k) { for (auto m : META) if (!strcmp(k, m)) return true; return false; }

static double xform(uint8_t xf, double v) {
  switch (xf) {
    case X_F_TO_C:  return (v - 32.0) * 5.0 / 9.0;
    case X_KMH_MS:  return v / 3.6;
    case X_MPH_MS:  return v * 0.44704;
    case X_KNOT_MS: return v * 0.514444;
    case X_IN_MM:   return v * 25.4;
    case X_MV_V:    return v / 1000.0;
    case X_INVERT:  return v ? 0.0 : 1.0;
    default:        return v;
  }
}

// Brand from the model's family prefix. rtl_433 models read "Acurite-5n1",
// "Fineoffset-WH65B", "LaCrosse-TX141THBv2", "Oregon-THGR122N" ...
static const char* brandFor(const String& model) {
  String p = model; int d = p.indexOf('-'); if (d > 0) p = p.substring(0, d);
  if (p.equalsIgnoreCase("Acurite"))    return "AcuRite";
  if (p.equalsIgnoreCase("Fineoffset")) return "Fine Offset (Ecowitt / Ambient)";
  if (p.equalsIgnoreCase("LaCrosse"))   return "La Crosse";
  if (p.equalsIgnoreCase("Oregon"))     return "Oregon Scientific";
  if (p.equalsIgnoreCase("Ambientweather")) return "Ambient Weather";
  if (p.equalsIgnoreCase("Ecowitt"))    return "Ecowitt";
  if (p.equalsIgnoreCase("Bresser"))    return "Bresser";
  if (p.equalsIgnoreCase("Davis"))      return "Davis";
  if (p.equalsIgnoreCase("TFA"))        return "TFA Dostmann";
  if (p.equalsIgnoreCase("Nexus") || p.equalsIgnoreCase("Prologue") || p.equalsIgnoreCase("Springfield")) return "generic";
  static char b[24]; strncpy(b, p.c_str(), sizeof(b) - 1); b[sizeof(b) - 1] = 0; return b;
}

// ---------------------------------------------------------------------------
// Stations heard — the display table. The core holds the readings; this holds
// who they came from.
// ---------------------------------------------------------------------------
struct Station { bool used; char sid[ID_LEN]; char model[32]; int rssi; bool haveRssi; bool gone; unsigned long lastMs; uint32_t frames; int slot; };
static Station stations[MAX_STATIONS];
static SemaphoreHandle_t stMutex = nullptr;
static inline void stLock()   { if (stMutex) xSemaphoreTake(stMutex, portMAX_DELAY); }
static inline void stUnlock() { if (stMutex) xSemaphoreGive(stMutex); }

static bool allowed(const char* sid) {
  if (!g_allow.length()) return true;
  String hay = g_allow; hay.toLowerCase(); String n = sid; n.toLowerCase();
  return hay.indexOf(n) >= 0;
}
static void noteUnmapped(const char* key) {
  if (!key || !key[0] || strstr(g_unmapped, key)) return;
  size_t len = strlen(g_unmapped);
  if (len + strlen(key) + 2 >= sizeof(g_unmapped)) return;
  if (len) strcat(g_unmapped, ",");
  strcat(g_unmapped, key);
}

static String slug(const String& s) {
  String o; o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) { char c = tolower(s[i]); o += (isalnum(c) ? c : '-'); }
  return o;
}

// One decoded signal. Runs in the decoder task: file it and get out.
static void onDecoded(char* message) {
  g_decoded++;
  JsonDocument doc;
  if (deserializeJson(doc, message)) { g_skipped++; return; }
  JsonObject o = doc.as<JsonObject>();
  const char* model = o["model"] | "";
  if (!model[0]) { g_skipped++; return; }

  // stream id: model slug + id (+ channel where the model has one)
  String sid = slug(model) + ":";
  if (!o["id"].isNull()) sid += String(o["id"].as<long>());
  else if (!o["sensor_id"].isNull()) sid += String(o["sensor_id"].as<long>());
  else sid += "0";
  if (!o["channel"].isNull()) { sid += "-"; sid += String(o["channel"].as<const char*>() ? o["channel"].as<const char*>() : String(o["channel"].as<int>()).c_str()); }
  if (sid.length() >= ID_LEN) sid = sid.substring(0, ID_LEN - 1);
  if (!allowed(sid.c_str())) { g_skipped++; return; }
  bool haveRssi = !o["rssi"].isNull();
  int rssi = haveRssi ? o["rssi"].as<int>() : 0;

  stLock();
  int d = -1, freeIdx = -1;
  for (int i = 0; i < MAX_STATIONS; i++) {
    if (stations[i].used && strcmp(stations[i].sid, sid.c_str()) == 0) { d = i; break; }
    if (!stations[i].used && freeIdx < 0) freeIdx = i;
  }
  if (d < 0) d = freeIdx;
  if (d < 0) { stUnlock(); g_skipped++; return; }
  Station& st = stations[d];
  if (!st.used) { memset(&st, 0, sizeof(st)); st.used = true; strncpy(st.sid, sid.c_str(), ID_LEN - 1); st.slot = oatcore::slotFor(st.sid, st.sid); }
  else if (st.gone || st.slot < 0) { st.slot = oatcore::slotFor(st.sid, st.sid); st.gone = false; }   // back after silence: re-claim
  strncpy(st.model, model, sizeof(st.model) - 1);
  st.rssi = rssi; st.haveRssi = haveRssi; st.lastMs = millis(); st.frames++;
  int slot = st.slot;
  stUnlock();
  if (slot < 0) return;

  oatcore::slotMeta(slot, brandFor(model), model);
  int battPct = -1;
  if (!o["battery"].isNull() && o["battery"].is<int>()) battPct = o["battery"].as<int>();
  if (!o["battery_pct"].isNull()) battPct = o["battery_pct"].as<int>();
  oatcore::slotLink(slot, haveRssi ? rssi : -1, battPct);   // -1 = unset: a bare receiver has no level

  for (JsonPair kv : o) {
    const char* k = kv.key().c_str();
    if (isMeta(k)) continue;
    JsonVariant v = kv.value();
    double val;
    if (v.is<bool>()) val = v.as<bool>() ? 1.0 : 0.0;
    else if (v.is<double>() || v.is<long>()) val = v.as<double>();
    else if (v.is<const char*>()) {                 // "OK"/"LOW" style strings for battery on some decoders
      const char* s = v.as<const char*>();
      if (!strcmp(k, "battery_ok") || !strcmp(k, "battery")) { oatcore::fold(slot, "battery_low", "", oat::KIND_STATE, strcasecmp(s, "LOW") == 0 ? 1.0 : 0.0); }
      continue;
    } else continue;
    bool mapped = false;
    for (size_t i = 0; i < MAP_N; i++) {
      if (strcmp(k, MAP[i].key) == 0) { oatcore::fold(slot, MAP[i].measurement, MAP[i].unit, MAP[i].kind, xform(MAP[i].xf, val)); mapped = true; break; }
    }
    if (!mapped) { oatcore::fold(slot, k, "", oat::KIND_CONTINUOUS, val); noteUnmapped(k); }
  }
  if (haveRssi) oatcore::fold(slot, "rssi", "dBm", oat::KIND_GAUGE, (double)rssi);
}

#ifdef OAT_RX_DATAPIN
// ---------------------------------------------------------------------------
// The bare-receiver path: a superheterodyne 433 MHz module's DATA pin on one
// GPIO. The ISR times every HIGH pulse; four sync pulses (~600 us) start a frame,
// then each bit is a long (~400 us = 1) or short (~220 us = 0) pulse, 64 bits,
// checksum = sum of bytes 0..6 mod 256 == byte 7. That is the AcuRite 5-in-1
// (Iris 06004M) as rtl_433's acurite.c documents it; the windows below are
// rtl_433's with its tolerance, wider than the IBC sketch's, which dropped frames.
// Two message types alternate every ~18 s: 0x31 wind + direction + rain counter,
// 0x38 wind + temperature + humidity. The decoded fields are handed to the SAME
// map as the rtl_433 path, so everything downstream is identical.
// ---------------------------------------------------------------------------
#define DP_SYNC_LO   500
#define DP_SYNC_HI   750
#define DP_LONG_LO   330
#define DP_LONG_HI   480
#define DP_SHORT_LO  150
#define DP_SHORT_HI  300
#define DP_RESET_US  10000
#define DP_BITS      64
static volatile uint32_t dp_rise = 0;
static volatile uint8_t  dp_state = 0, dp_sync = 0, dp_bits = 0;
static volatile uint8_t  dp_buf[8], dp_frame[8];
static volatile bool     dp_ready = false;
static volatile uint32_t dp_frames = 0, dp_badcrc = 0;

static void IRAM_ATTR dpIsr() {
  uint32_t now = micros();
  if (digitalRead(OAT_RX_DATAPIN) == HIGH) {
    if (now - dp_rise > DP_RESET_US) { dp_state = 0; dp_sync = 0; dp_bits = 0; }
    dp_rise = now; return;
  }
  uint32_t w = now - dp_rise;
  if (dp_state < 2) {                                   // hunting for the sync train
    if (w > DP_SYNC_LO && w < DP_SYNC_HI) { dp_state = 1; if (++dp_sync >= 4) { dp_state = 2; dp_sync = 0; dp_bits = 0; } }
    else { dp_state = 0; dp_sync = 0; }
    return;
  }
  if (dp_bits >= DP_BITS) return;                       // frame complete; wait for the loop to take it
  uint8_t bytepos = dp_bits >> 3, bit = 7 - (dp_bits & 7);
  if (w > DP_LONG_LO && w < DP_LONG_HI)       { dp_buf[bytepos] |=  (1 << bit); dp_bits++; }
  else if (w > DP_SHORT_LO && w < DP_SHORT_HI){ dp_buf[bytepos] &= ~(1 << bit); dp_bits++; }
  else { dp_state = 0; dp_sync = 0; dp_bits = 0; return; }   // a pulse outside every window: noise, start over
  if (dp_bits >= DP_BITS && !dp_ready) { for (int i = 0; i < 8; i++) dp_frame[i] = dp_buf[i]; dp_ready = true; dp_state = 0; }
}
static const float DP_WINDDIR[16] = { 315.0, 247.5, 292.5, 270.0, 337.5, 225.0, 0.0, 202.5, 67.5, 135.0, 90.0, 112.5, 45.0, 157.5, 22.5, 180.0 };

static void dpPoll() {
  if (!dp_ready) return;
  uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = dp_frame[i];
  dp_ready = false;
  int sum = 0; for (int i = 0; i < 7; i++) sum += b[i];
  if ((sum & 0xFF) != b[7] || sum == 0) { dp_badcrc++; return; }
  dp_frames++;
  // Compose what rtl_433 would have said, and hand it to the same map.
  JsonDocument d;
  d["model"] = "Acurite-5n1";
  d["id"] = ((b[0] & 0x0F) << 8) | b[1];
  static const char* CH[] = { "C", "?", "B", "A" };
  d["channel"] = CH[b[0] >> 6];
  d["battery_ok"] = (b[2] & 0x40) ? 1 : 0;
  int raw = ((b[3] & 0x1F) << 3) | ((b[4] & 0x70) >> 4);
  d["wind_avg_km_h"] = raw > 0 ? raw * 0.8278 + 1.0 : 0.0;
  uint8_t mt = b[2] & 0x3F;
  if (mt == 0x31) {
    d["wind_dir_deg"] = DP_WINDDIR[b[4] & 0x0F];
    d["rain_in"] = (((b[5] & 0x7F) << 7) | (b[6] & 0x7F)) * 0.01;
  } else if (mt == 0x38) {
    int t = ((b[4] & 0x0F) << 7) | (b[5] & 0x7F);
    d["temperature_F"] = (t - 400) / 10.0;
    d["humidity"] = b[6] & 0x7F;
  } else { g_skipped++; return; }
  // no "rssi": a bare receiver reports no signal level, and 0 dBm would be a lie
  serializeJson(d, msgBuf, JSON_MSG_BUFFER);
  onDecoded(msgBuf);
}
#endif

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------
static void radioStart() {
#ifdef OAT_RX_DATAPIN
  pinMode(OAT_RX_DATAPIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(OAT_RX_DATAPIN), dpIsr, CHANGE);
  g_radioOk = true; g_freq = 433.92f;
  Serial.printf("[wx] %s: bare 433 MHz receiver on GPIO %d, AcuRite 5-in-1 decoder\n", OAT_BOARD_NAME, OAT_RX_DATAPIN);
#else
  static bool logOn = false;
  if (!logOn) { Log.begin(LOG_LEVEL, &Serial); Log.setShowLevel(false); logOn = true; }
  // begin() reports failure instead of halting the board, so a missing or
  // mis-wired module leaves the setup page and console alive to say so.
  g_radioOk = rf.begin(RF_MODULE_RECEIVER_GPIO, g_freq);
  if (g_radioOk) { rf.setCallback(onDecoded, msgBuf, JSON_MSG_BUFFER); rf.enableReceiver(); }
  Serial.printf("[wx] %s: radio %s at %.2f MHz (%s)\n", OAT_BOARD_NAME, g_radioOk ? "listening" : "NOT FOUND (check the module's SPI wiring and 3V3)", g_freq, OOK_MODULATION ? "OOK" : "FSK");
#endif
}
// A band change re-initialises the receiver: the library owns the radio's task,
// queue and buffers and offers end()/begin() as the only clean way to retune.
static void radioRetune() {
#ifdef OAT_RX_DATAPIN
  return;                                               // the receiver module fixes the band
#else
  if (g_radioOk) { rf.disableReceiver(); rf.end(); g_radioOk = false; }
  radioStart();
#endif
}

// ---------------------------------------------------------------------------
// Driver hooks
// ---------------------------------------------------------------------------
static String getBand()  { return String(g_freq, 2); }
static bool   setBand(const String& v, String& why) {
#ifdef OAT_RX_DATAPIN
  why = "this image drives a bare 433 MHz receiver module; its band is fixed by the module"; return false;
#endif
  float f = v.toFloat();
  if (!(f >= 300 && f <= 928)) { why = "band must be a frequency in MHz: 433.92 (Acurite, La Crosse, Oregon) or 915.00 (Ecowitt / Ambient, US)"; return false; }
  bool changed = fabs(f - g_freq) > 0.001f; g_freq = f;
  if (changed && g_radioOk) radioRetune();
  return true;
}
static String getAllow() { return g_allow; }
static bool   setAllow(const String& v, String& why) { g_allow = v; return true; }

static const oatcore::Field FIELDS[] = {
  { "band", "Band (MHz)", "433.92 for AcuRite, La Crosse and Oregon Scientific; 915.00 for the Ecowitt / Ambient Weather family in the US. The radio hears one band at a time.", getBand, setBand },
  { "stations", "Station allow-list (optional)", "Comma-separated stream ids, e.g. acurite-5n1:2716. Empty means every station heard is reported, which on a shared band includes the neighbours'.", getAllow, setAllow },
};

static void wxBegin() {
  stMutex = xSemaphoreCreateMutex();
  radioStart();
}
static void wxSample()  { }
// A station silent for STATION_GONE_MS has its stream released and is shown as
// silent, the LoRa Gateway's rule; the row stays so it can file straight back in.
static void sweepSilent() {
  static unsigned long last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  stLock();
  for (int i = 0; i < MAX_STATIONS; i++) {
    Station& st = stations[i];
    if (!st.used || st.gone || millis() - st.lastMs < STATION_GONE_MS) continue;
    if (st.slot >= 0) oatcore::release(st.slot);
    st.slot = -1; st.gone = true;
    Serial.printf("[wx] %s silent for %lu min: released\n", st.sid, STATION_GONE_MS / 60000UL);
  }
  stUnlock();
}
static void wxCollect() {
#ifdef OAT_RX_DATAPIN
  dpPoll();
#else
  rf.loop();
#endif
  sweepSilent();
}
static void wxRescan()  { if (g_radioOk) radioRetune(); }

static String statusHtml() {
#ifdef OAT_RX_DATAPIN
  String p = "<div class='muted'>Bare 433 MHz receiver on GPIO " + String(OAT_RX_DATAPIN) + " &middot; AcuRite 5-in-1 decoder &middot; frames " + String(dp_frames) + ", bad checksum " + String(dp_badcrc) + " &middot; " + String(OAT_BOARD_NAME) + "</div>";
#else
  String p = "<div class='muted'>Radio " + String(g_radioOk ? "listening" : "NOT STARTED") + " &middot; " + String(g_freq, 2) + " MHz &middot; " + String(OOK_MODULATION ? "OOK" : "FSK") + " &middot; " + String(OAT_BOARD_NAME) + "</div>";
#endif
  p += "<table class='tbl'><tr><th>Station</th><th>Model</th><th>Signal</th><th>Last heard</th><th>Frames</th></tr>";
  int shown = 0;
  stLock();
  for (int i = 0; i < MAX_STATIONS; i++) {
    Station& s = stations[i]; if (!s.used) continue; shown++;
    p += "<tr><td>" + String(s.sid) + (s.gone ? " <span class='bad'>silent</span>" : "") + "</td><td>" + String(s.model) + "</td><td>" + (s.haveRssi ? String(s.rssi) + " dBm" : String("&mdash;")) + "</td><td>" + String((millis() - s.lastMs) / 1000) + "s ago</td><td>" + String(s.frames) + "</td></tr>";
  }
  stUnlock();
  p += "</table>";
  if (!shown) p += "<p class='bad'>Nothing heard yet. A station transmits every 16 to 60 seconds, so give it a minute. If it stays empty: the band must match the station (433.92 for AcuRite, 915.00 for Ecowitt/Ambient in the US), the antenna must be the right length for that band, and the station must be within range of a receiver that is far less sensitive than a dedicated SDR dongle.</p>";
  if (g_unmapped[0]) p += "<div class='muted'>Values arriving under keys the vocabulary does not map yet, forwarded raw: <code>" + String(g_unmapped) + "</code>. Nothing is lost; these can be promoted to named measurands.</div>";
  p += "<div class='muted'>Signals decoded " + String(g_decoded) + " &middot; skipped " + String(g_skipped) + (g_allow.length() ? " &middot; allow-list on" : " &middot; every station reported") + "</div>";
  return p;
}
static String statusText() {
  String s = "wx radio=" + String(g_radioOk ? "listening" : "off") + " freq=" + String(g_freq, 2) + " decoded=" + String(g_decoded) + " skipped=" + String(g_skipped) + "\n";
  stLock();
  for (int i = 0; i < MAX_STATIONS; i++) {
    Station& st = stations[i]; if (!st.used) continue;
    s += "station " + String(st.sid) + (st.gone ? " SILENT" : "") + " " + String(st.model) + " rssi=" + (st.haveRssi ? String(st.rssi) : String("-")) + " last=" + String((millis() - st.lastMs) / 1000) + "s frames=" + String(st.frames) + "\n";
  }
  stUnlock();
  if (g_unmapped[0]) s += "unmapped keys (forwarded raw): " + String(g_unmapped) + "\n";
  return s;
}
static String diagLine() {
  int n = 0; stLock(); for (int i = 0; i < MAX_STATIONS; i++) if (stations[i].used && !stations[i].gone) n++; stUnlock();
  return "decoded " + String(g_decoded) + " signal(s) from " + String(n) + " station(s) at " + String(g_freq, 2) + " MHz. A radio cannot be 'wired wrong' — if decoded is zero, the band, the antenna length, or the range is the question.";
}
static void cmdStations(const String&) { Serial.print(statusText()); }
static const oatcore::Command COMMANDS[] = {
  { "stations", "list the weather stations and sensors heard, with signal and age", cmdStations },
};

static const oatcore::Driver DRIVER = {
  TIER, "Stations heard", FW_VERSION, FW_SEMVER, NVS_NS,
  wxBegin, wxSample, wxCollect, nullptr, wxRescan,
  statusHtml, statusText, nullptr, diagLine,
  FIELDS,   (int)(sizeof(FIELDS)   / sizeof(FIELDS[0])),
  COMMANDS, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
};

void setup() { oatcore::begin(DRIVER); }
void loop()  { oatcore::loop(); }
