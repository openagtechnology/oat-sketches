#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Open Agriculture Technology Collective
# SPDX-License-Identifier: Apache-2.0
# Build the OAT Weather-Station Listener (every env in platformio.ini) and assemble the web-installer payload
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

# Two destinations. The bare-receiver and on-off-keying images are the public offer.
# The -fsk images have NOT heard a sensor yet (1.1.0, 2026-09-18), so they go to
# bench/, which only the unlisted bench page links. When an FSK image has been
# benched, move its env name into OFFERED_FSK below and it publishes with the rest.
OFFERED_FSK="esp32-cc1101-ecowitt-fsk"   # Build 5, heard on our bench 2026-10-04
BENCH="${ASSETS}/bench"
echo "==> Copying merged images + manifests: public -> ${ASSETS} ; unbenched FSK -> ${BENCH}"
mkdir -p "${ASSETS}" "${BENCH}"
# Replace only the images this build made: bench/ also holds images other sketches put
# there (the LoRa node and gateway), and a blanket rm deleted them.
for f in out/firmware-*.bin; do
  b="$(basename "$f")"; e="${b#firmware-}"; e="${e%.bin}"
  rm -f "${ASSETS}/${b}" "${BENCH}/${b}" "${ASSETS}/manifest-${e}.json" "${BENCH}/manifest-${e}.json"
done
for f in out/firmware-*.bin; do
  env="$(basename "$f" .bin)"; env="${env#firmware-}"
  case "$env" in
    *-fsk|*-fsk-bench)
      if [[ " ${OFFERED_FSK} " == *" ${env} "* ]]; then cp "$f" "${ASSETS}/"; else cp "$f" "${BENCH}/"; fi ;;
    *-bench) cp "$f" "${BENCH}/" ;;   # bench-tool twins: the bench page only
    *) cp "$f" "${ASSETS}/" ;;
  esac
done
python3 make_manifest.py "${VERSION}" "${ASSETS}"
python3 make_manifest.py "${VERSION}" "${BENCH}"

echo "==> Publishing source downloads (.ino + full project .zip)"
cp oat_weather_listener.ino "${ASSETS}/"
python3 - "${ASSETS}" <<'PY'
import zipfile, sys, os, glob
dst = sys.argv[1]
src = ["oat_weather_listener.ino", "platformio.ini", "merge_bin.py", "make_manifest.py", "build.sh", "docker-build.sh", "README.md"]
# Ship the shared libs too, or the downloaded project can't resolve ../lib.
libs = ["../lib/oat_ods/oat_ods.h", "../lib/oat_ods/oat_measurands.h", "../lib/oat_ods/library.json",
        "../lib/oat_sign/oat_sign.h", "../lib/oat_sign/library.json",
        "../lib/oat_node_core/oat_node_core.h", "../lib/oat_node_core/oat_node_core.cpp", "../lib/oat_node_core/library.json",
        "../lib/oat_rf433/oat_rf433.h", "../lib/oat_rf433/library.json",
        "../lib/oat_rf915/oat_rf915.h", "../lib/oat_rf915/library.json"]
with zipfile.ZipFile(os.path.join(dst, "oat-weather-listener-firmware.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for f in src:
        if os.path.exists(f): z.write(f, arcname="oat-weather-listener/" + f)
    for f in libs:
        if os.path.exists(f): z.write(f, arcname="lib/" + f.split("lib/", 1)[1])
    for f in ["../LICENSE", "../NOTICE"] + sorted(glob.glob("../LICENSES/*")):
        if os.path.exists(f): z.write(f, arcname=os.path.relpath(f, ".."))
    if os.path.exists("NOTICE"): z.write("NOTICE", arcname="oat-weather-listener/NOTICE")
print("  project zip: sketch + shared libs")
PY

echo "==> Done. Payload in ${ASSETS}:"
ls -la "${ASSETS}"
echo "Flash out/firmware-<mcu>.bin at offset 0 (esptool or ESP Web Tools)."
