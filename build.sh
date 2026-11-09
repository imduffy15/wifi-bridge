#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Build with the ESP-IDF v5.4 docker image.
#   ./build.sh            -> build/wifi-bridge.bin (OTA image), build/wifi-bridge.factory.bin (flash at 0x0)
#                            and build-safeboot/wifi-bridge-safeboot.bin (safeboot firmware)
#   ./build.sh dist       -> also dist/ (release files, see tools/package.sh)
#   ./build.sh clean      -> fullclean
set -euo pipefail
cd "$(dirname "$0")"
IMAGE=${IDF_IMAGE:-espressif/idf:v5.4}
run() { docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp "$IMAGE" bash -c "$1"; }
if [ "${1:-}" = clean ]; then run 'idf.py fullclean'; rm -f sdkconfig; exit 0; fi
# sdkconfig is generated from sdkconfig.defaults; remove it so changes to the defaults always apply
rm -f sdkconfig
DEFAULTS="sdkconfig.defaults"
if [ -f sdkconfig.secrets ]; then DEFAULTS="$DEFAULTS;sdkconfig.secrets"; echo "using presets from sdkconfig.secrets"; fi
run "idf.py -D SDKCONFIG_DEFAULTS='$DEFAULTS' build && (cd build && esptool.py --chip esp32 merge_bin -o wifi-bridge.factory.bin @flash_args) &&
     rm -f build-safeboot/sdkconfig && idf.py -C safeboot -B /project/build-safeboot -D SDKCONFIG=/project/build-safeboot/sdkconfig build"
if [ "${1:-}" = dist ]; then run 'tools/package.sh dist'; fi
ls -l build/wifi-bridge.bin build/wifi-bridge.factory.bin build-safeboot/wifi-bridge-safeboot.bin
