/* =============================================================================
   OAT DS18B20 Node  —  v2.0.6
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   Reads one or many DS18B20 / DS18S20 temperature probes on a single 1-Wire data
   line and pushes them as oat-ods to an endpoint the operator owns.

   v2.0.0 is the same firmware as 1.2.0 with the plumbing removed: config, NVS, the
   field registry, WiFi, the setup AP, the pages, the Console, the push engine and
   the heartbeat now come from oat_node_core, so this file is ONLY the sensor. The
   1-Wire code below is byte-for-byte the code that was bench-verified on 26 July,
   moved rather than rewritten — including every fix that bench found: interrupts
   held only across bit edges, open-drain level writes instead of pinMode (which
   sampled past tRDV and made every bit read as 1), an S20 setting the conversion
   pace for the whole bus, counters carried across a re-scan, and diagnostics that
   report only what they measured.

   WHAT THIS FILE OWNS: the bus. Discovery (ROM search), timing, the CRC, the
   decode for two families, failure accounting, and the diagnostics.
   WHAT IT DOES NOT OWN: anything else. If you find WiFi or push code here, it has
   been put in the wrong file.

   THE ONE-WIRE IDEA
     Every DS18B20/DS18S20 carries a unique 64-bit address lasered in at the
     factory (family code + 48-bit serial + CRC), so many probes share one data
     line and each still reports as itself. The node runs the ROM SEARCH (F0h,
     Maxim AN187) at boot and reports whatever is out there. The protocol sets no
     limit on how many; cable capacitance does, at around eight to ten on a short
     run with the standard 4.7k pull-up, which is why MAX_DS is ours and not the
     chip's.

   THE RESISTOR IS NOT OPTIONAL
     1-Wire is open drain: devices only pull the line DOWN, and one 4.7k resistor
     to 3V3 (for the whole bus, not per probe) is the only thing pulling it back
     up. Without it there is no bus, just a floating pin reading noise — which
     looks exactly like a broken sensor.

   CHANGELOG
     2.0.6  Rebuilt against the current core (the wired-node rssi fix and
            slotName landed after 2.0.5's binary was published, so the file on
            the site was two core commits behind its own source). No behaviour
            change for this sketch. The bump exists so one version number never
            names two different binaries.
     2.0.1  Core fix: saving a setting no longer re-joins WiFi unless the WiFi
            settings changed. Editing anything from the LAN used to tear down the
            station connection under the page you were reading.
     2.0.0  Ported to oat_node_core. No behaviour change intended: same bus code,
            same payloads, same setup page and Console, ~1,700 fewer lines in this
            file. Bench-verify against 1.2.0 before trusting it.
     1.2.0  The node stopped naming anything: stream.id is the probe's factory
            address (oat-ods §3/§4). Earlier history is in git.

   LICENSE: openly licensed, like everything in this library. Copy it, change it,
   sell what you build with it.
   ============================================================================= */

#include <oat_node_core.h>
#include <oat_measurands.h>
#include <driver/gpio.h>          // gpio_set_level/get_level: the fast path a bit slot needs

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
#define TIER        "oat-ds18b20-node"
#define FW_SEMVER   "2.0.6"
#define FW_VERSION  "OAT-DS18B20-Node/2.0.6"
#define NVS_NS      "oatds"       // unchanged, so a 1.2.0 node keeps its settings

// The default data pin, per chip. Different numbers because the unusable pins are
// different: GPIO 4 is free on the classic, the S3 and the C3, but it is a
// STRAPPING pin on the C6, and a 1-Wire bus idles HIGH through its pull-up, which
// is exactly the nudge that would change how a C6 boots. Sources: Espressif's
// per-chip GPIO documentation (strapping / SPI-flash / USB-JTAG pin lists).
#if   defined(CONFIG_IDF_TARGET_ESP32C6)
  #define DEFAULT_OW_PIN  6
#else
  #define DEFAULT_OW_PIN  4
#endif

#define MAX_DS            10      // our cap, not the protocol's — see the note above
#define DEFAULT_RES_BITS  12      // 9..12; 12-bit is 0.0625 C and a 750 ms conversion
#define BUS_FAIL_RESET    5       // consecutive failures before a bus re-scan
#define BUS_RECOVER_MIN_MS 30000UL
#define BLIND_RECOVER_MS  60000UL // with NOTHING on the wire, re-scan this often
#define PRESENT_FAIL_DROP 6       // failed reads before a probe stops counting as present

// ---------------------------------------------------------------------------
// Settings this driver adds to the shared registry (rendered, persisted and routed
// by the core; declared here because only the bus knows what they mean)
// ---------------------------------------------------------------------------
static int      g_pin = DEFAULT_OW_PIN;
static uint32_t g_res = DEFAULT_RES_BITS;
static float    g_off = 0.0f;

// ---------------------------------------------------------------------------
// Bus state
// ---------------------------------------------------------------------------
// Two families do this job, and they differ only in how the scratchpad decodes:
//   0x28  DS18B20  12-bit, 1/16 C steps, configurable resolution
//   0x10  DS18S20  9-bit,  1/2  C steps, extended precision via the count registers
enum DsReadResult { DS_OK, DS_NO_ANSWER, DS_CRC };

struct DsSensor {
  bool     present = false;
  bool     everSeen = false;
  uint8_t  family = 0;
  uint8_t  rom[8] = {0};
  char     serial[32] = {0};      // "ds18b20:28ff641f8c1a3b02" — 8 + 16 + NUL, not a byte less
  int      slot = -1;             // this probe's slot in the core's table
  uint32_t readsOk = 0, readsFail = 0, crcFail = 0, reset85 = 0;
  uint8_t  consecFail = 0;
  float    lastT = NAN;
  unsigned long lastOkMs = 0;
};
static DsSensor sensors[MAX_DS];

static bool     convPending   = false;
static unsigned long convStartedMs = 0;
static int      g_dsCount     = 0;
static bool     g_parasite    = false;
static uint32_t g_romBad      = 0;
static uint32_t g_foreign     = 0;
static uint32_t g_reset85     = 0;
static uint8_t  g_consecBusFail = 0;
static uint32_t g_busResets   = 0;
static unsigned long lastBusRecoverMs = 0;
static unsigned long lastBlindRecoverMs = 0;
static String   lastReadMsg   = "no read yet";

// Which pins can carry the bus, per chip, and WHY not otherwise — the reason is
// half the value, because every one of these fails looking like a broken sensor.
// Pin lists are Espressif's (strapping / SPI-flash / USB-JTAG), not folklore.
static bool pinOkForOneWire(int p, String& why) {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  if (p < 0 || p > 21)              { why = "pin must be 0-21 on an ESP32-C3"; return false; }
  if (p >= 12 && p <= 17)           { why = "GPIO 12-17 are the SPI flash this firmware runs from"; return false; }
  if (p == 18 || p == 19)           { why = "GPIO 18 and 19 are the USB port you flash and talk to it through"; return false; }
  if (p == 20 || p == 21)           { why = "GPIO 20 and 21 are UART0"; return false; }
  if (p == 2 || p == 8 || p == 9)   { why = "GPIO " + String(p) + " is a strapping pin; a bus that idles high would change how the board boots"; return false; }
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  if (p < 0 || p > 30)              { why = "pin must be 0-30 on an ESP32-C6"; return false; }
  if (p >= 24 && p <= 30)           { why = "GPIO 24-30 are the SPI flash this firmware runs from"; return false; }
  if (p == 12 || p == 13)           { why = "GPIO 12 and 13 are the USB port you flash and talk to it through"; return false; }
  if (p == 16 || p == 17)           { why = "GPIO 16 and 17 are UART0"; return false; }
  if (p == 4 || p == 5 || p == 8 || p == 9 || p == 15)
                                    { why = "GPIO " + String(p) + " is a strapping pin; a bus that idles high would change how the board boots"; return false; }
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  if (p < 0 || p > 48)              { why = "pin must be 0-48 on an ESP32-S3"; return false; }
  if (p >= 26 && p <= 37)           { why = "GPIO 26-37 belong to the flash and PSRAM"; return false; }
  if (p == 19 || p == 20)           { why = "GPIO 19 and 20 are the USB port you flash and talk to it through"; return false; }
  if (p == 43 || p == 44)           { why = "GPIO 43 and 44 are UART0"; return false; }
  if (p == 0 || p == 3 || p == 45 || p == 46)
                                    { why = "GPIO " + String(p) + " is a strapping pin; a bus that idles high would change how the board boots"; return false; }
#else
  if (p < 0 || p > 39)              { why = "pin must be 0-39 on a classic ESP32"; return false; }
  if (p >= 6 && p <= 11)            { why = "GPIO 6-11 are the flash chip this firmware runs from"; return false; }
  if (p >= 34)                      { why = "GPIO 34-39 are input-only and cannot drive a bus"; return false; }
  if (p == 1 || p == 3)             { why = "GPIO 1 and 3 are the USB console"; return false; }
  if (p == 0 || p == 2 || p == 5 || p == 12 || p == 15)
                                    { why = "GPIO " + String(p) + " is a strapping pin; a bus that idles high would change how the board boots"; return false; }
  if (p == 20 || p == 24 || (p >= 28 && p <= 31))
                                    { why = "GPIO " + String(p) + " is not brought out on a classic ESP32"; return false; }
#endif
  why = "ok";
  return true;
}

// ----------------------------------------------------------------------------
// DS18B20 driver — the 1-Wire bus, bit-banged, no vendor library
// ----------------------------------------------------------------------------
// One data wire carries everything: the master's commands, the sensors' answers,
// and (in parasite mode, which this node does not use) their power. Every slot is
// started by the master pulling the line low; how long it stays low is the bit.
// The line idles HIGH through the 4.7 k pull-up, which is why that resistor is
// not optional: with nothing holding the line up there is no bus, only a floating
// pin, and the node reads garbage or nothing at all.
//
// Timings below are the datasheet's (DS18B20, AC ELECTRICAL CHARACTERISTICS):
// reset low >= 480 us, slot 60-120 us, write-0 low 60-120 us, write-1 low 1-15 us,
// read data valid within 15 us of the falling edge, recovery >= 1 us. Each bit is
// taken inside a critical section, because a FreeRTOS switch or a WiFi interrupt
// in the middle of a 60 us slot is a corrupted bit — and a corrupted bit that
// happens to survive the CRC is exactly the wrong number to publish.
static portMUX_TYPE owMux = portMUX_INITIALIZER_UNLOCKED;

// The pin is put in INPUT_OUTPUT_OD (open-drain, input buffer live) ONCE, and the
// bit slots then only change its LEVEL. That matters more than it looks:
// pinMode() on an ESP32 reconfigures the IO MUX and costs several microseconds,
// and a 1-Wire read has to sample within 15 us of the falling edge. Toggling
// direction per bit put the sample at roughly 20 us, past the point where the
// slave has let go, so every bit read back as 1 — which the ROM search reads as
// "nobody is on this bus". A reset would still work (its timings are hundreds of
// microseconds) so the node saw a presence pulse and then found nothing, which is
// the confusing shape this cost us an evening in.
// Open drain also means "release" is just writing a 1: the pull-up does the rest,
// and nothing ever drives the line high, which is what an open-drain bus requires.
static void owBusInit() {
  gpio_reset_pin((gpio_num_t)g_pin);
  gpio_set_direction((gpio_num_t)g_pin, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_level((gpio_num_t)g_pin, 1);          // released
}
static inline void owDriveLow() { gpio_set_level((gpio_num_t)g_pin, 0); }
static inline void owRelease()  { gpio_set_level((gpio_num_t)g_pin, 1); }
static inline int  owLevel()    { return gpio_get_level((gpio_num_t)g_pin); }

// Is there even a bus? Before blaming a probe, ask the line itself.
//
// Released with no internal pull-up, a healthy 1-Wire line idles HIGH because the
// external 4.7k resistor holds it there. Read LOW and there are exactly two
// possibilities, and they need opposite fixes: nothing is pulling it up (no
// resistor fitted), or something is holding it down (data shorted to ground, the
// wrong pin, a probe with its wires crossed). The chip's own weak internal pull-up
// tells them apart, because roughly 45k can lift a floating line but cannot lift a
// line that is genuinely being held down. This is what a bench tech does with a
// multimeter, done by the node so nobody needs one.
static String owLineDiag() {
  pinMode(g_pin, INPUT);
  delayMicroseconds(300);
  bool idleHigh = digitalRead(g_pin);
  pinMode(g_pin, INPUT_PULLUP);
  delayMicroseconds(300);
  bool weakHigh = digitalRead(g_pin);
  // Third question, and the one that separates "our fault" from "their fault":
  // can WE pull this line down? If the node drives the pin low and the line does
  // not go low, the bus never sees a reset pulse at all and no device could ever
  // answer — that is a firmware or a pin-conflict problem, not a probe problem.
  // Worth proving rather than assuming, because everything downstream of the reset
  // depends on it.
  bool canDrive = false;
  if (idleHigh) {
    owBusInit();                                // open drain: the read below is the PAD, not a latch
    owDriveLow();
    delayMicroseconds(300);
    canDrive = (owLevel() == 0);
    owRelease();
    delayMicroseconds(300);
  }
  owBusInit();                           // restore open-drain; the diagnostics above changed the mode

  bool extPullup = externalPullupOn(g_pin);
  if (idleHigh && !extPullup)
    return "line reads high, but a pull-down inside the chip beats it: there is no 4.7k resistor on GPIO " + String(g_pin) + ". The high reading is the pin floating, not a bus";
  if (idleHigh && !canDrive)
    return "line idles high but this node CANNOT pull it low: the pin will not drive. Something else on the board owns GPIO " + String(g_pin) + ", or it is not the pin the wire is on";
  // ASK, do not assume. The previous version of this line asserted "nothing
  // answered the reset" as a fixed sentence — and printed it directly above a scan
  // that had just found a probe. A diagnostic must report what it measured.
  bool answered = owReset();
  if (idleHigh && answered)
    return "line is healthy on GPIO " + String(g_pin) + " and at least one device answered the reset";
  if (idleHigh)
    return "line is healthy on GPIO " + String(g_pin) + " (pull-up holds, the node can drive it low) but NOTHING answered the reset: check GND at the probe end, check that the data wire really is the data wire, and try one probe on its own";
  if (weakHigh)
    return "line reads LOW on its own but the chip's weak internal pull-up lifts it: there is no 4.7k resistor between the data line and 3V3, so nothing can answer";
  return "line is held LOW even against the chip's internal pull-up: data shorted to ground, the wrong pin, or a probe wired with data and ground swapped";
}

// Is there an external pull-up on THIS pin? The chip's internal pull-down is about
// 45k; a 4.7k pull-up to 3V3 beats it roughly ten to one, so the pin still reads
// high. Nothing attached and the internal pull-down wins, so it reads low.
//
// This measures the outside world. Reading back a pin the node is driving does not:
// on an ESP32 a pin set to OUTPUT can return its own output latch, so a "can I
// drive it low" check passes on a pin wired to nothing at all. That mistake cost an
// evening here; the test below is the one that means something.
static bool externalPullupOn(int pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(600);                 // let a long cable's capacitance settle
  bool high = digitalRead(pin);
  pinMode(pin, INPUT);
  return high;
}

// Which pin are the probes actually ON? Sweep every pin this chip could legally use
// for a 1-Wire bus, measure each for an external pull-up, and try a reset on each.
// A label silk-screened on a board is a claim; this is evidence.
static String owScanPins() {
  int saved = g_pin;
  String out;
  for (int pin = 0; pin <= 48; pin++) {
    String why;
    if (!pinOkForOneWire(pin, why)) continue;
    bool pu = externalPullupOn(pin);
    g_pin = pin;                      // owReset() works on g_pin
    bool presence = owReset();
    if (!pu && !presence) continue;        // nothing interesting on this one
    if (out.length()) out += "\n";
    out += "  GPIO " + String(pin) + ": " + (pu ? "external pull-up present" : "no pull-up");
    out += presence ? ", AND A DEVICE ANSWERED THE RESET" : ", nothing answered";
  }
  g_pin = saved;
  owBusInit();
  return out.length() ? out : String("  no pin on this chip shows an external pull-up or a device");
}

// The sharpest instrument in the box: issue a reset, then WATCH the line for the
// whole window instead of sampling it once, and report what was actually seen.
//
// A single sample answers "was it low at 70 us". A trace answers "did anything
// happen at all, and when" — which is the difference between "nothing is out
// there" and "something answered and we looked at the wrong moment". One of those
// is a wiring fault and the other is a firmware fault, and guessing between them
// wastes an evening.
static String owPresenceTrace() {
  owBusInit();
  delayMicroseconds(300);
  if (digitalRead(g_pin) == 0) return "line was already low before the reset, nothing to trace";

  uint32_t firstLow = 0, lastLow = 0, samples = 0;
  portENTER_CRITICAL(&owMux);
  owDriveLow();
  portEXIT_CRITICAL(&owMux);
  delayMicroseconds(500);                       // tRSTL >= 480 us

  portENTER_CRITICAL(&owMux);
  owRelease();                                  // release; the pull-up takes it high
  uint32_t t0 = micros(), now;
  do {
    now = micros();
    if (digitalRead(g_pin) == 0) { if (!firstLow) firstLow = now - t0; lastLow = now - t0; }
    samples++;
  } while (now - t0 < 600);
  portEXIT_CRITICAL(&owMux);

  if (!firstLow)
    return "traced 600 us after the reset (" + String(samples) + " samples): the line never went low. Nothing on the wire is answering";
  return "traced: line went low " + String(firstLow) + " us after release and stayed low until " + String(lastLow) +
         " us. The datasheet expects a presence pulse starting 15-60 us after release and lasting 60-240 us";
}

// Reset pulse + presence detect. True means at least one device answered by
// pulling the line low after we let go of it.
static bool owReset() {
  int present = 0;
  owRelease();
  // Wait for the bus to be idle-high first. If it is stuck low there is a short,
  // a wrong pin, or a sensor holding the line, and no reset can be issued.
  unsigned long t0 = millis();
  while (owLevel() == 0) { if (millis() - t0 > 5) return false; delayMicroseconds(20); }

  // Only the edges need protecting. The low period just has to be AT LEAST 480 us,
  // so an interrupt lengthening it is harmless; blocking interrupts through it is
  // not, because WiFi has to live on this chip too.
  portENTER_CRITICAL(&owMux);
  owDriveLow();
  portEXIT_CRITICAL(&owMux);
  delayMicroseconds(500);            // tRSTL >= 480 us
  portENTER_CRITICAL(&owMux);
  owRelease();
  delayMicroseconds(70);             // presence pulse starts 15-60 us after release
  present = (owLevel() == 0);
  portEXIT_CRITICAL(&owMux);
  delayMicroseconds(410);            // let the presence pulse finish (tRSTH)
  return present;
}

static void owWriteBit(uint8_t v) {
  portENTER_CRITICAL(&owMux);
  if (v) { owDriveLow(); delayMicroseconds(10); owRelease(); delayMicroseconds(55); }
  else   { owDriveLow(); delayMicroseconds(65); owRelease(); delayMicroseconds(5);  }
  portEXIT_CRITICAL(&owMux);
}

static uint8_t owReadBit() {
  uint8_t r;
  portENTER_CRITICAL(&owMux);
  owDriveLow();
  delayMicroseconds(3);
  owRelease();
  delayMicroseconds(9);              // sample ~12 us in, inside tRDV's 15 us
  r = owLevel() ? 1 : 0;
  portEXIT_CRITICAL(&owMux);
  delayMicroseconds(53);             // finish the 60 us slot
  return r;
}

static void owWrite(uint8_t b) { for (int i = 0; i < 8; i++) { owWriteBit(b & 1); b >>= 1; } }
static uint8_t owRead()        { uint8_t b = 0; for (int i = 0; i < 8; i++) if (owReadBit()) b |= (1 << i); return b; }

// Dallas CRC-8, polynomial X^8 + X^5 + X^4 + 1, fed least-significant-bit first.
// Guards both the 64-bit ROM code and the 9-byte scratchpad, which is what lets
// this node tell a wrong reading from a right one.
static uint8_t dsCrc8(const uint8_t* data, int len) {
  uint8_t crc = 0;
  for (int i = 0; i < len; i++) {
    uint8_t b = data[i];
    for (int j = 0; j < 8; j++) {
      uint8_t mix = (crc ^ b) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;          // 0x8C is 0x31 reflected, for LSB-first
      b >>= 1;
    }
  }
  return crc;
}

// ROM SEARCH (command F0h), the algorithm from Maxim application note 187. It
// walks the tree of 64-bit addresses one bit at a time, so one pass over the bus
// enumerates every device on it without knowing in advance what is there. This is
// the whole reason many sensors can share one wire.
static uint8_t owLastDiscrepancy = 0;
static bool    owLastDeviceFlag  = false;
static uint8_t owRomBuf[8];

static bool owSearch(uint8_t* rom, bool restart) {
  uint8_t idBit, cmpBit, dir;
  uint8_t bitNumber = 1, lastZero = 0, byteNumber = 0, byteMask = 1;
  bool result = false;

  if (restart) { owLastDiscrepancy = 0; owLastDeviceFlag = false; memset(owRomBuf, 0, 8); }
  if (owLastDeviceFlag) return false;
  if (!owReset()) { owLastDiscrepancy = 0; owLastDeviceFlag = false; return false; }

  owWrite(0xF0);
  do {
    idBit  = owReadBit();
    cmpBit = owReadBit();
    if (idBit && cmpBit) break;                    // nobody answered: bus went quiet
    if (idBit != cmpBit) {
      dir = idBit;                                 // all remaining devices agree on this bit
    } else {
      if (bitNumber < owLastDiscrepancy) dir = ((owRomBuf[byteNumber] & byteMask) > 0);
      else                               dir = (bitNumber == owLastDiscrepancy);
      if (!dir) lastZero = bitNumber;              // remember where the tree forked
    }
    if (dir) owRomBuf[byteNumber] |=  byteMask;
    else     owRomBuf[byteNumber] &= ~byteMask;
    owWriteBit(dir);                               // tell the others to drop out

    bitNumber++;
    byteMask <<= 1;
    if (!byteMask) { byteNumber++; byteMask = 1; }
  } while (byteNumber < 8);

  if (bitNumber >= 65) {
    owLastDiscrepancy = lastZero;
    if (owLastDiscrepancy == 0) owLastDeviceFlag = true;   // that was the last one
    result = true;
  }
  if (!result || owRomBuf[0] == 0) { owLastDiscrepancy = 0; owLastDeviceFlag = false; return false; }
  memcpy(rom, owRomBuf, 8);
  return true;
}

// Two families do this job. They share the bus, the search, the CRC and the
// command set; they differ in how the scratchpad decodes, which is handled at the
// one place it matters (readTemp) rather than smeared through the file.
//   0x28  DS18B20  12-bit, 1/16 C steps, configurable resolution
//   0x10  DS18S20  9-bit,  1/2  C steps, extended precision via the count registers
static bool familySupported(uint8_t f) { return f == 0x28 || f == 0x10; }

static const char* familyName(uint8_t f) {
  switch (f) {
    case 0x28: return "DS18B20";
    case 0x10: return "DS18S20";
    case 0x22: return "DS1822";
    case 0x3B: return "DS1825";
    case 0x42: return "DS28EA00";
    case 0x26: return "DS2438";
    case 0x01: return "DS2401 id";
    default:   return "unknown family";
  }
}

// "ds18s20:10ff641f8c1a3b02" — the sensor's own factory-lasered address, prefixed
// with what it actually is. Traceable to a chip rather than to a position in a
// list, and honest about which chip.
static String romHex(const uint8_t* rom) {
  char b[28];
  const char* pfx = rom[0] == 0x10 ? "ds18s20" : "ds18b20";
  snprintf(b, sizeof(b), "%s:%02x%02x%02x%02x%02x%02x%02x%02x", pfx,
           rom[0], rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7]);
  return String(b);
}

// Address one specific sensor: reset, MATCH ROM, then its 64-bit code.
static bool dsSelect(const uint8_t* rom) {
  if (!owReset()) return false;
  owWrite(0x55);
  for (int i = 0; i < 8; i++) owWrite(rom[i]);
  return true;
}

static DsReadResult dsReadScratchpad(const uint8_t* rom, uint8_t* sp) {
  if (!dsSelect(rom)) return DS_NO_ANSWER;
  owWrite(0xBE);
  for (int i = 0; i < 9; i++) sp[i] = owRead();
  if (dsCrc8(sp, 8) != sp[8]) return DS_CRC;   // byte 8 IS the CRC of the first eight
  // An all-zero or all-ones scratchpad passes nothing useful and can CRC by luck
  // on a dead bus; treat it as no answer.
  bool allZero = true, allOnes = true;
  for (int i = 0; i < 8; i++) { if (sp[i] != 0x00) allZero = false; if (sp[i] != 0xFF) allOnes = false; }
  if (allZero || allOnes) return DS_NO_ANSWER;
  return DS_OK;
}

// Resolution lives in the config register: 9 bits is a fast, coarse conversion,
// 12 bits is slow and fine. Writing it takes all three user bytes at once.
static bool dsSetResolution(const uint8_t* rom, int bits) {
  if (rom[0] != 0x28) return true;    // a DS18S20 has no config register: 9-bit, always
  if (bits < 9)  bits = 9;
  if (bits > 12) bits = 12;
  uint8_t cfgByte = 0x1F | ((uint8_t)(bits - 9) << 5);
  if (!dsSelect(rom)) return false;
  owWrite(0x4E);                      // WRITE SCRATCHPAD
  owWrite(0x00);                      // TH  (alarm high — unused here)
  owWrite(0x00);                      // TL  (alarm low  — unused here)
  owWrite(cfgByte);
  return true;
}

// How long to wait before collecting. The resolution setting only applies to the
// DS18B20; a DS18S20 has no config register and always takes the full 750 ms. So
// one S20 anywhere on the wire sets the pace for the whole bus — otherwise a user
// who picks 9-bit for their B20s would have us read an S20's scratchpad mid-
// conversion, which returns the previous value or the 85 C reset value. Reading
// early is a way to publish a plausible wrong number, which is the one thing this
// node is built not to do.
static uint32_t dsConvMs() {
  for (int i = 0; i < g_dsCount; i++)
    if (sensors[i].family == 0x10) return 800;
  switch (g_res) {             // datasheet tCONV, plus a little headroom
    case 9:  return 110;
    case 10: return 200;
    case 11: return 400;
    default: return 800;
  }
}

// Ask every sensor on the wire to convert at once (SKIP ROM + CONVERT T). One
// command, one conversion window, however many sensors are hanging off the line.
static bool dsConvertAll() {
  if (!owReset()) return false;
  owWrite(0xCC);                      // SKIP ROM
  owWrite(0x44);                      // CONVERT T
  return true;
}

// READ POWER SUPPLY: a parasite-powered device answers by pulling the line low.
// This node powers its sensors properly from 3V3, so a low answer means someone
// left the VDD wire off — worth saying out loud, because parasite mode also needs
// a strong pull-up this node does not provide, and its readings would be junk.
static bool dsAnyParasite() {
  if (!owReset()) return false;
  owWrite(0xCC);
  owWrite(0xB4);
  return owReadBit() == 0;
}

// Enumerate the bus and claim whatever is on it. No setting to get wrong: wire one
// sensor or ten, and the node finds them, in a stable order (sorted by address, so
// slot 1 stays slot 1 across reboots even when a sensor is added).
void startSensors() {
  owBusInit();
  // Keep the old census so a re-scan carries each probe's history across. Wiping
  // the counters every time the bus is re-scanned would reset "reads failed" to
  // zero in the middle of the fault the re-scan exists to recover from, which is
  // the one moment those numbers are worth having.
  DsSensor prev[MAX_DS];
  int prevCount = g_dsCount;
  for (int i = 0; i < MAX_DS; i++) prev[i] = sensors[i];

  for (int i = 0; i < MAX_DS; i++) sensors[i] = DsSensor();
  g_dsCount = 0;
  g_parasite = false;

  uint8_t rom[8];
  bool first = true;
  while (g_dsCount < MAX_DS && owSearch(rom, first)) {
    first = false;
    if (dsCrc8(rom, 7) != rom[7]) { g_romBad++; continue; }   // a garbled address is not an address
    if (!familySupported(rom[0])) {                          // 28h DS18B20, 10h DS18S20
      g_foreign++;
      Serial.printf("[ds] ignoring %02x%02x%02x%02x%02x%02x%02x%02x (%s): this firmware reads temperature probes only\n",
                    rom[0], rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7], familyName(rom[0]));
      continue;
    }
    DsSensor &s = sensors[g_dsCount];
    s.family = rom[0];
    memcpy(s.rom, rom, 8);
    strncpy(s.serial, romHex(rom).c_str(), sizeof(s.serial) - 1);
    s.present = true; s.everSeen = true;
    // The core keys the stream on whatever hardware id we hand it (oat-ods §3/§4).
    // We pass the probe's factory address and nothing else: no label, no position.
    s.slot = oatcore::slotFor(s.serial, s.serial);
    // Say what this actually is. The two families are different parts with different
    // accuracy, and an endpoint deciding whether to trust a reading deserves to know
    // which one it came from.
    oatcore::slotMeta(s.slot, "Analog Devices", familyName(s.family));          // wired and mains powered: no battery, no rssi
    for (int j = 0; j < prevCount; j++) {
      if (memcmp(prev[j].rom, rom, 8) != 0) continue;
      s.readsOk = prev[j].readsOk; s.readsFail = prev[j].readsFail;
      s.crcFail = prev[j].crcFail; s.reset85  = prev[j].reset85;
      s.lastT   = prev[j].lastT;   s.lastOkMs = prev[j].lastOkMs;
      break;
    }
    dsSetResolution(rom, g_res);
    g_dsCount++;
  }

  if (g_dsCount == 0) {
    Serial.printf("[ds] no usable probe on GPIO %d (%u device(s) ignored, %u address(es) failed CRC)\n",
                  g_pin, g_foreign, g_romBad);
    Serial.println("[ds] " + owLineDiag());
    Serial.println("[ds] " + owPresenceTrace());
    Serial.println("[ds] no probe found. Type 'pins' to sweep every usable pin for the bus, or 'bus' to list what answers on this one.");
    return;
  }
  if (g_foreign || g_romBad)
    Serial.printf("[ds] also on the wire: %u device(s) this firmware does not read, %u address(es) that failed their own CRC\n",
                  g_foreign, g_romBad);
  g_parasite = dsAnyParasite();
  Serial.printf("[ds] found %d probe%s on GPIO %d\n", g_dsCount, g_dsCount == 1 ? "" : "s", g_pin);
  for (int i = 0; i < g_dsCount; i++)
    Serial.printf("[ds]   %d. %s (%s, %s) -> slot %d\n", i + 1, sensors[i].serial,
                  familyName(sensors[i].family),
                  sensors[i].family == 0x28 ? (String(g_res) + "-bit").c_str() : "9-bit",
                  sensors[i].slot);
  if (g_parasite)
    Serial.println("[ds] a sensor is running on parasite power (no VDD wire). Wire VDD to 3V3: this node does not drive the strong pull-up parasite conversions need.");
  if (g_dsCount >= MAX_DS)
    Serial.printf("[ds] the list is full at %d. More sensors may be on the wire; this node reports the first %d it finds.\n", MAX_DS, MAX_DS);
}

// Rebuild the bus: release the pin, let it settle, then re-enumerate. A 1-Wire bus
// has no clock line to walk a stuck slave off, so there is nothing clever to do
// here — what this recovers is a sensor that was unplugged and plugged back in, a
// brownout that left the line confused, or a cable that was re-made. Rate-limited,
// or a permanently-removed sensor would have the node rebuilding forever.
static void busRecover() {
  if (lastBusRecoverMs && millis() - lastBusRecoverMs < BUS_RECOVER_MIN_MS) return;
  lastBusRecoverMs = millis();
  oatcore::countBusReset();
  g_busResets++;
  owBusInit();
  delay(20);
  startSensors();
  Serial.printf("[ds] bus re-scanned (rebuild #%u, %d sensor%s)\n", g_busResets, g_dsCount, g_dsCount == 1 ? "" : "s");
}

// Start a conversion on every sensor. The answer is not ready for up to 750 ms at
// 12-bit, so the reading half happens later, from the loop: blocking the whole
// node for three quarters of a second every cycle would stall the setup page and
// the push engine for no reason.
void sampleSensors() {
  if (g_dsCount == 0) {
    // Nothing answered at boot. Re-scan on a slow cadence rather than never: a
    // sensor wired up after power-on, or a bus that came back, should be found
    // without a reboot. Absent by design is not the same as broken, so this does
    // not count as a failure anywhere.
    if (!lastBlindRecoverMs || millis() - lastBlindRecoverMs > BLIND_RECOVER_MS) {
      lastBlindRecoverMs = millis();
      startSensors();
    }
    if (g_dsCount == 0) { lastReadMsg = "no sensor on the wire"; return; }
  }
  if (convPending) return;                     // a conversion is already in flight
  if (!dsConvertAll()) {
    lastReadMsg = "bus did not answer the convert command";
    oatcore::countRead(false, false);
    if (++g_consecBusFail >= BUS_FAIL_RESET) { g_consecBusFail = 0; busRecover(); }
    return;
  }
  g_consecBusFail = 0;
  convStartedMs = millis();
  convPending = true;
}

// The second half of a reading: collect every sensor's scratchpad once the
// conversion window has passed, CRC-check it, and fold what survives.
void collectConversion() {
  if (!convPending || millis() - convStartedMs < dsConvMs()) return;
  convPending = false;

  String msg;
  bool anyOk = false;
  for (int i = 0; i < g_dsCount; i++) {
    DsSensor &s = sensors[i];
    uint8_t sp[9];
    DsReadResult rr = dsReadScratchpad(s.rom, sp);
    bool ok = (rr == DS_OK);
    bool crcBad = (rr == DS_CRC);
    float t = NAN;

    if (ok) {
      int16_t raw = ((int16_t)sp[1] << 8) | sp[0];
      bool resetValue;
      if (s.family == 0x10) {
        // DS18S20: 9 bits in half-degree steps. The datasheet's extended-precision
        // trick recovers the rest from the count registers — shift to the B20's
        // 1/16 scale, then correct with COUNT_PER_C (sp[7]) and COUNT_REMAIN (sp[6]).
        resetValue = (raw == 0x00AA);          // 85.0 C in half-degree steps
        raw = raw << 3;
        if (sp[7] == 0x10) raw = (raw & 0xFFF0) + 12 - sp[6];
      } else {
        // DS18B20: 1/16 C steps. Lower resolutions leave the unused low bits
        // undefined — mask them rather than turning noise into decimal places.
        resetValue = (raw == 0x0550);          // 85.0 C in sixteenth-degree steps
        switch ((sp[4] >> 5) & 0x03) {
          case 0: raw &= ~7; break;            //  9-bit
          case 1: raw &= ~3; break;            // 10-bit
          case 2: raw &= ~1; break;            // 11-bit
          default: break;                      // 12-bit: all bits count
        }
      }
      t = raw / 16.0f;
      // 85.00 exactly is the power-on value of the temperature register: the
      // sensor was reset, or the conversion never ran, and the register was never
      // written. Publishing it as a measurement is publishing a default as an
      // observation. (If you genuinely need to measure 85 C, this node is the
      // wrong tool — say so rather than quietly making it up.)
      if (resetValue) { ok = false; s.reset85++; g_reset85++; }
    }

    if (ok) {
      t += g_off;
      s.present = true; s.everSeen = true;
      s.readsOk++; s.consecFail = 0;
      s.lastT = t; s.lastOkMs = millis();
      oatcore::countRead(true, false);
      oatcore::fold(s.slot, "temperature", "Cel", oat::KIND_CONTINUOUS, t);
      anyOk = true;
      if (msg.length()) msg += " | ";
      msg += String(i + 1) + ": " + String(t, 2) + "C";
    } else {
      s.readsFail++;
      if (crcBad) s.crcFail++;
      oatcore::countRead(false, crcBad);
      if (s.consecFail < 255) s.consecFail++;
      // Stop calling it present once it has clearly gone. Otherwise the status page
      // keeps showing an hours-old number in live-reading type, which is its own
      // kind of lie.
      if (s.consecFail >= PRESENT_FAIL_DROP && s.present) {
        s.present = false; s.lastT = NAN;
        oatcore::release(s.slot);            // stop pushing it: an average from a probe
        s.slot = -1;                          // that has gone is a number with no owner
        Serial.printf("[ds] %s stopped answering — dropped from the live view\n", s.serial);
      }
      if (msg.length()) msg += " | ";
      msg += String(i + 1) + ": " + (crcBad ? "CRC fail" : "no answer");
    }
  }

  lastReadMsg = msg.length() ? msg : "no sensor answered";
  if (!anyOk && ++g_consecBusFail >= BUS_FAIL_RESET) { g_consecBusFail = 0; busRecover(); }
  else if (anyOk) g_consecBusFail = 0;
}

// A user-initiated read: issue the conversion, wait for it, collect. Blocking is
// fine here because a person is watching a page or a console line, waiting.
void sampleBlocking() {
  sampleSensors();
  if (!convPending) return;
  delay(dsConvMs());
  collectConversion();
}

// What is out there, for the wiring-troubleshooting answer on the status page and
// at the Console. Reports every 1-Wire device, including any that is not a
// DS18B20, because "something answered but it is not a thermometer" is a different
// problem from "nothing answered".
String busScan() {
  String found;
  uint8_t rom[8];
  bool first = true;
  int n = 0;
  while (n < 24 && owSearch(rom, first)) {
    first = false;
    n++;
    if (found.length()) found += ", ";
    found += romHex(rom);
    if (dsCrc8(rom, 7) != rom[7]) found += " (bad CRC)";
    else if (rom[0] != 0x28)      found += " (not a DS18B20)";
  }
  return n ? found : String("nothing answered");
}

// ---------------------------------------------------------------------------
// What the operator sees
// ---------------------------------------------------------------------------
static String statusHtml() {
  String p;
  for (int i = 0; i < g_dsCount; i++) {
    DsSensor &s = sensors[i];
    if (!s.present) {
      if (s.everSeen)
        p += "<p class='bad'>" + String(i + 1) + " (" + String(s.serial) + "): stopped answering after " +
             String(s.readsOk) + " good reads. Last one " + String((millis() - s.lastOkMs) / 1000) +
             "s ago. Failed " + String(s.readsFail) + " times since (CRC " + String(s.crcFail) + ").</p>";
      continue;
    }
    p += "<div style='margin:.6rem 0'><span class='pill'>" + String(i + 1) + "</span>";
    p += "<span class='pill'>" + String(familyName(s.family)) + "</span><br>";
    p += "<span class='big'>" + String(s.lastT, 2) + " &deg;C</span>";
    p += "<div class='muted'>" + String(s.serial) + " &middot; reads ok " + String(s.readsOk) +
         " / failed " + String(s.readsFail) + " (CRC " + String(s.crcFail) + ")" +
         (s.reset85 ? (" &middot; 85 C discards " + String(s.reset85)) : "") +
         " &middot; last good " + String((millis() - s.lastOkMs) / 1000) + "s ago</div></div>";
  }
  if (g_parasite)
    p += "<p class='bad'>A probe is running on parasite power (no VDD wire). Wire VDD to 3V3: this node does not drive the strong pull-up parasite conversions need, so those readings cannot be trusted.</p>";
  if (g_foreign) p += "<div class='muted'>" + String(g_foreign) + " device(s) on the wire are not temperature probes and are being left alone.</div>";
  if (g_romBad)  p += "<div class='muted'>" + String(g_romBad) + " address(es) failed their own CRC — usually a marginal bus: check the resistor, shorten the run, or try 2.2k.</div>";
  p += "<div class='muted'>Last read: " + lastReadMsg + "</div>";
  return p;
}

static String statusText() {
  String s = "1-wire pin=" + String(g_pin) + " res=" + String(g_res) + "-bit probes=" + String(g_dsCount) +
             " re-scans=" + String(g_busResets) + (g_parasite ? " PARASITE-POWERED (wire VDD to 3V3)" : "") + "\n";
  for (int i = 0; i < g_dsCount; i++) {
    DsSensor &d = sensors[i];
    if (d.present)
      s += "probe " + String(i + 1) + " " + String(d.serial) + " (" + familyName(d.family) + ") temp=" +
           String(d.lastT, 2) + "C ok=" + String(d.readsOk) + " fail=" + String(d.readsFail) +
           " crc=" + String(d.crcFail) + "\n";
    else if (d.everSeen)
      s += "probe " + String(i + 1) + " GONE (" + String(d.serial) + ") — " + String(d.readsOk) +
           " good reads, last " + String((millis() - d.lastOkMs) / 1000) + "s ago\n";
  }
  if (g_dsCount == 0)
    s += "probes: none answering — check 3V3/GND, the data pin, and the 4.7k resistor to 3V3; try 'bus'\n";
  if (g_reset85) s += "85 C discards: " + String(g_reset85) + " (power-on register value, not a measurement)\n";
  return s;
}

// One line, for the boot log and the status page. Reports only what it measured.
static String diagLine() { return owLineDiag(); }

// Everything, for /diag and the Console's `diag`.
static String diagFull() {
  String s = owLineDiag() + "\n" + owPresenceTrace() + "\n";
  s += "1-wire addresses answering: " + busScan() + "\n";
  s += "sweeping every usable pin for a bus:\n" + owScanPins();
  return s;
}

// ---------------------------------------------------------------------------
// Settings, as data. One entry reaches the setup form, the Console's `set`, and NVS.
// ---------------------------------------------------------------------------
static String getPin() { return String(g_pin); }
static bool   setPin(const String& v, String& why) {
  int p = (int) v.toInt();
  if (!pinOkForOneWire(p, why)) { why = "data pin rejected: " + why; return false; }
  g_pin = p;
  return true;
}
static String getRes() { return String(g_res); }
static bool   setRes(const String& v, String& why) {
  uint32_t b = (uint32_t) v.toInt();
  if (b < 9 || b > 12) { why = "resolution is 9, 10, 11 or 12 bits"; return false; }
  g_res = b;
  return true;
}
static String getOff() { return String(g_off, 2); }
static bool   setOff(const String& v, String& why) {
  float f = v.toFloat();
  if (f < -20.0f || f > 20.0f) { why = "offset must be between -20 and +20 C"; return false; }
  g_off = f;
  return true;
}

static const oatcore::Field FIELDS[] = {
  { "ow",    "1-Wire data pin",
    "One data pin carries every probe. Don't forget the 4.7k resistor from the data line to 3V3.",
    getPin, setPin },
  { "res",   "Resolution (9-12 bits)",
    "12 bits is 0.0625 &deg;C and a 750 ms conversion; 9 bits is 0.5 &deg;C and 94 ms. A DS18S20 ignores this and always takes 750 ms.",
    getRes, setRes },
  { "t_off", "Temperature offset (&deg;C)",
    "Added to every reading, on every probe. Use it to line this node up against a reference you trust.",
    getOff, setOff },
};

// ---------------------------------------------------------------------------
// Console commands the bus adds (the core already has read / rescan / diag)
// ---------------------------------------------------------------------------
static void cmdBus(const String& rest)  { Serial.println("[ds] " + owLineDiag());
                                          Serial.println("[ds] " + owPresenceTrace());
                                          Serial.println("[ds] 1-wire: " + busScan()); }
static void cmdPins(const String& rest) { Serial.println("[ds] sweeping every usable pin for a 1-Wire bus:");
                                          Serial.println(owScanPins()); }

static const oatcore::Command COMMANDS[] = {
  { "bus",  "diagnose this pin and list the addresses that answer", cmdBus },
  { "pins", "sweep every usable pin: where is the bus really?",     cmdPins },
};

// ---------------------------------------------------------------------------
static const oatcore::Driver DRIVER = {
  TIER, "The probes", FW_VERSION, FW_SEMVER, NVS_NS,
  startSensors, sampleSensors, collectConversion, sampleBlocking, startSensors,
  statusHtml, statusText, diagFull, diagLine,
  FIELDS,   (int)(sizeof(FIELDS)   / sizeof(FIELDS[0])),
  COMMANDS, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
};

void setup() { oatcore::begin(DRIVER); }
void loop()  { oatcore::loop(); }
