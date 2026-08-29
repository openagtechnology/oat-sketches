#!/usr/bin/env bash
# Build the OAT SDI-12 Reader for every supported chip and assemble the web-
# installer payload (merged factory bins + manifest.json) into an assets
# staging dir (out/site-assets by default; the OAT site build overrides
# it via OAT_SITE_ASSETS).
#
# Prereq: PlatformIO core only (no Arduino IDE):
#     pipx install platformio        # or: pip install --user platformio
#
# Run from this directory:
#     ./build.sh
#
# First-build note: if `pio run` can't resolve the platform, bump the pioarduino
# release pin in platformio.ini (see the VERIFY ON FIRST BUILD comment there).
set -euo pipefail
cd "$(dirname "$0")"

ASSETS="${OAT_SITE_ASSETS:-out/site-assets}"
VERSION="$(grep -oE 'FW_SEMVER +"[^"]+"' oat_sdi12_reader.ino | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"

echo "==> OAT SDI-12 Reader v${VERSION} — building all chip families"
rm -f out/firmware-*.bin
pio run                       # builds default_envs; merge_bin.py emits out/firmware-<mcu>.bin

echo "==> Copying merged images + manifest to site assets: ${ASSETS}"
mkdir -p "${ASSETS}"
cp out/firmware-*.bin "${ASSETS}/"
python3 make_manifest.py "${VERSION}" "${ASSETS}"

echo "==> Publishing source downloads (.ino + full project .zip)"
cp oat_sdi12_reader.ino "${ASSETS}/"
python3 - "${ASSETS}" <<'PY'
import zipfile, sys, os
dst = sys.argv[1]
src = ["oat_sdi12_reader.ino", "platformio.ini", "merge_bin.py",
       "make_manifest.py", "build.sh", "README.md"]
# Ship the shared oat_ods lib too, or the downloaded project can't resolve ../lib.
libs = ["../lib/oat_ods/oat_ods.h", "../lib/oat_ods/oat_measurands.h", "../lib/oat_ods/library.json"]
with zipfile.ZipFile(os.path.join(dst, "oat-sdi12-reader-firmware.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for f in src:
        if os.path.exists(f): z.write(f, arcname="oat-sdi12-reader/" + f)
    for f in libs:
        if os.path.exists(f): z.write(f, arcname="lib/" + f.split("lib/", 1)[1])
print("  project zip: sketch + shared oat_ods lib")
PY

echo "==> Done. Payload in ${ASSETS}:"
ls -la "${ASSETS}"
echo "Flash out/firmware-<mcu>.bin at offset 0 (esptool or ESP Web Tools)."
