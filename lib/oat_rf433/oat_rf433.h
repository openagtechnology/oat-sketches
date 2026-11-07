// SPDX-FileCopyrightText: 2026 Open Agriculture Technology Collective
// SPDX-License-Identifier: Apache-2.0
/* =============================================================================
   oat_rf433 — hear AcuRite 5-in-1 weather stations on 433.92 MHz through a
   receiver that puts the demodulated signal on one GPIO
   -----------------------------------------------------------------------------
   Two front ends, one decoder:
     FRONT_BARE    a $2 superheterodyne receiver module, DATA on a pin;
     FRONT_CC1101  a CC1101 on SPI, programmed here as a plain OOK receiver whose
                   asynchronous data output (GDO0) is the pin. No rtl_433 library:
                   its signal-strength gate drops every on-off-keying 0 bit
                   (2026-09-20 bench), and this path has no gate.
   The decoder, the new-station and plausibility guards, the CC1101 register set
   and the console tools were built and proven on the Radio Sensor Listener's
   bench, 2026-09-26 (gweather-2, listener 1.2.x); they moved here unchanged so the
   listener and the LoRa field node run the same code. Deploy by including, never
   by copying a second decoder into a sketch.

   Use: begin(front, pins, &spi, 433.92, "tag") once; loop() every pass of the
   sketch's loop; while (take(r)) file r. One receiver per board (the ISR and the
   state are file-level). Bench tools compile only with -DOAT_RX_BENCH.
   ============================================================================= */
#pragma once
#include <Arduino.h>
#include <SPI.h>

#define OATRF_VERSION "1.0.0"

namespace oatrf {

enum Front : uint8_t { FRONT_NONE = 0, FRONT_BARE = 1, FRONT_CC1101 = 2 };
struct Pins { int sck = -1, miso = -1, mosi = -1, cs = -1, data = -1; };

// One decoded, checked reading. sid is the stream id the Radio Sensor Listener
// has always used for the station ("acurite-5n1:748-A"): the station's own id is
// the stream id wherever the reading goes, so a station keeps one history.
struct Reading {
  char     sid[24];
  uint16_t id; char ch; uint8_t type;                  // type 0x31 wind+dir+rain, 0x38 wind+temp+humidity
  bool     batteryOk; float windKmh;
  bool     hasDir;  float dirDeg;
  bool     hasRain; float rainIn;                      // the station's running tip count, 0.01 in per tip
  bool     hasTH;   float tempF, hum;
  bool     hasRssi; int rssi;                          // CC1101 only: the peak while this frame arrived
};

static Front     s_front = FRONT_NONE;
static Pins      s_pins;
static SPIClass* s_spi = nullptr;
static bool      s_ok = false;
static Stream*   s_log = &Serial;
static const char* s_tag = "rf433";
static uint32_t  s_skipped = 0;

struct Sane { uint16_t key; bool used, haveT, haveR, pendT, pendR; float t, h, r, pt, ph, pr; uint32_t tMs, rMs, pendTMs, pendRMs, lastMs; };

// ---------------------------------------------------------------------------
// The data-pin receivers: a bare 433 MHz superheterodyne module (build 1) or a
// CC1101 programmed as a receiver (build 4), each putting the demodulated signal
// on one GPIO. Both feed ONE decoder, below: a gap filter in the ISR, then the
// AcuRite 5-in-1 decoded on its bit clock. The 5-in-1 (Iris 06004M, rtl_433's
// acurite.c): four ~600 us sync pulses, then 64 bits, each starting ~628 us after
// the last (408 high + 220 low = 1, 220 high + 408 low = 0); checksum = sum of
// bytes 0..6 mod 256 == byte 7; bytes 2..6 carry even parity. Two messages
// alternate every ~18 s: 0x31 wind + direction + rain counter, 0x38 wind +
// temperature + humidity, each sent three times back to back. The decoded
// fields are handed to the SAME map as the rtl_433 path.
//
// Every number here came off the 2026-09-26 bench (`pulses`, gweather-2): a
// receiver's slicer shrinks short pulses (under 120 us, sometimes to nothing),
// merges two pulses when it swallows the gap between them, and adds 50-110 us
// spikes to the gaps. A decoder that judges pulses by width alone lost most
// frames to that; judged by WHEN they start, nearly all come through. 1.1.x's
// width-only decoder (rtl_433's windows) is what build 1 shipped until 1.3.0.
// ---------------------------------------------------------------------------
#define DP_SYNC_LO   500
#define DP_SYNC_HI   750
#define DP_RESET_US  10000
#define DP_BITS      64
#define DP_PERIOD    628        // the bit period (408 + 220 us); learned per frame from the bits themselves
#define DP_BEAT_TOL  120        // a bit's rise must land within this of the expected beat
#define DP_MAX_GHOST 3          // beats with no pulse at all that a frame may fill with a 0
#define DP_MIN_PULSE 30         // anything this long reaches the decoder; the bit clock decides if it is noise
#define DP_JOIN_US   100        // a piece starting this soon after the last pulse ended is part of it
#define DP_FIRST_LO  150        // first data bit: its rise this long after the last sync pulse ends...
#define DP_FIRST_HI  900        // ...and no later than this
// Width split between a 0 and a 1 at 315 us, the midpoint of the bench's two peaks
// (200-239 and 380-419); checksum + parity catch a wrong call. `win` (bench) moves them.
static volatile uint16_t W_SYNC_LO = DP_SYNC_LO, W_SYNC_HI = DP_SYNC_HI, W_LONG_LO = 315, W_LONG_HI = 540;
// Gap filter (the ISR). A low shorter than g_glitchUs inside a pulse is a dropout
// and is bridged; 130 also bridged the real ~220 us gap between bits whenever it
// shrank, so 80. A pulse shorter than g_spikeUs never counts toward a sync train.
static volatile uint32_t g_glitchUs = 80, g_spikeUs = 120;

static volatile uint8_t  dp_state = 0, dp_sync = 0, dp_bits = 0, dp_ghosts = 0;
static volatile uint8_t  dp_buf[8], dp_frame[8];
static volatile bool     dp_ready = false;
static volatile uint32_t dp_frames = 0, dp_badcrc = 0, dp_badparity = 0, dp_glitches = 0;
static volatile uint32_t dp_lastRise = 0, dp_lastEnd = 0, dp_prevRise = 0, dp_period = DP_PERIOD;

// `pulses`: where in-frame pulse widths land, and why the last frames were lost.
#define PH_BINS 50                                      // 20 us bins, 0-1000 us
static volatile uint16_t ph_hist[PH_BINS];
struct Miss { uint8_t bit; uint16_t w, gap; uint8_t drops; };
static volatile Miss     ph_miss[16];
static volatile uint8_t  ph_missN = 0, dg_lastDrops = 0;
static volatile uint32_t ph_missTotal = 0, ph_ghosts = 0, ph_merged = 0, ph_unconfirmed = 0, ph_held = 0;
static uint32_t          ph_type31 = 0, ph_type38 = 0;

#ifdef OAT_RX_BENCH
// `capture` (bench images): the raw pulses (start, width) of the next frames lost
// past bit 16, and of one good frame, to replay a real failure off the board.
#define CAP_RING 160
struct CapP { uint32_t s; uint16_t w; };
struct Cap  { uint8_t bits; uint8_t n; uint8_t buf[8]; CapP p[CAP_RING]; };
static CapP              cap_ring[CAP_RING];
static volatile uint16_t cap_head = 0, cap_frameStart = 0;
static Cap               caps[3];                      // 0,1 = lost frames, 2 = a good one
static volatile uint8_t  cap_lostWant = 0, cap_lostHave = 0;
static volatile bool     cap_goodWant = false, cap_goodHave = false;
static void IRAM_ATTR capSnap(Cap& c) {
  uint16_t n = cap_head - cap_frameStart; if (n > CAP_RING) n = CAP_RING;
  c.n = n; c.bits = dp_bits; for (int i = 0; i < 8; i++) c.buf[i] = dp_buf[i];
  for (uint16_t i = 0; i < n; i++) { const CapP& q = cap_ring[(uint16_t)(cap_head - n + i) % CAP_RING]; c.p[i].s = q.s; c.p[i].w = q.w; }
}
#endif

static inline void IRAM_ATTR dpReset() { dp_state = 0; dp_sync = 0; dp_bits = 0; }
static inline void IRAM_ATTR dpSetBit(uint8_t n, bool one) {
  uint8_t bytepos = n >> 3, bit = 7 - (n & 7);
  if (one) dp_buf[bytepos] |= (1 << bit); else dp_buf[bytepos] &= ~(1 << bit);
}
static inline void IRAM_ATTR dpHist(uint32_t w) { uint32_t b = w / 20; if (b >= PH_BINS) b = PH_BINS - 1; if (ph_hist[b] < 65535) ph_hist[b]++; }
static inline uint32_t IRAM_ATTR dpOff(uint32_t s, uint32_t beat) { return s > beat ? s - beat : beat - s; }
static inline void IRAM_ATTR dpLost(uint32_t w, uint32_t dt) {
#ifdef OAT_RX_BENCH
  if (cap_lostWant && dp_bits >= 16) { capSnap(caps[cap_lostHave < 2 ? cap_lostHave : 1]); cap_lostHave++; cap_lostWant--; }
#endif
  if (dp_bits >= 1) {
    volatile Miss& m = ph_miss[ph_missN % 16];
    m.bit = dp_bits; m.w = w > 65535 ? 65535 : w; m.gap = dt > 65535 ? 65535 : dt; m.drops = dg_lastDrops;
    ph_missN++; ph_missTotal++;
  }
  dpReset();
}
static inline void IRAM_ATTR dpDone() {
  if (!dp_ready) { for (int i = 0; i < 8; i++) dp_frame[i] = dp_buf[i]; dp_ready = true; }
#ifdef OAT_RX_BENCH
  if (cap_goodWant) { capSnap(caps[2]); cap_goodWant = false; cap_goodHave = true; }
#endif
  dp_state = 0;
}

// One pulse (rise time s, width w) into the frame. Inside a frame it is judged by
// when it starts:
//   on the beat (the learned period after the last bit's rise, +/- DP_BEAT_TOL)
//        -> the next bit, classified by width, however thin it has shrunk; a pulse
//           spanning two or three beats (the slicer swallowed the ~220 us gap after
//           a long) is split: every bit but the last was a 1;
//   early, and close behind the last pulse's end -> a split piece of that pulse;
//   early, and nearer the last bit's own beat than the pulse taken for it
//        -> that pulse was a spike: this one replaces it;
//   early otherwise -> a spike, ignored;
//   late by whole beats -> the missing pulses were shorts that shrank to nothing:
//        filled with 0s (a wrong fill always fails the checksum); else the frame is lost.
// The sync train is found by width (four ~600 us pulses).
static void IRAM_ATTR dpPulseAt(uint32_t s, uint32_t w) {
#ifdef OAT_RX_BENCH
  { CapP& q = cap_ring[cap_head % CAP_RING]; q.s = s; q.w = w > 65535 ? 65535 : w; cap_head++; }
#endif
  if (dp_bits >= DP_BITS) dpReset();
  if (dp_state == 2) {
    if (dp_bits == 0) {                                 // the first data bit, timed from the end of the sync train
      uint32_t d0 = s - dp_lastEnd;
      if (d0 < DP_FIRST_LO) return;                     // a spike straight after sync
      uint8_t lead = d0 <= DP_FIRST_HI ? 0 : (d0 <= DP_FIRST_HI + DP_PERIOD ? 1 : 2);   // 1 = the first bit's pulse vanished
      if (lead < 2 && w <= W_LONG_HI) {
        dpHist(w);
        dp_ghosts = lead; if (lead) { dpSetBit(0, false); ph_ghosts++; }
        dpSetBit(lead, w >= W_LONG_LO); dp_bits = lead + 1; dp_prevRise = dp_lastRise = s; dp_lastEnd = s + w; dp_period = DP_PERIOD;
        return;
      }
      dpLost(w, d0);                                    // no first bit: treat this pulse as a possible sync below
    } else {
      uint32_t beat = dp_lastRise + dp_period;
      if (dpOff(s, beat) <= DP_BEAT_TOL) {              // on the beat: the next bit
        dpHist(w);
        if (w > W_LONG_HI) {
          uint32_t k = 1; while (k < 3 && w > k * dp_period + W_LONG_HI) k++;
          if (w < k * dp_period + 60 || dp_bits + k + 1 > DP_BITS) { dpLost(w, s - dp_lastRise); return; }
          for (uint32_t i = 0; i < k; i++) { dpSetBit(dp_bits, true); dp_bits++; }
          dpSetBit(dp_bits, (w - k * dp_period) >= W_LONG_LO); dp_bits++; ph_merged++;
          dp_prevRise = s + (k - 1) * dp_period; dp_lastRise = s + k * dp_period; dp_lastEnd = s + w;
          if (dp_bits >= DP_BITS) dpDone();
          return;
        }
        dp_period = (dp_period * 7 + (s - dp_lastRise)) / 8;
        dpSetBit(dp_bits, w >= W_LONG_LO); dp_bits++; dp_prevRise = dp_lastRise; dp_lastRise = s; dp_lastEnd = s + w;
        if (dp_bits >= DP_BITS) dpDone();
        return;
      }
      if ((int32_t)(s - beat) < 0) {                    // early
        if (s - dp_lastEnd <= DP_JOIN_US && (s + w) - dp_lastRise <= W_LONG_HI) {
          dpSetBit(dp_bits - 1, (s + w) - dp_lastRise >= W_LONG_LO); dp_lastEnd = s + w;
          return;
        }
        if (dp_bits >= 2) {
          uint32_t want = dp_prevRise + dp_period;
          if (dpOff(s, want) < dpOff(dp_lastRise, want) && dpOff(s, want) <= DP_BEAT_TOL && w <= W_LONG_HI) {
            dpSetBit(dp_bits - 1, w >= W_LONG_LO); dp_lastRise = s; dp_lastEnd = s + w;
            return;
          }
        }
        return;
      }
      uint32_t dt = s - dp_lastRise, k = (dt + dp_period / 2) / dp_period;
      if (k >= 2 && k <= 3 && dpOff(dt, k * dp_period) <= DP_BEAT_TOL && w <= W_LONG_HI
          && dp_ghosts + (k - 1) <= DP_MAX_GHOST && dp_bits + k <= DP_BITS) {
        for (uint32_t g = 1; g < k; g++) { dpSetBit(dp_bits, false); dp_bits++; }
        dp_ghosts += k - 1; ph_ghosts += k - 1;
        dpHist(w);
        dpSetBit(dp_bits, w >= W_LONG_LO); dp_bits++; dp_prevRise = s - dp_period; dp_lastRise = s; dp_lastEnd = s + w;
        if (dp_bits >= DP_BITS) dpDone();
        return;
      }
      dpLost(w, dt);                                    // lost; this pulse may be the next repeat's sync
    }
  }
  if (w < g_spikeUs) return;                            // a spike does not break a sync train
  if (w > W_SYNC_LO && w < W_SYNC_HI) {
    if (dp_state == 1 && s - dp_lastRise > 1600) dp_sync = 0;
    dp_state = 1; dp_lastRise = s; dp_lastEnd = s + w;
    if (++dp_sync >= 4) {
      dp_state = 2; dp_sync = 0; dp_bits = 0; dp_ghosts = 0;
#ifdef OAT_RX_BENCH
      cap_frameStart = cap_head - 4;
#endif
    }
  } else { dp_state = 0; dp_sync = 0; }
}

// The gap filter. A pulse starts on a rising edge and ends on a falling edge only
// once the line has then STAYED low for g_glitchUs; a shorter low is a dropout and
// the pulse carries on through it, wherever in the pulse it falls.
static volatile uint32_t dg_start = 0, dg_fall = 0;
static volatile uint8_t  dg_drops = 0;
static volatile bool     dg_inPulse = false, dg_fallPending = false;
static portMUX_TYPE      dg_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void IRAM_ATTR dgFinish() {           // the pending fall stood: the pulse is over
  uint32_t w = dg_fall - dg_start;
  dg_lastDrops = dg_drops;
  if (w >= DP_MIN_PULSE) dpPulseAt(dg_start, w); else dp_glitches++;
  dg_inPulse = false; dg_fallPending = false;
}
static void IRAM_ATTR dpIsr() {
  portENTER_CRITICAL_ISR(&dg_mux);
  uint32_t now = micros();
  if (digitalRead(s_pins.data)) {                  // rising
    if (dg_fallPending) {
      if (now - dg_fall < g_glitchUs) { dg_fallPending = false; dp_glitches++; if (dg_drops < 255) dg_drops++; portEXIT_CRITICAL_ISR(&dg_mux); return; }   // a dropout: same pulse
      dgFinish();
      if (now - dg_fall > DP_RESET_US) dpReset();
    }
    if (!dg_inPulse) { dg_start = now; dg_inPulse = true; dg_drops = 0; }
  } else if (dg_inPulse) {                            // falling: maybe the end, maybe a dropout
    dg_fall = now; dg_fallPending = true;
  }
  portEXIT_CRITICAL_ISR(&dg_mux);
}
// Loop side: close a pulse whose fall has stood (the last pulse of a frame has no
// rise after it). 1.2.5 also finished a frame one bit short by taking the last bit
// from the checksum; it completed none in ten bench minutes, and filling a bit
// from the checksum weakens it (the host sim passed more wrong frames), so 1.3.0
// dropped it.
static void dpLoopSide() {
  portENTER_CRITICAL(&dg_mux);
  if (dg_fallPending && micros() - dg_fall >= g_glitchUs) dgFinish();
  portEXIT_CRITICAL(&dg_mux);
}

// ---------------------------------------------------------------------------
// Build 4: the CC1101 as the receiver. Registers are written here, over SPI,
// from the datasheet (SWRS061) and TI DN022; nothing is borrowed from a driver
// library, so every value the chip runs on is on this page and `regs` prints it.
// ---------------------------------------------------------------------------
#define CC_XOSC_MHZ 26.0
#define CC_SRES 0x30
#define CC_SRX  0x34
#define CC_SIDLE 0x36
#define CC_PARTNUM 0x30
#define CC_VERSION 0x31
#define CC_RSSI    0x34
#define CC_MARCSTATE 0x35
#define CC_MARC_RX 0x0D

static const SPISettings CC_SPI(4000000, MSBFIRST, SPI_MODE0);
static uint8_t  cc_version = 0;
static float    cc_bwKHz = 203.125f, cc_drateK = 5.0f;
static uint32_t cc_restarts = 0;

// After CS goes low the chip holds SO high until its crystal is running.
static bool ccReady() { uint32_t t = micros(); while (digitalRead(s_pins.miso)) if (micros() - t > 5000) return false; return true; }
static void ccStrobe(uint8_t c) {
  s_spi->beginTransaction(CC_SPI); digitalWrite(s_pins.cs, LOW); ccReady();
  s_spi->transfer(c); digitalWrite(s_pins.cs, HIGH); s_spi->endTransaction();
}
static void ccWrite(uint8_t a, uint8_t v) {
  s_spi->beginTransaction(CC_SPI); digitalWrite(s_pins.cs, LOW); ccReady();
  s_spi->transfer(a); s_spi->transfer(v); digitalWrite(s_pins.cs, HIGH); s_spi->endTransaction();
}
// Status registers (0x30-0x3D) share addresses with the strobes and are read with the burst bit.
static uint8_t ccRead(uint8_t a) {
  s_spi->beginTransaction(CC_SPI); digitalWrite(s_pins.cs, LOW); ccReady();
  s_spi->transfer(a | (a >= 0x30 ? 0xC0 : 0x80)); uint8_t v = s_spi->transfer(0);
  digitalWrite(s_pins.cs, HIGH); s_spi->endTransaction();
  return v;
}
static int ccRssi() { int r = ccRead(CC_RSSI); return (r >= 128 ? (r - 256) / 2 : r / 2) - 74; }   // 74 = the datasheet's offset at 433 MHz
static void ccRx() { ccStrobe(CC_SIDLE); ccStrobe(CC_SRX); }                                     // MCSM0 recalibrates on IDLE -> RX

static void ccSetFreq(float mhz) {
  uint32_t f = (uint32_t) lround(mhz * 65536.0 / CC_XOSC_MHZ);
  ccWrite(0x0D, (f >> 16) & 0xFF); ccWrite(0x0E, (f >> 8) & 0xFF); ccWrite(0x0F, f & 0xFF);
}
// MDMCFG4 = channel bandwidth (E, M) in the high nibble + data-rate exponent in the
// low; MDMCFG3 = data-rate mantissa. BW = f_xosc / (8 (4 + M) 2^E).
static void ccSetModem(float bwKHz, float drateK) {
  uint8_t be = 0, bm = 0; float best = 1e9;
  for (uint8_t e = 0; e < 4; e++) for (uint8_t m = 0; m < 4; m++) {
    float bw = CC_XOSC_MHZ * 1000.0f / (8.0f * (4 + m) * (1 << e));
    if (fabsf(bw - bwKHz) < best) { best = fabsf(bw - bwKHz); be = e; bm = m; }
  }
  cc_bwKHz = CC_XOSC_MHZ * 1000.0f / (8.0f * (4 + bm) * (1 << be));
  double r = drateK * 1000.0 * 268435456.0 / (CC_XOSC_MHZ * 1e6);   // (256 + M) 2^E
  int de = 0; while (de < 15 && r >= 512.0) { r /= 2.0; de++; }
  int dm = (int) lround(r - 256.0); if (dm < 0) dm = 0; if (dm > 255) dm = 255;
  cc_drateK = (float)((256 + dm) * pow(2.0, de) * CC_XOSC_MHZ * 1e6 / 268435456.0 / 1000.0);
  ccWrite(0x10, (uint8_t)(be << 6 | bm << 4 | (de & 0x0F)));
  ccWrite(0x11, (uint8_t)dm);
}

// The receive configuration. Values not marked are the datasheet reset values or
// SmartRF Studio's 433 MHz defaults, written anyway so a warm restart is clean.
static const uint8_t CC_INIT[][2] = {
  { 0x00, 0x0D },   // IOCFG2   GDO2 = asynchronous serial data out too (a second probe point)
  { 0x01, 0x2E },   // IOCFG1   GDO1 is SO on this bus: high-impedance when CS is high
  { 0x02, 0x0D },   // IOCFG0   GDO0 = asynchronous serial data out -> OAT_RX_DATAPIN
  { 0x03, 0x47 },   // FIFOTHR
  { 0x07, 0x00 },   // PKTCTRL1 no status append, no address check
  { 0x08, 0x32 },   // PKTCTRL0 asynchronous serial mode, infinite length, no CRC
  { 0x0A, 0x00 },   // CHANNR
  { 0x0B, 0x06 },   // FSCTRL1  IF 152 kHz
  { 0x0C, 0x00 },   // FSCTRL0
  { 0x12, 0x30 },   // MDMCFG2  ASK/OOK, no Manchester, no sync word (the decoder finds the frame)
  { 0x13, 0x02 },   // MDMCFG1
  { 0x14, 0xF8 },   // MDMCFG0
  { 0x15, 0x47 },   // DEVIATN  (FSK only)
  { 0x16, 0x07 },   // MCSM2
  { 0x17, 0x30 },   // MCSM1
  { 0x18, 0x18 },   // MCSM0    calibrate on IDLE -> RX
  { 0x19, 0x16 },   // FOCCFG
  { 0x1A, 0x6C },   // BSCFG
  { 0x1B, 0x03 },   // AGCCTRL2 DN022 OOK: all DVGA gain, max LNA gain, 33 dB target. rtl_433_ESP's 0xC7 costs ~18 dB.
  { 0x1C, 0x00 },   // AGCCTRL1 DN022 OOK: LNA gain first, carrier sense off
  { 0x1D, 0x91 },   // AGCCTRL0 DN022 OOK: medium hysteresis, 16-sample wait, 8 dB OOK decision boundary (0xB2 = 12 dB, never toggled)
  { 0x20, 0xFB },   // WORCTRL
  { 0x21, 0x56 },   // FREND1
  { 0x22, 0x11 },   // FREND0   (TX only)
  { 0x23, 0xE9 },   // FSCAL3
  { 0x24, 0x2A },   // FSCAL2
  { 0x25, 0x00 },   // FSCAL1
  { 0x26, 0x1F },   // FSCAL0
  { 0x29, 0x59 },   // FSTEST
  { 0x2C, 0x81 },   // TEST2    SmartRF: better sensitivity at these rates
  { 0x2D, 0x35 },   // TEST1
  { 0x2E, 0x09 },   // TEST0
};

static bool ccBegin(float mhz) {
  pinMode(s_pins.cs, OUTPUT); digitalWrite(s_pins.cs, HIGH);
  s_spi->begin(s_pins.sck, s_pins.miso, s_pins.mosi, -1);
  // Manual power-on reset (SWRS061 19.1.2): toggle CS, wait, then SRES.
  digitalWrite(s_pins.cs, LOW); delayMicroseconds(10); digitalWrite(s_pins.cs, HIGH); delayMicroseconds(45);
  ccStrobe(CC_SRES); delay(1);
  cc_version = ccRead(CC_VERSION);
  if (cc_version == 0x00 || cc_version == 0xFF) return false;   // nothing on the bus: MISO stuck low or high
  for (auto& r : CC_INIT) ccWrite(r[0], r[1]);
  ccSetFreq(mhz);
  ccSetModem(cc_bwKHz, cc_drateK);
  ccRx();
  delay(2);
  return ccRead(CC_MARCSTATE) == CC_MARC_RX;
}

// Signal level: RSSI is sampled on every loop pass while a frame is arriving and
// the peak kept (on-off keying drops the carrier for every 0, so an average would
// read low). A frame only gets a level if at least two samples landed in it.
static int g_pk = -200; static uint16_t g_pkN = 0; static uint32_t g_pkMs = 0;
static void ccTrack() {
  if (dp_state == 2) { int r = ccRssi(); if (r > g_pk) g_pk = r; g_pkN++; g_pkMs = millis(); }
  else if (!dp_ready && millis() - g_pkMs > 300) { g_pk = -200; g_pkN = 0; }
  static uint32_t lastChk = 0;                          // the chip should never leave RX; if it does, put it back
  if (millis() - lastChk > 5000) {
    lastChk = millis();
    if (s_ok && ccRead(CC_MARCSTATE) != CC_MARC_RX) { ccRx(); cc_restarts++; }
  }
}
static const float DP_WINDDIR[16] = { 315.0, 247.5, 292.5, 270.0, 337.5, 225.0, 0.0, 202.5, 67.5, 135.0, 90.0, 112.5, 45.0, 157.5, 22.5, 180.0 };

// A station id not heard before must be heard again, 5 s to 5 min later, before
// anything is filed under it. Two bit errors can cancel in the checksum: on the
// 2026-09-26 bench one did, and 323-A's readings filed as a station 1347-A.
static bool stationConfirmed(uint16_t key) {
  static uint16_t okKeys[16]; static uint8_t okN = 0;
  static struct { uint16_t key; uint32_t ms; } pend[8];
  for (int i = 0; i < okN; i++) if (okKeys[i] == key) return true;
  int slot = -1, freeI = -1;
  for (int i = 0; i < 8; i++) {
    if (pend[i].ms && pend[i].key == key) slot = i;
    else if ((!pend[i].ms || millis() - pend[i].ms > 300000UL) && freeI < 0) freeI = i;
  }
  if (slot >= 0 && millis() - pend[slot].ms >= 5000UL && millis() - pend[slot].ms <= 300000UL) {
    static const char* CHN[] = { "C", "?", "B", "A" };
    pend[slot].ms = 0; okKeys[okN < 16 ? okN++ : 15] = key;
    if (s_log) s_log->printf("[%s] station %u-%s confirmed (heard twice)\n", s_tag, (unsigned)(key >> 2), CHN[key & 3]);
    return true;
  }
  if (slot < 0 && freeI >= 0) { pend[freeI].key = key; pend[freeI].ms = millis(); }
  return false;
}

// Plausibility, per station. The checksum is a byte sum and the parity covers five
// bytes, so now and then a frame with two errors passes both (the host sim: about
// one in a few hundred valid frames on a noisy link). A reading that jumps more
// than weather does between frames (temperature 5 F, humidity 15 %, the rain
// counter backwards or up more than an inch) is held, and filed only if the next
// frame says the same. Real change passes at once; a one-off bad frame never files.
static Sane g_sane[16];
static Sane& saneFor(uint16_t key) {
  int oldest = 0;
  for (int i = 0; i < 16; i++) {
    if (g_sane[i].used && g_sane[i].key == key) { g_sane[i].lastMs = millis(); return g_sane[i]; }
    if (!g_sane[i].used || g_sane[i].lastMs < g_sane[oldest].lastMs) oldest = i;
  }
  Sane& z = g_sane[oldest]; memset(&z, 0, sizeof(z)); z.used = true; z.key = key; z.lastMs = millis();
  return z;
}
static bool saneTH(Sane& z, float t, float h) {
  bool ok = !z.haveT || millis() - z.tMs > 900000UL || (fabsf(t - z.t) <= 5.0f && fabsf(h - z.h) <= 15.0f)
         || (z.pendT && millis() - z.pendTMs < 300000UL && fabsf(t - z.pt) <= 1.0f && fabsf(h - z.ph) <= 3.0f);
  if (ok) { z.t = t; z.h = h; z.haveT = true; z.tMs = millis(); z.pendT = false; return true; }
  z.pendT = true; z.pt = t; z.ph = h; z.pendTMs = millis(); ph_held++; return false;
}
static bool saneRain(Sane& z, float r) {
  bool ok = !z.haveR || millis() - z.rMs > 3600000UL || (r >= z.r && r - z.r <= 1.0f) || r < 0.005f   // up, a little; or back to exactly zero (a battery pull)
         || (z.pendR && millis() - z.pendRMs < 300000UL && fabsf(r - z.pr) <= 0.02f);
  if (ok) { z.r = r; z.haveR = true; z.rMs = millis(); z.pendR = false; return true; }
  z.pendR = true; z.pr = r; z.pendRMs = millis(); ph_held++; return false;
}


static const float WINDDIR[16] = { 315.0, 247.5, 292.5, 270.0, 337.5, 225.0, 0.0, 202.5, 67.5, 135.0, 90.0, 112.5, 45.0, 157.5, 22.5, 180.0 };

inline void end() {
  if (s_ok && s_pins.data >= 0) detachInterrupt(digitalPinToInterrupt(s_pins.data));
  s_ok = false;
}
// Start the receiver. False when there is nothing there to start (no CC1101 on the
// bus: version reads 0x00 or 0xFF); the pins are then handed back as inputs.
inline bool begin(Front f, const Pins& p, SPIClass* spi, float mhz, const char* tag) {
  end();
  s_front = f; s_pins = p; s_spi = spi; if (tag) s_tag = tag;
  if (p.data < 0 || f == FRONT_NONE) return false;
  pinMode(p.data, INPUT);
  if (f == FRONT_CC1101) {
    if (!spi || p.cs < 0 || p.sck < 0 || p.miso < 0 || p.mosi < 0) return false;
    s_ok = ccBegin(mhz);
    if (!s_ok) { spi->end(); pinMode(p.cs, INPUT); pinMode(p.sck, INPUT); pinMode(p.mosi, INPUT); return false; }
  } else {
    s_ok = true;
  }
  attachInterrupt(digitalPinToInterrupt(p.data), dpIsr, CHANGE);
  return true;
}
inline bool running() { return s_ok; }
inline Front front() { return s_front; }
inline const Pins& pins() { return s_pins; }
inline bool retune(float mhz) {
  if (s_front != FRONT_CC1101 || !s_ok) return false;
  ccStrobe(CC_SIDLE); ccSetFreq(mhz); ccRx(); return true;
}
// Every pass of the sketch's loop: closes a pulse whose fall has stood and, on a
// CC1101, samples the signal level while a frame arrives.
inline void loop() {
  if (!s_ok) return;
  dpLoopSide();
  if (s_front == FRONT_CC1101) ccTrack();
}

// The next checked reading, if one is ready. Checksum, parity, then the
// new-station and plausibility guards; a frame that fails any is counted, not filed.
inline bool take(Reading& r) {
  if (!s_ok || !dp_ready) return false;
  uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = dp_frame[i];
  dp_ready = false;
  int sum = 0; for (int i = 0; i < 7; i++) sum += b[i];
  if ((sum & 0xFF) != b[7] || sum == 0) { dp_badcrc++; return false; }
  // rtl_433's acurite_txr_check: bytes 2..6 carry even parity in bit 7 (the id
  // bytes and the checksum are full 8-bit), and channel bits 01 never occur.
  for (int i = 2; i <= 6; i++) if (__builtin_parity(b[i])) { dp_badparity++; return false; }
  if ((b[0] >> 6) == 1) { dp_badparity++; return false; }
  uint16_t key = (uint16_t)(((b[0] & 0x0F) << 8 | b[1]) << 2 | (b[0] >> 6));
  if (!stationConfirmed(key)) { ph_unconfirmed++; return false; }
  memset(&r, 0, sizeof(r));
  static const char CH[] = { 'C', '?', 'B', 'A' };
  r.id = ((b[0] & 0x0F) << 8) | b[1]; r.ch = CH[b[0] >> 6];
  r.batteryOk = (b[2] & 0x40) != 0;
  int raw = ((b[3] & 0x1F) << 3) | ((b[4] & 0x70) >> 4);
  r.windKmh = raw > 0 ? raw * 0.8278f + 1.0f : 0.0f;
  r.type = b[2] & 0x3F;
  if (r.type == 0x31) {
    float rain = (((b[5] & 0x7F) << 7) | (b[6] & 0x7F)) * 0.01f;
    if (!saneRain(saneFor(key), rain)) return false;
    r.hasDir = true; r.dirDeg = WINDDIR[b[4] & 0x0F];
    r.hasRain = true; r.rainIn = rain;
    ph_type31++;
  } else if (r.type == 0x38) {
    int t = ((b[4] & 0x0F) << 7) | (b[5] & 0x7F);
    float tf = (t - 400) / 10.0f, hum = b[6] & 0x7F;
    if (!saneTH(saneFor(key), tf, hum)) return false;
    r.hasTH = true; r.tempF = tf; r.hum = hum;
    ph_type38++;
  } else { s_skipped++; return false; }
  if (s_front == FRONT_CC1101 && g_pkN >= 2) { r.hasRssi = true; r.rssi = g_pk; }
  g_pk = -200; g_pkN = 0;
  dp_frames++;
  snprintf(r.sid, sizeof(r.sid), "acurite-5n1:%u-%c", (unsigned)r.id, r.ch);
  return true;
}

struct Stats { uint32_t frames, badcrc, badparity, lostMid, glitches, ghosts, merged, unconfirmed, held, type31, type38, skipped; };
inline Stats stats() {
  return Stats{ dp_frames, dp_badcrc, dp_badparity, ph_missTotal, dp_glitches, ph_ghosts, ph_merged, ph_unconfirmed, ph_held, ph_type31, ph_type38, s_skipped };
}
inline uint8_t  ccVersion()  { return cc_version; }
inline bool     ccReceiving(){ return s_front == FRONT_CC1101 && s_ok && ccRead(CC_MARCSTATE) == CC_MARC_RX; }
inline uint8_t  ccState()    { return (s_front == FRONT_CC1101 && s_ok) ? ccRead(CC_MARCSTATE) : 0; }
inline float    ccBwKHz()    { return cc_bwKHz; }
inline float    ccDrateK()   { return cc_drateK; }
inline int      ccRssiNow()  { return (s_front == FRONT_CC1101 && s_ok) ? ccRssi() : 0; }
inline uint32_t ccRestarts() { return cc_restarts; }

// ---- console tools (the listener's `pulses`, `regs`, `rssi`) -----------------
inline void printPulses(const String& a, Stream& out) {
  String t = a; t.trim();
  if (t == "clear") { for (int i = 0; i < PH_BINS; i++) ph_hist[i] = 0; ph_missN = 0; ph_missTotal = 0; ph_type31 = ph_type38 = 0; ph_ghosts = 0; ph_merged = ph_unconfirmed = ph_held = 0; out.println("pulse counters cleared"); return; }
  out.printf("frames decoded: wind/rain (0x31) %lu, temp/humidity (0x38) %lu; frames lost mid-way %lu; bad checksum %lu, bad parity %lu; vanished pulses filled %lu\n",
                (unsigned long)ph_type31, (unsigned long)ph_type38, (unsigned long)ph_missTotal, (unsigned long)dp_badcrc, (unsigned long)dp_badparity, (unsigned long)ph_ghosts);
  out.printf("merged pulses split %lu, frames from unconfirmed ids held back %lu, jumps held for a second frame %lu\n", (unsigned long)ph_merged, (unsigned long)ph_unconfirmed, (unsigned long)ph_held);
  out.printf("0/1 split %u us, longest bit %u us, sync %u-%u us, gap bridge %lu us, spike %lu us\n", W_LONG_LO, W_LONG_HI, W_SYNC_LO, W_SYNC_HI, (unsigned long)g_glitchUs, (unsigned long)g_spikeUs);
  out.println("in-frame pulse widths (us: count):");
  uint16_t mx = 1; for (int i = 0; i < PH_BINS; i++) if (ph_hist[i] > mx) mx = ph_hist[i];
  for (int i = 0; i < PH_BINS; i++) {
    if (!ph_hist[i]) continue;
    out.printf("  %4d-%4d %6u ", i * 20, i * 20 + 19, ph_hist[i]);
    int bar = (int)((uint32_t)ph_hist[i] * 40 / mx); for (int k = 0; k < bar; k++) out.print('#'); out.println();
  }
  int n = ph_missN < 16 ? ph_missN : 16;
  if (n) out.println("last frames lost (bit reached, width of the pulse that ended it, time since the last bit's start, dropouts bridged inside it):");
  for (int i = 0; i < n; i++) { volatile Miss& m = ph_miss[(ph_missN - n + i) % 16]; out.printf("  bit %2u  width %5u us  after %5u us  dropouts %u\n", m.bit, m.w, m.gap, m.drops); }
  out.println("`pulses clear` resets these.");
}

inline void printRegs(const String&, Stream& out) {
  if (s_front != FRONT_CC1101 || !s_ok) { out.println("no CC1101 running"); return; }
  out.printf("CC1101 version 0x%02x partnum 0x%02x marcstate 0x%02x rssi %d dBm\n", ccRead(CC_VERSION), ccRead(CC_PARTNUM), ccRead(CC_MARCSTATE), ccRssi());
  for (uint8_t a = 0; a <= 0x2E; a++) { out.printf("%02x:%02x%s", a, ccRead(a), (a % 8 == 7) ? "\n" : "  "); }
  out.println();
}

inline void sampleRssi(const String&, Stream& out) {
  if (s_front != FRONT_CC1101 || !s_ok) { out.println("no CC1101 running"); return; }
  int lo = 0, hi = -200; long sum = 0;
  for (int i = 0; i < 50; i++) { int r = ccRssi(); sum += r; if (r > hi) hi = r; if (i == 0 || r < lo) lo = r; delay(2); }
  out.printf("rssi over 100 ms: avg %ld  min %d  max %d dBm (a quiet 433 band reads about -100 to -110)\n", sum / 50, lo, hi);
}

#ifdef OAT_RX_BENCH
// ---- bench tools: live, not saved --------------------------------------------
inline void benchGlitch(const String& a, Stream& out) {
  String t = a; t.trim();
  if (!t.length()) { out.printf("glitch filter %lu us (0 = off), %lu glitches dropped\n", (unsigned long)g_glitchUs, (unsigned long)dp_glitches); return; }
  long v = t.toInt(); if (v < 0 || v > 190) { out.println("usage: glitch <us>  0 to 190 (the shortest real AcuRite gap is ~200 us)"); return; }
  g_glitchUs = (uint32_t)v; out.printf("glitch filter <- %ld us (lows shorter than this inside a pulse are bridged)\n", v);
}

inline void benchSpike(const String& a, Stream& out) {
  String t = a; t.trim();
  if (!t.length()) { out.printf("spike filter %lu us: pulses shorter than this never count toward a sync train (inside a frame the bit clock decides)\n", (unsigned long)g_spikeUs); return; }
  long v = t.toInt(); if (v < 0 || v > 190) { out.println("usage: spike <us>  0 to 190 (the shortest real AcuRite pulse is ~200 us)"); return; }
  g_spikeUs = (uint32_t)v; out.printf("spike filter <- %ld us\n", v);
}

inline void benchWin(const String& a, Stream& out) {
  String t = a; t.trim(); int sp1 = t.indexOf(' '), sp2 = t.lastIndexOf(' ');
  if (sp1 > 0 && sp2 > sp1) {
    String k = t.substring(0, sp1); int lo = t.substring(sp1 + 1, sp2).toInt(), hi = t.substring(sp2 + 1).toInt();
    if (lo <= 0 || hi <= lo || hi > 2000) { out.println("the first number must be > 0 and below the second"); return; }
    if (k == "long") { W_LONG_LO = lo; W_LONG_HI = hi; } else if (k == "sync") { W_SYNC_LO = lo; W_SYNC_HI = hi; }
    else { out.println("usage: win long <0/1 split> <longest bit>  |  win sync <low> <high>   (us)"); return; }
  } else if (t.length()) { out.println("usage: win long 315 540  |  win sync 500 750"); return; }
  out.printf("0/1 split %u us, longest bit %u us, sync %u-%u us\n", W_LONG_LO, W_LONG_HI, W_SYNC_LO, W_SYNC_HI);
}

inline void benchCapture(const String& a, Stream& out) {
  String t = a; t.trim();
  if (t != "show") {
    portENTER_CRITICAL(&dg_mux); cap_lostHave = 0; cap_lostWant = 2; cap_goodHave = false; cap_goodWant = true; portEXIT_CRITICAL(&dg_mux);
    out.println("capture armed: the next 2 frames lost past bit 16 and the next good frame. `capture show` prints them (copy the whole block)."); return;
  }
  out.printf("CAPTURE fw=%s lost=%u good=%u\n", OATRF_VERSION, cap_lostHave > 2 ? 2 : cap_lostHave, cap_goodHave ? 1 : 0);
  for (int c = 0; c < 3; c++) {
    if (c < 2 && c >= (cap_lostHave > 2 ? 2 : cap_lostHave)) continue;
    if (c == 2 && !cap_goodHave) continue;
    Cap& k = caps[c];
    out.printf("FRAME %s bits=%u buf=", c == 2 ? "good" : "lost", k.bits);
    for (int i = 0; i < 8; i++) out.printf("%02x", k.buf[i]);
    out.printf(" n=%u  (pairs: start-since-previous-start width, us)\n", k.n);
    for (int i = 0; i < k.n; i++) { out.printf("%lu %u%s", (unsigned long)(i ? k.p[i].s - k.p[i - 1].s : 0), k.p[i].w, (i % 10 == 9) ? "\n" : " | "); }
    out.println(); out.println("END");
  }
}

inline void benchReg(const String& a, Stream& out) {
  String t = a; t.trim(); int sp = t.indexOf(' ');
  if (sp < 0) { out.println("usage: reg <addr-hex> <value-hex>   e.g. reg 1d 92 (12 dB OOK boundary), reg 02 4d (invert GDO0)"); return; }
  uint8_t addr = (uint8_t) strtol(t.substring(0, sp).c_str(), nullptr, 16), val = (uint8_t) strtol(t.substring(sp + 1).c_str(), nullptr, 16);
  if (addr > 0x2E) { out.println("config registers are 00-2e"); return; }
  ccStrobe(CC_SIDLE); ccWrite(addr, val); ccRx();
  out.printf("reg 0x%02x <- 0x%02x (readback 0x%02x), back in RX\n", addr, val, ccRead(addr));
}

inline void benchBw(const String& a, Stream& out) {
  float k = a.toFloat();
  if (!(k >= 58.0f && k <= 812.5f)) { out.println("usage: bw <kHz>  58, 68, 81, 102, 116, 135, 162, 203, 232, 270, 325, 406, 464, 541, 650, 812"); return; }
  ccStrobe(CC_SIDLE); ccSetModem(k, cc_drateK); ccRx();
  out.printf("rx bandwidth <- %.1f kHz\n", cc_bwKHz);
}

inline void benchDrate(const String& a, Stream& out) {
  float k = a.toFloat();
  if (!(k >= 0.6f && k <= 500.0f)) { out.println("usage: drate <kBaud>  e.g. 5 (the value the 2026-09-20 capture ran at)"); return; }
  ccStrobe(CC_SIDLE); ccSetModem(cc_bwKHz, k); ccRx();
  out.printf("data rate <- %.2f kBaud\n", cc_drateK);
}
#endif

}  // namespace oatrf
