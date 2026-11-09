#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Assemble the GitHub Pages web installer for one firmware release.
#
#   tools/build-installer.sh <firmware-dir> <version> [outdir]      outdir defaults to _site
#
# <firmware-dir> must contain wifi-bridge-<version>.factory.bin and SHA256SUMS (as made by
# tools/package.sh or downloaded from a GitHub release); wifi-bridge-<version>.ota.bin is
# published too when present. The checksums are verified before anything is copied.
#
# Result: <outdir>/ = web-installer/ with manifest.json filled in, plus
# <outdir>/firmware/{wifi-bridge-<version>.factory.bin,.ota.bin,.safeboot.bin,SHA256SUMS}.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
die() { echo "build-installer.sh: $*" >&2; exit 1; }

[ $# -ge 2 ] || die "usage: $0 <firmware-dir> <version> [outdir]"
FW=$1
VER=${2#v}
OUT=${3:-_site}
NAME=wifi-bridge-$VER
SRC=$ROOT/web-installer

[[ $VER =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.+-]+)?$ ]] || die "version '$VER' is not semver"
[ -f "$FW/$NAME.factory.bin" ] || die "$FW/$NAME.factory.bin not found"
[ -f "$FW/SHA256SUMS" ] || die "$FW/SHA256SUMS not found"
(cd "$FW" && sha256sum --check --ignore-missing --strict SHA256SUMS) || die "checksum mismatch in $FW"
grep -q " $NAME.factory.bin\$" "$FW/SHA256SUMS" || die "SHA256SUMS does not list $NAME.factory.bin"

rm -rf "$OUT"
mkdir -p "$OUT/firmware"
cp -R "$SRC/." "$OUT/"
cp "$FW/$NAME.factory.bin" "$OUT/firmware/"
files=("$NAME.factory.bin")
for f in "$NAME.ota.bin" "$NAME.safeboot.bin"; do
  if [ -f "$FW/$f" ]; then cp "$FW/$f" "$OUT/firmware/"; files+=("$f"); fi
done
# Publish only the checksums of the files actually on the site (not the .elf).
(cd "$OUT/firmware" && sha256sum "${files[@]}" > SHA256SUMS)

python3 - "$SRC/manifest.json" "$OUT/manifest.json" "$VER" "firmware/$NAME.factory.bin" <<'EOF'
import json, sys
src, dst, version, path = sys.argv[1:]
with open(src) as f:
    m = json.load(f)
m["version"] = version
builds = [b for b in m["builds"] if b.get("chipFamily") == "ESP32"]
if len(builds) != 1 or len(builds[0]["parts"]) != 1:
    sys.exit("manifest template must have exactly one ESP32 build with one part")
builds[0]["parts"][0]["path"] = path
builds[0]["parts"][0]["offset"] = 0
with open(dst, "w") as f:
    json.dump(m, f, indent=2)
    f.write("\n")
EOF

echo "Web installer for $VER in $OUT:"
(cd "$OUT" && find . -type f | sort)
