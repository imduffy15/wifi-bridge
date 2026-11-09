<!-- SPDX-License-Identifier: Apache-2.0 -->
# Contributing

Bug reports, fixes, and new board presets are welcome. For larger changes, open an issue first.

## Build

You need Docker. The build uses the `espressif/idf:v5.4` image.

```sh
./build.sh          # build/wifi-bridge.bin, build/wifi-bridge.factory.bin, build-safeboot/wifi-bridge-safeboot.bin
./build.sh dist     # release files in dist/
tools/ota.sh <device-ip>   # install build/wifi-bridge.bin on a bridge
```

To build an image that joins Wi-Fi right away (for example, to set up several bridges), copy
`sdkconfig.secrets.example` to `sdkconfig.secrets` and fill it in. Git ignores this file.

## Test

```sh
tools/qemu-build.sh
python3 tools/qemu_test.py            # firmware end to end: setup, API, commands, updates, backup, reset
python3 tools/qemu_safeboot_test.py   # the safeboot layout and the safeboot firmware
python3 tools/qemu_improv_test.py     # Improv Wi-Fi over serial
npm ci && npx playwright install chromium && npm test   # the web page in a browser
```

QEMU emulates the Ethernet port but not Wi-Fi. CI runs all of these tests on every pull request. When you add
an API endpoint or a command, add a test for it.

To work on the web page without hardware, run `python3 tools/mock_server.py --mode setup --port 8080`.
After you change the web page, run `node tools/screenshots.mjs` to update the README screenshots in
`docs/images/`.

## Code layout

| Path | Contents |
|---|---|
| `main/main.c` | Boot, setup or bridge mode, rollback protection |
| `main/config.c` | Settings: one table drives storage, the JSON API, backups, and commands |
| `main/wifi.c` | Wi-Fi: setup network, scan, connection test, reconnect, fast connect |
| `main/bridge.c`, `main/eth.c` | Frame forwarding between Wi-Fi and Ethernet, MAC and DHCP rewriting, Ethernet boards |
| `main/web.c`, `main/web_util.c` | Web server, JSON API, captive portal, access checks, firmware updates |
| `main/ota.c` | Firmware and safeboot firmware installs |
| `main/console.c`, `main/improv.c` | Commands; Improv Wi-Fi on the serial port |
| `main/mqtt.c`, `main/system.c`, `main/logbuf.c` | MQTT and Home Assistant; LED, button, time, mDNS, status; logs and syslog |
| `main/web/index.html` | The bridge's web page (one file, stored compressed in the firmware) |
| `safeboot/` | The safeboot firmware, for boards with a single firmware slot |
| `web-installer/` | The web installer page |
| `tools/` | Update script, packaging, tests, mock server |

## Conventions

- **Pull request titles:** Use [Conventional Commits](https://www.conventionalcommits.org/), such as
  `feat: …`, `fix: …`, or `docs: …`. Pull requests are squash-merged, and release-please turns the titles
  into versions and the changelog. See [docs/RELEASING.md](docs/RELEASING.md).
- **Settings:** Add a field to the `FIELDS` table in `main/config.c`. Storage, the API, backups, and a
  command come with it.
- **The web page:** It must work offline, because the bridge serves it on the setup network, so don't use
  CDNs or web fonts. Keep it small: it's stored compressed in the firmware.
- **API or command changes:** Update [docs/API.md](docs/API.md), [docs/commands.md](docs/commands.md), and
  the mock server together.
- **License:** Contributions are under the Apache License 2.0. Start new files with an SPDX header.
