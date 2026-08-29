#!/usr/bin/env python3
"""Assemble the ESP Web Tools manifest.json from whatever merged bins exist.

Usage:  python3 make_manifest.py <version> <dest_dir>

Data-driven: it lists one `builds[]` entry per firmware-<mcu>.bin present in
dest_dir, mapping the PlatformIO mcu name to the ESP Web Tools chipFamily. Add a
chip = add an env in platformio.ini; this picks it up with no edits here.

`new_install_prompt_erase` is FALSE on purpose: a reflash must preserve the
node's NVS config (the firmware's stated promise). A clean wipe stays an
advanced, opt-in action — never the default a grower walks into.
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


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else "0.0.0"
    dest = sys.argv[2] if len(sys.argv) > 2 else "."
    builds = []
    for path in sorted(glob.glob(os.path.join(dest, "firmware-*.bin"))):
        mcu = os.path.basename(path)[len("firmware-"):-len(".bin")]
        fam = FAMILY.get(mcu)
        if not fam:
            print("  ! no chipFamily mapping for mcu '%s' — skipped" % mcu)
            continue
        builds.append({
            "chipFamily": fam,
            "improv": False,
            "parts": [{"path": os.path.basename(path), "offset": 0}],
        })
    manifest = {
        "name": "OAT Load-Cell Water",
        "version": version,
        "funding_url": "https://openagriculturetechnology.com",
        "new_install_prompt_erase": False,
        "builds": builds,
    }
    out = os.path.join(dest, "manifest.json")
    with open(out, "w") as fh:
        fh.write(json.dumps(manifest, indent=2) + "\n")
    print("manifest.json: %d chip families -> %s" % (len(builds), out))


if __name__ == "__main__":
    main()
