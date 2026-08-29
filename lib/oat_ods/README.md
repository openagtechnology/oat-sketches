# oat_ods — the shared OAT encoder module

The one module every OAT device sketch links to emit **oat-ods**. Header-only,
ArduinoJson 7, no transport (the sketch owns the HTTP/MQTT send — this builds the
bytes). It is the device side of the Output Format Catalog in the
[oat-ods Core Contract](https://openagriculturetechnology.com/standard/reference/)
(Blueprint **BP-16441**).

## Why it exists

Write a new device — a BLE listener, a 1-Wire probe, a 5-sensor board — and it
pours into the **same message**. Only the transducer differs; the envelope comes
from here. That is Doctrine #7 (build once, deploy many) at the firmware layer.

## Use

```cpp
#include <oat_ods.h>

oat::Device dev = { "oat-greenhouse-2", "oat-1wire", "oat-single-probe/0.1.0" };

oat::Reading r;
r.stream_id   = "gh2-north-air";   // the place — the primary key, never changes
r.stream_name = "GH2 North Bench Air";
r.location    = "Greenhouse 2 / North Bench";
r.measurement = "temperature";
r.unit        = "Cel";
r.value       = 24.31;
r.physical_id = "28-0000071f3b2c"; // the gadget — swap a dead sensor, change only this
r.model       = "DS18B20";
r.observed_at = "2026-06-25T14:30:00Z";

String body;
oat::encodeNative(dev, r, body);   // -> POST body, or MQTT payload
```

See `examples/single_probe/`.

## The catalog (device-eligible formats)

| Call | Output |
|---|---|
| `encodeNative(dev, r, out)` | The canonical oat-ods observation (default). |
| `encodeSenml(dev, r, out)` | Strict SenML (RFC 8428) array — for SenML-aware servers. |
| `encodeFlat(dev, r, out)` | `{stream, measurement, value, ts}` — the easy custom/AI-endpoint target. |
| `encodeStatus(dev, state, iso, health, out)` | Liveness: `online` (boot/heartbeat) / `offline` (MQTT Last Will). |

MQTT helpers key topics on the **logical stream**, so they survive a sensor swap:
`mqttTopic(prefix, dev, r)` → `oat/<gateway_id>/<stream_id>/<measurement>`;
`statusTopic(prefix, dev)` → `oat/<gateway_id>/status`.

Home Assistant discovery: `haConfigTopic()` / `haDiscoveryConfig()` publish a
retained config so HA auto-creates the entity; the state goes to the native
per-stream topic and HA reads it with `val_tpl: {{ value_json.value }}`.
`haUnit()` translates SenML units to HA's (`Cel` → `°C`, `%RH` → `%`).

## Rules baked in (from the contract)

- **One measurement per message.**
- `stream.id` is the primary key; `source.physical_id` is swappable provenance.
- `gateway_id` is **assignable**, never the raw chip MAC.
- Empty/sentinel optional fields are omitted; consumers ignore unknown fields,
  so adding fields later is non-breaking.

## Golden output

`encodeNative` with the example values above produces (validated against
`oat-ods-0.3.schema.json`):

```json
{"schema":"oat-ods/0.3","msg_type":"observation","observed_at":"2026-06-25T14:30:00Z","stream":{"id":"gh2-north-air","name":"GH2 North Bench Air","location":"Greenhouse 2 / North Bench"},"measurement":"temperature","value":24.31,"unit":"Cel","source":{"tier":"oat-1wire","gateway_id":"oat-greenhouse-2","physical_id":"28-0000071f3b2c","model":"DS18B20"}}
```

> Note: the `status` message (`msg_type:"status"`) is a sibling of the observation
> and is **not** covered by the v0.3 observation schema — that's expected; a future
> schema revision will model it.
