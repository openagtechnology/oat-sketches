/* =============================================================================
   oat_lora_screen — the Heltec boards' OLED, six short lines
   -----------------------------------------------------------------------------
   Both Heltec WiFi LoRa 32 boards carry a 128x64 SSD1306 on I2C. These are the
   first OAT nodes a person walks away from, so the board itself says whether it
   is alive: the field node shows what it found and when it last transmitted; the
   gateway shows what it hears and whether the push landed. A blank screen on a
   powered board then means "did not boot", which separates a flashing problem
   from a radio problem before anyone opens a console.

   The OLED shares the I2C pins the SHT-30 defaults to. A sketch that moves the
   sensor to other pins puts it on Wire1 and leaves this bus alone.

   Refresh costs ~25 ms of blocking I2C at 400 kHz, so callers refresh on events
   and every couple of seconds, never every loop.
   ============================================================================= */
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

namespace oatlora {

#if defined(OAT_BOARD_HELTEC_V3) || defined(OAT_BOARD_HELTEC_V4)
  static const int OLED_SDA = 17, OLED_SCL = 18, OLED_RST = 21;   // the V4's OLED model shares the V3 wiring
#elif defined(OAT_BOARD_HELTEC_V2)
  static const int OLED_SDA = 4, OLED_SCL = 15, OLED_RST = 16;
#else
  #error "Define OAT_BOARD_HELTEC_V2, _V3 or _V4 in platformio.ini"
#endif

static const int SCREEN_LINES = 6;
static const int SCREEN_COLS  = 21;     // 6x10 font on 128 px

class Screen {
 public:
  bool ok = false;

  bool begin() {
    // Vext must already be on (the radio's begin() does that): the OLED is fed from it.
    u8g2 = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);
    u8g2->setBusClock(400000);
    ok = u8g2->begin();
    if (ok) { u8g2->setFont(u8g2_font_6x10_tf); u8g2->setFontMode(1); }
    return ok;
  }

  // Draw up to six lines. Longer lines are clipped, not wrapped: a status screen
  // that reflows is one nobody can read at a glance.
  void show(const String* lines, int n) {
    if (!ok) return;
    u8g2->clearBuffer();
    for (int i = 0; i < n && i < SCREEN_LINES; i++) {
      String l = lines[i];
      if ((int)l.length() > SCREEN_COLS) l = l.substring(0, SCREEN_COLS);
      u8g2->drawStr(0, 9 + i * 10 + (i ? 1 : 0), l.c_str());
    }
    u8g2->sendBuffer();
  }

 private:
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C* u8g2 = nullptr;
};

inline String ago(unsigned long sinceMs) {
  if (sinceMs == 0) return "never";
  unsigned long s = (millis() - sinceMs) / 1000;
  if (s < 60) return String(s) + "s";
  if (s < 3600) return String(s / 60) + "m" + String(s % 60) + "s";
  return String(s / 3600) + "h" + String((s % 3600) / 60) + "m";
}

}  // namespace oatlora
