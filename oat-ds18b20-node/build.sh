#!/usr/bin/env bash
# Build the OAT DS18B20 Node and assemble the web-installer payload (merged
# factory bin + manifests) into the OAT site assets, ready to rsync to the site.
#
# Prereq: PlatformIO core only (no Arduino IDE):
#     pipx install platformio        # or: pip install --user platformio
# No host toolchain? Use ./docker-build.sh instead, then re-run this for the
# asset steps (it skips the compile if out/ already holds the bins).
#
# Run from this directory:
#     ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

ASSETS="${OAT_SITE_ASSETS:-out/site-assets}"
VERSION="$(grep -oE 'FW_SEMVER +"[^"]+"' oat_ds18b20_node.ino | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"

echo "==> OAT DS18B20 Node v${VERSION}"
if [ -n "${SKIP_BUILD:-}" ] && compgen -G "out/firmware-*.bin" >/dev/null; then
  echo "    SKIP_BUILD set and out/ has bins — using them as-is"
else
  rm -f out/firmware-*.bin
  pio run                     # builds default_envs; merge_bin.py emits out/firmware-<mcu>.bin
fi

echo "==> Copying merged image + manifests to site assets: ${ASSETS}"
mkdir -p "${ASSETS}"
cp out/firmware-*.bin "${ASSETS}/"
python3 make_manifest.py "${VERSION}" "${ASSETS}"

echo "==> Publishing source downloads (.ino + full project .zip)"
cp oat_ds18b20_node.ino "${ASSETS}/"
python3 - "${ASSETS}" <<'PY'
import zipfile, sys, os
dst = sys.argv[1]
src = ["oat_ds18b20_node.ino", "platformio.ini", "merge_bin.py",
       "make_manifest.py", "build.sh", "docker-build.sh", "README.md"]
# Ship the shared libs too, or the downloaded project can't resolve ../lib.
libs = ["../lib/oat_ods/oat_ods.h", "../lib/oat_ods/oat_measurands.h", "../lib/oat_ods/library.json",
        "../lib/oat_sign/oat_sign.h", "../lib/oat_sign/library.json"]
with zipfile.ZipFile(os.path.join(dst, "oat-ds18b20-node-firmware.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for f in src:
        if os.path.exists(f): z.write(f, arcname="oat-ds18b20-node/" + f)
    for f in libs:
        if os.path.exists(f): z.write(f, arcname="lib/" + f.split("lib/", 1)[1])
print("  project zip: sketch + shared libs")
PY

echo "==> Done. Payload in ${ASSETS}:"
ls -la "${ASSETS}"
echo "Flash out/firmware-<mcu>.bin at offset 0 (esptool or ESP Web Tools)."
