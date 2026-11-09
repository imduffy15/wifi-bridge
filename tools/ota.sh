#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Update a WiFi Bridge over the network.
#
#   tools/ota.sh <device-ip> [image]        image defaults to build/wifi-bridge.bin
#
# Env:
#   BRIDGE_ADMIN_PASSWORD  web admin password, if one is set
#
# Boards with two firmware slots are updated with one upload. Boards with the safeboot layout restart into
# the WiFi Bridge safeboot firmware, which takes the upload and starts the new firmware.
set -euo pipefail
cd "$(dirname "$0")/.."
DEV=${1:?usage: tools/ota.sh <device-ip> [image]}
IMG=$(realpath "${2:-build/wifi-bridge.bin}")
[ -f "$IMG" ] || { echo "no image: $IMG" >&2; exit 1; }
[ "$(head -c1 "$IMG" | od -An -tx1 | tr -d ' ')" = e9 ] || { echo "$IMG is not an ESP32 app image (use the .ota.bin, not the .factory.bin)" >&2; exit 1; }
META=$(python3 - "$IMG" <<'PY'
import json, sys
with open(sys.argv[1], 'rb') as f:
    h = f.read(288)
def field(offset, length):
    return h[offset:offset+length].split(b'\0')[0].decode('ascii')
if len(h) < 288 or h[12:14] != b'\0\0' or h[32:36] != b'\x32\x54\xcd\xab' or field(80, 32) != 'wifi-bridge':
    sys.exit('Use a WiFi Bridge ESP32 .ota.bin image')
print(json.dumps({'version': field(48, 32), 'build': field(128, 16) + ' ' + field(112, 16), 'elf_sha256': h[176:208].hex()}))
PY
)

AUTH=()
[ -n "${BRIDGE_ADMIN_PASSWORD:-}" ] && AUTH=(-u "admin:$BRIDGE_ADMIN_PASSWORD")
log(){ echo "$(date +%T) $*"; }
json(){ python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1], {}, {"d": d}))' "$1"; }

wait_back(){
  for _ in $(seq 1 90); do
    if st=$(curl -sf -m4 "${AUTH[@]}" "http://$DEV/api/status"); then
      if python3 - "$META" "$st" <<'PY'
import json, sys
expected, status = map(json.loads, sys.argv[1:])
d = status.get('device', {})
# Older releases did not report a fingerprint; retain version/build verification for those.
if 'elf_sha256' not in d:
    expected.pop('elf_sha256')
sys.exit(0 if status.get('mode') in ('bridge', 'setup') and all(d.get(k) == v for k, v in expected.items()) else 1)
PY
      then
        log "verified: $(echo "$st" | json 'd["device"]["version"] + " on " + d["device"]["partition"] + ", built " + d["device"]["build"]')"; return 0
      fi
    fi
    sleep 3
  done
  log "the requested firmware was not verified at $DEV; check its status before retrying"; return 1
}

upload_native(){
  log "uploading $(basename "$IMG") ($(stat -c %s "$IMG") bytes)"
  local resp
  resp=$(curl -sS -m300 -w '\n%{http_code}' "${AUTH[@]}" -H 'Content-Type: application/octet-stream' \
         --data-binary "@$IMG" "http://$DEV/api/ota/upload") || {
    log "upload connection lost; checking whether the requested firmware starts"
    sleep 5; wait_back; return $?
  }
  [ "${resp##*$'\n'}" = 200 ] && [ "$(echo "${resp%$'\n'*}" | json 'd.get("ok")')" = True ] || {
    log "upload refused: ${resp%$'\n'*}"; return 1;
  }
  sleep 5; wait_back
}

st=$(curl -sf -m5 "${AUTH[@]}" "http://$DEV/api/status") || { log "no WiFi Bridge answering at $DEV"; exit 1; }
if [ "$(echo "$st" | json 'd.get("mode")')" = safeboot ]; then upload_native; exit $?; fi
MODE=$(echo "$st" | json 'd["device"]["ota_mode"]')
log "$DEV runs $(echo "$st" | json 'd["device"]["version"]'), update method: $MODE"
case "$MODE" in
  dual) upload_native; exit $? ;;
  safeboot) ;;
  *) log "this device can't be updated over the network (ota_mode $MODE): flash it over serial"; exit 1 ;;
esac
if [ "$(echo "$st" | json '(d["device"].get("safeboot") or {}).get("ours", False)')" != True ]; then
  log "install wifi-bridge-<version>.safeboot.bin from the Firmware page first"; exit 1
fi
curl -fsS -m10 "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"target":"safeboot"}' "http://$DEV/api/reboot" >/dev/null
log "restarting into safeboot"
for _ in $(seq 1 60); do
  if st=$(curl -sf -m4 "${AUTH[@]}" "http://$DEV/api/status") && [ "$(echo "$st" | json 'd.get("mode")')" = safeboot ]; then
    upload_native; exit $?
  fi
  sleep 2
done
log "the safeboot firmware did not appear at $DEV; check the device before retrying"; exit 1
