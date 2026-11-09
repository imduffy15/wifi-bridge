<!-- SPDX-License-Identifier: Apache-2.0 -->
# Changelog

## [0.1.1](https://github.com/imduffy15/wifi-bridge/compare/v0.1.0...v0.1.1) (2026-11-09)


### Bug Fixes

* **docs:** include screenshots in the installer and improve README previews ([#7](https://github.com/imduffy15/wifi-bridge/issues/7)) ([5efbed7](https://github.com/imduffy15/wifi-bridge/commit/5efbed7e66e3449e36fa8bfb69ef2e282a3c0568))

## 0.1.0 (2026-11-09)

First public release.

- Bridge one wired device onto Wi-Fi from an ESP32 Ethernet board (WT32-ETH01 and other presets).
- Set up from a phone through the setup network, from a laptop on the Ethernet port, or from the web
  installer with Improv Wi-Fi.
- Web page for status, settings, firmware updates, logs, backup, and reset.
- Firmware updates by upload or URL, with automatic rollback. Boards with a single firmware slot update
  through the WiFi Bridge safeboot firmware.
- Fast reconnect to the last access point at startup.
- Commands on the serial port, the web console, HTTP, and MQTT; Home Assistant discovery.
