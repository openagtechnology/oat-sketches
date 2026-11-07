// SPDX-FileCopyrightText: 2026 Open Agriculture Technology Collective
// SPDX-License-Identifier: Apache-2.0
/* =============================================================================
   OAT Weather-Station Listener  —  v1.4.4
   OpenAgricultureTechnology.com  ·  the Sketch Library (Collect layer)
   -----------------------------------------------------------------------------
   Hears the weather stations and outdoor sensors a grower already owns — the
   AcuRite Iris (5-in-1), Atlas and tower sensors, La Crosse, Oregon Scientific,
   the older Fine Offset arrays — every one of which shouts its readings into the
   air on 433 MHz for anyone to hear. This listens, harvests every value each
   device broadcasts, and pushes them as oat-ods to an endpoint the operator owns.
   The BLE Listener's sibling, tuned to the band where the outdoor sensors live.

   THREE BUILDS, ONE SOURCE (1.1.0). (1) The bare-receiver image: a 433 MHz
   superheterodyne module on one pin, this sketch's own AcuRite 5-in-1 decoder,
   proven on the bench. (2) The CC1101 / SX1276 on-off-keying images: rtl_433's
   OOK decoder set at 433.92. (3) The CC1101 FSK images (-DOOK_MODULATION=false,
   default band 915.00): the Fine Offset family sold as Ecowitt and Ambient
   Weather (WH51/WH52 soil, WH65/WS69/WS80/WS85/WS90 arrays, WH31/WN34 probes,
   WH40 rain, WH55 leak, WH57 lightning, WH41/43/45/46 air), La Crosse View and
   IT+, the Bresser 5/6/7-in-1 family. Every one of those sensors is FSK at every
   carrier it is sold on (915 Americas/Oceania, 868 Europe, 433 elsewhere), so the
   band is a setting and the modulation is the image. A radio demodulates one or
   the other, never both. STATUS 2026-09-18: builds 2 and 3 compile and have not
   yet heard a sensor on our bench; the site offers build 3 only after it has.

   THE DECODERS ARE NOT OURS. They are the rtl_433 project's, ported to the ESP32
   as rtl_433_ESP (GPL-3.0): ~220 on-off-keying device types, refreshed from the
   rtl_433 tree. We wrap them as a driver on oat_node_core; this file is the map
   from a decoder's JSON to the OAT vocabulary, and the station table. Nothing
   here decodes a radio protocol, and nothing here should: a new station is a
   decoder upstream, not a sketch release.

   THE RADIO. A CC1101 transceiver on SPI (any ESP32), or the SX1276 already on a
   Heltec WiFi LoRa 32 V2 — OR, built with -DOAT_RX_DATAPIN=<gpio>, a bare 433 MHz
   superheterodyne receiver whose DATA line drives one pin (the IBC v1.02 board's
   GPIO 33). The bare-receiver path has no RSSI to gate on, so the rtl_433 library
   cannot use it; it runs this sketch's own AcuRite 5-in-1 (Iris) decoder instead,
   the one build 4 uses (below): a gap filter, then the frame read on the
   station's bit clock. Fewer stations, same streams, same everything downstream. Both demodulate OOK and FSK, but not at the same time;
   the shipped images are OOK (Acurite, Fine Offset OOK, La Crosse, Oregon). An FSK
   build is the same source built with -DOOK_MODULATION=false. The receive
   frequency is a SETTING (band): 433.92 is where Acurite/La Crosse/Oregon live.

   TWO OUTPUTS, ONE FIRMWARE. With Wi-Fi and an endpoint configured, this is an OAT
   gateway and pushes oat-ods. With nothing configured it still prints every
   reading on the USB serial port in the oat-line grammar (the core's line output),
   so cabled to a LoRa Field Node's pod port it is a pod. Same decoder, same ids.

   NAMING LAW: the station's own id is the stream id — "<model-slug>:<id>[-<ch>]",
   e.g. acurite-5n1:2716. The listener names nothing; the endpoint maps ids to
   places. A station whose battery is swapped keeps its id on most models; the few
   that re-roll on battery change fragment their history and the page says so.

   BUILD 4 (1.2.0, bench): THE CC1101 AS A BETTER BARE RECEIVER. Built with
   -DOAT_RX_DATAPIN=<GDO0 gpio> -DOAT_RX_CC1101. The sketch programs the CC1101
   itself over SPI (433.92 MHz, on-off keying, asynchronous serial output, TI
   DN022's OOK gain settings) and never links rtl_433_ESP. The chip is then just a
   receiver whose demodulated data comes out on GDO0, and the SAME AcuRite 5-in-1
   decoder as build 1 reads it. Why: on 2026-09-20 the CC1101 heard the Iris and
   put its pulse train on GDO0 (220/408/620 us, rtl_433's own figures), but
   rtl_433_ESP decoded nothing: it keeps an edge only while RSSI is above a
   threshold, and on-off keying drops the carrier for every 0 bit. This path has no
   RSSI gate. What it adds over the bare receiver: a signal level per station (the
   chip's RSSI, read while a frame is arriving), a band that is a setting, and a
   receiver whose gain and bandwidth are registers. From 1.3.0 build 1 and build 4
   share that decoder, and bench images (-DOAT_RX_BENCH) add the live-tuning and
   capture commands the shipped ones leave out.

   BUILD 5 (1.4.0; WH52 since 1.4.1): ECOWITT THROUGH A CC1101 IN PACKET MODE. Built
   with -DOAT_RX_FSK915. The CC1101 does the FSK work in hardware: demodulation, the
   bit clock, the 2d d4 sync word, fixed 24-byte frames with its RSSI appended.
   The sketch polls the FIFO over SPI (lib/oat_rf915) and runs our own decoders
   for the WH31 (0x30/0x37), WH51 (0x51), WN34/WN38 (0x34/0x38) and WH52 (0xA2), each checked
   with the sensor's CRC-8 and SUM-8, and each reading is written with rtl_433's
   field names so the map and the stream ids are the ones build 3 would give.
   rtl_433_ESP is not linked. Only SPI is wired: GDO0 and GDO2 are not read.

   CHANGELOG
     1.4.4  Build 5 with the WH52 moves to the main listener page (2026-11-06).
            ec_raw dropped now that conductivity is fitted; moisture_ad stays.
            Builds 1-4 are unchanged since 1.4.0 and keep those images. Library
            oat_rf915 is now at 1.2.0.
     1.4.3  WH52 conductivity = (12 bits above byte 10) x 10 uS/cm, the Ecowitt
            gateway's own reading: paired endpoint data from two pots on
            2026-11-06 gave byte 9 = 8, 9, 32, 33 against the gateway's 80, 90,
            320, 330. rtl_433's /25.6 added byte 10 as a fraction (~7 % high at
            80-90). Raw counts still sent; still no WH52 battery. oat_rf915 1.1.2.
     1.4.2  WH52 heard on our bench 2026-11-06 (fineoffset-wh52:008f19, -48 dBm):
            moisture and temperature equal the GW1100B's every packet; EC 96.5 vs
            the gateway's 90 uS/cm; battery 2.92 vs 1.58 V. So: no battery voltage
            from the WH52 (byte 15 is not the battery on this probe), and the raw
            counts are sent as moisture_ad and ec_raw so EC can be fitted against
            the gateway's own counts on the endpoint. oat_rf915 1.1.1.
     1.4.1  Build 5 hears the WH52 (soil moisture + temperature + EC), family 0xA2,
            from rtl_433's fineoffset_wh52.c layout: 24-byte frame, CRC-8 over 0..21
            and SUM-8 over 0..22. lib/oat_rf915 1.1.0 reads 24 bytes per frame (was
            18, so a WH52 arrived cut short and counted as a family we do not
            decode). Stream fineoffset-wh52:<id>; EC and battery volts are
            rtl_433's empirical fits. 2026-11-06: the GW1100B on iot-test reported
            a WH52 on soil channel 3 while 1.4.0 filed nothing. Not yet benched.
     1.4.0  Build 5 (bench): Ecowitt / Ambient Weather FSK sensors through a
            915 MHz CC1101 in packet mode (above, lib/oat_rf915). A second path
            beside the esp32-wh51 / esp32-fsk images that waited on the bench for
            the 915 module: the 2026-09-20 run (rtl_433_ESP, a 433-matched module
            at 915) found preambles, sync and the WH31B / WN34 families, and
            traced its ~3.5 % bit jitter to the out-of-band front end. Packet mode
            does not depend on that answer: the chip times the bits. Console: `families` (frames per sensor family, good and
            failed-check), `raw on|off` (every frame in hex with its level and the
            chip's frequency-offset estimate), `tune bw|drate|dev`, `regs`, `rssi`.
            First bench beside a GW1100B that reports the same sensors to
            iot-test (ecowitt-c3f954aa), so every value has a reference.
     1.3.1  No behaviour change. The receiver code (gap filter, bit-clock decoder,
            new-station and plausibility guards, CC1101 driver, pulses/regs/rssi
            and the bench tools) moved unchanged into lib/oat_rf433 so the LoRa
            field node runs the same decoder; this sketch now calls it.
     1.3.0  The cleanup after build 4's bench day (2026-09-26).
            (1) Build 1 (the bare receiver) moves onto build 4's decoder: gap
            filter, bit clock, vanished-pulse fill, merged-pulse split, new-station
            confirmation. 1.2.5's last-bit completion is dropped: it completed no
            frame on the bench and, filling a bit from the checksum, let the host
            sim pass more wrong frames. A superhet's slicer distorts a
            weak station the way the CC1101's did; the bare receiver beside
            gweather-2 heard 748-A in 1 minute of 10 while the CC1101 heard it in
            10. The 1.1.x width-only decoder (dpHigh) is gone.
            (2) Bench tools out of the shipped images: capture, win, glitch, spike,
            reg, bw, drate only with -DOAT_RX_BENCH (the *-bench envs). Shipped
            images keep stations, pulses, regs, rssi, radio.
            (3) Boot breadcrumbs + safe mode: each start records its stage in RTC
            memory; the next start prints how the last one ended and where; three
            crashes in a row inside a minute and the radio stays off (`radio on`).
            Build 4 reboot-looped after some bench flashes and no crash text was
            ever caught.
            (4) Plausibility per station: a temperature, humidity or rain-counter
            jump beyond what weather does between frames is filed only when the
            next frame agrees, so a frame whose errors cancel in the checksum
            cannot file a false reading under a known station.
            (5) The bare receiver's empty-table help no longer claims it hears
            "tens of metres" (withdrawn 2026-09-25: that was a software issue).
     1.2.5  1.2.4 on the bench, 30 min: 748-A 27/21/18 (bare receiver 7/2/5) but
            323-A 12/8/5 (bare 29/29/29), and one phantom: 323-A's frame with two
            cancelling bit errors filed 7 rows as "1347-A". (1) A new station id
            must be heard twice, 5 s - 5 min apart, before it is filed. (2) A
            pulse spanning two or three beats (the gap after a long pulse shrank
            under the bridge) is split: 1s, then the part that sticks out. (3) A
            frame one bit short when the station falls silent takes the last bit
            the checksum's other seven bits agree with. (4) `capture`: raw pulses
            of lost frames and a good one, to replay real failures off the board.
     1.2.4  1.2.3 on the bench: 748-A fine, 323-A heard once in 10 minutes. 323-A's
            frames are mostly zeros (no wind, 7 rain tips): long runs of short
            pulses with long gaps, the case where the CC1101 shrinks shorts most,
            and some shrank to nothing (frames lost two and three beats after the
            last bit). (1) A beat with no pulse is filled with a 0, up to three
            per frame; a wrong fill always fails the checksum. (2) Pulses from
            30 us reach the decoder (was 80); the bit clock sorts noise. (3) Beat
            628 us +/- 120 (lost bits had landed at +87..+111 past 620).
     1.2.3  Build 4 decodes on the bit clock (dpPulseAt, above the ISRs). 1.2.2 on
            the bench: 748-A near complete, 323-A temp/humidity 3 of 17 minutes;
            lost frames ran a bit short into the next repeat's sync, because short
            pulses shrunk under the 120 us spike filter were dropped with the
            spikes. Now a pulse inside a frame is judged by when it starts: on the
            ~620 us beat it is a bit whatever its width, early and just behind the
            last pulse it is joined to it, early elsewhere it is ignored. The bare
            receiver's decoder (dpHigh) is untouched.
     1.2.2  Build 4 tuned from `pulses` on the bench (2026-09-26). The CC1101's
            timing was right (peaks at 220 and 408 us); the losses were ours.
            (1) Gap bridge 130 -> 80 us: 130 joined two real pulses whenever the
            CC1101 shrank the ~220 us gap between bits. (2) A separate spike
            filter, 120 us: a lone pulse that short is dropped without ending the
            frame (at bridge 80, 80-105 us spikes were 11 of 16 frames lost).
            (3) Windows split at the midpoint of the two peaks: short 120-315,
            long 315-540. Live settings 11:15 that morning (glitch 80, the new
            windows) took 748-A from 0 frames to 8 in 8 minutes against the bare
            receiver beside it. Console: `spike`.
     1.2.1  Build 4 bench, first run 2026-09-26 (gweather-2): the CC1101 decoded
            323-A at -60 dBm, but the temperature/humidity message (0x38) far
            less often than wind/rain (0x31): 3 against 11 in 30 minutes, where
            the bare receiver beside it got 29 of each. `pulses` shows where the
            in-frame pulse widths land and why each lost frame was lost; `win`
            moves the timing windows live. Diagnostic; the decoder is unchanged.
     1.2.0  Build 4, the CC1101 with this sketch's own AcuRite decoder (above).
            Bench only until it has decoded an Iris. The bare-receiver decoder is
            now one function both ISRs call (the bare path's behaviour is the
            same, except that a pulse arriving after a complete but uncollected
            frame now restarts the sync hunt instead of being ignored until a
            10 ms quiet gap, which a noisy receiver may never give).
     1.1.1  The radio line on `stations`. 2026-09-20, from the first FSK bench
            run: the board reported "radio listening" with decoded=0 skipped=0
            and there was no way to tell a dead data pin from a wrong band from
            a sensor out of range. Those two counters only ever count messages
            the library already delivered. `stations` now also prints the
            library's own capture counters and the live RSSI against its
            threshold, so a silent bench says WHICH stage is silent. Diagnostic
            only: no change to the radio, the decoders or the push path.
     1.1.0  The FSK family. Built 2026-09-18, NOT yet benched.
            (1) FSK images: every CC1101 env has an -fsk twin built with
            -DOOK_MODULATION=false and a 915.00 default band; the band field, the
            empty-table help and the console say what THIS image decodes.
            (2) rtl_433_ESP pinned to commit 236c816 (2026-09-02), the tree the
            1.0.x images were built from; lib_deps tracked master before, and
            master had already moved once under the 0.6.1 label.
            (3) A station id that arrives as a string (the WH52 sends hex text) is
            used as it is; it was read as a number and every WH52 filed as :0.
            (4) battery_ok as a level: the FSK decoders report 0.0 to 1.0, not 0/1.
            A value strictly between is passed as the stream's battery percent
            and battery_low is set below 0.2; 0 and 1 keep the old state rule.
            (5) Map: conductivity -> soil_conductivity (uS/cm, WH52), ext_power ->
            mains (WH45). Bookkeeping keys the FSK decoders add (boost, ad_raw,
            raw_value, sensitivity, rain_start, firmware, extra) are identity,
            not readings.
            (6) Station table 24 -> 32 rows, the core's slot count.
     1.0.6  Bench 2026-09-07 (Iris on the bare receiver, 1.0.5: parity held, six
            frames in 203 s, HTTP 200). Four small things from that run:
            (1) Repeats dropped. The 5-in-1 sends every message three times back
            to back; each was folded, so samples and frames read three per
            transmission. A frame identical to the previous one from the same
            station within two seconds is now counted as a repeat and skipped.
            Same rule on the rtl_433 path (identical decoded values).
            (2) Allow-list is an exact match on ids, comma-separated; it was a
            substring test, so acurite-5n1:271 admitted 2716 and 2719.
            (3) The console `status` text carries frames, bad checksum, bad
            parity and repeats for the bare-receiver path; they were web-only.
            (4) No signal level is passed to the core as oat::NO_RSSI; -1 went
            out on the line output as rssi=-1 and in the batch as rssi:-1.
            Core 1.2.2.
     1.0.5  (1) Station table recycles: a new station arriving when every row is
            taken now takes over the row that has been silent longest (its stream
            was already released), instead of being dropped without a word. The
            9/6 fix released the core slot but the row itself was kept for ever, so
            24 stations ever heard meant the 25th was never filed. (2) The bare-
            receiver path now checks parity as rtl_433's acurite_txr_check does:
            bytes 2..6 even parity, channel bits 01 rejected. The byte sum alone
            lets one corrupted frame in 256 through as a wild reading; parity is
            what rtl_433 uses to stop that. Bad-parity frames are counted on the
            setup page beside bad checksums.
     1.0.4  Text only, no behaviour change. The band field, the console message
            and this header no longer say 915.00 is "for Ecowitt / Ambient": that
            family is FSK and the OOK images cannot hear it (found 2026-09-07 in
            the rtl_433_ESP decoder table; the site pages were corrected the same
            day). The setup page now says what the band is and is not for.
     1.0.3  Core 1.2.1: a setting changed on the setup page now survives a reboot
            (the core saved before applying driver fields, so the web page was one
            save behind and a reboot reverted it). No behaviour change otherwise.
     1.0.2  (1) The bare-receiver path still stamped 0 dBm on every stream's link
            metadata (1.0.1 only dropped the rssi measurement); no level is now
            "unset", and the table shows a dash. (2) A station that goes quiet for
            ten minutes has its stream released and is marked silent, so a
            neighbour's sensor that drifted past, or a dead battery, no longer
            holds a slot for ever; it files straight back in when heard again.
     1.0.1  Bench 2026-09-05 (AcuRite Iris on a bare 433 receiver, GPIO 33): worked
            first time. A bare receiver has no signal level, so the data-pin path
            no longer reports rssi (it sent 0 dBm); link metadata is left unset.
     1.0.0  First release.

   LICENSE: this sketch is openly licensed; the bundled decoders are GPL-3.0 via
   rtl_433_ESP, so a built image is GPL-governed. Copy it, change it, share it.
   ============================================================================= */

#include <WiFi.h>   // cmdWifiOff only: the core owns Wi-Fi, this just kills the radio for a decode test
#include <oat_node_core.h>
#include <oat_measurands.h>
#include <ArduinoJson.h>
#if !defined(OAT_RX_DATAPIN) && !defined(OAT_RX_FSK915)
  #include <ArduinoLog.h>
  #include <rtl_433_ESP.h>
#endif

#define TIER        "oat-weather-listener"
#define FW_SEMVER   "1.4.4"
#define FW_VERSION  "OAT-Weather-Listener/1.4.4"
#define NVS_NS      "oatwx"
#ifndef OAT_BOARD_NAME
  #define OAT_BOARD_NAME "ESP32"
#endif
#ifndef RF_MODULE_FREQUENCY
  #define RF_MODULE_FREQUENCY 433.92
#endif

// What THIS image decodes, said once and used by the band field, the empty-table
// help and the console. The modulation is fixed at build time; the band is not.
#if defined(OAT_RX_FSK915)
  #define OAT_IMG_KIND  "CC1101 FSK packet mode, Ecowitt WH31 / WH51 / WN34 decoders"
  #define OAT_BAND_HELP "Your region's Ecowitt band: 915.00 in the Americas, Australia and New Zealand; 868.35 in Europe; 433.92 where the sensors are sold on 433. The CC1101 module and its antenna must be the kind for that band. This image decodes the Ecowitt / Ambient Weather WH31 thermo-hygrometers, WH51 soil moisture and WN34 / WN38 probes."
  #define OAT_EMPTY_HELP "Nothing heard yet. A WH31 sends about once a minute, a WH51 every 70 seconds (10 s for a while after its reading changes), a WN34 about every 50 seconds. If it stays empty: `families` on the USB console says whether frames are arriving at all and whether they fail their checks; `raw on` prints each one. Power down any OAT LoRa node on 915.0 nearby: it sits on the same channel and swamped the 2026-09-20 bench (`rssi` shows it as a steady high level). Check the band, a 915 MHz module, and an 8.2 cm antenna."
#elif defined(OAT_RX_DATAPIN) && defined(OAT_RX_CC1101)
  #define OAT_IMG_KIND  "CC1101 as the receiver, AcuRite 5-in-1 decoder"
  #define OAT_BAND_HELP "433.92 for the AcuRite 5-in-1 (Iris), the one station this image decodes. The CC1101 tunes 300 to 928 MHz, so the band can be nudged on the bench; the decoder does not change with it."
  #define OAT_EMPTY_HELP "Nothing heard yet. An AcuRite 5-in-1 transmits every 18 seconds, so give it a minute. If it stays empty: `stations` on the USB console shows the CC1101's state (it should say RX), its version (0x14) and the glitch count, which climbs while the data line carries anything at all. Check GDO0 is on the pin this image was built for, the module is a 433 MHz one, and it has a 17.3 cm antenna. This image decodes the AcuRite 5-in-1 only."
#elif defined(OAT_RX_DATAPIN)
  #define OAT_IMG_KIND  "bare 433 MHz receiver, AcuRite 5-in-1 decoder"
  #define OAT_BAND_HELP "Fixed at 433.92 by the receiver module. This image decodes the AcuRite 5-in-1 (Iris) and nothing else; a CC1101 image hears the rest."
  #define OAT_EMPTY_HELP "Nothing heard yet. An AcuRite 5-in-1 transmits every 18 seconds, so give it a minute. If it stays empty: give the module a straight 17.3 cm wire antenna, check DATA is on the pin this image was built for, and that the module runs from the ESP32's 3.3 V (a 5 V module drives 5 V into a 3.3 V pin). This image decodes the AcuRite 5-in-1 only."
#elif OOK_MODULATION
  #define OAT_IMG_KIND  "on-off keying (rtl_433 OOK decoders)"
  #define OAT_BAND_HELP "433.92 for AcuRite, La Crosse TX141, Oregon Scientific and the on-off-keying long tail, which is what this image decodes. The radio tunes 300 to 928 MHz and hears one band at a time. Ecowitt, Ambient Weather, La Crosse View and the Bresser 5-in-1 family are FSK: they need the FSK image."
  #define OAT_EMPTY_HELP "Nothing heard yet. A station transmits every 18 to 60 seconds, so give it a minute. If it stays empty: the band must match the station (433.92 for AcuRite, La Crosse TX141 and Oregon Scientific), the antenna must be cut for that band, and the station must be within range of a receiver that is far less sensitive than an SDR dongle. This is the on-off-keying image; Ecowitt, Ambient Weather, La Crosse View and Bresser 5-in-1 sensors are FSK and need the FSK image."
#else
  #define OAT_IMG_KIND  "FSK (rtl_433 FSK decoders)"
  #define OAT_BAND_HELP "Your region's band: 915.00 in the Americas, Australia and New Zealand; 868.35 in Europe; 433.92 where the sensors are sold on 433. This image decodes FSK sensors (Ecowitt and Ambient Weather, La Crosse View and IT+, the Bresser 5-in-1 family). It does not hear on-off-keying stations such as AcuRite; those need the on-off-keying image."
  #define OAT_EMPTY_HELP "Nothing heard yet. An Ecowitt soil or temperature probe transmits about once a minute, an array every 5 to 16 seconds. If it stays empty: the band must be your region's (915.00 Americas and Oceania, 868.35 Europe, 433.92 elsewhere), the CC1101 module and its antenna should be the 868/915 kind for those bands (8.2 cm wire at 915, 8.6 cm at 868), and the sensor must be in range. This is the FSK image; AcuRite, La Crosse TX141 and Oregon Scientific are on-off keying and need the other image."
#endif

#define JSON_MSG_BUFFER   1024
#define MAX_STATIONS      32
#define ID_LEN            40
#define STATION_GONE_MS   (10UL * 60UL * 1000UL)   // stations broadcast every 16-60 s; ten silent minutes is gone

#if !defined(OAT_RX_DATAPIN) && !defined(OAT_RX_FSK915)
static rtl_433_ESP rf;
#endif
static char msgBuf[JSON_MSG_BUFFER];
static float g_freq = RF_MODULE_FREQUENCY;
static String g_allow;                        // "" = every station heard
static uint32_t g_signals = 0, g_decoded = 0, g_skipped = 0;   // written only from the decoder task
static char g_unmapped[160] = {0};
static bool g_radioOk = false;

// ---------------------------------------------------------------------------
// Boot breadcrumbs (1.3.0). On 2026-09-26 Build 4 reboot-looped on the bench
// after some flashes and not others, and no crash text was ever caught. Each
// start now leaves its stage in memory a crash does not clear (RTC, no-init);
// the next start prints how the last one ended and where, with no serial
// monitor needed at the moment of the crash. Three crashes in a row, each
// before a minute of uptime, and the radio is left off (safe mode): the board
// stays up, says why, and `radio on` tries again.
// ---------------------------------------------------------------------------
#include <esp_system.h>
#define BOOT_MAGIC 0x0A7B0C51UL
RTC_NOINIT_ATTR static uint32_t rb_magic, rb_stage, rb_crashes, rb_lastStage;
static const char* const BOOT_STAGES[] = {
  "-", "setup entered", "driver starting", "radio configured", "receiving",
  "first loop pass", "first frame decoded", "-", "up a minute" };
static bool g_safeMode = false;
static inline void bootStage(uint32_t n) { if (n > rb_stage || n == 1) rb_stage = n; if (n == 8) rb_crashes = 0; }
static bool bootCrashed(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}
static void bootEnter() {
  esp_reset_reason_t r = esp_reset_reason();
  if (rb_magic != BOOT_MAGIC || r == ESP_RST_POWERON) { rb_magic = BOOT_MAGIC; rb_crashes = 0; rb_stage = 0; rb_lastStage = 0; }
  if (bootCrashed(r)) { rb_crashes++; rb_lastStage = rb_stage; }
  else rb_crashes = 0;
  bootStage(1);
}
static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on"; case ESP_RST_SW: return "restart"; case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT: return "CRASH (interrupt watchdog)"; case ESP_RST_TASK_WDT: return "CRASH (task watchdog)";
    case ESP_RST_WDT: return "CRASH (watchdog)"; case ESP_RST_BROWNOUT: return "BROWNOUT (supply dipped)"; case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "reset pin"; default: return "other";
  }
}
static void bootReport() {
  esp_reset_reason_t r = esp_reset_reason();
  if (bootCrashed(r)) {
    uint32_t st = rb_lastStage < 9 ? rb_lastStage : 0;
    Serial.printf("[wx] the last start ended in %s at stage %lu (%s); %lu in a row\n", resetName(r), (unsigned long)st, BOOT_STAGES[st], (unsigned long)rb_crashes);
  } else {
    Serial.printf("[wx] start after %s\n", resetName(r));
  }
  if (rb_crashes >= 3) {
    g_safeMode = true;
    Serial.println("[wx] SAFE MODE: the radio is left off after 3 crashes in a row. Copy the lines above; `radio on` tries again.");
  }
}

// ---------------------------------------------------------------------------
// The map from rtl_433's field names to the OAT vocabulary. rtl_433 reports the
// unit in the key (temperature_C, wind_avg_km_h, rain_in) so the conversion to
// the canonical unit is part of the row. Anything numeric that is not here and
// not identity is forwarded RAW under its own key, the BLE Listener's rule, and
// `status` lists it so it can be promoted.
// ---------------------------------------------------------------------------
enum Xf : uint8_t { X_NONE, X_F_TO_C, X_KMH_MS, X_MPH_MS, X_IN_MM, X_MV_V, X_INVERT, X_KNOT_MS };
struct Map { const char* key; const char* measurement; const char* unit; uint8_t kind; uint8_t xf; };
static const Map MAP[] = {
  { "temperature_C",   "temperature",        "Cel",   oat::KIND_CONTINUOUS, X_NONE },
  { "temperature_F",   "temperature",        "Cel",   oat::KIND_CONTINUOUS, X_F_TO_C },
  { "temperature_1_C", "temperature",        "Cel",   oat::KIND_CONTINUOUS, X_NONE },
  { "temperature_2_C", "temperature_2",      "Cel",   oat::KIND_CONTINUOUS, X_NONE },
  { "humidity",        "humidity",           "%RH",   oat::KIND_CONTINUOUS, X_NONE },
  { "wind_avg_m_s",    "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_NONE },
  { "wind_avg_km_h",   "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_KMH_MS },
  { "wind_avg_mi_h",   "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_MPH_MS },
  { "wind_speed_m_s",  "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_NONE },
  { "wind_speed_km_h", "wind_speed",         "m/s",   oat::KIND_CONTINUOUS, X_KMH_MS },
  { "wind_max_m_s",    "wind_gust",          "m/s",   oat::KIND_GAUGE,      X_NONE },
  { "wind_max_km_h",   "wind_gust",          "m/s",   oat::KIND_GAUGE,      X_KMH_MS },
  { "wind_max_mi_h",   "wind_gust",          "m/s",   oat::KIND_GAUGE,      X_MPH_MS },
  { "gust_speed_m_s",  "wind_gust",          "m/s",   oat::KIND_GAUGE,      X_NONE },
  { "wind_dir_deg",    "wind_direction",     "deg",   oat::KIND_GAUGE,      X_NONE },
  { "rain_mm",         "rain_total",         "mm",    oat::KIND_CUMULATIVE, X_NONE },
  { "rain_in",         "rain_total",         "mm",    oat::KIND_CUMULATIVE, X_IN_MM },
  { "rain_rate_mm_h",  "rain_rate",          "mm/h",  oat::KIND_CONTINUOUS, X_NONE },
  { "rain_rate_in_h",  "rain_rate",          "mm/h",  oat::KIND_CONTINUOUS, X_IN_MM },
  { "uv",              "uv_index",           "",      oat::KIND_CONTINUOUS, X_NONE },
  { "uvi",             "uv_index",           "",      oat::KIND_CONTINUOUS, X_NONE },
  { "light_lux",       "illuminance",        "lx",    oat::KIND_CONTINUOUS, X_NONE },
  { "lux",             "illuminance",        "lx",    oat::KIND_CONTINUOUS, X_NONE },
  { "radiation_W_m2",  "solar_radiation",    "W/m2",  oat::KIND_CONTINUOUS, X_NONE },
  { "moisture",        "soil_moisture",      "%",     oat::KIND_CONTINUOUS, X_NONE },
  { "conductivity",    "soil_conductivity",  "uS/cm", oat::KIND_CONTINUOUS, X_NONE },
  { "moisture_ad",     "moisture_ad",        "",      oat::KIND_CONTINUOUS, X_NONE },   // WH52 moisture count (= gateway soil_ec_hum_ad)
  { "leafwetness",     "leaf_wetness",       "%",     oat::KIND_CONTINUOUS, X_NONE },
  { "wetness",         "leaf_wetness",       "%",     oat::KIND_CONTINUOUS, X_NONE },
  { "strike_count",    "lightning_total",    "",      oat::KIND_CUMULATIVE, X_NONE },
  { "storm_dist_km",   "lightning_distance", "km",    oat::KIND_GAUGE,      X_NONE },
  { "storm_dist",      "lightning_distance", "km",    oat::KIND_GAUGE,      X_NONE },
  { "pressure_hPa",    "pressure",           "hPa",   oat::KIND_CONTINUOUS, X_NONE },
  { "pm2_5_ug_m3",     "pm25",               "ug/m3", oat::KIND_CONTINUOUS, X_NONE },
  { "pm10_ug_m3",      "pm10",               "ug/m3", oat::KIND_CONTINUOUS, X_NONE },
  { "co2_ppm",         "co2",                "ppm",   oat::KIND_CONTINUOUS, X_NONE },
  { "depth_cm",        "water_level",        "cm",    oat::KIND_CONTINUOUS, X_NONE },
  { "battery_ok",      "battery_low",        "",      oat::KIND_STATE,      X_INVERT },
  { "battery_mV",      "voltage",            "V",     oat::KIND_GAUGE,      X_MV_V },
  { "battery_V",       "voltage",            "V",     oat::KIND_GAUGE,      X_NONE },
  { "leak",            "water_leak",         "",      oat::KIND_STATE,      X_NONE },
  { "alarm",           "water_leak",         "",      oat::KIND_STATE,      X_NONE },
  { "ext_power",       "mains",              "",      oat::KIND_STATE,      X_NONE },
};
static const size_t MAP_N = sizeof(MAP) / sizeof(MAP[0]);

// Identity and radio bookkeeping rtl_433 emits: never a measurement.
static const char* const META[] = {
  "model", "id", "channel", "mic", "protocol", "rssi", "snr", "noise", "sequence_num", "message_type",
  "subtype", "duration", "pulses", "freq", "freq1", "freq2", "mod", "brand", "exception", "time",
  "battery", "battery_pct", "battery_level", "type", "flags", "raw_message", "data", "sensor_id",
  "boost", "ad_raw", "raw_value", "sensitivity", "rain_start", "firmware", "extra",
};
static bool isMeta(const char* k) { for (auto m : META) if (!strcmp(k, m)) return true; return false; }

static double xform(uint8_t xf, double v) {
  switch (xf) {
    case X_F_TO_C:  return (v - 32.0) * 5.0 / 9.0;
    case X_KMH_MS:  return v / 3.6;
    case X_MPH_MS:  return v * 0.44704;
    case X_KNOT_MS: return v * 0.514444;
    case X_IN_MM:   return v * 25.4;
    case X_MV_V:    return v / 1000.0;
    case X_INVERT:  return v ? 0.0 : 1.0;
    default:        return v;
  }
}

// Brand from the model's family prefix. rtl_433 models read "Acurite-5n1",
// "Fineoffset-WH65B", "LaCrosse-TX141THBv2", "Oregon-THGR122N" ...
static const char* brandFor(const String& model) {
  String p = model; int d = p.indexOf('-'); if (d > 0) p = p.substring(0, d);
  if (p.equalsIgnoreCase("Acurite"))    return "AcuRite";
  if (p.equalsIgnoreCase("Fineoffset")) return "Fine Offset (Ecowitt / Ambient)";
  if (p.equalsIgnoreCase("LaCrosse"))   return "La Crosse";
  if (p.equalsIgnoreCase("Oregon"))     return "Oregon Scientific";
  if (p.equalsIgnoreCase("Ambientweather")) return "Ambient Weather";
  if (p.equalsIgnoreCase("Ecowitt"))    return "Ecowitt";
  if (p.equalsIgnoreCase("Bresser"))    return "Bresser";
  if (p.equalsIgnoreCase("Davis"))      return "Davis";
  if (p.equalsIgnoreCase("TFA"))        return "TFA Dostmann";
  if (p.equalsIgnoreCase("Nexus") || p.equalsIgnoreCase("Prologue") || p.equalsIgnoreCase("Springfield")) return "generic";
  static char b[24]; strncpy(b, p.c_str(), sizeof(b) - 1); b[sizeof(b) - 1] = 0; return b;
}

// ---------------------------------------------------------------------------
// Stations heard — the display table. The core holds the readings; this holds
// who they came from.
// ---------------------------------------------------------------------------
struct Station { bool used; char sid[ID_LEN]; char model[32]; int rssi; bool haveRssi; bool gone; unsigned long lastMs; uint32_t frames; int slot; uint32_t lastHash; unsigned long lastHashMs; };
#define REPEAT_WINDOW_MS 2000UL   // a station's repeats are back to back; its next real message is 16 s or more away
static uint32_t g_repeats = 0;
static Station stations[MAX_STATIONS];
static SemaphoreHandle_t stMutex = nullptr;
static inline void stLock()   { if (stMutex) xSemaphoreTake(stMutex, portMAX_DELAY); }
static inline void stUnlock() { if (stMutex) xSemaphoreGive(stMutex); }

// Exact match on whole ids, comma-separated, case-insensitive.
static bool allowed(const char* sid) {
  if (!g_allow.length()) return true;
  int from = 0;
  while (from <= (int)g_allow.length()) {
    int to = g_allow.indexOf(',', from); if (to < 0) to = g_allow.length();
    String tok = g_allow.substring(from, to); tok.trim();
    if (tok.length() && tok.equalsIgnoreCase(sid)) return true;
    from = to + 1;
  }
  return false;
}
static void noteUnmapped(const char* key) {
  if (!key || !key[0] || strstr(g_unmapped, key)) return;
  size_t len = strlen(g_unmapped);
  if (len + strlen(key) + 2 >= sizeof(g_unmapped)) return;
  if (len) strcat(g_unmapped, ",");
  strcat(g_unmapped, key);
}

static String slug(const String& s) {
  String o; o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) { char c = tolower(s[i]); o += (isalnum(c) ? c : '-'); }
  return o;
}

// One decoded signal. Runs in the decoder task: file it and get out.
static void onDecoded(char* message) {
  g_decoded++;
  bootStage(6);
  JsonDocument doc;
  if (deserializeJson(doc, message)) { g_skipped++; return; }
  JsonObject o = doc.as<JsonObject>();
  const char* model = o["model"] | "";
  if (!model[0]) { g_skipped++; return; }

  // stream id: model slug + id (+ channel where the model has one)
  String sid = slug(model) + ":";
  // Most decoders report a number; a few (the WH52) report the id as hex text.
  if (o["id"].is<const char*>()) sid += slug(String(o["id"].as<const char*>()));
  else if (!o["id"].isNull()) sid += String(o["id"].as<long>());
  else if (o["sensor_id"].is<const char*>()) sid += slug(String(o["sensor_id"].as<const char*>()));
  else if (!o["sensor_id"].isNull()) sid += String(o["sensor_id"].as<long>());
  else sid += "0";
  if (!o["channel"].isNull()) { sid += "-"; sid += String(o["channel"].as<const char*>() ? o["channel"].as<const char*>() : String(o["channel"].as<int>()).c_str()); }
  if (sid.length() >= ID_LEN) sid = sid.substring(0, ID_LEN - 1);
  if (!allowed(sid.c_str())) { g_skipped++; return; }
  bool haveRssi = !o["rssi"].isNull();
  int rssi = haveRssi ? o["rssi"].as<int>() : 0;

  stLock();
  int d = -1, freeIdx = -1;
  for (int i = 0; i < MAX_STATIONS; i++) {
    if (stations[i].used && strcmp(stations[i].sid, sid.c_str()) == 0) { d = i; break; }
    if (!stations[i].used && freeIdx < 0) freeIdx = i;
  }
  if (d < 0) d = freeIdx;
  if (d < 0) {                                          // every row taken: reuse the one silent longest
    unsigned long oldest = 0; int victim = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
      if (!stations[i].gone) continue;
      unsigned long age = millis() - stations[i].lastMs;
      if (victim < 0 || age > oldest) { oldest = age; victim = i; }
    }
    if (victim < 0) { stUnlock(); g_skipped++; return; }   // all 24 live and talking: genuinely full
    Serial.printf("[wx] table full: %s (silent %lu min) makes room for %s\n", stations[victim].sid, oldest / 60000UL, sid.c_str());
    stations[victim].used = false;                      // its slot was released when it went silent
    d = victim;
  }
  Station& st = stations[d];
  if (!st.used) { memset(&st, 0, sizeof(st)); st.used = true; strncpy(st.sid, sid.c_str(), ID_LEN - 1); st.slot = oatcore::slotFor(st.sid, st.sid); }
  else if (st.gone || st.slot < 0) { st.slot = oatcore::slotFor(st.sid, st.sid); st.gone = false; }   // back after silence: re-claim
  strncpy(st.model, model, sizeof(st.model) - 1);
  st.rssi = rssi; st.haveRssi = haveRssi; st.lastMs = millis(); st.frames++;
  int slot = st.slot;
  stUnlock();
  if (slot < 0) return;

  // Repeat test: the same station saying the same thing within two seconds is
  // a repeat (the 5-in-1 sends each message three times), not a new reading.
  uint32_t h = 2166136261u;
  for (JsonPair kv : o) {
    const char* k = kv.key().c_str();
    if (isMeta(k)) continue;
    for (const char* c = k; *c; c++) { h ^= (uint8_t)*c; h *= 16777619u; }
    double dv = kv.value().is<double>() || kv.value().is<long>() ? kv.value().as<double>() : (kv.value().is<bool>() ? (kv.value().as<bool>() ? 1.0 : 0.0) : 0.0);
    uint64_t bits; memcpy(&bits, &dv, sizeof(bits));
    for (int i = 0; i < 8; i++) { h ^= (uint8_t)(bits >> (8 * i)); h *= 16777619u; }
  }
  stLock();
  bool repeat = (stations[d].lastHash == h) && (millis() - stations[d].lastHashMs < REPEAT_WINDOW_MS);
  if (repeat) { stations[d].frames--; g_repeats++; }
  else { stations[d].lastHash = h; stations[d].lastHashMs = millis(); }
  stUnlock();
  if (repeat) return;

  oatcore::slotMeta(slot, brandFor(model), model);
  int battPct = -1;
  if (!o["battery"].isNull() && o["battery"].is<int>()) battPct = o["battery"].as<int>();
  if (!o["battery_pct"].isNull()) battPct = o["battery_pct"].as<int>();
  // The FSK decoders report battery_ok as a level, 0.0 to 1.0. Strictly between the
  // ends it is a level (percent on the link, low below 0.2); 0 and 1 stay a state.
  bool battLevel = false; double battFrac = 0.0;
  if (o["battery_ok"].is<double>()) {
    battFrac = o["battery_ok"].as<double>();
    if (battFrac > 0.0 && battFrac < 1.0) { battLevel = true; if (battPct < 0) battPct = (int)(battFrac * 100.0 + 0.5); }
  }
  oatcore::slotLink(slot, haveRssi ? rssi : oat::NO_RSSI, battPct);   // no level on a bare receiver

  for (JsonPair kv : o) {
    const char* k = kv.key().c_str();
    if (isMeta(k)) continue;
    if (battLevel && !strcmp(k, "battery_ok")) { oatcore::fold(slot, "battery_low", "", oat::KIND_STATE, battFrac < 0.2 ? 1.0 : 0.0); continue; }
    JsonVariant v = kv.value();
    double val;
    if (v.is<bool>()) val = v.as<bool>() ? 1.0 : 0.0;
    else if (v.is<double>() || v.is<long>()) val = v.as<double>();
    else if (v.is<const char*>()) {                 // "OK"/"LOW" style strings for battery on some decoders
      const char* s = v.as<const char*>();
      if (!strcmp(k, "battery_ok") || !strcmp(k, "battery")) { oatcore::fold(slot, "battery_low", "", oat::KIND_STATE, strcasecmp(s, "LOW") == 0 ? 1.0 : 0.0); }
      continue;
    } else continue;
    bool mapped = false;
    for (size_t i = 0; i < MAP_N; i++) {
      if (strcmp(k, MAP[i].key) == 0) { oatcore::fold(slot, MAP[i].measurement, MAP[i].unit, MAP[i].kind, xform(MAP[i].xf, val)); mapped = true; break; }
    }
    if (!mapped) { oatcore::fold(slot, k, "", oat::KIND_CONTINUOUS, val); noteUnmapped(k); }
  }
  if (haveRssi) oatcore::fold(slot, "rssi", "dBm", oat::KIND_GAUGE, (double)rssi);
}

#ifdef OAT_RX_DATAPIN
// ---------------------------------------------------------------------------
// The data-pin receivers (build 1, a bare superhet; build 4, a CC1101) run the
// shared oat_rf433 library: gap filter, bit-clock AcuRite 5-in-1 decoder,
// new-station and plausibility guards, the CC1101 register set. It was this
// sketch's own code through 1.3.0 and moved out unchanged so the LoRa field node
// runs the same decoder. Each checked reading is written as the JSON rtl_433
// would have produced and handed to the same map as every other path.
// ---------------------------------------------------------------------------
#include <oat_rf433.h>
#ifdef OAT_RX_CC1101
  #ifndef RF_MODULE_SCK
    #define RF_MODULE_SCK  18
  #endif
  #ifndef RF_MODULE_MISO
    #define RF_MODULE_MISO 19
  #endif
  #ifndef RF_MODULE_MOSI
    #define RF_MODULE_MOSI 23
  #endif
  #ifndef RF_MODULE_CS
    #define RF_MODULE_CS   5
  #endif
static SPIClass ccSpi(VSPI);
#endif

static void dpPoll() {
  oatrf::loop();
  oatrf::Reading r;
  while (oatrf::take(r)) {
    JsonDocument d;
    d["model"] = "Acurite-5n1";
    d["id"] = r.id;
    char ch[2] = { r.ch, 0 }; d["channel"] = ch;
    d["battery_ok"] = r.batteryOk ? 1 : 0;
    d["wind_avg_km_h"] = r.windKmh;
    if (r.hasDir)  d["wind_dir_deg"] = r.dirDeg;
    if (r.hasRain) d["rain_in"] = r.rainIn;
    if (r.hasTH)   { d["temperature_F"] = r.tempF; d["humidity"] = r.hum; }
    if (r.hasRssi) d["rssi"] = r.rssi;                  // no "rssi" on a bare receiver: it has no signal level
    serializeJson(d, msgBuf, JSON_MSG_BUFFER);
    onDecoded(msgBuf);
  }
}
#endif

#ifdef OAT_RX_FSK915
// ---------------------------------------------------------------------------
// Build 5: the CC1101 in packet mode (lib/oat_rf915). Each checked frame is
// written as the JSON rtl_433 would have produced and handed to the same map.
// ---------------------------------------------------------------------------
#include <oat_rf915.h>
#ifndef RF_MODULE_SCK
  #define RF_MODULE_SCK  18
#endif
#ifndef RF_MODULE_MISO
  #define RF_MODULE_MISO 19
#endif
#ifndef RF_MODULE_MOSI
  #define RF_MODULE_MOSI 23
#endif
#ifndef RF_MODULE_CS
  #define RF_MODULE_CS   5
#endif
static SPIClass ccSpi(VSPI);

static void fskPoll() {
  oatrf915::loop();
  oatrf915::Reading r;
  while (oatrf915::take(r)) {
    JsonDocument d;
    d["model"] = r.model;
    if (r.idIsText) d["id"] = r.id; else d["id"] = atol(r.id);
    if (r.channel >= 0) d["channel"] = r.channel;
    for (int i = 0; i < r.n; i++) d[r.val[i].key] = r.val[i].v;
    d["rssi"] = r.rssi;
    serializeJson(d, msgBuf, JSON_MSG_BUFFER);
    onDecoded(msgBuf);
  }
}
#endif

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------
static void radioStart() {
#if defined(OAT_RX_FSK915)
  oatrf915::Pins p; p.sck = RF_MODULE_SCK; p.miso = RF_MODULE_MISO; p.mosi = RF_MODULE_MOSI; p.cs = RF_MODULE_CS;
  g_radioOk = oatrf915::begin(p, &ccSpi, g_freq);
  Serial.printf("[wx] %s: CC1101 %s (version 0x%02x) at %.2f MHz, 2-FSK packet mode, bw %.0f kHz, %.2f kBd, dev %.1f kHz, Ecowitt WH31/WH51/WN34 decoders\n",
                OAT_BOARD_NAME, g_radioOk ? "receiving" : "NOT RECEIVING (check SPI wiring, 3V3, and that it is a CC1101)", oatrf915::version(), g_freq, oatrf915::bwKHz(), oatrf915::drateK(), oatrf915::devKHz());
  bootStage(3);
  if (g_radioOk) bootStage(4);
#elif defined(OAT_RX_DATAPIN)
  oatrf::Pins p; p.data = OAT_RX_DATAPIN;
#ifdef OAT_RX_CC1101
  p.sck = RF_MODULE_SCK; p.miso = RF_MODULE_MISO; p.mosi = RF_MODULE_MOSI; p.cs = RF_MODULE_CS;
  g_radioOk = oatrf::begin(oatrf::FRONT_CC1101, p, &ccSpi, g_freq, "wx");
  Serial.printf("[wx] %s: CC1101 %s (version 0x%02x) at %.2f MHz, bw %.0f kHz, data on GDO0 -> GPIO %d, AcuRite 5-in-1 decoder\n",
                OAT_BOARD_NAME, g_radioOk ? "receiving" : "NOT RECEIVING (check SPI wiring, 3V3, and that it is a CC1101)", oatrf::ccVersion(), g_freq, oatrf::ccBwKHz(), OAT_RX_DATAPIN);
#else
  g_freq = 433.92f;
  g_radioOk = oatrf::begin(oatrf::FRONT_BARE, p, nullptr, g_freq, "wx");
  Serial.printf("[wx] %s: bare 433 MHz receiver on GPIO %d, AcuRite 5-in-1 decoder\n", OAT_BOARD_NAME, OAT_RX_DATAPIN);
#endif
  bootStage(3);
  if (g_radioOk) bootStage(4);
#else
  static bool logOn = false;
  if (!logOn) { Log.begin(LOG_LEVEL, &Serial); Log.setShowLevel(false); logOn = true; }
  // begin() reports failure instead of halting the board, so a missing or
  // mis-wired module leaves the setup page and console alive to say so.
  g_radioOk = rf.begin(RF_MODULE_RECEIVER_GPIO, g_freq);
  bootStage(3);
  if (g_radioOk) { rf.setCallback(onDecoded, msgBuf, JSON_MSG_BUFFER); rf.enableReceiver(); bootStage(4); }
  Serial.printf("[wx] %s: radio %s at %.2f MHz (%s)\n", OAT_BOARD_NAME, g_radioOk ? "listening" : "NOT FOUND (check the module's SPI wiring and 3V3)", g_freq, OOK_MODULATION ? "OOK" : "FSK");
#endif
}
// A band change re-initialises the receiver: the library owns the radio's task,
// queue and buffers and offers end()/begin() as the only clean way to retune.
static void radioRetune() {
#if defined(OAT_RX_FSK915)
  if (!g_radioOk) { radioStart(); return; }
  oatrf915::retune(g_freq);
  return;
#elif defined(OAT_RX_DATAPIN) && defined(OAT_RX_CC1101)
  if (!g_radioOk) { radioStart(); return; }
  oatrf::retune(g_freq);
  return;
#elif defined(OAT_RX_DATAPIN)
  return;                                               // the receiver module fixes the band
#else
  if (g_radioOk) { rf.disableReceiver(); rf.end(); g_radioOk = false; }
  radioStart();
#endif
}

// ---------------------------------------------------------------------------
// Driver hooks
// ---------------------------------------------------------------------------
static String getBand()  { return String(g_freq, 2); }
static bool   setBand(const String& v, String& why) {
#if defined(OAT_RX_DATAPIN) && !defined(OAT_RX_CC1101)
  why = "this image drives a bare 433 MHz receiver module; its band is fixed by the module"; return false;
#endif
  float f = v.toFloat();
  if (!(f >= 300 && f <= 928)) { why = "band must be a frequency in MHz, 300 to 928. " OAT_BAND_HELP; return false; }
  bool changed = fabs(f - g_freq) > 0.001f; g_freq = f;
  if (changed && g_radioOk) radioRetune();
  return true;
}
static String getAllow() { return g_allow; }
static bool   setAllow(const String& v, String& why) { g_allow = v; return true; }

static const oatcore::Field FIELDS[] = {
  { "band", "Band (MHz)", OAT_BAND_HELP, getBand, setBand },
  { "stations", "Station allow-list (optional)", "Comma-separated stream ids, e.g. acurite-5n1:2716. Empty means every station heard is reported, which on a shared band includes the neighbours'.", getAllow, setAllow },
};

static void wxBegin() {
  stMutex = xSemaphoreCreateMutex();
  bootStage(2);
  bootReport();
  if (!g_safeMode) radioStart();
}
static void wxSample()  { }
// A station silent for STATION_GONE_MS has its stream released and is shown as
// silent, the LoRa Gateway's rule; the row stays so it can file straight back in.
static void sweepSilent() {
  static unsigned long last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  stLock();
  for (int i = 0; i < MAX_STATIONS; i++) {
    Station& st = stations[i];
    if (!st.used || st.gone || millis() - st.lastMs < STATION_GONE_MS) continue;
    if (st.slot >= 0) oatcore::release(st.slot);
    st.slot = -1; st.gone = true;
    Serial.printf("[wx] %s silent for %lu min: released\n", st.sid, STATION_GONE_MS / 60000UL);
  }
  stUnlock();
}
static void wxCollect() {
  bootStage(5);
  if (millis() > 60000UL) bootStage(8);
  if (!g_radioOk) { sweepSilent(); return; }
#if defined(OAT_RX_FSK915)
  fskPoll();
#elif defined(OAT_RX_DATAPIN)
  dpPoll();
#else
  rf.loop();
#endif
  sweepSilent();
}
static void wxRescan()  { if (g_radioOk) radioRetune(); }

static String statusHtml() {
#if defined(OAT_RX_FSK915)
  oatrf915::Stats fs = oatrf915::stats();
  String p = "<div class='muted'>CC1101 " + String(g_radioOk ? "receiving" : "NOT RECEIVING") + " at " + String(g_freq, 2) + " MHz, 2-FSK packet mode &middot; Ecowitt WH31 / WH51 / WN34 decoders &middot; frames " + String(fs.frames) + ", decoded " + String(fs.good) + ", other " + String(fs.noise) + " &middot; " + String(OAT_BOARD_NAME) + "</div>";
#elif defined(OAT_RX_DATAPIN)
#ifdef OAT_RX_CC1101
  String p = "<div class='muted'>CC1101 " + String(g_radioOk ? "receiving" : "NOT RECEIVING") + " at " + String(g_freq, 2) + " MHz, bw " + String(oatrf::ccBwKHz(), 0) + " kHz, data on GPIO " + String(OAT_RX_DATAPIN);
#else
  String p = "<div class='muted'>Bare 433 MHz receiver on GPIO " + String(OAT_RX_DATAPIN);
#endif
  oatrf::Stats rs = oatrf::stats();
  p += " &middot; AcuRite 5-in-1 decoder &middot; frames " + String(rs.frames) + ", bad checksum " + String(rs.badcrc) + ", bad parity " + String(rs.badparity) + ", repeats " + String(g_repeats) + ", lost mid-frame " + String(rs.lostMid) + " &middot; " + String(OAT_BOARD_NAME) + "</div>";
#else
  String p = "<div class='muted'>Radio " + String(g_radioOk ? "listening" : "NOT STARTED") + " &middot; " + String(g_freq, 2) + " MHz &middot; " + String(OOK_MODULATION ? "OOK" : "FSK") + " &middot; " + String(OAT_BOARD_NAME) + "</div>";
#endif
  if (g_safeMode) p += "<p class='bad'>SAFE MODE: the radio was left off after 3 crashes in a row. The USB console says where the last start stopped; `radio on` there tries again.</p>";
  p += "<table class='tbl'><tr><th>Station</th><th>Model</th><th>Signal</th><th>Last heard</th><th>Frames</th></tr>";
  int shown = 0;
  stLock();
  for (int i = 0; i < MAX_STATIONS; i++) {
    Station& s = stations[i]; if (!s.used) continue; shown++;
    p += "<tr><td>" + String(s.sid) + (s.gone ? " <span class='bad'>silent</span>" : "") + "</td><td>" + String(s.model) + "</td><td>" + (s.haveRssi ? String(s.rssi) + " dBm" : String("&mdash;")) + "</td><td>" + String((millis() - s.lastMs) / 1000) + "s ago</td><td>" + String(s.frames) + "</td></tr>";
  }
  stUnlock();
  p += "</table>";
  if (!shown) p += "<p class='bad'>" OAT_EMPTY_HELP "</p>";
  if (g_unmapped[0]) p += "<div class='muted'>Values arriving under keys the vocabulary does not map yet, forwarded raw: <code>" + String(g_unmapped) + "</code>. Nothing is lost; these can be promoted to named measurands.</div>";
  p += "<div class='muted'>Signals decoded " + String(g_decoded) + " &middot; skipped " + String(g_skipped) + (g_allow.length() ? " &middot; allow-list on" : " &middot; every station reported") + "</div>";
  return p;
}
static String statusText() {
  String s = "wx radio=" + String(g_radioOk ? "listening" : "off") + " freq=" + String(g_freq, 2) + " decoded=" + String(g_decoded) + " skipped=" + String(g_skipped) + " repeats=" + String(g_repeats) + "\n";
#if defined(OAT_RX_FSK915)
  {
    oatrf915::Stats fs = oatrf915::stats();
    s += "wx cc1101 version=0x" + String(oatrf915::version(), HEX) + " state=0x" + String(oatrf915::state(), HEX) + " (0xd = RX)"
       + " bw=" + String(oatrf915::bwKHz(), 1) + "kHz drate=" + String(oatrf915::drateK(), 2) + "kBd dev=" + String(oatrf915::devKHz(), 1) + "kHz rssi_now=" + String(oatrf915::rssiNow()) + "dBm\n";
    s += "wx rf915 frames=" + String(fs.frames) + " decoded=" + String(fs.good) + " other=" + String(fs.noise) + " overflows=" + String(fs.overflows) + " restarts=" + String(fs.restarts) + "\n";
  }
#elif defined(OAT_RX_DATAPIN)
#ifdef OAT_RX_CC1101
  s += "wx cc1101 version=0x" + String(oatrf::ccVersion(), HEX) + " state=" + String(g_radioOk ? (oatrf::ccReceiving() ? "RX" : "0x" + String(oatrf::ccState(), HEX)) : "not-found")
     + " bw=" + String(oatrf::ccBwKHz(), 1) + "kHz drate=" + String(oatrf::ccDrateK(), 2) + "kBd rssi_now=" + String(oatrf::ccRssiNow()) + "dBm restarts=" + String(oatrf::ccRestarts()) + "\n";
#endif
  {
    oatrf::Stats rs = oatrf::stats();
    s += "wx receiver gpio=" + String(OAT_RX_DATAPIN) + " frames=" + String(rs.frames) + " bad_checksum=" + String(rs.badcrc) + " bad_parity=" + String(rs.badparity) + " lost_mid_frame=" + String(rs.lostMid) + " glitches=" + String(rs.glitches) + "\n";
  }
#else
  /* The radio line answers "is anything reaching the decoder at all?", which
     decoded/skipped cannot: both of those count messages the library already
     handed us, so a silent bench reads 0/0 whether the sensor is out of range,
     the band is wrong, or the data pin is not connected at all.

     captured  = bursts the ISR recorded (RSSI crossed the threshold AND edges
                 arrived on the data pin). Zero with a sensor beside the board
                 means nothing is getting in: data pin, band, antenna, range.
                 Note SPI proves none of that — RSSI is read over SPI, while the
                 demodulated bits arrive on a separate wire the SPI bus never
                 touches, so "radio listening" can be true with that wire cut.
     zero_dec  = bursts captured but decoded by nothing: signal is arriving and
                 the modulation, bit rate or deviation does not match it.
     dropped_* = the decoder could not keep up (noise storm or a busy loop). */
  {
    rtl_433_ESPStatus rs = rtl_433_ESP::statusSnapshot();
    s += "wx rf captured=" + String(rs.capturedSignals) + " decoded=" + String(rs.decodedSignals)
       + " zero_dec=" + String(rs.zeroDecodedSignals) + " msgs=" + String(rs.decodedMessages)
       + " dropped_buf=" + String(rs.droppedCaptureBuffers) + " dropped_q=" + String(rs.droppedDecoderQueue)
       + " rssi=" + String(rs.currentRssi) + " avg=" + String(rs.averageRssi)
       + " threshold=" + String(rs.rssiThreshold) + " rx=" + String(rs.receiverEnabled ? "on" : "off") + "\n";
  }
#endif
  if (g_safeMode) s += "wx SAFE MODE: radio left off after 3 crashes in a row; `radio on` tries again\n";
  stLock();
  for (int i = 0; i < MAX_STATIONS; i++) {
    Station& st = stations[i]; if (!st.used) continue;
    s += "station " + String(st.sid) + (st.gone ? " SILENT" : "") + " " + String(st.model) + " rssi=" + (st.haveRssi ? String(st.rssi) : String("-")) + " last=" + String((millis() - st.lastMs) / 1000) + "s frames=" + String(st.frames) + "\n";
  }
  stUnlock();
  if (g_unmapped[0]) s += "unmapped keys (forwarded raw): " + String(g_unmapped) + "\n";
  return s;
}
static String diagLine() {
  int n = 0; stLock(); for (int i = 0; i < MAX_STATIONS; i++) if (stations[i].used && !stations[i].gone) n++; stUnlock();
  return "decoded " + String(g_decoded) + " signal(s) from " + String(n) + " station(s) at " + String(g_freq, 2) + " MHz, image: " OAT_IMG_KIND ". If decoded is zero, the image must match the sensor's modulation, then the band, the antenna length and the range are the questions.";
}

#ifdef CC1101_OOK_TUNING
/* Live radio tuning (bench images only). The library exposes register access under
   CC1101_OOK_TUNING; without it every profile guess costs a rebuild, a reflash and a
   re-pair, which is how an afternoon disappears. Writing a register strobes the chip
   to IDLE, so each command re-enters receive afterwards.
   Addresses: AGCCTRL2 0x1b · AGCCTRL1 0x1c · AGCCTRL0 0x1d · MDMCFG4 0x10 (bandwidth
   + data-rate exponent) · MDMCFG2 0x12 · FREND1 0x21 · TEST2 0x2c · TEST1 0x2d. */
static void cmdReg(const String& a) {
  String t = a; t.trim();
  int sp = t.indexOf(' ');
  if (sp < 0) { Serial.println("usage: reg <addr-hex> <value-hex>   e.g. reg 1b 07"); return; }
  uint8_t addr = (uint8_t) strtol(t.substring(0, sp).c_str(), nullptr, 16);
  uint8_t val  = (uint8_t) strtol(t.substring(sp + 1).c_str(), nullptr, 16);
  rf.setCC1101Register(addr, val);
  rf.receiveDirect();
  Serial.printf("reg 0x%02x <- 0x%02x  (readback 0x%02x)\n", addr, val, rf.getCC1101Register(addr));
}
static void cmdBw(const String& a) {
  float k = a.toFloat();
  if (!(k >= 58.0f && k <= 812.5f)) { Serial.println("usage: bw <kHz>  58 to 812.5 (58, 81, 101, 116, 135, 162, 203, 232, 270, 325, 406, 464, 541, 650, 812)"); return; }
  int16_t st = rf.setRxBandwidth(k);
  rf.receiveDirect();
  Serial.printf("rx bandwidth <- %.3f kHz (state %d)\n", k, st);
}
static void cmdRegs(const String&) { rtl_433_ESP::getModuleStatus(); }
/* The CC1101's data rate, live. rtl_433_ESP fixes it at 17.24 kbps = 58.1 us/bit,
   which suits Fine Offset's 58 us devices (WH51). The WH31E/WH31B family clocks at
   56 us (17.857 kbps), and on the bench the slicer's measured bit width tracked OUR
   58 us rather than the sender's 56 — i.e. the radio is imposing its own grid, so a
   56 us transmitter slips about one bit in 28. That matches the observed error rate
   and is why the WH51 gets further than the thermometer.
     bitrate 17.857  -> 56.0 us/bit, the WH31E/WH31B family
     bitrate 17.24   -> 58.1 us/bit, the library default (WH51, WN34) */
static void cmdBitrate(const String& a) {
  float k = a.toFloat();
  if (!(k >= 0.6f && k <= 500.0f)) { Serial.println("usage: bitrate <kbps>  e.g. 17.857 for 56us bits, 17.24 for 58us"); return; }
  int16_t st = rf.setBitRate(k);
  rf.receiveDirect();
  Serial.printf("bit rate <- %.3f kbps = %.2f us/bit (state %d)\n", k, 1000.0f / k, st);
}
/* Shut the Wi-Fi radio down, live, to test whether packet corruption is interrupt
   starvation rather than a radio problem. A Fine Offset FSK bit is 56-58 us, so the
   capture ISR must service up to ~17,000 edges a second; the Wi-Fi stack runs at a
   higher priority and can stall a GPIO interrupt long enough to merge or drop edges.
   Symptom that points here: a strong clean signal (-38 dBm, symbols landing on 58 us)
   whose packet still fails CRC. Reboot restores Wi-Fi; the endpoint push stops while
   it is off, which does not matter for a decode test. */
static void cmdWifiOff(const String&) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("wifi OFF - decode test only, reboot to restore (no push while off)");
}
/* The RSSI gate. On CC1101 the capture ISR only records an edge while currentRssi is
   above the threshold — but in OOK the carrier is ABSENT for every "0" bit, so RSSI
   collapses to the noise floor mid-packet. currentRssi is sampled by a background
   poll, not per edge, so whenever that poll lands in a gap the gate shuts and the
   next transitions are thrown away: a pulse train that is mostly right, peppered with
   missing and merged edges, which is what the bench saw. The library knows: the
   SX127X arm of the same `if` skips the RSSI test entirely, commented "SX127X RSSI
   Value drops for a 0 value". A large negative delta holds the gate open.
   The background task recomputes threshold = average + delta every 50,000 samples,
   so the delta is what must change; the threshold is set here only to act at once. */
static void cmdThresh(const String& a) {
  String t = a; t.trim();
  if (!t.length()) { Serial.printf("rssi gate delta is %d dB; usage: thresh <dB>, e.g. thresh -40 to hold it open\n", rtl_433_ESP::rssiThresholdDelta); return; }
  int d = t.toInt();
  rtl_433_ESPStatus rs = rtl_433_ESP::statusSnapshot();
  rtl_433_ESP::rssiThresholdDelta = d;
  rtl_433_ESP::rssiThreshold = rs.averageRssi + d;
  Serial.printf("rssi gate delta <- %d dB, threshold now %d dBm (avg %d)\n", d, rtl_433_ESP::rssiThreshold, rs.averageRssi);
}
#endif

static void cmdRadio(const String& a) {
  String t = a; t.trim();
  if (t == "on") {
    if (g_radioOk) { Serial.println("radio is already on"); return; }
    g_safeMode = false; rb_crashes = 0; radioStart(); return;
  }
  Serial.printf("radio %s%s; last start stopped at stage %lu (%s), %lu crash(es) in a row. `radio on` starts it.\n",
                g_radioOk ? "on" : "off", g_safeMode ? " (SAFE MODE)" : "", (unsigned long)rb_lastStage, BOOT_STAGES[rb_lastStage < 9 ? rb_lastStage : 0], (unsigned long)rb_crashes);
}
#ifdef OAT_RX_DATAPIN
static void cmdPulses(const String& a) { oatrf::printPulses(a, Serial); }
#endif
#ifdef OAT_RX_CC1101
static void cmdCcRegs(const String& a) { oatrf::printRegs(a, Serial); }
static void cmdRssi(const String& a)   { oatrf::sampleRssi(a, Serial); }
#endif
#if defined(OAT_RX_DATAPIN) && defined(OAT_RX_BENCH)
static void cmdGlitch(const String& a)  { oatrf::benchGlitch(a, Serial); }
static void cmdSpike(const String& a)   { oatrf::benchSpike(a, Serial); }
static void cmdWin(const String& a)     { oatrf::benchWin(a, Serial); }
static void cmdCapture(const String& a) { oatrf::benchCapture(a, Serial); }
#ifdef OAT_RX_CC1101
static void cmdCcReg(const String& a)   { oatrf::benchReg(a, Serial); }
static void cmdCcBw(const String& a)    { oatrf::benchBw(a, Serial); }
static void cmdCcDrate(const String& a) { oatrf::benchDrate(a, Serial); }
#endif
#endif

#ifdef OAT_RX_FSK915
static void cmdFamilies(const String&) { oatrf915::printFamilies(Serial); }
static void cmdRaw(const String& a) {
  String t = a; t.trim();
  if (t == "on" || t == "off") oatrf915::setRaw(t == "on", Serial);
  Serial.printf("raw frame log %s (raw on | raw off)\n", oatrf915::raw() ? "ON: every frame the chip syncs on is printed" : "off");
}
static void cmdFskRegs(const String&) { oatrf915::printRegs(Serial); }
static void cmdFskRssi(const String&) { oatrf915::sampleRssi(Serial); }
static void cmdTune(const String& a)  { oatrf915::tune(a, Serial); }
#endif

static void cmdStations(const String&) { Serial.print(statusText()); }
static const oatcore::Command COMMANDS[] = {
  { "stations", "list the weather stations and sensors heard, with signal and age", cmdStations },
  { "radio",  "radio state and how the last start ended; `radio on` leaves safe mode", cmdRadio },
#ifdef OAT_RX_FSK915
  { "families", "frames per Ecowitt sensor family: decoded and failed-check", cmdFamilies },
  { "raw",    "print every frame the CC1101 syncs on, in hex: raw on | raw off", cmdRaw },
  { "regs",   "dump the CC1101's configuration registers and state", cmdFskRegs },
  { "rssi",   "sample the band's signal level for 100 ms", cmdFskRssi },
  { "tune",   "set the receiver live, not saved: tune bw 270 | tune drate 17.24 | tune dev 40", cmdTune },
#endif
#ifdef OAT_RX_DATAPIN
  { "pulses", "histogram of in-frame pulse widths + the last frames lost and why (pulses clear)", cmdPulses },
#endif
#ifdef OAT_RX_CC1101
  { "regs",   "dump the CC1101's configuration registers and state", cmdCcRegs },
  { "rssi",   "sample the band's signal level for 100 ms", cmdRssi },
#endif
#if defined(OAT_RX_DATAPIN) && defined(OAT_RX_BENCH)
  { "glitch", "bench: show or set the gap bridge in us: glitch 80 (0 = off)", cmdGlitch },
  { "spike",  "bench: show or set the spike filter in us: spike 120 (0 = off)", cmdSpike },
  { "win",    "bench: show or set the 0/1 split and sync window: win long 315 540", cmdWin },
  { "capture","bench: arm a raw capture of the next lost frames + one good one; `capture show` prints it", cmdCapture },
#ifdef OAT_RX_CC1101
  { "reg",    "bench: write a CC1101 register live: reg <addr-hex> <value-hex>", cmdCcReg },
  { "bw",     "bench: set the receive bandwidth live: bw 203", cmdCcBw },
  { "drate",  "bench: set the data rate live in kBaud: drate 5", cmdCcDrate },
#endif
#endif
#ifdef CC1101_OOK_TUNING
  { "reg",  "write a CC1101 register live: reg <addr-hex> <value-hex>", cmdReg },
  { "bw",   "set RX bandwidth in kHz live: bw 203", cmdBw },
  { "regs", "dump the CC1101 registers now", cmdRegs },
  { "bitrate", "set the CC1101 data rate live in kbps: bitrate 17.857", cmdBitrate },
  { "wifioff", "shut the WiFi radio down to test ISR starvation", cmdWifiOff },
  { "thresh", "set the RSSI gate delta in dB: thresh -40 holds the gate open", cmdThresh },
#endif
};

static const oatcore::Driver DRIVER = {
  TIER, "Stations heard", FW_VERSION, FW_SEMVER, NVS_NS,
  wxBegin, wxSample, wxCollect, nullptr, wxRescan,
  statusHtml, statusText, nullptr, diagLine,
  FIELDS,   (int)(sizeof(FIELDS)   / sizeof(FIELDS[0])),
  COMMANDS, (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])),
};

void setup() { bootEnter(); oatcore::begin(DRIVER); }
void loop()  { oatcore::loop(); }
