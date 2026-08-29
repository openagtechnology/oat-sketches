/**
 * oat_ingest.js — reference oat-ods receiver (Node / Express).
 *
 * Drop-in secure endpoint for OAT device pushes. Set ONE secret and run it; the
 * device signs with the same secret. Spec + test vector + the matching Python/PHP
 * references: https://openagriculturetechnology.com/standard/  (auth profile v1, "oat1").
 *
 *   npm i express
 *   export OAT_INGEST_KEY="k_<32 hex>"   # paste this into each device's "Endpoint key"
 *   node oat_ingest.js                   # serves on :8099
 *
 * No key set = open sandbox. Key set = a valid signature is required. The key is
 * never sent — only an HMAC over the raw body — so it is safe over plain HTTP.
 */
const crypto = require('crypto');
const express = require('express');

const KEY = process.env.OAT_INGEST_KEY || '';        // '' = open sandbox
const KEEP = 50, MAX_BOXES = 200, MAX_BODY = 131072;
const store = new Map();                              // box -> { seq, rows }  (demo; use a DB)

const app = express();
app.use(express.raw({ type: '*/*', limit: MAX_BODY }));   // RAW body — we MUST verify over the exact bytes

const boxToken = (s) => ((s || '').toLowerCase().replace(/[^a-z0-9_-]/g, '').slice(0, 32) || 'demo');

function boxFromMsg(m) {
  const src = m.source || {}, dev = m.device || {};
  for (const c of [src.gateway_id, dev.name, dev.device_id, src.physical_id]) {
    const b = boxToken(String(c || ''));
    if (b !== 'demo') return b;
  }
  return 'demo';
}

function verify(req, raw) {
  if (!KEY) return [true, ''];                        // sandbox
  const sig = req.get('X-OAT-Signature') || '', ts = req.get('X-OAT-Timestamp') || '';
  if (!sig.startsWith('oat1=')) return [false, 'missing/unsupported signature'];
  const expect = crypto.createHmac('sha256', KEY).update(ts + '.').update(raw).digest('hex');
  const a = Buffer.from(expect), b = Buffer.from(sig.slice(5));
  return [a.length === b.length && crypto.timingSafeEqual(a, b), 'bad signature'];
}

function records(m) {                                 // split a batch into standalone records (+seq)
  const seqOf = (x) => (/^-?\d+$/.test(String(x)) ? parseInt(x, 10) : null);
  if (m.msg_type === 'batch' && Array.isArray(m.messages)) {
    const bsrc = m.source || {}, schema = m.schema || 'oat-ods/0.3', sent = m.sent_at || '';
    const out = m.messages.filter((r) => r && typeof r === 'object').map((r) => ({
      ...r, source: { ...bsrc, ...(r.source || {}) }, schema, msg_type: 'observation',
      observed_at: r.observed_at || sent,
    }));
    return [out, seqOf(m.seq)];
  }
  return [[m], seqOf(m.seq)];
}

app.post('/ingest', (req, res) => {
  const raw = req.body || Buffer.alloc(0);
  const [ok, err] = verify(req, raw);
  if (!ok) return res.status(401).json({ ok: false, error: err });
  let msg;
  try { msg = JSON.parse(raw.toString('utf8')); if (typeof msg !== 'object' || !msg) throw 0; }
  catch { return res.status(400).json({ ok: false, error: 'body is not a JSON object' }); }

  const box = req.query.box ? boxToken(req.query.box) : boxFromMsg(msg);
  if (!store.has(box) && store.size >= MAX_BOXES) return res.status(507).json({ ok: false, error: 'too many boxes' });
  const bx = store.get(box) || { seq: -1, rows: [] };
  store.set(box, bx);

  const [recs, seq] = records(msg);
  if (seq !== null) {
    if (seq < bx.seq) return res.status(409).json({ ok: false, error: 'replay: seq not increasing', box });
    if (seq === bx.seq) return res.json({ ok: true, box, duplicate: true, stored: 0 });   // idempotent retry
    bx.seq = seq;
  }
  for (const r of recs) bx.rows = [...bx.rows, r].slice(-KEEP);   // validate with ods_validate before trusting
  res.json({ ok: true, box, records: recs.length, stored: recs.length, duplicate: false });
});

app.listen(8099, () => console.log('oat-ingest on :8099'));
