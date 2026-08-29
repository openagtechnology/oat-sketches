#!/usr/bin/env python3
"""
oat_ingest.py — reference oat-ods receiver (Python / Flask).

A drop-in secure endpoint for OAT device pushes. Set ONE secret and run it; the
device signs with the same secret and you are done. Full spec + test vector:
https://openagriculturetechnology.com/standard/  (oat-ods auth profile v1, "oat1").

    pip install flask
    export OAT_INGEST_KEY=$(python3 -c "import secrets;print('k_'+secrets.token_hex(16))")
    #  ^ print this once and paste it into each device's "Endpoint key" field.
    python3 oat_ingest.py        # serves on :8099

No key set = open sandbox (unsigned accepted). Key set = a valid signature is
required. Authenticity/integrity hold even over plain HTTP, because the key is
never sent — only a signature over the raw body.
"""
import hashlib
import hmac
import json
import os
import re
import time

from flask import Flask, request, jsonify

app = Flask(__name__)
KEY = os.environ.get("OAT_INGEST_KEY", "")          # "" = open sandbox
STORE = {}                                          # box -> {"seq": int, "rows": [...]}  (demo; use a DB)
KEEP, MAX_BOXES, MAX_BODY = 50, 200, 131072


def verify(raw: bytes) -> tuple[bool, str]:
    """Verify the oat1 HMAC over the RAW body. Constant-time. Replay is the seq, not the clock."""
    if not KEY:
        return True, ""                              # sandbox
    sig = request.headers.get("X-OAT-Signature", "")
    ts = request.headers.get("X-OAT-Timestamp", "")
    if not sig.startswith("oat1="):
        return False, "missing/unsupported signature"
    expect = hmac.new(KEY.encode(), (ts + ".").encode() + raw, hashlib.sha256).hexdigest()
    return (hmac.compare_digest(expect, sig[5:]), "bad signature")


def box_token(raw: str) -> str:
    b = re.sub(r"[^a-z0-9_-]", "", (raw or "").lower())[:32]
    return b or "demo"


def box_from_msg(m: dict) -> str:
    src, dev = m.get("source") or {}, m.get("device") or {}
    for cand in (src.get("gateway_id"), dev.get("name"), dev.get("device_id"), src.get("physical_id")):
        b = box_token(str(cand or ""))
        if b != "demo":
            return b
    return "demo"


def records(m: dict):
    """Split a batch into standalone records (+seq); a single message passes through."""
    if m.get("msg_type") == "batch" and isinstance(m.get("messages"), list):
        bsrc, schema, sent = m.get("source") or {}, m.get("schema", "oat-ods/0.3"), m.get("sent_at", "")
        seq = int(m["seq"]) if str(m.get("seq", "")).lstrip("-").isdigit() else None
        out = []
        for r in m["messages"]:
            if not isinstance(r, dict):
                continue
            r = {**r, "source": {**bsrc, **(r.get("source") or {})}, "schema": schema, "msg_type": "observation"}
            r.setdefault("observed_at", sent)
            out.append(r)
        return out, seq
    seq = int(m["seq"]) if str(m.get("seq", "")).lstrip("-").isdigit() else None
    return [m], seq


@app.post("/ingest")
def ingest():
    raw = request.get_data(cache=False)[:MAX_BODY]
    ok, err = verify(raw)
    if not ok:
        return jsonify(ok=False, error=err), 401
    try:
        msg = json.loads(raw)
        assert isinstance(msg, dict)
    except Exception:
        return jsonify(ok=False, error="body is not a JSON object"), 400

    box = box_token(request.args["box"]) if request.args.get("box") else box_from_msg(msg)
    if box not in STORE and len(STORE) >= MAX_BOXES:
        return jsonify(ok=False, error="too many boxes"), 507
    bx = STORE.setdefault(box, {"seq": -1, "rows": []})

    recs, seq = records(msg)
    if seq is not None:
        if seq < bx["seq"]:
            return jsonify(ok=False, error="replay: seq not increasing", box=box), 409
        if seq == bx["seq"]:                          # idempotent retry
            return jsonify(ok=True, box=box, duplicate=True, stored=0)
        bx["seq"] = seq

    for r in recs:                                    # validate + store (use ods_validate.py for the strict check)
        bx["rows"] = (bx["rows"] + [r])[-KEEP:]
    return jsonify(ok=True, box=box, records=len(recs), stored=len(recs), duplicate=False)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8099)
