#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Package a finished `idf.py build` into release artifacts.
#
#   tools/package.sh [outdir] [builddir]      outdir defaults to dist, builddir to build
#
# Produces in <outdir>:
#   wifi-bridge-<version>.ota.bin      app image only: web upload, URL, or install from the safeboot firmware
#   wifi-bridge-<version>.safeboot.bin safeboot firmware, for boards with the safeboot layout (from build-safeboot/)
#   wifi-bridge-<version>.factory.bin  bootloader + partition table + app, flash at 0x0
#   wifi-bridge-<version>.elf          symbols for decoding crash backtraces
#   SHA256SUMS
#
# The version comes from $VERSION if set, otherwise version.txt. The script fails if it
# does not match the PROJECT_VER the firmware was built with, so a release can never
# ship an image that reports a different version.
#
# Needs python3 and esptool.py on PATH (run `. "$IDF_PATH/export.sh"` first).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-dist}
BUILD=${2:-build}
case $OUT in /*) ;; *) OUT=$ROOT/$OUT ;; esac
case $BUILD in /*) ;; *) BUILD=$ROOT/$BUILD ;; esac

die() { echo "package.sh: $*" >&2; exit 1; }

DESC=$BUILD/project_description.json
[ -f "$DESC" ] || die "$DESC not found; run idf.py build first"
[ -f "$BUILD/flash_args" ] || die "$BUILD/flash_args not found"

# Read a key from project_description.json (paths in it are absolute to the build host,
# which may be a container, so only the file names are used).
desc() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$DESC" "$1"; }

APP_BIN=$BUILD/$(basename "$(desc app_bin)")
APP_ELF=$BUILD/$(basename "$(desc app_elf)")
BUILT_VER=$(desc project_version)

if [ -n "${VERSION:-}" ]; then
  VER=$VERSION
elif [ -f "$ROOT/version.txt" ]; then
  VER=$(tr -d ' \t\r\n' < "$ROOT/version.txt")
else
  die "no version: set VERSION or create version.txt"
fi
VER=${VER#v}
[[ $VER =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.+-]+)?$ ]] || die "version '$VER' is not semver"
[ "$BUILT_VER" = "$VER" ] || die "firmware was built as '$BUILT_VER' but version is '$VER' (stale build or CMakeLists.txt not reading version.txt?)"

for f in "$APP_BIN" "$APP_ELF" "$BUILD/bootloader/bootloader.bin" "$BUILD/partition_table/partition-table.bin"; do
  [ -f "$f" ] || die "$f not found"
done
command -v esptool.py >/dev/null || die "esptool.py not on PATH; source \$IDF_PATH/export.sh"

NAME=wifi-bridge-$VER
mkdir -p "$OUT"
rm -f "$OUT/$NAME".ota.bin "$OUT/$NAME".factory.bin "$OUT/$NAME".safeboot.bin "$OUT/$NAME".elf "$OUT/SHA256SUMS"

cp "$APP_BIN" "$OUT/$NAME.ota.bin"
cp "$APP_ELF" "$OUT/$NAME.elf"
# flash_args holds the flash mode/freq/size line and the offset/file pairs relative to the
# build dir, so merge_bin has to run from there.
(cd "$BUILD" && esptool.py --chip esp32 merge_bin -o "$OUT/$NAME.factory.bin" @flash_args) >/dev/null

SAFEBOOT=${SAFEBOOT_BIN:-build-safeboot/wifi-bridge-safeboot.bin}
FILES="$NAME.ota.bin $NAME.factory.bin $NAME.elf"
if [ -f "$SAFEBOOT" ]; then cp "$SAFEBOOT" "$OUT/$NAME.safeboot.bin"; FILES="$FILES $NAME.safeboot.bin"
else echo "package.sh: $SAFEBOOT not found, skipping the safeboot firmware" >&2; fi
(cd "$OUT" && sha256sum $FILES > SHA256SUMS)

echo "Packaged $NAME into $OUT:"
(cd "$OUT" && ls -l "$NAME".* SHA256SUMS)
