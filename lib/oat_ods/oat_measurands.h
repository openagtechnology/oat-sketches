// oat_measurands.h — GENERATED from the oat-measurands vocabulary (https://openagriculturetechnology.com/standard/reference/)
// DO NOT EDIT BY HAND. Regenerate: python3 scripts/oat/gen_measurands.py
// -----------------------------------------------------------------------------
// The measurand dictionary the harvester uses to turn a decoder's raw output keys
// (tempc, hum, moi, co2 ...) into canonical oat-ods measurements + SenML units +
// an aggregation kind. ESP32 keeps const data in memory-mapped flash and reads it
// directly, so no PROGMEM/pgm_read gymnastics are needed here.
#pragma once
#include <Arduino.h>
#include <strings.h>   // strcasecmp

namespace oat {

enum MeasKind : uint8_t { KIND_CONTINUOUS=0, KIND_GAUGE=1, KIND_CUMULATIVE=2, KIND_STATE=3, KIND_EVENT=4 };
enum MeasXform : uint8_t { XF_NONE=0, XF_F_TO_C=1 };

// The oat-ods §6 agg.method string for a kind.
inline const char* kindAgg(uint8_t k) {
  switch (k) { case KIND_CONTINUOUS: return "mean"; case KIND_EVENT: return "sum"; default: return "last"; }
}
// Apply a raw-value transform (e.g. a °F-only device -> canonical °C).
inline double applyXform(uint8_t xf, double v) { return xf == XF_F_TO_C ? (v - 32.0) * 5.0 / 9.0 : v; }

struct Measurand {
  const char* key;          // decoder output key to match (case-insensitive)
  const char* measurement;  // canonical oat-ods measurement
  const char* unit;         // SenML unit symbol ("" = unitless)
  uint8_t     kind;         // MeasKind -> aggregation
  uint8_t     xform;        // MeasXform applied to the raw value
};

// 33 harvestable decoder keys, generated from 28 measurand rows.
static const Measurand OAT_MEASURANDS[] = {
  {"tempc","temperature","Cel",KIND_CONTINUOUS,XF_NONE},
  {"tempf","temperature","Cel",KIND_CONTINUOUS,XF_F_TO_C},
  {"hum","humidity","%RH",KIND_CONTINUOUS,XF_NONE},
  {"moi","soil_moisture","%",KIND_CONTINUOUS,XF_NONE},
  {"fer","soil_conductivity","uS/cm",KIND_CONTINUOUS,XF_NONE},
  {"lux","illuminance","lx",KIND_CONTINUOUS,XF_NONE},
  {"pres","pressure","hPa",KIND_CONTINUOUS,XF_NONE},
  {"co2","co2","ppm",KIND_CONTINUOUS,XF_NONE},
  {"pm25","pm25","ug/m3",KIND_CONTINUOUS,XF_NONE},
  {"pm10","pm10","ug/m3",KIND_CONTINUOUS,XF_NONE},
  {"pm1","pm1","ug/m3",KIND_CONTINUOUS,XF_NONE},
  {"tvoc","tvoc","ppb",KIND_CONTINUOUS,XF_NONE},
  {"hcho","formaldehyde","mg/m3",KIND_CONTINUOUS,XF_NONE},
  {"formaldehyde","formaldehyde","mg/m3",KIND_CONTINUOUS,XF_NONE},
  {"batt","battery","%",KIND_GAUGE,XF_NONE},
  {"volt","voltage","V",KIND_GAUGE,XF_NONE},
  {"cur","current","A",KIND_GAUGE,XF_NONE},
  {"current","current","A",KIND_GAUGE,XF_NONE},
  {"pow","power","W",KIND_GAUGE,XF_NONE},
  {"power","power","W",KIND_GAUGE,XF_NONE},
  {"weight","weight","kg",KIND_GAUGE,XF_NONE},
  {"level","level","%",KIND_GAUGE,XF_NONE},
  {"steps","steps","/",KIND_CUMULATIVE,XF_NONE},
  {"nrj","energy","Wh",KIND_CUMULATIVE,XF_NONE},
  {"energy","energy","Wh",KIND_CUMULATIVE,XF_NONE},
  {"open","contact","",KIND_STATE,XF_NONE},
  {"contact","contact","",KIND_STATE,XF_NONE},
  {"motion","motion","",KIND_STATE,XF_NONE},
  {"pir","motion","",KIND_STATE,XF_NONE},
  {"presence","presence","",KIND_STATE,XF_NONE},
  {"unlocked","lock","",KIND_STATE,XF_NONE},
  {"button","button","",KIND_EVENT,XF_NONE},
  {"btn","button","",KIND_EVENT,XF_NONE},
};
static const size_t OAT_MEASURANDS_N = sizeof(OAT_MEASURANDS) / sizeof(OAT_MEASURANDS[0]);

// Identity/metadata keys the decoder emits that are NEVER a measurement.
static const char* const OAT_META_KEYS[] = {
  "id", "name", "mac", "type", "model", "model_id", "brand", "device", "manufacturerdata", "servicedata", "servicedatauuid", "cidc", "acts", "track_id", "aeskey", "mfid", "major", "minor", "uuid", "txpower", "cdepth", "track", "prmac", "batt_case", "batt_l", "batt_r", "charging_case", "charging_l", "charging_r"
};
static const size_t OAT_META_KEYS_N = sizeof(OAT_META_KEYS) / sizeof(OAT_META_KEYS[0]);

// A numeric decoder key that is neither mapped nor meta is forwarded RAW when true.
static const bool OAT_FORWARD_UNKNOWN = true;

inline bool isMetaKey(const char* k) {
  for (size_t i = 0; i < OAT_META_KEYS_N; i++) if (!strcasecmp(k, OAT_META_KEYS[i])) return true;
  return false;
}

// Map a decoder key to its measurand. Returns true + fills `out` when known.
inline bool lookupMeasurand(const char* key, Measurand& out) {
  for (size_t i = 0; i < OAT_MEASURANDS_N; i++) {
    if (!strcasecmp(key, OAT_MEASURANDS[i].key)) { out = OAT_MEASURANDS[i]; return true; }
  }
  return false;
}

} // namespace oat
