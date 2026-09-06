#!/usr/bin/env bash
# Build the OAT Weather-Station Listener for both Heltec boards and assemble the web-installer payload
# (merged factory bins + manifest.json) into the OAT site assets. Same shape as
# every OAT sketch's build.sh. Docker path: ./docker-build.sh, then SKIP_BUILD=1 ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

ASSETS="${OAT_SITE_ASSETS:-out/site-assets}"
VERSION="$(grep -oE 'FW_SEMVER +"[^"]+"' oat_weather_listener.ino | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"

echo "==> OAT Weather-Station Listener v${VERSION}"
if [ -n "${SKIP_BUILD:-}" ] && compgen -G "out/firmware-*.bin" >/dev/null; then
  echo "    SKIP_BUILD set and out/ has bins — using them as-is (docker-build.sh made them)"
else
  rm -f out/firmware-*.bin
  pio run
fi

echo "==> Copying merged images + manifest to site assets: ${ASSETS}"
mkdir -p "${ASSETS}"
cp out/firmware-*.bin "${ASSETS}/"
python3 make_manifest.py "${VERSION}" "${ASSETS}"

echo "==> Publishing source downloads (.ino + full project .zip)"
cp oat_weather_listener.ino "${ASSETS}/"
python3 - "${ASSETS}" <<'PY'
import zipfile, sys, os
dst = sys.argv[1]
src = ["oat_weather_listener.ino", "platformio.ini", "merge_bin.py", "make_manifest.py", "build.sh", "docker-build.sh", "README.md"]
# Ship the shared libs too, or the downloaded project can't resolve ../lib.
libs = ["../lib/oat_ods/oat_ods.h", "../lib/oat_ods/oat_measurands.h", "../lib/oat_ods/library.json",
        "../lib/oat_sign/oat_sign.h", "../lib/oat_sign/library.json",
        "../lib/oat_node_core/oat_node_core.h", "../lib/oat_node_core/oat_node_core.cpp", "../lib/oat_node_core/library.json"]
with zipfile.ZipFile(os.path.join(dst, "oat-weather-listener-firmware.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for f in src:
        if os.path.exists(f): z.write(f, arcname="oat-weather-listener/" + f)
    for f in libs:
        if os.path.exists(f): z.write(f, arcname="lib/" + f.split("lib/", 1)[1])
print("  project zip: sketch + shared libs")
PY

echo "==> Done. Payload in ${ASSETS}:"
ls -la "${ASSETS}"
echo "Flash out/firmware-<mcu>.bin at offset 0 (esptool or ESP Web Tools)."
