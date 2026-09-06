/* =============================================================================
   oat_lora_frame — the OAT over-air frame for a private 915 MHz sensor LAN
   -----------------------------------------------------------------------------
   WHY THIS EXISTS

   A LoRa frame carries about 200 bytes and a single oat-ods observation is ~330
   bytes of JSON, so the JSON cannot ride the radio. It does not need to. A field
   node is to the LoRa Gateway what a Govee is to the BLE Listener: a thing that
   broadcasts a few dozen bytes on a schedule and never expects a reply. The
   gateway claims a slot per sensor, folds the readings, and pushes oat-ods on the
   node's behalf — the same path the BLE Listener already takes. Nothing here is a
   protocol: no handshake, no ack, no routing. UDP semantics on radio.

   TWO FRAME KINDS, ONE HEADER

     DATA    readings: N x { sub-id, measurand code, int16 scaled value }
     ROSTER  the sub-id -> hardware-id map, so DATA frames carry a 1-byte sub-id
             instead of an 8-byte 1-Wire address every cycle. Sent at boot, then
             every ROSTER_EVERY data frames, then on demand. The gateway holds
             readings for a sub-id it has no roster entry for, and counts them,
             so a stream is never created under a placeholder id that would split
             the series the moment the real id arrives.

   HEADER (11 bytes, little-endian)
     0     magic     'O' (0x4F)  — rejects foreign LoRa on the same channel cheaply
     1     version   FRAME_VERSION
     2..5  unit id   low 32 bits of the node's efuse MAC (its hardware id)
     6     seq       rolls at 255; the gateway reports gaps as loss
     7     kind      FRAME_DATA | FRAME_ROSTER
     8     battery   0..100 %, BATT_NA when mains-powered / not measured
     9     interval  the node's transmit cadence in units of 10 s (0 = unknown).
                     The gateway releases a node's slots after MISSED_BEFORE_GONE
                     silent intervals — the interval IS the heartbeat.
     10    count     entries that follow
     ...   entries
     last 2  CRC-16/CCITT-FALSE over everything before it. The radio has its own
             CRC; this one guards the bytes as the SKETCH sees them (a UART modem
             between the two sides, say, has no such guarantee).

   DATA ENTRY (4 bytes)     sub-id u8 · code u8 · value i16 (scaled per code)
   ROSTER ENTRY (variable)  sub-id u8 · sensor kind u8 · id length u8 · id bytes

   The measurand code table below is the whole "codebook": each code names the
   oat-ods measurement, its SenML unit, its fold kind and the int16 scale. Adding a
   measurand = one row here, on both sides. Codes are never renumbered.
   ============================================================================= */
#pragma once
#include <Arduino.h>
#include <oat_measurands.h>   // KIND_* fold kinds, so both sides agree with the core

namespace oatlora {

static const uint8_t  MAGIC          = 0x4F;
static const uint8_t  FRAME_VERSION  = 1;
static const uint8_t  FRAME_DATA     = 1;
static const uint8_t  FRAME_ROSTER   = 2;
static const uint8_t  BATT_NA        = 0xFF;
static const size_t   HEADER_LEN     = 11;
static const size_t   CRC_LEN        = 2;
static const size_t   MAX_FRAME      = 200;        // under every LoRa modem's ceiling
static const size_t   DATA_ENTRY_LEN = 4;
static const uint8_t  MAX_DATA_PER_FRAME = (MAX_FRAME - HEADER_LEN - CRC_LEN) / DATA_ENTRY_LEN;   // 46
static const uint8_t  ROSTER_EVERY   = 5;          // data frames between roster repeats
static const uint8_t  MISSED_BEFORE_GONE = 3;      // silent intervals before the gateway releases

// Sensor kinds — how the gateway renders a roster id into a stream id string. The
// string forms match what the wired sketches already publish for the same part, so
// a probe moved from a Wi-Fi node to a LoRa node keeps its series.
enum SensorKind : uint8_t {
  SK_DS18B20 = 1,   // id = 8-byte ROM            -> "ds18b20:<16 hex>"
  SK_SHT30   = 2,   // id = 4-byte serial or addr  -> "sht30:<8 hex>"
  SK_ANALOG  = 3,   // id = 1-byte GPIO            -> "<unit hex>:a<pin>"  (a pin is hardware)
  SK_NODE    = 4,   // id = none                   -> "<unit hex>"  (the node's own gauges)
  // Reserved for the general-purpose node plan (2026-09-05). Numbers are final;
  // drivers arrive as they are built.
  SK_I2C     = 5,   // id = 1-byte addr + chip code -> "<unit hex>:i2c<addr hex>"  (BME280, SCD40, BH1750, MLX90614, INA219 ...)
  SK_ADS1115 = 6,   // id = addr + channel         -> "<unit hex>:ads<addr hex>c<n>"
  SK_MODBUS  = 7,   // id = unit address           -> "<unit hex>:mb<addr>"
  SK_SDI12   = 8,   // id = address + serial       -> "sdi12:<serial>" (the SDI-12 sketch's form)
  SK_PULSE   = 9,   // id = 1-byte GPIO            -> "<unit hex>:p<pin>"  (rain, wind, flow)
  SK_CONTACT = 10,  // id = 1-byte GPIO            -> "<unit hex>:c<pin>"
  SK_HX711   = 11,  // id = 1-byte data pin        -> "<unit hex>:hx<pin>"
  SK_SERIAL  = 13,  // id = the stream id STRING a pod printed on the node's serial port (oat-line
                    // grammar), carried verbatim, <= 24 bytes -> that string. The pod owns the id.
  SK_RF433   = 12,  // id = station model code + station id -> "rf433:<model>:<id>"
                    // A 433/915 MHz weather station (Acurite, Fine Offset, LaCrosse ...) heard by
                    // a receiver on the node. Like BLE: not wired, identified by what it broadcasts,
                    // may come and go. The station's own id is the stream id, never the node's.
};

struct Code {
  uint8_t     code;
  const char* measurement;   // oat-ods measurement
  const char* unit;          // SenML symbol
  uint8_t     kind;          // oat::MeasKind — picks the gateway's fold
  float       scale;         // value_on_air = round(value * scale), int16
  const char* brand;         // provenance the gateway stamps (slotMeta); "" = none
  const char* model;
};

// The codebook. Scale chosen so int16 covers the physical range with the
// resolution the part actually has. temperature x100 spans -327..327 C.
static const Code CODES[] = {
  {  1, "temperature",     "Cel",   oat::KIND_CONTINUOUS, 100.0f, "", "" },
  {  2, "humidity",        "%RH",   oat::KIND_CONTINUOUS, 100.0f, "", "" },
  {  3, "soil_moisture",   "%",     oat::KIND_CONTINUOUS, 100.0f, "", "" },
  {  4, "light_level",     "%",     oat::KIND_CONTINUOUS, 100.0f, "", "" },   // a photoresistor cannot honestly claim lux
  {  5, "analog_raw",      "",      oat::KIND_CONTINUOUS,   1.0f, "", "" },   // ADC counts, for calibrating at the endpoint
  {  6, "voltage",         "V",     oat::KIND_GAUGE,     1000.0f, "", "" },
  {  7, "battery",         "%",     oat::KIND_GAUGE,        1.0f, "", "" },
  {  8, "pressure",        "hPa",   oat::KIND_CONTINUOUS,  10.0f, "", "" },
  {  9, "co2",             "ppm",   oat::KIND_CONTINUOUS,   1.0f, "", "" },
  { 10, "illuminance",     "lx",    oat::KIND_CONTINUOUS,   0.1f, "", "" },   // to 327,670 lx at 10 lx steps
  { 11, "soil_conductivity","uS/cm",oat::KIND_CONTINUOUS,   1.0f, "", "" },
  { 12, "contact",         "",      oat::KIND_STATE,        1.0f, "", "" },
  { 13, "uptime",          "s",     oat::KIND_GAUGE,       0.001f,"", "" },   // 1000 s steps to ~379 days; x0.01 pinned an always-on node at 38 days (scale changed 2026-09-06, both sides)
  { 14, "mains",           "",      oat::KIND_STATE,        1.0f, "", "" },   // 1 = mains present, 0 = on battery
  // Reserved 2026-09-05 for the general-purpose node plan (weather, water, chemistry,
  // the 433 MHz station listener). Rows are the standard's vocabulary; codes never move.
  { 15, "wind_speed",      "m/s",   oat::KIND_CONTINUOUS, 100.0f, "", "" },
  { 16, "wind_direction",  "deg",   oat::KIND_GAUGE,        1.0f, "", "" },   // last, never averaged (0 and 359 are neighbours)
  { 17, "rainfall",        "mm",    oat::KIND_EVENT,       10.0f, "", "" },   // per cycle; the gateway folds with sum
  { 18, "flow",            "l/min", oat::KIND_CONTINUOUS, 100.0f, "", "" },
  { 19, "water_level",     "cm",    oat::KIND_CONTINUOUS,  10.0f, "", "" },
  { 20, "ph",              "pH",    oat::KIND_CONTINUOUS, 100.0f, "", "" },
  { 21, "conductivity",    "uS/cm", oat::KIND_CONTINUOUS,   1.0f, "", "" },   // solution EC; soil_conductivity is code 11
  { 22, "dissolved_oxygen","mg/l",  oat::KIND_CONTINUOUS, 100.0f, "", "" },
  { 23, "par",             "umol/m2/s", oat::KIND_CONTINUOUS, 1.0f, "", "" },
  { 24, "canopy_temperature","Cel", oat::KIND_CONTINUOUS, 100.0f, "", "" },
  { 25, "leaf_wetness",    "%",     oat::KIND_CONTINUOUS, 100.0f, "", "" },
  { 26, "weight",          "kg",    oat::KIND_GAUGE,       10.0f, "", "" },
  { 27, "current",         "A",     oat::KIND_GAUGE,     1000.0f, "", "" },
  { 28, "power",           "W",     oat::KIND_GAUGE,       10.0f, "", "" },
  { 29, "uv_index",        "",      oat::KIND_CONTINUOUS,  10.0f, "", "" },
  { 30, "wind_gust",       "m/s",   oat::KIND_GAUGE,      100.0f, "", "" },   // max, not mean
  { 31, "rain_rate",       "mm/h",  oat::KIND_CONTINUOUS,  10.0f, "", "" },
};
static const size_t CODES_N = sizeof(CODES) / sizeof(CODES[0]);

inline const Code* codeFor(uint8_t c) {
  for (size_t i = 0; i < CODES_N; i++) if (CODES[i].code == c) return &CODES[i];
  return nullptr;
}

inline int16_t encodeValue(const Code& c, double v) {
  double s = v * c.scale;
  if (s >  32767.0) s =  32767.0;
  if (s < -32768.0) s = -32768.0;
  return (int16_t)lround(s);
}
inline double decodeValue(const Code& c, int16_t raw) { return (double)raw / c.scale; }

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Small, table-free, and the one
// most serial tools compute if a person wants to check a captured frame by hand.
inline uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// ---------------------------------------------------------------------------
// Builder (node side). Fill the header, append entries, seal. The caller checks
// full() and starts a new frame when a data set does not fit.
// ---------------------------------------------------------------------------
struct Builder {
  uint8_t buf[MAX_FRAME];
  size_t  len = 0;

  void begin(uint32_t unitId, uint8_t seq, uint8_t kind, uint8_t battery, uint16_t intervalSec) {
    len = 0;
    buf[len++] = MAGIC;
    buf[len++] = FRAME_VERSION;
    buf[len++] = (uint8_t)(unitId);
    buf[len++] = (uint8_t)(unitId >> 8);
    buf[len++] = (uint8_t)(unitId >> 16);
    buf[len++] = (uint8_t)(unitId >> 24);
    buf[len++] = seq;
    buf[len++] = kind;
    buf[len++] = battery;
    uint16_t iv = intervalSec / 10; if (iv > 255) iv = 255;
    buf[len++] = (uint8_t)iv;
    buf[len++] = 0;               // count, patched by each add
  }
  uint8_t count() const { return buf[HEADER_LEN - 1]; }
  bool roomFor(size_t n) const { return len + n + CRC_LEN <= MAX_FRAME && count() < 255; }
  bool addData(uint8_t subId, uint8_t code, int16_t value) {
    if (!roomFor(DATA_ENTRY_LEN)) return false;
    buf[len++] = subId; buf[len++] = code;
    buf[len++] = (uint8_t)(value & 0xFF); buf[len++] = (uint8_t)((uint16_t)value >> 8);
    buf[HEADER_LEN - 1]++;
    return true;
  }
  bool addRoster(uint8_t subId, uint8_t sensorKind, const uint8_t* id, uint8_t idLen) {
    if (!roomFor(3 + idLen)) return false;
    buf[len++] = subId; buf[len++] = sensorKind; buf[len++] = idLen;
    memcpy(buf + len, id, idLen); len += idLen;
    buf[HEADER_LEN - 1]++;
    return true;
  }
  void seal() {
    uint16_t c = crc16(buf, len);
    buf[len++] = (uint8_t)(c >> 8); buf[len++] = (uint8_t)(c & 0xFF);
  }
};

// ---------------------------------------------------------------------------
// Parser (gateway side). Validates magic, version, length and CRC, then walks
// entries through the callbacks. Returns false with a reason on a bad frame.
// ---------------------------------------------------------------------------
struct Header {
  uint32_t unitId; uint8_t seq, kind, battery, count; uint16_t intervalSec;
};

inline bool parseHeader(const uint8_t* d, size_t n, Header& h, const char*& why) {
  if (n < HEADER_LEN + CRC_LEN)     { why = "short";   return false; }
  if (d[0] != MAGIC)                { why = "magic";   return false; }
  if (d[1] != FRAME_VERSION)        { why = "version"; return false; }
  uint16_t want = ((uint16_t)d[n-2] << 8) | d[n-1];
  if (crc16(d, n - CRC_LEN) != want){ why = "crc";     return false; }
  h.unitId = (uint32_t)d[2] | ((uint32_t)d[3] << 8) | ((uint32_t)d[4] << 16) | ((uint32_t)d[5] << 24);
  h.seq = d[6]; h.kind = d[7]; h.battery = d[8]; h.intervalSec = (uint16_t)d[9] * 10; h.count = d[10];
  return true;
}

// Walk DATA entries. cb(subId, code, rawValue).
template <typename F>
inline bool eachData(const uint8_t* d, size_t n, const Header& h, F cb) {
  size_t p = HEADER_LEN, end = n - CRC_LEN;
  for (uint8_t i = 0; i < h.count; i++) {
    if (p + DATA_ENTRY_LEN > end) return false;
    int16_t v = (int16_t)((uint16_t)d[p+2] | ((uint16_t)d[p+3] << 8));
    cb(d[p], d[p+1], v);
    p += DATA_ENTRY_LEN;
  }
  return true;
}

// Walk ROSTER entries. cb(subId, sensorKind, idBytes, idLen).
template <typename F>
inline bool eachRoster(const uint8_t* d, size_t n, const Header& h, F cb) {
  size_t p = HEADER_LEN, end = n - CRC_LEN;
  for (uint8_t i = 0; i < h.count; i++) {
    if (p + 3 > end) return false;
    uint8_t idLen = d[p+2];
    if (p + 3 + idLen > end) return false;
    cb(d[p], d[p+1], d + p + 3, idLen);
    p += 3 + idLen;
  }
  return true;
}

// Render a roster entry into the stream id the wired sketches would have used.
inline void streamIdFor(uint32_t unitId, uint8_t sensorKind, const uint8_t* id, uint8_t idLen, char* out, size_t outLen) {
  static const char* H = "0123456789abcdef";
  char hex[40]; size_t k = 0;
  for (uint8_t i = 0; i < idLen && k + 2 < sizeof(hex); i++) { hex[k++] = H[id[i] >> 4]; hex[k++] = H[id[i] & 0xF]; }
  hex[k] = 0;
  switch (sensorKind) {
    case SK_DS18B20: snprintf(out, outLen, "ds18b20:%s", hex); break;
    case SK_SHT30:   snprintf(out, outLen, "sht30:%s", hex); break;
    case SK_ANALOG:  snprintf(out, outLen, "%08lx:a%u", (unsigned long)unitId, idLen ? id[0] : 0); break;
    case SK_I2C:     snprintf(out, outLen, "%08lx:i2c%02x", (unsigned long)unitId, idLen ? id[0] : 0); break;
    case SK_ADS1115: snprintf(out, outLen, "%08lx:ads%02xc%u", (unsigned long)unitId, idLen ? id[0] : 0, idLen > 1 ? id[1] : 0); break;
    case SK_MODBUS:  snprintf(out, outLen, "%08lx:mb%u", (unsigned long)unitId, idLen ? id[0] : 0); break;
    case SK_SDI12:   snprintf(out, outLen, "sdi12:%s", hex); break;
    case SK_PULSE:   snprintf(out, outLen, "%08lx:p%u", (unsigned long)unitId, idLen ? id[0] : 0); break;
    case SK_CONTACT: snprintf(out, outLen, "%08lx:c%u", (unsigned long)unitId, idLen ? id[0] : 0); break;
    case SK_HX711:   snprintf(out, outLen, "%08lx:hx%u", (unsigned long)unitId, idLen ? id[0] : 0); break;
    case SK_RF433:   snprintf(out, outLen, "rf433:%s", hex); break;   // model code byte then the station's id bytes
    default:         snprintf(out, outLen, "%08lx", (unsigned long)unitId); break;
  }
}

// Radio settings both sides must share. Held here so a mismatch is a diff, not a hunt.
struct RadioPlan {
  float   freqMHz;      // US915 ISM. 915.0 sits mid-band, clear of LoRaWAN's uplink sub-band edges
  float   bwKHz;        // 125 = longer reach; 500 = 4x less airtime, ~6 dB less sensitivity
  uint8_t sf;           // 7 = campus/fast ... 10 = far/slow (SF11-12 exceed the US 400 ms dwell at 125 kHz)
  uint8_t cr;           // coding rate denominator, 5 = 4/5
  uint8_t syncWord;     // private network; 0x12 is the classic "private LoRa" word, never LoRaWAN's 0x34
  int8_t  txDbm;        // 17 dBm is plenty on a campus and kind to the PA; 20 for far units
  uint16_t preamble;    // symbols; 8 is the LoRa default the CAD detector is tuned for
};
static const RadioPlan DEFAULT_PLAN = { 915.0f, 125.0f, 7, 5, 0x12, 17, 8 };

}  // namespace oatlora
