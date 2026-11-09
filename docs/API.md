<!-- SPDX-License-Identifier: Apache-2.0 -->
# HTTP API

Use this API to read the bridge's status and change its settings from scripts and other tools. The bridge
serves every endpoint on port 80 of each interface that has an address:

| Situation | Address |
|---|---|
| Setup mode, setup network | `http://192.168.4.1/` (network name `<hostname>`, for example `wifi-bridge-a1b2c3`) |
| Setup mode, Ethernet | `http://192.168.5.1/` (the bridge runs a DHCP server on the Ethernet port) |
| Bridge mode | `http://<management-ip>/` and `http://<hostname>.local/` |
| Bridge mode, setup network active | Also `http://192.168.4.1/`, or `http://172.31.254.1/` if your network uses 192.168.4.0/24 (see `wifi.ap_ip`) |

The *management address* (`<management-ip>`) is the bridge's own fixed IP address on your network.

## Authentication and request rules

- If an admin password is set, every endpoint except the captive portal probes needs HTTP Basic
  authentication with the user name `admin`.
- Requests that change something use `POST` only.
- If a `POST` request has an `Origin` header, it must match the `Host` header. Otherwise, the bridge returns
  `403`.
- The `Host` header must be the bridge's IP address, `<hostname>`, `<hostname>.local`, or
  `<hostname>.<local domain>`. Otherwise, the bridge returns `403`. This protects against DNS rebinding.
- `/cm` also refuses cross-site browser requests. See [Console](#console) for its password rules.
- In bridge mode with no admin password, clients on the setup network get `403` from
  `POST /api/ota/upload`, `POST /api/ota/url`, `POST /api/config/restore`, `POST /api/cmd`, and `/cm`
  commands that change something. They can still fix the Wi-Fi settings.

Errors look like `{"ok":false,"error":"human readable message"}`, with a 4xx or 5xx status code.

## Pages

| Route | Description |
|---|---|
| `GET /` | The bridge's web page (gzip) |
| Any unknown `GET` while the setup network or setup mode is active | `302` to `http://<interface-ip>/` (captive portal) |

## `GET /api/status`

Returns the live state. The web page polls this endpoint every 2 seconds.

```json
{
  "mode": "setup",                     // "setup" | "bridge"
  "device": {
    "name": "WiFi Bridge", "hostname": "wifi-bridge-a1b2c3",
    "version": "0.1.0", "idf": "v5.4", "build": "Nov  8 2026 23:00:00",
    "elf_sha256": "3f2a…",                          // fingerprint of the running build
    "board": "wt32-eth01",
    "partition": "app0", "ota_mode": "dual",       // "dual" | "safeboot" (factory app + one app slot) | "none"
    "safeboot": null,                               // safeboot layout: {"ours":true,"version":"0.1.0","size":851968}
    "uptime_s": 1234, "reset_reason": "poweron", "boot_count": 12,
    "free_heap": 123456, "min_free_heap": 100000,
    "chip": "ESP32 rev 3.0, 2 cores", "cpu_mhz": 240, "flash_mb": 4,
    "mac_wifi": "aa:bb:cc:dd:ee:ff", "mac_eth": "aa:bb:cc:dd:ee:02",
    "time": "2026-11-08T23:00:00+0000",            // null until NTP sync
    "auth": false,                                  // true if an admin password is set
    "reboot_required": false,                       // a saved setting needs a restart
    "safe_mode": false,                             // 3 crashes within a minute of starting: setup page only
    "last_crash": null                              // or {"task","pc","backtrace","size"}, see /api/coredump
  },
  "wifi": {
    "state": "connected",              // "connected" | "connecting" | "disconnected" | "idle"
    "ssid": "HomeNet", "bssid": "aa:..", "channel": 6, "rssi": -55, "quality": 90,
    "ip": "192.168.1.250", "netmask": "255.255.255.0", "gateway": "192.168.1.1", "dns": "192.168.1.1",
    "connected_s": 1200, "disconnects": 2, "last_reason": "AUTH_EXPIRE",
    "ap_active": false, "ap_ssid": "wifi-bridge-a1b2c3", "ap_ip": "192.168.4.1", "ap_clients": 0
  },
  "ethernet": {
    "state": "up",                     // "up" | "down" | "init_failed" | "pending"
    "speed": 100, "duplex": "full",    // null when the link is down
    "role": "bridge",                  // "bridge" | "setup" (DHCP server for configuration)
    "ip": null                         // "192.168.5.1" in setup role
  },
  "client": { "mac": "11:22:33:44:55:66", "ip": "192.168.1.42" },   // learned wired device, null fields if unknown
  "frames": { "wired_to_wifi": 0, "wifi_to_wired": 0, "to_bridge": 0,
              "wifi_tx_fail": 0, "wifi_drop": 0, "wired_drop": 0 },
  "mqtt": { "enabled": false, "connected": false },
  "ota": { "state": "idle", "progress": 0, "error": "", "bytes": 0, "total": 0 }
}
```

## `GET /api/config` / `POST /api/config`

`GET` returns the whole configuration. It never returns secrets. Instead, `<field>_set: true|false` appears
where the secret would be.

`POST` takes a partial object with the same structure and changes only the keys that are present. For a
secret, a present key sets the value, and an empty string clears it. The response is
`{"ok":true,"reboot_required":true}`.

- An invalid value returns `400`, with an error that names the field.
- A storage failure returns `500`, and the active settings stay unchanged. The bridge saves changes before
  it applies them.
- Subnet masks must be contiguous (/1 through /30).
- The management and gateway addresses must be usable host addresses.
- A fixed `bridge.client_mac` must be a nonzero unicast MAC address.

```json
{
  "device":  { "name": "WiFi Bridge", "hostname": "wifi-bridge-a1b2c3" },
  "wifi":    { "ssid1": "HomeNet", "password1_set": true, "ssid2": "", "password2_set": false,
               "ap_fallback_min": 3,           // start the setup AP after N minutes without Wi-Fi, 0 = never
               "ap_password_set": false,       // optional WPA2 password for the setup/fallback AP (>= 8 chars)
               "power_save": "none",           // "none" | "min" | "max"
               "tx_power": 20 },                // dBm, 2..20
  "network": { "ip": "192.168.1.250", "netmask": "255.255.255.0", "gateway": "192.168.1.1", "dns": "192.168.1.1" },
  "bridge":  { "client_mac": "" },             // empty = learn from DHCP/IPv4/ARP traffic, or fixed "02:22:33:44:55:66"
  "ethernet":{ "board": "wt32-eth01",          // preset, or "custom"
               "phy": "lan8720", "addr": 1, "mdc": 23, "mdio": 18,
               "clk_mode": "ext_gpio0",        // "ext_gpio0" | "out_gpio0" | "out_gpio16" | "out_gpio17"
               "power_pin": 16 },              // -1 = none
  "admin":   { "password_set": false },
  "mqtt":    { "enabled": false, "host": "", "port": 1883, "user": "", "password_set": false,
               "topic": "wifi-bridge-a1b2c3", "discovery": true, "discovery_prefix": "homeassistant",
               "tele_period": 300 },
  "time":    { "ntp": "pool.ntp.org", "tz": "UTC0" },     // POSIX TZ string
  "syslog":  { "host": "", "port": 514 },
  "system":  { "log_level": "info",            // "error" | "warn" | "info" | "debug"
               "cpu_mhz": 240,                 // 80 | 160 | 240
               "led_pin": -1, "led_inverted": false,
               "button_pin": -1,               // active low; hold 3 s = setup mode once, hold 12 s = factory reset
               "powercycle_reset": true }      // 6 quick power cycles = factory reset
}
```

- `ethernet.phy` is one of `lan8720`, `ip101`, `rtl8201`, `dp83848`, or `ksz80xx`.
- Setting `ethernet.board` to a preset also sets the pin fields. Pin fields in the same request take
  precedence.
- An empty `network.ip` means the bridge isn't configured, so it starts in setup mode.
- The secrets you can set in a `POST` are `wifi.password1`, `wifi.password2`, `wifi.ap_password`,
  `admin.password`, and `mqtt.password`.

`GET /api/boards` returns the Ethernet board presets:
`[{"id":"wt32-eth01","name":"WT32-ETH01","phy":"lan8720","addr":1,"mdc":23,"mdio":18,"clk_mode":"ext_gpio0","power_pin":16}, ...]`

## Setup

`POST /api/setup` saves all setup settings at once and restarts into bridge mode. The setup page uses it.

```json
{ "ssid": "HomeNet", "password": "secret", "hostname": "wifi-bridge-a1b2c3",
  "admin_password": "optional",
  "network": { "ip": "192.168.1.250", "netmask": "255.255.255.0", "gateway": "192.168.1.1", "dns": "192.168.1.1" } }
```

The response is `{"ok":true,"url":"http://192.168.1.250/","mdns":"http://wifi-bridge-a1b2c3.local/"}`. The bridge
restarts about 2.5 seconds later.

### Wi-Fi scan

`GET /api/scan` starts a scan if none is running and the last result is older than 10 seconds.

```json
{ "scanning": false,
  "networks": [ { "ssid": "HomeNet", "rssi": -50, "quality": 100, "channel": 6, "auth": "WPA2", "secure": true } ] }
```

Networks are sorted by signal strength. For networks with the same SSID, only the strongest appears. While
`scanning` is `true`, `networks` holds the previous result, so poll until `scanning` is `false`. In bridge
mode, the bridge skips the scan while it's connecting or reconnecting.

### Connection test (setup mode only)

`POST /api/wifi/test {"ssid":"HomeNet","password":"secret"}` starts a connection test in the background,
including DHCP. To get the result, use `GET /api/wifi/test`:

```json
{ "state": "connected",                // "idle" | "connecting" | "connected" | "failed"
  "error": "",                          // "wrong password" | "network not found" | "no address from DHCP" |
                                        // "timed out" | "network security not supported (needs a password?)" | "could not connect"
  "ip": "192.168.1.37", "netmask": "255.255.255.0", "gateway": "192.168.1.1", "dns": "192.168.1.1",
  "suggested_ip": "192.168.1.250" }     // a free address (no ping reply) high in the subnet, for the static management IP
```

The bridge uses the DHCP address only during the test. The bridge and the wired device share one MAC
address on Wi-Fi, so the wired device gets that lease later. The management address must be a different,
fixed address.

## Actions

| Route | Body | Description |
|---|---|---|
| `POST /api/reboot` | `{"target":"normal"}`, `{"target":"safeboot"}`, or `{"target":"setup"}` | Restarts the bridge. `safeboot` starts the safeboot firmware (in the factory partition) if it's installed; otherwise, the bridge returns `409`. `setup` restarts into setup mode once and keeps the settings. |
| `POST /api/reset` | `{"scope":"all"}` or `{"scope":"wifi"}` | Resets this firmware's settings (all, or only Wi-Fi and the management address) and restarts into setup mode. |
| `GET /api/coredump` | None | Downloads the stored crash report (an ESP-IDF core dump). To decode it, use `idf.py coredump-info -c <file>` with the release's `.elf` file. Returns `404` if there's no report. `device.last_crash` in `/api/status` summarizes it as `{"task","pc","backtrace","size"}`, or `null`. |
| `POST /api/coredump/erase` | None | Deletes the stored crash report. |
| `GET /api/config/backup` | None | Downloads `wifi-bridge-<hostname>.json`. The file includes secrets only when an admin password is set. |
| `POST /api/config/restore` | Backup JSON (`Content-Type: application/json`) | Applies the settings and, after a successful save, restarts. The body must be a WiFi Bridge backup with `_firmware`, `device`, `wifi`, and `network` sections. Passwords that the backup omits are kept. |

## Firmware update

| Route | Description |
|---|---|
| `POST /api/ota/upload` | Send the raw image as the request body (`Content-Type: application/octet-stream`). The bridge responds after it verifies the image: `{"ok":true,"installed":"firmware"\|"safeboot","restart":true\|false}`. After a firmware image, the bridge restarts 2 seconds later. A `.safeboot.bin` (safeboot layout only) is installed without a restart. Returns `400` for a file that isn't a WiFi Bridge image. Returns `409` if an update is already running, or for a firmware image on a board with a single firmware slot; install that image from the safeboot firmware instead. |
| `POST /api/ota/url {"url":"http://host/fw.bin"}` | Downloads and installs the image in the background, over HTTP or HTTPS. Returns `400` for an invalid URL or if an update is already running. Image errors appear in `GET /api/ota`. After a firmware image, the bridge restarts 2 seconds after `done`. |
| `GET /api/ota` | Returns `{"state":"idle","progress":0,"error":"","bytes":0,"total":0}`. `state` is `idle`, `running`, `done`, or `error`. When the state is `done`, `installed` is `"firmware"` or `"safeboot"`. |

## Safeboot firmware

The *safeboot firmware* is a small recovery firmware for boards with a single firmware slot. It installs
firmware updates while WiFi Bridge isn't running. On boards with the safeboot layout,
`POST /api/reboot {"target":"safeboot"}` starts it.

The safeboot firmware answers at the management address. If Wi-Fi hasn't connected after 45 seconds, it also
opens the access point `<hostname>-safeboot` at `http://192.168.4.1/`, and serves `http://192.168.5.1/` on
Ethernet. Its API:

| Route | Description |
|---|---|
| `GET /` | The safeboot page |
| `GET /api/status` | `{"mode":"safeboot","device":{"name","hostname","safeboot_version","firmware_version","uptime_s","auth","return_in_s"},"wifi":{…},"ethernet":{…},"ota":{…}}` |
| `POST /api/ota/upload`, `POST /api/ota/url`, `GET /api/ota` | Same as in WiFi Bridge. The safeboot firmware installs a firmware image and starts it. |
| `POST /api/reboot` | `{"target":"normal"}` starts WiFi Bridge, or returns `409` if no valid firmware is installed. `{"target":"safeboot"}` restarts the safeboot firmware. |

Any other restart returns to WiFi Bridge. If no one opens the page for 10 minutes, the safeboot firmware
returns to WiFi Bridge by itself. Other paths redirect to `/`.

## Console

| Route | Description |
|---|---|
| `GET /api/log?since=N` | Returns `{"next":M,"lines":[{"seq":1,"t":12345,"lvl":"I","msg":"wifi: connected"}]}`: log lines with a sequence number greater than N, at most 200 per call. The bridge keeps the last 112 lines. `t` is milliseconds since startup, and `lvl` is one of `E W I D V -`. Pass `next` as `since` on the next call. If `next` is lower than your `since`, the bridge restarted. |
| `POST /api/cmd {"cmd":"Status 0"}` | Runs a command. Returns `{"ok":true,"result":<JSON or string>}`. |
| `GET /cm?cmnd=Status%200` | Runs a command and returns the result JSON. Commands that change something need `&user=admin&password=...` in the URL. If no admin password is set, `/cm` runs only commands that read. |

In bridge mode, `Ap 1` / `Ap 0` starts / stops the setup network, including its captive portal.

The same commands work on the serial port (115200 8N1), on the **Console** tab, and over MQTT
(`cmnd/<topic>/<Command>`). `Help` lists them. See [Commands](commands.md).
