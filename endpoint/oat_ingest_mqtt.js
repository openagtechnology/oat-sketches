/**
 * oat_ingest_mqtt.js — reference oat-ods receiver (Node / MQTT subscriber).
 *
 * The MQTT counterpart to oat_ingest.js (webhook). Point it at your broker and every
 * oat-ods message a device publishes lands here: observations on the logical topic
 *   oat/{gateway_id}/{stream.id}/{measurement}   (published retained)
 * and node liveness on
 *   oat/{gateway_id}/status                       (with a Last Will)
 * Spec + topic scheme: https://openagriculturetechnology.com/standard/reference/
 *
 *   npm i mqtt
 *   OAT_MQTT_URL=mqtt://localhost:1883 node oat_ingest_mqtt.js
 *   #  optional: OAT_MQTT_TOPIC (oat/#), OAT_MQTT_USER, OAT_MQTT_PASS
 *
 * Authenticity is the broker's job here (username/password + TLS), not a per-message
 * signature — the one difference from the webhook receiver. Retained messages arrive
 * the moment you subscribe, so a fresh subscriber immediately sees the latest value
 * for every stream.
 */
const mqtt = require('mqtt');                          // npm i mqtt

const URL = process.env.OAT_MQTT_URL || 'mqtt://localhost:1883';
const TOPIC = process.env.OAT_MQTT_TOPIC || 'oat/#';   // every OAT topic
const KEEP = 50;
const store = new Map();                               // stream.id -> recent rows  (demo; use a DB)

const opts = {};
if (process.env.OAT_MQTT_USER) {
  opts.username = process.env.OAT_MQTT_USER;
  opts.password = process.env.OAT_MQTT_PASS || '';
}

const client = mqtt.connect(URL, opts);

client.on('connect', () => {
  client.subscribe(TOPIC, (err) => {
    if (err) return console.error('subscribe failed:', err.message);
    console.log(`subscribed to ${TOPIC} on ${URL}`);
  });
});

client.on('message', (topic, payload, packet) => {
  // The topic tells us the shape: oat/{gw}/{stream}/{measurement} or oat/{gw}/status.
  const parts = topic.split('/');
  let m;
  try { m = JSON.parse(payload.toString('utf8')); if (typeof m !== 'object' || !m) throw 0; }
  catch { return console.error(`! bad payload on ${topic}`); }

  if (parts.length >= 2 && parts[parts.length - 1] === 'status') {
    return console.log(`[status] ${parts[1]}: ${m.state || '?'}`);
  }

  // An observation. Key on the LOGICAL stream, not the hardware, so a sensor swap is seamless.
  const stream = (m.stream && m.stream.id) || parts[2] || topic;
  const rows = store.get(stream) || [];
  rows.push(m);                                        // validate with the JSON Schema before trusting it
  if (rows.length > KEEP) rows.splice(0, rows.length - KEEP);
  store.set(stream, rows);
  const tag = packet.retain ? ' (retained)' : '';
  console.log(`[${stream}] ${m.measurement} = ${m.value} ${m.unit || ''}${tag}`.trimEnd());
});

client.on('error', (err) => console.error('mqtt error:', err.message));
