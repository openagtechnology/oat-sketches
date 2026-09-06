/* =============================================================================
   oat_lora_radio — one radio object for both Heltec boards, via RadioLib
   -----------------------------------------------------------------------------
   The Heltec WiFi LoRa 32 V2 carries an SX1276, the V3 an SX1262. RadioLib drives
   both behind PhysicalLayer, so the sketches hold a PhysicalLayer* and never
   branch on the chip. What differs is only the wiring, pinned here per board, and
   the V3's TCXO + DIO2 RF switch, which its begin() must be told about or the
   radio transmits into nothing and hears nothing — the silent failure every
   first-time Heltec V3 build hits.

   Pin maps are the ones Meshtastic's board variants use for these exact boards;
   they are the most-flashed LoRa firmware there is, so they are the best-verified
   wiring available short of the schematic. Verify on your board: if the sketch
   boots but `radio` reports an init error, the pins are the first suspect.

   Board selection is by PlatformIO env: -DOAT_BOARD_HELTEC_V2, _V3 or _V4.

   THE V4 IS NOT A V3 WITH A NEW STICKER. Between its SX1262 and the antenna sits a
   radio front-end module (a GC1109 on V4.2 boards, a KCT8103L on V4.3) with a
   power amplifier and a low-noise amplifier. It is powered by an LDO on GPIO 7,
   enabled by GPIO 2, and its TX/RX path follows the SX1262's DIO2 RF-switch line;
   one more GPIO picks full-PA vs bypass (GC1109, GPIO 46) or LNA vs bypass on
   receive (KCT8103L, GPIO 5). Which chip a board carries is detected the way
   Meshtastic does it: GPIO 2 read as an input floats HIGH on the KCT8103L board
   and LOW on the GC1109 board. With the module unpowered NOTHING passes in either
   direction — a V4 flashed with a V3 image reports "radio ok" and is deaf and
   mute. (Bench, 2026-09-05: a whole afternoon of that.) The PA adds ~+10 dB, so
   the V4's default transmit setting is 10 dBm, about +20 dBm at the antenna.
   Sequence and pin facts from meshtastic/firmware variants/esp32s3/heltec_v4 and
   src/mesh/LoRaFEMInterface.cpp.
   ============================================================================= */
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "oat_lora_frame.h"

namespace oatlora {

#if defined(OAT_BOARD_HELTEC_V4)
  // Heltec WiFi LoRa 32 V4: ESP32-S3 + SX1262 + RF front-end module (PA/LNA)
  static const char* BOARD_NAME = "Heltec WiFi LoRa 32 V4";
  static const int PIN_SCK = 9, PIN_MISO = 11, PIN_MOSI = 10, PIN_NSS = 8;
  static const int PIN_RST = 12, PIN_BUSY = 13, PIN_DIO1 = 14;
  static const int PIN_VEXT = 36;          // LOW = Vext on (OLED, and "the lora antenna boost" per Heltec)
  static const int PIN_LED  = 35;
  static const int DEF_BATT_PIN = 1, DEF_BATT_CTRL = 37;  // V4: ADC_CTRL is active HIGH (V3 is active LOW)
  static const bool BATT_CTRL_ACTIVE_HIGH = true;
  static const float BATT_MULT = 4.9f * 1.045f;
  static const int DEF_SDA = 17, DEF_SCL = 18;            // the OLED's bus (OLED model); sensors share it
  // GPIO 2, 5, 7 and 46 belong to the front end; 3 is a strapping pin; 8-14 the radio.
  // ADC1 on the S3 is GPIO 1-10, which leaves 4 and 6 for analog on a V4: past two
  // probes, an ADS1115 on the I2C bus is the honest path.
  static const int DEF_DS_PIN = 33;
  static const int DEF_LIGHT_PIN = -1;
  static const char* DEF_SOIL_PINS = "";
  static const int SUG_LIGHT_PIN = 4; static const char* SUG_SOIL_PINS = "6";
  static const int DEF_POD_RX = 47, DEF_POD_TX = 48;
  static const int8_t DEF_TX_DBM = 10;                    // +~10 dB in the front end -> ~20 dBm at the antenna
  static const bool HAS_FEM = true;
  static const int PIN_FEM_POWER = 7, PIN_FEM_CSD = 2, PIN_FEM_GC_TXEN = 46, PIN_FEM_KCT_CTX = 5;
  typedef SX1262 RadioChip;
#elif defined(OAT_BOARD_HELTEC_V3)
  // Heltec WiFi LoRa 32 V3: ESP32-S3 + SX1262
  static const char* BOARD_NAME = "Heltec WiFi LoRa 32 V3";
  static const int PIN_SCK = 9, PIN_MISO = 11, PIN_MOSI = 10, PIN_NSS = 8;
  static const int PIN_RST = 12, PIN_BUSY = 13, PIN_DIO1 = 14;
  static const int PIN_VEXT = 36;          // LOW = Vext 3V3 on (powers the OLED and the Vext header pin)
  static const int PIN_LED  = 35;
  static const int DEF_BATT_PIN = 1, DEF_BATT_CTRL = 37;  // ADC_CTRL LOW enables the divider
  static const bool BATT_CTRL_ACTIVE_HIGH = false;
  static const float BATT_MULT = 4.9f;     // 390k/100k divider on the V3
  static const int DEF_SDA = 17, DEF_SCL = 18;            // the OLED's bus; sensors share it
  static const int DEF_DS_PIN = 7;
  static const int DEF_LIGHT_PIN = -1;                     // analog is DECLARED, never discovered:
  static const char* DEF_SOIL_PINS = "";                   // a floating pin reads like a wet probe
  static const int SUG_LIGHT_PIN = 2; static const char* SUG_SOIL_PINS = "3,4,5,6";   // the pins the page suggests
  static const int DEF_POD_RX = 40, DEF_POD_TX = 41;      // the serial pod port (oat-line in)
  static const int8_t DEF_TX_DBM = 17;
  static const bool HAS_FEM = false;
  static const int PIN_FEM_POWER = -1, PIN_FEM_CSD = -1, PIN_FEM_GC_TXEN = -1, PIN_FEM_KCT_CTX = -1;
  typedef SX1262 RadioChip;
#elif defined(OAT_BOARD_HELTEC_V2)
  // Heltec WiFi LoRa 32 V2 / V2.1: classic ESP32 + SX1276
  static const char* BOARD_NAME = "Heltec WiFi LoRa 32 V2";
  static const int PIN_SCK = 5, PIN_MISO = 19, PIN_MOSI = 27, PIN_NSS = 18;
  static const int PIN_RST = 14, PIN_DIO0 = 26, PIN_DIO1 = 35;
  static const int PIN_VEXT = 21;          // LOW = Vext on
  static const int PIN_LED  = 25;
  static const int DEF_BATT_PIN = 37, DEF_BATT_CTRL = -1;
  static const bool BATT_CTRL_ACTIVE_HIGH = false;
  static const float BATT_MULT = 3.2f;     // 220k/100k divider on the V2.1
  static const int DEF_SDA = 4, DEF_SCL = 15;             // the OLED's bus; sensors share it
  static const int DEF_DS_PIN = 13;
  static const int DEF_LIGHT_PIN = -1;                     // analog is DECLARED, never discovered
  static const char* DEF_SOIL_PINS = "";
  static const int SUG_LIGHT_PIN = 36; static const char* SUG_SOIL_PINS = "38,39,32,33";   // all ADC1
  static const int DEF_POD_RX = 17, DEF_POD_TX = 23;      // the serial pod port (oat-line in)
  static const int8_t DEF_TX_DBM = 17;
  static const bool HAS_FEM = false;
  static const int PIN_FEM_POWER = -1, PIN_FEM_CSD = -1, PIN_FEM_GC_TXEN = -1, PIN_FEM_KCT_CTX = -1;
  typedef SX1276 RadioChip;
#else
  #error "Define OAT_BOARD_HELTEC_V2, _V3 or _V4 in platformio.ini"
#endif

enum FemType : uint8_t { FEM_NONE = 0, FEM_GC1109 = 1, FEM_KCT8103L = 2, FEM_UNKNOWN = 3 };

class Radio {
 public:
  PhysicalLayer* phy = nullptr;
  int lastState = RADIOLIB_ERR_NONE;
  RadioPlan plan = DEFAULT_PLAN;
  FemType fem = FEM_NONE;

  // The V4 front end. Power the LDO, detect the chip by reading GPIO 2 as an input
  // (KCT8103L pulls it high, GC1109 leaves it low), enable it, and park it in
  // receive. TX/RX path selection rides DIO2 automatically; the one line we drive
  // around each transmit is the PA-mode (GC1109) or LNA-mode (KCT8103L) pin.
  void femInit() {
    if (!HAS_FEM) return;
    pinMode(PIN_FEM_POWER, OUTPUT); digitalWrite(PIN_FEM_POWER, HIGH); delay(2);
    pinMode(PIN_FEM_CSD, INPUT); delay(2);
    if (digitalRead(PIN_FEM_CSD) == HIGH) {
      fem = FEM_KCT8103L;
      pinMode(PIN_FEM_CSD, OUTPUT); digitalWrite(PIN_FEM_CSD, HIGH);
      pinMode(PIN_FEM_KCT_CTX, OUTPUT); digitalWrite(PIN_FEM_KCT_CTX, LOW);    // LNA on for receive
    } else {
      fem = FEM_GC1109;
      pinMode(PIN_FEM_CSD, OUTPUT); digitalWrite(PIN_FEM_CSD, HIGH);
      pinMode(PIN_FEM_GC_TXEN, OUTPUT); digitalWrite(PIN_FEM_GC_TXEN, LOW);    // bypass until a transmit
    }
  }
  void femTx() {
    if (fem == FEM_GC1109)   digitalWrite(PIN_FEM_GC_TXEN, HIGH);
    if (fem == FEM_KCT8103L) digitalWrite(PIN_FEM_KCT_CTX, HIGH);
  }
  void femRx() {
    if (fem == FEM_GC1109)   digitalWrite(PIN_FEM_GC_TXEN, LOW);
    if (fem == FEM_KCT8103L) digitalWrite(PIN_FEM_KCT_CTX, LOW);
  }
  static const char* femName(FemType f) { return f == FEM_GC1109 ? "GC1109 front end" : f == FEM_KCT8103L ? "KCT8103L front end" : f == FEM_NONE ? "no front end" : "unknown front end"; }

  // Bring the chip up on the board's SPI pins with the plan. Safe to call again
  // after a plan change: it re-inits the chip.
  bool begin(const RadioPlan& p) {
    plan = p;
    pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);
    if (fem == FEM_NONE && HAS_FEM) femInit();
    if (!chip) {
      spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
#if defined(OAT_BOARD_HELTEC_V3) || defined(OAT_BOARD_HELTEC_V4)
      chip = new RadioChip(new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, spi));
#else
      chip = new RadioChip(new Module(PIN_NSS, PIN_DIO0, PIN_RST, PIN_DIO1, spi));
#endif
      phy = chip;
    }
#if defined(OAT_BOARD_HELTEC_V3) || defined(OAT_BOARD_HELTEC_V4)
    // 1.8 V TCXO on DIO3, RF switch on DIO2: the two facts a V3 will not work without.
    lastState = chip->begin(plan.freqMHz, plan.bwKHz, plan.sf, plan.cr, plan.syncWord, plan.txDbm, plan.preamble, 1.8f, false);
    if (lastState == RADIOLIB_ERR_NONE) chip->setDio2AsRfSwitch(true);
#else
    lastState = chip->begin(plan.freqMHz, plan.bwKHz, plan.sf, plan.cr, plan.syncWord, plan.txDbm, plan.preamble);
#endif
    if (lastState == RADIOLIB_ERR_NONE) chip->setCRC(true);
    return lastState == RADIOLIB_ERR_NONE;
  }

  // Listen-before-talk: channel activity detection. True when the channel is free.
  bool channelFree() {
    int st = chip->scanChannel();
    return st == RADIOLIB_CHANNEL_FREE;
  }

  // Blocking transmit with LBT and a short random back-off. Returns RadioLib state.
  int send(const uint8_t* d, size_t n) {
    for (int attempt = 0; attempt < 6; attempt++) {
      if (channelFree()) break;
      delay(50 + (esp_random() % 250));
    }
    femTx();
    lastState = chip->transmit(const_cast<uint8_t*>(d), n);
    femRx();
    return lastState;
  }

  void startReceive()                          { femRx(); chip->startReceive(); }
  void onReceive(void (*isr)())                { chip->setPacketReceivedAction(isr); }
  size_t packetLength()                        { return chip->getPacketLength(); }
  int    readData(uint8_t* buf, size_t n)      { lastState = chip->readData(buf, n); return lastState; }
  float  rssi()                                { return chip->getRSSI(); }
  float  snr()                                 { return chip->getSNR(); }
  float  rssiNow()                             { return chip->getRSSI(false); }   // instantaneous while receiving: is RF arriving at all?
  RadioChip* raw()                             { return chip; }

  static const char* stateName(int st) {
    switch (st) {
      case RADIOLIB_ERR_NONE: return "ok";
      case RADIOLIB_ERR_CHIP_NOT_FOUND: return "chip not found (SPI pins / power)";
      case RADIOLIB_ERR_PACKET_TOO_LONG: return "packet too long";
      case RADIOLIB_ERR_TX_TIMEOUT: return "tx timeout";
      case RADIOLIB_ERR_CRC_MISMATCH: return "crc mismatch";
      case RADIOLIB_ERR_INVALID_FREQUENCY: return "invalid frequency";
      case RADIOLIB_ERR_INVALID_BANDWIDTH: return "invalid bandwidth";
      case RADIOLIB_ERR_INVALID_SPREADING_FACTOR: return "invalid spreading factor";
      case RADIOLIB_ERR_INVALID_OUTPUT_POWER: return "invalid tx power";
      default: return "error";
    }
  }

 private:
  SPIClass spi{HSPI};
  RadioChip* chip = nullptr;
};

// Battery: read the divider, return volts (0 when no pin). % is a LiPo-curve
// estimate honest enough for "is it dying", never a fuel gauge. Above MAINS_V the
// divider is reading the charger rail with no cell behind it (a V4 on USB read
// 4.25 V and said "100 %"): that is mains, and the honest battery figure is none.
inline float readBatteryVolts(int pin, int ctrl, float mult) {
  if (pin < 0) return 0.0f;
  if (ctrl >= 0) { pinMode(ctrl, OUTPUT); digitalWrite(ctrl, BATT_CTRL_ACTIVE_HIGH ? HIGH : LOW); delay(5); }
  analogReadMilliVolts(pin);                 // configures the channel; attenuation only sticks after this
  analogSetPinAttenuation(pin, ADC_11db);
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(pin);
  if (ctrl >= 0) digitalWrite(ctrl, BATT_CTRL_ACTIVE_HIGH ? LOW : HIGH);
  return (mv / 8) * mult / 1000.0f;
}
static const float MAINS_V = 4.23f;        // a full LiPo rests at 4.20; only a charger rail sits above
inline bool onMains(float v) { return v >= MAINS_V; }
inline uint8_t batteryPercent(float v) {
  if (v <= 0.5f || onMains(v)) return BATT_NA;
  if (v >= 4.15f) return 100;
  if (v <= 3.30f) return 0;
  return (uint8_t)((v - 3.30f) / (4.15f - 3.30f) * 100.0f);
}

}  // namespace oatlora
