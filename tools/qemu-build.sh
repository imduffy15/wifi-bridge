#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Build the QEMU test variant into build-qemu/ and a 4 MB flash image build-qemu/flash.bin.
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE=${IDF_IMAGE:-espressif/idf:v5.4}
# Also builds the safeboot firmware (build-qemu-safeboot/) and build-qemu/flash-safeboot.bin: the safeboot
# layout with the safeboot firmware in the factory slot and the firmware in app0 (tools/qemu_safeboot_test.py).
cmd='rm -f build-qemu/sdkconfig build-qemu-safeboot/sdkconfig
     idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build &&
     (cd build-qemu && esptool.py --chip esp32 merge_bin --fill-flash-size 4MB -o flash.bin @flash_args) &&
     idf.py -C safeboot -B "$PWD/build-qemu-safeboot" -D SDKCONFIG="$PWD/build-qemu-safeboot/sdkconfig" \
            -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;../sdkconfig.qemu" build &&
     esptool.py --chip esp32 merge_bin --fill-flash-size 4MB -o build-qemu/flash-safeboot.bin \
            0x1000 build-qemu-safeboot/bootloader/bootloader.bin 0x8000 build-qemu-safeboot/partition_table/partition-table.bin \
            0x10000 build-qemu-safeboot/wifi-bridge-safeboot.bin 0xd0000 build-qemu/wifi-bridge.bin'
if [ -n "${IDF_PATH:-}" ]; then bash -c "$cmd"; else
  docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp "$IMAGE" bash -c "$cmd"
fi
