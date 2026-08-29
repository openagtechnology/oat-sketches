#!/usr/bin/env python3
"""Assemble the ESP Web Tools manifest.json from whatever merged bins exist.

Usage:  python3 make_manifest.py <version> <dest_dir>

Data-driven: it lists one `builds[]` entry per firmware-<mcu>.bin present in
dest_dir, mapping the PlatformIO mcu name to the ESP Web Tools chipFamily. This
sketch builds one board today (classic ESP32); adding a chip = add an env in
platformio.ini, and this picks it up with no edits here.

`new_install_prompt_erase` is TRUE on purpose: a flash is a clean-slate event. A
node may be repurposed (different farm, different job), so it must NOT carry the
previous owner's wifi/endpoint/name across a reflash. Full-erase every flash;
setup is quick and the device is guaranteed clean for whoever gets it next.
"""
import sys
import os
import json
import glob

FAMILY = {
    "esp32":   "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c2": "ESP32-C2",
    "esp32c3": "ESP32-C3",
    "esp32c5": "ESP32-C5",
    "esp32c6": "ESP32-C6",
}


def _write(path, obj):
    with open(path, "w") as fh:
        fh.write(json.dumps(obj, indent=2) + "\n")


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else "0.0.0"
    dest = sys.argv[2] if len(sys.argv) > 2 else "."
    common = {
        "name": "OAT DS18B20 Node",
        "version": version,
        "funding_url": "https://openagriculturetechnology.com",
        "new_install_prompt_erase": True,
    }
    builds = []
    per_board = []
    for path in sorted(glob.glob(os.path.join(dest, "firmware-*.bin"))):
        mcu = os.path.basename(path)[len("firmware-"):-len(".bin")]
        fam = FAMILY.get(mcu)
        if not fam:
            print("  ! no chipFamily mapping for mcu '%s' — skipped" % mcu)
            continue
        build = {"chipFamily": fam, "improv": False,
                 "parts": [{"path": os.path.basename(path), "offset": 0}]}
        builds.append(build)
        per_board.append((mcu, build))
    # Combined manifest (auto-detect: flashes whichever supported chip is connected).
    _write(os.path.join(dest, "manifest.json"), dict(common, builds=builds))
    print("manifest.json: %d chip families" % len(builds))
    # Per-board manifests (one chip each). The per-board install buttons point here,
    # so a button only ever flashes its own board and errors on a chip mismatch — the
    # "pick your board, we verify it matches" guardrail, not a silent auto-detect.
    for mcu, build in per_board:
        _write(os.path.join(dest, "manifest-%s.json" % mcu), dict(common, builds=[build]))
    print("manifest-<mcu>.json: %d per-board manifests written" % len(per_board))


if __name__ == "__main__":
    main()
