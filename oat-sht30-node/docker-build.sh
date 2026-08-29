#!/usr/bin/env bash
# Reproducible OAT firmware build in Docker (no host PlatformIO needed). Mounts
# the firmware ROOT so the sketch's lib_extra_dirs=../lib resolves to firmware/lib,
# with the sketch as workdir. Caches PlatformIO under /tmp/pio-cache between runs.
# Usage:  ./docker-build.sh             # the default env (classic ESP32)
#         ./docker-build.sh "-e esp32"  # name it explicitly
# After a green build, run SKIP_BUILD=1 ./build.sh for the asset steps.
set -uo pipefail
FWROOT="$(cd "$(dirname "$0")/.." && pwd)"   # the firmware root
PIOARGS="${1:-}"
echo "=== docker build start (pio run $PIOARGS) ==="; date
docker run --rm \
  -v "$FWROOT":/work \
  -v /tmp/pio-cache:/root/.platformio \
  -w /work/oat-sht30-node \
  python:3.11-slim bash -c "
    apt-get update -qq && apt-get install -y -qq git >/dev/null 2>&1 || { echo GIT_FAIL; exit 3; }
    pip install -q platformio 2>/dev/null || { echo PIP_FAIL; exit 2; }
    pio run $PIOARGS 2>&1; rc=\$?
    chmod -R a+rwX .pio out 2>/dev/null || true
    echo INNER_RC=\$rc
    ls -la out/ 2>/dev/null || echo '(no out/)'
    exit \$rc
  "
echo "=== docker exit=$? ==="
echo "=== BUILD_DONE ==="
