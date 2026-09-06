/* =============================================================================
   oat_soil.h  —  capacitive soil-moisture probes, shared by every OAT sketch
   that reads one. Header-only, no core dependency, so the standalone LoRa Field
   Node and the oat_node_core Soil-Moisture Node use the SAME implementation.
   -----------------------------------------------------------------------------
   WHAT IT OWNS
     - the read: N averaged analogReadMilliVolts() on a pin the caller declared,
       attenuation set after the first read (the ESP32 gotcha)
     - the floor: under FLOOR_MV a pin is a wire off, not a wet probe
     - the calibration table: one dry/wet pair PER PIN, because two probes from
       the same bag read hundreds of millivolts apart in the same glass of water
     - the grammar both consoles and the setup page share:
           <pin> dry | <pin> wet | <pin> clear | <pin> <dryMv> <wetMv>
       or the table it prints ("35:2876/1232 34:2610/1250"), so a stored string
       round-trips and a person can copy it onto a setup card
   WHAT IT DOES NOT OWN
     persistence (the core's Field registry or the sketch's own NVS), pin
     legality per chip (the sketch passes a pinOk callback), and which pins are
     declared (a declared callback). It refuses with a reason and never guesses.
   RULES
     - no calibration, no percentage: percent() is NAN until both points exist
     - dry and wet closer than MIN_SPAN_MV are not two different states
     - a capture under the floor is a wire off, not a calibration point
   ============================================================================= */
#pragma once
#include <Arduino.h>

namespace oatsoil {

static const int FLOOR_MV    = 150;   // under this a pin is a wire off
static const int SAMPLES     = 16;    // averaged per read; the ADC is noisy, the soil is not
static const int MIN_SPAN_MV = 100;   // dry - wet must be at least this
static const int MAX_CAL     = 8;     // entries kept (the Field Node takes eight probes)

typedef bool (*PinOkFn)(int pin, String& why);
typedef bool (*DeclaredFn)(int pin);

// The first read configures the pin as an ADC channel; attenuation can only be set
// after that. 11 dB gives the full 0-3.3 V range a probe on 3V3 produces.
inline void configurePin(int pin) { analogReadMilliVolts(pin); analogSetPinAttenuation(pin, ADC_11db); }

inline int readMv(int pin) {
  uint32_t sum = 0;
  for (int i = 0; i < SAMPLES; i++) sum += analogReadMilliVolts(pin);
  return (int)(sum / SAMPLES);
}

struct Cal { int pin; int dryMv; int wetMv; };

struct Table {
  Cal e[MAX_CAL]; int n = 0;

  Cal* find(int pin) { for (int i = 0; i < n; i++) if (e[i].pin == pin) return &e[i]; return nullptr; }
  const Cal* find(int pin) const { for (int i = 0; i < n; i++) if (e[i].pin == pin) return &e[i]; return nullptr; }
  Cal* findOrAdd(int pin) { if (Cal* c = find(pin)) return c; if (n >= MAX_CAL) return nullptr; e[n] = { pin, 0, 0 }; return &e[n++]; }
  void remove(int pin) { for (int i = 0; i < n; i++) if (e[i].pin == pin) { e[i] = e[--n]; return; } }
  void clear() { n = 0; }

  static bool valid(int dry, int wet) { return dry > 0 && wet > 0 && dry - wet >= MIN_SPAN_MV; }
  bool calibrated(int pin) const { const Cal* c = find(pin); return c && valid(c->dryMv, c->wetMv); }

  // NAN until both points exist; otherwise 0 = dry air, 100 = water, clamped.
  float percent(int pin, int mv) const {
    const Cal* c = find(pin);
    if (!c || !valid(c->dryMv, c->wetMv)) return NAN;
    float pct = (float)(c->dryMv - mv) / (float)(c->dryMv - c->wetMv) * 100.0f;
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
  }

  // "35:2876/1232 34:2610/1250" — pins with no point at all are left out.
  String toString() const {
    String t;
    for (int i = 0; i < n; i++) {
      if (!e[i].dryMv && !e[i].wetMv) continue;
      if (t.length()) t += ' ';
      t += String(e[i].pin) + ":" + String(e[i].dryMv) + "/" + String(e[i].wetMv);
    }
    return t;
  }

  // The table form. Replaces everything; refuses the whole string on any bad entry.
  bool fromString(const String& v, String& why, PinOkFn pinOk) {
    Cal fresh[MAX_CAL]; int m = 0;
    String s = v; s.trim();
    int from = 0;
    while (from < (int)s.length()) {
      int sp = s.indexOf(' ', from); if (sp < 0) sp = s.length();
      String tok = s.substring(from, sp); tok.trim();
      from = sp + 1;
      if (!tok.length()) continue;
      int c = tok.indexOf(':'), sl = tok.indexOf('/');
      if (c < 1 || sl < c) { why = "calibration entries look like 35:2876/1232"; return false; }
      int pin = tok.substring(0, c).toInt(), dry = tok.substring(c + 1, sl).toInt(), wet = tok.substring(sl + 1).toInt();
      if (pinOk && !pinOk(pin, why)) return false;
      if (dry && wet && dry - wet < MIN_SPAN_MV) { why = "GPIO " + String(pin) + ": dry and wet are only " + String(dry - wet) + " mV apart; that is not two different states"; return false; }
      if (m >= MAX_CAL) { why = "too many calibration entries"; return false; }
      fresh[m++] = { pin, dry, wet };
    }
    memcpy(e, fresh, sizeof(fresh)); n = m;
    return true;
  }

  // One entry point for a console `set cal ...` and the setup page's field:
  // the table form, or a command. Captures read the pin live.
  bool apply(const String& v, String& why, PinOkFn pinOk, DeclaredFn declared) {
    String s = v; s.trim();
    if (!s.length()) { clear(); return true; }                 // an emptied field clears every calibration
    if (s.indexOf(':') >= 0) return fromString(s, why, pinOk);
    int sp = s.indexOf(' ');
    if (sp < 0) { why = "say which: '35 dry', '35 wet', '35 clear', or '35 2876 1232'"; return false; }
    int pin = s.substring(0, sp).toInt();
    String rest = s.substring(sp + 1); rest.trim();
    if (pinOk && !pinOk(pin, why)) return false;
    if (rest == "clear") { remove(pin); return true; }
    Cal* c = findOrAdd(pin);
    if (!c) { why = "no room for another calibration entry"; return false; }
    if (rest == "dry" || rest == "wet") {
      if (declared && !declared(pin)) { why = "GPIO " + String(pin) + " is not in the pin list; declare it first, then calibrate"; return false; }
      int mv = readMv(pin);
      if (mv < FLOOR_MV) { why = "GPIO " + String(pin) + " reads " + String(mv) + " mV: that is a wire off, not a probe. Check 3V3, GND and the output wire, then try again"; return false; }
      if (rest == "dry") {
        if (c->wetMv && mv - c->wetMv < MIN_SPAN_MV) { why = "dry air read " + String(mv) + " mV, only " + String(mv - c->wetMv) + " above the water reading. Is the probe actually dry? Shake it off and wait a minute"; return false; }
        c->dryMv = mv;
      } else {
        if (c->dryMv && c->dryMv - mv < MIN_SPAN_MV) { why = "water read " + String(mv) + " mV, only " + String(c->dryMv - mv) + " below the dry reading. Is the probe in the water up to the line?"; return false; }
        c->wetMv = mv;
      }
      return true;
    }
    int sp2 = rest.indexOf(' ');
    if (sp2 > 0) {
      int dry = rest.substring(0, sp2).toInt(), wet = rest.substring(sp2 + 1).toInt();
      if (dry <= 0 || wet <= 0 || dry > 3300 || wet > 3300) { why = "millivolts, dry then wet, each between 1 and 3300"; return false; }
      if (dry - wet < MIN_SPAN_MV) { why = "dry and wet are only " + String(dry - wet) + " mV apart; that is not two different states"; return false; }
      c->dryMv = dry; c->wetMv = wet;
      return true;
    }
    why = "say which: '35 dry', '35 wet', '35 clear', or '35 2876 1232'";
    return false;
  }

  // What a status line says about a pin.
  String describe(int pin) const {
    const Cal* c = find(pin);
    if (!c || (!c->dryMv && !c->wetMv)) return "not calibrated";
    if (!valid(c->dryMv, c->wetMv)) return "half calibrated: dry " + String(c->dryMv ? String(c->dryMv) + " mV" : String("?")) + ", water " + String(c->wetMv ? String(c->wetMv) + " mV" : String("?"));
    return "dry " + String(c->dryMv) + " mV, water " + String(c->wetMv) + " mV";
  }
};

}  // namespace oatsoil
