<!-- SPDX-License-Identifier: Apache-2.0 -->
# Commands

You can run the same commands in these places:

- The serial port (TX0/RX0, 115200 baud)
- The **Console** tab on the bridge's page
- HTTP (see [Run commands over HTTP](#run-commands-over-http))
- MQTT (see [MQTT](#mqtt))

## Syntax

- Commands aren't case-sensitive.
- To show a setting's current value, send its command without an argument.
- To clear a text setting, send `"` as the argument.
- To run several commands at once, use `Backlog Cmd1 x; Cmd2 y`.

## Command reference

| Command | Description |
|---|---|
| `Help` | List the commands. |
| `Status` | Show everything on the **Dashboard**, as JSON. |
| `Config` | Show all settings, with secrets hidden. |
| `Restart 1` / `Restart 2` / `Restart 3` | Restart / restart into setup mode once, keeping settings / restart into the safeboot firmware. |
| `Reset 1` / `Reset 2` | Erase all settings / erase only the Wi-Fi and management address settings. Then restart into setup mode. |
| `Scan` | Scan for Wi-Fi networks. |
| `OtaUrl <url>`, `Upgrade 1` | Install a firmware update from a URL. |
| `Time` | Show the current time, after NTP sync. |
| `Boards` | List the Ethernet board presets. |
| `DeviceName`, `Hostname` | Friendly name; hostname, which is also the setup network name and the mDNS name. |
| `SSID1`, `Password1`, `SSID2`, `Password2` | Wi-Fi networks. |
| `ApFallback <min>` | Start the setup network after this many minutes without Wi-Fi. `0` turns it off. The default is `3`. |
| `Ap 1` / `Ap 0` | Start / stop the setup network now while bridging, until the next restart. For example, use this to configure the bridge from a phone. |
| `ApPassword` | WPA2 password for the setup network, 8 to 63 characters. By default, the setup network is open. |
| `PowerSave none\|min\|max`, `WifiPower <dBm>` | Wi-Fi modem sleep and transmit power. |
| `Sleep <0..250>` | Same as `PowerSave`: `0` is `none`, `1` to `50` is `min`, and higher values are `max`. The CPU always idles between packets. |
| `CoreDump`, `CoreDump 0` | Show / delete the stored crash report. The report is also on the **Maintenance** tab and at `/api/coredump`. |
| `IPAddress`, `Subnetmask`, `Gateway`, `DnsServer` | Management address settings. |
| `ClientMac <mac>` | Always bridge the wired device with this MAC address instead of learning it. `"` returns to learning. |
| `EthBoard`, `EthType`, `EthAddress`, `EthMdc`, `EthMdio`, `EthClockMode`, `EthPower` | Ethernet hardware. |
| `WebPassword` | Admin password for the bridge's page. `"` removes it. |
| `MqttEnable`, `MqttHost`, `MqttPort`, `MqttUser`, `MqttPassword`, `Topic`, `HassDiscovery`, `HassPrefix`, `TelePeriod` | MQTT. |
| `NtpServer`, `Timezone` | Time. The time zone is a POSIX TZ string, such as `CET-1CEST,M3.5.0,M10.5.0/3`. |
| `SysLogHost`, `SysLogPort` | Remote syslog. |
| `LogLevel error\|warn\|info\|debug` | Log verbosity. |
| `CpuFrequency 80\|160\|240` | CPU clock speed. Lower speeds use less power. |
| `LedPin`, `LedInverted`, `ButtonPin`, `PowerCycleReset` | Status LED, button, and reset by quick power cycles. |

## Run commands over HTTP

Send a command to `/cm` with the `cmnd` parameter:

```sh
curl -u admin:pw 'http://192.168.1.250/cm?cmnd=Status'
```

Commands that change something need the admin password in the URL:
`/cm?user=admin&password=pw&cmnd=...`. If no admin password is set, `/cm` runs only commands that read, such
as `Status`. To change settings without a password, use the **Console** tab or `POST /api/cmd`.

## MQTT

| Topic | Description |
|---|---|
| `tele/<topic>/LWT` | `Online` or `Offline` (retained). |
| `tele/<topic>/STATE` | Telemetry every `TelePeriod` seconds: uptime, free memory, Wi-Fi (SSID, BSSID, channel, RSSI, signal %), Ethernet link, wired device MAC and IP, and frame counters. |
| `cmnd/<topic>/<Command>` | Runs any command. The bridge publishes the result to `stat/<topic>/RESULT`. |

When Home Assistant discovery is on (the default), the bridge appears in Home Assistant as a device. It has
sensors for Wi-Fi signal and network, uptime, free memory, and the wired device's IP address, MAC address,
and traffic. It also has a binary sensor for the Ethernet link and a **Restart** button.
