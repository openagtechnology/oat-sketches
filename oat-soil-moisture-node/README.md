<p align="center">
  <img src="../docs/img/oat-logo.png" alt="Open Agriculture Technology" width="300">
</p>

<p align="center"><em><a href="https://openagriculturetechnology.com/">Open Agriculture Technology</a> is an open collective around one idea:<br>
appropriate technology for growing things — the right tool for the need, the budget, and the environment.</em></p>

<h1 align="center">OAT Soil-Moisture Node</h1>

<p align="center"><strong>A few-dollar capacitive probe, calibrated on the probe itself, reporting to a place you own.</strong><br>
One sketch in the <a href="../">OAT Sketch Library</a> — they all share one setup flow and push the same open
<a href="https://openagriculturetechnology.com/standard/">oat-ods</a> format to an endpoint you own.</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-flash_from_the_browser-6a994e" alt="Status: live, flash from the browser">
  <img src="https://img.shields.io/badge/interface-analog_·_capacitive_probe-0969da" alt="Interface: analog, capacitive soil probe">
  <img src="https://img.shields.io/badge/chips-ESP32_(bench--verified)-555" alt="Chips: classic ESP32, bench-verified">
  <img src="https://img.shields.io/badge/schema-oat--ods%2F0.3-6a994e" alt="Schema: oat-ods/0.3">
  <img src="https://img.shields.io/badge/license-Apache--2.0-blue" alt="License: Apache-2.0">
  <a href="https://openagriculturetechnology.com/build/sketches/soil-moisture-node/"><img src="https://img.shields.io/badge/site-sketch_page-333" alt="Site: sketch page"></a>
</p>

---

**Flash this sketch onto an ESP32 (the classic one) and the board becomes a
soil-moisture gateway.** The node hosts its own setup page: join its Wi-Fi and configure
everything in your browser — no app, no account, no code editing. It reads up to
six **capacitive soil-moisture probes** — three wires each — and pushes each
probe's output voltage, and a percentage once you have calibrated that probe,
as **oat-ods** to an endpoint you own, by webhook or MQTT. A live
[Test Endpoint](https://iot-test.openagriculturetechnology.com/) is ready to
catch your first reading ([Set it up](#set-it-up), step 4).

```mermaid
flowchart LR
  S1["Capacitive soil probe"] -- "three wires<br>AOUT → GPIO 35 · 3V3 · GND" --> GW["ESP32 Gateway<br>(this firmware)"]
  GW -- "oat-ods over<br>webhook or MQTT" --> E["YOUR endpoint<br>on your LAN or your cloud"]
```

<p align="center"><img src="https://openagriculturetechnology.com/assets/img/store/components/capacitive-soil.jpg" alt="A capacitive soil-moisture probe: a flat PCB blade with a three-pin connector at the top" width="360"></p>

Every OAT sketch shares one core: the same setup page and captive portal, the
same Wi-Fi flow, the same push engine (HTTPS-preferred, HMAC-signed, batched),
the same 60-second health heartbeat, and the same two-way USB console. **Set up
one OAT node and you have set up all of them** — only the sensor read differs.

**[Flash it from the browser](https://openagriculturetechnology.com/build/sketches/soil-moisture-node/)** —
the site page installs it over USB via ESP Web Tools (Chrome or Edge), and the
node's own setup page handles the rest. No IDE needed — or build from source
(below), if that's more your speed.

## Wiring (classic ESP32)

| Probe | ESP32 |
|---|---|
| VCC | **3V3** (not 5 V: the output must stay under what the chip can read) |
| GND | GND |
| AOUT | **GPIO 35** (the default); 32, 33, 34, 36 or 39 for more probes |

**Why only 32–39.** The classic ESP32 has two ADC blocks. ADC2 (GPIO 0, 2, 4,
12–15, 25–27) is shared with the Wi-Fi radio and stops answering the moment the
radio is up — which on this node is always. ADC1 is GPIO 32–39; 37 and 38 are not
brought out on a devkit. Six pins, six probes. The pin setting refuses anything
else, with the reason, because a pin choice is persisted and re-applied at every
boot.

**Analog is declared, never discovered.** A DS18B20 announces its serial and an
SHT-30 answers at its address, but a pin with nothing on it floats at a few
hundred millivolts and reads exactly like a wet probe. So the node reads only the
pins you list (35 out of the box), and a listed pin under 150 mV is reported as a
wire off, never as saturated soil.

## Set it up

1. **Flash from the browser** at the
   [sketch's site page](https://openagriculturetechnology.com/build/sketches/soil-moisture-node/)
   (Chrome or Edge, ESP Web Tools) — or build it yourself (see Build, below).
2. Join the node's own Wi-Fi (`OAT-…`) and its setup page walks you through
   Wi-Fi, delivery (webhook URL **or** MQTT), pins, and naming.
3. **Calibrate each probe on the setup page.** Hold it in dry air and click
   *this is dry air*; stand it in a glass of water to the line and click *this is
   water*. The node records the live millivolts for **that** probe. Until both
   points exist it sends the voltage only — no percentage, because it would be a
   guess.
4. **Watch it land.** Point the node at the live
   [Open Agriculture Technology Test Endpoint](https://iot-test.openagriculturetechnology.com/) —
   enter `https://iot-test.openagriculturetechnology.com/ingest` as the endpoint
   URL. (The push engine is HTTPS-preferred; if TLS ever won't fit the chip's
   memory it falls back to signed HTTP on its own — the oat1 signature keeps
   even that safe. You just enter the URL.) Then open the console
   and pick your farm — the gateway name you set at setup: **your gateway and
   its probes are there, live** — readings, charts, heartbeat. No account, no
   registration; showing up in the data is the registration. It's a
   proof-of-life bench (readings are kept about an hour): prove your chain
   works, then point the node at an endpoint you keep. Want a per-message
   schema verdict instead? The
   [conformance sandbox](https://openagriculturetechnology.com/standard/test-endpoint/)
   checks every POST against the standard.

## What it reports

| measurement | unit | fold over the push window | when |
|---|---|---|---|
| `voltage` | `V` | mean | always — the number of record |
| `soil_moisture` | `%` | mean | only once that probe has a dry point and a wet point |

```json
{
  "stream": "oat-3c8a1fa77d14:a35",
  "measurement": "soil_moisture",
  "value": 43.2,
  "unit": "%",
  "agg": { "window_s": 300, "samples": 10, "method": "mean" },
  "source": { "physical_id": "oat-3c8a1fa77d14:a35", "model": "capacitive soil probe" }
}
```

This is [**oat-ods**](https://openagriculturetechnology.com/standard/); the full
contract — batch envelope, field tables,
[JSON Schema](https://openagriculturetechnology.com/standard/oat-ods-0.3.schema.json),
sample payloads — lives in the
[developer reference](https://openagriculturetechnology.com/standard/reference/).

A capacitive probe has no serial number, so its identity is the pin it is on:
`<board id>:a<pin>`, the same shape the LoRa Field Node uses. Label the wire;
which pin is which pot is recorded at your endpoint.

## The parts that matter

- **The voltage is the number of record.** A capacitive probe is a copper trace,
  an oscillator and a filter; its output falls as the soil gets wetter, and that
  is all it promises. On the bench a v2.0 probe on 3V3 read 2876 mV in dry air and 1232 mV in water; two probes from one bag can read hundreds of millivolts apart in
  the same glass of water. So the raw voltage is always sent, and you can
  recalibrate at the endpoint years later without visiting the node.
- **The percentage is per probe, from two points you captured on that probe.**
  Dry air and a glass of water, recorded live from the setup page or the console
  (`set cal 35 dry`, `set cal 35 wet`, or `set cal 35 2876 1232` to type them).
  A probe with one point or none sends voltage only. A dry and a wet point less
  than 100 mV apart are refused with the reason: that is not two different states.
- **It is not volumetric water content.** Percent here means *between the two
  states you showed it*, in *this* soil. Sand, peat and clay each move the curve.
  For a number you can compare across sites, that is what SDI-12 instruments and
  tensiometers are for.
- **A wire off is reported as a wire off.** Under 150 mV a pin is not a wet probe;
  the reading is refused, counted, and shown on the status page in red.
- **The pin field is guarded.** ADC2 pins, flash pins and pins that are not analog
  inputs are refused with the reason. A silently accepted ADC2 pin would read
  fine on the bench and go dead the moment Wi-Fi joined.
- **Remote diagnosis.** When a probe dies, the 60 s heartbeat keeps arriving
  while its readings stop. That contrast — alive but quiet — is what the endpoint
  watches, and it is why the heartbeat is sent before the sensor push.

## Files

| File | What it is |
|---|---|
| `oat_soil_moisture_node.ino` | The firmware source (Arduino / ESP32). |
| `platformio.ini` | Build config. One env: classic ESP32. Pinned libs. |
| `merge_bin.py` | Post-build hook → one **merged factory image** at `out/firmware-esp32.bin`, flashable at offset 0. |
| `make_manifest.py` | Assembles `manifest.json` + `manifest-esp32.json` from whatever bins exist. |
| `build.sh` | Builds, then copies bins + manifests + source downloads into the site assets. |
| `docker-build.sh` | The same compile with no host toolchain (Docker + PlatformIO). |

## Build

**You don't need this section to use the node** — it flashes from the browser on
[its site page](https://openagriculturetechnology.com/build/sketches/soil-moisture-node/).
Building it yourself:

```bash
pipx install platformio        # or: pip install --user platformio
./build.sh
```

No host toolchain?

```bash
./docker-build.sh              # compile in Docker
SKIP_BUILD=1 ./build.sh        # then the asset steps
```

## FAQ

**Is the percentage volumetric water content?**
No. It is where the probe's voltage sits between two points you captured on
that probe, dry air and a glass of water, in the soil it is in. Sand, peat and
clay each move the curve. For a number that compares across soils, that is
what SDI-12 instruments and tensiometers are for; the
[Soil Moisture Tension page](https://openagriculturetechnology.com/library/soil-moisture-tension/)
explains the difference.

**Why does it read wet with nothing in the soil?**
Usually the output wire is off. An unconnected analog pin floats at a few
hundred millivolts, and because a capacitive probe's voltage falls as the soil
gets wetter, a low floating pin looks like saturated soil. Under 150 mV the node
says so in red and sends nothing for that pin.

**Can I use 5 V?**
You can, but there is nothing in it: the v2.0 probe gives the same 0 to 3.0 V
out on any supply from 3.3 to 5.5 V, and the ESP32's analog pins are not 5 V
tolerant. 3V3 is enough and safe. If a probe reads nothing useful from 3V3 it
is usually an older v1.2 board built with a timer chip that needs 5 V.

**How many probes on one board?**
Six on a classic ESP32: GPIO 35, 34, 32, 33, 36 and 39, the only analog pins
that keep working with Wi-Fi on. Past six, the
[Analog Reader](../oat-analog-reader/) adds four channels per ADS1115 on the
I²C bus, and the [LoRa Field Node](../oat-lora-field-node/) takes eight where
Wi-Fi does not reach.

## Related

- [`oat-lora-field-node/`](../oat-lora-field-node/) — the same probes at the far end of the property, over 915 MHz.
- [`oat-analog-reader/`](../oat-analog-reader/) — more analog channels on the I²C bus.
- [`../lib/oat_node_core/`](../lib/oat_node_core/) — the shared core every wired node runs on.
- [`../lib/oat_ods/`](../lib/oat_ods/) — the shared oat-ods encoder + measurand vocabulary.
- [The oat-ods standard](https://openagriculturetechnology.com/standard/reference/) — the wire format.

---
*Code: Apache-2.0 · Docs: CC BY 4.0 · An [OAT](https://openagriculturetechnology.com/) sketch — an OpenCDC initiative.*
