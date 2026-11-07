// SPDX-FileCopyrightText: 2026 Open Agriculture Technology Collective
// SPDX-License-Identifier: Apache-2.0
/* =============================================================================
   oat_rf915 — hear Ecowitt / Ambient Weather (Fine Offset) FSK sensors through
   a CC1101 in packet mode
   -----------------------------------------------------------------------------
   The CC1101 does the radio work in hardware: 2-FSK demodulation, bit-clock
   recovery, the 0x2DD4 sync-word match and framing. The ESP32 polls the receive
   FIFO over SPI and gets whole frames with the chip's RSSI appended. Nothing here
   times a bit, and no interrupt is involved.

   A second path beside rtl_433_ESP's (the listener's esp32-fsk / esp32-wh51
   images, waiting on the bench since 2026-09-20, CO-18736). That path samples the
   demodulated line on GDO0 in an ISR and rebuilds the bits in software. On 9/20,
   through a 433-matched module at 915, it found the preamble, the sync and the
   families (WH31B 0x37, WN34 0x34) but ~3.5 % bit jitter kept frames off their
   checksums; the bench traced that jitter to the out-of-band front end, so a 915
   module may be all that path needs. This one does not depend on the answer: the
   chip times the bits, so no software sampling is involved.

   Frames (preamble aa.., sync 2d d4, then; layouts from rtl_433's decoders,
   ambientweather_wh31e.c, fineoffset.c WH51, fineoffset_wn34.c; this code is
   ours):
     0x30 WH31E / 0x37 WH31B  temp + humidity      CRC-8 over 0..4 at 5, SUM-8 at 6
     0x51 WH51                soil moisture         CRC-8 over 0..11 at 12, SUM-8 at 13
     0x34 WN34 / 0x38 WN38    probe temperature     CRC-8 over 0..6 at 7, SUM-8 at 8
     0xA2 WH52                soil moisture + temperature + EC   CRC-8 over 0..21 at 22, SUM-8 at 23
                              (fineoffset_wh52.c; EC = (20 bits >> 8) x 10 uS/cm, as the Ecowitt
                              gateway reports it; rtl_433's battery byte did not hold on our probe)
   CRC-8 is poly 0x31, init 0x00, not reflected. A frame must pass both checks.
   Each reading comes out as the field names rtl_433 gives it, so a sketch that
   maps rtl_433 output maps these unchanged and the stream ids match
   ("fineoffset-wh51:0d0b2a").

   Use: begin(pins, &spi, 915.0) once; loop() every pass; while (take(r)) file r.
   One receiver per board.
   ============================================================================= */
#pragma once
#include <Arduino.h>
#include <SPI.h>

#define OATRF915_VERSION "1.2.0"

namespace oatrf915 {

struct Pins { int sck = -1, miso = -1, mosi = -1, cs = -1; };

#define RF915_MAX_VALUES 6
struct Value { const char* key; double v; };
struct Reading {
  char    model[24];
  char    id[12];        // as rtl_433 prints it: hex text for the WH51, a number for the rest
  bool    idIsText;
  int     channel;       // -1 when the model has none
  int     rssi;          // dBm, the chip's level for this frame
  float   offsetKHz;     // the chip's estimate of how far off the band this sensor sits
  uint8_t n;
  Value   val[RF915_MAX_VALUES];
};

// ---- the chip ---------------------------------------------------------------
#define RF915_XOSC_MHZ 26.0
#define RF915_PKTLEN   24            // the longest frame we decode is the WH52's 24; shorter frames leave the rest unread noise
#define RF915_FRAME    (RF915_PKTLEN + 2)   // + RSSI + LQI/CRC status bytes

static const SPISettings SPI_CFG(4000000, MSBFIRST, SPI_MODE0);
static Pins      s_pins;
static SPIClass* s_spi = nullptr;
static bool      s_ok = false;
static uint8_t   s_version = 0;
static float     s_mhz = 915.0f, s_bwKHz = 270.8f, s_drateK = 17.24f, s_devKHz = 40.0f;
static uint32_t  s_restarts = 0, s_overflows = 0;
static bool      s_raw = false;
static Stream*   s_out = &Serial;

static bool ready() { uint32_t t = micros(); while (digitalRead(s_pins.miso)) if (micros() - t > 5000) return false; return true; }
static void strobe(uint8_t c) {
  s_spi->beginTransaction(SPI_CFG); digitalWrite(s_pins.cs, LOW); ready();
  s_spi->transfer(c); digitalWrite(s_pins.cs, HIGH); s_spi->endTransaction();
}
static void wr(uint8_t a, uint8_t v) {
  s_spi->beginTransaction(SPI_CFG); digitalWrite(s_pins.cs, LOW); ready();
  s_spi->transfer(a); s_spi->transfer(v); digitalWrite(s_pins.cs, HIGH); s_spi->endTransaction();
}
// Status registers (0x30-0x3D) share addresses with the strobes: read them with the burst bit.
static uint8_t rd(uint8_t a) {
  s_spi->beginTransaction(SPI_CFG); digitalWrite(s_pins.cs, LOW); ready();
  s_spi->transfer(a | (a >= 0x30 ? 0xC0 : 0x80)); uint8_t v = s_spi->transfer(0);
  digitalWrite(s_pins.cs, HIGH); s_spi->endTransaction();
  return v;
}
static void rdFifo(uint8_t* b, int n) {
  s_spi->beginTransaction(SPI_CFG); digitalWrite(s_pins.cs, LOW); ready();
  s_spi->transfer(0xFF);                                   // burst read of the RX FIFO
  for (int i = 0; i < n; i++) b[i] = s_spi->transfer(0);
  digitalWrite(s_pins.cs, HIGH); s_spi->endTransaction();
}
// CC1101 errata: a status register read while it changes can return garbage;
// read until two agree.
static uint8_t rdStable(uint8_t a) { uint8_t x = rd(a), y; while ((y = rd(a)) != x) x = y; return x; }
static int rssiDbm(uint8_t raw) { int r = raw >= 128 ? (int)raw - 256 : raw; return r / 2 - 74; }   // 74: datasheet offset, 868/915 table
static void rx() { strobe(0x36); strobe(0x3A); strobe(0x34); }   // IDLE, flush RX FIFO, RX (MCSM0 calibrates on the way)

static void setFreq(float mhz) {
  uint32_t f = (uint32_t) lround(mhz * 65536.0 / RF915_XOSC_MHZ);
  wr(0x0D, (f >> 16) & 0xFF); wr(0x0E, (f >> 8) & 0xFF); wr(0x0F, f & 0xFF);
  s_mhz = mhz;
}
// MDMCFG4 = bandwidth (E, M) high nibble + data-rate exponent; MDMCFG3 = data-rate mantissa.
static void setModem(float bwKHz, float drateK) {
  uint8_t be = 0, bm = 0; float best = 1e9;
  for (uint8_t e = 0; e < 4; e++) for (uint8_t m = 0; m < 4; m++) {
    float bw = RF915_XOSC_MHZ * 1000.0f / (8.0f * (4 + m) * (1 << e));
    if (fabsf(bw - bwKHz) < best) { best = fabsf(bw - bwKHz); be = e; bm = m; }
  }
  s_bwKHz = RF915_XOSC_MHZ * 1000.0f / (8.0f * (4 + bm) * (1 << be));
  double r = drateK * 1000.0 * 268435456.0 / (RF915_XOSC_MHZ * 1e6);
  int de = 0; while (de < 15 && r >= 512.0) { r /= 2.0; de++; }
  int dm = (int) lround(r - 256.0); if (dm < 0) dm = 0; if (dm > 255) dm = 255;
  s_drateK = (float)((256 + dm) * pow(2.0, de) * RF915_XOSC_MHZ * 1e6 / 268435456.0 / 1000.0);
  wr(0x10, (uint8_t)(be << 6 | bm << 4 | (de & 0x0F)));
  wr(0x11, (uint8_t)dm);
}
// DEVIATN: f_xosc / 2^17 * (8 + M) * 2^E. In 2-FSK receive it is the deviation the
// demodulator expects.
static void setDev(float kHz) {
  uint8_t be = 0, bm = 0; float best = 1e9;
  for (uint8_t e = 0; e < 8; e++) for (uint8_t m = 0; m < 8; m++) {
    float d = RF915_XOSC_MHZ * 1000.0f / 131072.0f * (8 + m) * (1 << e);
    if (fabsf(d - kHz) < best) { best = fabsf(d - kHz); be = e; bm = m; }
  }
  s_devKHz = RF915_XOSC_MHZ * 1000.0f / 131072.0f * (8 + bm) * (1 << be);
  wr(0x15, (uint8_t)(be << 4 | bm));
}

// Packet-mode 2-FSK receive. Unmarked values are SmartRF Studio's 915 MHz FSK
// defaults, written so a warm restart is clean.
static const uint8_t INIT[][2] = {
  { 0x00, 0x2E },   // IOCFG2   unused, high impedance
  { 0x01, 0x2E },   // IOCFG1   SO on this bus
  { 0x02, 0x06 },   // IOCFG0   GDO0 high from sync word to end of frame (a probe point; not read)
  { 0x03, 0x47 },   // FIFOTHR
  { 0x04, 0x2D },   // SYNC1    Fine Offset sync word 2d d4
  { 0x05, 0xD4 },   // SYNC0
  { 0x06, RF915_PKTLEN },   // PKTLEN  fixed length frames
  { 0x07, 0x04 },   // PKTCTRL1 append RSSI + LQI, no address check, sync search always on
  { 0x08, 0x00 },   // PKTCTRL0 no whitening, FIFO mode, no hardware CRC (the sensors' own checks are used), fixed length
  { 0x09, 0x00 },   // ADDR
  { 0x0A, 0x00 },   // CHANNR
  { 0x0B, 0x06 },   // FSCTRL1  IF 152 kHz
  { 0x0C, 0x00 },   // FSCTRL0
  { 0x12, 0x02 },   // MDMCFG2  2-FSK, DC filter on, no Manchester, 16 of 16 sync bits
  { 0x13, 0x22 },   // MDMCFG1
  { 0x14, 0xF8 },   // MDMCFG0
  { 0x16, 0x07 },   // MCSM2
  { 0x17, 0x3C },   // MCSM1    stay in RX after a frame: the FIFO holds three frames
  { 0x18, 0x18 },   // MCSM0    calibrate on IDLE -> RX
  { 0x19, 0x16 },   // FOCCFG   frequency offset compensation, +/- bw/4 (sensor crystals drift)
  { 0x1A, 0x6C },   // BSCFG
  { 0x1B, 0x43 },   // AGCCTRL2
  { 0x1C, 0x40 },   // AGCCTRL1
  { 0x1D, 0x91 },   // AGCCTRL0
  { 0x20, 0xFB },   // WORCTRL
  { 0x21, 0x56 },   // FREND1
  { 0x22, 0x10 },   // FREND0   (TX only)
  { 0x23, 0xE9 },   // FSCAL3
  { 0x24, 0x2A },   // FSCAL2
  { 0x25, 0x00 },   // FSCAL1
  { 0x26, 0x1F },   // FSCAL0
  { 0x29, 0x59 },   // FSTEST
  { 0x2C, 0x81 },   // TEST2
  { 0x2D, 0x35 },   // TEST1
  { 0x2E, 0x09 },   // TEST0
};

// ---- the frames -------------------------------------------------------------
static uint8_t crc8(const uint8_t* b, int n) {
  uint8_t c = 0;
  for (int i = 0; i < n; i++) { c ^= b[i]; for (int k = 0; k < 8; k++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1); }
  return c;
}
static uint8_t sum8(const uint8_t* b, int n) { uint8_t s = 0; for (int i = 0; i < n; i++) s += b[i]; return s; }

struct FamilyCount { uint8_t fam; uint32_t good, bad; };
static FamilyCount s_fam[8];
static uint32_t s_frames = 0, s_good = 0, s_noise = 0;
static FamilyCount& famFor(uint8_t f) {
  for (auto& c : s_fam) if (c.fam == f && (c.good || c.bad)) return c;
  for (auto& c : s_fam) if (!c.good && !c.bad) { c.fam = f; return c; }
  return s_fam[7];
}

#define RF915_QUEUE 4
static Reading s_q[RF915_QUEUE];
static uint8_t s_qHead = 0, s_qN = 0;

static void add(Reading& r, const char* k, double v) { if (r.n < RF915_MAX_VALUES) r.val[r.n++] = { k, v }; }

// One frame -> a reading, or nothing. Returns a short verdict for the raw log.
static const char* decode(const uint8_t* b, int rssi, float offKHz, Reading& r) {
  memset(&r, 0, sizeof(r)); r.channel = -1; r.rssi = rssi; r.offsetKHz = offKHz;
  switch (b[0]) {
    case 0x30: case 0x37: {
      if (crc8(b, 6) != 0 || sum8(b, 6) != b[6]) return "WH31 bad check";
      strcpy(r.model, b[0] == 0x30 ? "AmbientWeather-WH31E" : "AmbientWeather-WH31B");
      snprintf(r.id, sizeof(r.id), "%u", b[1]);
      r.channel = ((b[2] & 0x70) >> 4) + 1;
      add(r, "battery_ok", (b[2] & 0x04) ? 0 : 1);
      add(r, "temperature_C", ((((b[2] & 0x03) << 8) | b[3]) - 400) * 0.1);
      add(r, "humidity", b[4]);
      return "WH31 ok";
    }
    case 0x51: {
      if (crc8(b, 12) != b[12] || sum8(b, 13) != b[13]) return "WH51 bad check";
      strcpy(r.model, "Fineoffset-WH51");
      snprintf(r.id, sizeof(r.id), "%02x%02x%02x", b[1], b[2], b[3]); r.idIsText = true;
      int mv = (b[4] & 0x1F) * 100;
      int bits = b[4] & 0x1F;
      add(r, "battery_ok", bits >= 16 ? 1.0 : bits == 15 ? 0.9 : bits == 14 ? 0.5 : bits == 13 ? 0.1 : 0.0);
      add(r, "battery_mV", mv);
      add(r, "moisture", b[6]);
      add(r, "boost", (b[4] & 0xE0) >> 5);
      add(r, "ad_raw", ((b[7] & 0x01) << 8) | b[8]);
      return "WH51 ok";
    }
    case 0x34: case 0x38: {
      if (crc8(b, 7) != b[7] || sum8(b, 8) != b[8]) return "WN34 bad check";
      int sub = (b[4] & 0xF0) >> 4;
      int raw = (int16_t)(((b[4] & 0x0F) << 12) | (b[5] << 4)) >> 4;   // sign-extended 12 bits
      double t = (b[0] == 0x38 || sub == 4) ? raw * 0.1 : raw * 0.1 - 40.0;
      strcpy(r.model, b[0] == 0x38 ? "Fineoffset-WN38" : sub == 4 ? "Fineoffset-WN34D" : "Fineoffset-WN34");
      snprintf(r.id, sizeof(r.id), "%lu", (unsigned long)(((uint32_t)b[1] << 16) | (b[2] << 8) | b[3]));
      int mv = (b[6] & 0x7F) * 20;
      int bars = mv > 1440 ? 5 : mv > 1380 ? 4 : mv > 1300 ? 3 : mv > 1200 ? 2 : 1;
      add(r, "battery_ok", (bars - 1) * 0.25);
      add(r, "battery_mV", mv);
      add(r, "temperature_C", t);
      return "WN34 ok";
    }
    case 0xA2: {
      if (crc8(b, 22) != b[22] || sum8(b, 23) != b[23]) return "WH52 bad check";
      strcpy(r.model, "Fineoffset-WH52");
      snprintf(r.id, sizeof(r.id), "%02x%02x%02x", b[1], b[2], b[3]); r.idIsText = true;
      add(r, "temperature_C", ((((b[4] & 0x1F) << 8) | b[5]) * 0.1) - 40.0);
      add(r, "moisture", b[6]);
      uint32_t ec = ((uint32_t)(b[8] & 0x0F) << 16) | ((uint32_t)b[9] << 8) | b[10];
      // EC in 10 uS/cm steps from the 12 bits above byte 10, as the Ecowitt gateway reports it:
      // byte 9 = 8, 9, 32, 33 -> gateway 80, 90, 320, 330 (2026-11-06, two pots). rtl_433's
      // ec / 25.6 also counts byte 10 as a fraction, which read ~7 % high at 80-90 uS/cm.
      add(r, "conductivity", (double)(ec >> 8) * 10.0);
      // The moisture count, as an Ecowitt gateway posts it (soil_ec_hum_ad), like the WH51's ad.
      // No battery: on our probe byte 15 read 0x95 against the gateway's 1.58 V.
      add(r, "moisture_ad", ((b[8] & 0xF0) << 4) | b[7]);
      add(r, "boost", (b[4] & 0xE0) >> 5);
      return "WH52 ok";
    }
  }
  return nullptr;   // a family we do not decode, or noise that matched the sync word
}

static void handle(const uint8_t* f) {
  s_frames++;
  int rssi = rssiDbm(f[RF915_PKTLEN]);
  int8_t fe = (int8_t)rd(0x32);                          // FREQEST, two's complement, f_xosc / 2^14 per step
  float offKHz = fe * (RF915_XOSC_MHZ * 1000.0f / 16384.0f);
  Reading r;
  const char* verdict = decode(f, rssi, offKHz, r);
  bool ok = verdict && strstr(verdict, " ok");
  if (verdict) { FamilyCount& c = famFor(f[0]); if (ok) c.good++; else c.bad++; }
  else s_noise++;
  if (s_raw) {
    s_out->printf("[rf915] %4d dBm %+6.1f kHz  ", rssi, offKHz);
    for (int i = 0; i < RF915_PKTLEN; i++) s_out->printf("%02x ", f[i]);
    s_out->printf(" %s\n", verdict ? verdict : "(not a family we decode)");
  }
  if (!ok) return;
  s_good++;
  if (s_qN == RF915_QUEUE) { s_qHead = (s_qHead + 1) % RF915_QUEUE; s_qN--; }   // keep the newest
  s_q[(s_qHead + s_qN) % RF915_QUEUE] = r; s_qN++;
}

// ---- public -----------------------------------------------------------------
inline void end() { if (s_ok) strobe(0x36); s_ok = false; }

// False when there is no CC1101 on the bus (version reads 0x00 or 0xFF).
inline bool begin(const Pins& p, SPIClass* spi, float mhz) {
  end();
  s_pins = p; s_spi = spi;
  if (!spi || p.cs < 0 || p.sck < 0 || p.miso < 0 || p.mosi < 0) return false;
  pinMode(p.cs, OUTPUT); digitalWrite(p.cs, HIGH);
  spi->begin(p.sck, p.miso, p.mosi, -1);
  digitalWrite(p.cs, LOW); delayMicroseconds(10); digitalWrite(p.cs, HIGH); delayMicroseconds(45);   // manual power-on reset, SWRS061 19.1.2
  strobe(0x30); delay(1);
  s_version = rd(0x31);
  if (s_version == 0x00 || s_version == 0xFF) { spi->end(); pinMode(p.cs, INPUT); return false; }
  for (auto& r : INIT) wr(r[0], r[1]);
  setFreq(mhz); setModem(s_bwKHz, s_drateK); setDev(s_devKHz);
  rx(); delay(2);
  s_ok = rd(0x35) == 0x0D;
  return s_ok;
}
inline bool running() { return s_ok; }
inline bool retune(float mhz) { if (!s_ok) return false; strobe(0x36); setFreq(mhz); rx(); return true; }

// Every pass of the sketch's loop: lift whole frames out of the FIFO. Only complete
// frames are read, so the FIFO is never emptied under a frame still arriving (errata).
inline void loop() {
  if (!s_ok) return;
  uint8_t n = rdStable(0x3B);
  if (n & 0x80) { s_overflows++; rx(); return; }        // more than three frames waited: start clean
  uint8_t f[RF915_FRAME];
  while ((n & 0x7F) >= RF915_FRAME) {
    rdFifo(f, RF915_FRAME);
    handle(f);
    n = rdStable(0x3B);
    if (n & 0x80) { s_overflows++; rx(); return; }
  }
  static uint32_t lastChk = 0;                            // the chip should never leave RX; if it does, put it back
  if (millis() - lastChk > 5000) {
    lastChk = millis();
    uint8_t st = rd(0x35);
    if (st != 0x0D && st != 0x0E && st != 0x0F) { rx(); s_restarts++; }
  }
}

inline bool take(Reading& r) {
  if (!s_qN) return false;
  r = s_q[s_qHead]; s_qHead = (s_qHead + 1) % RF915_QUEUE; s_qN--;
  return true;
}

struct Stats { uint32_t frames, good, noise, overflows, restarts; };
inline Stats stats() { return Stats{ s_frames, s_good, s_noise, s_overflows, s_restarts }; }
inline uint8_t version() { return s_version; }
inline uint8_t state()   { return s_ok ? rd(0x35) : 0; }
inline float   bwKHz()   { return s_bwKHz; }
inline float   drateK()  { return s_drateK; }
inline float   devKHz()  { return s_devKHz; }
inline int     rssiNow() { return s_ok ? rssiDbm(rd(0x34)) : 0; }

// ---- console tools ----------------------------------------------------------
// Families heard: good and failed-check counts per family byte. A family with only
// failures says the sensor is heard but its bits are not clean: band, deviation, range.
inline void printFamilies(Stream& out) {
  out.printf("rf915 frames=%lu decoded=%lu other=%lu overflows=%lu restarts=%lu\n",
             (unsigned long)s_frames, (unsigned long)s_good, (unsigned long)s_noise, (unsigned long)s_overflows, (unsigned long)s_restarts);
  for (auto& c : s_fam) if (c.good || c.bad) out.printf("  family 0x%02x good=%lu failed_check=%lu\n", c.fam, (unsigned long)c.good, (unsigned long)c.bad);
}
inline void printRegs(Stream& out) {
  if (!s_ok) { out.println("no CC1101 running"); return; }
  out.printf("CC1101 version 0x%02x marcstate 0x%02x rssi %d dBm  %.3f MHz bw %.1f kHz drate %.2f kBd dev %.1f kHz\n",
             rd(0x31), rd(0x35), rssiDbm(rd(0x34)), s_mhz, s_bwKHz, s_drateK, s_devKHz);
  for (uint8_t a = 0; a <= 0x2E; a++) out.printf("%02x:%02x%s", a, rd(a), (a % 8 == 7) ? "\n" : "  ");
  out.println();
}
inline void sampleRssi(Stream& out) {
  if (!s_ok) { out.println("no CC1101 running"); return; }
  int lo = 0, hi = -200; long sum = 0;
  for (int i = 0; i < 50; i++) { int v = rssiDbm(rd(0x34)); sum += v; if (v > hi) hi = v; if (i == 0 || v < lo) lo = v; delay(2); }
  out.printf("rssi over 100 ms: avg %ld  min %d  max %d dBm (a quiet 915 band reads about -100 to -110; a LoRa node on 915.0 nearby shows as a steady high level)\n", sum / 50, lo, hi);
}
inline void setRaw(bool on, Stream& out) { s_raw = on; s_out = &out; }
inline bool raw() { return s_raw; }
// Live tuning, not saved: tune bw|drate|dev <value>.
inline void tune(const String& a, Stream& out) {
  String t = a; t.trim(); int sp = t.indexOf(' ');
  if (!s_ok) { out.println("no CC1101 running"); return; }
  if (sp < 0) { out.printf("bw %.1f kHz, drate %.2f kBd, dev %.1f kHz. usage: tune bw 270 | tune drate 17.24 | tune dev 40\n", s_bwKHz, s_drateK, s_devKHz); return; }
  String k = t.substring(0, sp); float v = t.substring(sp + 1).toFloat();
  strobe(0x36);
  if (k == "bw" && v >= 58 && v <= 812.5f) setModem(v, s_drateK);
  else if (k == "drate" && v >= 0.6f && v <= 500) setModem(s_bwKHz, v);
  else if (k == "dev" && v >= 1.5f && v <= 380) setDev(v);
  else { rx(); out.println("usage: tune bw <58-812> | tune drate <kBd> | tune dev <kHz>"); return; }
  rx();
  out.printf("now bw %.1f kHz, drate %.2f kBd, dev %.1f kHz (live, not saved)\n", s_bwKHz, s_drateK, s_devKHz);
}

}  // namespace oatrf915
