#!/usr/bin/env python3
"""
oat_ingest_mqtt.py — reference oat-ods receiver (Python / MQTT subscriber).

The MQTT counterpart to oat_ingest.py (webhook). Point it at your broker and every
oat-ods message a device publishes lands here: observations on the logical topic
  oat/{gateway_id}/{stream.id}/{measurement}   (published retained)
and node liveness on
  oat/{gateway_id}/status                       (with a Last Will)
Spec + topic scheme: https://openagriculturetechnology.com/standard/reference/

    pip install paho-mqtt
    export OAT_MQTT_HOST=localhost        # your broker
    #  optional: OAT_MQTT_PORT (1883), OAT_MQTT_TOPIC (oat/#),
    #            OAT_MQTT_USER, OAT_MQTT_PASS
    python3 oat_ingest_mqtt.py

Authenticity is the broker's job here (username/password + TLS), not a per-message
signature — that's the one difference from the webhook receiver. Retained messages
arrive the moment you subscribe, so a fresh subscriber immediately sees the latest
value for every stream.
"""
import json
import os
import sys

import paho.mqtt.client as mqtt          # pip install paho-mqtt

HOST = os.environ.get("OAT_MQTT_HOST", "localhost")
PORT = int(os.environ.get("OAT_MQTT_PORT", "1883"))
TOPIC = os.environ.get("OAT_MQTT_TOPIC", "oat/#")     # every OAT topic
USER = os.environ.get("OAT_MQTT_USER", "")
PASS = os.environ.get("OAT_MQTT_PASS", "")
KEEP = 50
STORE = {}                                            # stream.id -> [recent rows]  (demo; use a DB)


def on_connect(client, userdata, flags, reason_code, properties=None):
    # paho v1 passes an int rc, v2 a ReasonCode; both print, and we subscribe either way.
    client.subscribe(TOPIC)
    print(f"connected ({reason_code}); subscribed to {TOPIC} on {HOST}:{PORT}")


def on_message(client, userdata, msg):
    # The topic tells us the shape: oat/{gw}/{stream}/{measurement} or oat/{gw}/status.
    parts = msg.topic.split("/")
    try:
        m = json.loads(msg.payload.decode("utf-8"))
        if not isinstance(m, dict):
            raise ValueError("not a JSON object")
    except (ValueError, UnicodeDecodeError) as e:
        print(f"! bad payload on {msg.topic}: {e}", file=sys.stderr)
        return

    if len(parts) >= 2 and parts[-1] == "status":
        print(f"[status] {parts[1]}: {m.get('state', '?')}")
        return

    # An observation. Key on the LOGICAL stream, not the hardware, so a sensor swap is seamless.
    stream = (m.get("stream") or {}).get("id") or (parts[2] if len(parts) > 2 else msg.topic)
    rows = STORE.setdefault(stream, [])
    rows.append(m)                                    # validate with the JSON Schema before trusting it
    del rows[:-KEEP]
    tag = " (retained)" if msg.retain else ""
    print(f"[{stream}] {m.get('measurement')} = {m.get('value')} {m.get('unit', '')}{tag}".rstrip())


def main() -> int:
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)   # paho-mqtt >= 2.0
    except (AttributeError, TypeError):
        client = mqtt.Client()                                   # paho-mqtt 1.x
    if USER:
        client.username_pw_set(USER, PASS)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(HOST, PORT, keepalive=60)
    client.loop_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
