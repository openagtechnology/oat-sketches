/* single_probe.ino — the smallest possible OAT sketch: one temperature stream.
 *
 * The point of this example is how LITTLE a new device needs. Fill a Device
 * (once) and a Reading (per sample), call oat::encodeNative(), POST it. Swap the
 * faked read below for a real DS18B20 / analog / I2C sensor and nothing else
 * changes — the envelope comes from the shared oat_ods module. That is "one
 * envelope, all sketches" (the oat-ods Core Contract).
 *
 * Build: place this repo's lib/oat_ods on the include path (PlatformIO finds a
 * project-local lib/ automatically). Depends on ArduinoJson >= 7.
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <oat_ods.h>

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PW   = "your-pass";
const char* ENDPOINT  = "https://example.org/ingest";   // your oat-ods endpoint

// Node identity — set once. gateway_id is assignable (NOT the chip MAC), so a
// replacement board with the same id keeps the streams flowing.
oat::Device dev = { "oat-bench-probe", "oat-1wire", "oat-single-probe/0.1.0" };

static String isoNowUTC() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";          // clock not synced yet
  struct tm t; gmtime_r(&now, &t);
  char b[24]; strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(b);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PW);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  configTime(0, 0, "pool.ntp.org");
}

void loop() {
  // 1) read the sensor (DS18B20 / analog / I2C). Faked here:
  double tempC = 21.5 + (millis() % 1000) / 500.0;

  // 2) describe it — logical stream (the place) + physical source (the gadget)
  String iso = isoNowUTC();
  oat::Reading r;
  r.stream_id   = "bench-probe-air";        // the place — never changes
  r.stream_name = "Bench Probe Air";
  r.location    = "Propagation bench";
  r.measurement = "temperature";
  r.unit        = "Cel";
  r.value       = tempC;
  r.physical_id = "28-0000071f3b2c";        // the DS18B20's 1-Wire ROM id (swap a dead probe -> change only this)
  r.model       = "DS18B20";
  r.observed_at = iso.c_str();

  // 3) encode one message per reading, and send it
  String body;
  oat::encodeNative(dev, r, body);

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure tls; tls.setInsecure();   // pin a cert for production
    HTTPClient http;
    if (http.begin(tls, ENDPOINT)) {
      http.addHeader("Content-Type", "application/json");
      int code = http.POST(body);
      Serial.printf("[oat] POST %d  %s\n", code, body.c_str());
      http.end();
    }
  }
  delay(60000);   // one reading per minute
}
