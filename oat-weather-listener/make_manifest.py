#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Open Agriculture Technology Collective
# SPDX-License-Identifier: Apache-2.0
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
        "name": "OAT Radio Sensor Listener",
        "version": version,
        "funding_url": "https://openagriculturetechnology.com",
        "new_install_prompt_erase": True,
    }
    builds = []
    per_board = []
    # Env name -> chip family. An -fsk (or -fsk-bench) twin is the same board as the env
    # it is named for, so the suffix is stripped before the lookup.
    ENV_FAMILY = {"esp32": "ESP32", "esp32-s3": "ESP32-S3", "esp32-c3": "ESP32-C3", "esp32-c6": "ESP32-C6", "heltec-v2": "ESP32", "heltec-v3": "ESP32-S3", "ibc-v102": "ESP32", "ibc-v102-cc1101": "ESP32", "esp32-cc1101-ecowitt": "ESP32",
                  "esp32-cc1101-5n1": "ESP32", "esp32-cc1101-5n1-bench": "ESP32", "ibc-v102-bench": "ESP32"}

    def base_env(name):
        for suf in ("-fsk-bench", "-fsk"):
            if name.endswith(suf):
                return name[:-len(suf)]
        return name
    for path in sorted(glob.glob(os.path.join(dest, "firmware-*.bin"))):
        mcu = os.path.basename(path)[len("firmware-"):-len(".bin")]
        fam = ENV_FAMILY.get(base_env(mcu)) or FAMILY.get(mcu)
        if not fam:
            print("  ! no chipFamily mapping for mcu '%s' — skipped" % mcu)
            continue
        build = {"chipFamily": fam, "improv": False,
                 "parts": [{"path": os.path.basename(path), "offset": 0}]}
        builds.append(build)
        per_board.append((mcu, build))
    # Combined manifest (auto-detect: flashes whichever supported chip is connected).
    # Two boards share each chip family here, so the combined manifest keeps only the
    # devkit builds; the Heltec images, the IBC images and Build 4 (a second classic-ESP32
    # image) are reached by their own per-board buttons.
    _write(os.path.join(dest, "manifest.json"), dict(common, builds=[b for m, b in per_board if not m.startswith(("heltec", "ibc")) and "-fsk" not in m and "-5n1" not in m]))
    print("manifest.json: %d chip families" % len(builds))
    # Per-board manifests (one chip each). The per-board install buttons point here,
    # so a button only ever flashes its own board and errors on a chip mismatch — the
    # "pick your board, we verify it matches" guardrail, not a silent auto-detect.
    for mcu, build in per_board:
        _write(os.path.join(dest, "manifest-%s.json" % mcu), dict(common, builds=[build]))
    print("manifest-<mcu>.json: %d per-board manifests written" % len(per_board))


if __name__ == "__main__":
    main()
