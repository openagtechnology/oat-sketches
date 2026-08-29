#!/usr/bin/env bash
# Build the OAT Load-Cell Water node for every supported chip and assemble the
# web-installer payload into the OAT site assets.
# Prereq: PlatformIO core. Run from this directory: ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

ASSETS="${OAT_SITE_ASSETS:-out/site-assets}"
VERSION="$(grep -oE 'FW_SEMVER +"[^"]+"' oat_loadcell_water.ino | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"

echo "==> OAT Load-Cell Water v${VERSION} — building all chip families"
rm -f out/firmware-*.bin
pio run
echo "==> Copying merged images + manifest to: ${ASSETS}"
mkdir -p "${ASSETS}"
cp out/firmware-*.bin "${ASSETS}/"
python3 make_manifest.py "${VERSION}" "${ASSETS}"
echo "==> Publishing source downloads"
cp oat_loadcell_water.ino "${ASSETS}/"
python3 - "${ASSETS}" <<'PY'
import zipfile, sys, os
dst = sys.argv[1]
src = ["oat_loadcell_water.ino", "platformio.ini", "merge_bin.py", "make_manifest.py", "build.sh", "README.md"]
# Ship the shared oat_ods lib too, or the downloaded project can't resolve ../lib.
libs = ["../lib/oat_ods/oat_ods.h", "../lib/oat_ods/oat_measurands.h", "../lib/oat_ods/library.json"]
with zipfile.ZipFile(os.path.join(dst, "oat-loadcell-water-firmware.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for f in src:
        if os.path.exists(f): z.write(f, arcname="oat-loadcell-water/" + f)
    for f in libs:
        if os.path.exists(f): z.write(f, arcname="lib/" + f.split("lib/", 1)[1])
print("  project zip: sketch + shared oat_ods lib")
PY
echo "==> Done. Deploy: rsync -rlt --chmod=D2775,F664 ${ASSETS}/ oat:/var/www/oat/public/assets/firmware/oat-loadcell-water/"
