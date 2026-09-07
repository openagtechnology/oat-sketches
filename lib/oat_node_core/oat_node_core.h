/* =============================================================================
   oat_node_core — everything an OAT node does EXCEPT talk to its sensor
   -----------------------------------------------------------------------------
   WHY THIS EXISTS

   The BLE Listener was always meant to be the model every other sketch followed,
   changing only the sensor actions. In practice each new sketch COPIED it, and by
   the third one about 1,200 lines were duplicated three times. Every bug then
   lived in exactly one copy: a buffer sized for a short sensor id, a stream-naming
   habit that contradicted our own published standard, NVS log noise, a bit-timing
   fault. Copy-paste guarantees that, and it gets worse per sketch.

   So the core is a LIBRARY, not a file to copy. A sketch supplies a Driver and
   nothing else. If a sketch contains WiFi, config, web, console or push code, it
   is wrong by construction.

   WHAT THE CORE OWNS
     - Config + NVS, and the field registry that makes the web form and the serial
       Console the same surface (they can never drift, because there is one list).
     - WiFi station + setup AP + captive portal, connect-then-confirm, mDNS.
     - The setup page, the status page, the two-way USB Console.
     - The push engine: https preferred, signed http fallback, HMAC, oat-ods batch
       envelope, chunking sized from the largest free heap block, monotonic seq.
     - The 60 s heartbeat (a fixed constant of oat-ods, not a setting).
     - The measurand window fold, and the slot table the batch is built from.

   WHAT A DRIVER OWNS
     - Talking to the part. Discovery, timing, checksums, decode, its own failure
       counters and its own status rendering.
     - Extra config fields and extra Console commands, declared as data so the core
       renders and routes them without knowing what they are.

   CONFORMANCE
     stream.id is the sensor's own hardware id, per oat-ods §3/§4 ("defaults to the
     MAC if the grower assigned no name"). The node names nothing: the endpoint owns
     the hardware-to-place mapping, because that is where it survives a reflash and
     can be edited without visiting the node. Drivers pass ids, never labels.

   ============================================================================= */
#pragma once
#include <Arduino.h>
#include <oat_measurands.h>
#include <oat_ods.h>          // oat::NO_RSSI is part of the driver API (slotLink)

namespace oatcore {

// Slots are sensors-worth-of-readings. A one-probe node uses one; a BLE listener in
// a busy room uses many. Override per sketch with -DOAT_MAX_SLOTS / -DOAT_MEAS_PER_SLOT.
#ifndef OAT_MAX_SLOTS
  #define OAT_MAX_SLOTS 12
#endif
#ifndef OAT_MEAS_PER_SLOT
  #define OAT_MEAS_PER_SLOT 6
#endif
static const int MAX_SLOTS     = OAT_MAX_SLOTS;
static const int MEAS_PER_SLOT = OAT_MEAS_PER_SLOT;

// Ids are hardware ids: a MAC, a 64-bit 1-Wire address with its family prefix, a
// chip serial. 40 bytes because "ds18b20:" + 16 hex + NUL is 25 and a 24-byte field
// silently truncated it — two probes differing in the last nibble reported as one.
static const int ID_LEN = 40;

// ---------------------------------------------------------------------------
// A config field a driver adds to the shared registry. The core renders it on the
// setup page, exposes it to `set` on the Console, and persists it — the driver only
// says what it is called and how to read and write it.
//
// set() returns false WITH a reason. That is load-bearing: a bad pin is persisted
// and re-applied at every boot, so it is not a mistake the user gets to notice and
// correct. Refusing it with the reason is the difference between a guard and a trap.
// ---------------------------------------------------------------------------
struct Field {
  const char* key;                     // NVS key + form field name (keep <= 12 chars)
  const char* label;                   // form label
  const char* help;                    // muted hint under the row ("" for none)
  String (*get)();
  bool   (*set)(const String& value, String& why);
};

// A Console command a driver adds. Same list feeds `help`.
struct Command {
  const char* name;
  const char* help;                    // one line, shown by `help`
  void (*run)(const String& rest);
};

// A driver: how to talk to one kind of part.
struct Driver {
  const char* tier;                    // oat-ods source.tier, e.g. "oat-ds18b20-node"
  const char* what;                    // heading for the sensor section, e.g. "The probes"
  const char* fw_version;              // "OAT-DS18B20-Node/1.2.0" — goes in every payload
  const char* fw_semver;               // "1.2.0" — the flasher manifest's version
  const char* nvs;                     // NVS namespace, e.g. "oatds" (<= 15 chars). Its own,
                                       // so a board reflashed from another OAT sketch never
                                       // inherits that sketch's settings.

  void (*begin)();                     // bus init + discovery. Call slotFor() per sensor found.
  void (*startSample)();               // take (or issue) a reading. Called on the sample timer.
  void (*collect)();                   // called EVERY loop and must be cheap: finish an async
                                       // read whose window has passed, or do nothing. A DS18B20
                                       // conversion is 750 ms, and blocking the node for that
                                       // would stall the setup page and the push engine.
  void (*sampleBlocking)();            // a person pressed "read now" and is waiting. May be null.
  void (*rescan)();                    // re-enumerate the bus. May be null.

  String (*statusHtml)();              // per-sensor block for the status page. May be null.
  String (*statusText)();              // same, for the Console's `status`. May be null.
  String (*diagFull)();                // multi-line diagnosis for the /diag page and the
                                       // Console's `diag`. May be null.
  String (*diag)();                    // one line of bus diagnosis, printed when nothing is
                                       // found. Must report only what it MEASURED — a
                                       // diagnostic that asserts a conclusion it never tested
                                       // sends people to check wiring that was always fine.

  const Field*   fields;    int nFields;
  const Command* commands;  int nCommands;
};

// ---------------------------------------------------------------------------
// The slot table — what the batch is built from.
// ---------------------------------------------------------------------------

// Find the slot for a hardware id, or claim a free one. Returns -1 when full.
// `physical` is the same id in nearly every case; it is separate because oat-ods
// keeps stream identity and hardware provenance as distinct fields.
int  slotFor(const char* id, const char* physical);

// Fold one measurand into a slot's window. kind picks the fold: continuous
// averages, event counts, everything else keeps the latest. Units are SenML
// symbols from the shared measurand registry ("Cel", "%RH"), never invented here.
void fold(int slot, const char* measurement, const char* unit, uint8_t kind, double value);

// Drop a slot from the next push (a sensor that has gone). The window is cleared:
// publishing an average of readings from a sensor that has since vanished would be
// a number with no owner.
void release(int slot);

// Provenance for this slot's readings: who made the part and what it is. Sticky —
// set it once at discovery. oat-ods source.brand / source.model.
//
// This exists because the extracted core inherited a HARDCODED brand/model from the
// sketch it was copied out of, so every DS18B20 reading went out labelled as a
// Sensirion SHT30. A provenance field naming the wrong chip is worse than an absent
// one: it is confidently wrong, and an endpoint has no way to know.
void slotMeta(int slot, const char* brand, const char* model);

// A label the DEVICE supplies about itself — a BLE sensor's advertised name, say.
// This is not the node naming anything: the node still names nothing, and stream.id
// is still the hardware id. It is passing along a fact the sensor broadcast, which
// is worth keeping because "GVH5100_484B" tells a person which Govee this is and a
// bare MAC does not. Drivers with nothing to pass simply never call it.
void slotName(int slot, const char* name);

// Link metadata for a radio sensor: latest wins. -1 leaves a value unset, which is
// how a mains-powered wired sensor says "battery does not apply to me" rather than
// claiming 0%.
void slotLink(int slot, int rssi, int battery_pct);   // rssi oat::NO_RSSI (or -1) = no level; battery -1 = none

int  slotCount();                      // slots currently claimed
const char* slotId(int slot);          // for a driver's own status rendering

// ---------------------------------------------------------------------------
// Driver state that must survive a reboot: a gateway's learned rosters, say.
// The core owns NVS, and a driver never includes Preferences — that is the
// contract, and the sketch checker enforces it. This is the one door. Blobs live
// in the driver's own namespace beside its config, so a factory reset clears
// them with everything else and a board reflashed from another sketch never
// inherits them. Keys are NVS keys: 15 characters at most.
// ---------------------------------------------------------------------------
size_t blobLoad(const char* key, void* buf, size_t max);      // bytes read; 0 = absent
bool   blobSave(const char* key, const void* data, size_t n); // unchanged data is not rewritten (NVS compares)
void   blobErase(const char* key);

// ---------------------------------------------------------------------------
// Node lifecycle. A sketch's setup() and loop() are two lines.
// ---------------------------------------------------------------------------
void begin(const Driver& driver);
void loop();

// Things a driver legitimately needs from the node's own config.
const String& deviceId();              // hardware-derived, never editable
const String& gatewayId();             // assignable node name (oat-ods source.gateway_id)
uint32_t      sampleSeconds();         // the configured read cadence

// Ask the core to push now (the Console's `push`, the status page's test button).
void pushNow();

// A read-only snapshot of node state for a driver that has somewhere to show it —
// a board with a screen, say. Drivers must not touch WiFi themselves (the core
// owns the radio), so this is the one door to that state.
struct NodeStatus {
  bool          wifi;          // station connected
  String        ip;            // LAN address when connected, else ""
  String        apSsid;        // the setup network's name
  bool          lastPushOk;
  uint32_t      pushOk, pushFail;
  unsigned long lastPushMs;    // millis() of the last push attempt, 0 = none yet
  String        lastPushMsg;
};
NodeStatus status();

// Counters the core shows and a driver contributes to, so "is my wiring alright?"
// has one answer rather than one per sketch.
void countRead(bool ok, bool crcFail);
void countBusReset();

}  // namespace oatcore
